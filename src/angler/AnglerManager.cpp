#include "AnglerManager.h"

#include <math.h>
#include <stdio.h>
#include <time.h>

#include "Layout.h"
#include "Board.h"          // board::BuzzerChirp + variant::HAS_AUDIO
#include "astro/Astro.h"    // SunAltDeg for the night auto-dim

namespace {

constexpr unsigned long AUTO_DWELL_MS    = 8000;   // ms per screen when auto-rotating
constexpr unsigned long INTERACT_HOLD_MS = 30000;  // pause auto-rotate this long after a touch
constexpr long          NTP_FLOOR        = 1600000000; // clock is real once past this epoch

} // namespace

void AnglerManager::Initialise()
{
    palette = angler::PaletteDefault();

    const String latStr = configServer.GetStoredString("latitude");
    const String lonStr = configServer.GetStoredString("longitude");
    hasLatLon = latStr.length() && lonStr.length();
    deviceLat = latStr.toDouble();
    deviceLon = lonStr.toDouble();

    // Local offset for bite windows / rise-set / clock. Default to the nominal zone from longitude
    // (matches the config page default); the user can set the exact offset incl. DST.
    const String tzStr = configServer.GetStoredString("tz-offset");
    const double tzHours = tzStr.length() ? tzStr.toDouble() : round(deviceLon / 15.0);
    tzOffsetSec = (long)lround(tzHours * 3600.0);

    // Screen enable/order. "ang-screens" is a CSV of screen ids in display order; empty = all.
    enabledOrder.clear();
    auto idToScreen = [](const String& id, Screen& out) -> bool {
        if (id == "bite")   { out = Screen::Bite;   return true; }
        if (id == "moon")   { out = Screen::Moon;   return true; }
        if (id == "sun")    { out = Screen::Sun;    return true; }
        if (id == "splash") { out = Screen::Splash; return true; }
        if (id == "clock")  { out = Screen::Clock;  return true; }
        return false;
    };
    const String screensCfg = configServer.GetStoredString("ang-screens");
    if (screensCfg.length()) {
        for (const String& id : SplitList(screensCfg, true)) {
            Screen s;
            if (idToScreen(id, s)) enabledOrder.push_back(s);
        }
    }
    if (enabledOrder.empty())
        for (int i = 0; i < (int)Screen::COUNT; ++i) enabledOrder.push_back((Screen)i);

    const String br = configServer.GetStoredString("brightness");
    configuredBrightness = br.isEmpty() ? 255 : (uint8_t)constrain(br.toInt(), 10, 255);
    const String ad = configServer.GetStoredString("autodim");
    autoDim = ad.isEmpty() || ad == "true";

    ntfyTopic = configServer.GetStoredString("ntfy-topic");
    auto boolCfg = [&](const char* k, bool def) {
        const String v = configServer.GetStoredString(k);
        return v.isEmpty() ? def : (v == "true");
    };
    alertBite    = boolCfg("ang-alert-bite", true);
    chimeOnAlert = boolCfg("ang-chime", true);

    currentBrightness = configuredBrightness;
    tft.setBrightness(currentBrightness);
    lastBrightnessCheck = 0;

    // Force a solunar recompute on the next Update; don't re-alert a window already open (a config
    // save shouldn't fire), which alertSeeded=false arranges the first time CheckAlerts runs.
    solunarValid = false;
    lastSolunarCalcMs = 0;
    alertSeeded = false;

    Serial.printf("[angler] init; latlon=%d lat=%.4f lon=%.4f tz=%+.1fh screens=%u alertBite=%d chime=%d\n",
                  (int)hasLatLon, deviceLat, deviceLon, tzOffsetSec / 3600.0,
                  (unsigned)enabledOrder.size(), (int)alertBite, (int)chimeOnAlert);

    static bool selfChecked = false;   // once per boot, not on every config-save re-init
    if (!selfChecked) { selfChecked = true; SelfCheck(); }
}

void AnglerManager::Update()
{
    board::BuzzerUpdate();        // ends a chirp when its time is up (no-op without audio)
    RecomputeSolunar(false);
    CheckAlerts();
    UpdateBrightness();
    HandleTouch();
    AutoRotate();
}

