/*
Purpose: Render flight info on a HUB75 RGB matrix via Adafruit_Protomatter.
Responsibilities:
- Initialize RGB matrix based on HardwareConfiguration and user display settings.
- Render a bordered, three-line flight “card” and a minimal loading screen.
- Cycle through multiple flights at a configurable interval.
*/
#include "adapters/ProtomatterDisplay.h"

#if defined(FLIGHTWALL_DISPLAY_PROTOMATTER)
#include <Adafruit_Protomatter.h>
#include "config/UserConfiguration.h"
#include "config/HardwareConfiguration.h"
#include "config/TimingConfiguration.h"

static uint8_t scaleByBrightness(uint8_t c)
{
    // UserConfiguration::DISPLAY_BRIGHTNESS is 0-255, but the original project used very low
    // values (e.g. 5) for NeoPixel matrices. On HUB75 panels this can be effectively black,
    // so enforce a minimum for visible output.
    uint16_t b = (uint16_t)UserConfiguration::DISPLAY_BRIGHTNESS;
    if (b > 0 && b < 64)
    {
        b = 64;
    }
    return (uint8_t)(((uint16_t)c * b) / 255U);
}

static uint16_t colorWithBrightness(Adafruit_Protomatter *m, uint8_t r, uint8_t g, uint8_t b)
{
    return m->color565(scaleByBrightness(r), scaleByBrightness(g), scaleByBrightness(b));
}

ProtomatterDisplay::ProtomatterDisplay() {}

ProtomatterDisplay::~ProtomatterDisplay()
{
    if (_matrix)
    {
        delete _matrix;
        _matrix = nullptr;
    }
}

bool ProtomatterDisplay::initialize()
{
    _matrixWidth = HardwareConfiguration::HUB75_MATRIX_WIDTH;
    _matrixHeight = HardwareConfiguration::HUB75_MATRIX_HEIGHT;

    if (_matrix)
    {
        delete _matrix;
        _matrix = nullptr;
    }

    // Adafruit_Protomatter constructor wants non-const pin arrays.
    static uint8_t rgbPins[6] = {
        HardwareConfiguration::HUB75_RGB_PINS[0],
        HardwareConfiguration::HUB75_RGB_PINS[1],
        HardwareConfiguration::HUB75_RGB_PINS[2],
        HardwareConfiguration::HUB75_RGB_PINS[3],
        HardwareConfiguration::HUB75_RGB_PINS[4],
        HardwareConfiguration::HUB75_RGB_PINS[5],
    };

    static uint8_t addrPins[4] = {
        HardwareConfiguration::HUB75_ADDR_PINS_32[0],
        HardwareConfiguration::HUB75_ADDR_PINS_32[1],
        HardwareConfiguration::HUB75_ADDR_PINS_32[2],
        HardwareConfiguration::HUB75_ADDR_PINS_32[3],
    };

    _matrix = new Adafruit_Protomatter(
        _matrixWidth,
        HardwareConfiguration::HUB75_BIT_DEPTH,
        1, // number of chained panels (horizontal)
        rgbPins,
        4, // address pins A-D for 64x32 panels
        addrPins,
        HardwareConfiguration::HUB75_CLK_PIN,
        HardwareConfiguration::HUB75_LAT_PIN,
        HardwareConfiguration::HUB75_OE_PIN,
        HardwareConfiguration::HUB75_DOUBLE_BUFFER);

    if (!_matrix)
    {
        return false;
    }

    int rc = _matrix->begin();
    if (rc != PROTOMATTER_OK)
    {
        Serial.print("ProtomatterDisplay: begin() failed, code: ");
        Serial.println(rc);
        return false;
    }

    _matrix->setTextWrap(false);
    _matrix->setTextSize(1);

    // Adafruit_Protomatter doesn't implement setBrightness().
    // We apply UserConfiguration::DISPLAY_BRIGHTNESS by scaling colors when drawing.

    clear();
    _currentFlightIndex = 0;
    _lastCycleMs = millis();
    return true;
}

void ProtomatterDisplay::clear()
{
    if (_matrix)
    {
        _matrix->fillScreen(0);
        _matrix->show();
    }
}

void ProtomatterDisplay::drawTextLine(int16_t x, int16_t y, const String &text, uint16_t color)
{
    _matrix->setCursor(x, y);
    _matrix->setTextColor(color);
    for (size_t i = 0; i < (size_t)text.length(); ++i)
    {
        _matrix->write(text[i]);
    }
}

String ProtomatterDisplay::truncateToColumns(const String &text, int maxColumns)
{
    if ((int)text.length() <= maxColumns)
        return text;
    if (maxColumns <= 3)
        return text.substring(0, maxColumns);
    return text.substring(0, maxColumns - 3) + String("...");
}

