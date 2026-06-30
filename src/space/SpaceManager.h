#pragma once

#include <Arduino.h>
#include <vector>

#include "ConfigurationWebServer.h"
#include "OpenSkyAuthTokenHandler.h"
#include "HttpRequestManager.h"
#include "LGFX.h"
#include "BandCanvas.h"
#include "SpaceTheme.h"
#include "SpaceFeedClient.h"

class Sgp4; // forward decl; the SGP4 propagator (Hopperpop lib) is held by pointer, defined in the .cpp

// FEATURE_SPACE top-level controller -- the Spacescope app: a desk window onto live space data
// (ISS, rocket launches, space weather, deep-space probes) built from the same boards + shared
// infra as the radar and the EAM monitor.
//
// Third sibling to AircraftManager / EamManager, selected at compile time in main.cpp: same
// lifecycle (Initialise / Update / Draw) driven by the loop task, and the same shared infra
// (ConfigurationWebServer, HttpRequestManager's single TLS client, OpenSky OAuth). It renders
// space screens instead of the radar; from Stage 2 it pulls from free public space APIs via a
// SpaceFeedClient (mirroring EamFeedClient).
//
// Stage 1 (this file) is the skeleton: the FEATURE_SPACE gate, the config form, the OTA channel,
// and the rotation / touch / brightness shell with a splash + a Zulu (UTC) clock. Each later stage
// is purely additive: a feed in SpaceFeedClient + a Screen enum entry + a DrawX() + a HasData()
// case; nothing else in here changes.
class SpaceManager
{
public:
    SpaceManager(ConfigurationWebServer& config, OpenSkyAuthTokenHandler& auth,
                 HttpRequestManager& httpManager, LGFX& tftGfx)
        : configServer(config), authHandler(auth), http(httpManager), tft(tftGfx),
          feed(httpManager)
    {
    }
    ~SpaceManager() = default;

    void Initialise();
    void Update();
    void Draw(BandCanvas& backbuffer, bool firstPass);

private:
    // The screens. Order here is the default rotation order; the user can enable/disable and
    // reorder via the "space-screens" CSV config key. Clock is the always-available idle screen,
    // so BuildRotation() is never empty; Splash is a cold-start welcome shown only until a live
    // feed has data. Later stages add more (DSN, Voyager, flares, ISS passes, ...).
    enum class Screen : uint8_t {
        Iss, IssPass, Launch, Kp, SolarWind, Scales, Flare, Aurora, Dsn, DeepSpace, Asteroid,
        Humans, Moon, StarMap, Observing, Eclipse, Meteor, CosmicClock, Splash, Clock, COUNT
    };

    ConfigurationWebServer& configServer;
    OpenSkyAuthTokenHandler& authHandler;   // reserved for a later OpenSky-direct feed (ABNCP-style)
    HttpRequestManager& http;
    LGFX& tft;
    SpaceFeedClient feed;

    // ---- config-derived state (set in Initialise) ----
    space::Palette palette = space::PaletteDefault();
    String backendBaseUrl;                   // optional Valar space-feed backend; empty = direct APIs
    std::vector<Screen> enabledOrder;        // enabled screens in display order
    bool hasLatLon = false;
    double deviceLat = 0.0, deviceLon = 0.0;
    uint8_t configuredBrightness = 255;
    bool autoDim = true;

    // ---- rotation / navigation ----
    Screen current = Screen::Clock;
    unsigned long lastAdvanceMs = 0;         // last auto-rotate tick
    unsigned long lastInteractionMs = 0;     // last touch; pauses auto-rotate briefly
    int cardIndex = 0;                       // rotating sub-item index for multi-item screens (DSN/deep-space)
    unsigned long lastCardMs = 0;            // last sub-item advance

    // ---- brightness / night-dim ----
    uint8_t currentBrightness = 255;
    bool nightDim = false;
    unsigned long lastBrightnessCheck = 0;

    // ---- ntfy alerts (reuses the shared ntfy-topic key + POST pattern, like the radar/EAM) ----
    String ntfyTopic;                        // ntfy.sh topic; empty disables all alerts
    bool alertLaunch = true;                 // launch crossing T-10 / T-1
    bool alertAurora = true;                 // high Kp (aurora likely)
    bool alertFlare = false;                 // reserved: needs the GOES X-ray feed (later stage)
    bool alertIss = false;                   // ISS visible pass overhead (SGP4)
    bool alertDsn = false;                   // reserved: needs the DSN feed (later stage)
    bool alertAsteroid = true;               // an asteroid passing inside ~1 lunar distance
    // edge state so an alert fires once per event, not every frame (persists across config reloads)
    long alertLaunchT0 = 0;                  // t0 the fired-flags below refer to (reset when it changes)
    long lastLaunchSecs = 0;                 // previous T-minus seconds, for up->down crossing detection
    bool firedT10 = false, firedT1 = false;  // launch lead-time edges already consumed
    bool kpAlerted = false;                  // high-Kp episode already alerted (reset when Kp drops)
    bool flareAlerted = false;               // M+ flare episode already alerted (reset when it drops)
    long issAlertedRise = 0;                  // rise epoch the ISS-overhead alert already fired for
    String asteroidAlertedDes;               // designation of the close-approach already alerted
    unsigned long lastNotifyMs = 0;          // throttle ntfy POSTs

