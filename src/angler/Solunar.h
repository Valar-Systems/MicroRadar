#pragma once

#include <Arduino.h>
#include <time.h>

// Solunar theory ("best bite times") for the Angler edition, computed ENTIRELY on-device from the
// shared sun/moon ephemeris in src/astro -- no network, works offline and worldwide. This is the
// edition's hero data.
//
// Definitions (validated against USNO + a published solunar table for NYC 2026-06-01, see the
// self-check in AnglerManager::RecomputeSolunar):
//   MAJOR periods -- 2 hours long, starting at the Moon's meridian transits: upper transit
//                    (overhead, hour-angle 0) and lower transit (underfoot, hour-angle 180).
//   MINOR periods -- 1 hour long, starting at moonrise and at moonset (altitude zero-crossings).
// (Windows START at the event -- matching the published solunar table this was validated against;
//  some calculators instead centre the window on the event. `center` holds the driving event.)
//   DAY rating    -- driven by Moon phase: strongest near new/full, weakest at the quarters,
//                    bumped one tier when a period coincides with sunrise/sunset (the "green
//                    window"). Read from the Moon's illuminated fraction at local noon.
//
// A lunar day is ~24h50m, so a calendar day holds 0, 1 or 2 of any given event -- never assume
// exactly two majors + two minors.
namespace angler {

enum class PeriodKind : uint8_t { MajorOverhead, MajorUnderfoot, MinorRise, MinorSet };
enum class Rating : uint8_t { Poor, Fair, Good, Excellent };

struct Period {
    time_t start = 0;   // UTC epoch, window open
    time_t end = 0;     // UTC epoch, window close
    time_t center = 0;  // the driving event (transit / rise / set), UTC epoch
    PeriodKind kind = PeriodKind::MajorOverhead;
    bool major() const { return kind == PeriodKind::MajorOverhead || kind == PeriodKind::MajorUnderfoot; }
};

struct SolunarDay {
    time_t dayStart = 0;        // UTC epoch of local midnight (the day this describes)
    long   tzOffsetSec = 0;     // local offset used to bound the day
    Period periods[4];          // up to 2 majors + 2 minors, sorted by start
    int    count = 0;
    Rating rating = Rating::Poor;
    double moonIllum = 0.0;     // 0=new .. 1=full, at local noon
    bool   sunBump = false;     // a period coincides with sunrise or sunset
    // day events within [dayStart, dayStart+24h); 0 = none that calendar day
    time_t sunrise = 0, sunset = 0;
    time_t moonrise = 0, moonset = 0, moonTransit = 0;
    bool   valid = false;
};

const char* PeriodLabel(PeriodKind k);   // "Overhead" / "Underfoot" / "Moonrise" / "Moonset"
const char* PeriodShort(PeriodKind k);   // "MAJOR" / "MINOR"
const char* RatingLabel(Rating r);       // "Poor" / "Fair" / "Good" / "Excellent"
int  RatingStars(Rating r);              // 1..4
const char* MoonPhaseName(double illum, bool waxing);

// Compute the solunar day for the local calendar day containing refUtc.
SolunarDay ComputeDay(time_t refUtc, double latDeg, double lonDeg, long tzOffsetSec);

} // namespace angler
