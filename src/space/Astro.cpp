#include "Astro.h"

#include <math.h>

// Low-precision solar + lunar ephemeris and coordinate transforms. Sun follows the standard
// "low accuracy" formulae; the Moon follows Paul Schlyter's compact lunar theory
// (https://stjarnhimlen.se/comp/ppcomp.html) with its main longitude/latitude perturbation terms.
// All internal angle work is in degrees; sind/cosd wrap the radian trig.

namespace {

constexpr double D2R = M_PI / 180.0;
constexpr double R2D = 180.0 / M_PI;

inline double sind(double d) { return sin(d * D2R); }
inline double cosd(double d) { return cos(d * D2R); }
inline double atan2d(double y, double x) { return atan2(y, x) * R2D; }
inline double asind(double x) { if (x > 1) x = 1; if (x < -1) x = -1; return asin(x) * R2D; }

// Normalize degrees to 0..360.
double norm360(double a)
{
    a = fmod(a, 360.0);
    if (a < 0) a += 360.0;
    return a;
}

// Schlyter day number: days (and fraction) since 2000 Jan 0.0 UT = JD 2451543.5.
double DayNumber(time_t utc) { return (double)utc / 86400.0 + 2440587.5 - 2451543.5; }

// The Sun in Schlyter's terms. Returns true ecliptic longitude (deg) and distance (AU), and hands
// back the mean anomaly + mean longitude the Moon's perturbations need.
double SunLongitude(double d, double& Ms_out, double& Ls_out)
{
    const double w = 282.9404 + 4.70935e-5 * d;   // argument of perihelion
    const double e = 0.016709 - 1.151e-9 * d;     // eccentricity
    const double M = norm360(356.0470 + 0.9856002585 * d); // mean anomaly
    Ms_out = M;
    Ls_out = norm360(w + M);                       // mean longitude

    // Eccentric anomaly (one Newton step is plenty at this eccentricity).
    double E = M + R2D * e * sind(M) * (1.0 + e * cosd(M));
    const double xv = cosd(E) - e;
    const double yv = sqrt(1.0 - e * e) * sind(E);
    const double v = atan2d(yv, xv);               // true anomaly
    return norm360(v + w);                          // true ecliptic longitude
}

double Obliquity(double d) { return 23.4393 - 3.563e-7 * d; }

} // namespace

