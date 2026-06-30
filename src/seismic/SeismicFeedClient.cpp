#include "SeismicFeedClient.h"

#include <time.h>

using seismic::SeismicFetchRequest;
using seismic::SeismicFetchResult;

namespace {

// Default poll intervals (ms). USGS adds events about once a minute; ~2 min keeps the picture
// fresh without hammering a free public service.
constexpr uint32_t RECENT_MS = 120000;   // ~2 m: worldwide recent quakes
constexpr uint32_t NEARBY_MS = 120000;   // ~2 m: quakes within the configured radius

constexpr uint32_t MAX_BACKOFF_MS = 600000; // cap exponential backoff at 10 m

constexpr size_t RECENT_RETAIN = 60;     // worldwide recent quakes kept
constexpr size_t NEARBY_RETAIN = 60;     // nearby quakes kept (also the query `limit`)

// "Nearby" pulls a lower magnitude floor than the worldwide query so the local radar populates with
// the small quakes that matter near you; the query `limit` bounds the response either way.
constexpr float NEARBY_MIN_MAG = 1.0f;

// A polite UA: some public APIs reject default/empty agents. No key, nothing identifying the user.
const char* USER_AGENT = "Blipscope-Seismic/1 (+https://github.com/Valar-Systems/Blipscope)";

const char* USGS_QUERY = "https://earthquake.usgs.gov/fdsnws/event/1/query";

} // namespace

void SeismicFeedClient::Begin()
{
    if (taskHandle != nullptr) return; // survives config reloads

    reqQueue = xQueueCreate(1, sizeof(SeismicFetchRequest*));
    resQueue = xQueueCreate(1, sizeof(SeismicFetchResult*));

    // Pinned to core 0 (the Wi-Fi core) at priority 1, like the radar/Space fetch tasks, so the
    // blocking TLS work never competes with the loop/render task. 12 KB stack covers a TLS
    // handshake + JSON decode.
    xTaskCreatePinnedToCore(Trampoline, "seismic_fetch", 12288, this, 1, &taskHandle, 0);
}

void SeismicFeedClient::Configure(const Config& newCfg)
{
    cfg = newCfg;

    float sc = cfg.intervalScale;
    if (sc < 1.0f) sc = 1.0f;
    if (sc > 8.0f) sc = 8.0f;

    feeds[F_RECENT].intervalMs = (uint32_t)(RECENT_MS * sc);
    feeds[F_NEARBY].intervalMs = (uint32_t)(NEARBY_MS * sc);

    // Stage the first poll of each endpoint shortly after (re)config, fanned out by ~400 ms so they
    // don't both hit the single TLS client at once.
    const uint32_t now = millis();
    for (int i = 0; i < F_COUNT; ++i) {
        feeds[i].failCount = 0;
        feeds[i].nextDueMs = now + (uint32_t)i * 400 + 300;
        firstLogged[i] = false; // re-confirm each feed after a (re)configure
    }
}

void SeismicFeedClient::Poll()
{
    // 1. apply a ready result (frees its heap) and clear the in-flight gate.
    SeismicFetchResult* res = nullptr;
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

    SeismicFetchRequest* req = new SeismicFetchRequest();
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

int SeismicFeedClient::PickDueFeed(uint32_t now) const
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

bool SeismicFeedClient::BuildRequest(int feedIdx, SeismicFetchRequest& req) const
{
    req.headers.push_back({"User-Agent", USER_AGENT});

    // Scope "recent" to the last 7 days once NTP is set, so the query stays small and current;
    // before NTP, omit starttime and rely on orderby=time + limit.
    String start;
    const time_t now = time(nullptr);
    if (now > 1600000000) {
        const time_t weekAgo = now - 7 * 86400;
        struct tm t;
        gmtime_r(&weekAgo, &t);
        char buf[12];
        strftime(buf, sizeof(buf), "%Y-%m-%d", &t);
        start = buf;
    }

    char mag[8];
    switch (feedIdx) {
        case F_RECENT: {
            req.endpoint = seismic::SeismicEndpoint::Recent;
            snprintf(mag, sizeof(mag), "%.1f", cfg.minMag);
            String u = String(USGS_QUERY) + "?format=geojson&orderby=time&limit=" + String((int)RECENT_RETAIN);
            u += "&minmagnitude="; u += mag;
            if (start.length()) { u += "&starttime="; u += start; }
            req.url = u;
            return true;
        }
        case F_NEARBY: {
            if (!cfg.hasLatLon) return false; // nothing to centre a radius on yet
            req.endpoint = seismic::SeismicEndpoint::Nearby;
            snprintf(mag, sizeof(mag), "%.1f", NEARBY_MIN_MAG);
            String u = String(USGS_QUERY) + "?format=geojson&orderby=time&limit=" + String((int)NEARBY_RETAIN);
            u += "&minmagnitude="; u += mag;
            u += "&latitude=" + String(cfg.lat, 4);
            u += "&longitude=" + String(cfg.lon, 4);
            u += "&maxradiuskm=" + String(cfg.radiusKm, 0);
            if (start.length()) { u += "&starttime="; u += start; }
            req.url = u;
            return true;
        }
        default:
            return false;
    }
}

int SeismicFeedClient::FeedForEndpoint(seismic::SeismicEndpoint e)
{
    switch (e) {
        case seismic::SeismicEndpoint::Recent: return F_RECENT;
        case seismic::SeismicEndpoint::Nearby: return F_NEARBY;
    }
    return F_RECENT;
}

void SeismicFeedClient::ApplyResult(const SeismicFetchResult& res)
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
        case seismic::SeismicEndpoint::Recent: recent = res.quakes; break;
        case seismic::SeismicEndpoint::Nearby: nearby = res.quakes; break;
    }

    if (!firstLogged[f]) {
        firstLogged[f] = true;
        const std::vector<seismic::Quake>& v = (res.endpoint == seismic::SeismicEndpoint::Recent) ? recent : nearby;
        Serial.printf("[seismic] %s ok: %u quakes%s\n",
                      res.endpoint == seismic::SeismicEndpoint::Recent ? "recent" : "nearby",
                      (unsigned)v.size(),
                      v.empty() ? "" : (String(" (newest M") + String(v.front().mag, 1) + ")").c_str());
    }
}

void SeismicFeedClient::Trampoline(void* arg)
{
    static_cast<SeismicFeedClient*>(arg)->RunWorker();
}

void SeismicFeedClient::RunWorker()
{
    for (;;) {
        SeismicFetchRequest* req = nullptr;
        if (xQueueReceive(reqQueue, &req, portMAX_DELAY) != pdTRUE || req == nullptr)
            continue;

        SeismicFetchResult* res = new SeismicFetchResult();
        res->endpoint = req->endpoint;
        Fetch(http, *req, *res);
        delete req;

        xQueueSend(resQueue, &res, portMAX_DELAY);
    }
}

void SeismicFeedClient::Fetch(HttpRequestManager& http,
                              const SeismicFetchRequest& req, SeismicFetchResult& res)
{
    JsonDocument doc;
    const HttpResult r = http.GetJson(req.url, doc, req.params, req.headers);
    if (!r.success || r.statusCode < 200 || r.statusCode >= 300) {
        res.ok = false;
        return;
    }
    const size_t cap = (req.endpoint == seismic::SeismicEndpoint::Recent) ? 60 : 60;
    seismic::ParseQuakes(doc.as<JsonObjectConst>(), res.quakes, cap);
    res.ok = true; // a successful fetch with zero quakes in range is valid "no data"
}
