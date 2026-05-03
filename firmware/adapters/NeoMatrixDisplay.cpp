/*
Purpose: Render flight info on a WS2812B NeoPixel matrix via FastLED_NeoMatrix.
Responsibilities:
- Initialize LED matrix based on HardwareConfiguration and user display settings.
- Render a bordered, three-line flight “card” and a minimal loading screen.
- Cycle through multiple flights at a configurable interval.
Inputs: FlightInfo list; UserConfiguration (colors/brightness), TimingConfiguration (cycle),
        HardwareConfiguration (dimensions/pin/tiling).
Outputs: Visual output to LED matrix using FastLED.
*/
#include "adapters/NeoMatrixDisplay.h"

#include <Adafruit_GFX.h>
#include <FastLED_NeoMatrix.h>
#include <FastLED.h>
#include "config/UserConfiguration.h"
#include "config/HardwareConfiguration.h"
#include "config/TimingConfiguration.h"
#include "config/DisplayConfiguration.h"

NeoMatrixDisplay::NeoMatrixDisplay() {}

NeoMatrixDisplay::~NeoMatrixDisplay()
{
    if (_leds)
    {
        delete[] _leds;
        _leds = nullptr;
    }
    if (_matrix)
    {
        delete _matrix;
        _matrix = nullptr;
    }
}

bool NeoMatrixDisplay::initialize()
{
    _matrixWidth = HardwareConfiguration::NEOMATRIX_MATRIX_WIDTH;
    _matrixHeight = HardwareConfiguration::NEOMATRIX_MATRIX_HEIGHT;
    _numPixels = (uint32_t)_matrixWidth * (uint32_t)_matrixHeight;

    _leds = new CRGB[_numPixels];

    _matrix = new FastLED_NeoMatrix(
        _leds,
        HardwareConfiguration::NEOMATRIX_TILE_PIXEL_W,
        HardwareConfiguration::NEOMATRIX_TILE_PIXEL_H,
        HardwareConfiguration::NEOMATRIX_TILES_X,
        HardwareConfiguration::NEOMATRIX_TILES_Y,
        NEO_MATRIX_BOTTOM + NEO_MATRIX_RIGHT +
            NEO_MATRIX_COLUMNS + NEO_MATRIX_ZIGZAG +
            NEO_TILE_TOP + NEO_TILE_RIGHT + NEO_TILE_COLUMNS + NEO_TILE_ZIGZAG);

    FastLED.addLeds<WS2812B, HardwareConfiguration::NEOMATRIX_DATA_PIN, GRB>(_leds, _numPixels);
    _matrix->setTextWrap(false);
    _matrix->setTextSize(1);
    _matrix->setBrightness(UserConfiguration::DISPLAY_BRIGHTNESS);
    clear();
    _currentFlightIndex = 0;
    _lastCycleMs = millis();
    return true;
}

void NeoMatrixDisplay::clear()
{
    if (_matrix)
    {
        _matrix->fillScreen(0);
        FastLED.show();
    }
}

String NeoMatrixDisplay::makeFlightLine(const FlightInfo &f)
{
    String airline = f.airline_display_name_full.length() ? f.airline_display_name_full
                                                          : (f.operator_iata.length() ? f.operator_iata : f.operator_icao);
    if (airline.length() == 0)
    {
        airline = f.operator_code;
    }
    String origin = f.origin.code_iata.length() ? f.origin.code_iata : f.origin.code_icao;
    String dest   = f.destination.code_iata.length() ? f.destination.code_iata : f.destination.code_icao;
    String route = origin + "-" + dest;
    String type = f.aircraft_display_name_short.length() ? f.aircraft_display_name_short : f.aircraft_code;
    String ident = f.ident.length() ? f.ident : f.ident_icao;
    String line = airline;
    if (ident.length())
    {
        line += " ";
        line += ident;
    }
    if (type.length())
    {
        line += " ";
        line += type;
    }
    if (route.length() > 1)
    {
        line += " ";
        line += route;
    }
    return line;
}

