#include "SpaceManager.h"

#include <math.h>
#include <time.h>

#include "Layout.h"

// Optional Valar space-feed backend base URL default. Normally NOT injected (Spacescope talks
// directly to free public space APIs and bakes in nothing); guarded so the file compiles whether
// or not -DSPACE_FEED_BASE is set. The runtime value ("space-base-url") overrides it.
#ifndef SPACE_FEED_BASE
#define SPACE_FEED_BASE ""
#endif

namespace {

constexpr unsigned long AUTO_DWELL_MS    = 8000;   // ms per screen when auto-rotating
constexpr unsigned long INTERACT_HOLD_MS = 30000;  // pause auto-rotate this long after a touch

// Alert thresholds.
constexpr long  LAUNCH_T10_S      = 600;  // fire the T-10 alert once inside this lead time
constexpr long  LAUNCH_T1_S       = 60;   // fire the T-1 alert once inside this lead time
constexpr float KP_AURORA_THRESH  = 6.0f; // Kp >= this (G2+) -> "aurora likely"

double Deg2Rad(double d) { return d * M_PI / 180.0; }

// Low-precision solar elevation (degrees) at lat/lon for a UTC epoch -- drives the same night
// auto-dim the radar / EAM use (sun below -0.833 deg = civil horizon). Copied from EamManager so
// the space app dims identically without depending on the EAM translation unit.
float SunElevationDeg(double latDeg, double lonDeg, time_t utc)
{
    const double n = (double)utc / 86400.0 - 10957.5; // days since J2000.0 (2000-01-01 12:00 UTC)
    double L = fmod(280.460 + 0.9856474 * n, 360.0);
    double g = fmod(357.528 + 0.9856003 * n, 360.0);
    const double lambda = L + 1.915 * sin(Deg2Rad(g)) + 0.020 * sin(Deg2Rad(2 * g));
    const double eps = 23.439 - 0.0000004 * n;
    const double lam = Deg2Rad(lambda), e = Deg2Rad(eps);
    const double dec = asin(sin(e) * sin(lam));
    const double ra = atan2(cos(e) * sin(lam), cos(lam)); // radians
    double gmst = fmod(18.697374558 + 24.06570982441908 * n, 24.0);
    if (gmst < 0) gmst += 24.0;
    const double lstDeg = gmst * 15.0 + lonDeg;
    const double Hdeg = lstDeg - ra * 180.0 / M_PI;
    const double H = Deg2Rad(Hdeg);
    const double lat = Deg2Rad(latDeg);
    double sinEl = sin(lat) * sin(dec) + cos(lat) * cos(dec) * cos(H);
    if (sinEl > 1) sinEl = 1;
    if (sinEl < -1) sinEl = -1;
    return (float)(asin(sinEl) * 180.0 / M_PI);
}

} // namespace

void SpaceManager::Initialise()
{
    // Optional backend (Phase-3 valar-space-feed). Empty default = talk directly to public APIs.
    backendBaseUrl = configServer.GetStoredString("space-base-url");
    if (backendBaseUrl.isEmpty())
        backendBaseUrl = SPACE_FEED_BASE;

    palette = space::PaletteDefault();

    const String br = configServer.GetStoredString("brightness");
    configuredBrightness = br.isEmpty() ? 255 : (uint8_t)constrain(br.toInt(), 10, 255);
    const String ad = configServer.GetStoredString("autodim");
    autoDim = ad.isEmpty() || ad == "true";

    // Screen enable/order. "space-screens" is a CSV of screen ids in display order; empty = all.
    enabledOrder.clear();
    const String screensCfg = configServer.GetStoredString("space-screens");
    auto idToScreen = [](const String& id, Screen& out) -> bool {
        if (id == "iss")    { out = Screen::Iss;    return true; }
        if (id == "launch") { out = Screen::Launch; return true; }
        if (id == "kp")     { out = Screen::Kp;     return true; }
        if (id == "splash") { out = Screen::Splash; return true; }
        if (id == "clock")  { out = Screen::Clock;  return true; }
        return false;
    };
    if (screensCfg.length()) {
        for (const String& id : SplitList(screensCfg, true)) {
            Screen s;
            if (idToScreen(id, s)) enabledOrder.push_back(s);
        }
    }
    if (enabledOrder.empty()) {
        for (int i = 0; i < (int)Screen::COUNT; ++i) enabledOrder.push_back((Screen)i);
    }

    // Optional device location: solar auto-dim now; ISS passes / local aurora in a later stage.
    const String latStr = configServer.GetStoredString("latitude");
    const String lonStr = configServer.GetStoredString("longitude");
    hasLatLon = latStr.length() && lonStr.length();
    deviceLat = latStr.toDouble();
    deviceLon = lonStr.toDouble();

    // Background feed poller (idempotent spawn, then apply config). Empty base = direct public APIs.
    SpaceFeedClient::Config fcfg;
    fcfg.baseUrl = backendBaseUrl;
    fcfg.hasLatLon = hasLatLon;
    fcfg.lat = deviceLat;
    fcfg.lon = deviceLon;
    fcfg.intervalScale = 1.0f;
    feed.Begin();
    feed.Configure(fcfg);

    // ntfy alerts (shared ntfy-topic key + per-trigger toggles). An empty topic disables all.
    // The edge-state flags are NOT reset here, so saving config mid-event doesn't re-fire.
    ntfyTopic = configServer.GetStoredString("ntfy-topic");
    auto boolCfg = [&](const char* key, bool def) {
        const String v = configServer.GetStoredString(key);
        return v.isEmpty() ? def : (v == "true");
    };
    alertLaunch = boolCfg("sp-alert-launch", true);
    alertAurora = boolCfg("sp-alert-aurora", true);
    alertFlare = boolCfg("sp-alert-flare", false);  // reserved (no GOES X-ray feed yet)
    alertIss = boolCfg("sp-alert-iss", false);      // reserved (no ISS-pass feed yet)
    alertDsn = boolCfg("sp-alert-dsn", false);      // reserved (no DSN feed yet)

    currentBrightness = configuredBrightness;
    tft.setBrightness(currentBrightness);
    lastBrightnessCheck = 0;

    Serial.printf("[space] init; backend=%s screens=%u latlon=%d\n",
                  backendBaseUrl.isEmpty() ? "(direct)" : backendBaseUrl.c_str(),
                  (unsigned)enabledOrder.size(), (int)hasLatLon);
}

