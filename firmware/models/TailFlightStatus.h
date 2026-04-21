#pragma once

#include <Arduino.h>

struct TailFlightStatus
{
    bool valid = false;

    // Flight identity
    String ident;

    // Status string from AeroAPI (e.g. "En Route", "Landed", "Scheduled").
    String status;

    // 0–100 progress through the flight. AeroAPI returns null for non-airborne
    // flights; we normalise: 0 for pre-departure, 100 for landed.
    int progress_percent = 0;

    // Unix epoch seconds; 0 means the event has not occurred yet.
    unsigned long actual_off_epoch = 0; // wheels up (takeoff)
    unsigned long actual_on_epoch  = 0; // wheels down (landing)

    // Last known position (NAN when unavailable).
    double lat = NAN;
    double lon = NAN;

    // Reverse-geocoded location strings.
    String city;
    String region; // 2-letter US state or 2-letter ISO country code

    // Best available destination airport code (IATA preferred, then local ID).
    // Shown on the status line when the flight has landed.
    String dest_code;

    // Snapshot taken at fetch time so the display can compute elapsed time
    // without polling WiFi.getTime() on every frame.
    unsigned long fetch_epoch  = 0; // epoch at time of successful fetch
    unsigned long fetch_millis = 0; // millis() at time of successful fetch
};
