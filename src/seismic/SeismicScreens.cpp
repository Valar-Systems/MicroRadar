#include "SeismicManager.h"

#include <math.h>
#include <time.h>

#include "Layout.h"

// The Seismic edition screens. Each draws one full frame into the band canvas in absolute screen
// coordinates (the S3 renders a single full-height band). Colours come from the palette scaled by
// GlowFactor() so night-dim fades everything uniformly, and from seismic::MagColor() for the
// magnitude-coded blips/rows.

namespace {

constexpr int RADAR_R = SCREEN_SIZE_DIV_2 - 30; // outer ring; matches ProjectQuakeToScreen

String FitWidth(BandCanvas& c, String s, int maxW)
{
    if (c.textWidth(s) <= maxW) return s;
    while (s.length() > 1 && c.textWidth(s + "...") > maxW) s.remove(s.length() - 1);
    return s + "...";
}

// Blip radius in px for a magnitude (clamped 2..8).
int MagRadius(float m)
{
    int r = 2 + (int)lroundf((m - 1.0f) * 0.9f);
    if (r < 2) r = 2;
    if (r > 8) r = 8;
    return r;
}

// 1.0 for a fresh quake, fading toward 0.45 over ~24 h, so the radar reads "what's happening now".
float AgeFade(long epoch)
{
    const time_t now = time(nullptr);
    if (now < 1600000000 || epoch <= 0) return 1.0f;
    const float h = ((float)now - (float)epoch) / 3600.0f;
    if (h <= 1.0f) return 1.0f;
    float f = 1.0f - (h - 1.0f) / 23.0f * 0.55f;
    if (f < 0.45f) f = 0.45f;
    return f;
}

const char* CompassPoint(double brg)
{
    static const char* pts[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    int i = (int)lround(brg / 45.0) & 7;
    return pts[i];
}

} // namespace

// --------------------------------------------------------------------------------- radar
void SeismicManager::DrawRadar(BandCanvas& c)
{
    const float gf = GlowFactor();
    const uint32_t fg    = seismic::ScaleColor(palette.fg, gf);
    const uint32_t dim   = seismic::ScaleColor(palette.dim, gf);
    const uint32_t faint = seismic::ScaleColor(palette.faint, gf);
    const uint32_t accent= seismic::ScaleColor(palette.accent, gf);

    const int cx = SCREEN_SIZE_DIV_2, cy = SCREEN_SIZE_DIV_2;

    // Range rings + crosshairs.
    c.drawCircle(cx, cy, RADAR_R, faint);
    c.drawCircle(cx, cy, RADAR_R * 2 / 3, faint);
    c.drawCircle(cx, cy, RADAR_R / 3, faint);
    c.drawFastHLine(cx - RADAR_R, cy, 2 * RADAR_R, faint);
    c.drawFastVLine(cx, cy - RADAR_R, 2 * RADAR_R, faint);
    c.setTextSize(1);
    CenterText(c, "N", cy - RADAR_R - 12, dim);

    // Header: clock + outer-ring range.
    DrawClock(c);
    char rng[16];
    snprintf(rng, sizeof(rng), "%.0f km", radiusKm);
    c.setTextSize(1);
    c.setTextColor(faint);
    c.drawString(rng, 14, SCREEN_SIZE - 22);

    if (!hasLatLon) {
        c.setTextSize(2);
        CenterText(c, "SET LOCATION", cy - 12, fg);
        c.setTextSize(1);
        CenterText(c, "add lat/lon in web config", cy + 14, dim);
        return;
    }

    // "You" at the centre.
    c.fillCircle(cx, cy, 3, accent);

    // Plot the nearby quakes; track the nearest for the readout line.
    const std::vector<seismic::Quake>& qs = feed.Nearby();
    const seismic::Quake* nearest = nullptr;
    double nearestD = 1e18;
    for (const seismic::Quake& q : qs) {
        const auto p = ProjectQuakeToScreen(q);
        const float fade = AgeFade(q.timeEpoch);
        const uint32_t col = seismic::ScaleColor(seismic::MagColor(q.mag), gf * fade);
        const int r = MagRadius(q.mag);
        c.fillCircle(p.first, p.second, r, col);
        if (q.mag >= 5.0f) c.drawCircle(p.first, p.second, r + 3, col); // ring the notable ones
        const double d = seismic::DistanceKm(deviceLat, deviceLon, q.lat, q.lon);
        if (d < nearestD) { nearestD = d; nearest = &q; }
    }

    if (qs.empty()) {
        c.setTextSize(1);
        CenterText(c, "no quakes in range", cy + RADAR_R / 2, dim);
    } else if (nearest) {
        char line[48];
        snprintf(line, sizeof(line), "M%.1f  %.0fkm %s",
                 nearest->mag, nearestD, CompassPoint(seismic::BearingDeg(deviceLat, deviceLon, nearest->lat, nearest->lon)));
        c.setTextSize(2);
        CenterText(c, line, SCREEN_SIZE - 40, fg);
    }

    // Contact count, top-left.
    char cnt[16];
    snprintf(cnt, sizeof(cnt), "%u", (unsigned)qs.size());
    c.setTextSize(2);
    c.setTextColor(dim);
    c.drawString(cnt, 16, 30);
}

// --------------------------------------------------------------------------------- list
void SeismicManager::DrawList(BandCanvas& c)
{
    const float gf = GlowFactor();
    const uint32_t fg    = seismic::ScaleColor(palette.fg, gf);
    const uint32_t dim   = seismic::ScaleColor(palette.dim, gf);
    const uint32_t faint = seismic::ScaleColor(palette.faint, gf);

    const bool local = hasLatLon;
    const std::vector<seismic::Quake> shown = local ? SortedNearbyByDistance() : feed.Recent();

    c.setTextSize(2);
    CenterText(c, local ? "NEARBY" : "RECENT", 26, fg);

    if (shown.empty()) {
        c.setTextSize(1);
        CenterText(c, local ? "no quakes in range" : "waiting for feed...", SCREEN_SIZE_DIV_2, dim);
        return;
    }

    const int LIST_TOP = 70, LIST_ROW_H = 34;
    const int rows = (SCREEN_SIZE - LIST_TOP - 24) / LIST_ROW_H;
    int shownCount = 0;
    for (int i = 0; i < rows; ++i) {
        const int idx = listScroll + i;
        if (idx >= (int)shown.size()) break;
        const seismic::Quake& q = shown[idx];
        const int y = LIST_TOP + i * LIST_ROW_H;
        const uint32_t mc = seismic::ScaleColor(seismic::MagColor(q.mag), gf);

        char mag[8];
        snprintf(mag, sizeof(mag), "M%.1f", q.mag);
        c.setTextSize(2);
        c.setTextColor(mc);
        c.drawString(mag, 16, y);

        // right column: distance+bearing (local) or time-ago
        String right;
        if (local) {
            char rb[24];
            snprintf(rb, sizeof(rb), "%.0fkm %s", q.distanceKm, CompassPoint(q.bearingDeg));
            right = rb;
        } else {
            right = FormatAgo(q.timeEpoch);
        }
        c.setTextSize(1);
        c.setTextColor(dim);
        c.drawString(right, SCREEN_SIZE - 16 - c.textWidth(right), y + 2);

        // place, fit between the mag chip and the right column
        c.setTextColor(faint);
        const int placeX = 86;
        const int placeMaxW = (SCREEN_SIZE - 16 - c.textWidth(right) - 8) - placeX;
        c.drawString(FitWidth(c, q.place, placeMaxW > 30 ? placeMaxW : 30), placeX, y + 14);
        shownCount++;
    }

    // simple scroll hint
    if (listScroll > 0 || listScroll + shownCount < (int)shown.size()) {
        char pos[16];
        snprintf(pos, sizeof(pos), "%d-%d/%u", listScroll + 1, listScroll + shownCount, (unsigned)shown.size());
        c.setTextSize(1);
        c.setTextColor(faint);
        c.drawString(pos, SCREEN_SIZE_DIV_2 - c.textWidth(pos) / 2, SCREEN_SIZE - 20);
    }
}

// --------------------------------------------------------------------------------- stats
void SeismicManager::DrawStats(BandCanvas& c)
{
    const float gf = GlowFactor();
    const uint32_t fg    = seismic::ScaleColor(palette.fg, gf);
    const uint32_t dim   = seismic::ScaleColor(palette.dim, gf);
    const uint32_t faint = seismic::ScaleColor(palette.faint, gf);

    DrawClock(c);

    const std::vector<seismic::Quake>& rec = feed.Recent();
    if (rec.empty()) {
        c.setTextSize(2);
        CenterText(c, "STATS", 30, fg);
        c.setTextSize(1);
        CenterText(c, "waiting for feed...", SCREEN_SIZE_DIV_2, dim);
        return;
    }

    // Biggest in the worldwide window.
    const seismic::Quake* big = &rec.front();
    for (const seismic::Quake& q : rec) if (q.mag > big->mag) big = &q;

    c.setTextSize(1);
    CenterText(c, "BIGGEST (7d)", 40, dim);
    char mag[8];
    snprintf(mag, sizeof(mag), "M%.1f", big->mag);
    c.setTextSize(6);
    CenterText(c, mag, 56, seismic::ScaleColor(seismic::MagColor(big->mag), gf));
    c.setTextSize(1);
    CenterText(c, FitWidth(c, big->place, SCREEN_SIZE - 70), 120, fg);
    char meta[40];
    snprintf(meta, sizeof(meta), "depth %.0f km   %s ago", big->depthKm, FormatAgo(big->timeEpoch).c_str());
    CenterText(c, meta, 140, faint);

    // Activity: count + time since the last significant (>= 4.5) quake worldwide.
    char cnt[40];
    snprintf(cnt, sizeof(cnt), "%u quakes / 7d", (unsigned)rec.size());
    c.setTextSize(2);
    CenterText(c, cnt, SCREEN_SIZE_DIV_2 + 30, fg);

    const seismic::Quake* lastSig = nullptr;
    for (const seismic::Quake& q : rec)
        if (q.mag >= 4.5f && (!lastSig || q.timeEpoch > lastSig->timeEpoch)) lastSig = &q;
    String sig = lastSig ? ("last M4.5+: " + FormatAgo(lastSig->timeEpoch) + " ago") : String("no M4.5+ in window");
    c.setTextSize(1);
    CenterText(c, sig, SCREEN_SIZE - 60, dim);
}

// --------------------------------------------------------------------------------- detail card
void SeismicManager::DrawDetailCard(BandCanvas& c)
{
    const float gf = GlowFactor();
    const uint32_t fg    = seismic::ScaleColor(palette.fg, gf);
    const uint32_t dim   = seismic::ScaleColor(palette.dim, gf);
    const uint32_t faint = seismic::ScaleColor(palette.faint, gf);
    const uint32_t mc    = seismic::ScaleColor(seismic::MagColor(selected.mag), gf);

    const seismic::Quake& q = selected;

    // Card background panel.
    const int m = 18;
    c.fillRoundRect(m, m, SCREEN_SIZE - 2 * m, SCREEN_SIZE - 2 * m, 16, palette.bg);
    c.drawRoundRect(m, m, SCREEN_SIZE - 2 * m, SCREEN_SIZE - 2 * m, 16, mc);

    char mag[8];
    snprintf(mag, sizeof(mag), "M%.1f", q.mag);
    c.setTextSize(6);
    CenterText(c, mag, 56, mc);
    c.setTextSize(2);
    CenterText(c, seismic::MagWord(q.mag), 118, fg);

    c.setTextSize(1);
    CenterText(c, FitWidth(c, q.place, SCREEN_SIZE - 90), 152, fg);

    int y = 184;
    auto row = [&](const String& s, uint32_t col) { CenterText(c, s, y, col); y += 20; };

    char depth[32];
    snprintf(depth, sizeof(depth), "depth %.1f km   %s",
             q.depthKm, q.magType.length() ? q.magType.c_str() : "");
    row(depth, dim);

    const time_t t = q.timeEpoch;
    char when[40] = "";
    if (t > 1600000000) {
        struct tm g; gmtime_r(&t, &g);
        snprintf(when, sizeof(when), "%02d:%02d UTC   %s ago", g.tm_hour, g.tm_min, FormatAgo(q.timeEpoch).c_str());
    }
    row(when, dim);

    if (hasLatLon) {
        const double d = seismic::DistanceKm(deviceLat, deviceLon, q.lat, q.lon);
        const double b = seismic::BearingDeg(deviceLat, deviceLon, q.lat, q.lon);
        char dist[32];
        snprintf(dist, sizeof(dist), "%.0f km %s of you", d, CompassPoint(b));
        row(dist, fg);
    }

    if (q.tsunami) row("TSUNAMI flag set", seismic::ScaleColor(palette.alert, gf));

    c.setTextSize(1);
    CenterText(c, "tap to close", SCREEN_SIZE - 40, faint);
}

// --------------------------------------------------------------------------------- chrome
void SeismicManager::DrawScreenDots(BandCanvas& c) const
{
    const int n = 3, gap = 10;
    const int totalW = (n - 1) * gap;
    int x = SCREEN_SIZE_DIV_2 - totalW / 2;
    const int y = SCREEN_SIZE - 12;
    for (int i = 0; i < n; ++i) {
        const bool active = (i == (int)screen);
        c.fillCircle(x, y, active ? 2 : 1, active ? palette.fg : palette.faint);
        x += gap;
    }
}

void SeismicManager::DrawClock(BandCanvas& c) const
{
    const time_t utc = time(nullptr);
    char hhmm[8] = "--:--";
    if (utc > 1600000000) {
        struct tm t; gmtime_r(&utc, &t);
        snprintf(hhmm, sizeof(hhmm), "%02d:%02d", t.tm_hour, t.tm_min);
    }
    BandCanvas& cc = const_cast<BandCanvas&>(c);
    cc.setTextSize(1);
    cc.setTextColor(seismic::ScaleColor(palette.faint, GlowFactor()));
    cc.drawString(hhmm, SCREEN_SIZE - 16 - cc.textWidth(hhmm), 30);
}
