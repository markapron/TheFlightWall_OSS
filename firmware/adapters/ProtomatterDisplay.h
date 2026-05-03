#pragma once

#include <stdint.h>
#include <vector>
#include <Arduino.h>
#include "interfaces/BaseDisplay.h"
#include "models/TailFlightStatus.h"

#if defined(FLIGHTWALL_DISPLAY_PROTOMATTER)
class Adafruit_Protomatter;

class ProtomatterDisplay : public BaseDisplay
{
public:
    ProtomatterDisplay();
    ~ProtomatterDisplay() override;

    bool initialize() override;
    void clear() override;
    void displayFlights(const std::vector<FlightInfo> &flights) override;

    // Tail tracker mode display.
    void displayTailTracker(const TailFlightStatus &status);
    void displayTailLoading();

    void displayMessage(const String &message);
    void showLoading();

private:
    Adafruit_Protomatter *_matrix = nullptr;
    uint16_t _matrixWidth = 0;
    uint16_t _matrixHeight = 0;

    size_t _currentFlightIndex = 0;
    unsigned long _lastCycleMs = 0;

    void drawTextLine(int16_t x, int16_t y, const String &text, uint16_t color);
    String truncateToColumns(const String &text, int maxColumns);
    void displaySingleFlightCard(const FlightInfo &f);
    void displayLoadingScreen();

    // Draw a north-up compass rose centered at (cx, cy) with the given ring
    // radius. The needle points toward bearingDeg (0 = north, 90 = east, etc.).
    void drawCompass(int16_t cx, int16_t cy, int16_t radius, double bearingDeg);
};
#else
// Stub so the file can be included in non-Protomatter builds without pulling
// in the Adafruit_Protomatter dependency.
class ProtomatterDisplay : public BaseDisplay
{
public:
    bool initialize() override { return false; }
    void clear() override {}
    void displayFlights(const std::vector<FlightInfo> &) override {}
    void displayTailTracker(const TailFlightStatus &) {}
    void displayTailLoading() {}
    void displayMessage(const String &) {}
    void showLoading() {}
};
#endif

