#include "SpaceManager.h"

#include <math.h>
#include <time.h>

#include "Layout.h"
#include "SpaceStars.h"

// The Spacescope screens. Each draws one full frame into the band canvas in absolute screen
// coordinates (the S3 renders a single full-height band). Colours come from the palette scaled by
// GlowFactor() so night-dim fades everything uniformly. New screens are added here + a Screen enum
// entry + a HasData() case in SpaceManager.cpp; nothing else changes.

namespace {

// Trim a string to fit maxW pixels at the canvas's current text size, appending an ellipsis.
String FitWidth(BandCanvas& c, String s, int maxW)
{
    if (c.textWidth(s) <= maxW) return s;
    while (s.length() > 1 && c.textWidth(s + "...") > maxW) s.remove(s.length() - 1);
    return s + "...";
}

// Human data-rate, e.g. 1559000 -> "1.6 Mbps", 14220 -> "14 kbps", 160 -> "160 bps".
String FmtRate(double bps)
{
    if (bps >= 1e6) return String(bps / 1e6, 1) + " Mbps";
    if (bps >= 1e3) return String(bps / 1e3, 0) + " kbps";
    return String((long)bps) + " bps";
}

// Light-travel time from seconds, e.g. 84945 -> "23h 36m", 312 -> "5m 12s", 5 -> "5s".
String FmtLightTime(double s)
{
    char b[24];
    if (s >= 3600)     snprintf(b, sizeof(b), "%dh %02dm", (int)(s / 3600), (int)(s / 60) % 60);
    else if (s >= 60)  snprintf(b, sizeof(b), "%dm %02ds", (int)(s / 60), (int)s % 60);
    else               snprintf(b, sizeof(b), "%ds", (int)(s + 0.5));
    return String(b);
}

// DSN dish -> ground complex from the DSS number (11-29 Goldstone, 30-49 Canberra, 50-69 Madrid).
String DishComplex(const String& dish)
{
    int i = 0;
    while (i < (int)dish.length() && (dish[i] < '0' || dish[i] > '9')) ++i;
    const int n = dish.substring(i).toInt();
    if (n >= 11 && n <= 29) return "Goldstone";
    if (n >= 30 && n <= 49) return "Canberra";
    if (n >= 50 && n <= 69) return "Madrid";
    return "DSN";
}

// Friendly names for a curated set of well-known DSN spacecraft codes; raw code otherwise.
String FriendlyCraft(const String& code)
{
    if (code == "VGR1") return "Voyager 1";
    if (code == "VGR2") return "Voyager 2";
    if (code == "NHPC") return "New Horizons";
    if (code == "JWST") return "James Webb";
    if (code == "MRO")  return "Mars Recon Orbiter";
    if (code == "MVN")  return "MAVEN";
    if (code == "M01O") return "Mars Odyssey";
    if (code == "TGO")  return "ExoMars TGO";
    if (code == "PSP")  return "Parker Solar Probe";
    if (code == "DSCO") return "DSCOVR";
    if (code == "ACE")  return "ACE";
    if (code == "CHDR") return "Chandra";
    return code;
}

// Moon phase name from the phase fraction p (0=new, 0.5=full).
const char* PhaseName(double p)
{
    if (p < 0.033 || p >= 0.967) return "New Moon";
    if (p < 0.217) return "Waxing Crescent";
    if (p < 0.283) return "First Quarter";
    if (p < 0.467) return "Waxing Gibbous";
    if (p < 0.533) return "Full Moon";
    if (p < 0.717) return "Waning Gibbous";
    if (p < 0.783) return "Last Quarter";
    return "Waning Crescent";
}

// Moon illuminated fraction (0..1) at a UTC epoch (synodic approximation, same epoch as DrawMoon).
double MoonIllum(time_t t)
{
    double age = fmod(((double)t - 947182440.0) / 86400.0, 29.530588853);
    if (age < 0) age += 29.530588853;
    return (1.0 - cos(2.0 * M_PI * age / 29.530588853)) / 2.0;
}

double JulianDate(time_t t) { return (double)t / 86400.0 + 2440587.5; }

// Greenwich Mean Sidereal Time, hours 0..24.
double GMSTHours(time_t t)
{
    const double d = JulianDate(t) - 2451545.0;
    double g = fmod(18.697374558 + 24.06570982441908 * d, 24.0);
    if (g < 0) g += 24.0;
    return g;
}

// Local Mean Sidereal Time, hours; lon east-positive degrees.
double LMSTHours(time_t t, double lonDeg)
{
    double h = fmod(GMSTHours(t) + lonDeg / 15.0, 24.0);
    if (h < 0) h += 24.0;
    return h;
}

// Mars Coordinated Time (hours 0..24) + Mars Sol Date.
void MarsTime(time_t t, double& mtcHours, double& msd)
{
    const double jdtt = JulianDate(t) + 69.184 / 86400.0; // ~TT-UTC in 2026
    msd = (jdtt - 2451549.5) / 1.0274912517 + 44796.0 - 0.0009626;
    mtcHours = fmod(msd * 24.0, 24.0);
    if (mtcHours < 0) mtcHours += 24.0;
}

// hours (0..24) -> "HH:MM:SS"
String Hms(double hours)
{
    if (hours < 0) hours += 24.0;
    long s = (long)(hours * 3600.0 + 0.5);
    char b[12];
    snprintf(b, sizeof(b), "%02ld:%02ld:%02ld", (s / 3600) % 24, (s / 60) % 60, s % 60);
    return String(b);
}

// --- baked event tables (greatest-eclipse / shower-peak in UTC; approximate, for countdowns) ---
struct EclipseEv { const char* label; int y, mo, d, h, mi; };
const EclipseEv ECLIPSES[] = {
    {"Total Solar Eclipse",   2026, 8, 12, 17, 46},
    {"Partial Lunar Eclipse", 2026, 8, 28,  4, 14},
    {"Annular Solar Eclipse", 2027, 2,  6, 16,  0},
    {"Total Solar Eclipse",   2027, 8,  2, 10,  7},
    {"Annular Solar Eclipse", 2028, 1, 12,  4, 13},
    {"Total Solar Eclipse",   2028, 7, 22,  2, 56},
    {"Total Lunar Eclipse",   2028,12, 31, 16, 52},
};
constexpr int ECLIPSE_N = (int)(sizeof(ECLIPSES) / sizeof(ECLIPSES[0]));

struct ShowerEv { const char* name; int y, mo, d, h; int zhr; };
const ShowerEv SHOWERS[] = {
    {"Perseids",      2026, 8, 13,  4, 100},
    {"Orionids",      2026,10, 21, 12,  20},
    {"Leonids",       2026,11, 17,  9,  15},
    {"Geminids",      2026,12, 14, 18, 150},
    {"Quadrantids",   2027, 1,  3, 20, 110},
    {"Lyrids",        2027, 4, 22, 12,  18},
    {"Eta Aquariids", 2027, 5,  6,  4,  50},
};
constexpr int SHOWER_N = (int)(sizeof(SHOWERS) / sizeof(SHOWERS[0]));

// Azimuth degrees -> 8-point compass label.
const char* Compass8(float az)
{
    static const char* C[] = {"N","NE","E","SE","S","SW","W","NW"};
    int i = (int)((az + 22.5f) / 45.0f) & 7;
    return C[i];
}

} // namespace

