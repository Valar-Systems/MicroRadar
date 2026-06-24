#pragma once

#include "Aircraft.h"

struct TrackedAircraft {
    Aircraft state;
    unsigned long lastSeen;

    // blending state
    float blendFromLat = 0.0f;
    float blendFromLon = 0.0f;
    float blendAlpha = 1.0f;  // 1.0 = blend complete, no interpolation active

    unsigned long lastTick = 0;

    // adsbdb.com enrichment: type/operator/registration aren't in the OpenSky
    // feed, so they're looked up once per aircraft by ICAO address and cached
    // here. NotFetched -> the manager may attempt a lookup; Fetched -> a
    // definitive answer arrived (which can still leave the strings empty for
    // aircraft adsbdb doesn't know), so it's never looked up again.
    enum class MetadataState : uint8_t { NotFetched, Fetched };
    MetadataState metadataState = MetadataState::NotFetched;
    bool watchNotified = false;     // a flyover alert has been sent for this tracking session
    bool overheadNotified = false;  // a "look up" overhead alert has been sent this session
    bool freshCatch = false;        // this sighting added a brand-new type/airline to the logbook
    String typeCode = "";    // adsbdb icao_type, e.g. "B738"
    String typeName = "";    // adsbdb full model, e.g. "Boeing 737-800"
    String operatorName = "";
    String registration = "";
    String photoUrl = "";    // adsbdb url_photo_thumbnail; fetched/decoded on inspect

    // Flight route, looked up by callsign from adsbdb only when the aircraft is
    // inspected (detail view). routeCallsign records the callsign the route was
    // resolved for -- including a "" result for unknown routes -- so it isn't
    // re-queried until the callsign changes.
    String routeOrigin = "";
    String routeDest = "";
    String routeCallsign = "";

    // Recent display positions for the fading trail, stored as geographic
    // coordinates (so they reproject correctly) in a ring buffer sampled once a
    // second. ~TRAIL_CAPACITY seconds of history.
    struct TrailPoint { float lat; float lon; };
    static constexpr int TRAIL_CAPACITY = 60;
    static constexpr unsigned long TRAIL_SAMPLE_MS = 1000;
    TrailPoint trail[TRAIL_CAPACITY];
    int trailWrite = 0;                 // index of the next slot to overwrite
    int trailCount = 0;                 // valid points so far (<= TRAIL_CAPACITY)
    unsigned long lastTrailSample = 0;

    // first appearance, no blend needed
    TrackedAircraft(const Aircraft& ac, unsigned long now)
        : state(ac), lastSeen(now),
        blendFromLat(ac.latitude),
        blendFromLon(ac.longitude),
        blendAlpha(1.0f) {
    }

    // subsequent update — blend from current visual position
    void Update(const Aircraft& newState, unsigned long now) {
        // capture visual position at moment of update before switching state
        auto [curLat, curLon] = GetDisplayPosition();
        blendFromLat = curLat;
        blendFromLon = curLon;
        blendAlpha = 0.0f;  // restart blend

        state = newState;
        lastSeen = now;
    }

    void Tick() {
        unsigned long now = millis();
        float deltaSeconds = (now - lastTick) / 1000.0f;
        lastTick = now;

        const float blendSpeed = 0.15f; // lower = slower, higher = faster
        blendAlpha = min(blendAlpha + deltaSeconds * blendSpeed, 1.0f);
    }

    // Append the current display position to the trail at most once per
    // TRAIL_SAMPLE_MS; call every frame (it self-throttles).
    void SampleTrail() {
        unsigned long now = millis();
        if (trailCount > 0 && now - lastTrailSample < TRAIL_SAMPLE_MS)
            return;
        lastTrailSample = now;

        auto [lat, lon] = GetDisplayPosition();
        trail[trailWrite] = { lat, lon };
        trailWrite = (trailWrite + 1) % TRAIL_CAPACITY;
        if (trailCount < TRAIL_CAPACITY)
            ++trailCount;
    }

    int TrailSize() const { return trailCount; }

    // Trail points oldest (i = 0) to newest (i = TrailSize() - 1).
    std::pair<float, float> TrailPointAt(int i) const {
        const int idx = (trailWrite - trailCount + i + TRAIL_CAPACITY) % TRAIL_CAPACITY;
        return { trail[idx].lat, trail[idx].lon };
    }

    std::pair<float, float> GetDisplayPosition() const {
        auto [deadLat, deadLon] = PredictPosition();

        if (blendAlpha >= 1.0f)
            return { deadLat, deadLon };

        // ease-in-out for smoother feel
        float t = blendAlpha * blendAlpha * (3.0f - 2.0f * blendAlpha);

        return {
            blendFromLat + t * (deadLat - blendFromLat),
            blendFromLon + t * (deadLon - blendFromLon)
        };
    }

    std::pair<float, float> PredictPosition() const {
        float dataAgeOnArrival = 0.0f;
        if (state.timePosition > 0 && state.lastContact > 0)
            dataAgeOnArrival = (float)(state.lastContact - state.timePosition);

        float localElapsed = (millis() - lastSeen) / 1000.0f;
        float dt = localElapsed + dataAgeOnArrival;

        float headingRad = radians(state.trueTrack);
        const float latMetersPerDeg = 111320.0f;
        float deltaLat = (state.velocity * dt * cos(headingRad)) / latMetersPerDeg;
        float deltaLon = (state.velocity * dt * sin(headingRad)) / (latMetersPerDeg * cos(radians(state.latitude)));

        return { state.latitude + deltaLat, state.longitude + deltaLon };
    }
};