#include "AnglerManager.h"

#include <math.h>
#include <stdio.h>
#include <time.h>

#include "Layout.h"
#include "astro/Astro.h"

// The Angler edition screens. Each draws one full frame into the band canvas in absolute screen
// coordinates (the S3 renders a single full-height band). Colours come from the palette scaled by
// GlowFactor(); the warm gold accent marks the sun and an active/major bite window.

namespace {

constexpr long NTP_FLOOR = 1600000000;

String FitWidth(BandCanvas& c, String s, int maxW)
{
    if (c.textWidth(s) <= maxW) return s;
    while (s.length() > 1 && c.textWidth(s + "...") > maxW) s.remove(s.length() - 1);
    return s + "...";
}

// "H:MM" for >= 1h, else "Mm". Used for bite-window countdowns.
String FormatDur(long secs)
{
    if (secs < 0) secs = 0;
    const int h = secs / 3600, m = (secs % 3600) / 60;
    char b[16];
    if (h > 0) snprintf(b, sizeof(b), "%d:%02d", h, m);
    else       snprintf(b, sizeof(b), "%dm", m);
    return String(b);
}

} // namespace

// --------------------------------------------------------------------------------- bite forecast (hero)
void AnglerManager::DrawBite(BandCanvas& c)
{
    const float gf = GlowFactor();
    const uint32_t fg     = angler::ScaleColor(palette.fg, gf);
    const uint32_t dim    = angler::ScaleColor(palette.dim, gf);
    const uint32_t faint  = angler::ScaleColor(palette.faint, gf);
    const uint32_t accent = angler::ScaleColor(palette.accent, gf);

    const int cx = SCREEN_SIZE_DIV_2, cy = SCREEN_SIZE_DIV_2;
    const int R = BiteRingRadius();
    const time_t now = time(nullptr);

    c.setTextSize(1);
    CenterText(c, "BITE FORECAST", 16, dim);

    // Daytime arc (sunrise -> sunset), a hair brighter, so the lit part of the 24h dial reads.
    if (today.sunrise && today.sunset && today.sunset > today.sunrise) {
        for (time_t t = today.sunrise; t <= today.sunset; t += 300) {
            const auto p = RingXY(LocalSod(t), R);
            c.fillCircle(p.first, p.second, 1, faint);
        }
    }
    // Base ring + quarter ticks with tiny hour labels.
    c.drawCircle(cx, cy, R, faint);
    for (int q = 0; q < 4; ++q) {
        const auto o = RingXY((long)q * 21600, R + 7);
        const auto i = RingXY((long)q * 21600, R - 7);
        c.drawLine(i.first, i.second, o.first, o.second, dim);
    }
    c.setTextColor(faint);
    { const auto p = RingXY(0, R + 18);      c.drawString("12a", p.first - c.textWidth("12a") / 2, p.second - 4); }
    { const auto p = RingXY(21600, R + 22);  c.drawString("6a",  p.first - c.textWidth("6a") / 2,  p.second - 4); }
    { const auto p = RingXY(43200, R + 18);  c.drawString("12p", p.first - c.textWidth("12p") / 2, p.second - 4); }
    { const auto p = RingXY(64800, R + 22);  c.drawString("6p",  p.first - c.textWidth("6p") / 2,  p.second - 4); }

    // Sunrise / sunset markers.
    if (today.sunrise) { const auto p = RingXY(LocalSod(today.sunrise), R); c.fillCircle(p.first, p.second, 3, accent); }
    if (today.sunset)  { const auto p = RingXY(LocalSod(today.sunset),  R); c.fillCircle(p.first, p.second, 3, accent); }

    // Period arcs: majors thick gold, minors thinner teal. Drawn along the ring from start to end.
    for (int i = 0; i < today.count; ++i) {
        const angler::Period& p = today.periods[i];
        const bool major = p.major();
        const uint32_t col = major ? accent : fg;
        for (time_t t = p.start; t <= p.end; t += 180) {
            const auto q = RingXY(LocalSod(t), R);
            c.fillCircle(q.first, q.second, major ? 4 : 3, col);
        }
        const auto ctr = PeriodMarkerXY(p);           // tap target marker
        c.fillCircle(ctr.first, ctr.second, major ? 3 : 2, palette.bg);
        c.drawCircle(ctr.first, ctr.second, major ? 6 : 4, col);
    }

    // "Now" hand.
    if (now > NTP_FLOOR) {
        const auto p = RingXY(LocalSod(now), R - 12);
        c.drawLine(cx, cy, p.first, p.second, accent);
        c.fillCircle(p.first, p.second, 3, accent);
    }
    c.fillCircle(cx, cy, 3, dim);

    // Centre hub: the headline countdown + the day rating.
    angler::Period act, nxt;
    const bool active = (now > NTP_FLOOR) && ActiveNow(now, act);
    const bool haveNext = (now > NTP_FLOOR) && NextPeriod(now, nxt, false);

    c.setTextSize(1);
    CenterText(c, active ? "FEEDING NOW" : "NEXT BITE", cy - 54, dim);
    c.setTextSize(3);
    if (active)        CenterText(c, FormatDur(act.end - now) + " left", cy - 40, accent);
    else if (haveNext) CenterText(c, "in " + FormatDur(nxt.start - now), cy - 40, fg);
    else               CenterText(c, "--", cy - 40, dim);

    c.setTextSize(1);
    const angler::Period& hub = active ? act : nxt;
    if (active || haveNext) {
        String k = String(angler::PeriodShort(hub.kind)) + " - " + angler::PeriodLabel(hub.kind) +
                   " " + LocalHM(hub.center);
        CenterText(c, k, cy - 2, dim);
    }

    // Rating stars + label.
    const int stars = angler::RatingStars(today.rating);
    const int sx0 = cx - (4 * 16) / 2 + 8;
    for (int i = 0; i < 4; ++i) {
        const bool on = i < stars;
        c.fillCircle(sx0 + i * 16, cy + 26, 4, on ? accent : palette.bg);
        c.drawCircle(sx0 + i * 16, cy + 26, 4, on ? accent : faint);
    }
    c.setTextColor(dim);
    CenterText(c, String(angler::RatingLabel(today.rating)) + " day", cy + 44, dim);
}

