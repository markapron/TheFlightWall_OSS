#pragma once

#include <Arduino.h>

namespace TimingConfiguration
{
    // How often to refresh positions from OpenSky in nearby mode (seconds).
    // OpenSky is free; this drives the position/compass update rate.
    static const uint32_t FETCH_INTERVAL_SECONDS = 30;

    // How long the nearby display pool persists before being flushed (seconds).
    // Pool accumulates up to NEARBY_POOL_SIZE flights during this window; once
    // full, new flights are ignored until the cycle resets.  AeroAPI is called
    // once per unique callsign discovery, keeping calls well under 500/month.
    static const uint32_t NEARBY_POOL_CYCLE_SECONDS = 8UL * 3600UL;

    // Display cycling configuration
    static const uint32_t DISPLAY_CYCLE_SECONDS = 6; // seconds per flight when multiple flights
}
