#pragma once

#include <Arduino.h>
#include <vector>
#include <utility>

#include "ConfigurationWebServer.h"
#include "OpenSkyAuthTokenHandler.h"
#include "HttpRequestManager.h"
#include "LGFX.h"
#include "BandCanvas.h"
#include "SeismicTheme.h"
#include "SeismicFeedClient.h"

// FEATURE_SEISMIC top-level controller -- the Seismic edition: a desk earthquake radar fed by the
// keyless USGS feed. The fourth sibling to AircraftManager / EamManager / SpaceManager, selected at
// compile time in main.cpp: same lifecycle (Initialise / Update / Draw) driven by the loop task, and
// the same shared infra (ConfigurationWebServer, HttpRequestManager's single TLS client).
//
// The UI mirrors the Aviation radar rather than the Space rotating shell: three swipe-able screens
// (Radar / List / Stats) with a tap-to-inspect detail card overlay. The data layer mirrors
// SpaceFeedClient. IsRadarView()/CurrentSweepAngle() are intentionally absent -- main.cpp gates its
// aircraft PPI sweep out of the FEATURE_SEISMIC build (quakes are static points, not moving
// contacts), so this radar draws static range rings instead of a rotating beam.
class SeismicManager
{
public:
    SeismicManager(ConfigurationWebServer& config, OpenSkyAuthTokenHandler& auth,
                   HttpRequestManager& httpManager, LGFX& tftGfx)
        : configServer(config), authHandler(auth), http(httpManager), tft(tftGfx),
          feed(httpManager)
    {
    }
    ~SeismicManager() = default;

    void Initialise();
    void Update();
    void Draw(BandCanvas& backbuffer, bool firstPass);

private:
    enum class Screen : uint8_t { Radar, List, Stats };

    ConfigurationWebServer& configServer;
    OpenSkyAuthTokenHandler& authHandler;   // reserved (parity with the other apps); unused here
    HttpRequestManager& http;
    LGFX& tft;
    SeismicFeedClient feed;

    // ---- config-derived state (set in Initialise) ----
    seismic::Palette palette = seismic::PaletteDefault();
    String backendBaseUrl;
    bool hasLatLon = false;
    double deviceLat = 0.0, deviceLon = 0.0;
    float minMag = 2.5f;          // worldwide "recent" magnitude floor
    double radiusKm = 500.0;      // near-me radar radius (also the outer ring)
    float bigMag = 6.0f;          // worldwide big-quake alert threshold
    float nearMag = 4.0f;         // near-me alert threshold
    uint8_t configuredBrightness = 255;
    bool autoDim = true;

    // ---- navigation / selection ----
    Screen screen = Screen::Radar;
    bool inDetail = false;            // detail card shown over the current screen
    seismic::Quake selected;          // the quake in the detail card (copied, not a pointer)
    bool selectedValid = false;
    int listScroll = 0;               // first visible row in the list view

    // ---- brightness / night-dim ----
    uint8_t currentBrightness = 255;
    bool nightDim = false;
    unsigned long lastBrightnessCheck = 0;

    // ---- ntfy alerts (shared ntfy-topic key + POST pattern, like the radar/Space) ----
    String ntfyTopic;
    bool alertBig = true;             // a big quake anywhere
    bool alertNear = true;            // a quake near the device
    bool alertTsunami = true;         // a quake flagged tsunami-relevant
    // edge state: seeded at first data so the backlog never fires; then only newer events alert.
    bool alertSeeded = false;
    long lastBigEpoch = 0, lastNearEpoch = 0, lastTsunamiEpoch = 0;
    unsigned long lastNotifyMs = 0;   // throttle ntfy POSTs

    // ---- touch / gestures ----
    bool wasTouched = false;
    int touchStartX = 0, touchStartY = 0;
    int touchLastX = 0, touchLastY = 0;

    // input
    void HandleTouch();
    void HandleTap(int tx, int ty);
    void ExitDetail() { inDetail = false; selectedValid = false; }

    // brightness / alerts
    void UpdateBrightness();
    float GlowFactor() const { return nightDim ? 0.5f : 1.0f; }
    void CheckAlerts();
    void SendNtfy(const String& title, const String& body, const String& tags, int priority);

    // screens (defined in SeismicScreens.cpp)
    void DrawRadar(BandCanvas& c);
    void DrawList(BandCanvas& c);
    void DrawStats(BandCanvas& c);
    void DrawDetailCard(BandCanvas& c);
    void DrawScreenDots(BandCanvas& c) const;
    void DrawClock(BandCanvas& c) const;

    // helpers
    std::pair<int, int> ProjectQuakeToScreen(const seismic::Quake& q) const;
    std::vector<seismic::Quake> SortedNearbyByDistance() const; // nearby with distance/bearing filled
    int HitTestQuake(int tx, int ty, const std::vector<seismic::Quake>& shown) const;
    static String FormatAgo(long epochSecs);
    static std::vector<String> SplitList(const String& s, bool lower);
    void CenterText(BandCanvas& c, const String& s, int y, uint32_t color);
};