// --------------------------------------------------------------------------------- moon
void AnglerManager::DrawMoon(BandCanvas& c)
{
    const float gf = GlowFactor();
    const uint32_t fg    = angler::ScaleColor(palette.fg, gf);
    const uint32_t dim   = angler::ScaleColor(palette.dim, gf);
    const uint32_t faint = angler::ScaleColor(palette.faint, gf);
    const uint32_t lit   = angler::ScaleColor(lgfx::color888(226, 232, 214), gf);

    const int cx = SCREEN_SIZE_DIV_2;
    const time_t now = time(nullptr);

    c.setTextSize(1);
    CenterText(c, "MOON", 20, dim);

    double ra, dec, f = today.moonIllum;
    bool waxing = true;
    if (now > NTP_FLOOR) {
        double il0, il1, r2, d2;
        space::astro::MoonRaDec(now, ra, dec, il0);
        space::astro::MoonRaDec(now + 3600, r2, d2, il1);
        f = il0;
        waxing = il1 >= il0;
    }

    // Phase disk: dark globe, then paint the lit fraction. Terminator x per scanline = xw*(1-2f).
    const int mcx = cx, mcy = 132, rr = 74;
    c.fillCircle(mcx, mcy, rr, angler::ScaleColor(palette.faint, gf * 0.7f));
    for (int dy = -rr; dy <= rr; ++dy) {
        const double xw = sqrt((double)rr * rr - (double)dy * dy);
        const double xt = xw * (1.0 - 2.0 * f);
        int x1, x2;
        if (waxing) { x1 = mcx + (int)lround(xt); x2 = mcx + (int)lround(xw); }
        else        { x1 = mcx - (int)lround(xw); x2 = mcx - (int)lround(xt); }
        if (x2 > x1) c.drawFastHLine(x1, mcy + dy, x2 - x1, lit);
    }
    c.drawCircle(mcx, mcy, rr, faint);

    c.setTextSize(2);
    CenterText(c, angler::MoonPhaseName(f, waxing), 226, fg);
    char pct[16];
    snprintf(pct, sizeof(pct), "%.0f%% lit", f * 100.0);
    c.setTextSize(1);
    CenterText(c, pct, 252, dim);

    // Rise / transit / set for the day.
    int y = 286;
    auto row = [&](const char* lbl, time_t t) {
        String s = String(lbl) + "  " + LocalHM(t);
        CenterText(c, s, y, t ? fg : faint);
        y += 22;
    };
    row("Rise  ", today.moonrise);
    row("High  ", today.moonTransit);
    row("Set   ", today.moonset);
}

