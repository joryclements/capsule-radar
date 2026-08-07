// Route lookup via adsbdb.com (free, no API key): GET /v0/callsign/{callsign}.
// Returns origin/destination city names (English). Device-only.
#include "route_client.h"
#include "config.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <string.h>
#include <time.h>   // route-cache TTL

#define ROUTE_CACHE_MAX 200   // wrap the cache before it can crowd NVS

// strip spaces -> a valid NVS key (callsigns are <= 8 chars)
static void route_key(const char *callsign, char *out, size_t on) {
    size_t j = 0;
    for (const char *p = callsign; *p && j < on - 1; ++p)
        if (*p != ' ') out[j++] = *p;
    out[j] = 0;
}

// Bump to invalidate cached routes when the stored field layout changes. NOT the user's
// chosen label format: every form is cached, so switching format must keep the cache.
//   v2 = "epoch|from|to"
//   v3 = "epoch|iata|icao|name|iata|icao|name"  (origin, then destination)
#define ROUTE_FMT_VER 3
#define ROUTE_FIELDS  7   // fields in a v3 cache entry

void route_cache_begin() {
    Preferences p;
    if (!p.begin("routes", false)) return;
    if (p.getUChar("__v", 0) != ROUTE_FMT_VER) { p.clear(); p.putUChar("__v", ROUTE_FMT_VER); }
    p.end();
}

// Split "a|b|c" into up to n parts; returns how many were found.
static int split_pipe(const String &v, String *out, int n) {
    int part = 0, start = 0;
    while (part < n) {
        const int b = v.indexOf('|', start);
        if (b < 0) { out[part++] = v.substring(start); break; }
        out[part++] = v.substring(start, b);
        start = b + 1;
    }
    return part;
}

bool route_cache_get(const char *callsign, RouteAirport &from, RouteAirport &to) {
    from = RouteAirport();
    to   = RouteAirport();
    if (!callsign || !callsign[0]) return false;
    char key[12];
    route_key(callsign, key, sizeof(key));
    if (!key[0]) return false;
    Preferences p;
    if (!p.begin("routes", true)) return false;
    String v = p.getString(key, "");     // stored as "epoch|iata|icao|name|iata|icao|name"
    p.end();
    if (v.length() == 0) return false;
    String f[ROUTE_FIELDS];
    if (split_pipe(v, f, ROUTE_FIELDS) != ROUTE_FIELDS) return false;
    const uint32_t ts  = (uint32_t)f[0].toInt();
    const uint32_t now = (uint32_t)time(nullptr);    // expire stale routes (reused callsigns)
    if (now > 1700000000UL && ts > 1700000000UL && (now - ts) > 86400UL) return false;  // 24 h TTL
    snprintf(from.iata, sizeof(from.iata), "%s", f[1].c_str());
    snprintf(from.icao, sizeof(from.icao), "%s", f[2].c_str());
    snprintf(from.name, sizeof(from.name), "%s", f[3].c_str());
    snprintf(to.iata,   sizeof(to.iata),   "%s", f[4].c_str());
    snprintf(to.icao,   sizeof(to.icao),   "%s", f[5].c_str());
    snprintf(to.name,   sizeof(to.name),   "%s", f[6].c_str());
    return true;
}

void route_cache_put(const char *callsign, const RouteAirport &from, const RouteAirport &to) {
    if (!callsign || !callsign[0]) return;
    char key[12];
    route_key(callsign, key, sizeof(key));
    if (!key[0]) return;
    Preferences p;
    if (!p.begin("routes", false)) return;
    int n = p.getInt("__n", 0);
    if (n >= ROUTE_CACHE_MAX) { p.clear(); n = 0; }   // wrap to bound NVS usage
    String v = String((uint32_t)time(nullptr));
    v += "|" + String(from.iata) + "|" + String(from.icao) + "|" + String(from.name);
    v += "|" + String(to.iata)   + "|" + String(to.icao)   + "|" + String(to.name);
    if (p.putString(key, v) > 0) p.putInt("__n", n + 1);
    p.end();
}

// Fill one end of the route. Every label form is kept, whatever the user's current choice
// is, so switching format later never needs a refetch. out.name is the most recognizable
// short label: a cleaned-up name ("Teesside", "Palma de Mallorca", "London Heathrow"),
// falling back to the municipality, then the IATA code.
static void pick_airport(JsonObjectConst ap, RouteAirport &out) {
    snprintf(out.iata, sizeof(out.iata), "%s", (const char *)(ap["iata_code"] | ""));
    snprintf(out.icao, sizeof(out.icao), "%s", (const char *)(ap["icao_code"] | ""));
    String s = (const char *)(ap["name"] | "");
    s.replace(" International Airport", "");
    s.replace(" Regional Airport", "");
    s.replace(" Airport", "");
    s.replace(" International", "");
    s.trim();
    if (s.length() == 0 || s.length() > 18) {           // name missing or too long -> municipality/IATA
        const char *muni = ap["municipality"] | "";
        snprintf(out.name, sizeof(out.name), "%s", muni[0] ? muni : out.iata);
        return;
    }
    snprintf(out.name, sizeof(out.name), "%s", s.c_str());
}

bool route_fetch(const char *callsign, RouteAirport &from, RouteAirport &to) {
    from = RouteAirport();
    to   = RouteAirport();
    if (!callsign || !callsign[0] || WiFi.status() != WL_CONNECTED) return false;

    // strip spaces from the callsign
    char cs[12];
    size_t j = 0;
    for (const char *p = callsign; *p && j < sizeof(cs) - 1; ++p)
        if (*p != ' ') cs[j++] = *p;
    cs[j] = 0;
    if (j == 0) return false;

    char url[96];
    snprintf(url, sizeof(url), "https://api.adsbdb.com/v0/callsign/%s", cs);

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(3000);   // short: runs on the feed task, don't stall the live poll
    http.setTimeout(6000);
    if (!http.begin(client, url)) return false;
    http.addHeader("User-Agent", ADSB_USER_AGENT);

    const int code = http.GET();
    if (code != 200) { http.end(); return false; }

    JsonDocument filter;
    filter["response"]["flightroute"]["origin"]["municipality"] = true;
    filter["response"]["flightroute"]["origin"]["iata_code"] = true;
    filter["response"]["flightroute"]["origin"]["icao_code"] = true;
    filter["response"]["flightroute"]["origin"]["name"] = true;
    filter["response"]["flightroute"]["destination"]["municipality"] = true;
    filter["response"]["flightroute"]["destination"]["iata_code"] = true;
    filter["response"]["flightroute"]["destination"]["icao_code"] = true;
    filter["response"]["flightroute"]["destination"]["name"] = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream(),
                                               DeserializationOption::Filter(filter));
    http.end();
    if (err) return false;

    JsonObjectConst fr = doc["response"]["flightroute"].as<JsonObjectConst>();
    if (fr.isNull()) return false;   // "unknown callsign" etc.

    pick_airport(fr["origin"].as<JsonObjectConst>(), from);
    pick_airport(fr["destination"].as<JsonObjectConst>(), to);
    return (from.name[0] || to.name[0] || from.iata[0] || to.iata[0] || from.icao[0] || to.icao[0]);
}
