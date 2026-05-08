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

// ---------------------------------------------------------------------------
// Per-callsign enrichment cache
// ---------------------------------------------------------------------------
struct EnrichCacheEntry
{
    String       callsign;
    FlightInfo   info;
    unsigned long cachedAtMs = 0;
};

static std::vector<EnrichCacheEntry> s_enrichCache;
static const unsigned long kEnrichCacheTtlMs  = 4UL * 3600UL * 1000UL; // 4 hours
static const size_t        kMaxEnrichCacheSize = 30;

static bool isEnrichedFlight(const FlightInfo &f)
{
    return f.operator_icao.length() > 0
        || f.origin.code_icao.length() > 0
        || f.origin.code_iata.length() > 0;
}

size_t FlightDataFetcher::enrichNewPoolEntries(std::vector<FlightInfo> &pool,
                                               const std::vector<StateVector> &states,
                                               size_t maxNewCalls)
{
    static FlightWallFetcher s_flightWall;

    const unsigned long nowMs = millis();
    size_t apiCallsMade = 0;

    for (FlightInfo &pf : pool)
    {
        if (isEnrichedFlight(pf))
            continue; // already has AeroAPI data
        if (pf.ident.length() == 0)
            continue;

        // Locate the matching state vector for haversine progress computation.
        const StateVector *matchSv = nullptr;
        for (const StateVector &sv : states)
        {
            String cs = sv.callsign;
            cs.trim();
            if (cs.equalsIgnoreCase(pf.ident))
            {
                matchSv = &sv;
                break;
            }
        }

        // --- Cache lookup ---
        bool appliedFromCache = false;
        for (EnrichCacheEntry &ce : s_enrichCache)
        {
            if (!ce.callsign.equalsIgnoreCase(pf.ident))
                continue;

            if (nowMs - ce.cachedAtMs >= kEnrichCacheTtlMs)
                break; // expired — fall through to AeroAPI

            // Cache hit: copy enriched route data, keep current position fields.
            double savedBearing  = pf.bearing_deg;
            double savedDistance = pf.distance_km;
            pf                   = ce.info;
            pf.bearing_deg       = savedBearing;
            pf.distance_km       = savedDistance;
            pf.progress_percent  = ce.info.progress_percent; // use cached value

            // Re-compute progress geometrically with current position if possible.
            if (matchSv
                && hasPlausibleLatLon(matchSv->lat, matchSv->lon)
                && hasPlausibleLatLon(pf.origin.latitude, pf.origin.longitude)
                && hasPlausibleLatLon(pf.destination.latitude, pf.destination.longitude))
            {
                const double totalKm = haversineKm(pf.origin.latitude, pf.origin.longitude,
                                                   pf.destination.latitude, pf.destination.longitude);
                if (totalKm > 10.0)
                {
                    const double traveledKm = haversineKm(pf.origin.latitude, pf.origin.longitude,
                                                          matchSv->lat, matchSv->lon);
                    int prog = (int)lround((traveledKm * 100.0) / totalKm);
                    if (prog < 0)   prog = 0;
                    if (prog > 100) prog = 100;
                    pf.progress_percent = prog;
                }
            }

            appliedFromCache = true;
            break;
        }

        if (appliedFromCache)
            continue;

        // --- Budget check ---
        if (apiCallsMade >= maxNewCalls)
            continue; // defer remaining new flights to the next 30-s cycle

        // --- AeroAPI call ---
        FlightInfo info;
        if (!_flightFetcher->fetchFlightInfo(pf.ident, info))
            continue;

        // Haversine progress fallback.
        if (info.progress_percent <= 0
            && matchSv
            && hasPlausibleLatLon(matchSv->lat, matchSv->lon)
            && hasPlausibleLatLon(info.origin.latitude, info.origin.longitude)
            && hasPlausibleLatLon(info.destination.latitude, info.destination.longitude))
        {
            const double totalKm = haversineKm(info.origin.latitude, info.origin.longitude,
                                               info.destination.latitude, info.destination.longitude);
            if (totalKm > 10.0)
            {
                const double traveledKm = haversineKm(info.origin.latitude, info.origin.longitude,
                                                      matchSv->lat, matchSv->lon);
                int prog = (int)lround((traveledKm * 100.0) / totalKm);
                if (prog < 0)   prog = 0;
                if (prog > 100) prog = 100;
                info.progress_percent = prog;
            }
        }

        // CDN names.
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

        // Store in cache (update existing slot or append).
        bool cacheUpdated = false;
        for (EnrichCacheEntry &ce : s_enrichCache)
        {
            if (ce.callsign.equalsIgnoreCase(pf.ident))
            {
                ce.info        = info;
                ce.cachedAtMs  = nowMs;
                cacheUpdated   = true;
                break;
            }
        }
        if (!cacheUpdated && s_enrichCache.size() < kMaxEnrichCacheSize)
        {
            EnrichCacheEntry e;
            e.callsign   = pf.ident;
            e.info        = info;
            e.cachedAtMs  = nowMs;
            s_enrichCache.push_back(e);
        }

        // Apply to pool entry, preserving OpenSky position fields.
        double savedBearing  = pf.bearing_deg;
        double savedDistance = pf.distance_km;
        pf                   = info;
        pf.bearing_deg       = savedBearing;
        pf.distance_km       = savedDistance;

        ++apiCallsMade;
    }

    return apiCallsMade;
}

