#pragma once

#include <Arduino.h>
#include <vector>
#include "interfaces/BaseStateVectorFetcher.h"
#include "interfaces/BaseFlightFetcher.h"
#include "models/StateVector.h"
#include "models/FlightInfo.h"

class FlightDataFetcher
{
public:
    FlightDataFetcher(BaseStateVectorFetcher *stateFetcher,
                      BaseFlightFetcher *flightFetcher);

    // Full fetch: OpenSky + AeroAPI enrichment + CDN display names.
    // Called on the slow AeroAPI enrichment schedule (~every 8 h).
    size_t fetchFlights(std::vector<StateVector> &outStates,
                        std::vector<FlightInfo> &outFlights);

    // Sticky pool update: maintains the pool across the 8-hour cycle window.
    // - Existing pool entries whose callsign appears in newStates get their
    //   position/bearing/progress refreshed; entries NOT in newStates keep their
    //   last known values (they stay in the pool even after leaving the bbox).
    // - New callsigns from newStates are appended (as unenriched stubs) only if
    //   pool.size() < maxPoolSize.  Once full, new aircraft are ignored.
    // Returns the number of entries newly added to the pool.
    size_t updateStickyPool(std::vector<FlightInfo> &pool,
                            const std::vector<StateVector> &newStates,
                            size_t maxPoolSize);

    // Enrich any unenriched pool entries with AeroAPI + CDN display names.
    // Uses an internal per-callsign cache (TTL = 4 h) so the same flight ident
    // never triggers more than one AeroAPI call per cache window.
    // `states` provides the current lat/lon for haversine progress computation.
    // Returns the number of actual AeroAPI calls made (cache hits = 0 counted).
    size_t enrichNewPoolEntries(std::vector<FlightInfo> &pool,
                                const std::vector<StateVector> &states,
                                size_t maxNewCalls);

private:
    BaseStateVectorFetcher *_stateFetcher;
    BaseFlightFetcher *_flightFetcher;
};
