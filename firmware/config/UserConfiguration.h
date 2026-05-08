#pragma once

#include <Arduino.h>

namespace UserConfiguration
{
    // Location configuration
    // static const double CENTER_LAT = 40.6892; // San Francisco (example)
    // static const double CENTER_LON = -73.9557;
    static const double CENTER_LAT = 39.8370; // San Francisco (example)
    static const double CENTER_LON = -75.4490;
    static const double RADIUS_KM = 10.0; // Search radius in km

    // Maximum number of flights enriched with AeroAPI data per enrichment batch.
    // At NEARBY_ENRICH_INTERVAL_SECONDS=8h: 5 flights × 3 batches/day × 30 days = 450 calls/month.
    static const size_t MAX_ENRICHED_FLIGHTS = 5;

    // Maximum number of flights held in the nearby display pool (including
    // OpenSky-only entries that have no AeroAPI route data yet).
    // The display cycles through the whole pool; unenriched entries show
    // callsign + bearing + distance without airline/route/progress.
    static const size_t NEARBY_POOL_SIZE = 8;

    // Overall display brightness (0-255).
    // Per-element colors are configured in config/DisplayConfiguration.h.
    static const uint8_t DISPLAY_BRIGHTNESS = 64;
}
