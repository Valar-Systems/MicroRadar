#include "ConfigurationWebServer.h"
#include <ESPmDNS.h>
#include "DeviceIdentity.h"
#include "OtaUpdater.h"
#if !defined(FEATURE_EAM) && !defined(FEATURE_SPACE) && !defined(FEATURE_SEISMIC)
#include "AircraftInfoFields.h"   // radar-only; filtered out of the FEATURE_EAM/FEATURE_SPACE builds
#endif

#ifdef FEATURE_EAM
#include "eam/EamLogbook.h"        // serves the on-device logbook as a CSV/JSON download
// The EAM build's backend base URL default. Normally injected per-env (-DEAM_FEED_BASE=...);
// guarded so the file still compiles without it. The runtime value ("eam-base-url") overrides.
#ifndef EAM_FEED_BASE
#define EAM_FEED_BASE "https://eam.example.com"
#endif
#endif

#ifdef FEATURE_SPACE
// The Spacescope build's optional backend base URL default. Empty by default: the device talks
// directly to free public space APIs and bakes in no backend. The runtime value ("space-base-url")
// overrides it (Phase-3 valar-space-feed). Guarded so the file compiles without the flag.
#ifndef SPACE_FEED_BASE
#define SPACE_FEED_BASE ""
#endif
#endif

