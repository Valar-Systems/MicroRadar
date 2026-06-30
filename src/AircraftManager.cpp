#include "AircraftManager.h"

#include <algorithm>
#include <cmath>
#include <time.h>
#include <utility>

#include <WiFi.h> // WiFi.localIP() for the device address on the Stats screen

#include "SpecialAircraft.h"
#include "DeviceIdentity.h"
#include "Layout.h"
#include "Board.h"

// adsbdb thumbnails (airport-data.com) are a standard 150x100
constexpr int PHOTO_W = 150;
constexpr int PHOTO_H = 100;

// Display-unit conversions. The feeds are normalised to OpenSky's SI internally
// (metres, m/s); aviation/US convention shows altitude in feet and ground speed
// in knots, so convert at each on-screen/notification site that shows telemetry.
constexpr float METRES_TO_FEET = 3.28084f;
constexpr float MS_TO_KNOTS    = 1.94384f;

// Critical-heap floor below which we don't even start an enrichment HTTPS lookup -- a
// last-ditch guard, not a routine throttle. The post-banding/streaming build runs with
// only ~24-28 KB largest free block yet the OpenSky fetch (same mbedTLS path) handshakes
// fine there, so the gate-time largest-block reading is a poor predictor of handshake
// success; keep this well below normal operation so enrichment behaves like the (ungated)
// OpenSky fetch -- attempt the handshake and let the 30 s failure backoff handle a miss.
// Also the threshold for the [fetch] low-heap warning. Raising it starves enrichment off.
constexpr uint32_t ENRICH_TLS_HEAP_FLOOR = 16000;

// aircraft list-view layout, shared by the renderer and the row hit-test. Everything is
// derived from SCREEN_SIZE so the list stays centred on any SKU: the columns are laid out
// inside a fixed-width block that DrawList centres horizontally, and the row count fills the
// panel down to the clock. (The old absolute 40/120/162 columns + 9 rows were 240-only and
// clustered the whole list into the top-left quadrant of the 412 panel.)
constexpr int LIST_ROW_TOP = 40;
constexpr int LIST_ROW_H = 18;
constexpr int LIST_BOTTOM_RESERVE = 38; // leaves the clock row (SCREEN_SIZE-30) clear; yields 9 rows on the 240 C3
constexpr int LIST_ROWS = (SCREEN_SIZE - LIST_ROW_TOP - LIST_BOTTOM_RESERVE) / LIST_ROW_H;
// Row columns, relative to the centred block's left edge (block-relative, not absolute):
constexpr int LIST_BLOCK_W  = 160; // nominal block width used to centre the columns (== the 240 C3's 40..200 span)
constexpr int LIST_COL_CS   = 0;   // callsign
constexpr int LIST_COL_TYPE = 80;  // type
constexpr int LIST_COL_ALT  = 122; // altitude

#include <ArduinoJson.h>

// Handoff payloads for the background OpenSky fetch task. Both are passed between
// tasks by pointer through a queue, transferring ownership: the receiver frees it.
struct FetchRequest {
    double lat, lon, radLat, radLon; // scan box, snapshotted on the loop task
    String token;                    // bearer token (empty = anonymous request)
    bool   local = false;            // true: poll the local receiver instead of OpenSky
    String url;                      // local aircraft.json URL (only when local)
};
struct FetchResult {
    bool ok = false;                 // false on network / parse failure
    std::vector<Aircraft> aircraft;  // parsed state vectors (empty unless ok)
};

// Handoff payloads for the background enrichment task (adsbdb metadata/route +
// aircraft photo). Like the fetch payloads above, ownership transfers by pointer
// through a depth-1 queue: the receiver frees it. The task fills a result the loop
// applies; it never touches trackedAircraft, the photo sprite, or the logbook.
enum class EnrichKind : uint8_t { Metadata, Route, Photo };

struct EnrichRequest {
    EnrichKind kind;
    String icao24;    // aircraft the result applies to (all kinds)
    String callsign;  // Route only
    String url;       // Photo only
};

struct EnrichResult {
    EnrichKind kind = EnrichKind::Metadata;
    String icao24;
    bool definitive = false; // a final HTTP answer arrived; false = transient failure (retry)

    // Metadata
    String typeCode, typeName, operatorName, registration, photoUrl;
    // Route
    String routeCallsign, routeOrigin, routeDest;
    // Photo: the raw JPEG body, decoded on the loop so the sprite stays single-task
    bool photoFetched = false;
    String photoBytes;
};

namespace {

// The international emergency squawk codes. These always trigger the alert
// styling regardless of display settings.
bool isEmergencySquawk(const String& squawk)
{
    return squawk == "7500" || squawk == "7600" || squawk == "7700";
}

// Marker color by barometric altitude (meters), low to high. Deliberately
// avoids red, which is reserved for the emergency alert.
uint32_t altitudeColor(float altMeters)
{
    if (altMeters < 1000.0f) return lgfx::color888(0, 255, 0);     // green   - low
    if (altMeters < 3000.0f) return lgfx::color888(170, 255, 0);   // lime
    if (altMeters < 6000.0f) return lgfx::color888(255, 255, 0);   // yellow
    if (altMeters < 9000.0f) return lgfx::color888(0, 255, 255);   // cyan
    return lgfx::color888(255, 255, 255);                          // white   - high
}

// Scale a packed RGB888 colour's brightness by f (0..1). Used to fade a blip as
// its radar return ages between sweep passes.
uint32_t scaleColor(uint32_t rgb, float f)
{
    if (f >= 1.0f) return rgb;
    if (f < 0.0f) f = 0.0f;
    const uint8_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    return lgfx::color888((uint8_t)(r * f), (uint8_t)(g * f), (uint8_t)(b * f));
}

// True when the sun is below the horizon at (lat, lon) for the given UTC time.
// Uses the NOAA solar-position equations and evaluates the sun's elevation
// directly, which sidesteps the UTC day-wrap pitfalls of comparing against
// sunrise/sunset clock times.
bool isNightNow(double latDeg, double lonDeg, time_t nowUtc)
{
    struct tm t;
    gmtime_r(&nowUtc, &t);

    const double gamma = 2.0 * PI / 365.0 * (t.tm_yday + (t.tm_hour - 12) / 24.0);
    const double eqTime = 229.18 * (0.000075 + 0.001868 * cos(gamma) - 0.032077 * sin(gamma)
                          - 0.014615 * cos(2 * gamma) - 0.040849 * sin(2 * gamma));
    const double decl = 0.006918 - 0.399912 * cos(gamma) + 0.070257 * sin(gamma)
                        - 0.006758 * cos(2 * gamma) + 0.000907 * sin(2 * gamma)
                        - 0.002697 * cos(3 * gamma) + 0.00148 * sin(3 * gamma);

    const double nowMin = t.tm_hour * 60.0 + t.tm_min + t.tm_sec / 60.0;
    const double trueSolarMin = nowMin + eqTime + 4.0 * lonDeg; // longitude east-positive
    const double hourAngle = radians(trueSolarMin / 4.0 - 180.0);
    const double latRad = radians(latDeg);
    const double elevation = asin(sin(latRad) * sin(decl) + cos(latRad) * cos(decl) * cos(hourAngle));

    return degrees(elevation) < -0.833; // standard sunrise/sunset refraction angle
}

// Turn whatever the user typed for their receiver into a usable aircraft.json URL.
// Accepts a bare host/IP ("192.168.1.50"), a base URL, or a full .json URL: we add
// a default scheme and the conventional dump1090/readsb path only when they're
// missing, so someone who pasted an exact URL is left untouched.
String normalizeLocalUrl(String url)
{
    url.trim();
    if (url.isEmpty())
        return url;

    if (url.indexOf("://") < 0)
        url = "http://" + url;

    String lower = url;
    lower.toLowerCase();
    if (lower.indexOf(".json") < 0) {
        while (url.endsWith("/"))
            url.remove(url.length() - 1);
        url += "/data/aircraft.json";
    }
    return url;
}

// --- Enrichment producers ---------------------------------------------------
// Each runs on the enrichment task: one blocking GET + parse, returning a
// heap-allocated result for the loop to apply. They reference nothing the loop
// owns -- only the shared, mutex-guarded HTTP client -- so an aircraft leaving
// range (erased on the loop) can never be written under them. Always non-null,
// so the loop's in-flight gate is always cleared by the matching result.

EnrichResult* fetchAircraftMetadata(HttpRequestManager& http, const String& icao24)
{
    EnrichResult* res = new EnrichResult();

    const HttpResult result = http.Get("https://api.adsbdb.com/v0/aircraft/" + icao24);

    // A network-level failure (no HTTP response at all) is transient: leave it
    // non-definitive so the loop returns the aircraft to NotFetched and retries.
    if (!result.success) {
        Serial.printf("[adsbdb] lookup for %s failed: %s\n", icao24.c_str(), result.errorMessage.c_str());
        return res;
    }

    // Any definitive HTTP response -- including 404 "unknown aircraft" -- is final.
    res->definitive = true;

    JsonDocument doc;
    if (deserializeJson(doc, result.response))
        return res; // malformed body: treat as no data, but don't retry

    // adsbdb wraps the record in { "response": { "aircraft": {...} } }; when the
    // address is unknown it returns { "response": "unknown aircraft" }, leaving
    // this lookup null.
    JsonObject aircraft = doc["response"]["aircraft"];
    if (aircraft.isNull()) {
        Serial.printf("[adsbdb] %s -> unknown aircraft\n", icao24.c_str());
        return res;
    }

    res->typeCode     = aircraft["icao_type"].isNull()           ? "" : aircraft["icao_type"].as<String>();
    res->typeName     = aircraft["type"].isNull()                ? "" : aircraft["type"].as<String>();
    res->operatorName = aircraft["registered_owner"].isNull()    ? "" : aircraft["registered_owner"].as<String>();
    res->registration = aircraft["registration"].isNull()        ? "" : aircraft["registration"].as<String>();
    res->photoUrl     = aircraft["url_photo_thumbnail"].isNull() ? "" : aircraft["url_photo_thumbnail"].as<String>();

    Serial.printf("[adsbdb] %s -> type=%s operator=%s reg=%s\n",
                  icao24.c_str(), res->typeCode.c_str(),
                  res->operatorName.c_str(), res->registration.c_str());
    return res;
}

EnrichResult* fetchRoute(HttpRequestManager& http, const String& callsign)
{
    EnrichResult* res = new EnrichResult();

    const HttpResult result = http.Get("https://api.adsbdb.com/v0/callsign/" + callsign);

    // network-level failure is transient: stay non-definitive so it's retried
    if (!result.success && result.statusCode <= 0) {
        Serial.printf("[adsbdb] route %s failed: %s\n", callsign.c_str(), result.errorMessage.c_str());
        return res;
    }

    // any definitive HTTP response (incl. 404 unknown callsign) is final
    res->definitive = true;
    res->routeCallsign = callsign;

    JsonDocument doc;
    if (deserializeJson(doc, result.response))
        return res;

    JsonObject flightroute = doc["response"]["flightroute"];
    if (flightroute.isNull()) {
        Serial.printf("[adsbdb] route %s -> unknown\n", callsign.c_str());
        return res;
    }

    // prefer the short IATA code, fall back to ICAO
    auto airportCode = [](JsonObject airport) -> String {
        if (airport.isNull()) return "";
        if (!airport["iata_code"].isNull()) return airport["iata_code"].as<String>();
        if (!airport["icao_code"].isNull()) return airport["icao_code"].as<String>();
        return "";
    };

    res->routeOrigin = airportCode(flightroute["origin"]);
    res->routeDest = airportCode(flightroute["destination"]);

    Serial.printf("[adsbdb] route %s -> %s->%s\n", callsign.c_str(),
                  res->routeOrigin.c_str(), res->routeDest.c_str());
    return res;
}

EnrichResult* fetchPhoto(HttpRequestManager& http, const String& url)
{
    EnrichResult* res = new EnrichResult();

    HttpResult result = http.Get(url);
    if (!result.success) {
        Serial.printf("[photo] fetch failed: %s\n", result.errorMessage.c_str());
        return res; // photoFetched stays false
    }

    res->photoFetched = true;
    res->photoBytes = std::move(result.response); // decoded on the loop into the shared sprite
    return res;
}

// Hand a request to the enrichment task. Frees it (and reports failure) if the
// depth-1 queue is somehow full -- which can't happen while the loop only ever
// queues one request at a time behind the enrichInFlight gate.
bool enqueueEnrich(QueueHandle_t queue, EnrichRequest* req)
{
    if (xQueueSend(queue, &req, 0) == pdTRUE)
        return true;
    delete req;
    return false;
}

} // namespace

