#include "SpaceManager.h"

#include <math.h>
#include <time.h>
#include <Sgp4.h>

#include "Layout.h"
#include "Astro.h"

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
        if (id == "iss")       { out = Screen::Iss;       return true; }
        if (id == "isspass")   { out = Screen::IssPass;   return true; }
        if (id == "launch")    { out = Screen::Launch;    return true; }
        if (id == "kp")        { out = Screen::Kp;        return true; }
        if (id == "solarwind") { out = Screen::SolarWind; return true; }
        if (id == "scales")    { out = Screen::Scales;    return true; }
        if (id == "flare")     { out = Screen::Flare;     return true; }
        if (id == "aurora")    { out = Screen::Aurora;    return true; }
        if (id == "dsn")       { out = Screen::Dsn;       return true; }
        if (id == "deepspace") { out = Screen::DeepSpace; return true; }
        if (id == "asteroid")  { out = Screen::Asteroid;  return true; }
        if (id == "humans")    { out = Screen::Humans;    return true; }
        if (id == "moon")      { out = Screen::Moon;      return true; }
        if (id == "starmap")   { out = Screen::StarMap;   return true; }
        if (id == "observing") { out = Screen::Observing; return true; }
        if (id == "eclipse")   { out = Screen::Eclipse;   return true; }
        if (id == "meteor")    { out = Screen::Meteor;    return true; }
        if (id == "cosmic")    { out = Screen::CosmicClock; return true; }
        if (id == "splash")    { out = Screen::Splash;    return true; }
        if (id == "clock")     { out = Screen::Clock;     return true; }
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
    alertFlare = boolCfg("sp-alert-flare", true);   // M+ solar flare (GOES X-ray feed)
    alertIss = boolCfg("sp-alert-iss", true);       // ISS visible pass overhead (SGP4)
    alertDsn = boolCfg("sp-alert-dsn", false);      // reserved (no DSN feed yet)
    alertAsteroid = boolCfg("sp-alert-asteroid", true); // asteroid inside ~1 lunar distance

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

    // Recompute the next ISS pass when the TLE/location changed, the last pass ended, or every 10 min.
    if (feed.Tle().valid && hasLatLon) {
        const time_t now = time(nullptr);
        if (now > 1600000000) {
            const bool stale = !passValid || passTleKey != feed.Tle().line1 ||
                               now > passSetEpoch || (millis() - lastPassCalcMs > 600000UL);
            if (stale) RecomputePass();
        }
    }

    // Refresh the on-device "observing window" cache ~once a minute (the geometry drifts slowly).
    if (hasLatLon) {
        const time_t now = time(nullptr);
        if (now > 1600000000 && (!obsValid || millis() - lastObsCalcMs > 60000UL))
            RecomputeObserving();
    }

    CheckAlerts();
    UpdateBrightness();
    HandleTouch();
    AutoRotate();

    // Advance the rotating sub-item index for multi-item screens (DSN links / deep-space targets).
    if (millis() - lastCardMs > 4000) { lastCardMs = millis(); cardIndex++; }
}

