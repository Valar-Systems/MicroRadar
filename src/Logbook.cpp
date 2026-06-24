#include "Logbook.h"

namespace {
constexpr char SEP = '\n'; // record separator: newline, so operator names may contain commas/spaces
}

void Logbook::Begin()
{
    if (started)
        return;
    started = true;

    // Opening read-write creates the namespace if it's missing (first ever run),
    // so the later reads don't log NOT_FOUND.
    prefs.begin("logbook", false);
    load(prefs, "types", types);
    load(prefs, "operators", operators);
    load(prefs, "countries", countries);
    contacts = prefs.getUInt("contacts", 0);
    prefs.end();

    lastPersist = millis();
    Serial.printf("[logbook] loaded %u types, %u airlines, %u countries, %u contacts\n",
                  (unsigned)types.size(), (unsigned)operators.size(),
                  (unsigned)countries.size(), (unsigned)contacts);
}

void Logbook::load(Preferences& p, const char* key, std::set<String>& out)
{
    if (!p.isKey(key))
        return;
    const String blob = p.getString(key, "");

    int start = 0;
    for (int i = 0; i <= (int)blob.length(); ++i) {
        if (i == (int)blob.length() || blob[i] == SEP) {
            if (i > start)
                out.insert(blob.substring(start, i));
            start = i + 1;
        }
    }
}

bool Logbook::noteInto(std::set<String>& s, const String& value, size_t cap)
{
    if (value.isEmpty())
        return false;
    if (s.count(value))
        return false;          // already collected
    if (s.size() >= cap)
        return false;          // at capacity: don't claim a new catch we can't store

    s.insert(value);
    dirty = true;
    return true;
}

bool Logbook::NoteType(const String& typeCode)
{
    String t = typeCode;
    t.trim();
    return noteInto(types, t, MAX_TYPES);
}

bool Logbook::NoteOperator(const String& operatorName)
{
    String op = operatorName;
    op.trim();
    if (op.length() > MAX_OP_LEN)
        op = op.substring(0, MAX_OP_LEN);
    return noteInto(operators, op, MAX_OPERATORS);
}

bool Logbook::NoteCountry(const String& country)
{
    String c = country;
    c.trim();
    return noteInto(countries, c, MAX_COUNTRIES);
}

void Logbook::NoteContact()
{
    ++contacts;
    dirty = true;
}

void Logbook::save(const char* key, const std::set<String>& s)
{
    String blob;
    for (const String& v : s) {
        if (blob.length() + v.length() + 1 > MAX_BLOB)
            break; // safety ceiling; the per-set caps keep us well short of this
        if (!blob.isEmpty())
            blob += SEP;
        blob += v;
    }
    prefs.putString(key, blob);
}

void Logbook::MaybePersist()
{
    if (!dirty)
        return;
    const unsigned long now = millis();
    if (now - lastPersist < PERSIST_INTERVAL_MS)
        return;

    prefs.begin("logbook", false);
    save("types", types);
    save("operators", operators);
    save("countries", countries);
    prefs.putUInt("contacts", contacts);
    prefs.end();

    lastPersist = now;
    dirty = false;
    Serial.printf("[logbook] persisted (%u types, %u airlines, %u countries, %u contacts)\n",
                  (unsigned)types.size(), (unsigned)operators.size(),
                  (unsigned)countries.size(), (unsigned)contacts);
}