void AnglerManager::Draw(BandCanvas& backbuffer, bool /*firstPass*/)
{
    std::vector<Screen> rot = BuildRotation();
    bool inRot = false;
    for (Screen s : rot) if (s == current) { inRot = true; break; }
    if (!inRot && !rot.empty()) { current = rot.front(); inDetail = false; }

    switch (current) {
        case Screen::Bite: DrawBite(backbuffer); break;
        case Screen::Moon: DrawMoon(backbuffer); break;
        case Screen::Sun:  DrawSun(backbuffer);  break;
        case Screen::Splash: DrawSplash(backbuffer); break;
        case Screen::Clock:
        default:           DrawClock(backbuffer); break;
    }

    if (inDetail) DrawDetailCard(backbuffer);
    else DrawScreenDots(backbuffer, rot);
}

// ----------------------------------------------------------------------------- rotation

bool AnglerManager::HasData(Screen s) const
{
    switch (s) {
        case Screen::Bite:
        case Screen::Moon:
        case Screen::Sun:    return hasLatLon && solunarValid;   // need a location + a real clock
        case Screen::Splash: return !(hasLatLon && solunarValid); // cold-start prompt, drops out once ready
        case Screen::Clock:  return true;
        default:             return false;
    }
}

std::vector<AnglerManager::Screen> AnglerManager::BuildRotation() const
{
    std::vector<Screen> rot;
    for (Screen s : enabledOrder)
        if (HasData(s)) rot.push_back(s);
    if (rot.empty()) rot.push_back(Screen::Clock);
    return rot;
}

void AnglerManager::AdvanceRotation(int dir)
{
    std::vector<Screen> rot = BuildRotation();
    int idx = 0;
    for (int i = 0; i < (int)rot.size(); ++i) if (rot[i] == current) { idx = i; break; }
    idx = (idx + dir + (int)rot.size()) % (int)rot.size();
    current = rot[idx];
    lastAdvanceMs = millis();
}

void AnglerManager::AutoRotate()
{
    if (inDetail) return;                                        // hold while inspecting
    if (millis() - lastInteractionMs < INTERACT_HOLD_MS) return; // user is driving
    if (millis() - lastAdvanceMs < AUTO_DWELL_MS) return;
    AdvanceRotation(+1);
}

// ----------------------------------------------------------------------------- input

void AnglerManager::HandleTouch()
{
    if (!http.TryAcquireBus()) return;
    int32_t tx = 0, ty = 0;
    const bool touched = tft.getTouch(&tx, &ty);
    http.ReleaseBus();

    const unsigned long now = millis();
    if (touched) {
        if (!wasTouched) { wasTouched = true; touchStartX = tx; touchStartY = ty; }
        touchLastX = tx;
        touchLastY = ty;
        lastInteractionMs = now;
        return;
    }
    if (!wasTouched) return;
    wasTouched = false;
    lastInteractionMs = now;

    const int dx = touchLastX - touchStartX;
    const int dy = touchLastY - touchStartY;

    if (abs(dx) < 40 && abs(dy) < 40) { HandleTap(touchLastX, touchLastY); return; }

    // a horizontal swipe navigates (and closes the detail card first)
    if (abs(dx) >= abs(dy)) {
        if (inDetail) { ExitDetail(); return; }
        AdvanceRotation(dx < 0 ? +1 : -1);
    }
}

void AnglerManager::HandleTap(int tx, int ty)
{
    if (inDetail) { ExitDetail(); return; }

    if (current == Screen::Bite) {
        selectedPeriod = PeriodHitTest(tx, ty);  // a period marker, or -1 for the day-summary card
        inDetail = true;
        return;
    }
    if (current == Screen::Moon || current == Screen::Sun) {
        inDetail = true;
    }
}

// ----------------------------------------------------------------------------- solunar cache

void AnglerManager::RecomputeSolunar(bool force)
{
    if (!hasLatLon) { solunarValid = false; return; }
    const time_t now = time(nullptr);
    if (now < NTP_FLOOR) { solunarValid = false; return; }   // wait for NTP

    if (!force && solunarValid && lastSolunarCalcMs != 0 && millis() - lastSolunarCalcMs < 60000) {
        // still refresh immediately on a local-day roll-over so "today" never goes stale
        const long ls = (long)now + tzOffsetSec;
        const long ds = (ls / 86400) * 86400 - tzOffsetSec;
        if (ds == solunarDayStart) return;
    }
    lastSolunarCalcMs = millis();

    today    = angler::ComputeDay(now, deviceLat, deviceLon, tzOffsetSec);
    tomorrow = angler::ComputeDay(now + 86400, deviceLat, deviceLon, tzOffsetSec);
    solunarDayStart = today.dayStart;
    solunarValid = today.valid;
}