void AircraftManager::Initialise()
{
    // get centre point + radius
    lat = configServer.GetStoredString("latitude").toDouble();
    lon = configServer.GetStoredString("longitude").toDouble();

    // "radius" is stored as a real-world distance (km or mi). Convert it into
    // separate latitude/longitude degree spans: 1 deg latitude is ~111 km
    // everywhere, but 1 deg longitude is ~111 km * cos(latitude), so the box
    // must be wider in degrees near the equator and narrower near the poles to
    // stay square on the ground.
    // Default to 100 when unset/empty, matching the config form's default (it shows "100" only when
    // the key is absent -- a stored empty string would otherwise read as 0). Without this, a device
    // with a location but no saved radius scans a ~111 m box (the MIN_DEGREES floor) and shows no
    // aircraft until the radius is explicitly changed.
    const String radiusStr = configServer.GetStoredString("radius");
    const double distance = radiusStr.isEmpty() ? 100.0 : radiusStr.toDouble();
    const bool inMiles = configServer.GetStoredString("radius-unit") == "mi";
    const double distanceKm = inMiles ? distance * 1.609344 : distance;

    constexpr double KM_PER_DEGREE = 111.0;
    constexpr double MAX_DEGREES = 2.0; // keep the OpenSky box within rate-limit area

    double cosLat = std::cos(radians(lat));
    if (cosLat < 0.01) cosLat = 0.01; // guard against div-by-zero near the poles

    constexpr double MIN_DEGREES = 0.001; // ~111 m floor; keeps the projection from dividing by zero

    radLat = std::min(std::max(distanceKm / KM_PER_DEGREE, MIN_DEGREES), MAX_DEGREES);
    radLon = std::min(std::max(distanceKm / (KM_PER_DEGREE * cosLat), MIN_DEGREES), MAX_DEGREES);

    // outer range-ring distance in the user's unit (derived from the clamped
    // radLat so the labels match what's actually drawn), for the ring labels
    rangeRadiusDisplay = radLat * KM_PER_DEGREE;
    if (inMiles) rangeRadiusDisplay /= 1.609344;
    rangeUnit = inMiles ? "mi" : "km";

    // configuration
    const String renderText = configServer.GetStoredString("infotext");
    const String renderTris = configServer.GetStoredString("triangle");
    const String renderTrail = configServer.GetStoredString("trail");
    const String renderAltColor = configServer.GetStoredString("altcolor");
    const String renderHighlight = configServer.GetStoredString("highlight");
    if (!renderText.isEmpty()) displayInfoText = renderText == "true" ? true : false;
    if (!renderTris.isEmpty()) displayTriangles = renderTris == "true" ? true : false;
    if (!renderTrail.isEmpty()) displayTrails = renderTrail == "true" ? true : false;
    if (!renderAltColor.isEmpty()) displayAltColor = renderAltColor == "true" ? true : false;
    if (!renderHighlight.isEmpty()) displayHighlight = renderHighlight == "true" ? true : false;

    // sweep beam: unset (or "true") = on, matching the radar's drawScan gate in main.cpp
    const String renderSweep = configServer.GetStoredString("scanline");
    displaySweep = renderSweep.isEmpty() || renderSweep == "true";

    // paint-and-fade blips: unset (or "true") = on. Only takes effect with the sweep on.
    const String renderFade = configServer.GetStoredString("fade");
    displayFade = renderFade.isEmpty() || renderFade == "true";

    // which individual info lines to show. An unset key (device never saved, or
    // an older save predating this field) falls back to the field's default.
    infoFieldEnabled.resize(AIRCRAFT_INFO_FIELD_COUNT);
    metadataNeeded = false;
    for (size_t i = 0; i < AIRCRAFT_INFO_FIELD_COUNT; ++i) {
        const String stored = configServer.GetStoredString(AIRCRAFT_INFO_FIELDS[i].key);
        infoFieldEnabled[i] = stored.isEmpty() ? AIRCRAFT_INFO_FIELDS[i].defaultOn
                                               : (stored == "true");
        // only spend network calls on adsbdb when an enrichment field is shown
        if (infoFieldEnabled[i] && AIRCRAFT_INFO_FIELDS[i].needsLookup)
            metadataNeeded = true;
    }

    // watchlist: split on commas/newlines/semicolons into lowercased prefixes
    watchlist.clear();
    const String wl = configServer.GetStoredString("watchlist");
    int tokenStart = 0;
    for (int i = 0; i <= (int)wl.length(); ++i) {
        const char c = (i < (int)wl.length()) ? wl[i] : ',';
        if (c == ',' || c == ';' || c == '\n' || c == '\r') {
            String token = wl.substring(tokenStart, i);
            token.trim();
            token.toLowerCase();
            if (!token.isEmpty()) watchlist.push_back(token);
            tokenStart = i + 1;
        }
    }
    ntfyTopic = configServer.GetStoredString("ntfy-topic");
    ntfyTopic.trim();

    // a watchlist needs registration/type, so make sure metadata gets fetched
    if (!watchlist.empty()) metadataNeeded = true;

    // military / special-aircraft detection (offline, ICAO-address based; needs no
    // enrichment). Highlighting defaults on; the ntfy alert defaults off.
    const String milShow = configServer.GetStoredString("mil-show");
    showMilitary = milShow.isEmpty() ? true : (milShow == "true");
    const String milAlert = configServer.GetStoredString("mil-alert");
    alertMilitary = milAlert.isEmpty() ? false : (milAlert == "true");
    const String heliShow = configServer.GetStoredString("heli-show");
    showHelicopters = heliShow.isEmpty() ? false : (heliShow == "true");
    const String spcShow = configServer.GetStoredString("spc-show");
    showSpecial = spcShow.isEmpty() ? false : (spcShow == "true");

    // spotting logbook: when on, learn each contact's type/airline (so it needs
    // the adsbdb enrichment) and start the persistent store once.
    const String logbookStr = configServer.GetStoredString("logbook");
    logbookEnabled = logbookStr.isEmpty() ? false : (logbookStr == "true");
    if (logbookEnabled) {
        metadataNeeded = true;
        logbook.Begin(); // idempotent: only the first call loads from NVS
    }

    // "look up!" overhead alert. The distance is entered in the same unit as the
    // radar radius; store it in km for the centre-distance comparison.
    showOverhead = configServer.GetStoredString("lookup") == "true";
    alertOverhead = configServer.GetStoredString("lookup-alert") == "true";
    double lookupDist = configServer.GetStoredString("lookup-dist").toDouble();
    if (lookupDist <= 0.0) lookupDist = 3.0;
    overheadKm = inMiles ? lookupDist * 1.609344 : lookupDist;

    // Home Assistant / MQTT publishing (off by default).
    mqttEnabled = configServer.GetStoredString("mqtt") == "true";
    mqttDiscovery = configServer.GetStoredString("mqtt-disco") != "false"; // default on
    mqttBase = configServer.GetStoredString("mqtt-base");
    mqttBase.trim();
    if (mqttBase.isEmpty()) mqttBase = "blipscope";

    MqttPublisher::Config mc;
    mc.enabled = mqttEnabled;
    mc.host = configServer.GetStoredString("mqtt-host");  mc.host.trim();
    const String mqttPort = configServer.GetStoredString("mqtt-port");
    mc.port = mqttPort.isEmpty() ? 1883 : (uint16_t)mqttPort.toInt();
    mc.user = configServer.GetStoredString("mqtt-user");
    mc.pass = configServer.GetStoredString("mqtt-pass");
    mc.statusTopic = mqttBase + "/status";
    mqtt.Begin(mc); // spawns the publisher task once; reconfigures it thereafter
    lastMqttState = millis() - 5000; // publish a first snapshot promptly once connected

    // data source: OpenSky cloud (default) or the user's own ADS-B receiver. The
    // URL is normalised once here so the fetch task gets a ready-to-GET endpoint.
    useLocalSource = configServer.GetStoredString("data-source") == "local";
    localUrl = useLocalSource ? normalizeLocalUrl(configServer.GetStoredString("local-url")) : "";

    if (useLocalSource) {
        // A local receiver has no API rate limit and refreshes about once a second;
        // poll at that rate. The GET + parse runs on the background fetch task, so
        // this cadence doesn't stall the render loop. No OpenSky token is needed.
        constexpr unsigned long LOCAL_FETCH_INTERVAL = 1000;
        fetchInterval = LOCAL_FETCH_INTERVAL;
        Serial.printf("[source] local receiver: %s (every %lu ms)\n",
                      localUrl.c_str(), fetchInterval);
    } else {
        // calculate how often we can call OpenSky API before being rate limited
        constexpr int MS_PER_DAY = 24 * 60 * 60 * 1000;
        constexpr int ANONYMOUS_TOKENS_PER_DAY = 400;
        constexpr int AUTHED_TOKENS_PER_DAY = 4000;
        constexpr int TOKEN_BUFFER = 3;
        int dailyRequestBudget = ANONYMOUS_TOKENS_PER_DAY - TOKEN_BUFFER; // non-authed tokens minus buffer

        const String token = authHandler.GetValidToken(configServer.GetStoredString("opensky-id"), configServer.GetStoredString("opensky-secret"));
        if (!token.isEmpty())
            dailyRequestBudget = AUTHED_TOKENS_PER_DAY - TOKEN_BUFFER; // authed tokens minus buffer

        fetchInterval = MS_PER_DAY / dailyRequestBudget;
    }

    // backlight brightness (PWM). Default full; clamp away from 0 so the screen
    // can't be saved completely dark. This is the daytime/base level; auto-dim
    // reduces it at night.
    const String brightnessStr = configServer.GetStoredString("brightness");
    configuredBrightness = brightnessStr.isEmpty()
        ? 255 : (uint8_t)constrain(brightnessStr.toInt(), 10, 255);
    tft.setBrightness(configuredBrightness);
    currentBrightness = configuredBrightness;

    const String autoDimStr = configServer.GetStoredString("autodim");
    autoDim = autoDimStr.isEmpty() ? true : (autoDimStr == "true");

    // clock offset: default to the nominal zone from longitude (15 deg/hour)
    const String tzStr = configServer.GetStoredString("tz-offset");
    utcOffsetSec = tzStr.isEmpty() ? (long)lround(lon / 15.0) * 3600
                                   : (long)(tzStr.toFloat() * 3600.0f);

    lastBrightnessCheck = 0; // re-evaluate dimming promptly after a reload

    // Force the next Update() to fetch immediately. On a config reload this means
    // a changed location/radius refreshes the radar right away rather than after
    // a full interval; subtracting the interval (unsigned millis() wraparound)
    // makes the next "now - lastFetch >= fetchInterval" check true at any uptime.
    lastFetch = millis() - fetchInterval;

    // seed the touch clock to now, so enrichment is paused for the first few seconds after
    // boot/reload -- that gives the touch poll the bus first while the radar populates.
    lastTouchActivityMs = millis();

    // spawn the background fetch task (once; survives config reloads -- it picks up
    // new location/credentials from each RequestFetch snapshot)
    StartFetchTask();

    // spawn the background enrichment task (once; same lifetime as the fetch task)
    StartEnrichTask();
}

