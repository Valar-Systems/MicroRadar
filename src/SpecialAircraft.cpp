#include "SpecialAircraft.h"

#include <cctype>
#include <cstdlib>

namespace {

// Military ICAO 24-bit address ranges (inclusive), sorted ascending and
// non-overlapping. This is the long-standing VirtualRadarServer "military
// ranges" table, reproduced widely across ADS-B tooling. The address blocks are
// the source of truth; the country comments only mark the well-known anchors and
// are intentionally omitted where the allocation isn't clearly attributable.
struct Range { uint32_t lo, hi; };

constexpr Range MIL_RANGES[] = {
    { 0x010070, 0x01008F }, // Egypt
    { 0x0A4000, 0x0A4FFF }, // Algeria
    { 0x33FF00, 0x33FFFF }, // Italy
    { 0x350000, 0x37FFFF }, // Spain
    { 0x3AA000, 0x3AFFFF }, // France
    { 0x3B7000, 0x3BFFFF }, // France
    { 0x3EA000, 0x3EBFFF }, // Germany
    { 0x3F4000, 0x3FBFFF }, // Germany
    { 0x400000, 0x40003F }, // United Kingdom
    { 0x43C000, 0x43CFFF }, // United Kingdom
    { 0x444000, 0x446FFF }, // Belgium
    { 0x44F000, 0x44FFFF },
    { 0x457000, 0x457FFF },
    { 0x45F400, 0x45F4FF },
    { 0x468000, 0x4683FF }, // Greece
    { 0x473C00, 0x473C0F },
    { 0x478100, 0x4781FF },
    { 0x480000, 0x480FFF }, // Netherlands
    { 0x48D800, 0x48D87F },
    { 0x497C00, 0x497CFF },
    { 0x498420, 0x49842F },
    { 0x4B7000, 0x4B7FFF }, // Switzerland
    { 0x4B8200, 0x4B82FF },
    { 0x70C070, 0x70C07F },
    { 0x710258, 0x71028F },
    { 0x710380, 0x71039F },
    { 0x738A00, 0x738AFF }, // Israel
    { 0x7CF800, 0x7CFAFF }, // Australia
    { 0x800200, 0x8002FF }, // India
    { 0xADF7C8, 0xAFFFFF }, // United States
    { 0xC20000, 0xC3FFFF }, // Canada
    { 0xE40000, 0xE41FFF }, // Brazil
};

// Distinctive callsign prefixes worth flagging, matched case-insensitively
// against the trimmed callsign. Deliberately conservative so ordinary airline
// callsigns don't trip it -- extend by adding to this one list.
constexpr const char* SPECIAL_CALLSIGNS[] = {
    // rescue / medical
    "RESCUE", "HEMS", "MEDEVAC", "MEDIC", "LIFEGUARD", "HELIMED",
    // police / law enforcement
    "POLICE", "NPAS", "SHERIFF",
    // agencies / manufacturer test flights
    "NASA", "BOEING", "AIRBUS",
};

constexpr int HELI_CATEGORY = 8; // OpenSky emitter category: rotorcraft

} // namespace

namespace SpecialAircraft {

bool IsMilitary(uint32_t a)
{
    for (const Range& r : MIL_RANGES) {
        // ranges are sorted ascending, so once we pass the address no later
        // range can contain it -- bail out early.
        if (a < r.lo) return false;
        if (a <= r.hi) return true;
    }
    return false;
}

bool IsMilitary(const String& hex)
{
    if (hex.isEmpty())
        return false;

    // Skip any leading non-hex characters (e.g. a '~' prefix on local-receiver
    // TIS-B addresses, or stray whitespace) before parsing.
    const char* p = hex.c_str();
    while (*p && !std::isxdigit(static_cast<unsigned char>(*p)))
        ++p;
    if (!*p)
        return false;

    return IsMilitary(static_cast<uint32_t>(std::strtoul(p, nullptr, 16)));
}

bool IsSpecialCallsign(const String& callsign)
{
    String cs = callsign;
    cs.trim();
    if (cs.isEmpty())
        return false;
    cs.toUpperCase();

    for (const char* prefix : SPECIAL_CALLSIGNS)
        if (cs.startsWith(prefix))
            return true;
    return false;
}

bool IsHelicopter(int category)
{
    return category == HELI_CATEGORY;
}

const char* Tag(Class c)
{
    switch (c) {
        case Class::Military:   return "MIL";
        case Class::Special:    return "SPC";
        case Class::Helicopter: return "HELI";
        default:                return "";
    }
}

} // namespace SpecialAircraft
