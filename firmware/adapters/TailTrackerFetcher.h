#pragma once

#include <Arduino.h>
#include "models/TailFlightStatus.h"

/*
Purpose: Fetch real-time status for a tracked tail number.
Strategy:
  - Route data (origin, destination, timestamps, ident) is fetched from AeroAPI
    ONCE per flight leg and cached.  This dramatically reduces AeroAPI usage.
  - Position, altitude, and on-ground status come exclusively from the 30-second
    OpenSky timer in main.cpp via updateStickyPosition() / fetchReverseGeocode().
    fetchStatus() never calls OpenSky directly.
  - Flight progress is computed geometrically from the cached origin/destination
    coordinates and the sticky position.
  - AeroAPI is refreshed when:
      * The configured ident changes.
      * flagRouteNeedsRefresh() is called (e.g. landing detected via OpenSky).
      * The cached route is older than ROUTE_REFRESH_INTERVAL_MS (safety net).
*/
class TailTrackerFetcher
{
public:
    TailTrackerFetcher() = default;

    // Populate `out` with the most recent flight data for `ident`.
    // Returns true on success; `out` is left unchanged on failure.
    bool fetchStatus(const String &ident, TailFlightStatus &out);

    // Reverse-geocode lat/lon to city/region. Re-queries Nominatim at most once
    // per POSITION_FETCH_INTERVAL_SECONDS*2; returns cached result otherwise.
    bool fetchReverseGeocode(double lat, double lon,
                             String &outCity, String &outRegion);

    // Keep the persistent sticky position in sync with live OpenSky fixes so that
    // fetchStatus() can use the most recent known location for progress computation.
    static void updateStickyPosition(double lat, double lon, int altFt);

    // Signal that the cached route should be re-fetched from AeroAPI on the next
    // fetchStatus() call.  Called from main.cpp when OpenSky detects landing.
    static void flagRouteNeedsRefresh();

    // Compute ICAO24 hex address from a US FAA N-number registration.
    // Returns true and sets outHex (6 hex chars + NUL) for valid N-numbers.
    // Returns false for non-N-number idents (flight numbers, foreign regs).
    static bool nNumberToIcao24(const String &nNumber, char outHex[7]);

private:
    // Cached reverse-geocode result — refreshed on a time interval.
    unsigned long _lastGeocodeMs = 0;
    String _lastCity;
    String _lastRegion;

    // Falls back to GetLastTrack when last_position is absent from the
    // flights response.  Iterates the track array and returns the most
    // recent lat/lon/altitude.  Returns false if the track is unavailable
    // or empty.  outAlt is set to 0 when the endpoint omits altitude.
    bool fetchTrackPosition(const String &faFlightId,
                            double &outLat, double &outLon, int &outAlt);

    // Cache for forward geocode (identical query on each poll)
    String _lastForwardQuery;
    double _lastForwardLat = NAN;
    double _lastForwardLon = NAN;

    // Fetch route metadata (origin, destination, timestamps) from AeroAPI.
    // Updates the static route cache and returns true on success.
    bool fetchRouteFromAeroAPI(const String &ident);

    bool fetchForwardGeocodeForDestination(const String &searchQuery,
                                           double &outLat, double &outLon);

    static const char *usStateAbbrev(const char *fullName);
};