// HTML stored in flash
// %PLACEHOLDER% tokens are substituted at serve time by the template processor.
// The page is feature-specific: the radar build serves the radar settings form below; the
// FEATURE_EAM build serves the EAM monitor form; the FEATURE_SPACE build serves the Spacescope
// form. The ConfigurationWebServer shell (NVS namespace, mDNS, /reset-wifi, save flag) is shared.
#if !defined(FEATURE_EAM) && !defined(FEATURE_SPACE) && !defined(FEATURE_SEISMIC)
static const char CONFIG_HTML[] PROGMEM = R"(
<html>
    <head>
        <meta name="viewport" content="width=device-width, initial-scale=1">
        <title>Configure Blipscope</title>
        <!-- inline SVG favicon (radar blip) so the tab is easy to spot; no extra flash asset / route needed.
             Colors use rgb() not #-hex on purpose: a "#" in a data URI must be %-encoded as %23, and any
             stray "%" collides with this page's %PLACEHOLDER% template engine and shreds the whole form. -->
        <link rel="icon" href="data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 16 16'><rect width='16' height='16' rx='3' fill='rgb(17,24,39)'/><circle cx='8' cy='8' r='5.5' fill='none' stroke='rgb(34,197,94)' stroke-width='1'/><circle cx='8' cy='8' r='1.7' fill='rgb(34,197,94)'/></svg>">
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
                        <span>Sweep fade:</span>
                        <input
                            name="fade"
                            type="checkbox"
                            %FADE%
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
#elif defined(FEATURE_EAM)
// FEATURE_EAM config page. Stage 1: the valar-eam-feed backend base URL + Reset WiFi.
// Per-screen toggles/reorder, the command-post source dropdown, OpenSky credentials, ntfy,
// poller intervals, and lat/lon arrive in a later stage. Shares the page chrome/JS pattern.
static const char CONFIG_HTML[] PROGMEM = R"(
<html>
    <head>
        <meta name="viewport" content="width=device-width, initial-scale=1">
        <title>Configure Blipscope EAM</title>
        <link rel="icon" href="data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 16 16'><rect width='16' height='16' rx='3' fill='rgb(17,24,39)'/><circle cx='8' cy='8' r='5.5' fill='none' stroke='rgb(34,197,94)' stroke-width='1'/><circle cx='8' cy='8' r='1.7' fill='rgb(34,197,94)'/></svg>">
        <script src="https://cdn.jsdelivr.net/npm/@tailwindcss/browser@4.3.0"></script>
    </head>
    <body class="font-mono bg-gray-900 text-green-500 min-h-screen p-4 sm:p-0 text-md sm:text-sm">
        <fieldset class="border border-green-500 p-5 w-full max-w-2xl mx-auto sm:m-10">
            <legend class="px-2">Configure Blipscope EAM</legend>

            <form id="cfg" action="/save" method="POST" class="flex flex-col gap-4 sm:gap-2">

                <label class="flex flex-col sm:flex-row items-start sm:items-center gap-2">
                    <span>EAM feed base URL:</span>
                    <input name="eam-base-url" value='%EAM_BASE_URL%' placeholder="https://eam.example.com"
                        class="flex-1 border border-green-500 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                </label>
                <span class="text-xs text-green-700">The valar-eam-feed backend this device polls for EAM / Skyking / tempo / propagation / launch data.</span>

                <div class="flex flex-col sm:flex-row gap-4 sm:gap-5">
                    <label class="flex flex-col sm:flex-row gap-2 flex-1">
                        <span>Latitude:</span>
                        <input name="latitude" type="number" min="-90" step="0.000001" max="90" value='%LATITUDE%'
                            class="border border-green-500 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                    </label>
                    <label class="flex flex-col sm:flex-row gap-2 flex-1">
                        <span>Longitude:</span>
                        <input name="longitude" type="number" min="-180" step="0.000001" max="180" value='%LONGITUDE%'
                            class="border border-green-500 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                    </label>
                </div>
                <span class="text-xs text-green-700">Optional. Used for propagation day/night and the command-post bearing/distance.</span>

                <fieldset class="border border-green-500 p-3">
                    <legend class="px-2">Command-post watch</legend>
                    <label class="flex flex-col sm:flex-row items-start sm:items-center gap-2">
                        <span>Source:</span>
                        <select id="abncp-source" name="abncp-source"
                            class="flex-1 border border-green-500 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                            <option value="backend" %ABNCP_BACKEND%>adsb.lol &mdash; via Valar feed (no setup)</option>
                            <option value="opensky" %ABNCP_OPENSKY%>OpenSky &mdash; your account</option>
                        </select>
                    </label>
                    <div id="opensky-fields" class="flex flex-col gap-3 mt-3">
                        <label class="flex flex-col sm:flex-row sm:items-center gap-2">
                            <span>OpenSky client ID:</span>
                            <input name="opensky-id" value='%OPENSKY_ID%'
                                class="flex-1 border border-green-500 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                        </label>
                        <label class="flex flex-col sm:flex-row sm:items-center gap-2">
                            <span>OpenSky client secret:</span>
                            <input name="opensky-secret" value='%OPENSKY_SECRET%'
                                class="flex-1 border border-green-500 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                        </label>
                        <label class="flex flex-col gap-1">
                            <span>ICAO24 watchlist (hex, comma-separated):</span>
                            <textarea name="abncp-watch" rows="2"
                                class="border border-green-500 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">%ABNCP_WATCH%</textarea>
                        </label>
                        <span class="text-xs text-green-700">
                            Queried from this device with YOUR OpenSky account only &mdash; never shared, never routed through the Valar backend.
                            Seeded with the E-4B &ldquo;Nightwatch&rdquo; hexes (verify them); add E-6B hexes as needed. Blank ID/secret keeps the watch off.
                        </span>
                    </div>
                </fieldset>

                <fieldset class="border border-green-500 p-3">
                    <legend class="px-2">Alerts (ntfy)</legend>
                    <label class="flex flex-col sm:flex-row sm:items-center gap-2">
                        <span>ntfy.sh topic:</span>
                        <input name="ntfy-topic" value='%NTFY_TOPIC%'
                            class="flex-1 border border-green-500 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                    </label>
                    <div class="grid grid-cols-1 sm:grid-cols-2 gap-2 mt-3">
                        <label class="flex items-center gap-2"><input name="eam-alert-new" type="checkbox" %ALERT_NEW% class="accent-green-500"><span>New EAM</span></label>
                        <label class="flex items-center gap-2"><input name="eam-alert-tempo" type="checkbox" %ALERT_TEMPO% class="accent-green-500"><span>Tempo elevated/high</span></label>
                        <label class="flex items-center gap-2"><input name="eam-alert-abncp" type="checkbox" %ALERT_ABNCP% class="accent-green-500"><span>Command post airborne</span></label>
                        <label class="flex items-center gap-2"><input name="eam-alert-space" type="checkbox" %ALERT_SPACE% class="accent-green-500"><span>Space weather (HF blackout / storm)</span></label>
                    </div>
                    <span class="text-xs text-green-700 mt-1">Leave the topic blank to disable all push alerts.</span>
                </fieldset>

                <fieldset class="border border-green-500 p-3">
                    <legend class="px-2">Display</legend>
                    <div class="flex flex-col sm:flex-row gap-3 sm:gap-5">
                        <label class="flex flex-col sm:flex-row sm:items-center gap-2 flex-1">
                            <span>Palette:</span>
                            <select name="eam-palette" class="flex-1 border border-green-500 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                                <option value="green" %PAL_GREEN%>Green console</option>
                                <option value="amber" %PAL_AMBER%>Amber console</option>
                            </select>
                        </label>
                        <label class="flex flex-col sm:flex-row sm:items-center gap-2 flex-1">
                            <span>Refresh:</span>
                            <select name="eam-refresh" class="flex-1 border border-green-500 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                                <option value="normal" %RR_NORMAL%>Normal</option>
                                <option value="relaxed" %RR_RELAXED%>Relaxed (2x)</option>
                                <option value="battery" %RR_BATTERY%>Battery (4x)</option>
                            </select>
                        </label>
                    </div>
                    <div class="flex flex-col sm:flex-row gap-4 sm:gap-8 mt-3">
                        <label class="flex items-center gap-2"><input name="eam-colon-blink" type="checkbox" %COLON_BLINK% class="accent-green-500"><span>Clock colon blink</span></label>
                        <label class="flex items-center gap-2"><input name="autodim" type="checkbox" %AUTODIM% class="accent-green-500"><span>Auto-dim at night</span></label>
                    </div>
                    <label class="flex flex-col sm:flex-row items-start sm:items-center gap-2 mt-3">
                        <span>Brightness:</span>
                        <input name="brightness" type="range" min="10" max="255" value='%BRIGHTNESS%' class="flex-1 w-full accent-green-500">
                    </label>
                </fieldset>

                <fieldset class="border border-green-500 p-3">
                    <legend class="px-2">Screens</legend>
                    <label class="flex flex-col gap-1">
                        <span>Order &amp; enable (comma-separated; omit one to hide it):</span>
                        <input name="eam-screens" value='%EAM_SCREENS%'
                            class="border border-green-500 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                    </label>
                    <span class="text-xs text-green-700 mt-1">ids: ticker, tempo, activity, codewords, abncp, milair, prop, icbm, ref, clock. Empty rotates all. Activity and milair appear only when their feed has data; the clock always shows when nothing else does.</span>
                </fieldset>

                <fieldset class="border border-green-500 p-3">
                    <legend class="px-2">Logbook</legend>
                    <span class="text-xs text-green-700">Download the EAMs &amp; Skyking codewords this device has logged (codewords carry timestamps).</span>
                    <div class="flex gap-5 mt-2">
                        <a href="/eam-log.csv" class="text-green-500 underline">Download CSV</a>
                        <a href="/eam-log.json" class="text-green-500 underline">Download JSON</a>
                    </div>
                </fieldset>

                <div class="flex flex-col sm:flex-row gap-4 sm:gap-5">
                    <input type="submit" value="Save"
                        class="bg-green-500 text-black mt-4 px-4 py-3 text-lg sm:text-base sm:px-2 sm:py-0 self-start cursor-pointer">
                    <button type="button" id="resetwifi"
                        class="border border-red-500 text-red-500 mt-4 px-4 py-3 text-lg sm:text-base sm:px-2 sm:py-0 self-start cursor-pointer">
                        Reset WiFi</button>
                    <div id="result" class="mt-4 px-1 sm:px-10"></div>
                </div>
            </form>

            <div class="flex justify-between items-end text-xs text-green-700 mt-4">
                <a href="https://github.com/Valar-Systems/Blipscope/wiki" target="_blank" rel="noopener" class="text-green-500 underline">Help &amp; documentation</a>
                <span>Firmware v%FW_VERSION% (EAM)</span>
            </div>
        </fieldset>

        <script>
            document.getElementById('cfg').addEventListener('submit', function(e) {
                e.preventDefault();
                fetch(this.action, { method: 'POST', body: new FormData(this) })
                    .then(r => r.text())
                    .then(html => document.getElementById('result').innerHTML = html);
            });
            document.getElementById('resetwifi').addEventListener('click', function() {
                if (!confirm('Forget WiFi credentials and restart into setup mode? You will need to reconnect the device to a network.')) return;
                fetch('/reset-wifi', { method: 'POST' })
                    .then(r => r.text())
                    .then(html => document.getElementById('result').innerHTML = html);
            });
            // show the OpenSky credential fields only when that source is selected
            const abncpSrc = document.getElementById('abncp-source');
            const openskyFields = document.getElementById('opensky-fields');
            function syncAbncp() { openskyFields.style.display = abncpSrc.value === 'opensky' ? '' : 'none'; }
            abncpSrc.addEventListener('change', syncAbncp);
            syncAbncp();
        </script>
    </body>
