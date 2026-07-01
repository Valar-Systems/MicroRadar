#include "Solunar.h"

#include <math.h>

#include "astro/Astro.h"

namespace angler {

namespace {

constexpr long MAJOR_LEN  = 7200;   // major window length (s) -> 2 h, starting at the transit
constexpr long MINOR_LEN  = 3600;   // minor window length (s), starting at the event
constexpr long STEP       = 600;    // 10-min sampling step for zero-crossing search
constexpr double SUN_HORIZON = -0.833;   // standard sunrise/sunset (refraction + solar semidiameter)

double wrap180(double a)
{
    a = fmod(a, 360.0);
    if (a <= -180.0) a += 360.0;
    if (a >  180.0)  a -= 360.0;
    return a;
}

// Moon local hour angle (deg, -180..180]. HA increases ~14.5 deg/h; it passes 0 at the upper
// transit (overhead) and +-180 at the lower transit (underfoot).
double moonHA(time_t t, double lon)
{
    double ra, dec, il;
    space::astro::MoonRaDec(t, ra, dec, il);
    return wrap180(space::astro::GmstDeg(t) + lon - ra);
}

// Rising zero-crossings (dir=+1) or falling (dir=-1) of f over [t0,t1], bisected to ~30 s.
// Up to maxN events appended to out. F is any callable time_t -> double.
template <typename F>
void findCrossings(F f, time_t t0, time_t t1, int dir, time_t* out, int& n, int maxN)
{
    double prev = f(t0);
    for (time_t t = t0 + STEP; t <= t1 && n < maxN; t += STEP) {
        const double cur = f(t);
        const bool cross = (dir > 0) ? (prev < 0 && cur >= 0) : (prev >= 0 && cur < 0);
        if (cross) {
            time_t a = t - STEP, b = t;             // crossing is bracketed in [a,b]
            for (int i = 0; i < 7; ++i) {
                const time_t m = a + (b - a) / 2;
                const double fm = f(m);
                if (dir > 0) { if (fm < 0) a = m; else b = m; }
                else         { if (fm >= 0) a = m; else b = m; }
            }
            out[n++] = a + (b - a) / 2;
        }
        prev = cur;
    }
}

} // namespace

const char* PeriodLabel(PeriodKind k)
{
    switch (k) {
        case PeriodKind::MajorOverhead:  return "Overhead";
        case PeriodKind::MajorUnderfoot: return "Underfoot";
        case PeriodKind::MinorRise:      return "Moonrise";
        case PeriodKind::MinorSet:       return "Moonset";
    }
    return "";
}

const char* PeriodShort(PeriodKind k)
{
    return (k == PeriodKind::MajorOverhead || k == PeriodKind::MajorUnderfoot) ? "MAJOR" : "MINOR";
}

const char* RatingLabel(Rating r)
{
    switch (r) {
        case Rating::Poor:      return "Poor";
        case Rating::Fair:      return "Fair";
        case Rating::Good:      return "Good";
        case Rating::Excellent: return "Excellent";
    }
    return "";
}

int RatingStars(Rating r) { return (int)r + 1; }   // Poor=1 .. Excellent=4

const char* MoonPhaseName(double illum, bool waxing)
{
    if (illum < 0.03) return "New Moon";
    if (illum > 0.97) return "Full Moon";
    if (illum > 0.47 && illum < 0.53) return waxing ? "First Quarter" : "Last Quarter";
    if (illum < 0.5) return waxing ? "Waxing Crescent" : "Waning Crescent";
    return waxing ? "Waxing Gibbous" : "Waning Gibbous";
}

SolunarDay ComputeDay(time_t refUtc, double lat, double lon, long tz)
{
    SolunarDay d;
    d.tzOffsetSec = tz;

    // Local midnight (as a UTC epoch) of the calendar day containing refUtc.
    const long localSec = (long)refUtc + tz;
    const time_t dayStart = (time_t)((localSec / 86400) * 86400 - tz);
    d.dayStart = dayStart;
    const time_t t0 = dayStart, t1 = dayStart + 86400;
    const time_t s0 = t0 - STEP, s1 = t1 + STEP;  // search a hair wide so edge events are caught

    time_t up[4];  int nu  = 0; findCrossings([&](time_t t){ return moonHA(t, lon); },              s0, s1, +1, up, nu, 4);
    time_t lo[4];  int nl  = 0; findCrossings([&](time_t t){ return wrap180(moonHA(t, lon) - 180.0); }, s0, s1, +1, lo, nl, 4);
    time_t mr[4];  int nmr = 0; findCrossings([&](time_t t){ return space::astro::MoonAltDeg(t, lat, lon); }, s0, s1, +1, mr, nmr, 4);
    time_t ms[4];  int nms = 0; findCrossings([&](time_t t){ return space::astro::MoonAltDeg(t, lat, lon); }, s0, s1, -1, ms, nms, 4);
    time_t sr[4];  int nsr = 0; findCrossings([&](time_t t){ return space::astro::SunAltDeg(t, lat, lon) - SUN_HORIZON; }, s0, s1, +1, sr, nsr, 4);
    time_t ss[4];  int nss = 0; findCrossings([&](time_t t){ return space::astro::SunAltDeg(t, lat, lon) - SUN_HORIZON; }, s0, s1, -1, ss, nss, 4);

    auto inDay   = [&](time_t t) { return t >= t0 && t < t1; };
    auto firstIn = [&](const time_t* a, int n) -> time_t { for (int i = 0; i < n; ++i) if (inDay(a[i])) return a[i]; return 0; };

    d.moonTransit = firstIn(up, nu);
    d.moonrise    = firstIn(mr, nmr);
    d.moonset     = firstIn(ms, nms);
    d.sunrise     = firstIn(sr, nsr);
    d.sunset      = firstIn(ss, nss);

    auto addMajor = [&](const time_t* a, int n, PeriodKind k) {
        for (int i = 0; i < n && d.count < 4; ++i)
            if (inDay(a[i])) d.periods[d.count++] = { a[i], a[i] + MAJOR_LEN, a[i], k };
    };
    auto addMinor = [&](const time_t* a, int n, PeriodKind k) {
        for (int i = 0; i < n && d.count < 4; ++i)
            if (inDay(a[i])) d.periods[d.count++] = { a[i], a[i] + MINOR_LEN, a[i], k };
    };
    addMajor(up, nu, PeriodKind::MajorOverhead);
    addMajor(lo, nl, PeriodKind::MajorUnderfoot);
    addMinor(mr, nmr, PeriodKind::MinorRise);
    addMinor(ms, nms, PeriodKind::MinorSet);

    // sort periods by start (insertion; <= 4 items)
    for (int i = 1; i < d.count; ++i) {
        Period key = d.periods[i];
        int j = i - 1;
        while (j >= 0 && d.periods[j].start > key.start) { d.periods[j + 1] = d.periods[j]; --j; }
        d.periods[j + 1] = key;
    }

    // Day rating from the Moon's illuminated fraction at local noon (dayStart + 12 h of real time).
    double ra, dec, il;
    space::astro::MoonRaDec(dayStart + 43200, ra, dec, il);
    d.moonIllum = il;
    const double strength = fabs(il - 0.5) / 0.5;   // 1 at new/full, 0 at the quarters
    Rating base = strength >= 0.85 ? Rating::Excellent
                : strength >= 0.55 ? Rating::Good
                : strength >= 0.25 ? Rating::Fair : Rating::Poor;

    bool bump = false;
    for (int i = 0; i < d.count; ++i) {
        if (d.sunrise && d.periods[i].start <= d.sunrise && d.sunrise <= d.periods[i].end) bump = true;
        if (d.sunset  && d.periods[i].start <= d.sunset  && d.sunset  <= d.periods[i].end) bump = true;
    }
    d.sunBump = bump;
    if (bump && base != Rating::Excellent) base = (Rating)((int)base + 1);
    d.rating = base;

    d.valid = true;
    return d;
}

} // namespace angler
