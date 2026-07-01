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

// The Sun in Schlyter's terms. Returns true ecliptic longitude (deg) and hands back the mean
// anomaly + mean longitude (for the Moon's perturbations) and the Earth-Sun distance (for planets).
double SunLongitude(double d, double& Ms_out, double& Ls_out, double& rs_out)
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
    rs_out = sqrt(xv * xv + yv * yv);              // Earth-Sun distance (AU)
    return norm360(v + w);                          // true ecliptic longitude
}

double Obliquity(double d) { return 23.4393 - 3.563e-7 * d; }

// Schlyter primary orbital elements for the five naked-eye planets (angles deg, a in AU; the *d
// terms are per-day rates). Order matches space::astro::Planet.
struct OrbEl { double N0, Nd, i0, id, w0, wd, a, e0, ed, M0, Md; };
const OrbEl PLANETS[5] = {
    {  48.3313, 3.24587e-5,  7.0047,  5.00e-8,  29.1241, 1.01444e-5, 0.387098, 0.205635,  5.59e-10, 168.6562, 4.0923344368 }, // Mercury
    {  76.6799, 2.46590e-5,  3.3946,  2.75e-8,  54.8910, 1.38374e-5, 0.723330, 0.006773, -1.302e-9,  48.0052, 1.6021302244 }, // Venus
    {  49.5574, 2.11081e-5,  1.8497, -1.78e-8, 286.5016, 2.92961e-5, 1.523688, 0.093405,  2.516e-9,  18.6021, 0.5240207766 }, // Mars
    { 100.4542, 2.76854e-5,  1.3030, -1.557e-7,273.8777, 1.64505e-5, 5.202560, 0.048498,  4.469e-9,  19.8950, 0.0830853001 }, // Jupiter
    { 113.6634, 2.38980e-5,  2.4886, -1.081e-7,339.3939, 2.97661e-5, 9.554750, 0.055546, -9.499e-9, 316.9670, 0.0334442282 }, // Saturn
};

// Heliocentric ecliptic longitude/latitude (deg) + distance (AU) of a planet, with the main
// Jupiter/Saturn perturbations. Shared by the geocentric RA/Dec path and the orrery.
void planetHelio(int idx, double d, double& lonecl, double& latecl, double& r)
{
    const OrbEl& el = PLANETS[idx];
    const double N = el.N0 + el.Nd * d;
    const double i = el.i0 + el.id * d;
    const double w = el.w0 + el.wd * d;
    const double a = el.a;
    const double e = el.e0 + el.ed * d;
    const double M = norm360(el.M0 + el.Md * d);

    double E = M + R2D * e * sind(M) * (1.0 + e * cosd(M));
    for (int it = 0; it < 6; ++it) E = E - (E - R2D * e * sind(E) - M) / (1.0 - e * cosd(E));

    const double xv = a * (cosd(E) - e);
    const double yv = a * (sqrt(1.0 - e * e) * sind(E));
    const double v = atan2d(yv, xv);
    r = sqrt(xv * xv + yv * yv);

    const double xh = r * (cosd(N) * cosd(v + w) - sind(N) * sind(v + w) * cosd(i));
    const double yh = r * (sind(N) * cosd(v + w) + cosd(N) * sind(v + w) * cosd(i));
    const double zh = r * (sind(v + w) * sind(i));
    lonecl = norm360(atan2d(yh, xh));
    latecl = atan2d(zh, sqrt(xh * xh + yh * yh));

    if (idx == 3 || idx == 4) { // Jupiter / Saturn main perturbations
        const double Mj = norm360(PLANETS[3].M0 + PLANETS[3].Md * d);
        const double Msa = norm360(PLANETS[4].M0 + PLANETS[4].Md * d);
        if (idx == 3) {
            lonecl += -0.332 * sind(2 * Mj - 5 * Msa - 67.6);
            lonecl += -0.056 * sind(2 * Mj - 2 * Msa + 21);
            lonecl += +0.042 * sind(3 * Mj - 5 * Msa + 21);
            lonecl += -0.036 * sind(Mj - 2 * Msa);
            lonecl += +0.022 * cosd(Mj - Msa);
            lonecl += +0.023 * sind(2 * Mj - 3 * Msa + 52);
            lonecl += -0.016 * sind(Mj - 5 * Msa - 69);
        } else {
            lonecl += +0.812 * sind(2 * Mj - 5 * Msa - 67.6);
            lonecl += -0.229 * cosd(2 * Mj - 4 * Msa - 2);
            lonecl += +0.119 * sind(Mj - 2 * Msa - 3);
            lonecl += +0.046 * sind(2 * Mj - 6 * Msa - 69);
            lonecl += +0.014 * sind(Mj - 3 * Msa + 32);
            latecl  += -0.020 * cosd(2 * Mj - 4 * Msa - 2);
            latecl  += +0.018 * sind(2 * Mj - 6 * Msa - 49);
        }
        lonecl = norm360(lonecl);
    }
}

