#pragma once

#include <Arduino.h>
#include <time.h>

// Shared on-device astronomy core for Spacescope: low-precision but reliable solar/lunar position,
// sidereal time, and horizontal-coordinate (alt/az) conversion. One engine for every sky screen
// (the observing window first; planet dial, variable-star, deep-sky-of-the-night, etc. later) so the
// celestial mechanics live in exactly one place instead of being re-derived per screen.
//
// Accuracy is "naked-eye good": Sun ~0.01 deg, Moon ~0.1-0.2 deg (Schlyter's low-precision lunar
// theory with the main perturbation terms), alt/az to a fraction of a degree (atmospheric refraction
// ignored). Ample for "is it up, how bright, when does it get dark". Everything takes a UTC unix
// epoch and works in degrees. Results are only as good as the device clock (NTP) and the lat/lon.
namespace space { namespace astro {

double JulianDate(time_t utc);          // Julian Date (days) from a UTC unix epoch
double GmstDeg(time_t utc);             // Greenwich mean sidereal time, degrees 0..360

// Apparent geocentric equatorial coordinates, degrees. raDeg in 0..360, decDeg in -90..90.
void SunRaDec(time_t utc, double& raDeg, double& decDeg);
// Moon equatorial coordinates + illuminated fraction (0=new .. 1=full). Low-precision (Schlyter).
void MoonRaDec(time_t utc, double& raDeg, double& decDeg, double& illumFrac);

// Equatorial -> horizontal for an observer. altDeg is elevation above the horizon (refraction
// ignored); azDeg is measured from North through East (0=N, 90=E, 180=S, 270=W). lon east-positive.
void AltAz(double raDeg, double decDeg, double latDeg, double lonDeg, time_t utc,
           double& altDeg, double& azDeg);

double SunAltDeg(time_t utc, double latDeg, double lonDeg);   // convenience: Sun elevation
double MoonAltDeg(time_t utc, double latDeg, double lonDeg);  // convenience: Moon elevation

} } // namespace space::astro
