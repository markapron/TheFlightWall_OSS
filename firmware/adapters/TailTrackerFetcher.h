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
    // icao24Override: 6-char hex string that bypasses the N-number formula (pass ""
    // to derive from formula as normal).  Use this for aircraft whose FAA-assigned
    // ICAO24 doesn't match the computed formula value.
    // Returns true on success; `out` is left unchanged on failure.
    bool fetchStatus(const String &ident, TailFlightStatus &out,
                     const String &icao24Override = "");

private:
    OpenSkyFetcher *_openSky;

    // Reverse-geocode cache — reused until the aircraft moves more than
    // TailTrackerConfiguration::GEO_CACHE_THRESHOLD_KM.
    double _lastGeoLat = NAN;
    double _lastGeoLon = NAN;
    String _lastCity;
    String _lastRegion;

    // Forward-geocode cache (origin / destination city → lat/lon).
    String _lastForwardQuery;
    double _lastForwardLat = NAN;
    double _lastForwardLon = NAN;

    // Fetch route metadata (origin, destination, timestamps) from AeroAPI.
    // Updates the static route cache and returns true on success.
    bool fetchRouteFromAeroAPI(const String &ident);

    // Nominatim helpers (identical to original implementation).
    bool fetchReverseGeocode(double lat, double lon,
                             String &outCity, String &outRegion);
    bool fetchForwardGeocodeForDestination(const String &searchQuery,
                                           double &outLat, double &outLon);

    static const char *usStateAbbrev(const char *fullName);

    // Compute ICAO24 hex address from a US FAA N-number registration.
    // Returns true and sets outIcao24 (6 hex chars) for valid N-numbers.
    // Returns false for non-N-number idents (flight numbers, foreign regs).
    static bool nNumberToIcao24(const String &nNumber, char outHex[7]);
};