void AnglerManager::SelfCheck()
{
    // One-shot self-check against the validated reference (USNO + a published solunar table):
    // NYC 2026-06-01 EDT should give majors 01:33 & 13:58 (2 h, start-at-transit), minors 05:52 &
    // 22:05 (allow a couple minutes for the low-precision ephemeris / geometric horizon). Confirms
    // the on-device math on real hardware. Uses mktime for the reference epoch (TZ=UTC from
    // configTime(0,0)), so it needs neither NTP nor a device location.
    struct tm t = {};
    t.tm_year = 2026 - 1900; t.tm_mon = 5; t.tm_mday = 1; t.tm_hour = 16; // 16:00 UTC = 12:00 EDT
    const time_t ref = mktime(&t);
    const long edt = -4L * 3600;
    angler::SolunarDay ny = angler::ComputeDay(ref, 40.7128, -74.0060, edt);
    Serial.println("[angler] solunar self-check NYC 2026-06-01 (expect major 01:33 & 13:58, minor ~05:52 & ~22:05 EDT):");
    for (int i = 0; i < ny.count; ++i)
        Serial.printf("    %-5s %-9s  event %s  [%s - %s]\n",
                      angler::PeriodShort(ny.periods[i].kind), angler::PeriodLabel(ny.periods[i].kind),
                      HM(ny.periods[i].center, edt).c_str(),
                      HM(ny.periods[i].start, edt).c_str(), HM(ny.periods[i].end, edt).c_str());
    Serial.printf("    rating=%s illum=%.0f%% sunrise %s sunset %s\n",
                  angler::RatingLabel(ny.rating), ny.moonIllum * 100.0,
                  HM(ny.sunrise, edt).c_str(), HM(ny.sunset, edt).c_str());
}

// ----------------------------------------------------------------------------- period search

bool AnglerManager::NextPeriod(time_t nowUtc, angler::Period& out, bool majorOnly) const
{
    bool found = false;
    time_t bestStart = 0;
    auto scan = [&](const angler::SolunarDay& d) {
        for (int i = 0; i < d.count; ++i) {
            const angler::Period& p = d.periods[i];
            if (majorOnly && !p.major()) continue;
            if (p.start > nowUtc && (!found || p.start < bestStart)) { found = true; bestStart = p.start; out = p; }
        }
    };
    scan(today);
    scan(tomorrow);
    return found;
}

bool AnglerManager::ActiveNow(time_t nowUtc, angler::Period& out) const
{
    auto scan = [&](const angler::SolunarDay& d) -> bool {
        for (int i = 0; i < d.count; ++i) {
            const angler::Period& p = d.periods[i];
            if (p.start <= nowUtc && nowUtc < p.end) { out = p; return true; }
        }
        return false;
    };
    return scan(today) || scan(tomorrow);
}

// ----------------------------------------------------------------------------- ring geometry

int AnglerManager::BiteRingRadius() { return SCREEN_SIZE_DIV_2 - 46; }

// 24-hour dial: midnight at the top (12 o'clock), noon at the bottom, advancing clockwise.
std::pair<int, int> AnglerManager::RingXY(long sodLocal, int radius)
{
    const double theta = (double)sodLocal / 86400.0 * 2.0 * M_PI;
    const int cx = SCREEN_SIZE_DIV_2, cy = SCREEN_SIZE_DIV_2;
    return { cx + (int)lround(radius * sin(theta)), cy - (int)lround(radius * cos(theta)) };
}

std::pair<int, int> AnglerManager::PeriodMarkerXY(const angler::Period& p) const
{
    return RingXY(LocalSod(p.center), BiteRingRadius());
}