void AircraftManager::Update()
{
    unsigned long now = millis();

    // advance the radar sweep + paint the contacts it crossed this frame, before
    // any early-return below, so the beam keeps turning even while a card is open
    AdvanceSweep();

    // solar day/night backlight dimming (self-throttled)
    UpdateBrightness();

    // Optional on-board peripherals (compiled out on SKUs without them). Both run before the
    // inDetail early-return below so the buzzer still finishes its beep and the tilt readout
    // keeps updating while a card is open.
    if constexpr (variant::HAS_AUDIO)
        board::BuzzerUpdate();
    if constexpr (variant::HAS_IMU) {
        if (now - lastImuReadMs >= 200) { // ~5 Hz is ample for a tilt readout
            lastImuReadMs = now;
            board::Imu s;
            if (board::ImuRead(s)) {
                constexpr float RAD2DEG = 57.2957795f;
                // The QMI8658 sits with +Z pointing away from the screen, so it reads az = -1g when
                // the board lies flat. Referencing roll against -az makes "flat" read ~0 instead of
                // ~180; pitch's denominator is sign-independent so it already reads 0 at flat.
                imuPitch = atan2f(-s.ax, sqrtf(s.ay * s.ay + s.az * s.az)) * RAD2DEG;
                imuRoll  = atan2f(s.ay, -s.az) * RAD2DEG;
                imuValid = true;
            }
        }
    }

    // flush the logbook to flash if it's accumulated changes (debounced internally)
    if (logbookEnabled)
        logbook.MaybePersist();

    // Home Assistant / MQTT: (re)publish the discovery configs + a fresh snapshot
    // on each connect, then a retained summary every few seconds. Runs regardless
    // of the detail view so the broker stays current; the publisher task does the
    // actual (non-blocking) socket work.
    if (mqttEnabled) {
        if (mqtt.ConsumeJustConnected()) {
            if (mqttDiscovery) PublishMqttDiscovery();
            PublishMqttState();
            lastMqttState = now;
        } else if (mqtt.Connected() && now - lastMqttState >= 5000) {
            lastMqttState = now;
            PublishMqttState();
        }
    }

    // apply any completed background enrichment (metadata / route / photo) so the
    // detail card and radar labels pick it up the same frame it arrives. Done before
    // the inDetail early-return below so a card's lookups keep flowing while it's open.
    ConsumeEnrichResults();

    // fill in the selected aircraft's details first, so the detail card from the
    // prior frame's tap stays on screen during each brief lookup
    ProcessDetailLookups();

    // handle taps every loop so the UI stays responsive between fetches
    HandleTouch();

    // While the detail card is open the radar isn't visible and the user is
    // interacting, so skip the radar's background network work below (metadata
    // enrichment, watchlist/overhead alerts, and the periodic feed fetch). The
    // enrichment and the fetch are non-blocking now (each runs on its own task),
    // but the ntfy alert POST still blocks the loop for up to a few seconds, and the
    // touch panel is only polled once per loop -- so a quick "tap to close" landing
    // during that POST was never sampled, which is what made it take two or three
    // taps. Bailing out here keeps the loop fast while a card is up, so a dismiss tap
    // registers the first time. The card's own lookups still flow via
    // ProcessDetailLookups()/ConsumeEnrichResults() above, and normal fetching
    // resumes the instant the card closes (lastFetch is already stale, so the next
    // Update() refreshes immediately).
    if (inDetail)
        return;

    // Merge a completed background fetch into trackedAircraft. The blocking GET +
    // JSON decode already ran on the fetch task; this is just a fast map merge.
    ConsumeFetchResult();

    // Hold off the radar enrichment + alerts while a feed fetch is in flight. The
    // metadata enrichment is queued to the enrich task now (non-blocking), but gating
    // it here keeps a second adsbdb request from queueing right behind the feed fetch
    // and spacing them keeps peak heap down on this tight board; the ntfy POST in
    // ProcessAlerts still blocks the loop, and RequestFetch's token refresh can still
    // block on the rare ~29-minute renewal.
    if (!fetchInFlight) {
        // enrich a tracked aircraft with adsbdb metadata (queued, throttled internally)
        ProcessMetadataLookups();

        // alert on watchlisted / military / overhead aircraft (throttled internally)
        ProcessAlerts();

        // kick off the next fetch when due. Non-blocking: the loop keeps polling
        // touch and drawing while the request runs on the fetch task, so tapping a
        // plane during a refresh is no longer missed.
        if (now - lastFetch >= fetchInterval) {
            lastFetch = now;
            RequestFetch();
        }
    }
}

void AircraftManager::StartFetchTask()
{
    if (fetchTaskHandle != nullptr)
        return; // already running; survive Initialise() being re-run on a config reload

    // Depth 1: only ever one fetch outstanding (gated by fetchInFlight), so a single
    // slot for the request and one for the result is enough.
    fetchRequestQueue = xQueueCreate(1, sizeof(FetchRequest*));
    fetchResultQueue  = xQueueCreate(1, sizeof(FetchResult*));

    // 12 KB stack: the HTTPS handshake (mbedTLS) is the stack-hungry part, plus the
    // JSON decode. The Arduino loop task ran the same workload in 8 KB, so this has
    // headroom. Priority 1 (same as the loop); it spends almost all its life blocked
    // on the request queue.
    //
    // Pin to core 0 -- the WiFi core. On the dual-core S3 that keeps the TLS/JSON work off
    // core 1, where the Arduino loop drives the RGB panel: an unpinned task would otherwise
    // float onto the draw core and add to the frame-timing jitter. (Harmless on the single-core
    // C3, where core 0 is the only core.)
    xTaskCreatePinnedToCore(FetchTaskTrampoline, "osky_fetch", 12288, this, 1, &fetchTaskHandle, 0);
}

void AircraftManager::FetchTaskTrampoline(void* arg)
{
    static_cast<AircraftManager*>(arg)->RunFetchTask();
}

void AircraftManager::RunFetchTask()
{
    for (;;) {
        // block until the loop requests a fetch
        FetchRequest* req = nullptr;
        if (xQueueReceive(fetchRequestQueue, &req, portMAX_DELAY) != pdTRUE || req == nullptr)
            continue;

        FetchResult* res = new FetchResult();

        // GET + decode the feed straight from the socket (GetJson streams when possible
        // so the raw body and the parsed document aren't both held at once -- that peak
        // is what starved the heap). Either the user's local receiver (no params/auth)
        // or the OpenSky API bounded to the scan box; both share the one HTTP client.
        JsonDocument doc;
        HttpResult result;
        if (req->local) {
            result = http.GetJson(req->url, doc);
        } else {
            std::vector<std::pair<String, String>> headers = {};
            if (!req->token.isEmpty()) headers.push_back({ "Authorization", "Bearer " + req->token });

            result = http.GetJson(
                "https://opensky-network.org/api/states/all",
                doc,
                {
                  // 6 decimals (~0.1 m): String(double) defaults to only 2, which would
                  // quantize small km/mi radii into a coarse ~1 km box or collapse it
                  {"lamin", String(req->lat - req->radLat, 6)},
                  {"lamax", String(req->lat + req->radLat, 6)},
                  {"lomin", String(req->lon - req->radLon, 6)},
                  {"lomax", String(req->lon + req->radLon, 6)},
                  // category (state vector index 17) is omitted from the default
                  // response; without extended=1 the array stops at index 16 and the
                  // Category info line is always blank
                  {"extended", "1"}
                },
                headers
            );
        }

        const char* sourceName = req->local ? "Local ADS-B" : "OpenSky";
        if (!result.success) {
            Serial.printf("[WARN] %s request failed: %s\n", sourceName, result.errorMessage.c_str());
        } else if (req->local) {
            // dump1090/readsb returns objects under "aircraft"; convert each and
            // clip to the scan box ourselves (OpenSky does this server-side, but a
            // local receiver reports everything it hears).
            for (JsonVariantConst entry : doc["aircraft"].as<JsonArrayConst>()) {
                Aircraft ac;
                if (!JsonParser::ParseLocalAircraft(entry, ac))
                    continue;
                if (ac.latitude  < req->lat - req->radLat || ac.latitude  > req->lat + req->radLat ||
                    ac.longitude < req->lon - req->radLon || ac.longitude > req->lon + req->radLon)
                    continue;
                res->aircraft.push_back(ac);
            }
            res->ok = true;
        } else {
            res->aircraft = JsonParser::ParseArray<Aircraft>(doc["states"]);
            res->ok = true;
        }

        // Heap health check right after the decode -- the cycle's low point, the same
        // pressure TLS handshakes and the config web server fight for. Stay silent when
        // healthy; warn only when the largest block falls to where enrichment starts
        // getting throttled (ENRICH_TLS_HEAP_FLOOR) -- the early sign we're sliding back
        // toward the TLS / config-page failures this all fixed.
        if (const uint32_t largest = ESP.getMaxAllocHeap(); largest < ENRICH_TLS_HEAP_FLOOR)
            Serial.printf("[fetch] LOW HEAP after %s: free=%u largest=%u aircraft=%u\n",
                          sourceName, (unsigned)ESP.getFreeHeap(), (unsigned)largest, (unsigned)res->aircraft.size());

        delete req;

        // hand the result back; the loop consumed the previous one before requesting
        // again, so the depth-1 queue always has room
        if (xQueueSend(fetchResultQueue, &res, 0) != pdTRUE)
            delete res; // unreachable in practice; just don't leak if it ever happens
    }
}

void AircraftManager::RequestFetch()
{
    FetchRequest* req = new FetchRequest();
    req->lat = lat; req->lon = lon; req->radLat = radLat; req->radLon = radLon;
    req->local = useLocalSource;

    if (useLocalSource) {
        // local receiver: no auth, no token lookup. Skip entirely if no URL is set
        // yet (local selected but the field left blank) rather than GET an empty URL.
        if (localUrl.isEmpty()) {
            delete req;
            return;
        }
        req->url = localUrl;
    } else {
        // Token lookup is normally instant (cached); it only blocks the loop on the rare
        // ~29-minute refresh, so it stays here rather than racing the fetch task's client.
        req->token = authHandler.GetValidToken(
            configServer.GetStoredString("opensky-id"),
            configServer.GetStoredString("opensky-secret")
        );
    }

    if (xQueueSend(fetchRequestQueue, &req, 0) == pdTRUE)
        fetchInFlight = true;
    else
        delete req; // queue full (shouldn't happen: we only request when !fetchInFlight)
}

void AircraftManager::ConsumeFetchResult()
{
    if (fetchResultQueue == nullptr)
        return;

    FetchResult* res = nullptr;
    if (xQueueReceive(fetchResultQueue, &res, 0) != pdTRUE)
        return; // nothing ready

    fetchInFlight = false;

    if (res->ok) {
        const unsigned long now = millis();

        for (auto& ac : res->aircraft) {
            auto it = trackedAircraft.find(ac.icao24);
            if (it == trackedAircraft.end()) {
                trackedAircraft.emplace(ac.icao24, TrackedAircraft{ ac, now });
                // a fresh contact entered range: bump the odometer and log its
                // origin country (the one logbook field the feed gives us directly).
                if (logbookEnabled) {
                    logbook.NoteContact();
                    logbook.NoteCountry(ac.originCountry);
                }
                // chirp on genuinely new arrivals (HAS_AUDIO boards), but not during the
                // initial bulk population -- that would be a burst of beeps on first sync.
                if constexpr (variant::HAS_AUDIO)
                    if (initialSyncDone) board::BuzzerChirp(40);
            } else {
                it->second.Update(ac, now);
            }
        }

        // the first successful fetch is the baseline; arrivals after it are "new"
        initialSyncDone = true;

        // remove any planes that disappeared from the feed
        for (auto it = trackedAircraft.begin(); it != trackedAircraft.end(); ) {
            const bool present = std::any_of(res->aircraft.begin(), res->aircraft.end(),
                [&](const Aircraft& ac) { return ac.icao24 == it->first; });
            if (!present)
                it = trackedAircraft.erase(it);
            else
                ++it;
        }
    }

    delete res;
}

void AircraftManager::Draw(BandCanvas& backbuffer, bool firstPass)
{
    // the detail card overlays whichever screen is active; fall back to it if the
    // selected aircraft has since dropped out of the feed
    if (inDetail) {
        auto it = trackedAircraft.find(selectedIcao);
        if (it != trackedAircraft.end()) {
            DrawDetailCard(backbuffer, it->second);
            return;
        }
        ExitDetail(); // selected aircraft left the feed (idempotent across band passes)
    }

    switch (screen) {
        case Screen::List:  DrawList(backbuffer);  break;
        case Screen::Stats: DrawStats(backbuffer); break;
        case Screen::Radar:
        default:            DrawRadar(backbuffer, firstPass);  break;
    }
    DrawScreenIndicator(backbuffer);
    DrawClock(backbuffer);
}

SpecialAircraft::Class AircraftManager::SpecialClassOf(const TrackedAircraft& tracked) const
{
    // priority order matches SpecialAircraft::Class: military first, then a
    // distinctive callsign, then a plain rotorcraft. Each gated by its toggle.
    if (showMilitary && SpecialAircraft::IsMilitary(tracked.state.icao24))
        return SpecialAircraft::Class::Military;
    if (showSpecial && SpecialAircraft::IsSpecialCallsign(tracked.state.callsign))
        return SpecialAircraft::Class::Special;
    if (showHelicopters && SpecialAircraft::IsHelicopter(tracked.state.category))
        return SpecialAircraft::Class::Helicopter;
    return SpecialAircraft::Class::None;
}

uint32_t AircraftManager::SpecialColor(SpecialAircraft::Class c)
{
    switch (c) {
        case SpecialAircraft::Class::Military:   return lgfx::color888(255, 120, 0);  // orange (redder than watchlist amber)
        case SpecialAircraft::Class::Special:    return lgfx::color888(80, 170, 255);  // blue
        case SpecialAircraft::Class::Helicopter: return lgfx::color888(190, 110, 255); // violet
        default:                                  return lgfx::color888(0, 200, 0);
    }
}