</html>
)";
#elif defined(FEATURE_SPACE)
// FEATURE_SPACE (Spacescope) config page. Stage 1: location, optional backend, screen order,
// ntfy alerts, display. Per-source API keys (Launch Library / NASA) and finer per-screen options
// arrive with the screens that use them. Shares the page chrome / JS pattern.
static const char CONFIG_HTML[] PROGMEM = R"(
<html>
    <head>
        <meta name="viewport" content="width=device-width, initial-scale=1">
        <title>Configure Spacescope</title>
        <link rel="icon" href="data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 16 16'><rect width='16' height='16' rx='3' fill='rgb(8,12,28)'/><circle cx='8' cy='8' r='2' fill='rgb(120,200,255)'/><circle cx='8' cy='8' r='5.5' fill='none' stroke='rgb(120,200,255)' stroke-width='0.8'/><circle cx='13' cy='4' r='1' fill='rgb(255,255,255)'/></svg>">
        <script src="https://cdn.jsdelivr.net/npm/@tailwindcss/browser@4.3.0"></script>
    </head>
    <body class="font-mono bg-gray-900 text-sky-300 min-h-screen p-4 sm:p-0 text-md sm:text-sm">
        <fieldset class="border border-sky-400 p-5 w-full max-w-2xl mx-auto sm:m-10">
            <legend class="px-2">Configure Spacescope</legend>

            <form id="cfg" action="/save" method="POST" class="flex flex-col gap-4 sm:gap-2">

                <div class="flex flex-col sm:flex-row gap-4 sm:gap-5">
                    <label class="flex flex-col sm:flex-row gap-2 flex-1">
                        <span>Latitude:</span>
                        <input name="latitude" type="number" min="-90" step="0.000001" max="90" value='%LATITUDE%'
                            class="border border-sky-400 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                    </label>
                    <label class="flex flex-col sm:flex-row gap-2 flex-1">
                        <span>Longitude:</span>
                        <input name="longitude" type="number" min="-180" step="0.000001" max="180" value='%LONGITUDE%'
                            class="border border-sky-400 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                    </label>
                </div>
                <span class="text-xs text-sky-600">Optional, but unlocks the location-aware screens: next visible ISS pass, local aurora odds, and the solar night auto-dim.</span>

                <fieldset class="border border-sky-400 p-3">
                    <legend class="px-2">Alerts (ntfy)</legend>
                    <label class="flex flex-col sm:flex-row sm:items-center gap-2">
                        <span>ntfy.sh topic:</span>
                        <input name="ntfy-topic" value='%NTFY_TOPIC%'
                            class="flex-1 border border-sky-400 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                    </label>
                    <div class="grid grid-cols-1 sm:grid-cols-2 gap-2 mt-3">
                        <label class="flex items-center gap-2"><input name="sp-alert-launch" type="checkbox" %AL_LAUNCH% class="accent-sky-400"><span>Launch imminent (T-10 / T-1)</span></label>
                        <label class="flex items-center gap-2"><input name="sp-alert-aurora" type="checkbox" %AL_AURORA% class="accent-sky-400"><span>Aurora likely (high Kp)</span></label>
                        <label class="flex items-center gap-2"><input name="sp-alert-flare" type="checkbox" %AL_FLARE% class="accent-sky-400"><span>Solar flare (M+ class)</span></label>
                        <label class="flex items-center gap-2"><input name="sp-alert-iss" type="checkbox" %AL_ISS% class="accent-sky-400"><span>ISS passing overhead</span></label>
                        <label class="flex items-center gap-2"><input name="sp-alert-dsn" type="checkbox" %AL_DSN% class="accent-sky-400"><span>Deep-space probe contact (DSN)</span></label>
                    </div>
                    <span class="text-xs text-sky-600 mt-1">Leave the topic blank to disable all push alerts. ISS / aurora alerts need a location above.</span>
                </fieldset>

                <fieldset class="border border-sky-400 p-3">
                    <legend class="px-2">Display</legend>
                    <div class="flex flex-col sm:flex-row gap-4 sm:gap-8">
                        <label class="flex items-center gap-2"><input name="autodim" type="checkbox" %AUTODIM% class="accent-sky-400"><span>Auto-dim at night</span></label>
                    </div>
                    <label class="flex flex-col sm:flex-row items-start sm:items-center gap-2 mt-3">
                        <span>Brightness:</span>
                        <input name="brightness" type="range" min="10" max="255" value='%BRIGHTNESS%' class="flex-1 w-full accent-sky-400">
                    </label>
                </fieldset>

                <fieldset class="border border-sky-400 p-3">
                    <legend class="px-2">Screens</legend>
                    <label class="flex flex-col gap-1">
                        <span>Order &amp; enable (comma-separated; omit one to hide it):</span>
                        <input name="space-screens" value='%SPACE_SCREENS%'
                            class="border border-sky-400 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                    </label>
                    <span class="text-xs text-sky-600 mt-1">ids: iss, launch, kp, solarwind, scales, flare, aurora, dsn, deepspace, humans, moon, splash, clock. Empty rotates all. Each screen appears only when its feed has data; the clock and moon always show. aurora needs a location. (More screens arrive in upcoming firmware.)</span>
                </fieldset>

                <fieldset class="border border-sky-400 p-3">
                    <legend class="px-2">Advanced</legend>
                    <label class="flex flex-col sm:flex-row items-start sm:items-center gap-2">
                        <span>Backend base URL:</span>
                        <input name="space-base-url" value='%SPACE_BASE_URL%' placeholder="blank = direct public APIs"
                            class="flex-1 border border-sky-400 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                    </label>
                    <span class="text-xs text-sky-600">Optional. Leave blank and Spacescope pulls straight from free public space APIs. Point it at a valar-space-feed backend to offload the heavy / key-gated sources.</span>
                </fieldset>

                <div class="flex flex-col sm:flex-row gap-4 sm:gap-5">
                    <input type="submit" value="Save"
                        class="bg-sky-400 text-black mt-4 px-4 py-3 text-lg sm:text-base sm:px-2 sm:py-0 self-start cursor-pointer">
                    <button type="button" id="resetwifi"
                        class="border border-red-500 text-red-500 mt-4 px-4 py-3 text-lg sm:text-base sm:px-2 sm:py-0 self-start cursor-pointer">
                        Reset WiFi</button>
                    <div id="result" class="mt-4 px-1 sm:px-10"></div>
                </div>
            </form>

            <div class="flex justify-between items-end text-xs text-sky-600 mt-4">
                <a href="https://github.com/Valar-Systems/Blipscope/wiki" target="_blank" rel="noopener" class="text-sky-300 underline">Help &amp; documentation</a>
                <span>Firmware v%FW_VERSION% (Space)</span>
            </div>
        </fieldset>

        <script>
            document.getElementById('cfg').addEventListener('submit', function(e) {
                e.preventDefault();
                fetch(this.action, { method: 'POST', body: new FormData(this) })
                    .then(r => r.text())
                    .then(html => document.getElementById('result').innerHTML = html);
            });
            document.getElementById('resetwifi').addEventListener('click', function() {
                if (!confirm('Forget WiFi credentials and restart into setup mode? You will need to reconnect the device to a network.')) return;
                fetch('/reset-wifi', { method: 'POST' })
                    .then(r => r.text())
                    .then(html => document.getElementById('result').innerHTML = html);
            });
        </script>
    </body>
