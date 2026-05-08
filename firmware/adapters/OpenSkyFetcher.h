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

private:
    String m_accessToken;
    unsigned long m_tokenExpiryMs = 0;

    bool ensureAccessToken(bool forceRefresh = false);
    bool requestAccessToken(String &outToken, unsigned long &outExpiryMs);
};
