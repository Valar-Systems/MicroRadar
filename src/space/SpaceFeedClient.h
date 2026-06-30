#pragma once

#include <Arduino.h>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "HttpRequestManager.h"
#include "SpaceModels.h"

// Polls Spacescope's data sources and holds the bounded, retained results the screens read.
//
// Architecture mirrors EamFeedClient (and AircraftManager's fetch task): ONE background worker
// does the blocking TLS GET + JSON parse (sharing HttpRequestManager's single TLS client), and
// hands a parsed result back to the loop via a depth-1 queue. ALL scheduling, backoff, and the
// result store live on the loop task (Poll()), so the store is only ever touched by one task --
// the worker never holds a pointer into it. One fetch is in flight at a time; each endpoint has
// its own interval and exponential backoff on failure (last-good data is retained across failures).
//
// Stage 2 polls three keyless public APIs directly (no backend, no key): ISS position, the next
// launch, and the planetary Kp index. The optional `baseUrl` (valar-space-feed) is carried for a
// later stage; while empty, the client hits the public hosts directly.
class SpaceFeedClient {
public:
    explicit SpaceFeedClient(HttpRequestManager& httpManager) : http(httpManager) {}

    // Loop-task config snapshot, applied by Configure().
    struct Config {
        String baseUrl;             // optional valar-space-feed; empty = direct public APIs
        bool hasLatLon = false;
        double lat = 0.0;
        double lon = 0.0;
        float intervalScale = 1.0f; // global poll-interval multiplier (>=1 relaxes all pollers)
    };

    void Begin();                       // spawn the worker task + queues (once; idempotent)
    void Configure(const Config& cfg);  // loop: apply config, restage the schedule
    void Poll();                        // loop: apply a ready result, then dispatch the next due fetch

    // ---- result store (loop-task read) ----
    const space::IssState& Iss() const { return iss; }
    const std::vector<space::Launch>& Launches() const { return launches; }
    const space::SpaceWx& Wx() const { return wx; }

private:
    enum FeedIdx : uint8_t { F_ISS, F_LAUNCH, F_KP, F_COUNT };
    struct Feed {
        uint32_t intervalMs = 0;
        uint32_t nextDueMs = 0;
        uint16_t failCount = 0;
    };
    Feed feeds[F_COUNT];

    HttpRequestManager& http;
    Config cfg;

    // result store
    space::IssState iss;
    std::vector<space::Launch> launches;
    space::SpaceWx wx;

    bool firstLogged[F_COUNT] = {}; // log a one-line summary the first time each feed lands

    // worker task
    TaskHandle_t taskHandle = nullptr;
    QueueHandle_t reqQueue = nullptr;  // loop -> worker: SpaceFetchRequest*
    QueueHandle_t resQueue = nullptr;  // worker -> loop: SpaceFetchResult*
    bool inFlight = false;

    static void Trampoline(void* arg);
    void RunWorker();                                  // worker task body
    static void Fetch(HttpRequestManager& http,
                      const space::SpaceFetchRequest& req, space::SpaceFetchResult& res);

    int PickDueFeed(uint32_t now) const;               // most-overdue ready feed, or -1
    bool BuildRequest(int feedIdx, space::SpaceFetchRequest& req) const;
    void ApplyResult(const space::SpaceFetchResult& res);
    static int FeedForEndpoint(space::SpaceEndpoint e);
};
