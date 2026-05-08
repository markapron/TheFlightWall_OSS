#pragma once

#include <Arduino.h>
#include "models/TailFlightStatus.h"
#include "adapters/OpenSkyFetcher.h"

/*
Purpose: Fetch real-time status for a tracked tail number.
Strategy:
  - Route data (origin, destination, timestamps, ident) is fetched from AeroAPI
    ONCE per flight leg and cached.  This dramatically reduces AeroAPI usage.
  - For US N-number registrations the ICAO24 transponder address is computed
    locally (no API call) so OpenSky can locate the aircraft globally via
    fetchByIcao24() on every poll.
  - Position, altitude, and on-ground status come from OpenSky on every call.
  - Flight progress is computed geometrically from the cached origin/destination
    coordinates and the current OpenSky position.
  - Reverse geocoding (Nominatim) is called only when the aircraft moves more
    than GEO_CACHE_THRESHOLD_KM since the last geocode.
  - AeroAPI is refreshed when:
      * The configured ident changes.
      * A landing transition is detected (on_ground flip while airborne).
      * The cached route is older than ROUTE_REFRESH_INTERVAL_MS (safety net).
*/
class TailTrackerFetcher
{
public:
    explicit TailTrackerFetcher(OpenSkyFetcher *openSky);

    // Populate `out` with the most recent flight data for `ident`.
    // Returns true on success; `out` is left unchanged on failure.
    bool fetchStatus(const String &ident, TailFlightStatus &out);

    // Reverse-geocode lat/lon to city/region. Re-queries Nominatim at most once
    // per POSITION_FETCH_INTERVAL_SECONDS*2; returns cached result otherwise.
    bool fetchReverseGeocode(double lat, double lon,
                             String &outCity, String &outRegion);

    // Keep the persistent sticky position in sync with live OpenSky fixes so that
    // when AeroAPI fetchStatus falls back to sticky (no last_position in response)
    // it uses the most recent known location rather than a stale en-route fix.
    static void updateStickyPosition(double lat, double lon, int altFt);

private:
    OpenSkyFetcher *_openSky;

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

    // Compute ICAO24 hex address from a US FAA N-number registration.
    // Returns true and sets outIcao24 (6 hex chars) for valid N-numbers.
    // Returns false for non-N-number idents (flight numbers, foreign regs).
    static bool nNumberToIcao24(const String &nNumber, char outHex[7]);
};
