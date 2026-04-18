#pragma once

#include <Arduino.h>

namespace UserConfiguration
{
    // Location configuration
    static const double CENTER_LAT = 37.7749; // San Francisco (example)
    static const double CENTER_LON = -122.4194;
    static const double RADIUS_KM = 10.0; // Search radius in km

    // Maximum number of flights to enrich with AeroAPI per fetch cycle.
    // Each enrichment is one HTTPS request. AeroAPI free/basic tiers have strict
    // per-minute rate limits, so keep this at 3-5.
    static const size_t MAX_ENRICHED_FLIGHTS = 3;

    // Display customization
    // Brightness controls overall display brightness (0-255)
    static const uint8_t DISPLAY_BRIGHTNESS = 64;

    // RGB color for all text rendering on the LED matrix
    static const uint8_t TEXT_COLOR_R = 255;
    static const uint8_t TEXT_COLOR_G = 255;
    static const uint8_t TEXT_COLOR_B = 255;
}
