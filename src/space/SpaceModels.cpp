#include "SpaceModels.h"

#include <stdlib.h>
#include <string.h>

namespace {

// Days from the civil (proleptic Gregorian) date to 1970-01-01 (Howard Hinnant's algorithm).
// Avoids timegm(), which isn't reliably present in this newlib; correct for all UTC dates.
long DaysFromCivil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097L + (long)doe - 719468;
}

} // namespace

namespace space {

bool ParseIss(JsonObjectConst root, IssState& out)
{
    if (root.isNull() || root["latitude"].isNull() || root["longitude"].isNull())
        return false;

    out.lat = root["latitude"] | 0.0;
    out.lon = root["longitude"] | 0.0;
    out.altKm = root["altitude"] | 0.0;
    out.velocityKmh = root["velocity"] | 0.0;
    out.footprintKm = root["footprint"] | 0.0;
    out.timestamp = (long)(root["timestamp"] | 0);
    const char* vis = root["visibility"] | "";
    out.sunlit = (strcmp(vis, "daylight") == 0);
    out.valid = true;
    return true;
}

void ParseLaunches(JsonObjectConst root, std::vector<Launch>& out, size_t cap)
{
    out.clear();
    JsonArrayConst arr = root["result"].as<JsonArrayConst>();
    if (arr.isNull()) return;

    for (JsonObjectConst o : arr) {
        if (out.size() >= cap) break;
        Launch L;

        // RocketLaunch.Live names read "Vehicle | Mission"; keep the mission half when present.
        String name = (const char*)(o["name"] | "");
        const int bar = name.indexOf('|');
        if (bar >= 0) { L.mission = name.substring(bar + 1); L.mission.trim(); }
        else          { L.mission = name; }

        L.provider = (const char*)(o["provider"]["name"] | "");
        L.vehicle = (const char*)(o["vehicle"]["name"] | "");

        const String padName = (const char*)(o["pad"]["name"] | "");
        const String loc = (const char*)(o["pad"]["location"]["name"] | "");
        L.pad = padName;
        if (loc.length()) L.pad += (padName.length() ? ", " : "") + loc;

        // Best launch instant: t0 (precise) -> win_open (precise) -> sort_date (coarse unix).
        const String t0 = (const char*)(o["t0"] | "");
        const String winOpen = (const char*)(o["win_open"] | "");
        if (t0.length()) { L.t0Epoch = Iso8601ToEpoch(t0); L.precise = L.t0Epoch > 0; }
        if (!L.precise && winOpen.length()) { L.t0Epoch = Iso8601ToEpoch(winOpen); L.precise = L.t0Epoch > 0; }
        if (L.t0Epoch <= 0) {
            const long sd = String((const char*)(o["sort_date"] | "")).toInt();
            if (sd > 0) { L.t0Epoch = sd; L.precise = false; }
        }
        L.dateStr = (const char*)(o["date_str"] | "");

        out.push_back(L);
    }
}

bool ParseKp(JsonArrayConst root, SpaceWx& out, size_t historyCap)
{
    if (root.isNull()) return false;

    // SWPC's noaa-planetary-k-index.json is an array of objects {time_tag, Kp, a_running,
    // station_count}, newest last, with Kp a JSON number (e.g. 3.33). Verified against the live
    // feed 2026-06-30.
    std::vector<float> vals;
    String lastTime;
    for (JsonObjectConst row : root) {
        if (row.isNull() || row["Kp"].isNull()) continue;
        vals.push_back(row["Kp"] | 0.0f);
        lastTime = (const char*)(row["time_tag"] | "");
    }
    if (vals.empty()) return false;

    out.kp = vals.back();
    out.timeTagEpoch = Iso8601ToEpoch(lastTime);
    const size_t start = vals.size() > historyCap ? vals.size() - historyCap : 0;
    out.history.assign(vals.begin() + start, vals.end());
    out.valid = true;
    return true;
}

long Iso8601ToEpoch(const String& s)
{
    if (s.length() < 16) return 0; // need at least "YYYY-MM-DDThh:mm"
    int Y = 0, Mo = 0, D = 0, h = 0, m = 0, sec = 0;
    if (sscanf(s.c_str(), "%d-%d-%d", &Y, &Mo, &D) != 3) return 0;
    if (Mo < 1 || Mo > 12 || D < 1 || D > 31) return 0;

    const char* t = s.c_str() + 10;        // index 10 is the date/time separator ('T' or ' ')
    if (*t == 'T' || *t == ' ') ++t; else return 0;
    if (sscanf(t, "%d:%d:%d", &h, &m, &sec) < 2) return 0; // seconds optional

    return DaysFromCivil(Y, (unsigned)Mo, (unsigned)D) * 86400L + h * 3600L + m * 60L + sec;
}

} // namespace space