void AircraftManager::AdvanceSweep()
{
    // Always advance the beam (even off-radar) so it stays continuous when the
    // user returns to the radar screen.
    prevSweepAngle = sweepAngle;
    sweepAngle = std::fmod((float)millis() / SWEEP_PERIOD_MS * TWO_PI, TWO_PI);

    // Skip the per-contact crossing test unless paint-and-fade is active (blips
    // then glide live at full brightness, with or without the beam drawn).
    if (!PaintAndFadeActive())
        return;

    // The beam advanced from prevSweepAngle to sweepAngle this frame -- a small
    // positive arc (mod 2*PI). Any contact whose bearing from centre lies in that
    // arc was just swept: latch its position and reset its fade. Bearings use the
    // same centre and cos/sin convention as the drawn beam, so the paint lands
    // exactly under the beam.
    constexpr int CENTRE = SCREEN_SIZE_DIV_2 - 1;
    float arc = sweepAngle - prevSweepAngle;
    if (arc < 0.0f) arc += TWO_PI; // beam wrapped past 0 / 2*PI this frame

    for (auto& [icao, t] : trackedAircraft) {
        if (t.state.onGround) continue;

        auto [la, lo] = t.GetDisplayPosition();
        auto [x, y] = ProjectCoordinateToScreen(la, lo);

        float rel = std::atan2((float)(y - CENTRE), (float)(x - CENTRE)) - prevSweepAngle;
        while (rel < 0.0f) rel += TWO_PI;
        while (rel >= TWO_PI) rel -= TWO_PI;

        if (rel <= arc) {
            t.Paint();
            if (displayTrails)
                t.SampleTrail(); // trail samples a return per pass, not per frame
        }
    }
}

std::pair<float, float> AircraftManager::RadarBlipPosition(const TrackedAircraft& tracked) const
{
    if (PaintAndFadeActive() && tracked.everPainted)
        return { tracked.paintLat, tracked.paintLon };
    return tracked.GetDisplayPosition();
}

float AircraftManager::RadarBlipBrightness(const TrackedAircraft& tracked) const
{
    return PaintAndFadeActive() ? tracked.PaintBrightness(SWEEP_PERIOD_MS) : 1.0f;
}

void AircraftManager::DrawRadar(BandCanvas& backbuffer, bool firstPass)
{
    DrawRadarCircles(backbuffer);

    // identify the "of interest" contacts to ring: nearest, highest, fastest
    String nearestIcao, highestIcao, fastestIcao;
    if (displayHighlight) {
        float minDist2 = 1e30f, maxAlt = -1e30f, maxVel = -1e30f;
        for (auto& [icao, t] : trackedAircraft) {
            if (t.state.onGround) continue;
            auto [la, lo] = t.GetDisplayPosition();
            const float dLat = la - (float)lat, dLon = lo - (float)lon;
            const float d2 = dLat * dLat + dLon * dLon;
            if (d2 < minDist2)                 { minDist2 = d2; nearestIcao = icao; }
            if (t.state.baroAltitude > maxAlt) { maxAlt = t.state.baroAltitude; highestIcao = icao; }
            if (t.state.velocity > maxVel)     { maxVel = t.state.velocity; fastestIcao = icao; }
        }
    }

    for (auto& [icao, tracked] : trackedAircraft) {
        if (tracked.state.onGround) continue;

        // The scene is rendered once per band; advance per-frame animation/trail state
        // only on the first band so both halves use identical positions (no seam tear).
        if (firstPass) {
            tracked.Tick();
            // With paint-and-fade on, the trail is sampled once per beam pass (in
            // AdvanceSweep); only sample per-frame when blips glide live.
            if (displayTrails && !PaintAndFadeActive())
                tracked.SampleTrail();
        }

        auto [predLat, predLon] = RadarBlipPosition(tracked);
        auto [x, y] = ProjectCoordinateToScreen(predLat, predLon);

        // The whole contact -- marker, trail, label -- fades together as its
        // radar return ages, so a dim blip doesn't sit under a bright trail/label.
        const float blip = RadarBlipBrightness(tracked);

        // draw the trail first so the marker and label sit on top of it
        if (displayTrails)
            DrawAircraftTrail(backbuffer, tracked, x, y, blip);

        if (displayInfoText)
            DrawAircraftInfo(backbuffer, x, y, tracked, blip);

        if (isEmergencySquawk(tracked.state.squawk)) {
            DrawEmergencyAlert(backbuffer, x, y, tracked);
        } else {
            // base marker fades as its radar return ages between sweep passes
            // (full bright when the sweep is off). The annotation overlays below
            // -- highlight/watchlist/pin reticles, NEW flag -- stay full bright so
            // they remain legible regardless of the fade.
            const uint32_t markerColor = scaleColor(
                displayAltColor ? altitudeColor(tracked.state.baroAltitude)
                                : lgfx::color888(0, 255, 0),
                blip);

            if (displayTriangles)
                DrawAircraftTriangle(backbuffer, x, y, tracked, markerColor);
            else
                backbuffer.fillCircle(x, y, 3, markerColor);
        }

        // special contact (military / special callsign / helicopter): a coloured
        // diamond reticle + tag. All detected offline from the live feed, so they
        // work on any data source. Drawn as an overlay (not a marker replacement)
        // so altitude colour, emergency styling, and the highlight/watchlist/pin
        // rings all still stack on top.
        if (const SpecialAircraft::Class sc = SpecialClassOf(tracked); sc != SpecialAircraft::Class::None) {
            const uint32_t col = SpecialColor(sc);
            backbuffer.drawLine(x,     y - 9, x + 9, y,     col);
            backbuffer.drawLine(x + 9, y,     x,     y + 9, col);
            backbuffer.drawLine(x,     y + 9, x - 9, y,     col);
            backbuffer.drawLine(x - 9, y,     x,     y - 9, col);
            backbuffer.setTextSize(1);
            backbuffer.setTextColor(col);
            backbuffer.drawString(SpecialAircraft::Tag(sc), x + 11, y - 3);
        }

        // fresh logbook catch: a gold "NEW" flag for a never-before-seen type/airline
        if (logbookEnabled && tracked.freshCatch) {
            backbuffer.setTextSize(1);
            backbuffer.setTextColor(lgfx::color888(255, 215, 0));
            backbuffer.drawString("NEW", x + 11, y + 6);
        }

        // overhead: pulsing "look up!" ring when a contact is passing near-overhead
        if (showOverhead && IsOverhead(tracked))
            DrawOverheadAlert(backbuffer, x, y);

        // ring the standout contacts; tags stack up-left to avoid the info text
        if (displayHighlight) {
            const uint32_t HL = lgfx::color888(255, 0, 255); // magenta: not an altitude or emergency color
            int tagY = y - 4;
            auto highlight = [&](const String& tag) {
                backbuffer.drawCircle(x, y, 7, HL);
                backbuffer.setTextSize(1);
                backbuffer.setTextColor(HL);
                backbuffer.drawString(tag, x - (int)backbuffer.textWidth(tag) - 9, tagY);
                tagY -= 9;
            };
            if (icao == nearestIcao) highlight("NEAR");
            if (icao == highestIcao) highlight("HIGH");
            if (icao == fastestIcao) highlight("FAST");
        }

        // watchlisted contact: amber ring + always-on callsign
        if (MatchesWatchlist(tracked)) {
            const uint32_t AMBER = lgfx::color888(255, 140, 0);
            backbuffer.drawCircle(x, y, 8, AMBER);
            String cs = tracked.state.callsign;
            cs.trim();
            if (cs.isEmpty()) { cs = icao; cs.toUpperCase(); }
            backbuffer.setTextSize(1);
            backbuffer.setTextColor(AMBER);
            backbuffer.drawString(cs, x - (int)backbuffer.textWidth(cs) / 2, y - 16);
        }

        // pinned ("tracked") contact: white reticle + always-on callsign
        if (icao == pinnedIcao) {
            const uint32_t PIN = lgfx::color888(255, 255, 255);
            backbuffer.drawCircle(x, y, 9, PIN);
            backbuffer.drawCircle(x, y, 11, PIN);
            String cs = tracked.state.callsign;
            cs.trim();
            if (cs.isEmpty()) { cs = icao; cs.toUpperCase(); }
            backbuffer.setTextSize(1);
            backbuffer.setTextColor(PIN);
            backbuffer.drawString(cs, x - (int)backbuffer.textWidth(cs) / 2, y + 13);
        }
    }
}

void AircraftManager::DrawList(BandCanvas& backbuffer)
{
    constexpr int cx = SCREEN_SIZE_DIV_2;
    backbuffer.setTextSize(1);

    const std::vector<String> order = SortedAircraftByDistance();

    // clamp the scroll offset to the available rows
    const int maxScroll = std::max(0, (int)order.size() - LIST_ROWS);
    if (listScroll > maxScroll) listScroll = maxScroll;
    if (listScroll < 0) listScroll = 0;

    auto centered = [&](const String& s, int y) {
        backbuffer.drawString(s, cx - (int)backbuffer.textWidth(s) / 2, y);
    };

    // centre the fixed-width column block so the rows sit mid-screen on any panel size
    const int lx = (SCREEN_SIZE - LIST_BLOCK_W) / 2;

    backbuffer.setTextColor(lgfx::color888(0, 255, 0));
    centered("AIRCRAFT", 8);
    backbuffer.setTextColor(lgfx::color888(0, 130, 0));
    centered(String(order.size()) + " tracked", 23);

    // rows: callsign / type / altitude, in columns within the centred block (see LIST_COL_*)
    backbuffer.setTextColor(lgfx::color888(0, 200, 0));
    for (int r = 0; r < LIST_ROWS; ++r) {
        const int idx = listScroll + r;
        if (idx >= (int)order.size()) break;
        auto it = trackedAircraft.find(order[idx]);
        if (it == trackedAircraft.end()) continue;
        const TrackedAircraft& t = it->second;

        String cs = t.state.callsign;
        cs.trim();
        if (cs.isEmpty()) { cs = order[idx]; cs.toUpperCase(); }
        const String type = t.typeCode.isEmpty() ? "--" : t.typeCode;
        const String alt = String(lroundf(t.state.baroAltitude * METRES_TO_FEET)) + "ft";

        const int y = LIST_ROW_TOP + r * LIST_ROW_H;
        uint32_t rowColor = lgfx::color888(0, 200, 0);
        if (const SpecialAircraft::Class sc = SpecialClassOf(t); sc != SpecialAircraft::Class::None)
            rowColor = SpecialColor(sc);              // military/special/heli
        if (MatchesWatchlist(t))         rowColor = lgfx::color888(255, 140, 0); // amber
        if (order[idx] == pinnedIcao)    rowColor = lgfx::color888(255, 255, 255); // pin wins
        backbuffer.setTextColor(rowColor);
        backbuffer.drawString(cs,   lx + LIST_COL_CS,   y);
        backbuffer.drawString(type, lx + LIST_COL_TYPE, y);
        backbuffer.drawString(alt,  lx + LIST_COL_ALT,  y);
    }
}

void AircraftManager::DrawStats(BandCanvas& backbuffer)
{
    constexpr int cx = SCREEN_SIZE_DIV_2;
    backbuffer.setTextSize(1);

    auto centered = [&](const String& s, int y) {
        backbuffer.drawString(s, cx - (int)backbuffer.textWidth(s) / 2, y);
    };

    // snapshot over the airborne contacts
    int count = 0;
    String highIcao, fastIcao, nearIcao;
    float maxAlt = -1e30f, maxVel = -1e30f, minD2 = 1e30f;
    for (auto& [icao, t] : trackedAircraft) {
        if (t.state.onGround) continue;
        ++count;
        if (t.state.baroAltitude > maxAlt) { maxAlt = t.state.baroAltitude; highIcao = icao; }
        if (t.state.velocity > maxVel)     { maxVel = t.state.velocity; fastIcao = icao; }
        auto [la, lo] = t.GetDisplayPosition();
        const float dLa = la - (float)lat, dLo = lo - (float)lon;
        const float d2 = dLa * dLa + dLo * dLo;
        if (d2 < minD2) { minD2 = d2; nearIcao = icao; }
    }

    auto label = [&](const String& icao) -> String {
        auto it = trackedAircraft.find(icao);
        if (it == trackedAircraft.end()) return "-";
        String cs = it->second.state.callsign;
        cs.trim();
        if (cs.isEmpty()) { cs = icao; cs.toUpperCase(); }
        return cs;
    };

    backbuffer.setTextColor(lgfx::color888(0, 255, 0));
    centered("STATS", 14);

    int y = 48;
    const int lh = backbuffer.fontHeight() + 10;
    backbuffer.setTextColor(lgfx::color888(0, 200, 0));
    auto line = [&](const String& s) { centered(s, y); y += lh; };

    line(String(count) + " aircraft");
    if (count > 0) {
        line("High " + label(highIcao) + " " + String(lroundf(maxAlt * METRES_TO_FEET)) + "ft");
        line("Fast " + label(fastIcao) + " " + String(lroundf(maxVel * MS_TO_KNOTS)) + "kt");
        float distance = sqrtf(minD2) * 111.0f;
        if (rangeUnit == "mi") distance /= 1.609344f;
        line("Near " + label(nearIcao) + " " + String(distance, distance < 10.0f ? 1 : 0) + rangeUnit);
    }

    // spotting logbook totals (the persistent "lifelist")
    if (logbookEnabled) {
        y += 6;
        backbuffer.setTextColor(lgfx::color888(0, 255, 0));
        line("LIFELIST");
        backbuffer.setTextColor(lgfx::color888(0, 200, 0));
        line(String(logbook.TypeCount()) + " types  " + String(logbook.OperatorCount()) + " airlines");
        line(String(logbook.CountryCount()) + " countries  " + String(logbook.Contacts()) + " seen");
    }

    // live tilt from the on-board IMU (HAS_IMU boards) -- proves the sensor is alive and gives
    // the Stats screen something board-specific. Signed degrees: P = pitch, R = roll.
    if constexpr (variant::HAS_IMU) {
        if (imuValid) {
            y += 6;
            backbuffer.setTextColor(lgfx::color888(0, 200, 0));
            char buf[24];
            snprintf(buf, sizeof(buf), "Tilt P%+d R%+d", (int)lroundf(imuPitch), (int)lroundf(imuRoll));
            line(String(buf));
        }
    }

    // THIS DEVICE -- the config page is at http://<name>.local, so a user who forgot the name
    // can swipe here to read it (the IP is an mDNS fallback). Drawn last, and only as far as it
    // fits above the clock row, so it never collides on the small round C3 screen: the host line
    // is the one you type, so it gets priority; the IP follows only when there's vertical room.
    y += 6;
    const int clockTop = SCREEN_SIZE - 30; // matches DrawClock's y
    if (y + lh <= clockTop) {
        backbuffer.setTextColor(lgfx::color888(0, 255, 0));
        line(DeviceIdentity::Name() + ".local");
    }
    if (y + lh <= clockTop) {
        backbuffer.setTextColor(lgfx::color888(0, 200, 0));
        line(WiFi.localIP().toString());
    }
}

