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
#include "config/DisplayConfiguration.h"
#include "utils/GeoUtils.h"

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
                                                     DisplayConfiguration::NEARBY_BORDER_R,
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
    const uint16_t line1Color = colorWithBrightness(_matrix,
                                                    DisplayConfiguration::NEARBY_LINE1_R,
                                                    DisplayConfiguration::NEARBY_LINE1_G,
                                                    DisplayConfiguration::NEARBY_LINE1_B);
    const uint16_t iataColor  = colorWithBrightness(_matrix,
                                                    DisplayConfiguration::NEARBY_LINE2_IATA_R,
                                                    DisplayConfiguration::NEARBY_LINE2_IATA_G,
                                                    DisplayConfiguration::NEARBY_LINE2_IATA_B);
    const uint16_t icaoColor  = colorWithBrightness(_matrix,
                                                    DisplayConfiguration::NEARBY_LINE2_ICAO_R,
                                                    DisplayConfiguration::NEARBY_LINE2_ICAO_G,
                                                    DisplayConfiguration::NEARBY_LINE2_ICAO_B);
    const uint16_t sepColor   = colorWithBrightness(_matrix,
                                                    DisplayConfiguration::NEARBY_LINE2_SEP_R,
                                                    DisplayConfiguration::NEARBY_LINE2_SEP_G,
                                                    DisplayConfiguration::NEARBY_LINE2_SEP_B);
    const uint16_t line3Color = colorWithBrightness(_matrix,
                                                    DisplayConfiguration::NEARBY_LINE3_R,
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

    // Line 2 — always show ICAO; IATA-matching chars are bright, prefix chars dim.
    // Helper: draw one airport code, honouring the column limit.
    // iataStart/iataEnd mark which ICAO chars overlap the IATA code (-1 = no match).
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
            // First char of an ICAO code is the regional prefix — render it dim.
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

    const uint16_t borderColor = colorWithBrightness(_matrix,
                                                     DisplayConfiguration::NEARBY_BORDER_R,
                                                     DisplayConfiguration::NEARBY_BORDER_G,
                                                     DisplayConfiguration::NEARBY_BORDER_B);
    _matrix->drawRect(0, 0, _matrixWidth, _matrixHeight, borderColor);

    const int charWidth = 6;
    const int charHeight = 8;
    const String loadingText = "...";
    const int textWidth = loadingText.length() * charWidth;

    const int16_t x = (_matrixWidth - textWidth) / 2;
    const int16_t y = (_matrixHeight - charHeight) / 2 - 2;

    const uint16_t textColor = colorWithBrightness(_matrix,
                                                   DisplayConfiguration::NEARBY_BORDER_R,
                                                   DisplayConfiguration::NEARBY_BORDER_G,
                                                   DisplayConfiguration::NEARBY_BORDER_B);
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
                                                   DisplayConfiguration::NEARBY_BORDER_R,
                                                   DisplayConfiguration::NEARBY_BORDER_G,
                                                   DisplayConfiguration::NEARBY_BORDER_B);

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

    const int     charWidth  = 6;
    const int16_t startX     = 0;

    // Reserve 28 px on the right for the compass widget (center x = matrixWidth-14).
    // This leaves 100 px for text → 16 character columns at 6 px each.
    const int16_t compassAreaX = (int16_t)_matrixWidth - 28;
    const int     maxCols      = (int)compassAreaX / charWidth; // 16

    // Compute current epoch from the cached snapshot so we do not need to
    // query WiFi.getTime() on every display frame.
    unsigned long currentEpoch = status.fetch_epoch
                               + (millis() - status.fetch_millis) / 1000UL;

    // Hard-clip helper: cut at maxCols with no ellipsis.
    auto clip = [](const String &s, int cols) -> String {
        return ((int)s.length() <= cols) ? s : s.substring(0, cols);
    };

    // --- Bearing / distance to the tracked aircraft ---
    // status.lat/lon are NAN when the API did not return a position.
    const bool hasPosition = !isnan(status.lat) && !isnan(status.lon);
    double bearingDeg  = 0.0;
    float  distanceMi  = 0.0f;
    if (hasPosition)
    {
        const double distKm = haversineKm(
            UserConfiguration::CENTER_LAT, UserConfiguration::CENTER_LON,
            status.lat,                    status.lon);
        distanceMi = (float)(distKm * 0.621371);
        bearingDeg = computeBearingDeg(
            UserConfiguration::CENTER_LAT, UserConfiguration::CENTER_LON,
            status.lat,                    status.lon);
    }

    // Computed early so both line 1 and line 2 can use them.
    bool isLanded   = status.actual_on_epoch  > 0;
    bool isAirborne = status.actual_off_epoch > 0 && !isLanded;

    // Compass bearing fallback: when no live position is available, point toward
    // the same location shown on line 3 — origin airport when pre-departure,
    // destination airport when landed.
    if (!hasPosition)
    {
        const double fbLat = isLanded ? status.dest_lat   : status.origin_lat;
        const double fbLon = isLanded ? status.dest_lon   : status.origin_lon;
        if (!isnan(fbLat) && !isnan(fbLon))
            bearingDeg = computeBearingDeg(
                UserConfiguration::CENTER_LAT, UserConfiguration::CENTER_LON,
                fbLat, fbLon);
    }

    // --- Line 1: status + destination ---
    // Use += with char literals instead of temporary String("x") objects to
    // avoid repeated small heap allocations on every display tick.
    String statusLine;
    if (isAirborne && status.dest_code.length() > 0)
    {
        statusLine = "Flying to ";
        statusLine += status.dest_code;
    }
    else
    {
        statusLine = status.status.length() > 0 ? status.status : "No Data";
        if (status.dest_code.length() > 0)
        {
            statusLine += ' ';
            statusLine += status.dest_code;
        }
    }
    statusLine = clip(statusLine, maxCols);

    // --- Line 2: compact elapsed time + altitude + distance (airborne);
    //             elapsed time + distance (landed / on-ground fallback). ---
    // snprintf into stack-local char buffers avoids heap allocation for the
    // numeric parts, which is important because this function runs on every
    // display tick() during a blocking HTTP fetch.
    String timeLine;
    if (isAirborne && status.altitude_ft > 0)
    {
        // Compact elapsed time: "1h23m", "45m", "10h" (no space between h and m).
        if (currentEpoch >= status.actual_off_epoch)
        {
            const unsigned long secs  = currentEpoch - status.actual_off_epoch;
            const unsigned long tmins = secs / 60;
            const unsigned long th    = tmins / 60;
            const unsigned long tm    = tmins % 60;
            // Cap to realistic display values before snprintf so the compiler
            // can verify the output fits the buffer (avoids -Wformat-truncation).
            const int hh = (int)(th    < 999  ? th    : 999);
            const int mm = (int)(tm    < 59   ? tm    : 59);
            const int mT = (int)(tmins < 9999 ? tmins : 9999);
            char tBuf[16];
            if      (th == 0)  snprintf(tBuf, sizeof(tBuf), "%dm",    mT);
            else if (th >= 10) snprintf(tBuf, sizeof(tBuf), "%dh",    hh);
            else               snprintf(tBuf, sizeof(tBuf), "%dh%dm", hh, mm);
            timeLine = tBuf;
        }

        // Altitude: "35.2k"
        char altBuf[10];
        snprintf(altBuf, sizeof(altBuf), "%.1fk", status.altitude_ft / 1000.0f);
        if (timeLine.length() > 0) timeLine += ' ';
        timeLine += altBuf;

        // Distance: " 142mi"
        if (hasPosition)
        {
            char distBuf[10];
            snprintf(distBuf, sizeof(distBuf), " %dmi", (int)roundf(distanceMi));
            timeLine += distBuf;
        }
    }
    else
    {
        if (isLanded && currentEpoch >= status.actual_on_epoch)
            timeLine = formatElapsed(currentEpoch - status.actual_on_epoch, true);
        else if (isAirborne && currentEpoch >= status.actual_off_epoch)
            timeLine = formatElapsed(currentEpoch - status.actual_off_epoch, false);
        else
        {
            // Pre-departure: show T-minus countdown to scheduled departure
            // when the time is known and still in the future.
            if (status.scheduled_off_epoch > 0
                    && status.actual_off_epoch == 0
                    && status.scheduled_off_epoch > currentEpoch)
            {
                const unsigned long secsToGo = status.scheduled_off_epoch - currentEpoch;
                const unsigned long minsRaw  = secsToGo / 60;
                const int minsToGo = (int)(minsRaw  < 9999 ? minsRaw  : 9999);
                const int hrsToGo  = (int)(minsRaw / 60 < 999 ? minsRaw / 60 : 999);
                char tBuf[16];
                if (hrsToGo >= 1)
                    snprintf(tBuf, sizeof(tBuf), "T-%dh%dm", hrsToGo, minsToGo % 60);
                else
                    snprintf(tBuf, sizeof(tBuf), "T-%dm", minsToGo);
                timeLine = String("On Ground ") + tBuf;
            }
            else
                timeLine = "On Ground";
        }

        if (hasPosition)
        {
            char distBuf[10];
            snprintf(distBuf, sizeof(distBuf), " %dmi", (int)roundf(distanceMi));
            timeLine += distBuf;
        }
    }
    timeLine = clip(timeLine, maxCols);

    // --- Line 3: city / region ---
    String locLine;
    if (status.city.length() > 0 && status.region.length() > 0)
    {
        locLine  = status.city;
        locLine += ' ';
        locLine += status.region;
    }
    else if (status.city.length() > 0)
        locLine = status.city;
    else if (status.region.length() > 0)
        locLine = status.region;
    else
        locLine = "---";
    locLine.replace("County", "Cnty");
    locLine = clip(locLine, maxCols);

    // --- Colors ---
    const uint16_t textColor    = colorWithBrightness(_matrix,
                                                      DisplayConfiguration::TAIL_STATUS_R,
                                                      DisplayConfiguration::TAIL_STATUS_G,
                                                      DisplayConfiguration::TAIL_STATUS_B);
    const uint16_t timeColor    = colorWithBrightness(_matrix,
                                                      DisplayConfiguration::TAIL_TIME_R,
                                                      DisplayConfiguration::TAIL_TIME_G,
                                                      DisplayConfiguration::TAIL_TIME_B);
    const uint16_t timeDimColor = colorWithBrightness(_matrix,
                                                      DisplayConfiguration::TAIL_TIME_DIM_R,
                                                      DisplayConfiguration::TAIL_TIME_DIM_G,
                                                      DisplayConfiguration::TAIL_TIME_DIM_B);
    const uint16_t locColor     = colorWithBrightness(_matrix,
                                                      DisplayConfiguration::TAIL_LOC_R,
                                                      DisplayConfiguration::TAIL_LOC_G,
                                                      DisplayConfiguration::TAIL_LOC_B);

    // --- Draw text lines ---

    // Line 1: status
    drawTextLine(startX, 1, statusLine, textColor);

    // Line 2: time (and distance when available) — digits bright, letters/spaces dim.
    // This single rendering pass works for both "1h 23m" and "1h 23m 142mi".
    _matrix->setCursor(startX, 10);
    for (size_t i = 0; i < (size_t)timeLine.length(); ++i)
    {
        const char c = timeLine[i];
        _matrix->setTextColor((c >= '0' && c <= '9') ? timeColor : timeDimColor);
        _matrix->write(c);
    }

    // Line 3: location
    drawTextLine(startX, 19, locLine, locColor);

    // --- Progress bar (bottom 2 rows, full panel width) ---
    const uint16_t barBg = _matrix->color565(DisplayConfiguration::TAIL_BAR_BG_R,
                                              DisplayConfiguration::TAIL_BAR_BG_G,
                                              DisplayConfiguration::TAIL_BAR_BG_B);
    const uint16_t barFg = colorWithBrightness(_matrix,
                                               DisplayConfiguration::TAIL_BAR_FG_R,
                                               DisplayConfiguration::TAIL_BAR_FG_G,
                                               DisplayConfiguration::TAIL_BAR_FG_B);
    _matrix->fillRect(0, 30, (int16_t)_matrixWidth, 2, barBg);

    int fillW = ((int)status.progress_percent * (int)_matrixWidth) / 100;
    if (fillW > (int)_matrixWidth) fillW = (int)_matrixWidth;
    if (fillW > 0)
        _matrix->fillRect(0, 30, (int16_t)fillW, 2, barFg);

    // --- Compass widget (right side, always shown in tracking mode) ---
    // bearingDeg is 0.0 (north) when no position is available, which serves
    // as a neutral default while the aircraft is on the ground or untracked.
    {
        const int16_t cx     = compassAreaX + 14; // horizontal center of compass area
        const int16_t cy     = (int16_t)(_matrixHeight / 2);
        const int16_t radius = 11;
        drawCompass(cx, cy, radius, bearingDeg);
    }

    _matrix->show();
}

