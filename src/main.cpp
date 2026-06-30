#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>
#include <esp_task_wdt.h>

#include "LGFX.h"
#include "Layout.h"
#include "BootScreen.h"
#include "Board.h"
#include "DeviceIdentity.h"
#include "WiFiManagerHelpers.h"
#include "ConfigurationWebServer.h"
#include "HttpRequestManager.h"
#include "OpenSkyAuthTokenHandler.h"
#include "OtaUpdater.h"
// The active app is a compile-time choice: the radar (default), the FEATURE_EAM monitor, the
// FEATURE_SPACE (Spacescope) monitor, or the FEATURE_SEISMIC earthquake radar. All expose the same
// Initialise()/Update()/Draw() surface, so loop() drives `appManager` without knowing which it is.
// The radar's sweep/models headers are radar-only.
#if defined(FEATURE_EAM)
#include "eam/EamManager.h"
#elif defined(FEATURE_SPACE)
#include "space/SpaceManager.h"
#elif defined(FEATURE_SEISMIC)
#include "seismic/SeismicManager.h"
#else
#include "AircraftManager.h"
#include "DrawHelpers.h"
#include "models/Aircraft.h"
#include "models/TrackedAircraft.h"
#endif

LGFX tft;
LGFX_Sprite backbuffer(&tft);

WiFiManager wm;
ConfigurationWebServer configServer;
HttpRequestManager http;
OpenSkyAuthTokenHandler authHandler(http);

#if defined(FEATURE_EAM)
EamManager appManager(configServer, authHandler, http, tft);
#elif defined(FEATURE_SPACE)
SpaceManager appManager(configServer, authHandler, http, tft);
#elif defined(FEATURE_SEISMIC)
SeismicManager appManager(configServer, authHandler, http, tft);
#else
AircraftManager appManager(configServer, authHandler, http, tft);
#endif