void AircraftManager::DrawScreenIndicator(BandCanvas& backbuffer) const
{
    constexpr int cx = SCREEN_SIZE_DIV_2;
    const int y = SCREEN_SIZE - 16;
    for (int i = 0; i < 3; ++i) {
        const bool active = (i == (int)screen);
        backbuffer.fillCircle(cx - 12 + i * 12, y, active ? 3 : 2,
                              active ? lgfx::color888(0, 255, 0) : lgfx::color888(0, 80, 0));
    }
}

void AircraftManager::DrawClock(BandCanvas& backbuffer) const
{
    const time_t utc = time(nullptr);
    if (utc < 1600000000) return; // NTP not synced yet

    const time_t local = utc + utcOffsetSec;
    struct tm t;
    gmtime_r(&local, &t);
    char buf[6];
    snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);

    backbuffer.setTextSize(1);
    backbuffer.setTextColor(lgfx::color888(0, 170, 0));
    backbuffer.drawString(buf, SCREEN_SIZE_DIV_2 - (int)backbuffer.textWidth(buf) / 2, SCREEN_SIZE - 30);
}

void AircraftManager::UpdateBrightness()
{
    const unsigned long now = millis();
    if (now - lastBrightnessCheck < 20000) return; // re-evaluate every 20s
    lastBrightnessCheck = now;

    uint8_t target = configuredBrightness;

    const time_t utc = time(nullptr);
    const bool synced = utc > 1600000000; // NTP has set the clock (>~2020)
    const bool night = synced && isNightNow(lat, lon, utc);
    if (autoDim && night) {
        target = configuredBrightness / 5; // ~20% of day level at night
        if (target < 10) target = 10;
    }

    if (target != currentBrightness) {
        tft.setBrightness(target);
        currentBrightness = target;
        Serial.printf("[dim] brightness -> %u (%s)\n",
                      target, target < configuredBrightness ? "night" : "day");
    }
}

std::vector<String> AircraftManager::SortedAircraftByDistance()
{
    std::vector<std::pair<float, String>> v;
    for (auto& [icao, t] : trackedAircraft) {
        if (t.state.onGround) continue;
        auto [la, lo] = t.GetDisplayPosition();
        const float dLa = la - (float)lat, dLo = lo - (float)lon;
        v.push_back({ dLa * dLa + dLo * dLo, icao });
    }
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    std::vector<String> out;
    out.reserve(v.size());
    for (auto& p : v) out.push_back(p.second);
    return out;
}

void AircraftManager::DrawRadarCircles(BandCanvas& backbuffer) const
{
    constexpr int CENTRE = SCREEN_SIZE_DIV_2 - 1;
    constexpr int OUTER = SCREEN_SIZE_DIV_2 - 1;

    backbuffer.drawCircle(CENTRE, CENTRE, OUTER, lgfx::color888(0, 200, 0));
    backbuffer.drawCircle(CENTRE, CENTRE, (OUTER / 3) * 2, lgfx::color888(0, 64, 0));
    backbuffer.drawCircle(CENTRE, CENTRE, OUTER / 3, lgfx::color888(0, 32, 0));

    backbuffer.setTextSize(1);

    // range-ring distance labels, stacked just right of the vertical centre line
    backbuffer.setTextColor(lgfx::color888(0, 110, 0));
    const int ringPx[3] = { OUTER, (OUTER / 3) * 2, OUTER / 3 };
    const float ringFrac[3] = { 1.0f, 2.0f / 3.0f, 1.0f / 3.0f };
    const int inset[3] = { 14, 3, 3 }; // push the outer label down off the bezel/N
    for (int i = 0; i < 3; ++i) {
        const float value = rangeRadiusDisplay * ringFrac[i];
        String label = String(value, value < 10.0f ? 1 : 0);
        if (i == 0) label += rangeUnit; // unit on the outer ring only
        backbuffer.drawString(label, CENTRE + 4, CENTRE - ringPx[i] + inset[i]);
    }

    // compass rose at the bezel
    backbuffer.setTextColor(lgfx::color888(0, 150, 0));
    auto compass = [&](const char* c, int x, int y) {
        backbuffer.drawString(c, x - (int)backbuffer.textWidth(c) / 2, y);
    };
    compass("N", CENTRE, 2);
    compass("S", CENTRE, SCREEN_SIZE - 10);
    compass("E", SCREEN_SIZE - 8, CENTRE - 3);
    compass("W", 7, CENTRE - 3);
}

std::pair<int, int> AircraftManager::ProjectCoordinateToScreen(float predLat, float predLon) const
{
    const float dLon = predLon - lon;
    const float dLat = predLat - lat;

    const float normLon = (dLon + radLon) / (2.0f * radLon);
    const float normLat = (dLat + radLat) / (2.0f * radLat);

    const int x = static_cast<int>(normLon * SCREEN_SIZE);
    const int y = static_cast<int>(SCREEN_SIZE - (normLat * SCREEN_SIZE));

    return { x, y };
}

void AircraftManager::DrawAircraftInfo(BandCanvas& backbuffer, int x, int y, const TrackedAircraft& tracked, float brightness) const
{
    const int lineHeight = tft.fontHeight() + 1;

    backbuffer.setTextSize(1);
    // fades with the rest of the contact; scaleColor's brightness floor keeps it legible
    backbuffer.setTextColor(scaleColor(lgfx::color888(0, 128, 0), brightness));

    // Stack only the enabled fields; a field that formats to "" (e.g. no squawk
    // reported) is skipped so it doesn't leave a blank gap between lines.
    int line = 0;
    for (size_t i = 0; i < AIRCRAFT_INFO_FIELD_COUNT; ++i) {
        if (i >= infoFieldEnabled.size() || !infoFieldEnabled[i])
            continue;

        const String text = AIRCRAFT_INFO_FIELDS[i].format(tracked);
        if (text.isEmpty())
            continue;

        backbuffer.drawString(text, x + 5, y + 5 + lineHeight * line);
        ++line;
    }
}

bool AircraftManager::AircraftLabelBox(const TrackedAircraft& tracked, int x, int y,
                                       int& bx, int& by, int& bw, int& bh) const
{
    if (!displayInfoText) return false;
    tft.setTextSize(1);
    const int lineHeight = tft.fontHeight() + 1;
    // Same field walk as DrawAircraftInfo: count the non-empty enabled lines and the widest.
    int lines = 0, maxW = 0;
    for (size_t i = 0; i < AIRCRAFT_INFO_FIELD_COUNT; ++i) {
        if (i >= infoFieldEnabled.size() || !infoFieldEnabled[i])
            continue;
        const String text = AIRCRAFT_INFO_FIELDS[i].format(tracked);
        if (text.isEmpty())
            continue;
        const int w = tft.textWidth(text);
        if (w > maxW) maxW = w;
        ++lines;
    }
    if (lines == 0) return false;
    bx = x + 5;                 // first line drawn at x+5, y+5; lines stack by lineHeight
    by = y + 5;
    bw = maxW;
    bh = lineHeight * lines;
    return true;
}

void AircraftManager::DrawAircraftTriangle(BandCanvas& backbuffer, int x, int y, const TrackedAircraft& tracked, uint32_t color) const
{
    // Type-aware marker, keyed off the ADS-B emitter category (normalised to
    // OpenSky's numbering for both the cloud and local feeds). Heading-less types
    // (rotorcraft, balloons) get a fixed glyph; everything else is a heading dart
    // whose size grows with the aircraft's weight class so a heavy reads
    // differently from a light single at a glance.
    const int cat = tracked.state.category;

    // rotorcraft: a hub with two crossed rotor blades. Non-directional, since
    // helicopters routinely hover and yaw independently of their ground track.
    if (cat == 8) {
        backbuffer.drawLine(x - 5, y - 5, x + 5, y + 5, color);
        backbuffer.drawLine(x - 5, y + 5, x + 5, y - 5, color);
        backbuffer.fillCircle(x, y, 2, color);
        return;
    }

    // lighter-than-air (balloon / airship): a simple ring, also non-directional.
    if (cat == 10) {
        backbuffer.drawCircle(x, y, 4, color);
        return;
    }

    // heading unit vector (forward) and its perpendicular (right "wing")
    const float dx = std::sin(radians(tracked.state.trueTrack));
    const float dy = -std::cos(radians(tracked.state.trueTrack));
    const float px = -dy;
    const float py = dx;

    // a dart pointing along the heading: tip ahead of the point, base behind it
    auto dart = [&](float tip, float base, float halfWidth) {
        const float tipX  = x + dx * tip,                 tipY  = y + dy * tip;
        const float leftX = x - dx * base + px * halfWidth, leftY = y - dy * base + py * halfWidth;
        const float rightX= x - dx * base - px * halfWidth, rightY= y - dy * base - py * halfWidth;
        backbuffer.fillTriangle(tipX, tipY, leftX, leftY, rightX, rightY, color);
    };

    // glider: a long, slender dart.
    if (cat == 9) {
        dart(9.0f, 2.0f, 1.0f);
        return;
    }

    // heavy / large jets (Large, High-vortex large, Heavy): a bigger dart with a
    // stub cross-wing so the airliners stand out from light traffic.
    const bool heavy = (cat == 4 || cat == 5 || cat == 6);
    if (heavy) {
        dart(8.0f, 4.0f, 2.5f);
        backbuffer.drawLine(x + px * 4.0f, y + py * 4.0f, x - px * 4.0f, y - py * 4.0f, color);
        return;
    }

    // everything else (light, small, high-performance, unknown): the standard dart
    dart(6.0f, 3.0f, 1.5f);
}

void AircraftManager::DrawEmergencyAlert(BandCanvas& backbuffer, int x, int y, const TrackedAircraft& tracked) const
{
    // expanding, fading "ping" ring to draw the eye to the emergency
    const float phase = (millis() % 900) / 900.0f;   // 0..1, ~0.9s period
    const int ringRadius = 4 + static_cast<int>(phase * 16.0f);
    const uint8_t ringBrightness = static_cast<uint8_t>((1.0f - phase) * 255.0f);
    backbuffer.drawCircle(x, y, ringRadius, lgfx::color888(ringBrightness, 0, 0));

    // steady red marker
    backbuffer.fillCircle(x, y, 3, lgfx::color888(255, 0, 0));

    // always-visible red label: squawk code + what it means
    const char* descriptor = "EMERG";
    if (tracked.state.squawk == "7500")      descriptor = "HIJACK";
    else if (tracked.state.squawk == "7600") descriptor = "NORDO";

    backbuffer.setTextSize(1);
    backbuffer.setTextColor(lgfx::color888(255, 0, 0));
    backbuffer.drawString(tracked.state.squawk + " " + descriptor, x + 6, y - 10);
}