void SpaceManager::Update()
{
    feed.Poll();
    CheckAlerts();
    UpdateBrightness();
    HandleTouch();
    AutoRotate();
}

void SpaceManager::Draw(BandCanvas& backbuffer, bool /*firstPass*/)
{
    std::vector<Screen> rot = BuildRotation();
    // Keep `current` valid against the live rotation set (data can come and go).
    bool inRot = false;
    for (Screen s : rot) if (s == current) { inRot = true; break; }
    if (!inRot && !rot.empty()) current = rot.front();

    switch (current) {
        case Screen::Iss:    DrawIss(backbuffer); break;
        case Screen::Launch: DrawLaunch(backbuffer); break;
        case Screen::Kp:     DrawKp(backbuffer); break;
        case Screen::Splash: DrawSplash(backbuffer); break;
        case Screen::Clock:
        default:             DrawClock(backbuffer); break;
    }

    DrawScreenDots(backbuffer, rot);
}

bool SpaceManager::HasData(Screen s) const
{
    switch (s) {
        case Screen::Iss:    return feed.Iss().valid;
        case Screen::Launch: return !feed.Launches().empty();
        case Screen::Kp:     return feed.Wx().valid;
        // Cold-start welcome: only while no live feed has data yet (so it drops out once they do).
        case Screen::Splash: return !(feed.Iss().valid || !feed.Launches().empty() || feed.Wx().valid);
        case Screen::Clock:  return true; // always-available idle screen
        default:             return false;
    }
}

std::vector<SpaceManager::Screen> SpaceManager::BuildRotation() const
{
    std::vector<Screen> rot;
    for (Screen s : enabledOrder)
        if (HasData(s)) rot.push_back(s);
    if (rot.empty()) rot.push_back(Screen::Clock); // always have the idle clock
    return rot;
}

void SpaceManager::AdvanceRotation(int dir)
{
    std::vector<Screen> rot = BuildRotation();
    int idx = 0;
    for (int i = 0; i < (int)rot.size(); ++i) if (rot[i] == current) { idx = i; break; }
    idx = (idx + dir + (int)rot.size()) % (int)rot.size();
    current = rot[idx];
    lastAdvanceMs = millis();
}

void SpaceManager::AutoRotate()
{
    if (millis() - lastInteractionMs < INTERACT_HOLD_MS) return; // user is driving
    if (millis() - lastAdvanceMs < AUTO_DWELL_MS) return;
    AdvanceRotation(+1);
}

void SpaceManager::HandleTouch()
{
    // Serialize the touch I2C read against any network use of the shared bus -- the same pattern
    // the radar / EAM use (inert on dual-core S3, where TryAcquireBus is uncontended).
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
    if (abs(dx) < 40 && abs(dy) < 40) return; // tap: just holds auto-rotate (handled above)
    if (abs(dx) >= abs(dy))
        AdvanceRotation(dx < 0 ? +1 : -1);    // swipe left -> next, right -> prev
}