    // ---- ISS visible-pass prediction (SGP4 from the live TLE; computed on the loop task) ----
    Sgp4* sat = nullptr;                      // propagator (allocated in Initialise)
    bool passValid = false;
    long passRiseEpoch = 0, passSetEpoch = 0; // UTC unix AOS / LOS
    float passMaxEl = 0;                       // peak elevation, deg
    float passAzRise = 0;                      // azimuth at rise, deg
    bool passVisible = false;                  // sunlit satellite in a dark sky
    String passTleKey;                         // TLE line1 the current pass was computed from
    unsigned long lastPassCalcMs = 0;
    bool sgp4Checked = false;                  // one-shot sub-point cross-check vs the live feed

    // ---- touch / gestures ----
    bool wasTouched = false;
    int touchStartX = 0, touchStartY = 0;
    int touchLastX = 0, touchLastY = 0;

    // rotation helpers
    std::vector<Screen> BuildRotation() const;   // enabled screens that currently have data
    bool HasData(Screen s) const;
    void AdvanceRotation(int dir);               // +1 next, -1 prev (manual)
    void AutoRotate();                           // dwell-timed auto advance, skipping empty

    // input + chrome
    void HandleTouch();
    void DrawScreenDots(BandCanvas& c, const std::vector<Screen>& rot) const;

    // screens (defined in SpaceScreens.cpp)
    void DrawIss(BandCanvas& c);
    void DrawIssPass(BandCanvas& c);
    void DrawLaunch(BandCanvas& c);
    void DrawKp(BandCanvas& c);
    void DrawSolarWind(BandCanvas& c);
    void DrawScales(BandCanvas& c);
    void DrawAurora(BandCanvas& c);
    void DrawDsn(BandCanvas& c);
    void DrawDeepSpace(BandCanvas& c);
    void DrawAsteroid(BandCanvas& c);
    void DrawFlare(BandCanvas& c);
    void DrawHumans(BandCanvas& c);
    void DrawMoon(BandCanvas& c);
    void DrawStarMap(BandCanvas& c);
    void DrawObserving(BandCanvas& c);
    void DrawEclipse(BandCanvas& c);
    void DrawMeteor(BandCanvas& c);
    void DrawCosmicClock(BandCanvas& c);
    void DrawSplash(BandCanvas& c);
    void DrawClock(BandCanvas& c);

    // brightness
    void UpdateBrightness();
    float GlowFactor() const { return nightDim ? 0.5f : 1.0f; }

    // ISS pass prediction (loop task)
    void RecomputePass();                    // SGP4: find the next visible ISS overpass for the site

    // ---- "Tonight's observing window" cache (on-device ephemeris; recomputed ~1/min) ----
    // Sampling the next 24h of Sun/Moon altitude is cheap but not free, so DrawObserving reads this
    // cache and RecomputeObserving() refreshes it on the loop task (the geometry changes slowly).
    static constexpr int OBS_SEG = 72;        // ring resolution: 72 x 20-min steps = 24h
    uint8_t obsSunBand[OBS_SEG] = {};         // per-segment darkness band 0=day..4=astronomical night
    bool obsMoonUp[OBS_SEG] = {};             // Moon above the horizon at that segment
    long obsSegEpoch0 = 0;                     // UTC epoch of segment 0 (== the compute time)
    long obsDusk = 0, obsDawn = 0;             // astronomical-night window (UTC epoch), 0 = none found
    bool obsDarkNow = false;                   // already in astronomical darkness at compute time
    double obsMoonIllum = 0;                    // Moon illuminated fraction at the window (0..1)
    long obsMoonUpMin = 0;                      // minutes the Moon is up within the dark window
    bool obsValid = false;
    unsigned long lastObsCalcMs = 0;
    void RecomputeObserving();                 // refresh the cache from the on-device ephemeris

    // ntfy alerts (loop task)
    void CheckAlerts();                      // evaluate the toggleable triggers (launch / aurora)
    void SendNtfy(const String& title, const String& body, const String& tags, int priority);

    // small helpers
    static std::vector<String> SplitList(const String& s, bool lower);
    // "T-HH:MM:SS" / "T-2d 03:14" before T0; "T+HH:MM:SS" / "T+2d 03:14" after (negative = elapsed).
    static String FormatTMinus(long secondsToT0);
    void CenterText(BandCanvas& c, const String& s, int y, uint32_t color);
};
