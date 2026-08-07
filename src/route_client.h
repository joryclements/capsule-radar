#pragma once
// Look up a flight's origin/destination by callsign via adsbdb.com (free, no key).
// Device-only (uses WiFi/HTTPS). City names are returned in English.
#include "route.h"

bool route_fetch(const char *callsign, RouteAirport &from, RouteAirport &to);

// NVS route cache (avoids re-querying adsbdb for the same flight across reboots).
// Entries hold every label form, so the user's display format can change freely; only a
// change to the stored layout itself (ROUTE_FMT_VER) invalidates them.
void route_cache_begin();   // call once at boot; clears the cache if the stored layout changed
bool route_cache_get(const char *callsign, RouteAirport &from, RouteAirport &to);
void route_cache_put(const char *callsign, const RouteAirport &from, const RouteAirport &to);
