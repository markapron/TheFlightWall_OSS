/*
Purpose: Orchestrate fetching and enrichment of flight data for display.
Flow:
1) Use BaseStateVectorFetcher to fetch nearby state vectors by geo filter.
2) Sort: airborne first (on_ground == false), then by distance — so AeroAPI slots
   favor flying aircraft and meaningful route progress.
3) For each callsign, use BaseFlightFetcher (e.g., AeroAPI) to retrieve FlightInfo.
4) Enrich names via FlightWallFetcher (airline/aircraft display names).
Output: Returns count of enriched flights and fills outStates/outFlights.
*/
#include "core/FlightDataFetcher.h"
#include "config/UserConfiguration.h"
#include "adapters/FlightWallFetcher.h"
#include "utils/GeoUtils.h"
#include "utils/MemoryUtils.h"
#include <algorithm>

// Airborne first (OpenSky on_ground == false), then nearest-first by distance.
static bool compareStateForEnrichment(const StateVector &a, const StateVector &b)
{
    if (a.on_ground != b.on_ground)
        return !a.on_ground;
    return a.distance_km < b.distance_km;
}

FlightDataFetcher::FlightDataFetcher(BaseStateVectorFetcher *stateFetcher,
                                     BaseFlightFetcher *flightFetcher)
    : _stateFetcher(stateFetcher), _flightFetcher(flightFetcher) {}

static bool hasPlausibleLatLon(double lat, double lon)
{
    if (isnan(lat) || isnan(lon))
        return false;
    if (lat == 0.0 && lon == 0.0)
        return false;
    return true;
}

size_t FlightDataFetcher::fetchFlights(std::vector<StateVector> &outStates,
                                       std::vector<FlightInfo> &outFlights)
{
    outStates.clear();
    outFlights.clear();

    bool ok = _stateFetcher->fetchStateVectors(
        UserConfiguration::CENTER_LAT,
        UserConfiguration::CENTER_LON,
        UserConfiguration::RADIUS_KM,
        outStates);
    if (!ok)
        return 0;

    std::sort(outStates.begin(), outStates.end(), compareStateForEnrichment);

    // Reuse a single instance so each enrichment pass does not construct HTTP/temp
    // state on a constrained heap.
    static FlightWallFetcher s_flightWall;

    // Enrich up to MAX_ENRICHED_FLIGHTS entries in priority order (airborne first,
    // then by distance) so AeroAPI progress reflects en-route flights when possible.
    size_t enriched = 0;
    for (const StateVector &s : outStates)
    {
        if (enriched >= UserConfiguration::MAX_ENRICHED_FLIGHTS)
            break;

        if (s.callsign.length() == 0)
            continue;

        FlightInfo info;
        if (!_flightFetcher->fetchFlightInfo(s.callsign, info))
            continue;

        // Nearby progress fallback: AeroAPI intermittently omits progress_percent.
        // If we have origin/dest airport coordinates (from AeroAPI) and a current
        // aircraft fix (from OpenSky), compute a best-effort route completion.
        if (info.progress_percent <= 0
            && hasPlausibleLatLon(s.lat, s.lon)
            && hasPlausibleLatLon(info.origin.latitude, info.origin.longitude)
            && hasPlausibleLatLon(info.destination.latitude, info.destination.longitude))
        {
            const double totalKm = haversineKm(info.origin.latitude, info.origin.longitude,
                                               info.destination.latitude, info.destination.longitude);
            // Ignore degenerate routes / missing coords.
            if (totalKm > 10.0)
            {
                const double traveledKm = haversineKm(info.origin.latitude, info.origin.longitude,
                                                     s.lat, s.lon);
                int prog = (int)lround((traveledKm * 100.0) / totalKm);
                if (prog < 0)   prog = 0;
                if (prog > 100) prog = 100;
                info.progress_percent = prog;
            }
        }

        if (info.operator_icao.length())
        {
            String airlineFull;
            if (s_flightWall.getAirlineName(info.operator_icao, airlineFull))
                info.airline_display_name_full = airlineFull;
            flightwallStringDrop(airlineFull);
        }
        if (info.aircraft_code.length())
        {
            String aircraftShort, aircraftFull;
            if (s_flightWall.getAircraftName(info.aircraft_code, aircraftShort, aircraftFull))
            {
                if (aircraftShort.length())
                    info.aircraft_display_name_short = aircraftShort;
            }
            flightwallStringDrop(aircraftShort);
            flightwallStringDrop(aircraftFull);
        }
        info.bearing_deg  = s.bearing_deg;
        info.distance_km  = s.distance_km;
        outFlights.push_back(info);
        enriched++;
    }
    return enriched;
}