</html>
)";
#elif defined(FEATURE_SEISMIC)
// FEATURE_SEISMIC (Seismic edition) config page: location, radar magnitude/radius, ntfy alerts,
// display, and an optional backend. Shares the page chrome / JS pattern with the other editions.
static const char CONFIG_HTML[] PROGMEM = R"(
<html>
    <head>
        <meta name="viewport" content="width=device-width, initial-scale=1">
        <title>Configure Blipscope Seismic</title>
        <link rel="icon" href="data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 16 16'><rect width='16' height='16' rx='3' fill='rgb(24,14,4)'/><path d='M1 8 L4 8 L5 3 L7 13 L9 6 L10.5 8 L15 8' fill='none' stroke='rgb(255,170,0)' stroke-width='1.2'/></svg>">
        <script src="https://cdn.jsdelivr.net/npm/@tailwindcss/browser@4.3.0"></script>
    </head>
    <body class="font-mono bg-gray-900 text-amber-300 min-h-screen p-4 sm:p-0 text-md sm:text-sm">
        <fieldset class="border border-amber-400 p-5 w-full max-w-2xl mx-auto sm:m-10">
            <legend class="px-2">Configure Blipscope &mdash; Seismic</legend>

            <form id="cfg" action="/save" method="POST" class="flex flex-col gap-4 sm:gap-2">

                <div class="flex flex-col sm:flex-row gap-4 sm:gap-5">
                    <label class="flex flex-col sm:flex-row gap-2 flex-1">
                        <span>Latitude:</span>
                        <input name="latitude" type="number" min="-90" step="0.000001" max="90" value='%LATITUDE%'
                            class="border border-amber-400 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                    </label>
                    <label class="flex flex-col sm:flex-row gap-2 flex-1">
                        <span>Longitude:</span>
                        <input name="longitude" type="number" min="-180" step="0.000001" max="180" value='%LONGITUDE%'
                            class="border border-amber-400 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                    </label>
                </div>
                <span class="text-xs text-amber-600">Your location centres the quake radar, the "near me" feed and alerts, and the solar night auto-dim. Without it you still get the worldwide list and stats.</span>

                <fieldset class="border border-amber-400 p-3">
                    <legend class="px-2">Radar</legend>
                    <div class="flex flex-col sm:flex-row gap-4 sm:gap-5">
                        <label class="flex flex-col sm:flex-row gap-2 flex-1">
                            <span>Min magnitude (worldwide):</span>
                            <input name="se-min-mag" type="number" min="0" max="9" step="0.1" value='%SE_MIN_MAG%'
                                class="border border-amber-400 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                        </label>
                        <label class="flex flex-col sm:flex-row gap-2 flex-1">
                            <span>Radar radius (km):</span>
                            <input name="se-radius-km" type="number" min="50" max="20000" step="10" value='%SE_RADIUS%'
                                class="border border-amber-400 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                        </label>
                    </div>
                </fieldset>

                <fieldset class="border border-amber-400 p-3">
                    <legend class="px-2">Alerts (ntfy)</legend>
                    <label class="flex flex-col sm:flex-row sm:items-center gap-2">
                        <span>ntfy.sh topic:</span>
                        <input name="ntfy-topic" value='%NTFY_TOPIC%'
                            class="flex-1 border border-amber-400 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                    </label>
                    <div class="grid grid-cols-1 gap-2 mt-3">
                        <label class="flex items-center gap-2"><input name="se-alert-big" type="checkbox" %AL_BIG% class="accent-amber-400"><span>Big quake worldwide, M &ge;</span>
                            <input name="se-big-mag" type="number" min="0" max="9" step="0.1" value='%SE_BIG_MAG%' class="border border-amber-400 bg-gray-900 w-16 px-2 sm:py-0"></label>
                        <label class="flex items-center gap-2"><input name="se-alert-near" type="checkbox" %AL_NEAR% class="accent-amber-400"><span>Quake near me, M &ge;</span>
                            <input name="se-near-mag" type="number" min="0" max="9" step="0.1" value='%SE_NEAR_MAG%' class="border border-amber-400 bg-gray-900 w-16 px-2 sm:py-0"></label>
                        <label class="flex items-center gap-2"><input name="se-alert-tsunami" type="checkbox" %AL_TSUNAMI% class="accent-amber-400"><span>Tsunami-flagged quake</span></label>
                    </div>
                    <span class="text-xs text-amber-600 mt-1">Leave the topic blank to disable all push alerts. The "near me" alert needs a location above.</span>
                </fieldset>

                <fieldset class="border border-amber-400 p-3">
                    <legend class="px-2">Display</legend>
                    <div class="flex flex-col sm:flex-row gap-4 sm:gap-8">
                        <label class="flex items-center gap-2"><input name="autodim" type="checkbox" %AUTODIM% class="accent-amber-400"><span>Auto-dim at night</span></label>
                    </div>
                    <label class="flex flex-col sm:flex-row items-start sm:items-center gap-2 mt-3">
                        <span>Brightness:</span>
                        <input name="brightness" type="range" min="10" max="255" value='%BRIGHTNESS%' class="flex-1 w-full accent-amber-400">
                    </label>
                </fieldset>

                <fieldset class="border border-amber-400 p-3">
                    <legend class="px-2">Advanced</legend>
                    <label class="flex flex-col sm:flex-row items-start sm:items-center gap-2">
                        <span>Backend base URL:</span>
                        <input name="se-base-url" value='%SE_BASE_URL%' placeholder="blank = USGS directly"
                            class="flex-1 border border-amber-400 bg-gray-900 w-full px-3 py-2 text-lg sm:text-base sm:px-1 sm:py-0">
                    </label>
                    <span class="text-xs text-amber-600">Optional. Leave blank and the device pulls straight from the public USGS earthquake API.</span>
                </fieldset>

                <div class="flex flex-col sm:flex-row gap-4 sm:gap-5">
                    <input type="submit" value="Save"
                        class="bg-amber-400 text-black mt-4 px-4 py-3 text-lg sm:text-base sm:px-2 sm:py-0 self-start cursor-pointer">
                    <button type="button" id="resetwifi"
                        class="border border-red-500 text-red-500 mt-4 px-4 py-3 text-lg sm:text-base sm:px-2 sm:py-0 self-start cursor-pointer">
                        Reset WiFi</button>
                    <div id="result" class="mt-4 px-1 sm:px-10"></div>
                </div>
            </form>

            <div class="flex justify-between items-end text-xs text-amber-600 mt-4">
                <a href="https://github.com/Valar-Systems/Blipscope/wiki" target="_blank" rel="noopener" class="text-amber-300 underline">Help &amp; documentation</a>
                <span>Firmware v%FW_VERSION% (Seismic)</span>
            </div>
        </fieldset>

        <script>
            document.getElementById('cfg').addEventListener('submit', function(e) {
                e.preventDefault();
                fetch(this.action, { method: 'POST', body: new FormData(this) })
                    .then(r => r.text())
                    .then(html => document.getElementById('result').innerHTML = html);
            });
            document.getElementById('resetwifi').addEventListener('click', function() {
                if (!confirm('Forget WiFi credentials and restart into setup mode? You will need to reconnect the device to a network.')) return;
                fetch('/reset-wifi', { method: 'POST' })
                    .then(r => r.text())
                    .then(html => document.getElementById('result').innerHTML = html);
            });
        </script>
    </body>
