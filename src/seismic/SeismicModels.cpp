#include "SeismicModels.h"

#include <math.h>

namespace seismic {

void ParseQuakes(JsonObjectConst root, std::vector<Quake>& out, size_t cap)
{
    out.clear();
    if (root.isNull()) return;

    JsonArrayConst features = root["features"].as<JsonArrayConst>();
    if (features.isNull()) return;

    for (JsonObjectConst f : features) {
        if (out.size() >= cap) break;
        JsonObjectConst p = f["properties"].as<JsonObjectConst>();
        if (p.isNull() || p["mag"].isNull()) continue; // a quake with no magnitude isn't plottable

        Quake q;
        q.id = (const char*)(f["id"] | "");
        q.mag = p["mag"] | 0.0f;
        q.magType = (const char*)(p["magType"] | "");
        q.place = (const char*)(p["place"] | "");
        // USGS `time` is milliseconds since epoch, larger than a 32-bit int -> read as 64-bit.
        const long long ms = p["time"] | 0LL;
        q.timeEpoch = (long)(ms / 1000LL);
        q.tsunami = (int)(p["tsunami"] | 0) != 0;

        // geometry.coordinates = [lon, lat, depthKm]
        JsonArrayConst c = f["geometry"]["coordinates"].as<JsonArrayConst>();
        if (!c.isNull() && c.size() >= 2) {
            q.lon = c[0] | 0.0;
            q.lat = c[1] | 0.0;
            q.depthKm = c.size() >= 3 ? (c[2] | 0.0) : 0.0;
        }

        out.push_back(q);
    }
}

double DistanceKm(double lat1, double lon1, double lat2, double lon2)
{
    constexpr double R = 6371.0; // mean Earth radius, km
    const double dLat = (lat2 - lat1) * M_PI / 180.0;
    const double dLon = (lon2 - lon1) * M_PI / 180.0;
    const double a = sin(dLat / 2) * sin(dLat / 2) +
                     cos(lat1 * M_PI / 180.0) * cos(lat2 * M_PI / 180.0) *
                     sin(dLon / 2) * sin(dLon / 2);
    return 2.0 * R * atan2(sqrt(a), sqrt(1 - a));
}

double BearingDeg(double lat1, double lon1, double lat2, double lon2)
{
    const double phi1 = lat1 * M_PI / 180.0, phi2 = lat2 * M_PI / 180.0;
    const double dLon = (lon2 - lon1) * M_PI / 180.0;
    const double y = sin(dLon) * cos(phi2);
    const double x = cos(phi1) * sin(phi2) - sin(phi1) * cos(phi2) * cos(dLon);
    double brg = atan2(y, x) * 180.0 / M_PI;
    if (brg < 0) brg += 360.0;
    return brg;
}

} // namespace seismic
