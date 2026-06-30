#pragma once

#include <Arduino.h>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "HttpRequestManager.h"
#include "SeismicModels.h"

// Polls the USGS earthquake feed and holds the bounded, retained results the screens read.
//
// Architecture mirrors SpaceFeedClient (and AircraftManager's fetch task): ONE background worker
// does the blocking TLS GET + JSON parse (sharing HttpRequestManager's single TLS client), and
// hands a parsed result back to the loop via a depth-1 queue. ALL scheduling, backoff, and the
// result store live on the loop task (Poll()), so the store is only ever touched by one task --
// the worker never holds a pointer into it. One fetch is in flight at a time; each endpoint has
// its own interval and exponential backoff on failure (last-good data is retained across failures).
//
// Two endpoints, both the keyless USGS FDSN event query API: a worldwide "recent" query, and a
// "nearby" query bounded to a radius around the device (only dispatched once a location is set).
// The optional `baseUrl` (a future valar-seismic-feed) is carried but unused while empty; the
// client hits earthquake.usgs.gov directly.
class SeismicFeedClient {
public:
    explicit SeismicFeedClient(HttpRequestManager& httpManager) : http(httpManager) {}

    // Loop-task config snapshot, applied by Configure().
    struct Config {
        String baseUrl;             // optional backend; empty = USGS directly
        bool hasLatLon = false;
        double lat = 0.0;
        double lon = 0.0;
        float minMag = 2.5f;        // worldwide "recent" minimum magnitude
        double radiusKm = 500.0;    // "nearby" query radius around the device
        float intervalScale = 1.0f; // global poll-interval multiplier (>=1 relaxes all pollers)
    };

    void Begin();                       // spawn the worker task + queues (once; idempotent)
    void Configure(const Config& cfg);  // loop: apply config, restage the schedule
    void Poll();                        // loop: apply a ready result, then dispatch the next due fetch

    // ---- result store (loop-task read) ----
    const std::vector<seismic::Quake>& Recent() const { return recent; }
    const std::vector<seismic::Quake>& Nearby() const { return nearby; }
    bool HasAny() const { return !recent.empty() || !nearby.empty(); }

private:
    enum FeedIdx : uint8_t { F_RECENT, F_NEARBY, F_COUNT };
    struct Feed {
        uint32_t intervalMs = 0;
        uint32_t nextDueMs = 0;
        uint16_t failCount = 0;
    };
    Feed feeds[F_COUNT];

    HttpRequestManager& http;
    Config cfg;

    // result store
    std::vector<seismic::Quake> recent;
    std::vector<seismic::Quake> nearby;

    bool firstLogged[F_COUNT] = {}; // log a one-line summary the first time each feed lands

    // worker task
    TaskHandle_t taskHandle = nullptr;
    QueueHandle_t reqQueue = nullptr;  // loop -> worker: SeismicFetchRequest*
    QueueHandle_t resQueue = nullptr;  // worker -> loop: SeismicFetchResult*
    bool inFlight = false;

    static void Trampoline(void* arg);
    void RunWorker();                                  // worker task body
    static void Fetch(HttpRequestManager& http,
                      const seismic::SeismicFetchRequest& req, seismic::SeismicFetchResult& res);

    int PickDueFeed(uint32_t now) const;               // most-overdue ready feed, or -1
    bool BuildRequest(int feedIdx, seismic::SeismicFetchRequest& req) const;
    void ApplyResult(const seismic::SeismicFetchResult& res);
    static int FeedForEndpoint(seismic::SeismicEndpoint e);
};
