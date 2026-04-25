#pragma once

#include <Arduino.h>
#include "models/TailFlightStatus.h"

// Fetches real-time flight status for a tracked tail number / ident from
// AeroAPI and reverse-geocodes the aircraft position via Nominatim.
class TailTrackerFetcher
{
public:
    TailTrackerFetcher() = default;

    // Populate `out` with the most recent flight data for `ident`.
    // Returns true on success; `out` is left unchanged on failure.
    bool fetchStatus(const String &ident, TailFlightStatus &out);

private:
    // Cached reverse-geocode result — reused until the aircraft moves more
    // than TailTrackerConfiguration::GEO_CACHE_THRESHOLD_KM.
    double _lastGeoLat = NAN;
    double _lastGeoLon = NAN;
    String _lastCity;
    String _lastRegion;

    // Falls back to GetLastTrack when last_position is absent from the
    // flights response.  Iterates the track array and returns the most
    // recent lat/lon/altitude.  Returns false if the track is unavailable
    // or empty.  outAlt is set to 0 when the endpoint omits altitude.
    bool fetchTrackPosition(const String &faFlightId,
                            double &outLat, double &outLon, int &outAlt);

    // Nominatim /search — one best hit; used to fill dest_lat/dest_lon for landed
    // state when the AeroAPI object omits airport coordinates (compass bearing).
    bool fetchForwardGeocodeForDestination(const String &searchQuery,
                                            double &outLat, double &outLon);

    bool fetchReverseGeocode(double lat, double lon,
                             String &outCity, String &outRegion);

    static const char *usStateAbbrev(const char *fullName);

    // Cache for forward geocode (identical query on each poll)
    String _lastForwardQuery;
    double _lastForwardLat = NAN;
    double _lastForwardLon = NAN;
};