void NeoMatrixDisplay::drawTextLine(int16_t x, int16_t y, const String &text, uint16_t color)
{
    _matrix->setCursor(x, y);
    _matrix->setTextColor(color);
    for (size_t i = 0; i < (size_t)text.length(); ++i)
    {
        _matrix->write(text[i]);
    }
}

String NeoMatrixDisplay::truncateToColumns(const String &text, int maxColumns)
{
    if ((int)text.length() <= maxColumns)
        return text;
    if (maxColumns <= 3)
        return text.substring(0, maxColumns);
    return text.substring(0, maxColumns - 3) + String("...");
}

void NeoMatrixDisplay::displaySingleFlightCard(const FlightInfo &f)
{
    // Border
    const uint16_t borderColor = _matrix->Color(DisplayConfiguration::NEARBY_BORDER_R,
                                                DisplayConfiguration::NEARBY_BORDER_G,
                                                DisplayConfiguration::NEARBY_BORDER_B);
    _matrix->drawRect(0, 0, _matrixWidth, _matrixHeight, borderColor);

    const int charWidth = 6;
    const int charHeight = 8;
    const int padding = 2;
    const int innerWidth = _matrixWidth - 2 - (2 * padding);
    const int innerHeight = _matrixHeight - 2 - (2 * padding);
    const int maxCols = innerWidth / charWidth;

    // Line 1: airline / operator name
    String airline = f.airline_display_name_full.length() ? f.airline_display_name_full
                                                          : (f.operator_iata.length() ? f.operator_iata : (f.operator_icao.length() ? f.operator_icao : f.operator_code));
    String line1 = truncateToColumns(airline, maxCols);

    // Line 2: route — ICAO with first char dim, remaining chars bright.
    // Falls back to all-bright IATA when no ICAO is available.
    const String &origIcao = f.origin.code_icao;
    const String &origIata = f.origin.code_iata;
    const String &dstIcao  = f.destination.code_icao;
    const String &dstIata  = f.destination.code_iata;

    // Line 3: aircraft type
    String line3 = f.aircraft_display_name_short.length() ? f.aircraft_display_name_short : f.aircraft_code;
    line3 = truncateToColumns(line3, maxCols);

    // Colors
    const uint16_t line1Color = _matrix->Color(DisplayConfiguration::NEARBY_LINE1_R,
                                               DisplayConfiguration::NEARBY_LINE1_G,
                                               DisplayConfiguration::NEARBY_LINE1_B);
    const uint16_t iataColor  = _matrix->Color(DisplayConfiguration::NEARBY_LINE2_IATA_R,
                                               DisplayConfiguration::NEARBY_LINE2_IATA_G,
                                               DisplayConfiguration::NEARBY_LINE2_IATA_B);
    const uint16_t icaoColor  = _matrix->Color(DisplayConfiguration::NEARBY_LINE2_ICAO_R,
                                               DisplayConfiguration::NEARBY_LINE2_ICAO_G,
                                               DisplayConfiguration::NEARBY_LINE2_ICAO_B);
    const uint16_t sepColor   = _matrix->Color(DisplayConfiguration::NEARBY_LINE2_SEP_R,
                                               DisplayConfiguration::NEARBY_LINE2_SEP_G,
                                               DisplayConfiguration::NEARBY_LINE2_SEP_B);
    const uint16_t line3Color = _matrix->Color(DisplayConfiguration::NEARBY_LINE3_R,
                                               DisplayConfiguration::NEARBY_LINE3_G,
                                               DisplayConfiguration::NEARBY_LINE3_B);

    const int lineCount = 3;
    const int lineSpacing = 1;
    const int totalTextHeight = lineCount * charHeight + (lineCount - 1) * lineSpacing;
    const int topOffset = 1 + padding + (innerHeight - totalTextHeight) / 2;
    const int16_t startX = 1 + padding;

    int16_t y = topOffset;

    // Line 1
    drawTextLine(startX, y, line1, line1Color);
    y += charHeight + lineSpacing;

    // Draw one airport code: ICAO with first char dim, rest bright.
    // Falls back to all-bright IATA when ICAO is absent.
    auto drawAirport = [&](const String &icao, const String &iata, int &colsUsed)
    {
        const String &code = icao.length() ? icao : iata;
        if (code.length() == 0)
            return;

        const bool useIcao = icao.length() > 0;
        for (size_t i = 0; i < code.length() && colsUsed < maxCols; ++i, ++colsUsed)
        {
            _matrix->setTextColor((useIcao && i == 0) ? icaoColor : iataColor);
            _matrix->write(code[i]);
        }
    };

    {
        _matrix->setCursor(startX, y);
        int colsUsed = 0;

        drawAirport(origIcao, origIata, colsUsed);

        if (colsUsed < maxCols)
        {
            _matrix->setTextColor(sepColor);
            _matrix->write('>');
            ++colsUsed;
        }

        drawAirport(dstIcao, dstIata, colsUsed);
    }
    y += charHeight + lineSpacing;

    // Line 3
    drawTextLine(startX, y, line3, line3Color);

    // Route progress (AeroAPI); same colors as tail-tracker bar.
    const uint16_t barBg = _matrix->Color(DisplayConfiguration::TAIL_BAR_BG_R,
                                          DisplayConfiguration::TAIL_BAR_BG_G,
                                          DisplayConfiguration::TAIL_BAR_BG_B);
    const uint16_t barFg = _matrix->Color(DisplayConfiguration::TAIL_BAR_FG_R,
                                          DisplayConfiguration::TAIL_BAR_FG_G,
                                          DisplayConfiguration::TAIL_BAR_FG_B);
    _matrix->fillRect(0, 30, _matrixWidth, 2, barBg);
    int prog = f.progress_percent;
    if (prog < 0) prog = 0;
    if (prog > 100) prog = 100;
    int fillW = (prog * (int)_matrixWidth) / 100;
    if (fillW > (int)_matrixWidth) fillW = (int)_matrixWidth;
    if (fillW > 0)
        _matrix->fillRect(0, 30, fillW, 2, barFg);
}

