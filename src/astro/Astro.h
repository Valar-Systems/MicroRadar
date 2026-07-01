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

// The five naked-eye planets.
enum class Planet : uint8_t { Mercury, Venus, Mars, Jupiter, Saturn };
// Geocentric equatorial coordinates (deg) + apparent visual magnitude for a planet (Schlyter's
// low-precision theory, with the main Jupiter/Saturn perturbations). Good to a few arcminutes.
void PlanetRaDec(Planet p, time_t utc, double& raDeg, double& decDeg, double& magOut);
const char* PlanetName(Planet p);

// Heliocentric ecliptic position (deg lon/lat, AU) of a planet / of Earth -- for the orrery.
void PlanetHeliocentric(Planet p, time_t utc, double& lonDeg, double& latDeg, double& rAu);
void EarthHeliocentric(time_t utc, double& lonDeg, double& rAu);

// Moon optical libration (degrees; + longitude = eastern limb tipped toward us, + latitude = north)
// and the selenographic colongitude of the Sun (deg; ~270 new, ~0 first qtr, ~90 full, ~180 last) --
// the longitude of the sunrise terminator, i.e. "what's dramatically lit on the Moon right now".
void MoonLibration(time_t utc, double& libLonDeg, double& libLatDeg, double& colongDeg);

// Apparent positions of the four Galilean moons (Meeus low-accuracy). x = east-west offset from
// Jupiter in Jupiter-equatorial-radii (+ = west); front = true when the moon is in front of the
// disc (nearer Earth) rather than behind. out[0..3] = Io, Europa, Ganymede, Callisto.
struct GalileanMoon { float x; bool front; };
void JupiterMoons(time_t utc, GalileanMoon out[4]);
const char* GalileanName(int i);

// Equatorial -> horizontal for an observer. altDeg is elevation above the horizon (refraction
// ignored); azDeg is measured from North through East (0=N, 90=E, 180=S, 270=W). lon east-positive.
void AltAz(double raDeg, double decDeg, double latDeg, double lonDeg, time_t utc,
           double& altDeg, double& azDeg);

double SunAltDeg(time_t utc, double latDeg, double lonDeg);   // convenience: Sun elevation
double MoonAltDeg(time_t utc, double latDeg, double lonDeg);  // convenience: Moon elevation

} } // namespace space::astro