void AircraftManager::DrawOverheadAlert(BandCanvas& backbuffer, int x, int y) const
{
    // expanding, fading cyan "ping" plus a steady "LOOK UP" label, to pull your
    // eye to the sky as the contact passes near-overhead
    const float phase = (millis() % 1200) / 1200.0f;            // 0..1, ~1.2s period
    const int ringRadius = 6 + static_cast<int>(phase * 18.0f);
    const uint8_t b = static_cast<uint8_t>((1.0f - phase) * 255.0f);
    backbuffer.drawCircle(x, y, ringRadius, lgfx::color888(0, b, b));

    backbuffer.setTextSize(1);
    backbuffer.setTextColor(lgfx::color888(0, 255, 255));
    const char* label = "LOOK UP";
    backbuffer.drawString(label, x - (int)backbuffer.textWidth(label) / 2, y - 18);
}

void AircraftManager::DrawAircraftTrail(BandCanvas& backbuffer, const TrackedAircraft& tracked, int headX, int headY, float brightness) const
{
    const int n = tracked.TrailSize();
    if (n < 1) return;

    int prevX = 0, prevY = 0;
    bool havePrev = false;
    for (int i = 0; i < n; ++i) {
        auto [lat, lon] = tracked.TrailPointAt(i);
        auto [x, y] = ProjectCoordinateToScreen(lat, lon);

        if (havePrev) {
            // Fade from dim (oldest) to bright (newest). The floor of 40 keeps
            // the tail above the 8-bit display's green quantization step so it
            // doesn't vanish. brightness scales the whole trail with the blip so
            // the contact fades as a unit when paint-and-fade is on.
            const uint8_t g = static_cast<uint8_t>((40 + (180 * i) / n) * brightness);
            backbuffer.drawLine(prevX, prevY, x, y, lgfx::color888(0, g, 0));
        }

        prevX = x;
        prevY = y;
        havePrev = true;
    }

    // connect the most recent sample to the live aircraft position so the trail
    // stays attached to the marker between samples
    backbuffer.drawLine(prevX, prevY, headX, headY, lgfx::color888(0, (uint8_t)(220 * brightness), 0));
}

void AircraftManager::HandleTouch()
{
    int32_t tx = 0, ty = 0;
    bool touched;

    if constexpr (variant::SERIALIZE_TOUCH_BUS) {
        // Single-core (C3): serialize touch I2C with network TLS via the shared HTTP request
        // lock. A touch I2C transfer that overlaps a TLS handshake wedges the CST816 off the
        // bus -- the v4 regression (touch worked in v2, which had no concurrent network task).
        // The HTTP client holds this mutex for the full duration of every GET/POST on any task,
        // so taking it here guarantees no I2C transfer runs concurrently with a request.
        //
        // We gate this way ONLY on the radar view. While a detail card is open the radar isn't
        // shown, the card's own enrichment (metadata/route/photo) holds the bus for several
        // seconds, and gating would make the close tap wait that whole time (up to ~30 s when a
        // lookup times out). So on the card view we poll every loop regardless -- closing stays
        // instant. The brief overlap risk there is acceptable: the card view has no sweep to
        // protect, the enrichment is bounded and one-shot, and the controller wakes on the next
        // touch. The earlier fetchInFlight/enrichInFlight flags did NOT work for this -- they
        // don't actually span the on-task TLS window; the mutex does.
        const bool gateOnBus = !inDetail;
        const bool gotBus = http.TryAcquireBus();
        if (gateOnBus && !gotBus)
            return; // radar view: a request is mid-flight, skip this poll to avoid the wedge

        touched = tft.getTouch(&tx, &ty);

        if (gotBus)
            http.ReleaseBus();
    } else {
        // Dual-core (S3): touch sits on its own I2C bus and the network runs over WiFi on a
        // separate core, so there is no touch/TLS wedge to prevent. Gating the poll on the HTTP
        // mutex here would silently drop taps whenever always-on enrichment held the bus -- which
        // it does constantly -- so a blip tap could take many tries to land. Poll every loop.
        touched = tft.getTouch(&tx, &ty);
    }

    const unsigned long now = millis();
    if (touched) {
        lastTouchActivityMs = now; // proof the controller is alive
        if (!wasTouched) { touchStartX = tx; touchStartY = ty; } // press edge
        touchLastX = tx;
        touchLastY = ty;
    } else if (wasTouched) {
        // release: classify the stroke as a tap or a 4-way swipe from its delta
        const int dx = touchLastX - touchStartX;
        const int dy = touchLastY - touchStartY;
        const int adx = abs(dx), ady = abs(dy);
        constexpr int SWIPE_MIN = 40;

        if (adx < SWIPE_MIN && ady < SWIPE_MIN)
            HandleTap(touchStartX, touchStartY);
        else if (adx >= ady)
            HandleSwipe(dx > 0 ? Swipe::Right : Swipe::Left);
        else
            HandleSwipe(dy > 0 ? Swipe::Down : Swipe::Up);
    }

    wasTouched = touched;
}

void AircraftManager::ExitDetail()
{
    inDetail = false;
    detailPage = 0;
    // Reclaim the ~15 KB photo sprite. Holding it allocated is what tips the heap
    // below what an adsbdb/photo TLS handshake needs (see ENRICH_TLS_HEAP_FLOOR);
    // it's lazily recreated the next time a photo decodes. Clear photoIcao so the
    // photo is re-fetched if the same aircraft is opened again.
    if (photoSprite.getBuffer() != nullptr)
        photoSprite.deleteSprite();
    photoReady = false;
    photoResolved = false;
    photoIcao = "";
}

void AircraftManager::HandleTap(int tx, int ty)
{
    // detail card: flip the photo page to the data page, else close
    if (inDetail) {
        const bool hasPhoto = photoReady && photoIcao == selectedIcao && photoSprite.getBuffer() != nullptr;
        if (hasPhoto && detailPage == 0)
            detailPage = 1;
        else ExitDetail();
        return;
    }

    if (screen == Screen::Radar) {
        // Pick the contact under the finger. Markers are tiny (~3 px) and a fingertip lands a
        // couple of mm off -- worse for the low/edge contacts -- so the hit region is generous:
        // the marker within TAP_RADIUS, OR the info label (drawn below-right, which the eye reads
        // as "the aircraft"). Dense areas stack several contacts within a finger-width, so we
        // gather every candidate and, on repeated taps at ~the same spot, cycle through them
        // (nearest marker first) -- otherwise a buried contact is unreachable.
        constexpr int TAP_RADIUS = 28;
        std::vector<std::pair<int, String>> cands; // (dist2 to marker, icao)
        for (auto& [icao, tracked] : trackedAircraft) {
            if (tracked.state.onGround) continue;
            // hit-test where the blip is drawn (latched under paint-and-fade), not its live
            // position -- otherwise taps miss a blip paused mid-sweep waiting for the next pass
            auto [la, lo] = RadarBlipPosition(tracked);
            auto [x, y] = ProjectCoordinateToScreen(la, lo);
            const int dx = x - tx, dy = y - ty;
            const int dist2 = dx * dx + dy * dy;
            bool hit = dist2 <= TAP_RADIUS * TAP_RADIUS;
            if (!hit) {
                int bx, by, bw, bh;
                if (AircraftLabelBox(tracked, x, y, bx, by, bw, bh))
                    hit = (tx >= bx && tx <= bx + bw && ty >= by && ty <= by + bh);
            }
            if (hit) cands.emplace_back(dist2, icao);
        }
        if (cands.empty()) {
            pinnedIcao = ""; // tap on empty radar clears the pin
            lastTapX = lastTapY = -1000;
        } else {
            std::sort(cands.begin(), cands.end()); // nearest marker first
            const int ddx = tx - lastTapX, ddy = ty - lastTapY;
            const bool sameSpot = lastTapX > -1000 && (ddx * ddx + ddy * ddy) <= 18 * 18;
            tapCycleIndex = sameSpot ? (tapCycleIndex + 1) % (int)cands.size() : 0;
            lastTapX = tx;
            lastTapY = ty;
            selectedIcao = cands[tapCycleIndex].second;
            inDetail = true;
            detailPage = 0;
        }
    } else if (screen == Screen::List) {
        // map the tapped row to an aircraft (same layout as DrawList)
        if (ty >= LIST_ROW_TOP) {
            const int r = (ty - LIST_ROW_TOP) / LIST_ROW_H;
            if (r >= 0 && r < LIST_ROWS) {
                const std::vector<String> order = SortedAircraftByDistance();
                const int idx = listScroll + r;
                if (idx >= 0 && idx < (int)order.size()) {
                    selectedIcao = order[idx];
                    inDetail = true;
                    detailPage = 0;
                }
            }
        }
    }
    // Stats screen: tap does nothing
}

void AircraftManager::HandleSwipe(Swipe swipe)
{
    // detail card: swipe up pins ("tracks") the aircraft and returns to the
    // radar; any other swipe just closes the card
    if (inDetail) {
        if (swipe == Swipe::Up) {
            pinnedIcao = (pinnedIcao == selectedIcao) ? "" : selectedIcao;
            screen = Screen::Radar;
        }
        ExitDetail();
        return;
    }

    // list view: vertical swipe scrolls
    if (screen == Screen::List && (swipe == Swipe::Up || swipe == Swipe::Down)) {
        listScroll += (swipe == Swipe::Up) ? LIST_ROWS - 1 : -(LIST_ROWS - 1);
        if (listScroll < 0) listScroll = 0; // upper bound clamped in DrawList
        return;
    }

    // horizontal swipe cycles the top-level screens (left = next, right = prev)
    if (swipe == Swipe::Left)  screen = (Screen)(((int)screen + 1) % 3);
    if (swipe == Swipe::Right) screen = (Screen)(((int)screen + 2) % 3);
}

void AircraftManager::ProcessDetailLookups()
{
    if (!inDetail)
        return;

    auto it = trackedAircraft.find(selectedIcao);
    if (it == trackedAircraft.end())
        return;
    TrackedAircraft& tracked = it->second;

    // One enrichment request is outstanding at a time (shared with the radar
    // metadata path); wait for it to land before issuing the next step.
    if (enrichInFlight)
        return;

    // same TLS-heap guard as the radar path: a handshake with too little contiguous
    // heap only fails and churns, so defer the detail lookups until heap recovers.
    if (ESP.getMaxAllocHeap() < ENRICH_TLS_HEAP_FLOOR)
        return;

    // 1. metadata (type/operator/registration + photo URL). Queued here -- not only
    //    in the throttled radar path -- so the card is complete even when the
    //    enrichment fields are disabled. The photo step below needs the photoUrl it
    //    resolves, so wait for it (Fetching) before moving on.
    if (tracked.metadataState == TrackedAircraft::MetadataState::NotFetched) {
        tracked.metadataState = TrackedAircraft::MetadataState::Fetching;
        RequestMetadata(selectedIcao);
        return;
    }
    if (tracked.metadataState == TrackedAircraft::MetadataState::Fetching)
        return;

    // 2. route, once per callsign
    String callsign = tracked.state.callsign;
    callsign.trim();
    if (!callsign.isEmpty() && tracked.routeCallsign != callsign) {
        RequestRoute(selectedIcao, callsign);
        return;
    }

    // 3. photo, once per aircraft
    if (photoIcao != selectedIcao) {
        photoIcao = selectedIcao; // mark attempted regardless of outcome (no retry)
        photoReady = false;
        if (!tracked.photoUrl.isEmpty()) {
            photoResolved = false; // a fetch is coming; the card shows "Loading..." until it lands
            RequestPhoto(selectedIcao, tracked.photoUrl);
        } else {
            photoResolved = true;  // adsbdb has none: resolved now, so the card shows the silhouette
        }
    }
}

void AircraftManager::StartEnrichTask()
{
    if (enrichTaskHandle != nullptr)
        return; // already running; survive Initialise() being re-run on a config reload

    // Depth 1: only ever one enrichment outstanding (gated by enrichInFlight), so a
    // single slot for the request and one for the result is enough.
    enrichRequestQueue = xQueueCreate(1, sizeof(EnrichRequest*));
    enrichResultQueue  = xQueueCreate(1, sizeof(EnrichResult*));

    // Same workload class as the fetch task (HTTPS GET + small JSON decode, plus a
    // ~10 KB photo body carried back in the result), so the same 12 KB stack and
    // priority 1. It spends almost all its life blocked on the request queue.
    // Pinned to core 0 (the WiFi core) for the same reason as the fetch task: keep network
    // work off the panel-driving loop on core 1 (S3); no-op on the single-core C3.
    xTaskCreatePinnedToCore(EnrichTaskTrampoline, "enrich", 12288, this, 1, &enrichTaskHandle, 0);
}

void AircraftManager::EnrichTaskTrampoline(void* arg)
{
    static_cast<AircraftManager*>(arg)->RunEnrichTask();
}