void NeoMatrixDisplay::displayFlights(const std::vector<FlightInfo> &flights)
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

    FastLED.show();
}

void NeoMatrixDisplay::displayLoadingScreen()
{
    if (_matrix == nullptr)
        return;

    _matrix->fillScreen(0);

    const uint16_t borderColor = _matrix->Color(DisplayConfiguration::NEARBY_BORDER_R,
                                                DisplayConfiguration::NEARBY_BORDER_G,
                                                DisplayConfiguration::NEARBY_BORDER_B);
    _matrix->drawRect(0, 0, _matrixWidth, _matrixHeight, borderColor);

    const int charWidth = 6;
    const int charHeight = 8;
    const String loadingText = "...";
    const int textWidth = loadingText.length() * charWidth;

    const int16_t x = (_matrixWidth - textWidth) / 2;
    const int16_t y = (_matrixHeight - charHeight) / 2 - 2;

    const uint16_t textColor = _matrix->Color(DisplayConfiguration::NEARBY_BORDER_R,
                                              DisplayConfiguration::NEARBY_BORDER_G,
                                              DisplayConfiguration::NEARBY_BORDER_B);
    drawTextLine(x, y, loadingText, textColor);

    FastLED.show();
}

void NeoMatrixDisplay::displayMessage(const String &message)
{
    if (_matrix == nullptr)
        return;

    _matrix->fillScreen(0);

    const int charWidth = 6;
    const int charHeight = 6;

    const uint16_t textColor = _matrix->Color(DisplayConfiguration::NEARBY_BORDER_R,
                                              DisplayConfiguration::NEARBY_BORDER_G,
                                              DisplayConfiguration::NEARBY_BORDER_B);

    // Simple single-line message; truncate if needed
    const int innerWidth = _matrixWidth;
    const int maxCols = innerWidth / charWidth;
    String line = truncateToColumns(message, maxCols);

    const int16_t x = 0;
    const int16_t y = (_matrixHeight - charHeight) / 2;
    drawTextLine(x, y, line, textColor);
    FastLED.show();
}

void NeoMatrixDisplay::showLoading()
{
    displayLoadingScreen();
}
