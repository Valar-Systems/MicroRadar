#pragma once

#include <Arduino.h>
#include <cstdint>

#include "models/Aircraft.h"

// Offline classification of "interesting" contacts. Every signal used here comes
// straight from the live feed -- the ICAO 24-bit address, the ADS-B emitter
// category, and the callsign -- so detection needs no network lookup and works
// identically on the OpenSky feed and a local ADS-B receiver, even with all
// adsbdb enrichment switched off.
namespace SpecialAircraft {

// Ordered by display priority, highest first: when a contact matches more than
// one class the manager shows the earlier one (a military helicopter reads as
// "MIL", a police helicopter as "SPC").
enum class Class : uint8_t {
    None = 0,
    Military,    // ICAO 24-bit address in a known military allocation
    Special,     // distinctive callsign (rescue / police / agency / test ...)
    Helicopter,  // ADS-B emitter category = rotorcraft
};

// Military: derived purely from the ICAO 24-bit address.
bool IsMilitary(uint32_t icaoAddr);
// Convenience overload: parse a hex ICAO string ("adf7c8"). Tolerates a leading
// '~' (local-receiver TIS-B addresses) and whitespace; false if unparseable.
bool IsMilitary(const String& icao24Hex);

// Special: the trimmed, upper-cased callsign begins with a known distinctive
// prefix (rescue / medical / police / agency / manufacturer test flights).
bool IsSpecialCallsign(const String& callsign);

// Helicopter: the (OpenSky-numbered) ADS-B emitter category marks a rotorcraft.
bool IsHelicopter(int category);

// Short uppercase tag for the radar/list ("MIL" / "SPC" / "HELI"); "" for None.
const char* Tag(Class c);

} // namespace SpecialAircraft
