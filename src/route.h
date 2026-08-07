#pragma once
// Shared route state (origin -> destination by callsign). Portable: the UI thread
// requests a lookup, a network task fulfils it, the UI reads the result.
#include "config.h"
#include <stddef.h>

// One end of a route, as adsbdb describes it. All three labels are kept together so the
// display format (AirportLabelFormat) can change without invalidating a lookup or a cache
// entry. name is always filled in when the airport is known — it is the fallback for every
// format, because small and military fields frequently have no IATA code.
struct RouteAirport {
    char iata[4]  = "";   // 3-letter code, e.g. "LAX" (often empty)
    char icao[5]  = "";   // 4-letter code, e.g. "KLAX"
    char name[40] = "";   // shortened airport/city name, e.g. "London Heathrow"
};

void route_request(const char *callsign);                     // UI: want a route for this callsign
bool route_pending(char *callOut, size_t n);                  // task: is a lookup needed? returns callsign
void route_store(const char *callsign, const RouteAirport &from, const RouteAirport &to);  // task/sim: store result
bool route_get(const char *callsign, RouteAirport &from, RouteAirport &to);   // UI: read result

// Render one end of a route for the detail card. Falls back to the full name whenever the
// requested code is missing, so an airport with no IATA code still reads sensibly.
void route_format(const RouteAirport &ap, AirportLabelFormat fmt, char *out, size_t n);