// --------------------------------------------------------------------------------- sun
void AnglerManager::DrawSun(BandCanvas& c)
{
    const float gf = GlowFactor();
    const uint32_t fg     = angler::ScaleColor(palette.fg, gf);
    const uint32_t dim    = angler::ScaleColor(palette.dim, gf);
    const uint32_t faint  = angler::ScaleColor(palette.faint, gf);
    const uint32_t accent = angler::ScaleColor(palette.accent, gf);

    const int cx = SCREEN_SIZE_DIV_2;
    const time_t now = time(nullptr);

    c.setTextSize(1);
    CenterText(c, "SUN", 20, dim);

    const int left = 60, right = SCREEN_SIZE - 60, horizon = SCREEN_SIZE_DIV_2 + 10;
    const int width = right - left, arcH = 96;
    c.drawFastHLine(left - 10, horizon, width + 20, faint);

    // Day arc (sunrise -> sunset) as a dotted dome above the horizon.
    for (int i = 0; i <= 60; ++i) {
        const double u = i / 60.0;
        const int x = left + (int)lround(u * width);
        const int yy = horizon - (int)lround(sin(u * M_PI) * arcH);
        c.fillCircle(x, yy, 1, faint);
    }

    // Sun marker: fraction of daylight elapsed along the arc; below the horizon at night.
    if (today.sunrise && today.sunset && today.sunset > today.sunrise && now > NTP_FLOOR) {
        double u = (double)(now - today.sunrise) / (double)(today.sunset - today.sunrise);
        if (u >= 0.0 && u <= 1.0) {
            const int x = left + (int)lround(u * width);
            const int yy = horizon - (int)lround(sin(u * M_PI) * arcH);
            c.fillCircle(x, yy, 8, accent);
        } else {
            const int x = (u < 0.0) ? left : right;
            c.fillCircle(x, horizon + 16, 6, dim);
        }
    }

    c.setTextSize(2);
    c.setTextColor(accent);
    c.drawString(LocalHM(today.sunrise), left - 14, horizon + 16);
    { const String s = LocalHM(today.sunset); c.drawString(s, right + 14 - c.textWidth(s), horizon + 16); }
    c.setTextSize(1);
    c.setTextColor(dim);
    c.drawString("rise", left - 14, horizon + 40);
    c.drawString("set", right + 14 - c.textWidth("set"), horizon + 40);

    if (today.sunrise && today.sunset && today.sunset > today.sunrise) {
        const long dl = today.sunset - today.sunrise;
        char b[24];
        snprintf(b, sizeof(b), "%ldh %02ldm daylight", dl / 3600, (dl % 3600) / 60);
        c.setTextSize(1);
        CenterText(c, b, SCREEN_SIZE - 52, fg);
        CenterText(c, "golden hour near rise & set", SCREEN_SIZE - 32, faint);
    }
}

// --------------------------------------------------------------------------------- splash
void AnglerManager::DrawSplash(BandCanvas& c)
{
    const float gf = GlowFactor();
    const uint32_t fg     = angler::ScaleColor(palette.fg, gf);
    const uint32_t dim    = angler::ScaleColor(palette.dim, gf);
    const uint32_t accent = angler::ScaleColor(palette.accent, gf);

    c.setTextSize(3);
    CenterText(c, "BLIPSCOPE", SCREEN_SIZE_DIV_2 - 44, fg);
    c.setTextSize(2);
    CenterText(c, "angler edition", SCREEN_SIZE_DIV_2 - 8, dim);

    c.setTextSize(1);
    const char* hint = !hasLatLon ? "add your location in web config"
                                  : "computing the bite forecast...";
    CenterText(c, hint, SCREEN_SIZE_DIV_2 + 36, accent);
}

