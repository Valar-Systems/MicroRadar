#include "SpaceManager.h"

#include <math.h>
#include <time.h>

#include "Layout.h"

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