void AircraftManager::RunEnrichTask()
{
    for (;;) {
        // block until the loop requests an enrichment
        EnrichRequest* req = nullptr;
        if (xQueueReceive(enrichRequestQueue, &req, portMAX_DELAY) != pdTRUE || req == nullptr)
            continue;

        EnrichResult* res = nullptr;
        switch (req->kind) {
            case EnrichKind::Metadata: res = fetchAircraftMetadata(http, req->icao24); break;
            case EnrichKind::Route:    res = fetchRoute(http, req->callsign);          break;
            case EnrichKind::Photo:    res = fetchPhoto(http, req->url);               break;
        }

        if (res != nullptr) {
            res->kind = req->kind;
            res->icao24 = req->icao24; // who the result applies to (route req carries it too)

            // hand the result back; the loop consumed the previous one before
            // requesting again, so the depth-1 queue always has room
            if (xQueueSend(enrichResultQueue, &res, 0) != pdTRUE)
                delete res;
        }

        delete req;
    }
}

void AircraftManager::RequestMetadata(const String& icao24)
{
    if (enqueueEnrich(enrichRequestQueue, new EnrichRequest{ EnrichKind::Metadata, icao24, "", "" }))
        enrichInFlight = true;
}

void AircraftManager::RequestRoute(const String& icao24, const String& callsign)
{
    if (enqueueEnrich(enrichRequestQueue, new EnrichRequest{ EnrichKind::Route, icao24, callsign, "" }))
        enrichInFlight = true;
}

void AircraftManager::RequestPhoto(const String& icao24, const String& url)
{
    if (enqueueEnrich(enrichRequestQueue, new EnrichRequest{ EnrichKind::Photo, icao24, "", url }))
        enrichInFlight = true;
}

void AircraftManager::ConsumeEnrichResults()
{
    if (enrichResultQueue == nullptr)
        return;

    EnrichResult* res = nullptr;
    if (xQueueReceive(enrichResultQueue, &res, 0) != pdTRUE)
        return; // nothing ready

    enrichInFlight = false;

    // The aircraft may have left range while the lookup was outstanding. Metadata
    // and route results target a map entry (gone -> nothing to apply); the photo is
    // keyed to photoIcao instead, since the sprite is shared, not per-entry.
    auto it = trackedAircraft.find(res->icao24);

    switch (res->kind) {
        case EnrichKind::Metadata:
            if (it != trackedAircraft.end()) {
                if (res->definitive) {
                    TrackedAircraft& t = it->second;
                    t.metadataState = TrackedAircraft::MetadataState::Fetched;
                    t.typeCode = res->typeCode;
                    t.typeName = res->typeName;
                    t.operatorName = res->operatorName;
                    t.registration = res->registration;
                    t.photoUrl = res->photoUrl;

                    // add the type / airline to the lifelist; a brand-new entry marks
                    // this a "fresh catch". On the loop -- the logbook is loop-owned.
                    if (logbookEnabled) {
                        const bool newType = logbook.NoteType(t.typeCode);
                        const bool newOperator = logbook.NoteOperator(t.operatorName);
                        if (newType || newOperator)
                            t.freshCatch = true;
                    }
                } else {
                    // transient network failure: allow a later retry, but not before
                    // a cooldown -- otherwise this same aircraft is re-picked every
                    // cycle and hammers the (often heap-starved) TLS path in a storm.
                    constexpr unsigned long METADATA_RETRY_COOLDOWN = 30000;
                    it->second.metadataState = TrackedAircraft::MetadataState::NotFetched;
                    it->second.metadataRetryAfter = millis() + METADATA_RETRY_COOLDOWN;
                }
            }
            break;

        case EnrichKind::Route:
            // only a definitive answer is recorded; a transient failure leaves
            // routeCallsign unchanged so the detail path retries it
            if (it != trackedAircraft.end() && res->definitive) {
                TrackedAircraft& t = it->second;
                t.routeCallsign = res->routeCallsign;
                t.routeOrigin = res->routeOrigin;
                t.routeDest = res->routeDest;
            }
            break;

        case EnrichKind::Photo:
            // decode here, on the loop, so the photo sprite is only ever touched by
            // one task. Only apply it if this photo is still wanted: ExitDetail()
            // clears photoIcao (and frees the sprite) when the card closes, so a
            // late-arriving result must not re-allocate the ~15 KB sprite behind it.
            if (photoIcao == res->icao24) {
                photoResolved = true; // this aircraft's photo attempt is done (decoded below, or not)
                if (res->photoFetched && res->photoBytes.length() > 0) {
                    if (photoSprite.getBuffer() == nullptr) {
                        // PSRAM boards keep the photo off the scarce internal heap (where WiFi/TLS
                        // and the JPEG decoder also live), so decoding a card doesn't fragment it.
                        if constexpr (!variant::BANDED_RENDER)
                            photoSprite.setPsram(true);
                        photoSprite.setColorDepth(8);
                        photoSprite.createSprite(PHOTO_W, PHOTO_H);
                    }
                    photoSprite.fillScreen(lgfx::color888(0, 0, 0));
                    photoReady = photoSprite.drawJpg((const uint8_t*)res->photoBytes.c_str(),
                                                     res->photoBytes.length(), 0, 0, PHOTO_W, PHOTO_H);
                }
            }
            break;
    }

    delete res;
}

bool AircraftManager::MatchesWatchlist(const TrackedAircraft& tracked) const
{
    if (watchlist.empty())
        return false;

    String callsign = tracked.state.callsign; callsign.trim(); callsign.toLowerCase();
    String icao = tracked.state.icao24; icao.toLowerCase();
    String reg = tracked.registration; reg.toLowerCase();
    String type = tracked.typeCode; type.toLowerCase();

    for (const String& w : watchlist) {
        if (callsign.startsWith(w) && !callsign.isEmpty()) return true;
        if (icao.startsWith(w) && !icao.isEmpty())         return true;
        if (reg.startsWith(w) && !reg.isEmpty())           return true;
        if (type.startsWith(w) && !type.isEmpty())         return true;
    }
    return false;
}

bool AircraftManager::IsOverhead(const TrackedAircraft& tracked) const
{
    if (tracked.state.onGround)
        return false;
    auto [aLat, aLon] = tracked.GetDisplayPosition();
    const float dLatKm = (aLat - (float)lat) * 111.0f;
    const float dLonKm = (aLon - (float)lon) * 111.0f * cosf(radians((float)lat));
    return sqrtf(dLatKm * dLatKm + dLonKm * dLonKm) <= (float)overheadKm;
}

void AircraftManager::ProcessAlerts()
{
    if (ntfyTopic.isEmpty())
        return;
    const bool flyoverEnabled = !watchlist.empty() || alertMilitary;
    if (!flyoverEnabled && !alertOverhead)
        return;

    const unsigned long now = millis();
    if (now - lastNotifyCheck < 2000) // one blocking POST at a time, spaced out
        return;
    lastNotifyCheck = now;

    for (auto& [icao, tracked] : trackedAircraft) {
        if (tracked.state.onGround) continue;

        // overhead "look up" alert -- one-shot per tracking session
        if (alertOverhead && !tracked.overheadNotified && IsOverhead(tracked)) {
            SendOverheadNotification(tracked);
            tracked.overheadNotified = true;
            return; // at most one notification per tick
        }

        // watchlist / military flyover alert
        if (flyoverEnabled && !tracked.watchNotified) {
            const bool military = alertMilitary && SpecialAircraft::IsMilitary(tracked.state.icao24);
            if (military || MatchesWatchlist(tracked)) {
                SendFlyoverNotification(tracked, military);
                tracked.watchNotified = true;
                return;
            }
        }
    }
}

void AircraftManager::SendFlyoverNotification(const TrackedAircraft& tracked, bool military)
{
    String callsign = tracked.state.callsign;
    callsign.trim();
    if (callsign.isEmpty()) { callsign = tracked.state.icao24; callsign.toUpperCase(); }

    String body = military ? "MILITARY " + callsign : callsign;
    if (!tracked.typeCode.isEmpty())     body += " (" + tracked.typeCode + ")";
    if (!tracked.operatorName.isEmpty()) body += " " + tracked.operatorName;
    body += " at " + String(lroundf(tracked.state.baroAltitude * METRES_TO_FEET)) + " ft";

    const HttpResult result = http.Post(
        "https://ntfy.sh/" + ntfyTopic, body,
        { { "Title", military ? "Blipscope military flyover" : "Blipscope flyover" },
          { "Tags", military ? "rotating_light" : "airplane" } });

    Serial.printf("[ntfy] %s%s -> %s\n", military ? "[MIL] " : "", callsign.c_str(),
                  result.success ? "sent" : result.errorMessage.c_str());
}

void AircraftManager::SendOverheadNotification(const TrackedAircraft& tracked)
{
    String callsign = tracked.state.callsign;
    callsign.trim();
    if (callsign.isEmpty()) { callsign = tracked.state.icao24; callsign.toUpperCase(); }

    String body = callsign + " passing overhead";
    if (!tracked.typeCode.isEmpty()) body += " (" + tracked.typeCode + ")";
    body += " at " + String(lroundf(tracked.state.baroAltitude * METRES_TO_FEET)) + " ft";

    const HttpResult result = http.Post(
        "https://ntfy.sh/" + ntfyTopic, body,
        { { "Title", "Blipscope overhead - look up!" }, { "Tags", "eyes" } });

    Serial.printf("[ntfy] [OVH] %s -> %s\n", callsign.c_str(),
                  result.success ? "sent" : result.errorMessage.c_str());
}

void AircraftManager::PublishMqttState()
{
    if (!mqtt.Connected())
        return;

    int count = 0;
    String highIcao, fastIcao, nearIcao, milIcao, ovhIcao;
    float maxAlt = -1e30f, maxVel = -1e30f, minD2 = 1e30f;
    bool anyMil = false, anyOvh = false;
    const bool overheadActive = showOverhead || alertOverhead;

    for (auto& [icao, t] : trackedAircraft) {
        if (t.state.onGround) continue;
        ++count;
        if (t.state.baroAltitude > maxAlt) { maxAlt = t.state.baroAltitude; highIcao = icao; }
        if (t.state.velocity > maxVel)     { maxVel = t.state.velocity; fastIcao = icao; }
        auto [la, lo] = t.GetDisplayPosition();
        const float dLa = la - (float)lat, dLo = lo - (float)lon;
        const float d2 = dLa * dLa + dLo * dLo;
        if (d2 < minD2) { minD2 = d2; nearIcao = icao; }
        if (SpecialAircraft::IsMilitary(t.state.icao24)) { anyMil = true; if (milIcao.isEmpty()) milIcao = icao; }
        if (overheadActive && IsOverhead(t))             { anyOvh = true; if (ovhIcao.isEmpty()) ovhIcao = icao; }
    }

    auto callsignOf = [&](const String& icao) -> String {
        auto it = trackedAircraft.find(icao);
        if (it == trackedAircraft.end()) return "";
        String cs = it->second.state.callsign; cs.trim();
        if (cs.isEmpty()) { cs = icao; cs.toUpperCase(); }
        return cs;
    };

    JsonDocument doc;
    doc["count"] = count;
    doc["military"] = anyMil;
    doc["overhead"] = anyOvh;

    if (count > 0) {
        auto it = trackedAircraft.find(nearIcao);
        if (it != trackedAircraft.end()) {
            const TrackedAircraft& n = it->second;
            auto [aLat, aLon] = n.GetDisplayPosition();
            const float dLatKm = (aLat - (float)lat) * 111.0f;
            const float dLonKm = (aLon - (float)lon) * 111.0f * cosf(radians((float)lat));
            float bearing = degrees(atan2f(dLonKm, dLatKm));
            if (bearing < 0.0f) bearing += 360.0f;

            JsonObject nearest = doc["nearest"].to<JsonObject>();
            nearest["callsign"] = callsignOf(nearIcao);
            nearest["type"] = n.typeCode;
            nearest["registration"] = n.registration;
            nearest["operator"] = n.operatorName;
            nearest["dist_km"] = roundf(sqrtf(minD2) * 111.0f * 10.0f) / 10.0f;
            nearest["alt_m"] = (int)lroundf(n.state.baroAltitude);
            nearest["speed_ms"] = (int)lroundf(n.state.velocity);
            nearest["bearing"] = (int)lroundf(bearing);
        }
        doc["highest_callsign"] = callsignOf(highIcao);
        doc["highest_alt_m"] = (int)lroundf(maxAlt);
        doc["fastest_callsign"] = callsignOf(fastIcao);
        doc["fastest_speed_ms"] = (int)lroundf(maxVel);
    }
    if (anyMil) doc["military_callsign"] = callsignOf(milIcao);
    if (anyOvh) doc["overhead_callsign"] = callsignOf(ovhIcao);

    String payload;
    serializeJson(doc, payload);
    mqtt.Publish(mqttBase + "/summary", payload, true);
}