// --------------------------------------------------------------------------------- ISS tracker
void SpaceManager::DrawIss(BandCanvas& c)
{
    const float gf = GlowFactor();
    const uint32_t fg     = space::ScaleColor(palette.fg, gf);
    const uint32_t dim    = space::ScaleColor(palette.dim, gf);
    const uint32_t faint  = space::ScaleColor(palette.faint, gf);
    const uint32_t accent = space::ScaleColor(palette.accent, gf);
    const uint32_t warn   = space::ScaleColor(palette.warn, gf);

    const int cx = SCREEN_SIZE_DIV_2, cy = SCREEN_SIZE_DIV_2;
    const int R = SCREEN_SIZE_DIV_2 - 30;

    // North-polar azimuthal "globe": pole at centre, rim is the south pole, the equator a mid ring.
    c.drawCircle(cx, cy, R, dim);
    c.drawCircle(cx, cy, R / 2, faint);
    c.drawFastHLine(cx - R, cy, 2 * R, faint);
    c.drawFastVLine(cx, cy - R, 2 * R, faint);
    c.fillCircle(cx, cy, 2, faint);

    const space::IssState& s = feed.Iss();
    if (!s.valid) { c.setTextSize(2); CenterText(c, "ISS", cy - 8, fg); return; }

    // r grows from the pole (lat +90) to the rim (lat -90); lon sets the bearing (0 at top).
    const float rr = (float)((90.0 - s.lat) / 180.0) * R;
    const float ang = (float)(s.lon * M_PI / 180.0) - (float)M_PI / 2.0f;
    const int bx = cx + (int)(rr * cosf(ang));
    const int by = cy + (int)(rr * sinf(ang));
    c.fillCircle(bx, by, 4, accent);
    c.drawCircle(bx, by, 8, fg);

    c.setTextSize(2); CenterText(c, "ISS", 16, fg);
    c.setTextSize(1); CenterText(c, s.sunlit ? "SUNLIT" : "ECLIPSED", 42, s.sunlit ? warn : dim);

    char line[40];
    snprintf(line, sizeof(line), "%d km   %d km/h", (int)lround(s.altKm), (int)lround(s.velocityKmh));
    c.setTextSize(1); CenterText(c, line, SCREEN_SIZE - 30, dim);
}

// ------------------------------------------------------------------------------ launch T-minus
void SpaceManager::DrawLaunch(BandCanvas& c)
{
    const float gf = GlowFactor();
    const uint32_t fg     = space::ScaleColor(palette.fg, gf);
    const uint32_t dim    = space::ScaleColor(palette.dim, gf);
    const uint32_t faint  = space::ScaleColor(palette.faint, gf);
    const uint32_t accent = space::ScaleColor(palette.accent, gf);

    const std::vector<space::Launch>& ls = feed.Launches();
    if (ls.empty()) { c.setTextSize(2); CenterText(c, "NO LAUNCH", SCREEN_SIZE_DIV_2 - 8, dim); return; }
    const space::Launch& L = ls.front();

    c.setTextSize(2);
    CenterText(c, FitWidth(c, L.provider.length() ? L.provider : String("LAUNCH"), SCREEN_SIZE - 60), 22, fg);
    if (L.vehicle.length()) { c.setTextSize(1); CenterText(c, FitWidth(c, L.vehicle, SCREEN_SIZE - 80), 50, dim); }

    // The big countdown ticks locally from NTP; only the launch instant comes from the feed.
    const time_t now = time(nullptr);
    const bool synced = now > 1600000000;
    String big;
    uint32_t bigColor = fg;
    if (L.precise && L.t0Epoch > 0 && synced) {
        const long delta = (long)L.t0Epoch - (long)now;
        big = FormatTMinus(delta);
        if (delta < 0) bigColor = dim; // already lifted off
    } else if (L.dateStr.length()) {
        big = "NET " + L.dateStr;
    } else {
        big = "SCHEDULED";
    }
    c.setTextSize(big.length() > 9 ? 3 : 4);
    CenterText(c, big, SCREEN_SIZE_DIV_2 - 22, bigColor);

    c.setTextSize(1);
    if (L.mission.length()) CenterText(c, FitWidth(c, L.mission, SCREEN_SIZE - 70), SCREEN_SIZE_DIV_2 + 24, accent);
    if (L.pad.length())     CenterText(c, FitWidth(c, L.pad, SCREEN_SIZE - 70), SCREEN_SIZE - 40, faint);
}

// -------------------------------------------------------------------------------- Kp gauge
void SpaceManager::DrawKp(BandCanvas& c)
{
    const float gf = GlowFactor();
    const uint32_t fg     = space::ScaleColor(palette.fg, gf);
    const uint32_t dim    = space::ScaleColor(palette.dim, gf);
    const uint32_t faint  = space::ScaleColor(palette.faint, gf);
    const uint32_t accent = space::ScaleColor(palette.accent, gf);

    const int cx = SCREEN_SIZE_DIV_2, cy = SCREEN_SIZE_DIV_2;
    const int R = SCREEN_SIZE_DIV_2 - 22;
    const int r = R - 28;
    const float startA = 135.0f, sweep = 270.0f; // 270 deg gauge opening at the bottom

    c.setTextSize(1); CenterText(c, "GEOMAGNETIC  Kp", 18, dim);
    c.fillArc(cx, cy, r, R, startA, startA + sweep, faint); // background track

    const space::SpaceWx& wx = feed.Wx();
    if (!wx.valid) { c.setTextSize(2); CenterText(c, "--", cy - 8, dim); return; }

    float kp = wx.kp; if (kp < 0) kp = 0; if (kp > 9) kp = 9;
    const float ang = startA + (kp / 9.0f) * sweep;

    const uint32_t kpRaw = kp >= 7 ? palette.alert : kp >= 5 ? palette.warn
                         : kp >= 4 ? lgfx::color888(120, 210, 90) : lgfx::color888(60, 180, 255);
    const uint32_t kpColor = space::ScaleColor(kpRaw, gf);
    c.fillArc(cx, cy, r, R, startA, ang, kpColor);
    c.fillArc(cx, cy, r - 3, R + 3, ang - 1.5f, ang + 1.5f, accent); // current-value tick

    char val[8]; snprintf(val, sizeof(val), "%.1f", kp);
    c.setTextSize(5); CenterText(c, val, cy - 36, fg);

    String label;
    if (kp >= 5)      label = "G" + String((int)floorf(kp) - 4); // Kp 5..9 -> G1..G5
    else if (kp >= 4) label = "ACTIVE";
    else if (kp >= 3) label = "UNSETTLED";
    else              label = "QUIET";
    c.setTextSize(2); CenterText(c, label, cy + 12, kpColor);

    // Recent-Kp sparkline in the gauge's open bottom wedge.
    if (wx.history.size() >= 2) {
        const int n = (int)wx.history.size();
        const int sw = 132, sh = 22;
        const int sx = cx - sw / 2, baseY = SCREEN_SIZE - 30;
        const int bw = sw / n;
        for (int i = 0; i < n; ++i) {
            float v = wx.history[i]; if (v < 0) v = 0; if (v > 9) v = 9;
            const int bh = (int)(v / 9.0f * sh);
            c.fillRect(sx + i * bw, baseY - bh, bw > 1 ? bw - 1 : 1, bh, i == n - 1 ? accent : dim);
        }
    }
}

