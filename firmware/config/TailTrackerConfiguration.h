#pragma once

#include <Arduino.h>
#include "Secrets.h"

namespace TailTrackerConfiguration
{
    // Aircraft registration or ident to track (e.g. "N12345", "UAL123").
    // Passed directly to AeroAPI /flights/{ident}; registrations and flight
    // numbers are both accepted.  Set the value in config/Secrets.h.
    static const char *TRACKED_TAIL_NUMBER = SECRET_TRACKED_TAIL_NUMBER;

    // How often to refresh tail tracker data while in tail tracker mode (seconds).
    static const unsigned long FETCH_INTERVAL_SECONDS = 60;

    // Minimum time between full matrix redraws in tail mode (avoids String churn /
    // heap fragmentation from repainting at loop() rate; elapsed time on screen
    // updates at most this often).
    static const unsigned long DISPLAY_REDRAW_MIN_MS = 500;

    // Serial log interval for free-RAM reporting while in tail mode (0 = disabled).
    static const unsigned long MEM_LOG_INTERVAL_MS = 60000;

    // Reverse-geocode cache threshold: only re-query Nominatim when the
    // aircraft has moved more than this distance since the last geocode (km).
    static const double GEO_CACHE_THRESHOLD_KM = 10.0;

    // AeroAPI position fallback: minimum interval between GET /flights/{id}/position
    // calls when OpenSky cannot locate the aircraft. Only fires when the aircraft is
    // believed to be airborne. Verify per-call cost before extending to production.
    static const unsigned long AEROAPI_POSITION_FALLBACK_INTERVAL_MS = 2UL * 60UL * 1000UL;

    // When OpenSky is rate-limited and the aircraft is on the ground, still call
    // AeroAPI /position to detect takeoffs. Slower rate than the airborne fallback.
    static const unsigned long AEROAPI_GROUND_FALLBACK_INTERVAL_MS = 60UL * 60UL * 1000UL;

    // Telemetry-based landing/takeoff inference thresholds (helicopter-tuned).
    // Infer landed when BOTH conditions hold for this many consecutive position polls.
    static const int INFER_LAND_CONSECUTIVE = 2;
    static const int INFER_LAND_ALT_FT      = 600;  // below 200 ft barometric
    static const int INFER_LAND_SPEED_KT    = 30;   // below 25 kt

    // Infer flying when BOTH conditions hold for this many consecutive polls.
    static const int INFER_FLY_CONSECUTIVE  = 2;
    static const int INFER_FLY_ALT_FT       = 600;  // above 400 ft
    static const int INFER_FLY_SPEED_KT     = 60;   // above 20 kt
}