void AircraftManager::PublishMqttDiscovery()
{
    const String id = DeviceIdentity::Name(); // e.g. "Blipscope-A1B2C3"
    String uid = id;
    uid.toLowerCase();
    const String summaryTopic = mqttBase + "/summary";
    const String statusTopic = mqttBase + "/status";

    // common fields shared by every entity: state/availability topics, unique id,
    // and the parent device so HA groups them under one "Blipscope" device.
    auto base = [&](JsonDocument& doc, const char* name, const String& key) {
        doc["name"] = name;
        doc["state_topic"] = summaryTopic;
        doc["availability_topic"] = statusTopic;
        doc["unique_id"] = uid + "_" + key;
        JsonObject dev = doc["device"].to<JsonObject>();
        dev["identifiers"].to<JsonArray>().add(uid);
        dev["name"] = id;
        dev["manufacturer"] = "Valar Systems";
        dev["model"] = "Blipscope";
    };
    auto send = [&](const char* component, const String& key, JsonDocument& doc) {
        String payload;
        serializeJson(doc, payload);
        mqtt.Publish(String("homeassistant/") + component + "/" + uid + "_" + key + "/config", payload, true);
    };

    {
        JsonDocument d; base(d, "Aircraft in range", "count");
        d["value_template"] = "{{ value_json.count }}";
        d["unit_of_measurement"] = "aircraft";
        d["icon"] = "mdi:airplane";
        send("sensor", "count", d);
    }
    {
        JsonDocument d; base(d, "Nearest aircraft", "nearest");
        d["value_template"] = "{{ value_json.nearest.callsign if value_json.nearest is defined else 'none' }}";
        d["json_attributes_topic"] = summaryTopic;
        d["json_attributes_template"] = "{{ value_json.nearest | tojson }}";
        d["icon"] = "mdi:airplane-search";
        send("sensor", "nearest", d);
    }
    {
        JsonDocument d; base(d, "Aircraft overhead", "overhead");
        d["value_template"] = "{{ 'ON' if value_json.overhead else 'OFF' }}";
        d["payload_on"] = "ON";
        d["payload_off"] = "OFF";
        d["device_class"] = "occupancy";
        send("binary_sensor", "overhead", d);
    }
    {
        JsonDocument d; base(d, "Military aircraft in range", "military");
        d["value_template"] = "{{ 'ON' if value_json.military else 'OFF' }}";
        d["payload_on"] = "ON";
        d["payload_off"] = "OFF";
        d["icon"] = "mdi:shield-airplane";
        send("binary_sensor", "military", d);
    }

    Serial.printf("[mqtt] published HA discovery for %s\n", uid.c_str());
}

void AircraftManager::DrawAircraftSilhouette(BandCanvas& g, int cx, int cy, const TrackedAircraft& tracked) const
{
    // Dim, so it reads as a placeholder rather than data. Same vector language as the radar
    // markers, varied by emitter category like DrawAircraftTriangle.
    const uint32_t col = lgfx::color888(0, 115, 0);
    const int cat = tracked.state.category;

    if (cat == 8) { // rotorcraft: cabin + crossed main rotor + tail boom & rotor
        g.drawLine(cx - 38, cy - 20, cx + 38, cy + 8, col);
        g.drawLine(cx - 38, cy + 8, cx + 38, cy - 20, col);
        g.fillRect(cx - 2, cy - 6, 4, 46, col);   // tail boom
        g.fillRect(cx - 12, cy + 36, 24, 3, col); // tail rotor
        g.fillCircle(cx, cy - 8, 9, col);         // cabin
        return;
    }
    if (cat == 10) { // lighter-than-air: envelope + gondola
        g.fillCircle(cx, cy - 6, 28, col);
        g.fillTriangle(cx - 9, cy + 14, cx + 9, cy + 14, cx, cy + 30, col);
        return;
    }

    // fixed-wing airliner, viewed top-down, nose up
    g.fillTriangle(cx, cy - 44, cx - 5, cy - 30, cx + 5, cy - 30, col);      // nose
    g.fillRoundRect(cx - 5, cy - 34, 10, 74, 5, col);                        // fuselage
    g.fillTriangle(cx - 4, cy - 12, cx - 50, cy + 14, cx - 4, cy + 12, col); // left wing (swept)
    g.fillTriangle(cx + 4, cy - 12, cx + 50, cy + 14, cx + 4, cy + 12, col); // right wing
    g.fillTriangle(cx - 4, cy + 26, cx - 22, cy + 38, cx - 4, cy + 36, col); // left tailplane
    g.fillTriangle(cx + 4, cy + 26, cx + 22, cy + 38, cx + 4, cy + 36, col); // right tailplane
}

void AircraftManager::DrawDetailCard(BandCanvas& backbuffer, const TrackedAircraft& tracked)
{
    const Aircraft& s = tracked.state;
    constexpr int cx = SCREEN_SIZE_DIV_2;

    backbuffer.fillScreen(lgfx::color888(0, 0, 0));
    // frame ring to match the round display
    backbuffer.drawCircle(cx - 1, cx - 1, SCREEN_SIZE_DIV_2 - 1, lgfx::color888(0, 200, 0));

    auto centered = [&](const String& str, int yy) {
        const int x = cx - static_cast<int>(backbuffer.textWidth(str)) / 2;
        backbuffer.drawString(str, x, yy);
    };

    // title: callsign, or the ICAO address if there's no callsign
    String title = s.callsign;
    title.trim();
    if (title.isEmpty()) {
        title = s.icao24;
        title.toUpperCase();
    }

    // The photo slot at the top of the card holds the real photo when one decoded, a generic
    // aircraft silhouette when adsbdb has none, or a "Loading..." note while it's still resolving --
    // so a photo-less card reads as designed, not broken. Only the explicit data page (tapped over
    // from a real photo) drops the slot for the full-height text layout. Photo page 0 keeps the
    // tighter text set; the silhouette/data pages show full telemetry (gated on showPhoto below).
    const bool hasPhoto = photoReady && photoIcao == selectedIcao && photoSprite.getBuffer() != nullptr;
    const bool showPhoto = hasPhoto && detailPage == 0;
    const bool photoSettled = photoIcao == selectedIcao && photoResolved; // we now know there's no photo
    const bool useSlot = showPhoto || !hasPhoto;                          // slot unless flipped to data page

    int y;
    backbuffer.setTextColor(lgfx::color888(0, 255, 0));
    if (useSlot) {
        if (showPhoto) {
            // blit onto the band sprite directly, shifting Y by the band top like BandCanvas does
            photoSprite.pushSprite(&backbuffer.sprite(), cx - PHOTO_W / 2, 30 - backbuffer.offsetY());
        } else if (photoSettled) {
            DrawAircraftSilhouette(backbuffer, cx, 76, tracked);
            backbuffer.setTextSize(1);
            backbuffer.setTextColor(lgfx::color888(0, 120, 0));
            centered("No photo available", 120);
        } else {
            backbuffer.setTextSize(1);
            backbuffer.setTextColor(lgfx::color888(0, 120, 0));
            centered("Loading photo...", 74);
        }
        backbuffer.setTextSize(2);
        backbuffer.setTextColor(lgfx::color888(0, 255, 0));
        centered(title, 136);
        y = 162;
    } else {
        backbuffer.setTextSize(2);
        centered(title, 36);
        y = 70;
    }

    backbuffer.setTextSize(1);
    backbuffer.setTextColor(lgfx::color888(0, 200, 0));
    const int lineHeight = backbuffer.fontHeight() + 5;
    auto line = [&](const String& str) {
        if (str.isEmpty()) return;
        centered(str, y);
        y += lineHeight;
    };

    // flag a special contact up top, in the same colour as its radar marker
    if (const SpecialAircraft::Class sc = SpecialClassOf(tracked); sc != SpecialAircraft::Class::None) {
        const char* label = "";
        switch (sc) {
            case SpecialAircraft::Class::Military:   label = "- MILITARY -";   break;
            case SpecialAircraft::Class::Special:    label = "- SPECIAL -";    break;
            case SpecialAircraft::Class::Helicopter: label = "- HELICOPTER -"; break;
            default: break;
        }
        backbuffer.setTextColor(SpecialColor(sc));
        line(label);
        backbuffer.setTextColor(lgfx::color888(0, 200, 0));
    }

    // first-ever sighting of this type/airline
    if (logbookEnabled && tracked.freshCatch) {
        backbuffer.setTextColor(lgfx::color888(255, 215, 0));
        line("* NEW CATCH *");
        backbuffer.setTextColor(lgfx::color888(0, 200, 0));
    }

    // identity first (route + type + operator), shown in both layouts
    if (!tracked.routeOrigin.isEmpty() && !tracked.routeDest.isEmpty())
        line(tracked.routeOrigin + " -> " + tracked.routeDest);
    if (!tracked.typeCode.isEmpty())     line("Type: " + tracked.typeCode);
    if (!showPhoto && !tracked.typeName.isEmpty()) line(tracked.typeName); // full model, data page only
    if (!tracked.operatorName.isEmpty()) line(tracked.operatorName);

    // the photo page hides the full telemetry for space; the data page (and
    // photo-less aircraft) show everything
    if (!showPhoto) {
        // distance + bearing from the radar centre
        auto [aLat, aLon] = tracked.GetDisplayPosition();
        const float dLatKm = (aLat - (float)lat) * 111.0f;
        const float dLonKm = (aLon - (float)lon) * 111.0f * cosf(radians((float)lat));
        float distance = sqrtf(dLatKm * dLatKm + dLonKm * dLonKm);
        if (rangeUnit == "mi") distance /= 1.609344f;
        float bearing = degrees(atan2f(dLonKm, dLatKm));
        if (bearing < 0.0f) bearing += 360.0f;
        static const char* DIRS[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
        const char* dir = DIRS[((int)roundf(bearing / 45.0f)) % 8];
        line("Dist: " + String(distance, distance < 10.0f ? 1 : 0) + " " + rangeUnit + " " + dir);

        if (!tracked.registration.isEmpty()) line("Reg: " + tracked.registration);
        line("Alt: " + String(lroundf(s.baroAltitude * METRES_TO_FEET)) + " ft");
        line("Spd: " + String(lroundf(s.velocity * MS_TO_KNOTS)) + " kt");
        line("Hdg: " + String(lroundf(s.trueTrack)) + " deg");
        if (!s.squawk.isEmpty()) line("Sqk: " + s.squawk);
    }

    backbuffer.setTextColor(lgfx::color888(0, 110, 0));
    centered(pinnedIcao == selectedIcao ? "swipe up: unpin" : "swipe up: pin", SCREEN_SIZE - 46);
    centered(showPhoto ? "tap: details" : "tap: back", SCREEN_SIZE - 34);
}

void AircraftManager::ProcessMetadataLookups()
{
    if (!metadataNeeded)
        return;

    // one enrichment outstanding at a time, shared with the detail-card path
    if (enrichInFlight)
        return;

    // not enough contiguous heap for a TLS handshake -> don't even try. Attempting
    // it only fails ("BIGNUM alloc failed") and the churn starves the web server.
    if (ESP.getMaxAllocHeap() < ENRICH_TLS_HEAP_FLOOR)
        return;

    const unsigned long now = millis();

    // Pause background enrichment for a few seconds after any touch. The enrichment task's
    // TLS holds the touch I2C bus (touch is serialized against it on the radar view), so
    // enriching while the user is interacting gates taps out -- during a burst of new
    // aircraft that makes touch look dead until enrichment catches up. Pausing keeps the bus
    // free while the user is active; an idle radar still enriches and catches up normally.
    constexpr unsigned long ENRICH_TOUCH_PAUSE = 4000;
    if (now - lastTouchActivityMs < ENRICH_TOUCH_PAUSE)
        return;

    // Space lookups out so a burst of new aircraft doesn't fire many HTTP calls back to back
    // (each holds the bus, gating touch) and to stay friendly to the free adsbdb service.
    // The spacing also guarantees a free-bus window between lookups so a tap can land and
    // trigger the pause above.
    constexpr unsigned long METADATA_LOOKUP_INTERVAL = 5000;
    if (now - lastMetadataLookup < METADATA_LOOKUP_INTERVAL)
        return;

    // Queue the first aircraft still awaiting a lookup, then stop for this tick.
    for (auto& [icao, tracked] : trackedAircraft) {
        if (tracked.metadataState != TrackedAircraft::MetadataState::NotFetched)
            continue;
        if (now < tracked.metadataRetryAfter)
            continue; // still in post-failure cooldown; skip so others get a turn

        lastMetadataLookup = now;
        tracked.metadataState = TrackedAircraft::MetadataState::Fetching;
        RequestMetadata(icao);
        return;
    }
}