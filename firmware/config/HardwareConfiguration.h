#pragma once

#include <Arduino.h>

namespace HardwareConfiguration
{
    // --- HUB75 RGB matrix (Adafruit Matrix Portal M4 / Protomatter) ---
    // Defaults from Adafruit's MatrixPortal M4 Protomatter guide.
    static const uint16_t HUB75_MATRIX_WIDTH = 64;
    static const uint16_t HUB75_MATRIX_HEIGHT = 32;
    static const uint8_t HUB75_BIT_DEPTH = 4;       // 1-6
    static const bool HUB75_DOUBLE_BUFFER = true;   // smoother updates, more RAM
    static const uint8_t HUB75_RGB_PINS[6] = {7, 8, 9, 10, 11, 12}; // R1,G1,B1,R2,G2,B2
    static const uint8_t HUB75_ADDR_PINS_32[4] = {17, 18, 19, 20};  // A,B,C,D (64x32)
    static const uint8_t HUB75_CLK_PIN = 14;
    static const uint8_t HUB75_LAT_PIN = 15;
    static const uint8_t HUB75_OE_PIN = 16;

    // --- NeoPixel matrix (ESP32 / FastLED NeoMatrix) ---
    // These are not used on Matrix Portal M4's HUB75 output, but kept so the ESP32 build can
    // target a WS2812/NeoPixel-style matrix using the existing NeoMatrixDisplay adapter.
#if defined(FLIGHTWALL_NEOMATRIX_DATA_PIN)
    static const uint8_t NEOMATRIX_DATA_PIN = (uint8_t)FLIGHTWALL_NEOMATRIX_DATA_PIN;
#else
    static const uint8_t NEOMATRIX_DATA_PIN = 9; // WS2812 data pin (override per env)
#endif
    static const uint16_t NEOMATRIX_TILE_PIXEL_W = 64;
    static const uint16_t NEOMATRIX_TILE_PIXEL_H = 32;
    static const uint8_t NEOMATRIX_TILES_X = 1;
    static const uint8_t NEOMATRIX_TILES_Y = 1;
    static const uint16_t NEOMATRIX_MATRIX_WIDTH = NEOMATRIX_TILE_PIXEL_W * NEOMATRIX_TILES_X;
    static const uint16_t NEOMATRIX_MATRIX_HEIGHT = NEOMATRIX_TILE_PIXEL_H * NEOMATRIX_TILES_Y;
}