int AnglerManager::PeriodHitTest(int tx, int ty) const
{
    int best = -1;
    long bestD2 = 30 * 30;   // tap tolerance
    for (int i = 0; i < today.count; ++i) {
        const auto pos = PeriodMarkerXY(today.periods[i]);
        const long ddx = pos.first - tx, ddy = pos.second - ty;
        const long d2 = ddx * ddx + ddy * ddy;
        if (d2 < bestD2) { bestD2 = d2; best = i; }
    }
    return best;
}

// ----------------------------------------------------------------------------- alerts

void AnglerManager::CheckAlerts()
{
    if (!solunarValid || !hasLatLon) return;
    const time_t now = time(nullptr);
    if (now < NTP_FLOOR) return;

    // Is a MAJOR feeding window open right now?
    const angler::Period* act = nullptr;
    auto scan = [&](const angler::SolunarDay& d) {
        for (int i = 0; i < d.count; ++i) {
            const angler::Period& p = d.periods[i];
            if (p.major() && p.start <= now && now < p.end) act = &d.periods[i];
        }
    };
    scan(today);
    scan(tomorrow);

    if (!alertSeeded) {   // never fire for a window already open at boot / after a config save
        lastAlertedBiteCenter = act ? act->center : 0;
        alertSeeded = true;
        return;
    }

    if (act && act->center != lastAlertedBiteCenter) {
        lastAlertedBiteCenter = act->center;
        if (chimeOnAlert && variant::HAS_AUDIO) board::BuzzerChirp(140);
        if (alertBite) {
            String body = String("Major feeding (") + angler::PeriodLabel(act->kind) +
                          ") until " + LocalHM(act->end);
            SendNtfy("Bite window open", body, "fish,fishing_pole_and_fish", 4);
        }
    }
}

void AnglerManager::SendNtfy(const String& title, const String& body, const String& tags, int priority)
{
    if (ntfyTopic.isEmpty()) return;
    if (lastNotifyMs != 0 && millis() - lastNotifyMs < 5000) return;
    lastNotifyMs = millis();

    const std::vector<std::pair<String, String>> headers = {
        {"Title", title}, {"Tags", tags}, {"Priority", String(priority)}
    };
    (void)http.Post(String("https://ntfy.sh/") + ntfyTopic, body, headers);
}

// ----------------------------------------------------------------------------- brightness

void AnglerManager::UpdateBrightness()
{
    if (lastBrightnessCheck != 0 && millis() - lastBrightnessCheck < 20000) return;
    lastBrightnessCheck = millis();

    bool night = false;
    if (autoDim && hasLatLon) {
        const time_t utc = time(nullptr);
        if (utc > NTP_FLOOR)
            night = space::astro::SunAltDeg(utc, deviceLat, deviceLon) < -0.833;
    }
    nightDim = night;

    uint8_t target = configuredBrightness;
    if (night) {
        target = configuredBrightness / 5;
        if (target < 10) target = 10;
    }
    if (target != currentBrightness) {
        currentBrightness = target;
        tft.setBrightness(target);
    }
}

// ----------------------------------------------------------------------------- helpers

std::vector<String> AnglerManager::SplitList(const String& s, bool lower)
{
    std::vector<String> out;
    int start = 0;
    const int n = (int)s.length();
    for (int i = 0; i <= n; ++i) {
        const bool sep = (i == n) || s[i] == ',' || s[i] == ';';
        if (sep) {
            if (i > start) {
                String tok = s.substring(start, i);
                tok.trim();
                if (lower) tok.toLowerCase();
                if (tok.length()) out.push_back(tok);
            }
            start = i + 1;
        }
    }
    return out;
}

void AnglerManager::CenterText(BandCanvas& c, const String& s, int y, uint32_t color)
{
    c.setTextColor(color);
    c.drawString(s, SCREEN_SIZE_DIV_2 - c.textWidth(s) / 2, y);
}

String AnglerManager::HM(time_t utc, long tzSec)
{
    if (utc == 0) return String("--:--");
    const time_t local = utc + tzSec;
    struct tm t;
    gmtime_r(&local, &t);   // gmtime of the shifted epoch == local wall clock
    char b[8];
    snprintf(b, sizeof(b), "%02d:%02d", t.tm_hour, t.tm_min);
    return String(b);
}

String AnglerManager::LocalHM(time_t utc) const { return HM(utc, tzOffsetSec); }
