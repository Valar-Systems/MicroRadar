#include "SeismicManager.h"

#include <algorithm>
#include <math.h>
#include <time.h>

#include "Layout.h"

namespace {

constexpr unsigned long INTERACT_HOLD_MS = 0; // (no auto-rotate here; kept for parity/readability)

double Deg2Rad(double d) { return d * M_PI / 180.0; }

// Low-precision solar elevation (degrees) at lat/lon for a UTC epoch -- drives the same night
// auto-dim the radar / Space use (sun below -0.833 deg = civil horizon). Copied from SpaceManager so
// the seismic app dims identically without depending on another translation unit.
float SunElevationDeg(double latDeg, double lonDeg, time_t utc)
{
    const double n = (double)utc / 86400.0 - 10957.5; // days since J2000.0
    double L = fmod(280.460 + 0.9856474 * n, 360.0);
    double g = fmod(357.528 + 0.9856003 * n, 360.0);
    const double lambda = L + 1.915 * sin(Deg2Rad(g)) + 0.020 * sin(Deg2Rad(2 * g));
    const double eps = 23.439 - 0.0000004 * n;
    const double lam = Deg2Rad(lambda), e = Deg2Rad(eps);
    const double dec = asin(sin(e) * sin(lam));
    const double ra = atan2(cos(e) * sin(lam), cos(lam));
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

void SeismicManager::Initialise()
{
    palette = seismic::PaletteDefault();
    backendBaseUrl = configServer.GetStoredString("se-base-url");

    const String latStr = configServer.GetStoredString("latitude");
    const String lonStr = configServer.GetStoredString("longitude");
    hasLatLon = latStr.length() && lonStr.length();
    deviceLat = latStr.toDouble();
    deviceLon = lonStr.toDouble();

    auto numCfg = [&](const char* key, float def) {
        const String v = configServer.GetStoredString(key);
        return v.isEmpty() ? def : v.toFloat();
    };
    minMag = numCfg("se-min-mag", 2.5f);
    radiusKm = numCfg("se-radius-km", 500.0f);
    bigMag = numCfg("se-big-mag", 6.0f);
    nearMag = numCfg("se-near-mag", 4.0f);
    if (radiusKm < 50) radiusKm = 50;
    if (radiusKm > 20000) radiusKm = 20000;

    const String br = configServer.GetStoredString("brightness");
    configuredBrightness = br.isEmpty() ? 255 : (uint8_t)constrain(br.toInt(), 10, 255);
    const String ad = configServer.GetStoredString("autodim");
    autoDim = ad.isEmpty() || ad == "true";

    ntfyTopic = configServer.GetStoredString("ntfy-topic");
    auto boolCfg = [&](const char* key, bool def) {
        const String v = configServer.GetStoredString(key);
        return v.isEmpty() ? def : (v == "true");
    };
    alertBig = boolCfg("se-alert-big", true);
    alertNear = boolCfg("se-alert-near", true);
    alertTsunami = boolCfg("se-alert-tsunami", true);

    // Background feed poller (idempotent spawn, then apply config).
    SeismicFeedClient::Config fcfg;
    fcfg.baseUrl = backendBaseUrl;
    fcfg.hasLatLon = hasLatLon;
    fcfg.lat = deviceLat;
    fcfg.lon = deviceLon;
    fcfg.minMag = minMag;
    fcfg.radiusKm = radiusKm;
    fcfg.intervalScale = 1.0f;
    feed.Begin();
    feed.Configure(fcfg);

    currentBrightness = configuredBrightness;
    tft.setBrightness(currentBrightness);
    lastBrightnessCheck = 0;

    // A re-init (config save) shouldn't re-fire alerts for events already on screen.
    alertSeeded = false;

    Serial.printf("[seismic] init; latlon=%d radius=%.0fkm minMag=%.1f big=%.1f near=%.1f\n",
                  (int)hasLatLon, radiusKm, minMag, bigMag, nearMag);
}

void SeismicManager::Update()
{
    feed.Poll();
    CheckAlerts();
    UpdateBrightness();
    HandleTouch();
}

void SeismicManager::Draw(BandCanvas& backbuffer, bool /*firstPass*/)
{
    switch (screen) {
        case Screen::Radar: DrawRadar(backbuffer); break;
        case Screen::List:  DrawList(backbuffer); break;
        case Screen::Stats: DrawStats(backbuffer); break;
    }
    if (inDetail && selectedValid) DrawDetailCard(backbuffer);
    else DrawScreenDots(backbuffer);
}

// ----------------------------------------------------------------------------- input

void SeismicManager::HandleTouch()
{
    // Serialize the touch I2C read against any network use of the shared bus -- the same pattern the
    // radar / Space use (inert on dual-core S3, where TryAcquireBus is uncontended).
    if (!http.TryAcquireBus()) return;
    int32_t tx = 0, ty = 0;
    const bool touched = tft.getTouch(&tx, &ty);
    http.ReleaseBus();

    if (touched) {
        if (!wasTouched) { wasTouched = true; touchStartX = tx; touchStartY = ty; }
        touchLastX = tx;
        touchLastY = ty;
        return;
    }
    if (!wasTouched) return;
    wasTouched = false;

    const int dx = touchLastX - touchStartX;
    const int dy = touchLastY - touchStartY;

    if (abs(dx) < 40 && abs(dy) < 40) { HandleTap(touchLastX, touchLastY); return; } // tap

    if (abs(dx) >= abs(dy)) {
        // horizontal swipe: cycle Radar -> List -> Stats (left = next)
        if (inDetail) { ExitDetail(); return; }
        int s = (int)screen + (dx < 0 ? 1 : -1);
        if (s < 0) s = (int)Screen::Stats;
        if (s > (int)Screen::Stats) s = 0;
        screen = (Screen)s;
        listScroll = 0;
    } else {
        // vertical swipe: scroll the list
        if (screen == Screen::List && !inDetail) {
            listScroll += (dy < 0 ? 3 : -3);
            if (listScroll < 0) listScroll = 0;
        }
    }
    (void)INTERACT_HOLD_MS;
}

void SeismicManager::HandleTap(int tx, int ty)
{
    if (inDetail) { ExitDetail(); return; }

    if (screen == Screen::Radar && hasLatLon) {
        const std::vector<seismic::Quake> shown = SortedNearbyByDistance();
        const int hit = HitTestQuake(tx, ty, shown);
        if (hit >= 0) { selected = shown[hit]; selectedValid = true; inDetail = true; }
        return;
    }

    if (screen == Screen::List) {
        // Map the tapped row to a quake. Rows start at LIST_TOP with LIST_ROW_H spacing (kept in
        // sync with DrawList).
        const int LIST_TOP = 70, LIST_ROW_H = 34;
        const int row = (ty - LIST_TOP) / LIST_ROW_H;
        if (row < 0) return;
        const std::vector<seismic::Quake> shown =
            hasLatLon ? SortedNearbyByDistance() : feed.Recent();
        const int idx = listScroll + row;
        if (idx >= 0 && idx < (int)shown.size()) { selected = shown[idx]; selectedValid = true; inDetail = true; }
    }
}

// ----------------------------------------------------------------------------- alerts

void SeismicManager::CheckAlerts()
{
    if (!feed.HasAny()) return;

    long newest = 0;
    for (const seismic::Quake& q : feed.Recent()) if (q.timeEpoch > newest) newest = q.timeEpoch;
    for (const seismic::Quake& q : feed.Nearby()) if (q.timeEpoch > newest) newest = q.timeEpoch;

    if (!alertSeeded) {
        alertSeeded = true;
        lastBigEpoch = lastNearEpoch = lastTsunamiEpoch = newest;
        return; // never alert for the backlog present at boot / re-config
    }

    // Big quake anywhere (from the worldwide feed).
    const seismic::Quake* big = nullptr;
    for (const seismic::Quake& q : feed.Recent())
        if (q.mag >= bigMag && q.timeEpoch > lastBigEpoch && (!big || q.timeEpoch > big->timeEpoch)) big = &q;
    if (big) {
        lastBigEpoch = big->timeEpoch;
        if (alertBig) {
            char title[40];
            snprintf(title, sizeof(title), "M%.1f earthquake", big->mag);
            SendNtfy(title, big->place.length() ? big->place : String("worldwide"),
                     "earth_americas,warning", big->mag >= 7.0f ? 5 : 4);
        }
    }

    // Tsunami-flagged quake anywhere.
    const seismic::Quake* tsu = nullptr;
    for (const seismic::Quake& q : feed.Recent())
        if (q.tsunami && q.timeEpoch > lastTsunamiEpoch && (!tsu || q.timeEpoch > tsu->timeEpoch)) tsu = &q;
    if (tsu) {
        lastTsunamiEpoch = tsu->timeEpoch;
        if (alertTsunami) {
            char title[48];
            snprintf(title, sizeof(title), "Tsunami flag: M%.1f", tsu->mag);
            SendNtfy(title, tsu->place.length() ? tsu->place : String("coastal quake"),
                     "ocean,rotating_light", 5);
        }
    }

    // Quake near the device (from the radius-bounded feed).
    const seismic::Quake* near = nullptr;
    for (const seismic::Quake& q : feed.Nearby())
        if (q.mag >= nearMag && q.timeEpoch > lastNearEpoch && (!near || q.timeEpoch > near->timeEpoch)) near = &q;
    if (near) {
        lastNearEpoch = near->timeEpoch;
        if (alertNear) {
            const double d = hasLatLon ? seismic::DistanceKm(deviceLat, deviceLon, near->lat, near->lon) : 0;
            char title[40], body[80];
            snprintf(title, sizeof(title), "M%.1f quake nearby", near->mag);
            snprintf(body, sizeof(body), "%.0f km away - %s", d,
                     near->place.length() ? near->place.c_str() : "near you");
            SendNtfy(title, body, "warning,bell", near->mag >= 5.0f ? 5 : 4);
        }
    }
}

void SeismicManager::SendNtfy(const String& title, const String& body, const String& tags, int priority)
{
    if (ntfyTopic.isEmpty()) return;
    if (lastNotifyMs != 0 && millis() - lastNotifyMs < 5000) return; // throttle bursts
    lastNotifyMs = millis();

    const std::vector<std::pair<String, String>> headers = {
        {"Title", title}, {"Tags", tags}, {"Priority", String(priority)}
    };
    (void)http.Post(String("https://ntfy.sh/") + ntfyTopic, body, headers);
}

// ----------------------------------------------------------------------------- brightness

void SeismicManager::UpdateBrightness()
{
    if (lastBrightnessCheck != 0 && millis() - lastBrightnessCheck < 20000) return;
    lastBrightnessCheck = millis();

    bool night = false;
    if (autoDim && hasLatLon) {
        const time_t utc = time(nullptr);
        if (utc > 1600000000)
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

// ----------------------------------------------------------------------------- helpers

std::pair<int, int> SeismicManager::ProjectQuakeToScreen(const seismic::Quake& q) const
{
    const int cx = SCREEN_SIZE_DIV_2, cy = SCREEN_SIZE_DIV_2;
    const int R = SCREEN_SIZE_DIV_2 - 30; // outer ring radius (matches DrawRadar)
    double d = q.distanceKm;
    double brg = q.bearingDeg;
    if (d <= 0) { // not pre-filled: compute now
        d = seismic::DistanceKm(deviceLat, deviceLon, q.lat, q.lon);
        brg = seismic::BearingDeg(deviceLat, deviceLon, q.lat, q.lon);
    }
    double rr = (d / radiusKm) * R;
    if (rr > R) rr = R; // clamp out-of-range quakes onto the rim
    const double a = brg * M_PI / 180.0;
    return { cx + (int)lround(rr * sin(a)), cy - (int)lround(rr * cos(a)) };
}

std::vector<seismic::Quake> SeismicManager::SortedNearbyByDistance() const
{
    std::vector<seismic::Quake> v = feed.Nearby();
    for (seismic::Quake& q : v) {
        q.distanceKm = seismic::DistanceKm(deviceLat, deviceLon, q.lat, q.lon);
        q.bearingDeg = seismic::BearingDeg(deviceLat, deviceLon, q.lat, q.lon);
    }
    std::sort(v.begin(), v.end(),
              [](const seismic::Quake& a, const seismic::Quake& b) { return a.distanceKm < b.distanceKm; });
    return v;
}

int SeismicManager::HitTestQuake(int tx, int ty, const std::vector<seismic::Quake>& shown) const
{
    int best = -1;
    long bestD2 = 26 * 26; // within ~26 px counts as a hit
    for (int i = 0; i < (int)shown.size(); ++i) {
        const auto p = ProjectQuakeToScreen(shown[i]);
        const long ddx = p.first - tx, ddy = p.second - ty;
        const long d2 = ddx * ddx + ddy * ddy;
        if (d2 < bestD2) { bestD2 = d2; best = i; }
    }
    return best;
}

String SeismicManager::FormatAgo(long epochSecs)
{
    const time_t now = time(nullptr);
    if (now < 1600000000 || epochSecs <= 0) return "";
    long s = (long)now - epochSecs;
    if (s < 0) s = 0;
    char b[16];
    if (s < 60)        snprintf(b, sizeof(b), "%lds", s);
    else if (s < 3600) snprintf(b, sizeof(b), "%ldm", s / 60);
    else if (s < 86400) snprintf(b, sizeof(b), "%ldh", s / 3600);
    else               snprintf(b, sizeof(b), "%ldd", s / 86400);
    return String(b);
}

std::vector<String> SeismicManager::SplitList(const String& s, bool lower)
{
    std::vector<String> out;
    int start = 0;
    const int n = (int)s.length();
    for (int i = 0; i <= n; ++i) {
        const bool sep = (i == n) || s[i] == ',' || s[i] == ' ' || s[i] == ';';
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

void SeismicManager::CenterText(BandCanvas& c, const String& s, int y, uint32_t color)
{
    c.setTextColor(color);
    c.drawString(s, SCREEN_SIZE_DIV_2 - c.textWidth(s) / 2, y);
}