// --------------------------------------------------------------------------------- clock
void AnglerManager::DrawClock(BandCanvas& c)
{
    const float gf = GlowFactor();
    const uint32_t fg    = angler::ScaleColor(palette.fg, gf);
    const uint32_t faint = angler::ScaleColor(palette.faint, gf);

    const time_t utc = time(nullptr);
    char hhmm[16] = "--:--";
    if (utc > NTP_FLOOR) {
        const time_t local = utc + tzOffsetSec;
        struct tm t; gmtime_r(&local, &t);
        snprintf(hhmm, sizeof(hhmm), "%02d:%02d", t.tm_hour, t.tm_min);
    }
    c.setTextSize(1); CenterText(c, "LOCAL", SCREEN_SIZE_DIV_2 - 44, faint);
    c.setTextSize(6); CenterText(c, hhmm, SCREEN_SIZE_DIV_2 - 28, fg);
}

// --------------------------------------------------------------------------------- detail card
void AnglerManager::DrawDetailCard(BandCanvas& c)
{
    const float gf = GlowFactor();
    const uint32_t fg     = angler::ScaleColor(palette.fg, gf);
    const uint32_t dim    = angler::ScaleColor(palette.dim, gf);
    const uint32_t faint  = angler::ScaleColor(palette.faint, gf);
    const uint32_t accent = angler::ScaleColor(palette.accent, gf);

    const time_t now = time(nullptr);
    const int m = 18;
    c.fillRoundRect(m, m, SCREEN_SIZE - 2 * m, SCREEN_SIZE - 2 * m, 16, palette.bg);

    int y = 64;
    auto row = [&](const String& s, uint32_t col) { CenterText(c, s, y, col); y += 24; };

    if (current == Screen::Bite && selectedPeriod >= 0 && selectedPeriod < today.count) {
        // ---- a single feeding period ----
        const angler::Period& p = today.periods[selectedPeriod];
        const bool major = p.major();
        const uint32_t head = major ? accent : fg;
        c.drawRoundRect(m, m, SCREEN_SIZE - 2 * m, SCREEN_SIZE - 2 * m, 16, head);

        c.setTextSize(2);
        row(String(angler::PeriodShort(p.kind)) + " - " + angler::PeriodLabel(p.kind), head);
        y += 6;
        c.setTextSize(3);
        row(LocalHM(p.start) + " - " + LocalHM(p.end), fg);
        y += 4;
        c.setTextSize(2);
        if (now > NTP_FLOOR) {
            if (now < p.start)      row("opens in " + FormatDur(p.start - now), dim);
            else if (now < p.end)   row("active - " + FormatDur(p.end - now) + " left", accent);
            else                    row("ended", dim);
        }
        const char* ev = (p.kind == angler::PeriodKind::MajorOverhead)  ? "Moon overhead"
                       : (p.kind == angler::PeriodKind::MajorUnderfoot) ? "Moon underfoot"
                       : (p.kind == angler::PeriodKind::MinorRise)      ? "Moonrise"
                                                                        : "Moonset";
        row(String(ev) + " " + LocalHM(p.center), dim);
        row(major ? "major - stronger feeding" : "minor - lighter feeding", faint);
        bool green = (today.sunrise && p.start <= today.sunrise && today.sunrise <= p.end) ||
                     (today.sunset  && p.start <= today.sunset  && today.sunset  <= p.end);
        if (green) row("+ green window (dawn/dusk)", accent);

    } else if (current == Screen::Bite) {
        // ---- day summary ----
        c.drawRoundRect(m, m, SCREEN_SIZE - 2 * m, SCREEN_SIZE - 2 * m, 16, accent);
        c.setTextSize(2);
        row("TODAY", accent);
        y += 4;
        const int stars = angler::RatingStars(today.rating);
        const int cx = SCREEN_SIZE_DIV_2, sx0 = cx - (4 * 18) / 2 + 9;
        for (int i = 0; i < 4; ++i) {
            const bool on = i < stars;
            c.fillCircle(sx0 + i * 18, y + 4, 5, on ? accent : palette.bg);
            c.drawCircle(sx0 + i * 18, y + 4, 5, on ? accent : faint);
        }
        y += 30;
        c.setTextSize(2);
        row(angler::RatingLabel(today.rating), fg);
        char pct[24];
        snprintf(pct, sizeof(pct), "Moon %.0f%% lit", today.moonIllum * 100.0);
        row(pct, dim);
        row("Sun " + LocalHM(today.sunrise) + " - " + LocalHM(today.sunset), dim);
        if (today.sunBump) row("+ green window today", accent);
        row(String(today.count) + " feeding periods", faint);

    } else if (current == Screen::Moon) {
        c.drawRoundRect(m, m, SCREEN_SIZE - 2 * m, SCREEN_SIZE - 2 * m, 16, fg);
        double ra, dec, f = today.moonIllum; bool waxing = true;
        if (now > NTP_FLOOR) {
            double il0, il1, r2, d2;
            space::astro::MoonRaDec(now, ra, dec, il0);
            space::astro::MoonRaDec(now + 3600, r2, d2, il1);
            f = il0; waxing = il1 >= il0;
        }
        c.setTextSize(2);
        row("MOON", fg); y += 6;
        row(angler::MoonPhaseName(f, waxing), fg);
        char pct[16]; snprintf(pct, sizeof(pct), "%.0f%% lit", f * 100.0);
        row(pct, dim);
        row(String(waxing ? "waxing" : "waning"), faint);
        y += 6;
        row("Rise  " + LocalHM(today.moonrise), today.moonrise ? fg : faint);
        row("High  " + LocalHM(today.moonTransit), today.moonTransit ? fg : faint);
        row("Set   " + LocalHM(today.moonset), today.moonset ? fg : faint);

    } else { // Sun
        c.drawRoundRect(m, m, SCREEN_SIZE - 2 * m, SCREEN_SIZE - 2 * m, 16, accent);
        c.setTextSize(2);
        row("SUN", accent); y += 6;
        row("Rise  " + LocalHM(today.sunrise), today.sunrise ? fg : faint);
        if (today.sunrise && today.sunset) {
            const time_t noon = today.sunrise + (today.sunset - today.sunrise) / 2;
            row("Noon  " + LocalHM(noon), fg);
        }
        row("Set   " + LocalHM(today.sunset), today.sunset ? fg : faint);
        if (today.sunrise && today.sunset && today.sunset > today.sunrise) {
            const long dl = today.sunset - today.sunrise;
            char b[24]; snprintf(b, sizeof(b), "%ldh %02ldm daylight", dl / 3600, (dl % 3600) / 60);
            row(b, dim);
            row("Golden " + LocalHM(today.sunrise) + " & " + LocalHM(today.sunset - 3000), faint);
        }
    }

    c.setTextSize(1);
    CenterText(c, "tap to close", SCREEN_SIZE - 40, faint);
}

// --------------------------------------------------------------------------------- chrome
void AnglerManager::DrawScreenDots(BandCanvas& c, const std::vector<Screen>& rot) const
{
    const int n = (int)rot.size();
    if (n <= 1) return;
    int activeIdx = 0;
    for (int i = 0; i < n; ++i) if (rot[i] == current) { activeIdx = i; break; }

    const int gap = 9;
    const int totalW = (n - 1) * gap;
    int x = SCREEN_SIZE_DIV_2 - totalW / 2;
    const int y = SCREEN_SIZE - 12;
    for (int i = 0; i < n; ++i) {
        c.fillCircle(x, y, i == activeIdx ? 2 : 1, i == activeIdx ? palette.fg : palette.faint);
        x += gap;
    }
}
