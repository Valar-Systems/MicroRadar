#include "SpaceFeedClient.h"

using space::SpaceFetchRequest;
using space::SpaceFetchResult;

namespace {

// Default poll intervals (ms).
constexpr uint32_t ISS_MS    = 5000;      // ~5 s: a smoothly gliding blip (well under wheretheiss' ~1 req/s)
constexpr uint32_t LAUNCH_MS = 1200000;   // ~20 m: editorial, slow-moving; the T-minus clock ticks locally
constexpr uint32_t KP_MS     = 720000;    // ~12 m: SWPC Kp is 3-hourly with a recent estimate

constexpr uint32_t MAX_BACKOFF_MS = 600000; // cap exponential backoff at 10 m

constexpr size_t LAUNCH_RETAIN = 5;       // a few upcoming launches (the screen shows the next)
constexpr size_t KP_HISTORY     = 24;     // recent Kp samples kept for the gauge sparkline

// A polite UA: some public APIs reject default/empty agents. No key, nothing identifying the user.
const char* USER_AGENT = "Blipscope-Spacescope/1 (+https://github.com/Valar-Systems/Blipscope)";

} // namespace

void SpaceFeedClient::Begin()
{
    if (taskHandle != nullptr) return; // survives config reloads

    reqQueue = xQueueCreate(1, sizeof(SpaceFetchRequest*));
    resQueue = xQueueCreate(1, sizeof(SpaceFetchResult*));

    // Pinned to core 0 (the Wi-Fi core) at priority 1, like the radar/EAM fetch tasks, so the
    // blocking TLS work never competes with the loop/render task. 12 KB stack covers a TLS
    // handshake + JSON decode.
    xTaskCreatePinnedToCore(Trampoline, "space_fetch", 12288, this, 1, &taskHandle, 0);
}

void SpaceFeedClient::Configure(const Config& newCfg)
{
    cfg = newCfg;

    float sc = cfg.intervalScale;
    if (sc < 1.0f) sc = 1.0f;
    if (sc > 8.0f) sc = 8.0f;

    feeds[F_ISS].intervalMs    = (uint32_t)(ISS_MS * sc);
    feeds[F_LAUNCH].intervalMs = (uint32_t)(LAUNCH_MS * sc);
    feeds[F_KP].intervalMs     = (uint32_t)(KP_MS * sc);

    // Stage the first poll of each endpoint shortly after (re)config, fanned out by ~400 ms so
    // they don't all hit the single TLS client at once.
    const uint32_t now = millis();
    for (int i = 0; i < F_COUNT; ++i) {
        feeds[i].failCount = 0;
        feeds[i].nextDueMs = now + (uint32_t)i * 400 + 300;
        firstLogged[i] = false; // re-confirm each feed after a (re)configure
    }
}

void SpaceFeedClient::Poll()
{
    // 1. apply a ready result (frees its heap) and clear the in-flight gate.
    SpaceFetchResult* res = nullptr;
    if (xQueueReceive(resQueue, &res, 0) == pdTRUE && res != nullptr) {
        ApplyResult(*res);
        delete res;
        inFlight = false;
    }
    if (inFlight) return; // one fetch at a time over the shared TLS client

    // 2. dispatch the next due endpoint.
    const uint32_t now = millis();
    const int feedIdx = PickDueFeed(now);
    if (feedIdx < 0) return;

    SpaceFetchRequest* req = new SpaceFetchRequest();
    if (!BuildRequest(feedIdx, *req)) {
        delete req;
        feeds[feedIdx].nextDueMs = now + feeds[feedIdx].intervalMs;
        return;
    }

    if (xQueueSend(reqQueue, &req, 0) == pdTRUE) {
        inFlight = true;
    } else {
        delete req; // queue unexpectedly full; try again next Poll()
    }
}

int SpaceFeedClient::PickDueFeed(uint32_t now) const
{
    int best = -1;
    uint32_t bestDue = 0;
    for (int i = 0; i < F_COUNT; ++i) {
        // millis() wraps after ~49 days; (now - nextDue) as signed handles the wrap correctly.
        if ((int32_t)(now - feeds[i].nextDueMs) >= 0) {
            if (best < 0 || (int32_t)(feeds[i].nextDueMs - bestDue) < 0) {
                best = i;
                bestDue = feeds[i].nextDueMs;
            }
        }
    }
    return best;
}

