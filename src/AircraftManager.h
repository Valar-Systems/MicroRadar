#pragma once

#include <map>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "HttpRequestManager.h"
#include "models/TrackedAircraft.h"
#include "ConfigurationWebServer.h"
#include "OpenSkyAuthTokenHandler.h"
#include "AircraftInfoFields.h"
#include "SpecialAircraft.h"
#include "Logbook.h"
#include "MqttPublisher.h"
#include "LGFX.h"

class AircraftManager
{
private:
    double lat = 0.0;
    double lon = 0.0;
    double radLat = 0.2; // latitude half-span of the scan box, in degrees
    double radLon = 0.2; // longitude half-span of the scan box, in degrees
    double rangeRadiusDisplay = 0.0; // outer ring distance in the user's unit, for range labels
    String rangeUnit = "km";
    std::map<String, TrackedAircraft> trackedAircraft;

    bool displayInfoText = true;
    bool displayTriangles = true;
    bool displayTrails = true;
    bool displayAltColor = true;  // color aircraft markers by altitude band
    bool displayHighlight = true; // ring the nearest/highest/fastest contacts

    // Screen navigation. Three top-level screens cycle via horizontal swipe; the
    // detail card overlays whichever screen you're on.
    enum class Screen { Radar, List, Stats };
    Screen screen = Screen::Radar;
    bool inDetail = false;     // detail card shown over the current screen
    String selectedIcao = "";  // aircraft shown in the detail card
    String pinnedIcao = "";    // aircraft kept highlighted ("tracked") on the radar
    int detailPage = 0;        // 0 = photo card, 1 = full-data card
    int listScroll = 0;        // first visible row in the list view

    // touch/gesture state: a release is classified as a tap or a 4-way swipe
    bool wasTouched = false;
    int touchStartX = 0, touchStartY = 0;
    int touchLastX = 0, touchLastY = 0;
    enum class Swipe { Up, Down, Left, Right };

    // Touch-controller liveness watchdog. The CST816S can drop off the I2C bus
    // (standby wedge / bus lockup), and the LovyanGFX driver never recovers it
    // -- it just keeps reporting "no touch" forever. We can't distinguish that
    // from a genuinely idle panel through getTouch(), so during quiet gaps we
    // periodically pulse the controller's reset and re-init it. See HandleTouch.
    unsigned long lastTouchActivityMs = 0; // last frame getTouch() actually read a touch
    unsigned long lastTouchReinitMs = 0;   // last time we reset + re-inited the controller

    // Decoded aircraft photo for the detail view. The sprite is created once and
    // reused; photoIcao/photoReady track which aircraft it currently holds.
    LGFX_Sprite photoSprite;
    String photoIcao = "";
    bool photoReady = false;

    // Parallel to AIRCRAFT_INFO_FIELDS: which info lines the user has enabled.
    // Populated once in Initialise() (config changes restart the device).
    std::vector<bool> infoFieldEnabled;

    // True when at least one enabled field needs the adsbdb lookup; lets us skip
    // all enrichment network traffic when the user shows none of those fields.
    bool metadataNeeded = false;
    unsigned long lastMetadataLookup = 0;

    // Watchlist: aircraft whose callsign/icao/registration/type starts with one
    // of these (lowercased) prefixes is flagged on screen and triggers an ntfy
    // flyover alert. Empty watchlist disables all of it.
    std::vector<String> watchlist;
    String ntfyTopic = "";
    unsigned long lastNotifyCheck = 0;

    // Special-aircraft detection. Every class is derived offline from the live
    // feed (ICAO address / emitter category / callsign), so it works on any data
    // source and even with all adsbdb enrichment disabled.
    bool showMilitary = true;     // ring + "MIL" tag military contacts on radar/list
    bool alertMilitary = false;   // also raise an ntfy flyover alert for them
    bool showHelicopters = false; // ring + "HELI" tag rotorcraft
    bool showSpecial = false;     // ring + "SPC" tag distinctive callsigns

    // "Look up!" overhead alert: flag a contact passing within overheadKm of the
    // centre so you can physically glance up and spot it.
    bool showOverhead = false;    // pulsing "LOOK UP" ring on the radar
    bool alertOverhead = false;   // also raise an ntfy alert
    double overheadKm = 3.0;      // how close to the centre counts as "overhead"

    // Spotting logbook ("lifelist"): persistent tally of unique types/airlines/
    // countries seen. Opt-in, because it forces an adsbdb lookup on every contact
    // (to learn the type/airline) and writes to flash. Country + contact odometer
    // work from the OpenSky feed alone.
    Logbook logbook;
    bool logbookEnabled = false;

    // Home Assistant / MQTT. The publisher runs on its own task; we just hand it a
    // retained JSON "summary" (count, nearest, overhead/military flags) every few
    // seconds, plus the HA discovery configs on (re)connect so sensors auto-create.
    MqttPublisher mqtt;
    bool mqttEnabled = false;
    bool mqttDiscovery = true;
    String mqttBase = "blipscope";
    unsigned long lastMqttState = 0;

    unsigned long fetchInterval = 0;
    unsigned long lastFetch = 999999;

    // Data source. Default is the OpenSky cloud API; the user can instead point
    // Blipscope at their own ADS-B receiver's dump1090-fa/readsb "aircraft.json"
    // HTTP endpoint, which has no rate limit and updates ~1 Hz. The two sources
    // are mutually exclusive (config selector), so only one feed is ever polled.
    bool useLocalSource = false;
    String localUrl = ""; // normalised aircraft.json URL, empty unless local

