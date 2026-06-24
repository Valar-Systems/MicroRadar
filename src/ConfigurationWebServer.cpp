#include "ConfigurationWebServer.h"
#include <ESPmDNS.h>
#include "DeviceIdentity.h"
#include "AircraftInfoFields.h"
#include "OtaUpdater.h"

// HTML stored in flash
// %PLACEHOLDER% tokens are substituted at serve time by the template processor
static const char CONFIG_HTML[] PROGMEM = R"(
<html>
    <head>
        <meta name="viewport" content="width=device-width, initial-scale=1">
        <title>Configure Blipscope</title>
        <script src="https://cdn.jsdelivr.net/npm/@tailwindcss/browser@4.3.0"></script>
    </head>
    <body class="font-mono bg-gray-900 text-green-500 min-h-screen p-4 sm:p-0 text-md sm:text-sm">
        <fieldset class="border border-green-500 p-5 w-full max-w-2xl mx-auto sm:m-10">
            <legend class="px-2">Configure Blipscope</legend>

            <form id="cfg" action="/save" method="POST" class="flex flex-col gap-4 sm:gap-2">

                <div class="flex flex-col sm:flex-row gap-4 sm:gap-5">
                    <label class="flex flex-col sm:flex-row gap-2 flex-1">
                        <span>Latitude:</span>
                        <input
                            name="latitude"
                            type="number"
                            min="-90"
                            step="0.000001"
                            max="90"
                            value='%LATITUDE%'
                            class="border border-green-500 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                    </label>

                    <label class="flex flex-col sm:flex-row gap-2 flex-1">
                        <span>Longitude:</span>
                        <input
                            name="longitude"
                            type="number"
                            min="-180"
                            step="0.000001"
                            max="180"
                            value='%LONGITUDE%'
                            class="border border-green-500 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                    </label>
                </div>

                <label class="flex flex-col sm:flex-row items-start sm:items-center gap-2">
                    <span>Radius:</span>
                    <input
                        id="radius"
                        name="radius"
                        type="number"
                        min="0.1"
                        step="0.1"
                        max="222"
                        value='%RADIUS%'
                        class="flex-1 border border-green-500 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                    <select
                        id="radius-unit"
                        name="radius-unit"
                        class="border border-green-500 bg-gray-900 px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                        <option value="km" %RADIUS_UNIT_KM%>km</option>
                        <option value="mi" %RADIUS_UNIT_MI%>mi</option>
                    </select>
                </label>

                <label class="flex flex-col sm:flex-row items-start sm:items-center gap-2">
                    <span>Data source:</span>
                    <select
                        id="data-source"
                        name="data-source"
                        class="flex-1 border border-green-500 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                        <option value="opensky" %DATASRC_OPENSKY%>OpenSky Network (cloud)</option>
                        <option value="local" %DATASRC_LOCAL%>My own ADS-B receiver</option>
                    </select>
                </label>

                <div id="opensky-fields" class="flex flex-col gap-4 sm:gap-2">
                    <label class="flex flex-col sm:flex-row items-start sm:items-center gap-2">
                        <span>OpenSkyAPI Client ID:</span>
                        <input
                            name="opensky-id"
                            value='%OPENSKY_ID%'
                            class="flex-1 border border-green-500 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                    </label>

                    <label class="flex flex-col sm:flex-row items-start sm:items-center gap-2">
                        <span>OpenSkyAPI Client Secret:</span>
                        <input
                            name="opensky-secret"
                            value='%OPENSKY_SECRET%'
                            class="flex-1 border border-green-500 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                    </label>
                </div>

                <div id="local-fields" class="flex flex-col gap-1">
                    <label class="flex flex-col sm:flex-row items-start sm:items-center gap-2">
                        <span>Receiver URL:</span>
                        <input
                            name="local-url"
                            value='%LOCAL_URL%'
                            placeholder="http://192.168.1.50/data/aircraft.json"
                            class="flex-1 border border-green-500 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                    </label>
                    <span class="text-xs text-green-700">
                        dump1090-fa / readsb / PiAware / tar1090. Enter the device's IP (e.g. 192.168.1.50)
                        or the full aircraft.json URL. No API limits &mdash; the radar updates once a second.
                    </span>
                </div>

                <div class="flex flex-col sm:flex-row gap-4 sm:justify-between">
                    <label class="flex flex-col sm:flex-row items-start sm:items-center gap-2">
                        <span>Radar sweep:</span>
                        <input
                            name="scanline"
                            type="checkbox"
                            %SCANLINE%
                            class="px-3 sm:px-1 accent-green-500">
                    </label>
                    <label class="flex flex-col sm:flex-row items-start sm:items-center gap-2">
                        <span>Directional Aircraft:</span>
                        <input
                            name="triangle"
                            type="checkbox"
                            %TRIANGLE%
                            class="px-3 sm:px-1 accent-green-500">
                    </label>
                    <label class="flex flex-col sm:flex-row items-start sm:items-center gap-2">
                        <span>Flight trails:</span>
                        <input
                            name="trail"
                            type="checkbox"
                            %TRAIL%
                            class="px-3 sm:px-1 accent-green-500">
                    </label>
                    <label class="flex flex-col sm:flex-row items-start sm:items-center gap-2">
                        <span>Altitude colors:</span>
                        <input
                            name="altcolor"
                            type="checkbox"
                            %ALTCOLOR%
                            class="px-3 sm:px-1 accent-green-500">
                    </label>
                    <label class="flex flex-col sm:flex-row items-start sm:items-center gap-2">
                        <span>Highlights:</span>
                        <input
                            name="highlight"
                            type="checkbox"
                            %HIGHLIGHT%
                            class="px-3 sm:px-1 accent-green-500">
                    </label>
                    <label class="flex flex-col sm:flex-row items-start sm:items-center gap-2">
                        <span>Auto-dim at night:</span>
                        <input
                            name="autodim"
                            type="checkbox"
                            %AUTODIM%
                            class="px-3 sm:px-1 accent-green-500">
                    </label>
                </div>

                <label class="flex flex-col sm:flex-row items-start sm:items-center gap-2">
                    <span>Brightness:</span>
                    <input
                        name="brightness"
                        type="range"
                        min="10"
                        max="255"
                        value='%BRIGHTNESS%'
                        class="flex-1 w-full accent-green-500">
                </label>

                <label class="flex flex-col sm:flex-row items-start sm:items-center gap-2">
                    <span>Clock UTC offset (hrs):</span>
                    <input
                        name="tz-offset"
                        type="number"
                        min="-12"
                        max="14"
                        step="0.5"
                        value='%TZ_OFFSET%'
                        class="flex-1 border border-green-500 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                </label>

                <fieldset class="border border-green-500 p-3">
                    <legend class="px-2">
                        <label class="flex items-center gap-2">
                            <span>Aircraft Info</span>
                            <input
                                name="infotext"
                                type="checkbox"
                                %INFOTEXT%
                                class="accent-green-500">
                        </label>
                    </legend>
                    <div id="info-fields" class="grid grid-cols-2 sm:grid-cols-3 gap-x-4 gap-y-2">
                        %INFO_FIELDS%
                    </div>
                </fieldset>

                <fieldset class="border border-green-500 p-3">
                    <legend class="px-2">Watchlist &amp; alerts</legend>
                    <label class="flex flex-col gap-1">
                        <span>Watch (callsign / tail / ICAO / type, comma-separated):</span>
                        <textarea
                            name="watchlist"
                            rows="2"
                            class="border border-green-500 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">%WATCHLIST%</textarea>
                    </label>
                    <label class="flex flex-col sm:flex-row sm:items-center gap-2 mt-3">
                        <span>ntfy.sh topic (phone alerts):</span>
                        <input
                            name="ntfy-topic"
                            value='%NTFY_TOPIC%'
                            class="flex-1 border border-green-500 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                    </label>
                    <div class="grid grid-cols-1 sm:grid-cols-2 gap-2 mt-3">
                        <label class="flex items-center gap-2">
                            <input name="mil-show" type="checkbox" %MIL_SHOW% class="accent-green-500">
                            <span>Highlight military</span>
                        </label>
                        <label class="flex items-center gap-2">
                            <input name="mil-alert" type="checkbox" %MIL_ALERT% class="accent-green-500">
                            <span>Alert on military (ntfy)</span>
                        </label>
                        <label class="flex items-center gap-2">
                            <input name="heli-show" type="checkbox" %HELI_SHOW% class="accent-green-500">
                            <span>Highlight helicopters</span>
                        </label>
                        <label class="flex items-center gap-2">
                            <input name="spc-show" type="checkbox" %SPC_SHOW% class="accent-green-500">
                            <span>Highlight special flights</span>
                        </label>
                    </div>
                    <span class="text-xs text-green-700 mt-1">
                        Detected offline from the live feed &mdash; no account or lookup needed. On the radar:
                        military = orange &ldquo;MIL&rdquo;, special flights (rescue / police / NASA / Boeing / Airbus test &hellip;) = blue &ldquo;SPC&rdquo;,
                        helicopters = violet &ldquo;HELI&rdquo;.
                    </span>
                    <div class="flex flex-col sm:flex-row sm:items-center gap-2 mt-3">
                        <label class="flex items-center gap-2">
                            <input name="lookup" type="checkbox" %LOOKUP% class="accent-green-500">
                            <span>&ldquo;Look up!&rdquo; overhead alert within</span>
                        </label>
                        <input
                            name="lookup-dist"
                            type="number"
                            min="0.5"
                            step="0.5"
                            value='%LOOKUP_DIST%'
                            class="border border-green-500 bg-gray-900 w-24 px-2 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                        <label class="flex items-center gap-2">
                            <input name="lookup-alert" type="checkbox" %LOOKUP_ALERT% class="accent-green-500">
                            <span>also ntfy</span>
                        </label>
                    </div>
                    <span class="text-xs text-green-700 mt-1">
                        Flashes a cyan &ldquo;LOOK UP&rdquo; ring when a contact passes within that distance (in your radar's units) of your location &mdash; glance up and spot it.
                    </span>
                </fieldset>

                <fieldset class="border border-green-500 p-3">
                    <legend class="px-2">
                        <label class="flex items-center gap-2">
                            <span>Spotting logbook</span>
                            <input name="logbook" type="checkbox" %LOGBOOK% class="accent-green-500">
                        </label>
                    </legend>
                    <span class="text-xs text-green-700">
                        Keeps a running &ldquo;lifelist&rdquo; of every unique aircraft type, airline, and country
                        you've seen overhead (shown on the Stats screen), and flags a gold &ldquo;NEW&rdquo; on first
                        sightings. It looks up each contact's type/airline, so it adds a little network traffic.
                    </span>
                </fieldset>

                <fieldset class="border border-green-500 p-3">
                    <legend class="px-2">
                        <label class="flex items-center gap-2">
                            <span>Home Assistant / MQTT</span>
                            <input name="mqtt" type="checkbox" %MQTT% class="accent-green-500">
                        </label>
                    </legend>
                    <div class="flex flex-col gap-3 sm:gap-2">
                        <div class="flex flex-col sm:flex-row gap-3 sm:gap-5">
                            <label class="flex flex-col sm:flex-row sm:items-center gap-2 flex-1">
                                <span>Broker:</span>
                                <input name="mqtt-host" value='%MQTT_HOST%' placeholder="192.168.1.10"
                                    class="flex-1 border border-green-500 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                            </label>
                            <label class="flex flex-col sm:flex-row sm:items-center gap-2">
                                <span>Port:</span>
                                <input name="mqtt-port" type="number" min="1" max="65535" value='%MQTT_PORT%'
                                    class="border border-green-500 bg-gray-900 w-24 px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                            </label>
                        </div>
                        <div class="flex flex-col sm:flex-row gap-3 sm:gap-5">
                            <label class="flex flex-col sm:flex-row sm:items-center gap-2 flex-1">
                                <span>Username:</span>
                                <input name="mqtt-user" value='%MQTT_USER%'
                                    class="flex-1 border border-green-500 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                            </label>
                            <label class="flex flex-col sm:flex-row sm:items-center gap-2 flex-1">
                                <span>Password:</span>
                                <input name="mqtt-pass" value='%MQTT_PASS%'
                                    class="flex-1 border border-green-500 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                            </label>
                        </div>
                        <div class="flex flex-col sm:flex-row gap-3 sm:gap-5 sm:items-center">
                            <label class="flex flex-col sm:flex-row sm:items-center gap-2 flex-1">
                                <span>Base topic:</span>
                                <input name="mqtt-base" value='%MQTT_BASE%' placeholder="blipscope"
                                    class="flex-1 border border-green-500 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                            </label>
                            <label class="flex items-center gap-2">
                                <input name="mqtt-disco" type="checkbox" %MQTT_DISCO% class="accent-green-500">
                                <span>HA auto-discovery</span>
                            </label>
                        </div>
                    </div>
                    <span class="text-xs text-green-700 mt-2 block">
                        Publishes a retained &ldquo;&lt;base&gt;/summary&rdquo; (count, nearest aircraft, overhead &amp; military flags)
                        to your broker every few seconds. With auto-discovery on, Home Assistant creates the sensors automatically.
                    </span>
                </fieldset>

                <div class="flex flex-col sm:flex-row gap-4 sm:gap-5">
                    <input
                        type="submit"
                        value="Save"
                        class="bg-green-500 text-black mt-4 px-4 py-3 text-lg sm:text-base sm:px-2 sm:py-0 self-start cursor-pointer">

                    <button
                        type="button"
                        id="resetwifi"
                        class="border border-red-500 text-red-500 mt-4 px-4 py-3 text-lg sm:text-base sm:px-2 sm:py-0 self-start cursor-pointer">
                        Reset WiFi</button>

                        <div id="result" class="mt-4 px-1 sm:px-10"></div>
                </div>
            </form>

            <div class="flex justify-between items-end text-xs text-green-700 mt-4">
                <a href="https://github.com/Valar-Systems/Blipscope/wiki" target="_blank" rel="noopener" class="text-green-500 underline">Help &amp; documentation</a>
                <span>Firmware v%FW_VERSION%</span>
            </div>
        </fieldset>

        <script>
            document.getElementById('cfg').addEventListener('submit', function(e) {
                e.preventDefault();
                fetch(this.action, { method: 'POST', body: new FormData(this) })
                    .then(r => r.text())
                    .then(html => document.getElementById('result').innerHTML = html);
            });

            // Reset WiFi: erase saved credentials and reboot into the setup portal
            document.getElementById('resetwifi').addEventListener('click', function() {
                if (!confirm('Forget WiFi credentials and restart into setup mode? You will need to reconnect the device to a network.')) return;
                fetch('/reset-wifi', { method: 'POST' })
                    .then(r => r.text())
                    .then(html => document.getElementById('result').innerHTML = html);
            });

            // cap the radius at ~2 degrees of scan box (222 km / 138 mi) to stay
            // within OpenSky's rate-limit area, swapping the limit with the unit
            const radiusInput = document.getElementById('radius');
            const radiusUnit = document.getElementById('radius-unit');
            const KM_PER_MILE = 1.609344;
            function updateRadiusMax() {
                radiusInput.max = radiusUnit.value === 'mi' ? '138' : '222';
            }
            radiusUnit.addEventListener('change', function() {
                // the unit just flipped, so convert the displayed value to keep the
                // real-world distance the same: -> mi means it was km, -> km means it was mi
                const value = parseFloat(radiusInput.value);
                if (!isNaN(value)) {
                    const converted = radiusUnit.value === 'mi' ? value / KM_PER_MILE : value * KM_PER_MILE;
                    radiusInput.value = Math.round(converted * 10) / 10;
                }
                updateRadiusMax();
            });
            updateRadiusMax();

            // show only the fields relevant to the selected data source. The hidden
            // block's inputs still submit, but the firmware ignores whichever source
            // isn't selected, so a leftover value does no harm.
            const dataSource = document.getElementById('data-source');
            const openskyFields = document.getElementById('opensky-fields');
            const localFields = document.getElementById('local-fields');
            function syncDataSource() {
                const local = dataSource.value === 'local';
                openskyFields.style.display = local ? 'none' : '';
                localFields.style.display = local ? '' : 'none';
            }
            dataSource.addEventListener('change', syncDataSource);
            syncDataSource();

            // dim the per-field list when the master Aircraft Info toggle is off.
            // purely cosmetic -- the inputs stay enabled so their state still saves.
            const infoMaster = document.querySelector('input[name="infotext"]');
            const infoFields = document.getElementById('info-fields');
            function syncInfoFields() {
                infoFields.style.opacity = infoMaster.checked ? '1' : '0.4';
            }
            infoMaster.addEventListener('change', syncInfoFields);
            syncInfoFields();
        </script>
    </body>
</html>
)";

