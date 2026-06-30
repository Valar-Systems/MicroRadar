#pragma once

#include <Arduino.h>
#include <vector>
#include <utility>
#include <ArduinoJson.h>

// Typed models for Spacescope's data feeds plus the parsers that build them off the wire, and the
// request/result envelopes the background poller hands across the task boundary. Mirrors the EAM
// models' shape (feed-agnostic structs + free-function parsers + a fetch envelope).
//
// Stage 2 covers three keyless public sources: ISS position (wheretheiss.at), the next rocket
// launch (RocketLaunch.Live), and the planetary Kp index (NOAA SWPC). All fields are best-effort;
// the screens degrade gracefully when a poll hasn't landed or a field is missing.
namespace space {

// ------------------------------------------------------------------------------- ISS position
struct IssState {
    bool valid = false;
    double lat = 0.0;          // degrees
    double lon = 0.0;          // degrees
    double altKm = 0.0;        // km
    double velocityKmh = 0.0;  // km/h
    double footprintKm = 0.0;  // visible-circle diameter, km (optional)
    bool sunlit = true;        // visibility == "daylight" (vs "eclipsed")
    long timestamp = 0;        // unix seconds from the feed (optional)
};

// ----------------------------------------------------------------------------------- launches
struct Launch {
    String mission;            // payload / mission (best-effort; parsed from the name)
    String provider;           // e.g. "SpaceX"
    String vehicle;            // e.g. "Falcon 9"
    String pad;                // pad + location, e.g. "SLC-40, Cape Canaveral"
    long t0Epoch = 0;          // best-available launch instant (UTC unix), 0 if unknown
    bool precise = false;      // true when t0Epoch came from t0/win_open (a real time), not a coarse date
    String dateStr;            // coarse fallback for the display, e.g. "Jun 30"
};

// ------------------------------------------------------------------------------ space weather
struct SpaceWx {
    bool valid = false;
    float kp = -1.0f;          // latest planetary Kp 0..9 (-1 = unknown)
    long timeTagEpoch = 0;     // UTC unix of the latest sample (optional)
    std::vector<float> history; // recent Kp values, oldest..newest, bounded (for a sparkline)
};

// ----------------------------------------------------------------- poller request / result
enum class SpaceEndpoint : uint8_t { Iss, Launch, Kp };

// Loop -> worker: a single GET to perform, fully built on the loop task.
struct SpaceFetchRequest {
    SpaceEndpoint endpoint = SpaceEndpoint::Iss;
    String url;
    std::vector<std::pair<String, String>> params;
    std::vector<std::pair<String, String>> headers;
};

// Worker -> loop: the parsed payload (only the field matching `endpoint` is populated). ok is the
// HTTP/parse-level outcome; an ok-but-empty result (e.g. no upcoming launch) is valid "no data".
struct SpaceFetchResult {
    SpaceEndpoint endpoint = SpaceEndpoint::Iss;
    bool ok = false;
    IssState iss;
    std::vector<Launch> launches;
    SpaceWx wx;
};

// -------------------------------------------------------------------------------- parsers
// Each takes an already-decoded ArduinoJson value and fills the typed struct, tolerating missing
// optional fields. Caps bound retained vectors so a hostile/huge response can't exhaust RAM.
bool ParseIss(JsonObjectConst root, IssState& out);                              // wheretheiss.at flat object
void ParseLaunches(JsonObjectConst root, std::vector<Launch>& out, size_t cap);  // reads root["result"]
bool ParseKp(JsonArrayConst root, SpaceWx& out, size_t historyCap);              // SWPC array of {time_tag,Kp} objects

// Parse "YYYY-MM-DD(T| )hh:mm[:ss][.fff][Z|+oo:oo]" (treated as UTC) to a Unix epoch; 0 on failure.
long Iso8601ToEpoch(const String& s);

} // namespace space
