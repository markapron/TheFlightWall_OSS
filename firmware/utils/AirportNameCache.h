#pragma once

#include <Arduino.h>

struct AirportCacheEntry {
    char  code[8];   // IATA or ICAO, NUL-terminated
    char  name[48];  // Abbreviated airport name, NUL-terminated
    float lat;
    float lon;
};

namespace AirportNameCache {
    // Load from flash at startup — call from SerialConfig::begin().
    void begin();

    // Find a cached entry by airport code. Returns true and fills `out` if found.
    bool findByCode(const char *code, AirportCacheEntry &out);

    // Find the closest cached airport within threshKm. Returns true and fills `out`.
    bool findNearest(double lat, double lon, double threshKm, AirportCacheEntry &out);

    // Store an entry (overwrites same code, FIFO evicts oldest when full).
    // Flushes to persistent flash storage.
    void store(const char *code, const char *name, float lat, float lon);

    // Abbreviate long hospital substrings before storing.
    String abbreviateName(const String &raw);
}