namespace space { namespace astro {

double JulianDate(time_t utc) { return (double)utc / 86400.0 + 2440587.5; }

double GmstDeg(time_t utc)
{
    // GMST via the standard sidereal expression (matches the rest of Spacescope's clock screens).
    const double dJ2000 = JulianDate(utc) - 2451545.0;
    double h = fmod(18.697374558 + 24.06570982441908 * dJ2000, 24.0);
    if (h < 0) h += 24.0;
    return h * 15.0;
}

void SunRaDec(time_t utc, double& raDeg, double& decDeg)
{
    const double d = DayNumber(utc);
    double Ms, Ls;
    const double lon = SunLongitude(d, Ms, Ls);
    const double ecl = Obliquity(d);
    // Ecliptic (lat 0 for the Sun) -> equatorial.
    const double xs = cosd(lon), ys = sind(lon);
    const double xe = xs;
    const double ye = ys * cosd(ecl);
    const double ze = ys * sind(ecl);
    raDeg = norm360(atan2d(ye, xe));
    decDeg = atan2d(ze, sqrt(xe * xe + ye * ye));
}

void MoonRaDec(time_t utc, double& raDeg, double& decDeg, double& illumFrac)
{
    const double d = DayNumber(utc);

    // Moon mean orbital elements (deg / Earth-radii).
    const double N = norm360(125.1228 - 0.0529538083 * d);  // ascending node
    const double i = 5.1454;                                 // inclination
    const double w = norm360(318.0634 + 0.1643573223 * d);  // arg. of perigee
    const double a = 60.2666;                                // mean distance (Earth radii)
    const double e = 0.054900;                               // eccentricity
    const double M = norm360(115.3654 + 13.0649929509 * d);  // mean anomaly

    // Eccentric anomaly (iterate twice; lunar e is larger than the Sun's).
    double E = M + R2D * e * sind(M) * (1.0 + e * cosd(M));
    for (int it = 0; it < 2; ++it)
        E = E - (E - R2D * e * sind(E) - M) / (1.0 - e * cosd(E));

    // Position in the orbital plane.
    const double xv = a * (cosd(E) - e);
    const double yv = a * (sqrt(1.0 - e * e) * sind(E));
    const double v = atan2d(yv, xv);          // true anomaly
    const double r = sqrt(xv * xv + yv * yv); // distance (Earth radii)

    // Geocentric ecliptic rectangular -> longitude/latitude.
    const double xh = r * (cosd(N) * cosd(v + w) - sind(N) * sind(v + w) * cosd(i));
    const double yh = r * (sind(N) * cosd(v + w) + cosd(N) * sind(v + w) * cosd(i));
    const double zh = r * (sind(v + w) * sind(i));
    double lon = norm360(atan2d(yh, xh));
    double lat = atan2d(zh, sqrt(xh * xh + yh * yh));

    // Perturbations need the Sun's + Moon's mean longitudes and the mean elongation/arg-of-latitude.
    double Ms, Ls;
    const double sunLon = SunLongitude(d, Ms, Ls);
    const double Lm = norm360(N + w + M);   // Moon's mean longitude
    const double Dm = Lm - Ls;              // mean elongation
    const double F = Lm - N;                // argument of latitude

    // Main longitude perturbations (deg).
    lon += -1.274 * sind(M - 2 * Dm);          // evection
    lon += +0.658 * sind(2 * Dm);              // variation
    lon += -0.186 * sind(Ms);                  // yearly equation
    lon += -0.059 * sind(2 * M - 2 * Dm);
    lon += -0.057 * sind(M - 2 * Dm + Ms);
    lon += +0.053 * sind(M + 2 * Dm);
    lon += +0.046 * sind(2 * Dm - Ms);
    lon += +0.041 * sind(M - Ms);
    lon += -0.035 * sind(Dm);                  // parallactic equation
    lon += -0.031 * sind(M + Ms);
    lon += -0.015 * sind(2 * F - 2 * Dm);
    lon += +0.011 * sind(M - 4 * Dm);
    // Main latitude perturbations (deg).
    lat += -0.173 * sind(F - 2 * Dm);
    lat += -0.055 * sind(M - F - 2 * Dm);
    lat += -0.046 * sind(M + F - 2 * Dm);
    lat += +0.033 * sind(F + 2 * Dm);
    lat += +0.017 * sind(2 * M + F);

    lon = norm360(lon);

    // Illuminated fraction from the Sun-Moon elongation in ecliptic longitude.
    const double elong = lon - sunLon;
    illumFrac = (1.0 - cosd(elong)) * 0.5;

    // Ecliptic -> equatorial.
    const double ecl = Obliquity(d);
    const double xg = cosd(lon) * cosd(lat);
    const double yg = sind(lon) * cosd(lat);
    const double zg = sind(lat);
    const double xe = xg;
    const double ye = yg * cosd(ecl) - zg * sind(ecl);
    const double ze = yg * sind(ecl) + zg * cosd(ecl);
    raDeg = norm360(atan2d(ye, xe));
    decDeg = atan2d(ze, sqrt(xe * xe + ye * ye));
}

void AltAz(double raDeg, double decDeg, double latDeg, double lonDeg, time_t utc,
           double& altDeg, double& azDeg)
{
    const double H = GmstDeg(utc) + lonDeg - raDeg;  // local hour angle, deg (lon east-positive)
    const double sinAlt = sind(decDeg) * sind(latDeg) + cosd(decDeg) * cosd(latDeg) * cosd(H);
    altDeg = asind(sinAlt);
    double cosAz = (sind(decDeg) - sind(altDeg) * sind(latDeg)) / (cosd(altDeg) * cosd(latDeg));
    if (cosAz > 1) cosAz = 1;
    if (cosAz < -1) cosAz = -1;
    double A = acos(cosAz) * R2D;
    azDeg = (sind(H) > 0) ? (360.0 - A) : A;  // from North through East
}

double SunAltDeg(time_t utc, double latDeg, double lonDeg)
{
    double ra, dec, alt, az;
    SunRaDec(utc, ra, dec);
    AltAz(ra, dec, latDeg, lonDeg, utc, alt, az);
    return alt;
}

double MoonAltDeg(time_t utc, double latDeg, double lonDeg)
{
    double ra, dec, illum, alt, az;
    MoonRaDec(utc, ra, dec, illum);
    AltAz(ra, dec, latDeg, lonDeg, utc, alt, az);
    return alt;
}

} } // namespace space::astro