    // Background OpenSky states fetch. The HTTPS GET + JSON decode used to run
    // inline on the loop and stall it for a second or two each cycle; since touch
    // is only polled once per loop, a tap on a plane during that stall was missed.
    // A dedicated task now does the GET + parse, and the loop only merges the parsed
    // result into trackedAircraft (a fast map operation), so it stays responsive
    // throughout the refresh. The task shares the loop's single HTTP client (the C3
    // hasn't the heap for a second TLS context); HttpRequestManager's own mutex
    // serializes the two, and the !fetchInFlight gate keeps the loop from blocking
    // on it during a fetch. All trackedAircraft mutation still happens on the loop
    // task; the fetch task only ever produces a parsed vector of its own.
    TaskHandle_t  fetchTaskHandle = nullptr;   // non-null once the task is running
    QueueHandle_t fetchRequestQueue = nullptr; // loop -> task: FetchRequest*
    QueueHandle_t fetchResultQueue = nullptr;  // task -> loop: FetchResult*
    bool fetchInFlight = false;                // loop-task-only: a request is outstanding

    // backlight + clock
    uint8_t configuredBrightness = 255; // day/base level from the slider
    uint8_t currentBrightness = 255;    // currently applied level (avoids redundant writes)
    bool autoDim = true;                // dim at night based on solar elevation
    long utcOffsetSec = 0;              // local time = UTC + this, for the clock
    unsigned long lastBrightnessCheck = 0;

    ConfigurationWebServer& configServer;
    OpenSkyAuthTokenHandler& authHandler;
    HttpRequestManager& http;
    LGFX& tft;

    void DrawRadarCircles(LGFX_Sprite& backbuffer) const;
    std::pair<int, int> ProjectCoordinateToScreen(float predLat, float predLon) const;
    void DrawAircraftInfo(LGFX_Sprite& backbuffer, int x, int y, const TrackedAircraft& tracked) const;
    void DrawAircraftTriangle(LGFX_Sprite& backbuffer, int x, int y, const TrackedAircraft& tracked, uint32_t color) const;
    void DrawAircraftTrail(LGFX_Sprite& backbuffer, const TrackedAircraft& tracked, int headX, int headY) const;
    void DrawEmergencyAlert(LGFX_Sprite& backbuffer, int x, int y, const TrackedAircraft& tracked) const;
    void DrawDetailCard(LGFX_Sprite& backbuffer, const TrackedAircraft& tracked);

    void DrawRadar(LGFX_Sprite& backbuffer);
    void DrawList(LGFX_Sprite& backbuffer);
    void DrawStats(LGFX_Sprite& backbuffer);
    void DrawScreenIndicator(LGFX_Sprite& backbuffer) const;
    void DrawClock(LGFX_Sprite& backbuffer) const;
    std::vector<String> SortedAircraftByDistance();

    void UpdateBrightness(); // apply solar day/night dimming (throttled)

    void StartFetchTask();                      // spawn the OpenSky fetch task once
    static void FetchTaskTrampoline(void* arg); // FreeRTOS entry -> RunFetchTask()
    void RunFetchTask();                        // blocking GET + JSON decode, off-loop
    void RequestFetch();                        // loop: snapshot params + token, signal task
    void ConsumeFetchResult();                  // loop: merge a ready result, non-blocking

    void HandleTouch();             // poll the touch panel, classify tap vs swipe
    void HandleTap(int tx, int ty); // route a tap to selection / dismissal
    void HandleSwipe(Swipe swipe);  // route a swipe to navigation / pin

    // Resolve the selected aircraft's metadata, route, then photo -- one blocking
    // lookup per frame so the detail card fills in progressively. Runs for the
    // inspected aircraft even when the radar's enrichment fields are disabled.
    void ProcessDetailLookups();
    void LookupRoute(const String& callsign, TrackedAircraft& tracked);
    void LoadPhoto(const String& url);

    // Resolve type/operator/registration for tracked aircraft via adsbdb.com,
    // one at a time and throttled, so the blocking HTTP calls don't stall the
    // render loop more than the existing OpenSky fetch already does.
    void ProcessMetadataLookups();
    void LookupAircraftMetadata(const String& icao24, TrackedAircraft& tracked);

    bool MatchesWatchlist(const TrackedAircraft& tracked) const;
    bool IsOverhead(const TrackedAircraft& tracked) const; // within overheadKm of the centre
    void ProcessAlerts();                                  // ntfy: flyover + overhead, throttled
    void SendFlyoverNotification(const TrackedAircraft& tracked, bool military = false);
    void SendOverheadNotification(const TrackedAircraft& tracked);
    void DrawOverheadAlert(LGFX_Sprite& backbuffer, int x, int y) const;

    void PublishMqttState();     // retained JSON summary of the current picture
    void PublishMqttDiscovery(); // Home Assistant MQTT discovery configs (retained)

    // Toggle-aware special classification + its display colour, shared by the
    // radar, list, and detail views. Honors the per-class show toggles and the
    // priority order in SpecialAircraft::Class.
    SpecialAircraft::Class SpecialClassOf(const TrackedAircraft& tracked) const;
    static uint32_t SpecialColor(SpecialAircraft::Class c);

public:
    AircraftManager(ConfigurationWebServer& config, OpenSkyAuthTokenHandler& auth, HttpRequestManager& httpManager, LGFX& tftGfx)
        : configServer(config), authHandler(auth), http(httpManager), tft(tftGfx)
    {
    }
    ~AircraftManager() = default;

    void Initialise();
    void Update();
    void Draw(LGFX_Sprite& backbuffer);
    bool IsRadarView() const { return screen == Screen::Radar && !inDetail; }
};