</html>
)";
#endif

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
        // Diagnostic: the async response needs a ~2.8 KB *contiguous* send buffer
        // (ASYNC_RESPONCE_BUFF_SIZE = 2 x TCP_MSS). If the largest free block is
        // below that, ESPAsyncWebServer silently fails to send and the page hangs.
        Serial.printf("[GET] heap free=%u largest-block=%u\n",
                      ESP.getFreeHeap(), ESP.getMaxAllocHeap());

        // read all values up front so the processor lambda can capture by value
        prefs.begin("config", true);
#if !defined(FEATURE_EAM) && !defined(FEATURE_SPACE) && !defined(FEATURE_SEISMIC)
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
        const String fadeEnabled = prefs.getString("fade", "true");
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
#elif defined(FEATURE_EAM)
        // FEATURE_EAM: load the EAM config fields. isKey() guards keep a not-yet-saved read from
        // logging NVS NOT_FOUND; the base-URL default is the EAM_FEED_BASE build flag.
        const String eamBaseUrl = prefs.isKey("eam-base-url")
            ? prefs.getString("eam-base-url", EAM_FEED_BASE)
            : String(EAM_FEED_BASE);
        const String latitude = prefs.getString("latitude", "");
        const String longitude = prefs.getString("longitude", "");
        const String abncpSource = prefs.isKey("abncp-source") ? prefs.getString("abncp-source", "backend") : "backend";
        const String openskyClientId = prefs.getString("opensky-id", "");
        String openskySecret = prefs.getString("opensky-secret", "");
        const String abncpWatch = prefs.getString("abncp-watch", "");
        const String ntfyTopic = prefs.getString("ntfy-topic", "");
        const String alertNew = prefs.isKey("eam-alert-new") ? prefs.getString("eam-alert-new", "true") : "true";
        const String alertTempo = prefs.isKey("eam-alert-tempo") ? prefs.getString("eam-alert-tempo", "true") : "true";
        const String alertAbncp = prefs.isKey("eam-alert-abncp") ? prefs.getString("eam-alert-abncp", "true") : "true";
        const String alertSpace = prefs.isKey("eam-alert-space") ? prefs.getString("eam-alert-space", "true") : "true";
        const String eamPalette = prefs.isKey("eam-palette") ? prefs.getString("eam-palette", "green") : "green";
        const String eamRefresh = prefs.isKey("eam-refresh") ? prefs.getString("eam-refresh", "normal") : "normal";
        const String colonBlink = prefs.isKey("eam-colon-blink") ? prefs.getString("eam-colon-blink", "false") : "false";
        const String autoDimEnabled = prefs.isKey("autodim") ? prefs.getString("autodim", "true") : "true";
        const String brightness = prefs.getString("brightness", "255");
        // default the field to the full ordered set so the user can see and edit it
        const String eamScreens = prefs.isKey("eam-screens")
            ? prefs.getString("eam-screens", "")
            : String("ticker,tempo,activity,codewords,abncp,milair,prop,icbm,ref,clock");
