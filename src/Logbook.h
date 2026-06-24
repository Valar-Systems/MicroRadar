#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <set>

// Persistent "lifelist" of what this device has ever seen overhead: the sets of
// unique aircraft types, airlines, and countries, plus a running contact
// odometer. Backed by its own NVS namespace and written back lazily (debounced)
// to spare the flash. Every set is bounded so the logbook can never overrun NVS
// and starve the config namespace that shares the same partition.
class Logbook {
public:
    void Begin(); // load persisted sets + counters from NVS (call once)

    // Record a sighting's metadata. Each returns true only when it was a
    // brand-new entry (first time ever seen), so the caller can flag a fresh
    // catch on screen. Empty / already-known / at-capacity inputs return false.
    bool NoteType(const String& typeCode);
    bool NoteOperator(const String& operatorName);
    bool NoteCountry(const String& country);

    void NoteContact(); // a new contact entered range; bumps the odometer

    size_t TypeCount() const     { return types.size(); }
    size_t OperatorCount() const { return operators.size(); }
    size_t CountryCount() const  { return countries.size(); }
    uint32_t Contacts() const    { return contacts; }

    void MaybePersist(); // flush to NVS when dirty and the debounce has elapsed

private:
    std::set<String> types, operators, countries;
    uint32_t contacts = 0;
    bool dirty = false;
    bool started = false;
    unsigned long lastPersist = 0;

    Preferences prefs;

    // Bounds chosen so each serialized set stays under the NVS ~4000-byte string
    // cap (and well under it together): types ~400x5, operators ~140x25,
    // countries ~64x20.
    static constexpr size_t MAX_TYPES     = 400;
    static constexpr size_t MAX_OPERATORS = 140;
    static constexpr size_t MAX_COUNTRIES = 64;
    static constexpr size_t MAX_OP_LEN    = 24;   // truncate long airline names
    static constexpr size_t MAX_BLOB      = 3800; // hard ceiling per serialized set
    static constexpr unsigned long PERSIST_INTERVAL_MS = 10UL * 60UL * 1000UL; // 10 min

    bool noteInto(std::set<String>& s, const String& value, size_t cap);
    static void load(Preferences& p, const char* key, std::set<String>& out);
    void save(const char* key, const std::set<String>& s);
};
