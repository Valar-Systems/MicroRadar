#include "AircraftInfoFields.h"

#include <cmath>

namespace {

String trimmed(const String& s) { String c = s; c.trim(); return c; }

// Display units. The feeds are normalised to OpenSky's SI internally (metres,
// m/s); aviation/US convention shows altitude in feet, ground speed in knots,
// and vertical rate in feet per minute, so convert here at the display layer.
constexpr float METRES_TO_FEET = 3.28084f;
constexpr float MS_TO_KNOTS    = 1.94384f;
constexpr float MS_TO_FTMIN    = 196.850f; // 60 / 0.3048

// --- fields straight from the OpenSky /states/all feed ---
String fmtCallsign(const TrackedAircraft& t) { return trimmed(t.state.callsign); }
String fmtIcao(const TrackedAircraft& t)     { return t.state.icao24; }
String fmtCountry(const TrackedAircraft& t)  { return t.state.originCountry; }
String fmtSquawk(const TrackedAircraft& t)   { return t.state.squawk; }

String fmtSpeed(const TrackedAircraft& t)    { return String(lroundf(t.state.velocity * MS_TO_KNOTS)) + " kt"; }
String fmtBaroAlt(const TrackedAircraft& t)  { return String(lroundf(t.state.baroAltitude * METRES_TO_FEET)) + " ft"; }
String fmtGeoAlt(const TrackedAircraft& t)   { return String(lroundf(t.state.geoAltitude * METRES_TO_FEET)) + " ft"; }
String fmtHeading(const TrackedAircraft& t)  { return String(lroundf(t.state.trueTrack)) + " deg"; }

String fmtVerticalRate(const TrackedAircraft& t) {
    const long r = lroundf(t.state.verticalRate * MS_TO_FTMIN);
    String s;
    if (r > 0) s = "+";          // mark climbs; descents already carry the minus sign
    s += String(r);
    s += " ft/min";
    return s;
}

// OpenSky aircraft category codes (state vector index 17). Codes 0/1/13 carry no
// useful information, so they return "" and the line is skipped.
String fmtCategory(const TrackedAircraft& t) {
    switch (t.state.category) {
        case 2:  return "Light";
        case 3:  return "Small";
        case 4:  return "Large";
        case 5:  return "Heavy (HV)";
        case 6:  return "Heavy";
        case 7:  return "High perf";
        case 8:  return "Rotorcraft";
        case 9:  return "Glider";
        case 10: return "Lighter-air";
        case 11: return "Skydiver";
        case 12: return "Ultralight";
        case 14: return "UAV";
        case 15: return "Space";
        case 16: return "Emrg veh";
        case 17: return "Svc veh";
        case 18:
        case 19:
        case 20: return "Obstacle";
        default: return "";
    }
}

// OpenSky position source (state vector index 16).
String fmtPosSource(const TrackedAircraft& t) {
    switch (t.state.positionSource) {
        case 0:  return "ADS-B";
        case 1:  return "ASTERIX";
        case 2:  return "MLAT";
        case 3:  return "FLARM";
        default: return "";
    }
}

// --- fields enriched per-aircraft from adsbdb.com (see AircraftManager) ---
// These stay blank until the lookup for that aircraft completes, and may stay
// blank permanently for aircraft adsbdb doesn't know.
String fmtType(const TrackedAircraft& t)     { return t.typeCode; }
String fmtOperator(const TrackedAircraft& t) { return t.operatorName; }
String fmtReg(const TrackedAircraft& t)      { return t.registration; }

} // namespace

// Display order matches this table's order. The three OpenSky fields defaulting
// to on (callsign, ground speed, barometric altitude) reproduce the original
// fixed layout so existing devices look unchanged until the user opts into more.
const AircraftInfoFieldDef AIRCRAFT_INFO_FIELDS[] = {
    { "info-callsign", "Callsign",        true,  false, fmtCallsign },
    { "info-type",     "Aircraft type",   false, true,  fmtType },
    { "info-operator", "Operator",        false, true,  fmtOperator },
    { "info-reg",      "Registration",    false, true,  fmtReg },
    { "info-icao",     "ICAO address",    false, false, fmtIcao },
    { "info-country",  "Origin country",  false, false, fmtCountry },
    { "info-speed",    "Ground speed",    true,  false, fmtSpeed },
    { "info-vrate",    "Vertical rate",   false, false, fmtVerticalRate },
    { "info-baroalt",  "Barometric alt",  true,  false, fmtBaroAlt },
    { "info-geoalt",   "Geometric alt",   false, false, fmtGeoAlt },
    { "info-heading",  "Heading",         false, false, fmtHeading },
    { "info-squawk",   "Squawk",          false, false, fmtSquawk },
    { "info-category", "Category",        false, false, fmtCategory },
    { "info-possrc",   "Position source", false, false, fmtPosSource },
};

const size_t AIRCRAFT_INFO_FIELD_COUNT =
    sizeof(AIRCRAFT_INFO_FIELDS) / sizeof(AIRCRAFT_INFO_FIELDS[0]);