#elif defined(FEATURE_SPACE)
        // FEATURE_SPACE: load the Spacescope config fields. isKey() guards keep not-yet-saved
        // reads from logging NVS NOT_FOUND; the backend base-URL default is the SPACE_FEED_BASE
        // build flag (empty = direct public APIs).
        const String spaceBaseUrl = prefs.isKey("space-base-url")
            ? prefs.getString("space-base-url", SPACE_FEED_BASE)
            : String(SPACE_FEED_BASE);
        const String latitude = prefs.getString("latitude", "");
        const String longitude = prefs.getString("longitude", "");
        const String ntfyTopic = prefs.getString("ntfy-topic", "");
        const String alertLaunch = prefs.isKey("sp-alert-launch") ? prefs.getString("sp-alert-launch", "true") : "true";
        const String alertAurora = prefs.isKey("sp-alert-aurora") ? prefs.getString("sp-alert-aurora", "true") : "true";
        const String alertFlare = prefs.isKey("sp-alert-flare") ? prefs.getString("sp-alert-flare", "true") : "true";
        const String alertIss = prefs.isKey("sp-alert-iss") ? prefs.getString("sp-alert-iss", "true") : "true";
        const String alertDsn = prefs.isKey("sp-alert-dsn") ? prefs.getString("sp-alert-dsn", "false") : "false";
        const String autoDimEnabled = prefs.isKey("autodim") ? prefs.getString("autodim", "true") : "true";
        const String brightness = prefs.getString("brightness", "255");
        const String spaceScreens = prefs.isKey("space-screens")
            ? prefs.getString("space-screens", "")
            : String("iss,launch,kp,solarwind,scales,flare,aurora,dsn,deepspace,humans,moon,clock");
#elif defined(FEATURE_SEISMIC)
        // FEATURE_SEISMIC: load the Seismic edition config fields. isKey() guards keep not-yet-saved
        // reads from logging NVS NOT_FOUND; the device talks to USGS directly (se-base-url empty).
        const String seBaseUrl = prefs.getString("se-base-url", "");
        const String latitude = prefs.getString("latitude", "");
        const String longitude = prefs.getString("longitude", "");
        const String seMinMag = prefs.isKey("se-min-mag") ? prefs.getString("se-min-mag", "2.5") : "2.5";
        const String seRadius = prefs.isKey("se-radius-km") ? prefs.getString("se-radius-km", "500") : "500";
        const String seBigMag = prefs.isKey("se-big-mag") ? prefs.getString("se-big-mag", "6.0") : "6.0";
        const String seNearMag = prefs.isKey("se-near-mag") ? prefs.getString("se-near-mag", "4.0") : "4.0";
        const String ntfyTopic = prefs.getString("ntfy-topic", "");
        const String alertBig = prefs.isKey("se-alert-big") ? prefs.getString("se-alert-big", "true") : "true";
        const String alertNear = prefs.isKey("se-alert-near") ? prefs.getString("se-alert-near", "true") : "true";
        const String alertTsunami = prefs.isKey("se-alert-tsunami") ? prefs.getString("se-alert-tsunami", "true") : "true";
        const String autoDimEnabled = prefs.isKey("autodim") ? prefs.getString("autodim", "true") : "true";
        const String brightness = prefs.getString("brightness", "255");
#endif
        prefs.end();

#if !defined(FEATURE_EAM) && !defined(FEATURE_SPACE) && !defined(FEATURE_SEISMIC)
        // mask secrets before sending to client
        std::fill(openskySecret.begin(), openskySecret.end(), '*');
        std::fill(mqttPass.begin(), mqttPass.end(), '*');
