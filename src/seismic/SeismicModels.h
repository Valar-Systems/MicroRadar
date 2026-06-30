#pragma once

#include <Arduino.h>
#include <vector>
#include <utility>
#include <ArduinoJson.h>

// Typed models for the Seismic edition's data feed plus the parser that builds them off the wire,
// and the request/result envelopes the background poller hands across the task boundary. Mirrors
// the Space/EAM models' shape (feed-agnostic structs + free-function parsers + a fetch envelope).
//
// The one and only source is the USGS Earthquake Hazards Program FDSN event query API, which serves
// GeoJSON and honours a `limit`, so responses stay bounded (unlike the bulk summary feeds, which can
// balloon during a swarm). Keyless. All fields are best-effort; the screens degrade gracefully when
// a poll hasn't landed or a field is missing.
namespace seismic {

// ------------------------------------------------------------------------------------ one quake
struct Quake {
    String id;                 // USGS event id (e.g. "us7000abcd"); the natural alert-dedupe key
    float mag = 0.0f;          // magnitude (USGS `mag`, may be null -> 0)
    String magType;            // e.g. "mb", "mww"
    String place;              // human description, e.g. "14 km SSW of ..."
    double lat = 0.0;          // degrees
    double lon = 0.0;          // degrees
    double depthKm = 0.0;      // km below the surface
    long timeEpoch = 0;        // event time, Unix seconds (USGS reports ms)
    bool tsunami = false;      // USGS tsunami flag (a NOAA-relevant event, not a live warning)

    // Filled in by the manager relative to the device location (not from the feed).
    double distanceKm = 0.0;
    double bearingDeg = 0.0;   // 0 = north, clockwise
};

// ------------------------------------------------------------------ poller request / result
// Two endpoints: a worldwide "recent" query (powers Biggest / Stats / the no-location fallback),
// and a "nearby" query bounded to a radius around the device (powers the Radar / nearest list).
enum class SeismicEndpoint : uint8_t { Recent, Nearby };

// Loop -> worker: a single GET to perform, fully built on the loop task.
struct SeismicFetchRequest {
    SeismicEndpoint endpoint = SeismicEndpoint::Recent;
    String url;
    std::vector<std::pair<String, String>> params;
    std::vector<std::pair<String, String>> headers;
};

// Worker -> loop: the parsed quake list. ok is the HTTP/parse-level outcome; an ok-but-empty result
// (e.g. no quakes in range) is valid "no data".
struct SeismicFetchResult {
    SeismicEndpoint endpoint = SeismicEndpoint::Recent;
    bool ok = false;
    std::vector<Quake> quakes;
};

// -------------------------------------------------------------------------------- parser
// Reads a USGS GeoJSON FeatureCollection (root["features"]) into `out`, newest first as served,
// tolerating missing optional fields. `cap` bounds the retained vector so a huge response can't
// exhaust RAM.
void ParseQuakes(JsonObjectConst root, std::vector<Quake>& out, size_t cap);

// ----------------------------------------------------------------------------- geo helpers
// Great-circle distance (km) and initial bearing (degrees, 0 = N, clockwise) from point 1 to 2.
double DistanceKm(double lat1, double lon1, double lat2, double lon2);
double BearingDeg(double lat1, double lon1, double lat2, double lon2);

} // namespace seismic