void setup()
{
  Serial.begin(115200);
  while (!Serial && millis() < 3000) { delay(10); } // wait up to 3s for the USB CDC host to open the port

  // Give the Task Watchdog headroom over a single synchronous network call. The OpenSky
  // and adsbdb fetches run TLS handshakes that take the lwIP core lock and don't yield;
  // on the single-core C3 that can keep the watchdog-fed async_tcp service task from
  // running for several seconds. The IDF default TWDT is 5 s -- the same order as a slow
  // handshake -- so a legitimately slow (but still progressing) fetch tripped it and
  // rebooted the board (async_tcp / osky_fetch watchdog abort). 10 s clears the worst-case
  // connect (bounded to 3 s in HttpRequestManager) plus margin, while a genuine hang still
  // reboots. Reconfigure, not init: IDF startup already armed the TWDT.
  esp_task_wdt_config_t wdtConfig = {
    .timeout_ms = 10000,
    .idle_core_mask = (1 << 0), // CPU0 idle task -- matches the single-core Arduino default
    .trigger_panic = true,      // still reboot on a real hang
  };
  esp_task_wdt_reconfigure(&wdtConfig);

  // Board-specific bring-up that has to happen before the display. On SKUs whose panel/touch
  // reset and chip-select hang off an I2C IO expander (the S3-2.1), this drives that expander
  // (and the IMU); on the C3 it's a no-op. See variant::BoardPreInit().
  variant::BoardPreInit();

  // initialise LGFX + screen. init() returns false if the panel/bus didn't come up (on an
  // RGB panel that means the framebuffer/bus init failed -> nothing scans out).
  const bool panelOk = tft.init();
  tft.invertDisplay(BLIPSCOPE_DISP_INVERT); // per-variant: the GC9A01 boots inverted, the ST7701 doesn't
  // drive the backlight via PWM (configured in LGFX.h) so it's dimmable; full
  // brightness for the boot screen until AircraftManager applies the saved level
  tft.setBrightness(255);

  // The full-frame backbuffer only fits on boards with PSRAM (480x480x8bpp ~= 230 KB); banded
  // SKUs keep it in internal RAM so a TLS handshake still has contiguous heap. setPsram() must
  // precede createSprite().
  if constexpr (!variant::BANDED_RENDER)
    backbuffer.setPsram(true);
  backbuffer.setColorDepth(8);
  void* spriteBuf = backbuffer.createSprite(SCREEN_SIZE, BAND_H);
  Serial.printf("[disp] tft.init=%d %dx%d  backbuffer %s; psram_free=%u heap_free=%u\n",
                panelOk, (int)tft.width(), (int)tft.height(), spriteBuf ? "ok" : "ALLOC FAILED",
                (unsigned)ESP.getFreePsram(), (unsigned)ESP.getFreeHeap());

#ifdef BLIPSCOPE_BRINGUP_DIAG
  // Bring-up only (behind a per-env flag): cycle full-screen colours straight to the panel so we
  // can SEE whether direct draws reach the scanned framebuffer. If the screen flashes
  // red/green/blue/white, the RGB path works and the problem is elsewhere; if it stays black,
  // our pixels aren't reaching the framebuffer the bus scans out. Remove once the panel is up.
  {
    const uint32_t colors[] = { 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF, 0x000000 };
    const char* names[]     = { "RED", "GREEN", "BLUE", "WHITE", "BLACK" };
    for (int pass = 0; pass < 3; ++pass) {
      for (int c = 0; c < 5; ++c) {
        tft.fillScreen(lgfx::color888((colors[c] >> 16) & 0xFF, (colors[c] >> 8) & 0xFF, colors[c] & 0xFF));
        board::DisplayFlush(tft); // push the fill out of cache so the RGB DMA scans it
        Serial.printf("[diag] fillScreen %-5s  tft.init=%d %dx%d psram_free=%u\n",
                      names[c], panelOk, (int)tft.width(), (int)tft.height(), (unsigned)ESP.getFreePsram());
        Serial.flush();
        delay(700);
      }
    }
  }
#endif

  // establish WiFi connection. Composed through the backbuffer so it renders on the SPD2010 (which
  // can't take direct per-glyph writes); a no-op-different path on every other SKU. See BootScreen.h.
  DrawCenteredScreen(tft, backbuffer, lgfx::color888(0, 0, 0), lgfx::color888(0, 255, 0), "Connecting to WiFi...");
  board::DisplayFlush(tft); // RGB panels: make the boot screen visible (no-op on SPI SKUs)

#if defined(BLIPSCOPE_PANEL_SPD2010)
  // Critical ordering for the 1.46B: the Wi-Fi radio must NOT come up in the first seconds after
  // power-on or its bring-up glitches the QSPI panel and it never recovers (writes stop landing)
  // until a cold reboot -- a power/clock stabilisation issue, bisected on hardware (WiFi at ~7 s
  // still fails; ~10 s is reliable). Hold the boot screen here so the rails settle before the radio
  // inrush; the radar renders normally afterwards.
  delay(9000);
#endif

  // Log every WiFi radio event so we can see exactly where a join fails.
  // These fire on the WiFi event task even while autoConnect() blocks below.
  WiFi.onEvent([](arduino_event_id_t event, arduino_event_info_t info) {
    switch (event) {
      case ARDUINO_EVENT_WIFI_STA_START:
        // Re-apply the hostname the instant the STA interface starts -- before the DHCP
        // request goes out -- so it's sent as DHCP option 12 and the router registers it.
        // That's what lets Angry IP Scanner (and the router's device list) resolve the
        // name; WiFiManager sets it too, but its mode-cycling applies it after DHCP, too
        // late. This mirrors MiniSpeedCam's working mode->setHostname->begin ordering.
        WiFi.setHostname(DeviceIdentity::Name().c_str());
        // Full TX power (chip default) for best range. Both setters require a started STA,
        // which is exactly what this event signals.
        WiFi.setTxPower(WIFI_POWER_19_5dBm);
        Serial.printf("[WiFi] STA started; hostname=%s, TX 19.5dBm\n", DeviceIdentity::Name().c_str());
        break;
      case ARDUINO_EVENT_WIFI_STA_CONNECTED:
        Serial.printf("[WiFi] Associated with \"%s\", waiting for IP...\n", WiFi.SSID().c_str());
        break;
      case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        Serial.printf("[WiFi] CONNECTED  IP=%s  RSSI=%d dBm\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
        break;
      case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
        const auto reason = (wifi_err_reason_t)info.wifi_sta_disconnected.reason;
        Serial.printf("[WiFi] DISCONNECTED  reason=%d (%s)\n",
                      reason, WiFi.disconnectReasonName(reason));
        if (reason == WIFI_REASON_NO_AP_FOUND)
          Serial.println("       SSID not found: check spelling/range. The ESP32-C3 is 2.4GHz-only and cannot see 5GHz networks.");
        else if (reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                 reason == WIFI_REASON_AUTH_EXPIRE)
          Serial.println("       Auth/key mismatch: most likely a wrong password.");
        else if (reason == WIFI_REASON_HANDSHAKE_TIMEOUT)
          Serial.println("       Handshake frames lost (not a key mismatch): RF/power instability or weak signal. Try lower TX power / better supply.");
        break;
      }
      default:
        break;
    }
  });

  WiFiManagerHelpers::ConfigureWiFiManager(wm, tft, backbuffer);

  const bool connected = wm.autoConnect(WiFiManagerHelpers::WiFiManagerName().c_str());
  Serial.printf("[WiFi] autoConnect() returned %s\n",
                connected ? "true (connected)" : "false (portal timed out / not connected)");

  // Disable Wi-Fi modem-sleep. By default the radio sleeps between beacons and
  // wakes each DTIM to listen, pulsing the supply current at the beacon rate.
  // On this board that periodic load makes the decoupling caps near the ESP32-C3
  // sing -- a faint, steady buzz. Keeping the radio always-on flattens the draw
  // and silences it. The device is USB/mains powered, so the extra ~20-30 mA is
  // a non-issue, and latency/throughput actually improve.
  if (connected) {
    WiFi.setSleep(WIFI_PS_NONE);

    // Show how to reach the config page so the user doesn't have to remember the device's
    // name later: it lives at http://<name>.local (mDNS), with the IP as a fallback for
    // networks where mDNS doesn't resolve. Held a few seconds before OTA/app startup.
    // Composed through the backbuffer so it renders on the SPD2010 (see BootScreen.h); the
    // host line is also available any time on the radar's Stats screen.
    const String host = DeviceIdentity::Name() + ".local";
    const String ip   = WiFi.localIP().toString();
    DrawCenteredScreen(tft, backbuffer, lgfx::color888(0, 0, 0), lgfx::color888(0, 255, 0),
                       "- CONNECTED -", host.c_str(), ip.c_str());
    board::DisplayFlush(tft); // RGB panels: make the screen visible (no-op on SPI SKUs)
    delay(4000);
  }

  // start NTP in UTC; the on-screen clock applies the configured offset, and the
  // solar auto-dim works directly in UTC
  configTime(0, 0, "pool.ntp.org");

  // self-update from the latest GitHub release before normal startup; reboots
  // into the new firmware if one is newer than this build
  MaybeUpdateFirmware(tft);

  // begin background server for configuration
  configServer.Initialise();

  // initialise the active app (radar or EAM monitor)
  appManager.Initialise();
}