void SpaceManager::UpdateBrightness()
{
    if (lastBrightnessCheck != 0 && millis() - lastBrightnessCheck < 20000) return;
    lastBrightnessCheck = millis();

    bool night = false;
    if (autoDim && hasLatLon) {
        const time_t utc = time(nullptr);
        if (utc > 1600000000) // NTP synced
            night = SunElevationDeg(deviceLat, deviceLon, utc) < -0.833f;
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

void SpaceManager::CheckAlerts()
{
    const time_t nowUtc = time(nullptr);
    const bool synced = nowUtc > 1600000000;

    // --- Launch: fire once at T-10 min and once at T-1 min for the next launch with a real time.
    // Edge-detected on the T-minus seconds crossing each threshold downward, so a boot mid-window
    // doesn't emit a mislabeled alert (on first sight of a launch we seed lastLaunchSecs = now).
    const std::vector<space::Launch>& ls = feed.Launches();
    if (synced && !ls.empty() && ls.front().precise && ls.front().t0Epoch > 0) {
        const space::Launch& L = ls.front();
        const long secs = L.t0Epoch - (long)nowUtc;
        if (L.t0Epoch != alertLaunchT0) {        // a new launch became next: re-arm
            alertLaunchT0 = L.t0Epoch;
            firedT10 = firedT1 = false;
            lastLaunchSecs = secs;               // seed so first sight isn't treated as a crossing
        }
        String who = L.provider;
        if (L.vehicle.length()) who += " " + L.vehicle;
        if (L.mission.length()) who += " - " + L.mission;
        if (!firedT10 && lastLaunchSecs > LAUNCH_T10_S && secs <= LAUNCH_T10_S) {
            firedT10 = true;
            if (alertLaunch) SendNtfy("Launch T-10 min", who, "rocket", 4);
        }
        if (!firedT1 && lastLaunchSecs > LAUNCH_T1_S && secs <= LAUNCH_T1_S) {
            firedT1 = true;
            if (alertLaunch) SendNtfy("Launch imminent (T-1 min)", who, "rocket,rotating_light", 5);
        }
        lastLaunchSecs = secs;
    }

    // --- Aurora: fire once when Kp crosses up to the threshold; re-arm when it drops back below.
    const space::SpaceWx& wx = feed.Wx();
    if (wx.valid) {
        const bool high = wx.kp >= KP_AURORA_THRESH;
        if (high && !kpAlerted) {
            kpAlerted = true;
            int g = (int)floorf(wx.kp) - 4;      // Kp 5..9 -> G1..G5
            if (g < 1) g = 1; else if (g > 5) g = 5;
            char body[48];
            snprintf(body, sizeof(body), "Kp %.1f (G%d) - aurora likely", wx.kp, g);
            if (alertAurora) SendNtfy("Aurora watch", body, "zap", 4);
        } else if (!high) {
            kpAlerted = false;
        }
    }
}

void SpaceManager::SendNtfy(const String& title, const String& body, const String& tags, int priority)
{
    if (ntfyTopic.isEmpty()) return;
    if (lastNotifyMs != 0 && millis() - lastNotifyMs < 5000) return; // throttle bursts
    lastNotifyMs = millis();

    // Blocking POST on the loop task, serialized with the feed worker via HttpRequestManager's
    // mutex -- the same pattern the radar/EAM use. Triggers are rare, so the brief stall is fine.
    const std::vector<std::pair<String, String>> headers = {
        {"Title", title}, {"Tags", tags}, {"Priority", String(priority)}
    };
    (void)http.Post(String("https://ntfy.sh/") + ntfyTopic, body, headers);
}

void SpaceManager::DrawScreenDots(BandCanvas& c, const std::vector<Screen>& rot) const
{
    const int n = (int)rot.size();
    if (n <= 1) return;
    int activeIdx = 0;
    for (int i = 0; i < n; ++i) if (rot[i] == current) { activeIdx = i; break; }

    const int gap = 9;
    const int totalW = (n - 1) * gap;
    int x = SCREEN_SIZE_DIV_2 - totalW / 2;
    const int y = SCREEN_SIZE - 14;
    for (int i = 0; i < n; ++i) {
        c.fillCircle(x, y, i == activeIdx ? 2 : 1, i == activeIdx ? palette.fg : palette.faint);
        x += gap;
    }
}

// The screens (DrawIss / DrawLaunch / DrawKp / DrawSplash / DrawClock) live in SpaceScreens.cpp.

// ----------------------------------------------------------------------------- helpers

String SpaceManager::FormatTMinus(long secondsToT0)
{
    const bool before = secondsToT0 >= 0;
    long s = before ? secondsToT0 : -secondsToT0;
    const long days = s / 86400;
    const long h = (s % 86400) / 3600;
    const long m = (s % 3600) / 60;
    const long sec = s % 60;
    const char sign = before ? '-' : '+';
    char buf[32];
    if (days > 0) snprintf(buf, sizeof(buf), "T%c%ldd %02ld:%02ld", sign, days, h, m);
    else          snprintf(buf, sizeof(buf), "T%c%02ld:%02ld:%02ld", sign, h, m, sec);
    return String(buf);
}

std::vector<String> SpaceManager::SplitList(const String& s, bool lower)
{
    std::vector<String> out;
    int start = 0;
    const int n = (int)s.length();
    for (int i = 0; i <= n; ++i) {
        const bool sep = (i == n) || s[i] == ',' || s[i] == ' ' || s[i] == '\t' ||
                         s[i] == '\r' || s[i] == '\n' || s[i] == ';';
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

void SpaceManager::CenterText(BandCanvas& c, const String& s, int y, uint32_t color)
{
    c.setTextColor(color);
    c.drawString(s, SCREEN_SIZE_DIV_2 - c.textWidth(s) / 2, y);
}
