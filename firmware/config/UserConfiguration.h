#pragma once

#include <Arduino.h>

namespace UserConfiguration
{
    // Location configuration
    static const double CENTER_LAT = 40.6892; // San Francisco (example)
    static const double CENTER_LON = -73.9557;
    // static const double CENTER_LAT = 39.8370; // San Francisco (example)
    // static const double CENTER_LON = -75.4490;
    static const double RADIUS_KM = 30.0; // Search radius in km

    // Maximum number of flights to enrich with AeroAPI per fetch cycle.
    // Each enrichment is one HTTPS request. AeroAPI free/basic tiers have strict
    // per-minute rate limits, so keep this at 3-5.
    static const size_t MAX_ENRICHED_FLIGHTS = 3;

    // Overall display brightness (0-255).
    // Per-element colors are configured in config/DisplayConfiguration.h.
    static const uint8_t DISPLAY_BRIGHTNESS = 64;
}