// ----------------------------------------------------------------------------------- splash
void SpaceManager::DrawSplash(BandCanvas& c)
{
    const float gf = GlowFactor();
    const uint32_t fg     = space::ScaleColor(palette.fg, gf);
    const uint32_t accent = space::ScaleColor(palette.accent, gf);
    const uint32_t faint  = space::ScaleColor(palette.faint, gf);

    // A few fixed "stars" so the card reads as space, deterministic so the banded passes match.
    static const struct { int x, y, r; } stars[] = {
        {54, 70, 1}, {120, 44, 1}, {210, 58, 2}, {300, 40, 1}, {360, 96, 1},
        {40, 180, 1}, {380, 210, 2}, {70, 300, 1}, {150, 360, 1}, {260, 350, 2},
        {340, 320, 1}, {200, 110, 1}, {90, 130, 1}, {320, 160, 1}, {180, 300, 1},
    };
    for (const auto& s : stars)
        c.fillCircle(s.x, s.y, s.r, s.r > 1 ? accent : faint);

    c.setTextSize(4); CenterText(c, "SPACESCOPE", SCREEN_SIZE_DIV_2 - 40, fg);
    c.setTextSize(1); CenterText(c, "a desk window into everything above you", SCREEN_SIZE_DIV_2 + 4, faint);
    c.setTextSize(2); CenterText(c, "awaiting feeds...", SCREEN_SIZE_DIV_2 + 44, accent);
}