#elif defined(FEATURE_EAM)
        // mask the OpenSky secret before sending to the client (same masked-value guard on save)
        std::fill(openskySecret.begin(), openskySecret.end(), '*');
#endif
        // FEATURE_SPACE has no secret fields yet (no API keys until the key-gated screens land).

        // template processor called once per %PLACEHOLDER% token found in CONFIG_HTML.
#if !defined(FEATURE_EAM) && !defined(FEATURE_SPACE) && !defined(FEATURE_SEISMIC)
        AsyncWebServerResponse* response = request->beginResponse(
            200, "text/html",
            (const uint8_t*)CONFIG_HTML, sizeof(CONFIG_HTML) - 1,
            [latitude, longitude, radius, radiusUnit, openskyClientId, openskySecret, dataSource, localUrl, scanlineEnabled, fadeEnabled, infoTextEnabled, triangleEnabled, trailEnabled, altColorEnabled, highlightEnabled, autoDimEnabled, brightness, tzOffset, watchlist, ntfyTopic, milShow, milAlert, heliShow, spcShow, logbookOn, lookupOn, lookupAlert, lookupDist, mqttOn, mqttHost, mqttPort, mqttUser, mqttPass, mqttBase, mqttDisco, infoFieldsHtml]
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
                if (var == "FADE")           return fadeEnabled == "true" ? "checked" : "";
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
#elif defined(FEATURE_EAM)
        AsyncWebServerResponse* response = request->beginResponse(
            200, "text/html",
            (const uint8_t*)CONFIG_HTML, sizeof(CONFIG_HTML) - 1,
            [eamBaseUrl, latitude, longitude, abncpSource, openskyClientId, openskySecret, abncpWatch, ntfyTopic, alertNew, alertTempo, alertAbncp, alertSpace, eamPalette, eamRefresh, colonBlink, autoDimEnabled, brightness, eamScreens]
            (const String& var) -> String {
                if (var == "EAM_BASE_URL")   return eamBaseUrl;
                if (var == "LATITUDE")       return latitude;
                if (var == "LONGITUDE")      return longitude;
                if (var == "ABNCP_BACKEND")  return abncpSource == "opensky" ? "" : "selected";
                if (var == "ABNCP_OPENSKY")  return abncpSource == "opensky" ? "selected" : "";
                if (var == "OPENSKY_ID")     return openskyClientId;
                if (var == "OPENSKY_SECRET") return openskySecret;
                if (var == "ABNCP_WATCH")    return abncpWatch;
                if (var == "NTFY_TOPIC")     return ntfyTopic;
                if (var == "ALERT_NEW")      return alertNew == "true" ? "checked" : "";
                if (var == "ALERT_TEMPO")    return alertTempo == "true" ? "checked" : "";
                if (var == "ALERT_ABNCP")    return alertAbncp == "true" ? "checked" : "";
                if (var == "ALERT_SPACE")    return alertSpace == "true" ? "checked" : "";
                if (var == "PAL_GREEN")      return eamPalette == "amber" ? "" : "selected";
                if (var == "PAL_AMBER")      return eamPalette == "amber" ? "selected" : "";
                if (var == "RR_NORMAL")      return eamRefresh == "relaxed" || eamRefresh == "battery" ? "" : "selected";
                if (var == "RR_RELAXED")     return eamRefresh == "relaxed" ? "selected" : "";
                if (var == "RR_BATTERY")     return eamRefresh == "battery" ? "selected" : "";
                if (var == "COLON_BLINK")    return colonBlink == "true" ? "checked" : "";
                if (var == "AUTODIM")        return autoDimEnabled == "true" ? "checked" : "";
                if (var == "BRIGHTNESS")     return brightness;
                if (var == "EAM_SCREENS")    return eamScreens;
                if (var == "FW_VERSION")     return String(FW_VERSION);
                return "";
            }
        );
#elif defined(FEATURE_SPACE)
        AsyncWebServerResponse* response = request->beginResponse(
            200, "text/html",
            (const uint8_t*)CONFIG_HTML, sizeof(CONFIG_HTML) - 1,
            [spaceBaseUrl, latitude, longitude, ntfyTopic, alertLaunch, alertAurora, alertFlare, alertIss, alertDsn, autoDimEnabled, brightness, spaceScreens]
            (const String& var) -> String {
                if (var == "SPACE_BASE_URL") return spaceBaseUrl;
                if (var == "LATITUDE")       return latitude;
                if (var == "LONGITUDE")      return longitude;
                if (var == "NTFY_TOPIC")     return ntfyTopic;
                if (var == "AL_LAUNCH")      return alertLaunch == "true" ? "checked" : "";
                if (var == "AL_AURORA")      return alertAurora == "true" ? "checked" : "";
                if (var == "AL_FLARE")       return alertFlare == "true" ? "checked" : "";
                if (var == "AL_ISS")         return alertIss == "true" ? "checked" : "";
                if (var == "AL_DSN")         return alertDsn == "true" ? "checked" : "";
                if (var == "AUTODIM")        return autoDimEnabled == "true" ? "checked" : "";
                if (var == "BRIGHTNESS")     return brightness;
                if (var == "SPACE_SCREENS")  return spaceScreens;
                if (var == "FW_VERSION")     return String(FW_VERSION);
                return "";
            }
        );
#elif defined(FEATURE_SEISMIC)
        AsyncWebServerResponse* response = request->beginResponse(
            200, "text/html",
            (const uint8_t*)CONFIG_HTML, sizeof(CONFIG_HTML) - 1,
            [seBaseUrl, latitude, longitude, seMinMag, seRadius, seBigMag, seNearMag, ntfyTopic, alertBig, alertNear, alertTsunami, autoDimEnabled, brightness]
            (const String& var) -> String {
                if (var == "SE_BASE_URL")    return seBaseUrl;
                if (var == "LATITUDE")       return latitude;
                if (var == "LONGITUDE")      return longitude;
                if (var == "SE_MIN_MAG")     return seMinMag;
                if (var == "SE_RADIUS")      return seRadius;
                if (var == "SE_BIG_MAG")     return seBigMag;
                if (var == "SE_NEAR_MAG")    return seNearMag;
                if (var == "NTFY_TOPIC")     return ntfyTopic;
                if (var == "AL_BIG")         return alertBig == "true" ? "checked" : "";
                if (var == "AL_NEAR")        return alertNear == "true" ? "checked" : "";
                if (var == "AL_TSUNAMI")     return alertTsunami == "true" ? "checked" : "";
                if (var == "AUTODIM")        return autoDimEnabled == "true" ? "checked" : "";
                if (var == "BRIGHTNESS")     return brightness;
                if (var == "FW_VERSION")     return String(FW_VERSION);
                return "";
            }
        );
#endif
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

#if !defined(FEATURE_EAM) && !defined(FEATURE_SPACE) && !defined(FEATURE_SEISMIC)
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
        prefs.putString("fade", request->hasParam("fade", true) ? "true" : "false");
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
#elif defined(FEATURE_EAM)
        // FEATURE_EAM: persist the EAM config fields.
        TrySaveParam("eam-base-url");
        TrySaveParam("latitude");
        TrySaveParam("longitude");
        TrySaveParam("abncp-source");
        TrySaveParam("opensky-id");
        TrySaveParam("abncp-watch");
        TrySaveParam("ntfy-topic");
        TrySaveParam("eam-palette");
        TrySaveParam("eam-refresh");
        TrySaveParam("brightness");
        TrySaveParam("eam-screens");

        // OpenSky secret: don't overwrite the stored value with the masked placeholder
        const auto* eamSecret = request->getParam("opensky-secret", true);
        if (eamSecret != nullptr) {
            const String& secret = eamSecret->value();
            if (secret.indexOf('*') == -1)
                prefs.putString("opensky-secret", secret);
        }

        // checkboxes: absent in the body when unchecked, so hasParam() is the on/off signal
        prefs.putString("eam-alert-new", request->hasParam("eam-alert-new", true) ? "true" : "false");
        prefs.putString("eam-alert-tempo", request->hasParam("eam-alert-tempo", true) ? "true" : "false");
        prefs.putString("eam-alert-abncp", request->hasParam("eam-alert-abncp", true) ? "true" : "false");
        prefs.putString("eam-alert-space", request->hasParam("eam-alert-space", true) ? "true" : "false");
        prefs.putString("eam-colon-blink", request->hasParam("eam-colon-blink", true) ? "true" : "false");
        prefs.putString("autodim", request->hasParam("autodim", true) ? "true" : "false");
#elif defined(FEATURE_SPACE)
        // FEATURE_SPACE: persist the Spacescope config fields.
        TrySaveParam("space-base-url");
        TrySaveParam("latitude");
        TrySaveParam("longitude");
        TrySaveParam("ntfy-topic");
        TrySaveParam("brightness");
        TrySaveParam("space-screens");

        // checkboxes: absent in the body when unchecked, so hasParam() is the on/off signal
        prefs.putString("sp-alert-launch", request->hasParam("sp-alert-launch", true) ? "true" : "false");
        prefs.putString("sp-alert-aurora", request->hasParam("sp-alert-aurora", true) ? "true" : "false");
        prefs.putString("sp-alert-flare", request->hasParam("sp-alert-flare", true) ? "true" : "false");
        prefs.putString("sp-alert-iss", request->hasParam("sp-alert-iss", true) ? "true" : "false");
        prefs.putString("sp-alert-dsn", request->hasParam("sp-alert-dsn", true) ? "true" : "false");
        prefs.putString("autodim", request->hasParam("autodim", true) ? "true" : "false");
#elif defined(FEATURE_SEISMIC)
        // FEATURE_SEISMIC: persist the Seismic edition config fields.
        TrySaveParam("se-base-url");
        TrySaveParam("latitude");
        TrySaveParam("longitude");
        TrySaveParam("se-min-mag");
        TrySaveParam("se-radius-km");
        TrySaveParam("se-big-mag");
        TrySaveParam("se-near-mag");
        TrySaveParam("ntfy-topic");
        TrySaveParam("brightness");

        // checkboxes: absent in the body when unchecked, so hasParam() is the on/off signal
        prefs.putString("se-alert-big", request->hasParam("se-alert-big", true) ? "true" : "false");
        prefs.putString("se-alert-near", request->hasParam("se-alert-near", true) ? "true" : "false");
        prefs.putString("se-alert-tsunami", request->hasParam("se-alert-tsunami", true) ? "true" : "false");
        prefs.putString("autodim", request->hasParam("autodim", true) ? "true" : "false");
#endif
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

#ifdef FEATURE_EAM
    // Logbook export (firmware-only; no backend). Serves the persisted EAM/codeword log straight
    // from NVS as a file download. Read-only, so it's safe from the async task alongside the
    // loop-task logbook writer.
    server.on("/eam-log.csv", HTTP_GET, [](AsyncWebServerRequest* request) {
        AsyncWebServerResponse* r = request->beginResponse(200, "text/csv", EamLogbook::ExportCsv());
        r->addHeader("Content-Disposition", "attachment; filename=\"eam-log.csv\"");
        r->addHeader("Cache-Control", "no-store");
        request->send(r);
    });
    server.on("/eam-log.json", HTTP_GET, [](AsyncWebServerRequest* request) {
        AsyncWebServerResponse* r = request->beginResponse(200, "application/json", EamLogbook::ExportJson());
        r->addHeader("Content-Disposition", "attachment; filename=\"eam-log.json\"");
        r->addHeader("Cache-Control", "no-store");
        request->send(r);
    });
#endif

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