// Draw a north-up compass rose.  North is at the 12 o'clock position; the
// needle points in bearingDeg (0 = N, 90 = E, 180 = S, 270 = W).
void ProtomatterDisplay::drawCompass(int16_t cx, int16_t cy, int16_t radius, double bearingDeg)
{
    const uint16_t ringColor   = colorWithBrightness(_matrix,
                                                     DisplayConfiguration::TAIL_COMPASS_RING_R,
                                                     DisplayConfiguration::TAIL_COMPASS_RING_G,
                                                     DisplayConfiguration::TAIL_COMPASS_RING_B);
    const uint16_t northColor  = colorWithBrightness(_matrix,
                                                     DisplayConfiguration::TAIL_COMPASS_NORTH_R,
                                                     DisplayConfiguration::TAIL_COMPASS_NORTH_G,
                                                     DisplayConfiguration::TAIL_COMPASS_NORTH_B);
    const uint16_t needleColor = colorWithBrightness(_matrix,
                                                     DisplayConfiguration::TAIL_COMPASS_NEEDLE_R,
                                                     DisplayConfiguration::TAIL_COMPASS_NEEDLE_G,
                                                     DisplayConfiguration::TAIL_COMPASS_NEEDLE_B);

    // Outer ring
    _matrix->drawCircle(cx, cy, radius, ringColor);

    // Cardinal direction labels — custom bitmaps inside the compass ring.
    // N and W use 5-wide bitmaps (bits 4..0); S and E use 3-wide (bits 2..0).
    // Each glyph is 5 rows tall; MSB of each row byte is the left-most pixel.
    static const uint8_t glyphN[5] = {0b10001, 0b11001, 0b10101, 0b10011, 0b10001};
    static const uint8_t glyphS[5] = {0b111,   0b100,   0b111,   0b001,   0b111  };
    static const uint8_t glyphE[5] = {0b111,   0b100,   0b110,   0b100,   0b111  };
    static const uint8_t glyphW[5] = {0b10001, 0b10001, 0b10101, 0b10101, 0b01010};

    // w = number of pixel columns; the top bit of each row byte is the left pixel.
    auto drawGlyph = [&](int16_t x0, int16_t y0, const uint8_t *g, int w) {
        const uint8_t msb = static_cast<uint8_t>(1 << (w - 1));
        for (int r = 0; r < 5; ++r)
            for (int c = 0; c < w; ++c)
                if (g[r] & (msb >> c))
                    _matrix->drawPixel(x0 + c, y0 + r, northColor);
    };

    drawGlyph(cx - 2,          cy - radius + 2, glyphN, 5); // N — top    (5-wide, centred)
    drawGlyph(cx - 1,          cy + radius - 6, glyphS, 3); // S — bottom (3-wide)
    drawGlyph(cx + radius - 4, cy - 2,          glyphE, 3); // E — right  (3-wide)
    drawGlyph(cx - radius + 2, cy - 2,          glyphW, 5); // W — left   (5-wide)

    // Bearing needle: line from center to a point near the ring edge.
    const double   rad      = degreesToRadians(bearingDeg);
    const int16_t  needleR  = radius - 2; // slightly inset from the ring
    const int16_t  ex       = cx + (int16_t)round(sin(rad) * needleR);
    const int16_t  ey       = cy - (int16_t)round(cos(rad) * needleR);
    _matrix->drawLine(cx, cy, ex, ey, needleColor);

    // Bright pixel at the needle tip to emphasise the direction.
    _matrix->drawPixel(ex, ey, needleColor);
}

void ProtomatterDisplay::displayTailLoading()
{
    if (_matrix == nullptr) return;

    _matrix->fillScreen(0);

    const uint16_t color = colorWithBrightness(_matrix,
                                               DisplayConfiguration::TAIL_LOADING_R,
                                               DisplayConfiguration::TAIL_LOADING_G,
                                               DisplayConfiguration::TAIL_LOADING_B);
    const String   text  = "Tracking";
    const int      charWidth  = 6;
    const int      charHeight = 8;
    const int16_t  x = ((int16_t)_matrixWidth  - (int16_t)(text.length() * charWidth))  / 2;
    const int16_t  y = ((int16_t)_matrixHeight - charHeight) / 2 - 2;

    drawTextLine(x, y, text, color);
    _matrix->show();
}

#endif // FLIGHTWALL_DISPLAY_PROTOMATTER

