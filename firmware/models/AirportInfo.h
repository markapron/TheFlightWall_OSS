#pragma once

#include <Arduino.h>

struct AirportInfo
{
    String code_icao;
    String code_iata;
    // Optional lat/lon from AeroAPI (NAN when unavailable).
    double latitude  = NAN;
    double longitude = NAN;
};
