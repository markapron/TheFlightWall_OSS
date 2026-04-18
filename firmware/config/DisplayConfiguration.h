#pragma once

#include <Arduino.h>

namespace DisplayConfiguration
{
    // -------------------------------------------------------------------------
    // Nearby-flights card — border and loading/message screens
    // -------------------------------------------------------------------------
    static const uint8_t NEARBY_BORDER_R = 255;
    static const uint8_t NEARBY_BORDER_G = 255;
    static const uint8_t NEARBY_BORDER_B = 255;

    // Line 1: Airline / operator name
    static const uint8_t NEARBY_LINE1_R = 255;
    static const uint8_t NEARBY_LINE1_G = 255;
    static const uint8_t NEARBY_LINE1_B = 255;

    // Line 2: Route — IATA codes (bright), ICAO codes (dim), separator
    static const uint8_t NEARBY_LINE2_IATA_R = 255;
    static const uint8_t NEARBY_LINE2_IATA_G = 200;
    static const uint8_t NEARBY_LINE2_IATA_B = 50;

    static const uint8_t NEARBY_LINE2_ICAO_R = 130;
    static const uint8_t NEARBY_LINE2_ICAO_G = 100;
    static const uint8_t NEARBY_LINE2_ICAO_B = 25;

    static const uint8_t NEARBY_LINE2_SEP_R = 100;
    static const uint8_t NEARBY_LINE2_SEP_G = 100;
    static const uint8_t NEARBY_LINE2_SEP_B = 100;

    // Line 3: Aircraft type
    static const uint8_t NEARBY_LINE3_R = 100;
    static const uint8_t NEARBY_LINE3_G = 200;
    static const uint8_t NEARBY_LINE3_B = 255;

    // -------------------------------------------------------------------------
    // Tail tracker — status line (line 1)
    // -------------------------------------------------------------------------
    static const uint8_t TAIL_STATUS_R = 255;
    static const uint8_t TAIL_STATUS_G = 255;
    static const uint8_t TAIL_STATUS_B = 255;

    // -------------------------------------------------------------------------
    // Tail tracker — time line (line 2)
    // BRIGHT is used for digit characters; DIM for unit letters (h, m, etc.)
    // -------------------------------------------------------------------------
    static const uint8_t TAIL_TIME_R     = 255;
    static const uint8_t TAIL_TIME_G     = 200;
    static const uint8_t TAIL_TIME_B     =  50;

    static const uint8_t TAIL_TIME_DIM_R = 130;
    static const uint8_t TAIL_TIME_DIM_G = 100;
    static const uint8_t TAIL_TIME_DIM_B =  25;

    // -------------------------------------------------------------------------
    // Tail tracker — location line (line 3)
    // -------------------------------------------------------------------------
    static const uint8_t TAIL_LOC_R = 100;
    static const uint8_t TAIL_LOC_G = 200;
    static const uint8_t TAIL_LOC_B = 255;

    // -------------------------------------------------------------------------
    // Tail tracker — progress bar
    // BAR_FG is the filled (completed) portion; BAR_BG is the empty portion.
    // BAR_BG intentionally bypasses the global brightness scaler so the
    // background strip always reads as a very dim grey regardless of brightness.
    // -------------------------------------------------------------------------
    static const uint8_t TAIL_BAR_FG_R =   0;
    static const uint8_t TAIL_BAR_FG_G = 220;
    static const uint8_t TAIL_BAR_FG_B =   0;

    static const uint8_t TAIL_BAR_BG_R =  20;
    static const uint8_t TAIL_BAR_BG_G =  20;
    static const uint8_t TAIL_BAR_BG_B =  20;

    // -------------------------------------------------------------------------
    // Tail tracker — "Tracking" loading screen
    // -------------------------------------------------------------------------
    static const uint8_t TAIL_LOADING_R = 255;
    static const uint8_t TAIL_LOADING_G = 200;
    static const uint8_t TAIL_LOADING_B =  50;
}