void SpaceManager::Draw(BandCanvas& backbuffer, bool /*firstPass*/)
{
    std::vector<Screen> rot = BuildRotation();
    // Keep `current` valid against the live rotation set (data can come and go).
    bool inRot = false;
    for (Screen s : rot) if (s == current) { inRot = true; break; }
    if (!inRot && !rot.empty()) current = rot.front();

    switch (current) {
        case Screen::Iss:       DrawIss(backbuffer); break;
        case Screen::IssPass:   DrawIssPass(backbuffer); break;
        case Screen::Launch:    DrawLaunch(backbuffer); break;
        case Screen::Kp:        DrawKp(backbuffer); break;
        case Screen::SolarWind: DrawSolarWind(backbuffer); break;
        case Screen::Scales:    DrawScales(backbuffer); break;
        case Screen::Flare:     DrawFlare(backbuffer); break;
        case Screen::Aurora:    DrawAurora(backbuffer); break;
        case Screen::Dsn:       DrawDsn(backbuffer); break;
        case Screen::DeepSpace: DrawDeepSpace(backbuffer); break;
        case Screen::Asteroid:  DrawAsteroid(backbuffer); break;
        case Screen::Humans:    DrawHumans(backbuffer); break;
        case Screen::Moon:      DrawMoon(backbuffer); break;
        case Screen::StarMap:   DrawStarMap(backbuffer); break;
        case Screen::Observing: DrawObserving(backbuffer); break;
        case Screen::Eclipse:   DrawEclipse(backbuffer); break;
        case Screen::Meteor:    DrawMeteor(backbuffer); break;
        case Screen::CosmicClock: DrawCosmicClock(backbuffer); break;
        case Screen::Splash:    DrawSplash(backbuffer); break;
        case Screen::Clock:
        default:                DrawClock(backbuffer); break;
    }

    DrawScreenDots(backbuffer, rot);
}

