#pragma once

#include <Arduino.h>
#include "Secrets.h"

namespace TailTrackerConfiguration
{
    // Aircraft registration or ident to track (e.g. "N12345", "UAL123").
    // Passed directly to AeroAPI /flights/{ident}; registrations and flight
    // numbers are both accepted.  Set the value in config/Secrets.h.
    static const char *TRACKED_TAIL_NUMBER = SECRET_TRACKED_TAIL_NUMBER;

    // How often to refresh AeroAPI enrichment data (route, status, timestamps) in seconds.
    static const unsigned long ENRICHMENT_FETCH_INTERVAL_SECONDS = 300;

    // How often to poll OpenSky for a live position update in tail tracker mode (seconds).
    static const unsigned long POSITION_FETCH_INTERVAL_SECONDS = 30;

    // Bounding-box search radius for OpenSky callsign lookup when ICAO24 is not yet cached.
    static const double POSITION_SEARCH_RADIUS_KM = 400.0;

    // Minimum time between full matrix redraws in tail mode (avoids String churn /
    // heap fragmentation from repainting at loop() rate; elapsed time on screen
    // updates at most this often).
    static const unsigned long DISPLAY_REDRAW_MIN_MS = 500;

    // Serial log interval for free-RAM reporting while in tail mode (0 = disabled).
    static const unsigned long MEM_LOG_INTERVAL_MS = 60000;

}