// ------------------------------------------------------------------------------------- clock
void SpaceManager::DrawClock(BandCanvas& c)
{
    const float gf = GlowFactor();
    const uint32_t fg    = space::ScaleColor(palette.fg, gf);
    const uint32_t faint = space::ScaleColor(palette.faint, gf);

    const time_t utc = time(nullptr);
    char hhmmss[16] = "--:--:--";
    if (utc > 1600000000) { // NTP synced
        struct tm t;
        gmtime_r(&utc, &t);
        snprintf(hhmmss, sizeof(hhmmss), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
    }

    c.setTextSize(1); CenterText(c, "UTC", SCREEN_SIZE_DIV_2 - 46, faint);
    c.setTextSize(5); CenterText(c, hhmmss, SCREEN_SIZE_DIV_2 - 28, fg);
}

// ------------------------------------------------------------------------------- DSN Now
void SpaceManager::DrawDsn(BandCanvas& c)
{
    const float gf = GlowFactor();
    const uint32_t fg     = space::ScaleColor(palette.fg, gf);
    const uint32_t dim    = space::ScaleColor(palette.dim, gf);
    const uint32_t faint  = space::ScaleColor(palette.faint, gf);
    const uint32_t accent = space::ScaleColor(palette.accent, gf);

    c.setTextSize(1); CenterText(c, "DEEP SPACE NETWORK", 20, dim);

    const space::DsnState& d = feed.Dsn();
    if (!d.valid || d.links.empty()) {
        c.setTextSize(2); CenterText(c, d.valid ? "no active links" : "acquiring...", SCREEN_SIZE_DIV_2 - 8, dim);
        return;
    }

    // Headline: rotate one active link at a time so each gets the round panel to itself.
    const int n = (int)d.links.size();
    const space::DsnLink& L = d.links[cardIndex % n];
    const uint32_t dirColor = L.up ? accent : fg; // uplink (commanding) vs downlink (receiving)

    c.setTextSize(3); CenterText(c, FitWidth(c, FriendlyCraft(L.spacecraft), SCREEN_SIZE - 40), SCREEN_SIZE_DIV_2 - 56, fg);
    c.setTextSize(1); CenterText(c, DishComplex(L.dish) + "  " + L.dish, SCREEN_SIZE_DIV_2 - 18, dim);

    String rate = (L.up ? "^ " : "v ") + FmtRate(L.dataRateBps);
    if (L.band.length()) rate += "   " + L.band + "-band";
    c.setTextSize(2); CenterText(c, rate, SCREEN_SIZE_DIV_2 + 8, dirColor);

    char foot[40];
    snprintf(foot, sizeof(foot), "%d active link%s   %s", n, n == 1 ? "" : "s", L.up ? "uplink" : "downlink");
    c.setTextSize(1); CenterText(c, foot, SCREEN_SIZE - 32, faint);
}

// --------------------------------------------------------------------------- deep-space distance
void SpaceManager::DrawDeepSpace(BandCanvas& c)
{
    const float gf = GlowFactor();
    const uint32_t fg     = space::ScaleColor(palette.fg, gf);
    const uint32_t dim    = space::ScaleColor(palette.dim, gf);
    const uint32_t faint  = space::ScaleColor(palette.faint, gf);
    const uint32_t accent = space::ScaleColor(palette.accent, gf);

    c.setTextSize(1); CenterText(c, "DEEP SPACE", 20, dim);

    // Gather the targets that have a fix; rotate through them.
    std::vector<const space::DeepSpaceTarget*> live;
    for (const space::DeepSpaceTarget& t : feed.DeepTargets()) if (t.valid) live.push_back(&t);
    if (live.empty()) {
        c.setTextSize(2); CenterText(c, "acquiring...", SCREEN_SIZE_DIV_2 - 8, dim);
        return;
    }
    const space::DeepSpaceTarget& t = *live[cardIndex % (int)live.size()];

    String name = t.name; name.toUpperCase();
    c.setTextSize(2); CenterText(c, name, SCREEN_SIZE_DIV_2 - 58, accent);

    // Distance: AU once past ~1 AU, otherwise million-km so JWST/Parker read sensibly.
    String big;
    if (t.distanceAu >= 1.0) big = String(t.distanceAu, 1) + " AU";
    else                     big = String(t.distanceAu * 149.597871, 2) + "M km";
    c.setTextSize(4); CenterText(c, big, SCREEN_SIZE_DIV_2 - 24, fg);

    String spd = String(t.speedKms, 1) + " km/s " + (t.receding ? "receding" : "approaching");
    c.setTextSize(1); CenterText(c, spd, SCREEN_SIZE_DIV_2 + 22, dim);
    c.setTextSize(1); CenterText(c, FmtLightTime(t.distanceAu * 499.004784) + " light delay", SCREEN_SIZE_DIV_2 + 44, faint);
}

// ------------------------------------------------------------------- asteroid close approach
void SpaceManager::DrawAsteroid(BandCanvas& c)
{
    const float gf = GlowFactor();
    const uint32_t fg     = space::ScaleColor(palette.fg, gf);
    const uint32_t dim    = space::ScaleColor(palette.dim, gf);
    const uint32_t faint  = space::ScaleColor(palette.faint, gf);
    const uint32_t accent = space::ScaleColor(palette.accent, gf);
    const uint32_t warn   = space::ScaleColor(palette.warn, gf);
    const uint32_t alert  = space::ScaleColor(palette.alert, gf);
    const uint32_t earthC = space::ScaleColor(lgfx::color888(60, 140, 235), gf);
    const uint32_t moonC  = space::ScaleColor(lgfx::color888(200, 200, 210), gf);

    c.setTextSize(1); CenterText(c, "ASTEROID FLYBY", 18, dim);

    const std::vector<space::Asteroid>& as = feed.Asteroids();
    if (as.empty()) { c.setTextSize(2); CenterText(c, "acquiring...", SCREEN_SIZE_DIV_2 - 8, dim); return; }
    const space::Asteroid& a = as[cardIndex % (int)as.size()];

    c.setTextSize(3); CenterText(c, FitWidth(c, a.designation, SCREEN_SIZE - 60), 42, accent);

    // Headline miss distance in lunar distances -- inside 1 LD is the eye-catching case.
    const uint32_t ldCol = a.distLd < 1.0 ? alert : a.distLd < 5.0 ? warn : fg;
    c.setTextSize(5); CenterText(c, String(a.distLd, 2) + " LD", 92, ldCol);
    c.setTextSize(1);
    CenterText(c, a.distLd < 1.0 ? "inside the Moon's orbit" : "lunar distances from Earth", 136, dim);

    // Earth -- Moon -- asteroid strip (log scale, 0.1..20 LD) showing where the rock sits vs the Moon.
    const int x0 = 56, x1 = SCREEN_SIZE - 56, baseY = SCREEN_SIZE_DIV_2 - 26;
    auto xfor = [&](double ld) -> int {
        if (ld < 0.1) ld = 0.1;
        if (ld > 20.0) ld = 20.0;
        const double t = (log10(ld) - log10(0.1)) / (log10(20.0) - log10(0.1));
        return x0 + (int)(t * (x1 - x0));
    };
    c.drawFastHLine(x0, baseY, x1 - x0, faint);
    c.fillCircle(x0, baseY, 6, earthC);
    c.setTextSize(1); c.setTextColor(faint); c.drawString("Earth", x0 - 6, baseY + 10);
    const int mx = xfor(1.0);
    c.fillCircle(mx, baseY, 3, moonC);
    c.setTextColor(faint); c.drawString("Moon", mx - 12, baseY + 10);
    const int ax = xfor(a.distLd);
    c.drawFastVLine(ax, baseY - 12, 8, ldCol);
    c.fillCircle(ax, baseY, 4, ldCol);

    // Size estimate (from absolute magnitude) + approach speed.
    const double dm = space::AsteroidDiameterMeters(a.h);
    String sz = dm <= 0 ? String("size n/a")
              : dm < 1000 ? "~" + String((int)(dm + 0.5)) + " m"
                          : "~" + String(dm / 1000.0, 1) + " km";
    char det[48]; snprintf(det, sizeof(det), "%s   %.1f km/s", sz.c_str(), a.velKms);
    c.setTextSize(2); CenterText(c, det, baseY + 34, fg);

    // Live countdown to closest approach (needs NTP).
    const time_t now = time(nullptr);
    if (now > 1600000000 && a.caEpoch > 0) {
        const long dt = a.caEpoch - (long)now;
        c.setTextSize(2); CenterText(c, FormatTMinus(dt), baseY + 60, dt < 0 ? dim : accent);
    }

    const int n = (int)as.size();
    char foot[40]; snprintf(foot, sizeof(foot), "%d tracked   %d/%d", n, (cardIndex % n) + 1, n);
    c.setTextSize(1); CenterText(c, foot, SCREEN_SIZE - 22, faint);
}

// ----------------------------------------------------------------------------- solar flare
void SpaceManager::DrawFlare(BandCanvas& c)
{
    const float gf = GlowFactor();
    const uint32_t fg     = space::ScaleColor(palette.fg, gf);
    const uint32_t dim    = space::ScaleColor(palette.dim, gf);
    const uint32_t faint  = space::ScaleColor(palette.faint, gf);
    const uint32_t accent = space::ScaleColor(palette.accent, gf);
    const uint32_t warn   = space::ScaleColor(palette.warn, gf);
    const uint32_t alert  = space::ScaleColor(palette.alert, gf);

    const int cx = SCREEN_SIZE_DIV_2, cy = SCREEN_SIZE_DIV_2;
    const int R = SCREEN_SIZE_DIV_2 - 22, r = R - 28;
    const float startA = 135.0f, sweep = 270.0f;

    c.setTextSize(1); CenterText(c, "SOLAR X-RAY FLUX", 18, dim);
    c.fillArc(cx, cy, r, R, startA, startA + sweep, faint);

    const space::Flare& f = feed.Flare();
    if (!f.valid) { c.setTextSize(2); CenterText(c, "acquiring...", cy - 8, dim); return; }

    // Log scale across the NOAA decades A(1e-8) .. X(1e-3).
    float frac = (log10f(f.fluxWm2 > 0 ? f.fluxWm2 : 1e-9f) + 8.0f) / 5.0f;
    if (frac < 0) frac = 0; if (frac > 1) frac = 1;
    const float ang = startA + frac * sweep;

    const String cls = space::XrayClass(f.fluxWm2);
    const char L = cls.length() ? cls[0] : 'A';
    const uint32_t col = (L == 'X') ? alert : (L == 'M') ? warn : (L == 'C') ? fg : dim;

    c.fillArc(cx, cy, r, R, startA, ang, col);
    c.fillArc(cx, cy, r - 3, R + 3, ang - 1.5f, ang + 1.5f, accent);

    c.setTextSize(5); CenterText(c, cls, cy - 30, col);
    c.setTextSize(1); CenterText(c, "A   B   C   M   X", cy + 18, faint);
    c.setTextSize(1); CenterText(c, "6h peak " + space::XrayClass(f.peakFluxWm2), SCREEN_SIZE - 30, dim);
}

// --------------------------------------------------------------------------- humans in space
void SpaceManager::DrawHumans(BandCanvas& c)
{
    const float gf = GlowFactor();
    const uint32_t fg     = space::ScaleColor(palette.fg, gf);
    const uint32_t dim    = space::ScaleColor(palette.dim, gf);
    const uint32_t faint  = space::ScaleColor(palette.faint, gf);
    const uint32_t accent = space::ScaleColor(palette.accent, gf);

    c.setTextSize(1); CenterText(c, "HUMANS IN SPACE", 22, dim);

    const space::Crew& cr = feed.Crew();
    if (!cr.valid || cr.number <= 0) { c.setTextSize(2); CenterText(c, "acquiring...", SCREEN_SIZE_DIV_2 - 8, dim); return; }

    char num[8]; snprintf(num, sizeof(num), "%d", cr.number);
    c.setTextSize(6); CenterText(c, num, SCREEN_SIZE_DIV_2 - 58, fg);
    c.setTextSize(1); CenterText(c, "aboard right now", SCREEN_SIZE_DIV_2 + 6, faint);

    int iss = 0, tg = 0, other = 0;
    for (const auto& p : cr.people) {
        if (p.first == "ISS") ++iss;
        else if (p.first == "Tiangong") ++tg;
        else ++other;
    }
    String tally;
    if (iss) tally += "ISS " + String(iss);
    if (tg)  { if (tally.length()) tally += "    "; tally += "Tiangong " + String(tg); }
    if (other) { if (tally.length()) tally += "    "; tally += "+" + String(other); }
    c.setTextSize(1); CenterText(c, tally, SCREEN_SIZE_DIV_2 + 28, dim);

    // Rotate one crew member's name at a time.
    if (!cr.people.empty()) {
        const auto& p = cr.people[cardIndex % (int)cr.people.size()];
        c.setTextSize(2); CenterText(c, FitWidth(c, p.second, SCREEN_SIZE - 40), SCREEN_SIZE - 58, accent);
        c.setTextSize(1); CenterText(c, p.first, SCREEN_SIZE - 32, faint);
    }
}

// ------------------------------------------------------------------------------------- moon
void SpaceManager::DrawMoon(BandCanvas& c)
{
    const float gf = GlowFactor();
    const uint32_t dim    = space::ScaleColor(palette.dim, gf);
    const uint32_t faint  = space::ScaleColor(palette.faint, gf);
    const uint32_t accent = space::ScaleColor(palette.accent, gf);
    const uint32_t bodyDark = space::ScaleColor(lgfx::color888(34, 38, 52), gf);
    const uint32_t lit      = space::ScaleColor(lgfx::color888(225, 228, 235), gf); // pale moonlight

    const int cx = SCREEN_SIZE_DIV_2, cyTop = SCREEN_SIZE_DIV_2 - 30, R = 76;
    c.setTextSize(1); CenterText(c, "MOON", 18, dim);
    c.fillCircle(cx, cyTop, R, bodyDark);

    const time_t now = time(nullptr);
    if (now <= 1600000000) { // need NTP for the absolute phase
        c.drawCircle(cx, cyTop, R, faint);
        c.setTextSize(1); CenterText(c, "awaiting clock", cyTop + R + 22, dim);
        return;
    }

    // Phase fraction p (0=new, 0.5=full) from the synodic month since a known new moon.
    constexpr double SYN = 29.530588853;
    constexpr long REF = 947182440; // 2000-01-06 18:14 UTC new moon
    double age = fmod(((double)now - (double)REF) / 86400.0, SYN);
    if (age < 0) age += SYN;
    const double p = age / SYN;
    const double cph = cos(2.0 * M_PI * p);

    // Lit span per scanline: waxing lit on the right, waning on the left (N-hemisphere convention).
    for (int y = -R; y <= R; ++y) {
        const double hw = sqrt((double)R * R - (double)y * y);
        int left, right;
        if (p <= 0.5) { left = (int)(cph * hw); right = (int)hw; }
        else          { left = (int)(-hw);      right = (int)(-cph * hw); }
        if (right > left) c.drawFastHLine(cx + left, cyTop + y, right - left, lit);
    }
    c.drawCircle(cx, cyTop, R, faint);

    const double illum = (1.0 - cph) / 2.0;
    const double daysToFull = fmod(0.5 - p + 1.0, 1.0) * SYN;
    c.setTextSize(2); CenterText(c, PhaseName(p), cyTop + R + 14, accent);
    char info[44];
    snprintf(info, sizeof(info), "%d%% lit   full in %.0fd", (int)(illum * 100 + 0.5), daysToFull);
    c.setTextSize(1); CenterText(c, info, cyTop + R + 40, dim);
}

// --------------------------------------------------------------------------------- solar wind
void SpaceManager::DrawSolarWind(BandCanvas& c)
{
    const float gf = GlowFactor();
    const uint32_t fg     = space::ScaleColor(palette.fg, gf);
    const uint32_t dim    = space::ScaleColor(palette.dim, gf);
    const uint32_t faint  = space::ScaleColor(palette.faint, gf);
    const uint32_t warn   = space::ScaleColor(palette.warn, gf);
    const uint32_t alert  = space::ScaleColor(palette.alert, gf);
    const uint32_t green  = space::ScaleColor(lgfx::color888(120, 210, 90), gf);

    c.setTextSize(1); CenterText(c, "SOLAR WIND", 20, dim);

    const space::SolarWind& sw = feed.SolarWind();
    if (!sw.valid) { c.setTextSize(2); CenterText(c, "acquiring...", SCREEN_SIZE_DIV_2 - 8, dim); return; }

    c.setTextSize(4); CenterText(c, String((int)lround(sw.speedKms)) + " km/s", SCREEN_SIZE_DIV_2 - 52, fg);
    c.setTextSize(1); CenterText(c, String(sw.densityPcc, 1) + " p/cm3 density", SCREEN_SIZE_DIV_2 - 14, dim);

    // Bz: negative (southward) drives aurora -> red; positive (northward) -> green.
    const uint32_t bzCol = sw.bzNt <= -10 ? alert : sw.bzNt < 0 ? warn : green;
    c.setTextSize(3); CenterText(c, "Bz " + String(sw.bzNt, 1) + " nT", SCREEN_SIZE_DIV_2 + 18, bzCol);
    c.setTextSize(1);
    CenterText(c, sw.bzNt <= -10 ? "strongly southward - aurora drive"
                  : sw.bzNt < 0 ? "southward" : "northward (quiet)",
               SCREEN_SIZE - 32, faint);
}

// ----------------------------------------------------------------------- NOAA R/S/G scales
void SpaceManager::DrawScales(BandCanvas& c)
{
    const float gf = GlowFactor();
    const uint32_t fg     = space::ScaleColor(palette.fg, gf);
    const uint32_t dim    = space::ScaleColor(palette.dim, gf);
    const uint32_t faint  = space::ScaleColor(palette.faint, gf);
    const uint32_t warn   = space::ScaleColor(palette.warn, gf);
    const uint32_t alert  = space::ScaleColor(palette.alert, gf);

    const int cx = SCREEN_SIZE_DIV_2, cy = SCREEN_SIZE_DIV_2;
    const float startA = 135.0f, sweep = 270.0f;
    c.setTextSize(1); CenterText(c, "NOAA SPACE-WX SCALES", 18, dim);

    const space::NoaaScales& sc = feed.Scales();
    if (!sc.valid) { c.setTextSize(2); CenterText(c, "acquiring...", cy - 8, dim); return; }

    auto lvlColor = [&](int v) -> uint32_t { return v >= 4 ? alert : v >= 3 ? warn : v >= 1 ? fg : faint; };
    const int R = SCREEN_SIZE_DIV_2 - 22;
    // Three concentric gauges: G (outer), S (middle), R (inner).
    const int rings[3][2] = { {R - 20, R}, {R - 46, R - 26}, {R - 72, R - 52} };
    const int vals[3] = { sc.g, sc.s, sc.r };
    for (int i = 0; i < 3; ++i) {
        c.fillArc(cx, cy, rings[i][0], rings[i][1], startA, startA + sweep, faint);
        if (vals[i] > 0)
            c.fillArc(cx, cy, rings[i][0], rings[i][1], startA, startA + sweep * (vals[i] / 5.0f), lvlColor(vals[i]));
    }

    // Center readout.
    c.setTextSize(2);
    CenterText(c, "G" + String(sc.g), cy - 26, lvlColor(sc.g));
    CenterText(c, "S" + String(sc.s), cy - 2, lvlColor(sc.s));
    CenterText(c, "R" + String(sc.r), cy + 22, lvlColor(sc.r));
    c.setTextSize(1); CenterText(c, "geomag / radiation / radio", SCREEN_SIZE - 30, faint);
}

// ------------------------------------------------------------------------------- aurora (local)
void SpaceManager::DrawAurora(BandCanvas& c)
{
    const float gf = GlowFactor();
    const uint32_t fg     = space::ScaleColor(palette.fg, gf);
    const uint32_t dim    = space::ScaleColor(palette.dim, gf);
    const uint32_t faint  = space::ScaleColor(palette.faint, gf);
    const uint32_t warn   = space::ScaleColor(palette.warn, gf);
    const uint32_t green  = space::ScaleColor(lgfx::color888(120, 210, 90), gf);

    c.setTextSize(1); CenterText(c, "AURORA FORECAST", 22, dim);

    const space::SpaceWx& wx = feed.Wx();
    if (!wx.valid || !hasLatLon) { c.setTextSize(2); CenterText(c, "set location", SCREEN_SIZE_DIV_2 - 8, dim); return; }

    const double gm = fabs(space::GeomagLatitude(deviceLat, deviceLon));
    const float oval = space::AuroraOvalLat(wx.kp);

    const char* verdict; uint32_t vcol;
    if (gm >= oval)          { verdict = "OVERHEAD POSSIBLE"; vcol = green; }
    else if (gm >= oval - 5) { verdict = "LOW ON N HORIZON";  vcol = warn; }
    else                     { verdict = "UNLIKELY TONIGHT";  vcol = dim; }

    c.setTextSize(3); CenterText(c, verdict, SCREEN_SIZE_DIV_2 - 40, vcol);
    char l1[40]; snprintf(l1, sizeof(l1), "Kp %.1f   oval edge %.0f deg", wx.kp, oval);
    char l2[40]; snprintf(l2, sizeof(l2), "your geomag lat %.0f deg", gm);
    c.setTextSize(1); CenterText(c, l1, SCREEN_SIZE_DIV_2 + 2, dim);
    c.setTextSize(1); CenterText(c, l2, SCREEN_SIZE_DIV_2 + 24, faint);

    // Latitude bar 40..80 deg: oval edge tick vs your latitude marker.
    const int bx = SCREEN_SIZE_DIV_2 - 120, bw = 240, by = SCREEN_SIZE - 48;
    auto xfor = [&](float lat) { float f = (lat - 40.0f) / 40.0f; if (f < 0) f = 0; if (f > 1) f = 1; return bx + (int)(f * bw); };
    c.drawFastHLine(bx, by, bw, faint);
    c.fillRect(xfor(oval), by - 6, 2, 12, warn);                  // oval edge
    c.fillCircle(xfor((float)gm), by, 4, gm >= oval ? green : fg); // you
}

// ------------------------------------------------------------------------------- eclipse
void SpaceManager::DrawEclipse(BandCanvas& c)
{
    const float gf = GlowFactor();
    const uint32_t fg     = space::ScaleColor(palette.fg, gf);
    const uint32_t dim    = space::ScaleColor(palette.dim, gf);
    const uint32_t faint  = space::ScaleColor(palette.faint, gf);
    const uint32_t accent = space::ScaleColor(palette.accent, gf);
    static const char* MON[] = {"", "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};

    c.setTextSize(1); CenterText(c, "NEXT ECLIPSE", 20, dim);

    const time_t now = time(nullptr);
    if (now <= 1600000000) { c.setTextSize(2); CenterText(c, "awaiting clock", SCREEN_SIZE_DIV_2 - 8, dim); return; }

    long bestEp = 0; const EclipseEv* ev = nullptr;
    for (int i = 0; i < ECLIPSE_N; ++i) {
        const long ep = space::EpochUTC(ECLIPSES[i].y, ECLIPSES[i].mo, ECLIPSES[i].d, ECLIPSES[i].h, ECLIPSES[i].mi);
        if (ep > now && (ev == nullptr || ep < bestEp)) { bestEp = ep; ev = &ECLIPSES[i]; }
    }
    if (!ev) { c.setTextSize(2); CenterText(c, "no upcoming", SCREEN_SIZE_DIV_2 - 8, dim); return; }
    const bool solar = String(ev->label).indexOf("Solar") >= 0;

    // small disc icon: sun = bright ring, lunar = shaded disc
    const int dcx = SCREEN_SIZE_DIV_2, dcy = 66, dr = 20;
    if (solar) { c.fillCircle(dcx, dcy, dr, accent); c.fillCircle(dcx + 7, dcy - 4, dr, space::ScaleColor(palette.bg, 1.0f)); }
    else       { c.fillCircle(dcx, dcy, dr, space::ScaleColor(lgfx::color888(180, 80, 60), gf)); }

    c.setTextSize(2); CenterText(c, ev->label, SCREEN_SIZE_DIV_2 - 36, solar ? accent : fg);
    c.setTextSize(3); CenterText(c, FormatTMinus(bestEp - (long)now), SCREEN_SIZE_DIV_2 + 2, fg);
    char d[40]; snprintf(d, sizeof(d), "%s %d, %d   %02d:%02d UT", MON[ev->mo], ev->d, ev->y, ev->h, ev->mi);
    c.setTextSize(1); CenterText(c, d, SCREEN_SIZE_DIV_2 + 40, dim);
}

// ------------------------------------------------------------------------- meteor shower
void SpaceManager::DrawMeteor(BandCanvas& c)
{
    const float gf = GlowFactor();
    const uint32_t fg     = space::ScaleColor(palette.fg, gf);
    const uint32_t dim    = space::ScaleColor(palette.dim, gf);
    const uint32_t faint  = space::ScaleColor(palette.faint, gf);
    const uint32_t accent = space::ScaleColor(palette.accent, gf);
    const uint32_t warn   = space::ScaleColor(palette.warn, gf);

    c.setTextSize(1); CenterText(c, "NEXT METEOR SHOWER", 20, dim);

    const time_t now = time(nullptr);
    if (now <= 1600000000) { c.setTextSize(2); CenterText(c, "awaiting clock", SCREEN_SIZE_DIV_2 - 8, dim); return; }

    long bestEp = 0; const ShowerEv* sh = nullptr;
    for (int i = 0; i < SHOWER_N; ++i) {
        const long ep = space::EpochUTC(SHOWERS[i].y, SHOWERS[i].mo, SHOWERS[i].d, SHOWERS[i].h, 0);
        if (ep > now && (sh == nullptr || ep < bestEp)) { bestEp = ep; sh = &SHOWERS[i]; }
    }
    if (!sh) { c.setTextSize(2); CenterText(c, "no upcoming", SCREEN_SIZE_DIV_2 - 8, dim); return; }

    c.setTextSize(3); CenterText(c, sh->name, SCREEN_SIZE_DIV_2 - 52, accent);
    c.setTextSize(3); CenterText(c, FormatTMinus(bestEp - (long)now), SCREEN_SIZE_DIV_2 - 10, fg);
    c.setTextSize(2); CenterText(c, "ZHR ~" + String(sh->zhr) + "/hr", SCREEN_SIZE_DIV_2 + 26, dim);

    const int moon = (int)(MoonIllum((time_t)bestEp) * 100 + 0.5);
    char m[40]; snprintf(m, sizeof(m), "%d%% moon at peak%s", moon, moon >= 55 ? " (washout)" : "");
    c.setTextSize(1); CenterText(c, m, SCREEN_SIZE - 32, moon >= 55 ? warn : faint);
}

// --------------------------------------------------------------------- cosmic clock faces
void SpaceManager::DrawCosmicClock(BandCanvas& c)
{
    const float gf = GlowFactor();
    const uint32_t fg    = space::ScaleColor(palette.fg, gf);
    const uint32_t dim   = space::ScaleColor(palette.dim, gf);
    const uint32_t faint = space::ScaleColor(palette.faint, gf);

    const time_t now = time(nullptr);
    const int face = cardIndex % 3; // LST / Mars / Julian, cycled by the 4s card tick

    const char* title = face == 0 ? "SIDEREAL TIME" : face == 1 ? "MARS TIME (MTC)" : "JULIAN DATE";
    c.setTextSize(1); CenterText(c, title, SCREEN_SIZE_DIV_2 - 50, faint);

    if (now <= 1600000000) { c.setTextSize(3); CenterText(c, "--:--:--", SCREEN_SIZE_DIV_2 - 16, dim); return; }

    if (face == 0) {
        const bool loc = hasLatLon;
        const double h = loc ? LMSTHours(now, deviceLon) : GMSTHours(now);
        c.setTextSize(4); CenterText(c, Hms(h), SCREEN_SIZE_DIV_2 - 18, fg);
        c.setTextSize(1); CenterText(c, loc ? "local apparent" : "Greenwich (set location)", SCREEN_SIZE_DIV_2 + 24, dim);
    } else if (face == 1) {
        double mtc, msd; MarsTime(now, mtc, msd);
        c.setTextSize(4); CenterText(c, Hms(mtc), SCREEN_SIZE_DIV_2 - 18, fg);
        c.setTextSize(1); CenterText(c, "Mars Sol Date " + String((long)msd), SCREEN_SIZE_DIV_2 + 24, dim);
    } else {
        c.setTextSize(3); CenterText(c, String(JulianDate(now), 4), SCREEN_SIZE_DIV_2 - 14, fg);
        c.setTextSize(1); CenterText(c, "days since 4713 BC", SCREEN_SIZE_DIV_2 + 22, dim);
    }
}

// ------------------------------------------------------------------------------- star map
void SpaceManager::DrawStarMap(BandCanvas& c)
{
    const float gf = GlowFactor();
    const uint32_t dim   = space::ScaleColor(palette.dim, gf);
    const uint32_t faint = space::ScaleColor(palette.faint, gf);

    const int cx = SCREEN_SIZE_DIV_2, cy = SCREEN_SIZE_DIV_2, R = SCREEN_SIZE_DIV_2 - 8;
    c.drawCircle(cx, cy, R, faint);
    c.setTextSize(1); c.setTextColor(dim);
    c.drawString("N", cx - 3, cy - R + 3);
    c.drawString("S", cx - 3, cy + R - 11);
    c.drawString("E", cx + R - 10, cy - 4);
    c.drawString("W", cx - R + 3, cy - 4);

    const time_t now = time(nullptr);
    if (!hasLatLon || now <= 1600000000) {
        c.setTextSize(2); CenterText(c, hasLatLon ? "awaiting clock" : "set location", cy - 8, dim);
        return;
    }

    // Local apparent sidereal time (deg) + observer latitude drive each star's alt/az.
    const double lst = LMSTHours(now, deviceLon) * 15.0;
    const double latR = deviceLat * M_PI / 180.0;
    const double halfPi = M_PI / 2.0;
    int up = 0; const char* topName = nullptr; float topMag = 99;

    for (int i = 0; i < space::STAR_N; ++i) {
        const space::Star& st = space::STARS[i];
        const double H = (lst - st.raDeg) * M_PI / 180.0;
        const double decR = st.dec * M_PI / 180.0;
        const double sinAlt = sin(decR) * sin(latR) + cos(decR) * cos(latR) * cos(H);
        if (sinAlt <= 0.02) continue; // below (or right at) the horizon
        ++up;
        const double alt = asin(sinAlt);
        double cosAz = (sin(decR) - sinAlt * sin(latR)) / (cos(alt) * cos(latR));
        if (cosAz > 1) cosAz = 1;
        if (cosAz < -1) cosAz = -1;
        double az = acos(cosAz);
        if (sin(H) > 0) az = 2.0 * M_PI - az; // measured from North, clockwise through East
        const double rr = (1.0 - alt / halfPi) * R; // zenith at centre, horizon at rim
        const int x = cx + (int)(rr * sin(az));
        const int y = cy - (int)(rr * cos(az));

        float b = (3.2f - st.mag) / 3.2f; if (b < 0.18f) b = 0.18f; if (b > 1) b = 1;
        c.fillCircle(x, y, st.mag <= 1.0f ? 2 : 1, space::ScaleColor(palette.accent, b * gf));
        if (st.mag < topMag && st.name && st.name[0]) { topMag = st.mag; topName = st.name; }
    }

    c.setTextSize(1); CenterText(c, "NIGHT SKY", 16, dim);
    char info[40]; snprintf(info, sizeof(info), "%d stars up", up);
    c.setTextSize(1); CenterText(c, info, SCREEN_SIZE - 38, faint);
    if (topName) { c.setTextSize(1); CenterText(c, String("brightest: ") + topName, SCREEN_SIZE - 22, dim); }
}

// ------------------------------------------------------------------- tonight's observing window
void SpaceManager::DrawObserving(BandCanvas& c)
{
    const float gf = GlowFactor();
    const uint32_t fg    = space::ScaleColor(palette.fg, gf);
    const uint32_t dim   = space::ScaleColor(palette.dim, gf);
    const uint32_t faint = space::ScaleColor(palette.faint, gf);
    const uint32_t green = space::ScaleColor(lgfx::color888(120, 210, 140), gf); // "go" / dark

    c.setTextSize(1); CenterText(c, "OBSERVING WINDOW", 14, dim);

    if (!hasLatLon) { c.setTextSize(2); CenterText(c, "set location", SCREEN_SIZE_DIV_2 - 8, dim); return; }
    if (!obsValid)  { c.setTextSize(2); CenterText(c, "awaiting clock", SCREEN_SIZE_DIV_2 - 8, dim); return; }

    const int cx = SCREEN_SIZE_DIV_2, cy = SCREEN_SIZE_DIV_2;
    const int sunOuter = SCREEN_SIZE_DIV_2 - 24, sunInner = sunOuter - 16;
    const int moonOuter = sunInner - 3, moonInner = moonOuter - 6;

    // 24h ring in local solar time: midnight at the bottom, noon at top, AM left / PM right.
    auto solarHours = [&](long ep) -> double {
        const double utcH = fmod((double)(((ep % 86400) + 86400) % 86400) / 3600.0, 24.0);
        return fmod(utcH + deviceLon / 15.0 + 48.0, 24.0);
    };
    auto angleFor = [&](long ep) -> float { return (float)fmod(90.0 + solarHours(ep) / 24.0 * 360.0, 360.0); };

    // Twilight-depth band colours: day -> faint, deepening blue through twilight, green at true night.
    auto bandColor = [&](uint8_t b) -> uint32_t {
        switch (b) {
            case 0:  return faint;                                              // day
            case 1:  return space::ScaleColor(lgfx::color888(40, 70, 120), gf);  // civil
            case 2:  return space::ScaleColor(lgfx::color888(45, 110, 175), gf); // nautical
            case 3:  return space::ScaleColor(lgfx::color888(60, 160, 220), gf); // astronomical twilight
            default: return green;                                              // astronomical night
        }
    };

    const long stepSec = 86400 / OBS_SEG;
    const uint32_t moonCol = space::ScaleColor(lgfx::color888(200, 200, 210),
                                               (0.35f + 0.65f * (float)obsMoonIllum) * gf);
    for (int s = 0; s < OBS_SEG; ++s) {
        const long ep = obsSegEpoch0 + (long)s * stepSec;
        const float a = angleFor(ep);
        c.fillArc(cx, cy, sunInner, sunOuter, a - 2.6f, a + 2.6f, bandColor(obsSunBand[s]));
        if (obsMoonUp[s]) c.fillArc(cx, cy, moonInner, moonOuter, a - 2.6f, a + 2.6f, moonCol);
    }

    // "Now" marker just outside the ring.
    {
        const float a = angleFor(obsSegEpoch0) * (float)(M_PI / 180.0);
        const int mx = cx + (int)((sunOuter + 8) * cosf(a));
        const int my = cy + (int)((sunOuter + 8) * sinf(a));
        c.fillCircle(mx, my, 3, fg);
    }

    auto solarHM = [&](long ep) -> String {
        double h = solarHours(ep);
        int hh = (int)h, mm = (int)((h - hh) * 60.0 + 0.5);
        if (mm >= 60) { mm -= 60; hh = (hh + 1) % 24; }
        char b[8]; snprintf(b, sizeof(b), "%02d:%02d", hh, mm); return String(b);
    };

    // Centre readout.
    if (obsDusk && obsDawn) {
        const long len = obsDawn - obsDusk;
        char big[12];
        if (len >= 3600) snprintf(big, sizeof(big), "%ldh %02ld", len / 3600, (len % 3600) / 60);
        else             snprintf(big, sizeof(big), "%ldm", len / 60);
        c.setTextSize(5); CenterText(c, big, cy - 44, green);
        c.setTextSize(1); CenterText(c, "ASTRO-DARK TONIGHT", cy - 2, dim);
        c.setTextSize(1);
        CenterText(c, solarHM(obsDusk) + " - " + solarHM(obsDawn) + "  solar", cy + 16, faint);
    } else if (obsDarkNow && !obsDawn) {
        c.setTextSize(3); CenterText(c, "DARK", cy - 30, green);
        c.setTextSize(1); CenterText(c, "astronomical night all window", cy + 2, dim);
    } else {
        c.setTextSize(2); CenterText(c, "NO ASTRO NIGHT", cy - 22, dim);
        c.setTextSize(1); CenterText(c, "sun stays above -18 deg", cy + 2, faint);
    }

    // Moon line: how much it intrudes on the dark window.
    const int pct = (int)(obsMoonIllum * 100 + 0.5);
    String moon;
    if (obsDusk && obsDawn && obsMoonUpMin <= 0) moon = "Moon down - dark skies";
    else if (obsMoonUpMin > 0) {
        char m[40];
        if (obsMoonUpMin >= 60) snprintf(m, sizeof(m), "Moon %d%%  up %ldh%02ld", pct, obsMoonUpMin / 60, obsMoonUpMin % 60);
        else                    snprintf(m, sizeof(m), "Moon %d%%  up %ldm", pct, obsMoonUpMin);
        moon = m;
    } else moon = "Moon " + String(pct) + "%";
    c.setTextSize(1); CenterText(c, moon, cy + 36, pct >= 55 && obsMoonUpMin > 0 ? dim : faint);
}

// --------------------------------------------------------------------------- ISS visible pass
void SpaceManager::DrawIssPass(BandCanvas& c)
{
    const float gf = GlowFactor();
    const uint32_t fg    = space::ScaleColor(palette.fg, gf);
    const uint32_t dim   = space::ScaleColor(palette.dim, gf);
    const uint32_t faint = space::ScaleColor(palette.faint, gf);
    const uint32_t green = space::ScaleColor(lgfx::color888(120, 210, 90), gf);

    c.setTextSize(1); CenterText(c, "NEXT ISS PASS", 20, dim);
    if (!hasLatLon) { c.setTextSize(2); CenterText(c, "set location", SCREEN_SIZE_DIV_2 - 8, dim); return; }
    if (!passValid) { c.setTextSize(2); CenterText(c, "computing...", SCREEN_SIZE_DIV_2 - 8, dim); return; }

    const time_t now = time(nullptr);
    const char* dir = Compass8(passAzRise);

    if (now >= passRiseEpoch && now <= passSetEpoch) {
        c.setTextSize(3); CenterText(c, "OVERHEAD NOW", SCREEN_SIZE_DIV_2 - 18, passVisible ? green : fg);
        char l[44]; snprintf(l, sizeof(l), "max %d deg   rises %s", (int)passMaxEl, dir);
        c.setTextSize(1); CenterText(c, l, SCREEN_SIZE_DIV_2 + 16, dim);
        return;
    }

    c.setTextSize(2); CenterText(c, passVisible ? "VISIBLE PASS" : "daylight pass", SCREEN_SIZE_DIV_2 - 56,
                                 passVisible ? green : dim);
    c.setTextSize(3); CenterText(c, FormatTMinus(passRiseEpoch - (long)now), SCREEN_SIZE_DIV_2 - 22, fg);
    char l[48]; snprintf(l, sizeof(l), "rises %s   max %d deg", dir, (int)passMaxEl);
    c.setTextSize(1); CenterText(c, l, SCREEN_SIZE_DIV_2 + 18, dim);
    long dur = passSetEpoch - passRiseEpoch; if (dur < 0) dur = 0;
    c.setTextSize(1); CenterText(c, String("above horizon ~") + String(dur / 60) + " min", SCREEN_SIZE - 34, faint);
}