void ProtomatterDisplay::displaySingleFlightCard(const FlightInfo &f)
{
    const uint16_t borderColor = colorWithBrightness(_matrix,
                                                     UserConfiguration::TEXT_COLOR_R,
                                                     UserConfiguration::TEXT_COLOR_G,
                                                     UserConfiguration::TEXT_COLOR_B);
    _matrix->drawRect(0, 0, _matrixWidth, _matrixHeight, borderColor);

    const int charWidth = 6;
    const int charHeight = 8;
    const int padding = 2;
    const int innerWidth = _matrixWidth - 2 - (2 * padding);
    const int innerHeight = _matrixHeight - 2 - (2 * padding);
    const int maxCols = innerWidth / charWidth;

    String airline = f.airline_display_name_full.length() ? f.airline_display_name_full
                                                          : (f.operator_iata.length() ? f.operator_iata : (f.operator_icao.length() ? f.operator_icao : f.operator_code));

    String origin = f.origin.code_icao;
    String dest = f.destination.code_icao;
    String line2 = origin + String(">") + dest;

    String line3 = f.aircraft_display_name_short.length() ? f.aircraft_display_name_short : f.aircraft_code;

    String line1 = truncateToColumns(airline, maxCols);
    line2 = truncateToColumns(line2, maxCols);
    line3 = truncateToColumns(line3, maxCols);

    const uint16_t textColor = colorWithBrightness(_matrix,
                                                   UserConfiguration::TEXT_COLOR_R,
                                                   UserConfiguration::TEXT_COLOR_G,
                                                   UserConfiguration::TEXT_COLOR_B);
    const int lineCount = 3;
    const int lineSpacing = 1;
    const int totalTextHeight = lineCount * charHeight + (lineCount - 1) * lineSpacing;
    const int topOffset = 1 + padding + (innerHeight - totalTextHeight) / 2;
    const int16_t startX = 1 + padding;

    int16_t y = topOffset;
    drawTextLine(startX, y, line1, textColor);
    y += charHeight + lineSpacing;
    drawTextLine(startX, y, line2, textColor);
    y += charHeight + lineSpacing;
    drawTextLine(startX, y, line3, textColor);
}

void ProtomatterDisplay::displayFlights(const std::vector<FlightInfo> &flights)
{
    if (_matrix == nullptr)
        return;

    _matrix->fillScreen(0);

    if (!flights.empty())
    {
        const unsigned long now = millis();
        const unsigned long intervalMs = TimingConfiguration::DISPLAY_CYCLE_SECONDS * 1000UL;

        if (flights.size() > 1)
        {
            if (now - _lastCycleMs >= intervalMs)
            {
                _lastCycleMs = now;
                _currentFlightIndex = (_currentFlightIndex + 1) % flights.size();
            }
        }
        else
        {
            _currentFlightIndex = 0;
        }

        const size_t index = _currentFlightIndex % flights.size();
        displaySingleFlightCard(flights[index]);
    }
    else
    {
        displayLoadingScreen();
    }

    _matrix->show();
}

void ProtomatterDisplay::displayLoadingScreen()
{
    if (_matrix == nullptr)
        return;

    _matrix->fillScreen(0);

    const uint16_t borderColor = colorWithBrightness(_matrix, 255, 255, 255);
    _matrix->drawRect(0, 0, _matrixWidth, _matrixHeight, borderColor);

    const int charWidth = 6;
    const int charHeight = 8;
    const String loadingText = "...";
    const int textWidth = loadingText.length() * charWidth;

    const int16_t x = (_matrixWidth - textWidth) / 2;
    const int16_t y = (_matrixHeight - charHeight) / 2 - 2;

    const uint16_t textColor = colorWithBrightness(_matrix,
                                                   UserConfiguration::TEXT_COLOR_R,
                                                   UserConfiguration::TEXT_COLOR_G,
                                                   UserConfiguration::TEXT_COLOR_B);
    drawTextLine(x, y, loadingText, textColor);

    _matrix->show();
}

void ProtomatterDisplay::displayMessage(const String &message)
{
    if (_matrix == nullptr)
        return;

    _matrix->fillScreen(0);

    const int charWidth = 6;
    const int charHeight = 6;

    const uint16_t textColor = colorWithBrightness(_matrix,
                                                   UserConfiguration::TEXT_COLOR_R,
                                                   UserConfiguration::TEXT_COLOR_G,
                                                   UserConfiguration::TEXT_COLOR_B);

    const int innerWidth = _matrixWidth;
    const int maxCols = innerWidth / charWidth;
    String line = truncateToColumns(message, maxCols);

    const int16_t x = 0;
    const int16_t y = (_matrixHeight - charHeight) / 2;
    drawTextLine(x, y, line, textColor);
    _matrix->show();
}

void ProtomatterDisplay::showLoading()
{
    displayLoadingScreen();
}

#endif // FLIGHTWALL_DISPLAY_PROTOMATTER