void loop()
{
  // Forget WiFi credentials and reboot into the setup portal when requested.
  if (configServer.ConsumeWifiReset()) {
    wm.resetSettings();
    delay(200); // let the HTTP response flush before the reboot
    ESP.restart();
  }

  // re-check for firmware updates once a day for always-on devices
  static unsigned long lastOtaCheck = 0;
  if (millis() - lastOtaCheck > 24UL * 60UL * 60UL * 1000UL) {
    lastOtaCheck = millis();
    MaybeUpdateFirmware(tft);
  }

  // Apply settings saved via the web UI without rebooting. Done here, on the
  // loop task, so all AircraftManager state changes stay on a single task
  // rather than racing the async web-server callback.
  if (configServer.ConsumeConfigChanged())
    appManager.Initialise();

  appManager.Update();

  // draw cycle: render the frame one horizontal band at a time into the half-height
  // backbuffer, each shifted into place by a BandCanvas, then pushed to its screen
  // rows. The scene is drawn once per band; the app advances per-frame state (animation
  // tick, trail sampling) only on the first pass so the bands stay in sync.
#if !defined(FEATURE_EAM) && !defined(FEATURE_SPACE) && !defined(FEATURE_SEISMIC)
  String renderScanlines = configServer.GetStoredString("scanline");
  const bool drawScan = (renderScanlines.isEmpty() || renderScanlines == "true") && appManager.IsRadarView();

  // The sweep angle is owned by AircraftManager (advanced in Update()), so the
  // drawn beam matches the blip paint-and-fade crossing test exactly. Sampled
  // once here so both render bands derive an identical wedge (no seam).
  const float sweep = appManager.CurrentSweepAngle();
#endif

  for (int bandY = 0; bandY < SCREEN_SIZE; bandY += BAND_H) {
    BandCanvas canvas(backbuffer, bandY);
    const bool firstPass = (bandY == 0);

    canvas.fillScreen(lgfx::color888(0, 0, 0));

#if !defined(FEATURE_EAM) && !defined(FEATURE_SPACE) && !defined(FEATURE_SEISMIC)
    if (drawScan)
      DrawRadarSweep(canvas, SCREEN_SIZE_DIV_2 - 1, SCREEN_SIZE_DIV_2 - 1, SCREEN_SIZE_DIV_2, sweep);
#endif

    appManager.Draw(canvas, firstPass);
    backbuffer.pushSprite(0, bandY);
  }
  // RGB SKUs draw into a cached PSRAM framebuffer; write it back so the panel DMA sees the
  // new frame. No-op on SPI SKUs (the pushSprite above already hit the panel directly).
  board::DisplayFlush(tft);
}