// Moon geocentric ecliptic longitude/latitude (deg), ascending node, argument of latitude, the
// Sun's longitude, and the illuminated fraction -- the shared guts of MoonRaDec + MoonLibration.
void moonEcl(double d, double& lon, double& lat, double& node, double& Fout, double& sunLon, double& illum)
{
    node = norm360(125.1228 - 0.0529538083 * d);
    const double i = 5.1454;
    const double w = norm360(318.0634 + 0.1643573223 * d);
    const double a = 60.2666;
    const double e = 0.054900;
    const double M = norm360(115.3654 + 13.0649929509 * d);

    double E = M + R2D * e * sind(M) * (1.0 + e * cosd(M));
    for (int it = 0; it < 2; ++it) E = E - (E - R2D * e * sind(E) - M) / (1.0 - e * cosd(E));

    const double xv = a * (cosd(E) - e);
    const double yv = a * (sqrt(1.0 - e * e) * sind(E));
    const double v = atan2d(yv, xv);
    const double r = sqrt(xv * xv + yv * yv);

    const double xh = r * (cosd(node) * cosd(v + w) - sind(node) * sind(v + w) * cosd(i));
    const double yh = r * (sind(node) * cosd(v + w) + cosd(node) * sind(v + w) * cosd(i));
    const double zh = r * (sind(v + w) * sind(i));
    lon = norm360(atan2d(yh, xh));
    lat = atan2d(zh, sqrt(xh * xh + yh * yh));

    double Ms, Ls, rs;
    sunLon = SunLongitude(d, Ms, Ls, rs);
    const double Lm = norm360(node + w + M);
    const double Dm = Lm - Ls;
    Fout = Lm - node;

    lon += -1.274 * sind(M - 2 * Dm);
    lon += +0.658 * sind(2 * Dm);
    lon += -0.186 * sind(Ms);
    lon += -0.059 * sind(2 * M - 2 * Dm);
    lon += -0.057 * sind(M - 2 * Dm + Ms);
    lon += +0.053 * sind(M + 2 * Dm);
    lon += +0.046 * sind(2 * Dm - Ms);
    lon += +0.041 * sind(M - Ms);
    lon += -0.035 * sind(Dm);
    lon += -0.031 * sind(M + Ms);
    lon += -0.015 * sind(2 * Fout - 2 * Dm);
    lon += +0.011 * sind(M - 4 * Dm);
    lat += -0.173 * sind(Fout - 2 * Dm);
    lat += -0.055 * sind(M - Fout - 2 * Dm);
    lat += -0.046 * sind(M + Fout - 2 * Dm);
    lat += +0.033 * sind(Fout + 2 * Dm);
    lat += +0.017 * sind(2 * M + Fout);
    lon = norm360(lon);

    illum = (1.0 - cosd(lon - sunLon)) * 0.5;
}

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
    double Ms, Ls, rs;
    const double lon = SunLongitude(d, Ms, Ls, rs);
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
    double lon, lat, node, F, sunLon;
    moonEcl(d, lon, lat, node, F, sunLon, illumFrac);

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

void MoonLibration(time_t utc, double& libLonDeg, double& libLatDeg, double& colongDeg)
{
    const double d = DayNumber(utc);
    double lon, lat, node, F, sunLon, illum;
    moonEcl(d, lon, lat, node, F, sunLon, illum);

    constexpr double I = 1.54242; // inclination of the lunar equator to the ecliptic

    // Optical libration (Meeus 53.1): tilt of the Moon's mean sub-Earth point.
    const double W = lon - node;
    const double A = atan2d(sind(W) * cosd(lat) * cosd(I) - sind(lat) * sind(I), cosd(W) * cosd(lat));
    libLonDeg = fmod(A - F + 540.0, 360.0) - 180.0;            // -180..180
    libLatDeg = asind(-sind(W) * cosd(lat) * sind(I) - sind(lat) * cosd(I));

    // Selenographic colongitude of the Sun: same geometry for the sub-solar point (Sun lat ~0),
    // expressed as the longitude of the morning terminator.
    const double W0 = sunLon - node;
    const double A0 = atan2d(sind(W0) * cosd(I), cosd(W0));
    const double l0 = A0 - F;                                   // sub-solar selenographic longitude
    colongDeg = fmod(270.0 - l0, 360.0);                        // -> standard colongitude (verified vs pyephem)
    if (colongDeg < 0) colongDeg += 360.0;
}

const char* PlanetName(Planet p)
{
    switch (p) {
        case Planet::Mercury: return "Mercury";
        case Planet::Venus:   return "Venus";
        case Planet::Mars:    return "Mars";
        case Planet::Jupiter: return "Jupiter";
        default:              return "Saturn";
    }
}

