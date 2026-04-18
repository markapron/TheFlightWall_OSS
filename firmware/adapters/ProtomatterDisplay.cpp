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
    return text.substring(0, maxColumns);
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

// ---------------------------------------------------------------------------
// Tail tracker display
// ---------------------------------------------------------------------------

// Format a duration in seconds as a compact string, e.g. "1h 23m" or "45m".
static String formatDuration(unsigned long seconds)
{
    if (seconds < 60) return String("0m");
    unsigned long mins  = seconds / 60;
    unsigned long hours = mins / 60;
    unsigned long rmins = mins % 60;
    if (hours == 0) return String(mins) + String("m");
    // For very long flights drop minutes to keep the string short (≤8 chars).
    if (hours >= 10) return String(hours) + String("h");
    return String(hours) + String("h ") + String(rmins) + String("m");
}

// Format elapsed time as seen on the tail tracker:
//   airborne → "1h 23m"   (status line already says En Route)
//   landed   → "1h 23m ago" or "10h ago"
static String formatElapsed(unsigned long seconds, bool landed)
{
    unsigned long mins  = seconds / 60;
    unsigned long hours = mins / 60;
    unsigned long rmins = mins % 60;

    if (landed)
    {
        if (hours == 0)   return String(mins) + String("m ago");      // "45m ago"
        if (hours < 10)   return String(hours) + String("h ")
                               + String(rmins) + String("m ago");     // "2h 5m ago"
        return String(hours) + String("h ago");                       // "12h ago"
    }
    return formatDuration(seconds);
}

void ProtomatterDisplay::displayTailTracker(const TailFlightStatus &status)
{
    if (_matrix == nullptr) return;

    _matrix->fillScreen(0);

    // Use the full panel width with no left border so we get 10 char columns
    // (64 px / 6 px per char = 10).  This gives a clean data-dense look that
    // is visually distinct from the bordered nearby-flights card.
    const int charWidth  = 6;
    const int maxCols    = _matrixWidth / charWidth; // 10
    const int16_t startX = 0;

    // Compute current epoch from the cached snapshot so we do not need to
    // query WiFi.getTime() on every display frame.
    unsigned long currentEpoch = status.fetch_epoch
                               + (millis() - status.fetch_millis) / 1000UL;

    // Hard-clip helper: cut at maxCols with no ellipsis so every character
    // up to the panel edge is shown rather than sacrificing 3 chars to "...".
    auto clip = [](const String &s, int cols) -> String {
        return ((int)s.length() <= cols) ? s : s.substring(0, cols);
    };

    // --- Line 1: status string (append destination code when landed) ---
    String statusLine = status.status.length() > 0 ? status.status : String("No Data");
    if (status.actual_on_epoch > 0 && status.dest_code.length() > 0)
        statusLine = statusLine + String(" ") + status.dest_code;
    statusLine = clip(statusLine, maxCols);

    // --- Line 2: elapsed time or ground state ---
    String timeLine;
    bool isLanded   = status.actual_on_epoch  > 0;
    bool isAirborne = status.actual_off_epoch > 0 && !isLanded;

    if (isLanded && currentEpoch >= status.actual_on_epoch)
    {
        timeLine = formatElapsed(currentEpoch - status.actual_on_epoch, true);
    }
    else if (isAirborne && currentEpoch >= status.actual_off_epoch)
    {
        timeLine = formatElapsed(currentEpoch - status.actual_off_epoch, false);
    }
    else
    {
        timeLine = String("Preparing");
    }
    timeLine = clip(timeLine, maxCols);

    // --- Line 3: city / region ---
    String locLine;
    if (status.city.length() > 0 && status.region.length() > 0)
        locLine = status.city + String(" ") + status.region;
    else if (status.city.length() > 0)
        locLine = status.city;
    else if (status.region.length() > 0)
        locLine = status.region;
    else
        locLine = String("---");
    locLine = clip(locLine, maxCols);

    // Text color matches user config; use amber for the time line to add contrast.
    const uint16_t textColor    = colorWithBrightness(_matrix,
                                                      UserConfiguration::TEXT_COLOR_R,
                                                      UserConfiguration::TEXT_COLOR_G,
                                                      UserConfiguration::TEXT_COLOR_B);
    const uint16_t timeColor    = colorWithBrightness(_matrix, 255, 200,  50);
    const uint16_t timeDimColor = colorWithBrightness(_matrix, 130, 100,  25); // dim "h"/"m"
    const uint16_t locColor     = colorWithBrightness(_matrix, 150, 220, 255);

    drawTextLine(startX,  1, statusLine, textColor);

    // Draw the time line character by character so the unit letters "h" and "m"
    // are rendered dimmer than the numeric digits.
    _matrix->setCursor(startX, 10);
    for (size_t i = 0; i < (size_t)timeLine.length(); ++i)
    {
        const char c = timeLine[i];
        _matrix->setTextColor((c >= '0' && c <= '9') ? timeColor : timeDimColor);
        _matrix->write(c);
    }

    drawTextLine(startX, 19, locLine,    locColor);

    // --- Progress bar (bottom 2 rows) ---
    // Background: very dim grey stripe across the full width.
    const uint16_t barBg = _matrix->color565(20, 20, 20);
    const uint16_t barFg = colorWithBrightness(_matrix, 0, 220, 0); // green
    _matrix->fillRect(0, 30, (int16_t)_matrixWidth, 2, barBg);

    int fillW = ((int)status.progress_percent * (int)_matrixWidth) / 100;
    if (fillW > (int)_matrixWidth) fillW = (int)_matrixWidth;
    if (fillW > 0)
        _matrix->fillRect(0, 30, (int16_t)fillW, 2, barFg);

    _matrix->show();
}

void ProtomatterDisplay::displayTailLoading()
{
    if (_matrix == nullptr) return;

    _matrix->fillScreen(0);

    const uint16_t color = colorWithBrightness(_matrix, 255, 200, 50); // amber
    const String   text  = "Tracking";
    const int      charWidth  = 6;
    const int      charHeight = 8;
    const int16_t  x = ((int16_t)_matrixWidth  - (int16_t)(text.length() * charWidth))  / 2;
    const int16_t  y = ((int16_t)_matrixHeight - charHeight) / 2 - 2;

    drawTextLine(x, y, text, color);
    _matrix->show();
}

#endif // FLIGHTWALL_DISPLAY_PROTOMATTER