bool SpaceFeedClient::BuildRequest(int feedIdx, SpaceFetchRequest& req) const
{
    req.headers.push_back({"User-Agent", USER_AGENT});
    switch (feedIdx) {
        case F_ISS:
            req.endpoint = space::SpaceEndpoint::Iss;
            req.url = "https://api.wheretheiss.at/v1/satellites/25544";
            return true;
        case F_LAUNCH:
            req.endpoint = space::SpaceEndpoint::Launch;
            req.url = "https://fdo.rocketlaunch.live/json/launches/next/5";
            return true;
        case F_KP:
            req.endpoint = space::SpaceEndpoint::Kp;
            req.url = "https://services.swpc.noaa.gov/products/noaa-planetary-k-index.json";
            return true;
        default:
            return false;
    }
}

int SpaceFeedClient::FeedForEndpoint(space::SpaceEndpoint e)
{
    switch (e) {
        case space::SpaceEndpoint::Iss:    return F_ISS;
        case space::SpaceEndpoint::Launch: return F_LAUNCH;
        case space::SpaceEndpoint::Kp:     return F_KP;
    }
    return F_ISS;
}

void SpaceFeedClient::ApplyResult(const SpaceFetchResult& res)
{
    const int f = FeedForEndpoint(res.endpoint);
    const uint32_t now = millis();

    if (!res.ok) {
        // Exponential backoff; keep the previously-stored data so the screen doesn't blank.
        feeds[f].failCount++;
        const uint16_t shift = feeds[f].failCount > 5 ? 5 : feeds[f].failCount;
        uint32_t backoff = feeds[f].intervalMs << shift;
        if (backoff > MAX_BACKOFF_MS || backoff < feeds[f].intervalMs) backoff = MAX_BACKOFF_MS;
        feeds[f].nextDueMs = now + backoff;
        return;
    }

    feeds[f].failCount = 0;
    feeds[f].nextDueMs = now + feeds[f].intervalMs;

    switch (res.endpoint) {
        case space::SpaceEndpoint::Iss:
            iss = res.iss;
            break;
        case space::SpaceEndpoint::Launch:
            launches = res.launches;
            if (launches.size() > LAUNCH_RETAIN) launches.resize(LAUNCH_RETAIN);
            break;
        case space::SpaceEndpoint::Kp:
            wx = res.wx;
            break;
    }

    // One-line confirmation the first time each feed lands (handy for field/serial diagnostics).
    if (!firstLogged[f]) {
        firstLogged[f] = true;
        switch (res.endpoint) {
            case space::SpaceEndpoint::Iss:
                Serial.printf("[space] ISS ok: lat=%.1f lon=%.1f alt=%.0fkm %s\n",
                              iss.lat, iss.lon, iss.altKm, iss.sunlit ? "sunlit" : "eclipsed");
                break;
            case space::SpaceEndpoint::Launch:
                Serial.printf("[space] launch ok: %u upcoming; next=%s / %s\n",
                              (unsigned)launches.size(),
                              launches.empty() ? "-" : launches.front().provider.c_str(),
                              launches.empty() ? "-" : launches.front().mission.c_str());
                break;
            case space::SpaceEndpoint::Kp:
                Serial.printf("[space] Kp ok: %.2f (%u samples)\n", wx.kp, (unsigned)wx.history.size());
                break;
        }
    }
}

void SpaceFeedClient::Trampoline(void* arg)
{
    static_cast<SpaceFeedClient*>(arg)->RunWorker();
}

void SpaceFeedClient::RunWorker()
{
    for (;;) {
        SpaceFetchRequest* req = nullptr;
        if (xQueueReceive(reqQueue, &req, portMAX_DELAY) != pdTRUE || req == nullptr)
            continue;

        SpaceFetchResult* res = new SpaceFetchResult();
        res->endpoint = req->endpoint;
        Fetch(http, *req, *res);
        delete req;

        xQueueSend(resQueue, &res, portMAX_DELAY);
    }
}

void SpaceFeedClient::Fetch(HttpRequestManager& http,
                            const SpaceFetchRequest& req, SpaceFetchResult& res)
{
    JsonDocument doc;
    const HttpResult r = http.GetJson(req.url, doc, req.params, req.headers);
    if (!r.success || r.statusCode < 200 || r.statusCode >= 300) {
        res.ok = false;
        return;
    }

    switch (req.endpoint) {
        case space::SpaceEndpoint::Iss:
            res.ok = space::ParseIss(doc.as<JsonObjectConst>(), res.iss);
            break;
        case space::SpaceEndpoint::Launch:
            // The endpoint's root is an object with a "result" array. Empty = valid "no upcoming".
            space::ParseLaunches(doc.as<JsonObjectConst>(), res.launches, LAUNCH_RETAIN);
            res.ok = true;
            break;
        case space::SpaceEndpoint::Kp:
            // SWPC serves a bare JSON array (of {time_tag, Kp, ...} objects), not an object root.
            res.ok = space::ParseKp(doc.as<JsonArrayConst>(), res.wx, KP_HISTORY);
            break;
    }
}
