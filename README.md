<h1 align=center>
  📡 Blipscope
</h1>
<h6 align=center>
  a tiny desktop flight radar that shows you what's flying overhead
</h6>
<p align=center>
  <a href="#what-it-does">WHAT IT DOES</a> - <a href="#get-a-kit">GET A KIT</a> - <a href="#assembly">ASSEMBLY</a> - <a href="#firmware">FIRMWARE</a> - <a href="#setup--usage">SETUP</a> - <a href="#thanks">THANKS</a>
</p>

Blipscope is a small open-source flight radar for your desk. It sits on a 1.28" round display and shows live aircraft around your location in real time — pulled from public ADS-B data — so you can glance over and see what's in the sky above you, where it came from, and where it's headed.

## What it does

- **Live radar view** — aircraft plotted around your location, with fading trails, type-aware heading markers (helicopters, gliders, and heavies each draw differently), and altitude-based colour coding.
- **Tap to inspect** — touch an aircraft to open a detail card with callsign, type, operator, registration, route, altitude, speed, and a photo. Pin one to keep tracking it.
- **List & stats screens** — swipe between the radar, a list of everything in range, and at-a-glance statistics.
- **Spotting logbook** — an optional persistent "lifelist" that tallies every unique aircraft type, airline, and country you've ever seen overhead, shown on the Stats screen, with a gold "NEW" flag on first sightings.
- **Tail-number watchlist** — get a phone notification (via [ntfy](https://ntfy.sh)) whenever a specific aircraft flies over.
- **Emergency squawk alerts** — highlights aircraft broadcasting 7500/7600/7700.
- **"Look up!" overhead alert** — flashes a cyan ring when an aircraft passes within a set distance of your location, so you can glance up and spot it for real, with an optional phone alert.
- **Special-aircraft detection** — flags military (orange "MIL", from the ICAO address), helicopters (violet "HELI", from the ADS-B category), and distinctive callsigns such as rescue/police/test flights (blue "SPC") — all worked out offline with no account or lookup needed, plus an optional phone alert when a military aircraft flies over.
- **NTP clock & auto-dim** — keeps accurate time and dims itself at night based on your local sunrise/sunset.
- **Home Assistant / MQTT** — optionally publishes a live summary (aircraft count, nearest flight, and overhead/military flags) to your MQTT broker, with auto-discovery so Home Assistant creates the sensors for you.
- **Over-the-air updates** — pulls new firmware automatically from GitHub Releases, so it stays current without plugging in.
- **Configurable range & display** — set your centre point and scan radius in km or miles, and toggle the on-screen elements you want.

## Get a kit

The easiest way to build a Blipscope is to grab a kit. It includes the display module, the redesigned enclosure parts, and everything else you need in one box — no hunting around marketplaces for the right components, and the hardware is guaranteed to match the firmware and the enclosure.

### 👉 [Order a Blipscope kit from Valar Systems](https://valarsystems.com/products/blipscope)

Buying a kit is also the best way to support continued development of the project. Thank you 🙏

## Assembly

Blipscope's enclosure has been completely redesigned, so the build steps live in the project wiki rather than here in the README:

### 📖 [Assembly guide → Blipscope Wiki](https://github.com/Valar-Systems/Blipscope/wiki)

The wiki walks through the full build with photos. We recommend skimming the [Setup & Usage](#setup--usage) section below before you start, so you can test the hardware before everything is closed up.

## Firmware

Kits ship with firmware already flashed, and the device keeps itself up to date [over the air](#what-it-does). For most people there's nothing to install.

If you want to build from source or hack on it yourself, the firmware is here in this repo:

1. Install [VS Code](https://code.visualstudio.com/) with the [PlatformIO IDE extension](https://marketplace.visualstudio.com/items?itemName=platformio.platformio-ide).
2. Restart VS Code and open this repository folder — PlatformIO pulls in the dependencies automatically.
3. Plug the board in via USB-C and hit the upload button (→) in the bottom status bar.

If the board doesn't reboot into the new firmware automatically, hold the **BOOT** button, press **RESET** once, then release **BOOT**. If an upload fails, double-check the board selected in the status bar, try a different USB port, and make sure your cable supports data (some USB-C cables are charge-only). More on PlatformIO [here](https://docs.platformio.org/en/latest/).

## Setup & Usage

### First boot

On first boot, Blipscope broadcasts its own WiFi hotspot. Each device has a unique name like `Blipscope-A1B2C3` — the exact name is shown on the screen during setup. Connect to that hotspot from your phone or laptop and a configuration page appears automatically (open a browser if it doesn't). Enter your WiFi credentials and hit save; the device restarts and joins your network.

If the hotspot doesn't appear straight away, give it a moment. If it still hasn't shown up after 30 seconds, leave the WiFi settings on your device and go back in to force a refresh.

### Configuration

Once it's on your network, the config page is reachable from any device on the same network at the address shown on screen — `http://<device-name>.local` (for example `http://blipscope-a1b2c3.local`).

There you can set:

- **Location** (latitude and longitude) — the centre point of your radar.
- **Radar radius** — how far the scan extends, in km or miles (capped at ~222 km / 138 mi to stay within data rate limits).
- **Data source** — pull flight data from the OpenSky Network (the cloud default) or from your own ADS-B receiver on the local network (see below).
- **Display options** — toggle the on-screen elements and aircraft info fields.
- **Watchlist & alerts** — tail numbers to watch and the ntfy topic to notify, plus toggles to highlight military aircraft, helicopters, and special flights, and to alert when a military aircraft flies over.
- **OpenSky credentials** — your client ID and secret (optional, but recommended).
- **Home Assistant / MQTT** — broker address and port, optional username/password, base topic, and an auto-discovery toggle. When enabled, Blipscope publishes a retained `<base>/summary` (and, with discovery on, the Home Assistant config so an "Aircraft in range" count, a "Nearest aircraft" sensor, and "Aircraft overhead" / "Military aircraft in range" binary sensors appear automatically).

The config page is available any time the device is on WiFi, so you can tweak settings whenever you like.

### A note on OpenSky

Blipscope uses [OpenSky Network's](https://opensky-network.org) free API for flight data. It works without an account, but making one (it's free) raises your daily request limit from 400 to 4000, which makes the live view far more accurate. Grab your client ID and secret from your OpenSky account settings and enter them on the config page.

### Using your own ADS-B receiver

If you run your own ADS-B receiver — a Raspberry Pi with [dump1090-fa](https://github.com/flightaware/dump1090), [readsb](https://github.com/wiedehopf/readsb), [PiAware](https://www.flightaware.com/adsb/piaware/), [tar1090](https://github.com/wiedehopf/tar1090), or an ADS-B Exchange feeder image — Blipscope can read directly from it instead of OpenSky. Local data has **no rate limits** and refreshes about once a second, so the radar is smoother and more accurate, and works even if OpenSky is down.

#### Don't have a receiver yet?

A receiver is three things: an **SDR USB dongle**, a **1090 MHz antenna**, and **decoder software** on a Raspberry Pi (any Pi 2 or newer — even a Pi Zero 2 W — is plenty; the decoder is light).

- **Recommended starter:** a [Nooelec NESDR SMArt v5](https://www.nooelec.com/store/nesdr-smart-sdr.html) dongle plus a [Nooelec 1090 MHz ADS-B antenna](https://www.nooelec.com/store/sdr/sdr-addons/1090mhz-ads-b-antenna-5dbi-sma.html). Both ends are SMA, so they screw straight together with no adapter, and the pair runs well under the price of an all-in-one kit. In an RF-noisy spot you can add a [1090 MHz band-pass filter](https://www.nooelec.com/store/sdr/sdr-addons/) later, but try without one first.
- **If you can find one — better filtering:** a [FlightAware Pro Stick Plus](https://www.flightaware.com/adsb/prostick/) has a 1090 MHz filter + amp built in, so it shrugs off nearby noise. Pair it with the same SMA antenna above (no adapter needed). It's frequently out of stock, so don't wait on it if the Nooelec combo is available.

> Avoid the **RTL-SDR Blog V4** kit — its tuner chip was discontinued, so remaining stock sells at inflated prices. The **RTL-SDR Blog V3** (still in production) is a fine dongle if you already have one, but you'd still need to add a 1090 MHz antenna.

> The biggest factor for range is **antenna height and sky view**, not the dongle — a cheap antenna in an attic or upstairs window beats an expensive one on a desk.

For software, the quickest path is to flash the **[PiAware](https://www.flightaware.com/adsb/piaware/) SD-card image**: it bundles `dump1090-fa`, which serves exactly the feed Blipscope reads, and it earns you a free FlightAware account for feeding. Once it's running, check the receiver's map in a browser at `http://<pi-ip>:8080/` to confirm it's seeing aircraft.

#### Pointing Blipscope at it

On the config page, set **Data source** to *My own ADS-B receiver* and enter your receiver's address in **Receiver URL**. You can type just the device's IP (for example `192.168.1.50`) and Blipscope will assume the conventional `/data/aircraft.json` path, or paste the full URL if your setup serves it elsewhere (e.g. `http://192.168.1.50/tar1090/data/aircraft.json`). The receiver must be reachable on the same network as Blipscope.

PiAware / dump1090-fa serve the data on **port 8080**, so enter `http://<pi-ip>:8080` for those — Blipscope appends `/data/aircraft.json` to give `http://<pi-ip>:8080/data/aircraft.json`. If in doubt, open the receiver's map in a browser and use whatever base address it serves from, with `/data/aircraft.json` on the end.

> Altitudes and speeds in the receiver's feed are reported in feet and knots; Blipscope converts them so the display matches the OpenSky readings (metres and m/s). Aircraft outside your configured radius are filtered out the same way OpenSky bounds its results.

That's it — once configured, you'll have a live view of everything flying over your location. Enjoy ✈️

## Thanks

Blipscope is built on the wonderful **[Micro Radar](https://github.com/AnthonySturdy/micro-radar)** project by **[Anthony Sturdy](https://github.com/AnthonySturdy)**. Anthony designed and open-sourced the original device — the concept, the enclosure, and the firmware this project grew from. None of this would exist without his work, and we're hugely grateful he shared it with the world. Go give the [original repo](https://github.com/AnthonySturdy/micro-radar) a star. 🌟

The original Micro Radar was itself inspired by [therealhacksaw](https://www.instagram.com/therealhacksaw/)'s desk radar.

Blipscope is maintained by [Valar Systems](https://valarsystems.com).

## License

Blipscope is released under the **Open Community License (OCL v1)** — see [LICENSE](LICENSE) for the full text. In short: as a non-commercial user you're free to use, copy, modify, and hack it however you like, and to share derivatives under the same share-alike terms. Commercial replication requires a separate business or repair license. The aim is to keep the project open and repairable while preventing straight commercial cloning. Learn more about the OCL [here](https://github.com/OpenCommunityLicence/OpenCommunityLicence).
