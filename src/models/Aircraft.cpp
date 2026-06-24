#include "models/Aircraft.h"

#include <cstring>

namespace {

// Unit conversions from the dump1090/readsb feed into OpenSky's units.
constexpr float FEET_TO_METRES   = 0.3048f;
constexpr float KNOTS_TO_MS      = 0.514444f;
constexpr float FTMIN_TO_MS      = 0.00508f; // feet/minute -> metres/second

// dump1090 reports the ADS-B emitter category as a two-character set+number code
// ("A1".."C7"); map it onto OpenSky's numeric category so the existing display
// table (AircraftInfoFields) needs no special-casing. 0 = unknown/no info.
int LocalCategoryToOpenSky(const char* cat) {
    if (cat == nullptr || cat[0] == '\0' || cat[1] == '\0')
        return 0;
    const char set = cat[0];
    const int n = cat[1] - '0';
    if (set == 'A' && n >= 1 && n <= 7)
        return n + 1; // A1->2 (Light) .. A7->8 (Rotorcraft)
    if (set == 'B') {
        switch (n) {
            case 1: return 9;  // Glider
            case 2: return 10; // Lighter-than-air
            case 3: return 11; // Parachutist
            case 4: return 12; // Ultralight
            case 6: return 14; // UAV
            case 7: return 15; // Space
        }
    } else if (set == 'C') {
        switch (n) {
            case 1: return 16; // Surface emergency vehicle
            case 2: return 17; // Surface service vehicle
            case 3: return 18; // Point obstacle
            case 4: return 19;
            case 5: return 20;
        }
    }
    return 0;
}

} // namespace

namespace JsonParser {
    template<>
    Aircraft Parse<Aircraft>(const JsonVariant& state) {
        Aircraft a;

        a.icao24 = state[0].isNull() ? "" : state[0].as<String>();
        a.callsign = state[1].isNull() ? "" : state[1].as<String>();
        a.originCountry = state[2].isNull() ? "" : state[2].as<String>();
        a.timePosition = state[3].isNull() ? 0 : state[3].as<long>();
        a.lastContact = state[4].isNull() ? 0 : state[4].as<long>();
        a.longitude = state[5].isNull() ? 0.0f : state[5].as<float>();
        a.latitude = state[6].isNull() ? 0.0f : state[6].as<float>();
        a.baroAltitude = state[7].isNull() ? 0.0f : state[7].as<float>();
        a.onGround = state[8].as<bool>();
        a.velocity = state[9].isNull() ? 0.0f : state[9].as<float>();
        a.trueTrack = state[10].isNull() ? 0.0f : state[10].as<float>();
        a.verticalRate = state[11].isNull() ? 0.0f : state[11].as<float>();
        // state[12] = sensors, skipped
        a.geoAltitude = state[13].isNull() ? 0.0f : state[13].as<float>();
        a.squawk = state[14].isNull() ? "" : state[14].as<String>();
        a.spi = state[15].isNull() ? false : state[15].as<bool>();
        a.positionSource = state[16].isNull() ? 0 : state[16].as<int>();
        a.category = state[17].isNull() ? 0 : state[17].as<int>();

        return a;
    }

    bool ParseLocalAircraft(JsonVariantConst e, Aircraft& a) {
        // No position yet (aircraft seen by Mode-S but not ADS-B position) -- skip,
        // so it isn't plotted at (0, 0).
        if (e["lat"].isNull() || e["lon"].isNull())
            return false;

        a.latitude = e["lat"].as<float>();
        a.longitude = e["lon"].as<float>();

        // "hex": 24-bit ICAO address. Non-ICAO addresses (TIS-B / anonymised) are
        // prefixed with '~'; drop it so adsbdb lookups and watchlist matches use the
        // bare address, matching OpenSky's lowercase hex.
        String hex = e["hex"].isNull() ? "" : e["hex"].as<String>();
        if (hex.startsWith("~")) hex = hex.substring(1);
        hex.toLowerCase();
        a.icao24 = hex;

        a.callsign = e["flight"].isNull() ? "" : e["flight"].as<String>(); // trailing pad trimmed downstream

        // alt_baro is feet, or the literal string "ground" on the surface
        JsonVariantConst altBaro = e["alt_baro"];
        if (altBaro.is<const char*>()) { // "ground"
            a.onGround = true;
            a.baroAltitude = 0.0f;
        } else {
            a.onGround = false;
            a.baroAltitude = altBaro.isNull() ? 0.0f : altBaro.as<float>() * FEET_TO_METRES;
        }
        a.geoAltitude = e["alt_geom"].isNull() ? 0.0f : e["alt_geom"].as<float>() * FEET_TO_METRES;

        a.velocity = e["gs"].isNull() ? 0.0f : e["gs"].as<float>() * KNOTS_TO_MS;

        // heading: prefer the true ground track, fall back to true/magnetic heading
        if (!e["track"].isNull())             a.trueTrack = e["track"].as<float>();
        else if (!e["true_heading"].isNull()) a.trueTrack = e["true_heading"].as<float>();
        else if (!e["mag_heading"].isNull())  a.trueTrack = e["mag_heading"].as<float>();
        else                                  a.trueTrack = 0.0f;

        // vertical rate: barometric preferred, else geometric (both ft/min)
        if (!e["baro_rate"].isNull())      a.verticalRate = e["baro_rate"].as<float>() * FTMIN_TO_MS;
        else if (!e["geom_rate"].isNull()) a.verticalRate = e["geom_rate"].as<float>() * FTMIN_TO_MS;
        else                               a.verticalRate = 0.0f;

        a.squawk = e["squawk"].isNull() ? "" : e["squawk"].as<String>();
        a.category = LocalCategoryToOpenSky(e["category"].as<const char*>());

        // "type" describes how the position was derived; surface the MLAT case so
        // the optional Position-source line is meaningful, otherwise treat as ADS-B.
        const char* type = e["type"].as<const char*>();
        a.positionSource = (type != nullptr && strncmp(type, "mlat", 4) == 0) ? 2 : 0;

        // The local feed is real-time, so there's no server-side staleness to
        // dead-reckon away: leave the position-age timestamps at 0 and let the
        // predictor extrapolate from local elapsed time only.
        a.timePosition = 0;
        a.lastContact = 0;
        a.originCountry = ""; // not provided by the local feed
        a.spi = false;

        return true;
    }

}