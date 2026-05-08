#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include "interfaces/BaseStateVectorFetcher.h"
#include "utils/GeoUtils.h"
#include "config/APIConfiguration.h"
#include "config/UserConfiguration.h"

class OpenSkyFetcher : public BaseStateVectorFetcher
{
public:
    OpenSkyFetcher() = default;
    ~OpenSkyFetcher() override = default;

    bool fetchStateVectors(double centerLat,
                           double centerLon,
                           double radiusKm,
                           std::vector<StateVector> &outStateVectors) override;

    // Fetch a single aircraft globally by its ICAO24 transponder address (6 hex chars).
    // Uses ?icao24= query so the response is tiny (≤1 aircraft).
    // Returns true and populates outState when the aircraft is found online.
    bool fetchByIcao24(const String &icao24Hex, StateVector &outState);

    bool ensureAuthenticated(bool forceRefresh = false);

    // Find a single tracked aircraft by callsign. Searches a bounding box around
    // (centerLat, centerLon) and returns the first state whose callsign matches
    // (case-insensitive, trimmed). Returns false if not found or on error.
    bool findAircraftByCallsign(const String &callsign,
                                double centerLat, double centerLon,
                                double radiusKm, StateVector &outSV);

    // Find a single aircraft by ICAO24 transponder hex code. Queries the OpenSky
    // API directly (no bounding box), so much more efficient than a callsign search.
    // Returns false if the aircraft is not currently tracked or on error.
    bool findAircraftByIcao24(const String &icao24, StateVector &outSV);

private:
    String m_accessToken;
    unsigned long m_tokenExpiryMs = 0;
    unsigned long m_rateLimitBackoffUntilMs = 0;

    // Persistent 49 KB document reused by fetchStateVectors and findAircraftByCallsign.
    // Allocated once at construction; clear()ed between uses to avoid repeated
    // heap alloc/free that fragments the heap over long runs.
    DynamicJsonDocument m_statesDoc{49152};

    bool ensureAccessToken(bool forceRefresh = false);
    bool requestAccessToken(String &outToken, unsigned long &outExpiryMs);

    // Shared GET + parse helper used by findAircraftByCallsign / findAircraftByIcao24.
    // path is appended to OPENSKY_BASE_URL. matchCallsign empty = accept any first state.
    // useSharedDoc: true uses m_statesDoc (49 KB, for bounding-box responses);
    //               false allocates a small local doc (for single icao24 responses).
    bool fetchAndFindAircraft(const String &path, const String &matchCallsign,
                              StateVector &outSV, bool useSharedDoc);
};