void ConfigurationWebServer::Initialise() {
    // Create the "config" NVS namespace up front. Opening read-write creates it if
    // missing, so the read-only reads here, in AircraftManager, and every frame in
    // loop() stop logging "nvs_open failed: NOT_FOUND" before the user has ever saved
    // settings. Reads still fall back to their defaults until the config page is used.
    prefs.begin("config", false);
    prefs.end();

    // start mDNS with a per-device hostname (e.g. Blipscope-A1B2C3.local)
    // so multiple boards on the same network don't collide
    if (!MDNS.begin(DeviceIdentity::Name().c_str())) {
        Serial.println("[WARN] Failed to start mDNS. Continuing without mDNS...");
    }

    // Handle visit to config web server
    server.on("/", HTTP_GET, [&](AsyncWebServerRequest* request) {
        Serial.println("[GET] Handling request to config web server...");

        // read all values up front so the processor lambda can capture by value
        prefs.begin("config", true);
        const String latitude = prefs.getString("latitude", "");
        const String longitude = prefs.getString("longitude", "");
        const String radius = prefs.getString("radius", "100");
        // isKey() probes without logging; a plain getString() on this not-yet-saved
        // key spams "nvs_get_str ... NOT_FOUND" on every page load until first save
        const String radiusUnit = prefs.isKey("radius-unit") ? prefs.getString("radius-unit", "km") : "km";
        const String openskyClientId = prefs.getString("opensky-id", "");
        String openskySecret = prefs.getString("opensky-secret", "");
        const String dataSource = prefs.isKey("data-source") ? prefs.getString("data-source", "opensky") : "opensky";
        const String localUrl = prefs.getString("local-url", "");
        const String scanlineEnabled = prefs.getString("scanline", "true");
        const String infoTextEnabled = prefs.getString("infotext", "true");
        const String triangleEnabled = prefs.getString("triangle", "true");
        const String trailEnabled = prefs.getString("trail", "true");
        const String altColorEnabled = prefs.getString("altcolor", "true");
        const String highlightEnabled = prefs.getString("highlight", "true");
        const String autoDimEnabled = prefs.getString("autodim", "true");
        const String brightness = prefs.getString("brightness", "255");
        // default the clock offset to the nominal zone from longitude (15 deg/hour)
        const String tzOffset = prefs.isKey("tz-offset")
            ? prefs.getString("tz-offset", "0")
            : String((int)round(longitude.toFloat() / 15.0));
        const String watchlist = prefs.getString("watchlist", "");
        const String ntfyTopic = prefs.getString("ntfy-topic", "");
        // isKey() guards keep the not-yet-saved reads from logging NVS NOT_FOUND
        const String milShow = prefs.isKey("mil-show") ? prefs.getString("mil-show", "true") : "true";
        const String milAlert = prefs.isKey("mil-alert") ? prefs.getString("mil-alert", "false") : "false";
        const String heliShow = prefs.isKey("heli-show") ? prefs.getString("heli-show", "false") : "false";
        const String spcShow = prefs.isKey("spc-show") ? prefs.getString("spc-show", "false") : "false";
        const String logbookOn = prefs.isKey("logbook") ? prefs.getString("logbook", "false") : "false";
        const String lookupOn = prefs.isKey("lookup") ? prefs.getString("lookup", "false") : "false";
        const String lookupAlert = prefs.isKey("lookup-alert") ? prefs.getString("lookup-alert", "false") : "false";
        const String lookupDist = prefs.isKey("lookup-dist") ? prefs.getString("lookup-dist", "3") : "3";
        const String mqttOn = prefs.isKey("mqtt") ? prefs.getString("mqtt", "false") : "false";
        const String mqttHost = prefs.getString("mqtt-host", "");
        const String mqttPort = prefs.isKey("mqtt-port") ? prefs.getString("mqtt-port", "1883") : "1883";
        const String mqttUser = prefs.getString("mqtt-user", "");
        String mqttPass = prefs.getString("mqtt-pass", "");
        const String mqttBase = prefs.isKey("mqtt-base") ? prefs.getString("mqtt-base", "blipscope") : "blipscope";
        const String mqttDisco = prefs.isKey("mqtt-disco") ? prefs.getString("mqtt-disco", "true") : "true";

        // Build the per-field info checkboxes from the shared table so the form
        // always reflects exactly the fields the renderer knows how to draw.
        String infoFieldsHtml;
        for (size_t i = 0; i < AIRCRAFT_INFO_FIELD_COUNT; ++i) {
            const AircraftInfoFieldDef& field = AIRCRAFT_INFO_FIELDS[i];
            const bool checked = prefs.isKey(field.key)
                ? (prefs.getString(field.key, "") == "true")
                : field.defaultOn;
            infoFieldsHtml += F("<label class=\"flex items-center gap-2\"><input type=\"checkbox\" class=\"accent-green-500\" name=\"");
            infoFieldsHtml += field.key;
            infoFieldsHtml += '"';
            if (checked) infoFieldsHtml += F(" checked");
            infoFieldsHtml += F("><span>");
            infoFieldsHtml += field.label;
            infoFieldsHtml += F("</span></label>");
        }
        prefs.end();

        // mask secrets before sending to client
        std::fill(openskySecret.begin(), openskySecret.end(), '*');
        std::fill(mqttPass.begin(), mqttPass.end(), '*');

        // template processor called once per %PLACEHOLDER% token found in CONFIG_HTML.
        AsyncWebServerResponse* response = request->beginResponse(
            200, "text/html",
            (const uint8_t*)CONFIG_HTML, sizeof(CONFIG_HTML) - 1,
            [latitude, longitude, radius, radiusUnit, openskyClientId, openskySecret, dataSource, localUrl, scanlineEnabled, infoTextEnabled, triangleEnabled, trailEnabled, altColorEnabled, highlightEnabled, autoDimEnabled, brightness, tzOffset, watchlist, ntfyTopic, milShow, milAlert, heliShow, spcShow, logbookOn, lookupOn, lookupAlert, lookupDist, mqttOn, mqttHost, mqttPort, mqttUser, mqttPass, mqttBase, mqttDisco, infoFieldsHtml]
            (const String& var) -> String {
                if (var == "LATITUDE")       return latitude;
                if (var == "LONGITUDE")      return longitude;
                if (var == "RADIUS")         return radius;
                if (var == "RADIUS_UNIT_KM") return radiusUnit == "mi" ? "" : "selected";
                if (var == "RADIUS_UNIT_MI") return radiusUnit == "mi" ? "selected" : "";
                if (var == "OPENSKY_ID")     return openskyClientId;
                if (var == "OPENSKY_SECRET") return openskySecret;
                if (var == "DATASRC_OPENSKY") return dataSource == "local" ? "" : "selected";
                if (var == "DATASRC_LOCAL")   return dataSource == "local" ? "selected" : "";
                if (var == "LOCAL_URL")      return localUrl;
                if (var == "SCANLINE")       return scanlineEnabled == "true" ? "checked" : "";
                if (var == "INFOTEXT")       return infoTextEnabled == "true" ? "checked" : "";
                if (var == "TRIANGLE")       return triangleEnabled == "true" ? "checked" : "";
                if (var == "TRAIL")          return trailEnabled == "true" ? "checked" : "";
                if (var == "ALTCOLOR")       return altColorEnabled == "true" ? "checked" : "";
                if (var == "HIGHLIGHT")      return highlightEnabled == "true" ? "checked" : "";
                if (var == "AUTODIM")        return autoDimEnabled == "true" ? "checked" : "";
                if (var == "BRIGHTNESS")     return brightness;
                if (var == "TZ_OFFSET")      return tzOffset;
                if (var == "WATCHLIST")      return watchlist;
                if (var == "NTFY_TOPIC")     return ntfyTopic;
                if (var == "MIL_SHOW")       return milShow == "true" ? "checked" : "";
                if (var == "MIL_ALERT")      return milAlert == "true" ? "checked" : "";
                if (var == "HELI_SHOW")      return heliShow == "true" ? "checked" : "";
                if (var == "SPC_SHOW")       return spcShow == "true" ? "checked" : "";
                if (var == "LOGBOOK")        return logbookOn == "true" ? "checked" : "";
                if (var == "LOOKUP")         return lookupOn == "true" ? "checked" : "";
                if (var == "LOOKUP_ALERT")   return lookupAlert == "true" ? "checked" : "";
                if (var == "LOOKUP_DIST")    return lookupDist;
                if (var == "MQTT")           return mqttOn == "true" ? "checked" : "";
                if (var == "MQTT_HOST")      return mqttHost;
                if (var == "MQTT_PORT")      return mqttPort;
                if (var == "MQTT_USER")      return mqttUser;
                if (var == "MQTT_PASS")      return mqttPass;
                if (var == "MQTT_BASE")      return mqttBase;
                if (var == "MQTT_DISCO")     return mqttDisco == "true" ? "checked" : "";
                if (var == "INFO_FIELDS")    return infoFieldsHtml;
                if (var == "FW_VERSION")     return String(FW_VERSION);
                return "";
            }
        );
        // never cache the config page: a stale copy (e.g. predating a new option)
        // would hide controls and, once submitted, silently clear the missing fields
        response->addHeader("Cache-Control", "no-store");
        request->send(response);
        }
    );

    // Handle save submission to web server
    server.on("/save", HTTP_POST, [&](AsyncWebServerRequest* request) {
        Serial.println("[POST] Handling form submission to config web server...");

        // safe parameter retrieval helper lambda
        auto TrySaveParam = [request, this](const char* paramName) {
            const auto* param = request->getParam(paramName, true);
            if (param == nullptr)
                return false;

            prefs.putString(paramName, param->value());
            return true;
            };

        prefs.begin("config", false);

        TrySaveParam("latitude");
        TrySaveParam("longitude");
        TrySaveParam("radius");
        TrySaveParam("radius-unit");
        TrySaveParam("brightness");
        TrySaveParam("tz-offset");
        TrySaveParam("watchlist");
        TrySaveParam("ntfy-topic");
        TrySaveParam("opensky-id");
        TrySaveParam("data-source");
        TrySaveParam("local-url");
        TrySaveParam("lookup-dist");
        TrySaveParam("mqtt-host");
        TrySaveParam("mqtt-port");
        TrySaveParam("mqtt-user");
        TrySaveParam("mqtt-base");

        const auto* param = request->getParam("opensky-secret", true);
        if (param != nullptr) {
            const String& secret = param->value();
            if (secret.indexOf('*') == -1) { // Special handling for secret: don't overwrite with masked value
                prefs.putString("opensky-secret", secret);
            }
        }

        // MQTT password: same masked-value handling as the OpenSky secret
        const auto* mqttPassParam = request->getParam("mqtt-pass", true);
        if (mqttPassParam != nullptr) {
            const String& pass = mqttPassParam->value();
            if (pass.indexOf('*') == -1)
                prefs.putString("mqtt-pass", pass);
        }

        prefs.putString("scanline", request->hasParam("scanline", true) ? "true" : "false");
        prefs.putString("triangle", request->hasParam("triangle", true) ? "true" : "false");
        prefs.putString("trail", request->hasParam("trail", true) ? "true" : "false");
        prefs.putString("altcolor", request->hasParam("altcolor", true) ? "true" : "false");
        prefs.putString("highlight", request->hasParam("highlight", true) ? "true" : "false");
        prefs.putString("autodim", request->hasParam("autodim", true) ? "true" : "false");
        prefs.putString("infotext", request->hasParam("infotext", true) ? "true" : "false");
        prefs.putString("mil-show", request->hasParam("mil-show", true) ? "true" : "false");
        prefs.putString("mil-alert", request->hasParam("mil-alert", true) ? "true" : "false");
        prefs.putString("heli-show", request->hasParam("heli-show", true) ? "true" : "false");
        prefs.putString("spc-show", request->hasParam("spc-show", true) ? "true" : "false");
        prefs.putString("logbook", request->hasParam("logbook", true) ? "true" : "false");
        prefs.putString("lookup", request->hasParam("lookup", true) ? "true" : "false");
        prefs.putString("lookup-alert", request->hasParam("lookup-alert", true) ? "true" : "false");
        prefs.putString("mqtt", request->hasParam("mqtt", true) ? "true" : "false");
        prefs.putString("mqtt-disco", request->hasParam("mqtt-disco", true) ? "true" : "false");

        // an unchecked checkbox isn't sent in the form body, so hasParam() is the
        // on/off signal for each individual info field
        for (size_t i = 0; i < AIRCRAFT_INFO_FIELD_COUNT; ++i) {
            const char* key = AIRCRAFT_INFO_FIELDS[i].key;
            prefs.putString(key, request->hasParam(key, true) ? "true" : "false");
        }
        prefs.end();

        // No reboot: flag the change and let loop() re-read settings on the main
        // task. NVS is already committed by the putString() calls above, so the
        // reload will see the new values.
        configChanged = true;
        request->send(200, "text/html", "Saved - settings applied.");
        }
    );

    // Forget WiFi credentials and reboot into the WiFiManager setup portal. The
    // response is sent first; the restart is deferred a moment so it can flush.
    server.on("/reset-wifi", HTTP_POST, [&](AsyncWebServerRequest* request) {
        Serial.println("[POST] Clearing WiFi credentials and restarting...");
        request->send(200, "text/html", "WiFi cleared - restarting into setup mode. Reconnect to the device's setup network.");
        wifiResetRequested = true;
        }
    );

    server.begin();
}

bool ConfigurationWebServer::ConsumeConfigChanged()
{
    if (!configChanged)
        return false;
    configChanged = false;
    return true;
}

bool ConfigurationWebServer::ConsumeWifiReset()
{
    if (!wifiResetRequested)
        return false;
    wifiResetRequested = false;
    return true;
}

const String ConfigurationWebServer::GetStoredString(const char* key)
{
    prefs.begin("config", true);
    // isKey() probes without logging; calling getString() on a missing key would spam
    // "nvs_get_str ... NOT_FOUND" on every call (e.g. every frame for "scanline") until
    // the user first saves settings. Returns the same "" default as before when absent.
    const String value = prefs.isKey(key) ? prefs.getString(key, "") : String();
    prefs.end();
    return value;
}