size_t FlightDataFetcher::updateStickyPool(std::vector<FlightInfo> &pool,
                                           const std::vector<StateVector> &newStates,
                                           size_t maxPoolSize)
{
    // --- Step 1: refresh position for pool entries currently visible in newStates ---
    for (FlightInfo &pf : pool)
    {
        for (const StateVector &sv : newStates)
        {
            String cs = sv.callsign;
            cs.trim();
            if (!cs.equalsIgnoreCase(pf.ident))
                continue;

            pf.bearing_deg = sv.bearing_deg;
            pf.distance_km = sv.distance_km;

            // Recompute haversine progress with current fix if airport coords known.
            if (hasPlausibleLatLon(sv.lat, sv.lon)
                && hasPlausibleLatLon(pf.origin.latitude, pf.origin.longitude)
                && hasPlausibleLatLon(pf.destination.latitude, pf.destination.longitude))
            {
                const double totalKm = haversineKm(pf.origin.latitude, pf.origin.longitude,
                                                   pf.destination.latitude, pf.destination.longitude);
                if (totalKm > 10.0)
                {
                    const double traveled = haversineKm(pf.origin.latitude, pf.origin.longitude,
                                                        sv.lat, sv.lon);
                    int prog = (int)lround((traveled * 100.0) / totalKm);
                    if (prog < 0)   prog = 0;
                    if (prog > 100) prog = 100;
                    pf.progress_percent = prog;
                }
            }
            break;
        }
        // Flight not found in newStates → left bbox; pool entry persists unchanged.
    }

    // --- Step 2: append new callsigns only if the pool still has capacity ---
    if (pool.size() >= maxPoolSize)
        return 0;

    size_t added = 0;
    for (const StateVector &sv : newStates)
    {
        if (pool.size() >= maxPoolSize)
            break;
        if (sv.callsign.length() == 0)
            continue;

        String cs = sv.callsign;
        cs.trim();
        if (cs.length() == 0)
            continue;

        // Skip if already in pool.
        bool inPool = false;
        for (const FlightInfo &pf : pool)
        {
            if (pf.ident.equalsIgnoreCase(cs)) { inPool = true; break; }
        }
        if (inPool)
            continue;

        // Add unenriched stub; enrichNewPoolEntries will fill in route data.
        FlightInfo fi;
        fi.ident       = cs;
        fi.bearing_deg = sv.bearing_deg;
        fi.distance_km = sv.distance_km;
        pool.push_back(fi);
        ++added;
    }
    return added;
}
