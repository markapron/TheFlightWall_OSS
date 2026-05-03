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
    static const double GEO_CACHE_THRESHOLD_KM = 50.0;
}