bool SpaceManager::HasData(Screen s) const
{
    switch (s) {
        case Screen::Iss:    return feed.Iss().valid;
        case Screen::IssPass: return hasLatLon && passValid; // SGP4 found an upcoming overpass
        case Screen::Launch: return !feed.Launches().empty();
        case Screen::Kp:     return feed.Wx().valid;
        case Screen::SolarWind: return feed.SolarWind().valid;
        case Screen::Scales: return feed.Scales().valid;
        case Screen::Flare:  return feed.Flare().valid;
        case Screen::Aurora: return hasLatLon && feed.Wx().valid; // needs location + Kp

        case Screen::Dsn:    return feed.Dsn().valid && !feed.Dsn().links.empty();
        case Screen::DeepSpace: {
            for (const space::DeepSpaceTarget& t : feed.DeepTargets()) if (t.valid) return true;
            return false;
        }
        case Screen::Asteroid: return !feed.Asteroids().empty();
        case Screen::Humans: return feed.Crew().valid && feed.Crew().number > 0;
        case Screen::Moon:   return true; // computed on-device, always available
        case Screen::StarMap: return hasLatLon; // on-device sky map; needs observer location
        case Screen::Observing: return hasLatLon && obsValid; // on-device dark-hours window; needs location + clock
        case Screen::Eclipse:     return true; // baked table, on-device
        case Screen::Meteor:      return true; // baked table, on-device
        case Screen::CosmicClock: return true; // on-device clock faces
        // Cold-start welcome: only while no live network feed has data yet (so it drops out once they do).
        case Screen::Splash: {
            bool any = feed.Iss().valid || !feed.Launches().empty() || feed.Wx().valid ||
                       feed.Flare().valid || (feed.Crew().valid && feed.Crew().number > 0) ||
                       feed.SolarWind().valid || feed.Scales().valid ||
                       !feed.Asteroids().empty() ||
                       (feed.Dsn().valid && !feed.Dsn().links.empty());
            for (const space::DeepSpaceTarget& t : feed.DeepTargets()) if (t.valid) any = true;
            return !any;
        }
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

    // --- Aurora: fire once when Kp crosses up to the threshold AND (if a location is set) the oval
    // actually reaches the user's geomagnetic latitude; re-arm when either condition drops.
    const space::SpaceWx& wx = feed.Wx();
    if (wx.valid) {
        const bool high = wx.kp >= KP_AURORA_THRESH;
        bool reachable = true;
        if (hasLatLon) {
            const double gm = fabs(space::GeomagLatitude(deviceLat, deviceLon));
            reachable = gm >= (double)space::AuroraOvalLat(wx.kp) - 3.0; // within ~3 deg of the visible edge
        }
        if (high && reachable && !kpAlerted) {
            kpAlerted = true;
            int g = (int)floorf(wx.kp) - 4;      // Kp 5..9 -> G1..G5
            if (g < 1) g = 1; else if (g > 5) g = 5;
            char body[64];
            snprintf(body, sizeof(body), "Kp %.1f (G%d) - aurora %s", wx.kp, g,
                     hasLatLon ? "may be visible at your location" : "likely");
            if (alertAurora) SendNtfy("Aurora watch", body, "zap", 4);
        } else if (!(high && reachable)) {
            kpAlerted = false;
        }
    }

    // --- Flare: fire once when GOES long-band flux crosses up to M-class (>= 1e-5 W/m2); re-arm below.
    const space::Flare& fl = feed.Flare();
    if (fl.valid) {
        const bool m = fl.fluxWm2 >= 1e-5f;
        if (m && !flareAlerted) {
            flareAlerted = true;
            const String cls = space::XrayClass(fl.fluxWm2);
            if (alertFlare)
                SendNtfy("Solar flare " + cls, "GOES X-ray " + cls + " - HF radio impact",
                         "radioactive,warning", fl.fluxWm2 >= 1e-4f ? 5 : 4);
        } else if (!m) {
            flareAlerted = false;
        }
    }

    // --- ISS overhead: fire once per visible pass as it approaches the horizon (~T-5 min).
    if (passValid && passVisible) {
        const time_t now = time(nullptr);
        const long toRise = passRiseEpoch - (long)now;
        if (issAlertedRise != passRiseEpoch && toRise <= 300 && toRise > -60) {
            issAlertedRise = passRiseEpoch;
            char b[64];
            snprintf(b, sizeof(b), "in %ld min, max %.0f deg, rises in the sky", toRise > 0 ? toRise / 60 : 0L, passMaxEl);
            if (alertIss) SendNtfy("ISS passing overhead", b, "satellite,rotating_light", 4);
        }
    }

    // --- Asteroid: fire once when an upcoming approach passes inside ~1 lunar distance. The feed
    // is distance-sorted and date-min=now, so every entry is a future approach; alert the closest
    // qualifying one, edge-detected by designation so it fires once per object.
    const std::vector<space::Asteroid>& as = feed.Asteroids();
    if (!as.empty()) {
        const space::Asteroid* near = nullptr;
        for (const space::Asteroid& a : as)
            if (a.distLd > 0 && a.distLd <= 1.0 && (!near || a.distLd < near->distLd)) near = &a;
        if (near && near->designation != asteroidAlertedDes) {
            asteroidAlertedDes = near->designation;
            char body[96];
            snprintf(body, sizeof(body), "%s passes %.2f lunar distances at %.0f km/s",
                     near->designation.c_str(), near->distLd, near->velKms);
            if (alertAsteroid) SendNtfy("Asteroid close approach", body, "comet,warning", 4);
        }
    }
}

void SpaceManager::RecomputePass()
{
    if (!sat) sat = new Sgp4();
    const space::Tle& t = feed.Tle();
    const time_t now = time(nullptr);
    if (!t.valid || !hasLatLon || now <= 1600000000) { passValid = false; return; }

    char l1[130], l2[130], name[] = "ISS";
    strncpy(l1, t.line1.c_str(), sizeof(l1) - 1); l1[sizeof(l1) - 1] = 0;
    strncpy(l2, t.line2.c_str(), sizeof(l2) - 1); l2[sizeof(l2) - 1] = 0;
    sat->init(name, l1, l2);
    sat->site(deviceLat, deviceLon, 0.0);
    passTleKey = t.line1;
    lastPassCalcMs = millis();

    // One-shot sanity cross-check: SGP4 sub-point vs the live ISS feed (should match within ~1-2 deg).
    if (!sgp4Checked && feed.Iss().valid) {
        sgp4Checked = true;
        sat->findsat((unsigned long)now);
        Serial.printf("[space] sgp4 check: sgp4 %.1f,%.1f  feed %.1f,%.1f\n",
                      sat->satLat, sat->satLon, feed.Iss().lat, feed.Iss().lon);
    }

    passinfo pass;
    sat->initpredpoint((unsigned long)now, 0.0);
    if (sat->nextpass(&pass, 20, false, 10.0)) { // forward search, min 10 deg peak
        passRiseEpoch = (long)((pass.jdstart - 2440587.5) * 86400.0);
        passSetEpoch  = (long)((pass.jdstop - 2440587.5) * 86400.0);
        passMaxEl = (float)pass.maxelevation;
        passAzRise = (float)pass.azstart;
        passVisible = (pass.vismax == lighted);
        passValid = true;
        Serial.printf("[space] iss pass: rise in %lds maxEl %.0f vis %d\n",
                      passRiseEpoch - (long)now, passMaxEl, (int)passVisible);
    } else {
        passValid = false;
    }
}

void SpaceManager::RecomputeObserving()
{
    const time_t now = time(nullptr);
    if (!hasLatLon || now <= 1600000000) { obsValid = false; return; }
    lastObsCalcMs = millis();
    obsSegEpoch0 = (long)now;

    const long stepSec = 86400 / OBS_SEG; // 20-min steps spanning the next 24h
    // Sample the next 24h of Sun + Moon altitude into the ring arrays (on-device ephemeris).
    for (int s = 0; s < OBS_SEG; ++s) {
        const time_t e = now + (time_t)s * stepSec;
        const double sunAlt = space::astro::SunAltDeg(e, deviceLat, deviceLon);
        uint8_t band;
        if      (sunAlt > -0.833) band = 0; // day
        else if (sunAlt > -6.0)   band = 1; // civil twilight
        else if (sunAlt > -12.0)  band = 2; // nautical twilight
        else if (sunAlt > -18.0)  band = 3; // astronomical twilight
        else                      band = 4; // astronomical night ("go" time)
        obsSunBand[s] = band;
        obsMoonUp[s] = space::astro::MoonAltDeg(e, deviceLat, deviceLon) > 0.0;
    }

    // Derive the upcoming astronomical-night window (band 4) from the sampled ring.
    obsDarkNow = (obsSunBand[0] == 4);
    obsDusk = obsDawn = 0;
    int duskSeg = -1, dawnSeg = -1;
    if (obsDarkNow) {
        obsDusk = (long)now;                            // already dark; window runs to dawn
        for (int s = 1; s < OBS_SEG; ++s) if (obsSunBand[s] != 4) { dawnSeg = s; break; }
    } else {
        for (int s = 1; s < OBS_SEG; ++s) {
            if (duskSeg < 0 && obsSunBand[s] == 4) duskSeg = s;                 // fall into night
            else if (duskSeg >= 0 && obsSunBand[s] != 4) { dawnSeg = s; break; } // climb back out
        }
        if (duskSeg >= 0) obsDusk = (long)now + (long)duskSeg * stepSec;
    }
    if (dawnSeg >= 0) obsDawn = (long)now + (long)dawnSeg * stepSec;

    // Moon presence across the dark window + its illuminated fraction (drives "truly dark" minutes).
    obsMoonUpMin = 0;
    const int startSeg = obsDarkNow ? 0 : duskSeg;
    if (obsDusk && dawnSeg >= 0 && startSeg >= 0)
        for (int s = startSeg; s < dawnSeg; ++s)
            if (obsMoonUp[s]) obsMoonUpMin += stepSec / 60;
    double mra, mdec, illum;
    const time_t midNight = (obsDusk && obsDawn) ? (time_t)(obsDusk + (obsDawn - obsDusk) / 2) : now;
    space::astro::MoonRaDec(midNight, mra, mdec, illum);
    obsMoonIllum = illum;

    obsValid = true;

    // One-shot field check: confirm the on-device ephemeris produced a sane window + moon.
    static bool obsLogged = false;
    if (!obsLogged) {
        obsLogged = true;
        Serial.printf("[space] observing: darkNow=%d dusk=+%lds dawn=+%lds moon=%d%% up=%ldmin\n",
                      (int)obsDarkNow, obsDusk ? obsDusk - (long)now : -1,
                      obsDawn ? obsDawn - (long)now : -1, (int)(obsMoonIllum * 100 + 0.5), obsMoonUpMin);
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