void PlanetRaDec(Planet p, time_t utc, double& raDeg, double& decDeg, double& magOut)
{
    const double d = DayNumber(utc);
    double lonecl, latecl, r;
    planetHelio((int)p, d, lonecl, latecl, r);

    // Heliocentric -> geocentric: add the Sun's geocentric rectangular position (z_sun = 0).
    const double xhc = r * cosd(lonecl) * cosd(latecl);
    const double yhc = r * sind(lonecl) * cosd(latecl);
    const double zhc = r * sind(latecl);
    double Ms_s, Ls_s, rs;
    const double sunLon = SunLongitude(d, Ms_s, Ls_s, rs);
    const double xs = rs * cosd(sunLon), ys = rs * sind(sunLon);
    const double xg = xhc + xs, yg = yhc + ys, zg = zhc;

    // Ecliptic -> equatorial.
    const double ecl = Obliquity(d);
    const double xe = xg;
    const double ye = yg * cosd(ecl) - zg * sind(ecl);
    const double ze = yg * sind(ecl) + zg * cosd(ecl);
    raDeg = norm360(atan2d(ye, xe));
    decDeg = atan2d(ze, sqrt(xe * xe + ye * ye));

    // Apparent magnitude (Schlyter). FV = phase angle (Sun-planet-Earth) from the distance triangle.
    const double R = sqrt(xg * xg + yg * yg + zg * zg); // geocentric distance (AU)
    double cosFV = (r * r + R * R - rs * rs) / (2.0 * r * R);
    if (cosFV > 1) cosFV = 1;
    if (cosFV < -1) cosFV = -1;
    const double FV = acos(cosFV) * R2D;
    const double logrR = 5.0 * log10(r * R);
    switch (p) {
        case Planet::Mercury: magOut = -0.36 + logrR + 0.027 * FV + 2.2e-13 * pow(FV, 6); break;
        case Planet::Venus:   magOut = -4.34 + logrR + 0.013 * FV + 4.2e-7 * pow(FV, 3); break;
        case Planet::Mars:    magOut = -1.51 + logrR + 0.016 * FV; break;
        case Planet::Jupiter: magOut = -9.25 + logrR + 0.014 * FV; break;
        default:              magOut = -9.00 + logrR + 0.044 * FV; break; // Saturn (ring term omitted)
    }
}

void PlanetHeliocentric(Planet p, time_t utc, double& lonDeg, double& latDeg, double& rAu)
{
    planetHelio((int)p, DayNumber(utc), lonDeg, latDeg, rAu);
}

void EarthHeliocentric(time_t utc, double& lonDeg, double& rAu)
{
    double Ms, Ls, rs;
    const double sunLon = SunLongitude(DayNumber(utc), Ms, Ls, rs);
    lonDeg = norm360(sunLon + 180.0); // Earth sits opposite the Sun as seen from the Sun
    rAu = rs;
}

const char* GalileanName(int i)
{
    static const char* N[4] = {"Io", "Europa", "Ganymede", "Callisto"};
    return (i >= 0 && i < 4) ? N[i] : "";
}

void JupiterMoons(time_t utc, GalileanMoon out[4])
{
    // Meeus "low accuracy" Galilean theory (Astronomical Algorithms ch. 44).
    const double d = JulianDate(utc) - 2451545.0;
    const double V = 172.74 + 0.00111588 * d;
    const double M = 357.529 + 0.9856003 * d;
    const double sN = 20.020 + 0.0830853 * d + 0.329 * sind(V);
    const double J = 66.115 + 0.9025179 * d - 0.329 * sind(V);
    const double A = 1.915 * sind(M) + 0.020 * sind(2 * M);
    const double B = 5.555 * sind(sN) + 0.168 * sind(2 * sN);
    const double K = J + A - B;
    const double Re = 1.00014 - 0.01671 * cosd(M) - 0.00014 * cosd(2 * M);
    const double rj = 5.20872 - 0.25208 * cosd(sN) - 0.00611 * cosd(2 * sN);
    const double Delta = sqrt(rj * rj + Re * Re - 2 * rj * Re * cosd(K));
    const double psi = asind(Re / Delta * sind(K));
    const double base = d - Delta / 173.0; // correct for light-travel time

    double u[4] = {
        norm360(163.8067 + 203.4058643 * base + psi - B),
        norm360(358.4108 + 101.2916334 * base + psi - B),
        norm360(  5.7129 +  50.2345179 * base + psi - B),
        norm360(224.8151 +  21.4879801 * base + psi - B),
    };
    const double G = 331.18 + 50.310482 * base;
    const double H = 87.40 + 21.569231 * base;
    const double uc[4] = {
        u[0] + 0.473 * sind(2 * (u[0] - u[1])),
        u[1] + 1.065 * sind(2 * (u[1] - u[2])),
        u[2] + 0.165 * sind(G),
        u[3] + 0.843 * sind(H),
    };
    const double rad[4] = {
        5.9057  - 0.0244 * cosd(2 * (u[0] - u[1])),
        9.3966  - 0.0882 * cosd(2 * (u[1] - u[2])),
        14.9883 - 0.0216 * cosd(G),
        26.3627 - 0.1939 * cosd(H),
    };
    for (int i = 0; i < 4; ++i) {
        out[i].x = (float)(-rad[i] * sind(uc[i]));  // east-west offset, Jupiter radii (+ = west; matches pyephem)
        out[i].front = (cosd(uc[i]) < 0.0);          // nearer-Earth half of the orbit (in front of the disc)
    }
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
