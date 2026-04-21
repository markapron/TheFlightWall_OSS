/*
Purpose: Firmware entry point for the FlightWall.
Responsibilities:
- Initialize serial, connect to Wi‑Fi, and construct fetchers and display.
- Handle UP/DOWN button presses to switch between operating modes.
- MODE_NEARBY: periodically fetch nearby state vectors and enrich with AeroAPI;
  cycle through the resulting flight cards on the display.
- MODE_TAIL_TRACKER: periodically fetch status (progress, time, position) for a
  configured tail number and show a dedicated tracker screen with a progress bar
  and reverse-geocoded city/state.
Configuration: UserConfiguration, TailTrackerConfiguration, TimingConfiguration,
               WiFiConfiguration, HardwareConfiguration.
*/
#include <vector>
#if defined(ARDUINO_ARCH_ESP32)
  #include <WiFi.h>
#else
  #include <WiFiNINA.h>
#endif
#include "config/UserConfiguration.h"
#include "config/WiFiConfiguration.h"
#include "config/TimingConfiguration.h"
#include "config/HardwareConfiguration.h"
#include "config/TailTrackerConfiguration.h"
#include "SerialConfig.h"
#include "adapters/OpenSkyFetcher.h"
#include "adapters/AeroAPIFetcher.h"
#include "adapters/TailTrackerFetcher.h"
#include "core/FlightDataFetcher.h"
#include "models/TailFlightStatus.h"
#include "utils/HttpUtils.h"
#if defined(FLIGHTWALL_DISPLAY_NEOMATRIX)
#include "adapters/NeoMatrixDisplay.h"
using ActiveDisplay = NeoMatrixDisplay;
#else
// Default to HUB75/Protomatter (Matrix Portal M4)
#include "adapters/ProtomatterDisplay.h"
using ActiveDisplay = ProtomatterDisplay;
#endif

// ---------------------------------------------------------------------------
// Operating modes
// ---------------------------------------------------------------------------

enum AppMode
{
    MODE_NEARBY       = 0, // nearby-flights display
    MODE_TAIL_TRACKER = 1, // single-aircraft tracker
    MODE_SLEEP        = 2, // display off, no fetching (DOWN once)
    MODE_SERIAL_CONFIG = 3, // serial config menu active, no fetching (DOWN x3)
    MODE_COUNT        = 4,
};

// Number of "normal" (cycling) modes — SLEEP and SERIAL_CONFIG are not
// reachable via the UP-button cycle.
static const int kCycleModeCount = 2;

static AppMode g_appMode      = MODE_NEARBY;
static AppMode g_prevActiveMode = MODE_NEARBY; // mode to restore after sleep/config

// ---------------------------------------------------------------------------
// Global objects
// ---------------------------------------------------------------------------

static OpenSkyFetcher    g_openSky;
static AeroAPIFetcher    g_aeroApi;
static TailTrackerFetcher g_tailFetcher;
static FlightDataFetcher *g_fetcher = nullptr;
static ActiveDisplay     g_display;

// MODE_NEARBY state
static std::vector<FlightInfo> g_cachedFlights;
static unsigned long           g_lastFetchMs = 0;

// MODE_TAIL_TRACKER state
static TailFlightStatus  g_tailStatus;
static unsigned long     g_lastTailFetchMs = 0;

// ---------------------------------------------------------------------------
// Button handling — forward declaration so displayTick can call it
// ---------------------------------------------------------------------------
static void checkButtons();

// ---------------------------------------------------------------------------
// Display tick (called during blocking HTTP/TLS waits)
// ---------------------------------------------------------------------------

static void displayTick()
{
    // Process button presses mid-fetch so a mode switch is never blocked by
    // a long-running HTTPS request.
    checkButtons();

    // Don't touch the display in sleep or config modes.
    if (g_appMode == MODE_SLEEP || g_appMode == MODE_SERIAL_CONFIG)
        return;

    if (g_appMode == MODE_TAIL_TRACKER)
    {
        if (g_tailStatus.valid)
            g_display.displayTailTracker(g_tailStatus);
        else
            g_display.displayTailLoading();
    }
    else
    {
        g_display.displayFlights(g_cachedFlights);
    }
}

// ---------------------------------------------------------------------------
// Button handling
// ---------------------------------------------------------------------------

// ISR flags — set by hardware interrupts, read and cleared in checkButtons().
static volatile bool g_buttonUpPressed   = false;
static volatile bool g_buttonDownPressed = false;

static void buttonUpISR()   { g_buttonUpPressed   = true; }
static void buttonDownISR() { g_buttonDownPressed = true; }

// Per-button debounce timestamps.
static unsigned long g_lastUpMs   = 0;
static unsigned long g_lastDownMs = 0;
static const unsigned long kButtonDebounceMs = 150;

// Multi-tap state for the DOWN button.
// Presses are accumulated for up to kDownTapWindowMs after the first press.
// 3+ presses → serial config (triggered immediately on the 3rd press).
// 1 press + timeout → sleep.
static int           g_downTapCount      = 0;
static unsigned long g_downFirstTapMs    = 0;
static const unsigned long kDownTapWindowMs = 700;

static void checkButtons()
{
    const unsigned long now = millis();

    // ---- UP button: cycle between normal display modes ---------------------
    if (g_buttonUpPressed && now - g_lastUpMs >= kButtonDebounceMs)
    {
        g_buttonUpPressed = false;
        g_lastUpMs = now;

        if (g_appMode == MODE_SLEEP || g_appMode == MODE_SERIAL_CONFIG)
        {
            // UP always exits sleep / config and returns to the previous mode.
            g_appMode = g_prevActiveMode;
            g_lastFetchMs     = 0;
            g_lastTailFetchMs = 0;
            Serial.print(F("UP: restored mode → "));
            Serial.println(g_appMode == MODE_TAIL_TRACKER ? "TAIL_TRACKER" : "NEARBY");
        }
        else
        {
            AppMode prev = g_appMode;
            g_appMode = static_cast<AppMode>(((int)g_appMode + 1) % kCycleModeCount);
            if (g_appMode != prev)
            {
                g_prevActiveMode = g_appMode;
                Serial.print(F("Mode → "));
                Serial.println(g_appMode == MODE_TAIL_TRACKER ? "TAIL_TRACKER" : "NEARBY");
                if (g_appMode == MODE_TAIL_TRACKER)
                {
                    if (!g_tailStatus.valid) g_lastTailFetchMs = 0;
                }
                else
                {
                    g_lastFetchMs = 0;
                }
            }
        }
    }

    // ---- DOWN button: multi-tap detection ----------------------------------
    // While sleeping any press wakes immediately. Otherwise presses are
    // accumulated: 3 within kDownTapWindowMs → serial config (fires on 3rd
    // press); 1 press + timeout → sleep. Double-press is a no-op.

    if (g_buttonDownPressed && now - g_lastDownMs >= kButtonDebounceMs)
    {
        g_buttonDownPressed = false;
        g_lastDownMs = now;

        if (g_appMode == MODE_SLEEP)
        {
            // Any press exits sleep immediately; reset tap counter.
            g_downTapCount   = 0;
            g_downFirstTapMs = 0;
            g_appMode        = g_prevActiveMode;
            g_lastFetchMs     = 0;
            g_lastTailFetchMs = 0;
            Serial.println(F("DOWN: waking up."));
        }
        else if (g_appMode != MODE_SERIAL_CONFIG)
        {
            if (g_downTapCount == 0)
                g_downFirstTapMs = now;
            g_downTapCount++;
            Serial.print(F("DOWN tap "));
            Serial.println(g_downTapCount);

            if (g_downTapCount >= 3)
            {
                // Triple press: enter serial config immediately.
                g_downTapCount   = 0;
                g_downFirstTapMs = 0;
                g_prevActiveMode = g_appMode;
                g_appMode = MODE_SERIAL_CONFIG;
                g_display.displayMessage(String("FlightWall Config"));
                Serial.println(F("--- Serial config mode (DOWN x3) ---"));
                SerialConfig::openMenu();
            }
            // else: keep counting; single/double-press action deferred to timeout below.
        }
    }

    // Tap-window timeout: commit deferred single-press action.
    if (g_downTapCount > 0 && g_downFirstTapMs != 0 &&
        now - g_downFirstTapMs >= kDownTapWindowMs)
    {
        if (g_downTapCount == 1 &&
            g_appMode != MODE_SLEEP && g_appMode != MODE_SERIAL_CONFIG)
        {
            g_prevActiveMode = g_appMode;
            g_appMode = MODE_SLEEP;
            g_display.clear();
            Serial.println(F("DOWN: sleep mode."));
        }
        g_downTapCount   = 0;
        g_downFirstTapMs = 0;
    }
}

// ---------------------------------------------------------------------------
// Wi-Fi helpers
// ---------------------------------------------------------------------------

static const char *wifiStatusName(int st)
{
    switch (st)
    {
    case WL_IDLE_STATUS:     return "IDLE";
    case WL_NO_SSID_AVAIL:  return "NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED:  return "SCAN_COMPLETED";
    case WL_CONNECTED:       return "CONNECTED";
    case WL_CONNECT_FAILED:  return "CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED:    return "DISCONNECTED";
    default:                 return "UNKNOWN";
    }
}

static void printWifiScan()
{
    Serial.println("WiFi scan: starting...");
    int n = WiFi.scanNetworks();
    if (n < 0)
    {
        Serial.print("WiFi scan: failed, code=");
        Serial.println(n);
        return;
    }

    Serial.print("WiFi scan: found ");
    Serial.print(n);
    Serial.println(" network(s)");

    const int maxToPrint = (n > 15) ? 15 : n;
    for (int i = 0; i < maxToPrint; ++i)
    {
        Serial.print(" - ");
        Serial.print(WiFi.SSID(i));
        Serial.print("  RSSI=");
        Serial.print(WiFi.RSSI(i));

#if !defined(ARDUINO_ARCH_ESP32)
        Serial.print("  ENC=");
        Serial.print((int)WiFi.encryptionType(i));
#endif

        Serial.println();
    }
}

static String getMacString()
{
    uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
    WiFi.macAddress(mac);
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(buf);
}

// ---------------------------------------------------------------------------
// setup
// ---------------------------------------------------------------------------

void setup()
{
    Serial.begin(115200);
    delay(200);
    SerialConfig::begin();

    // Configure Matrix Portal M4 UP/DOWN buttons (active LOW, internal pull-up).
    pinMode(HardwareConfiguration::BUTTON_UP_PIN,   INPUT_PULLUP);
    pinMode(HardwareConfiguration::BUTTON_DOWN_PIN, INPUT_PULLUP);

    // Attach hardware interrupts to both buttons so presses are latched even
    // during blocking HTTPS fetches.  FALLING = HIGH→LOW (active-LOW buttons).
    attachInterrupt(digitalPinToInterrupt(HardwareConfiguration::BUTTON_UP_PIN),
                    buttonUpISR, FALLING);
    attachInterrupt(digitalPinToInterrupt(HardwareConfiguration::BUTTON_DOWN_PIN),
                    buttonDownISR, FALLING);

    g_display.initialize();
    g_display.displayMessage(String("FlightWall"));
    wifiClientTick = displayTick;

    if (SerialConfig::wifiSSID.length() > 0)
    {
#if !defined(ARDUINO_ARCH_ESP32)
        Serial.print("WiFiNINA firmware: ");
        Serial.println(WiFi.firmwareVersion());
#endif
        Serial.print("WiFi MAC: ");
        Serial.println(getMacString());

        printWifiScan();

        g_display.displayMessage(String("WiFi: ") + SerialConfig::wifiSSID);
        WiFi.begin(SerialConfig::wifiSSID.c_str(), SerialConfig::wifiPassword.c_str());
        Serial.print("Connecting to WiFi");
        int attempts = 0;
        int lastStatus = -999;
        while (WiFi.status() != WL_CONNECTED && attempts < 150) // ~30 s
        {
            delay(200);
            const int st = (int)WiFi.status();
            if (st != lastStatus)
            {
                lastStatus = st;
                Serial.print(" [status=");
                Serial.print(st);
                Serial.print(" ");
                Serial.print(wifiStatusName(st));
                Serial.print("]");
            }
            Serial.print(".");
            attempts++;
        }
        Serial.println();
        if (WiFi.status() == WL_CONNECTED)
        {
            Serial.print("WiFi connected: ");
            Serial.println(WiFi.localIP());
            IPAddress ip = WiFi.localIP();
            String ipStr = String(ip[0]) + "." + String(ip[1]) + "."
                         + String(ip[2]) + "." + String(ip[3]);
            g_display.displayMessage(String("WiFi OK ") + ipStr);
            delay(3000);
            g_display.showLoading();
        }
        else
        {
            Serial.print("WiFi not connected; proceeding without network. Final status=");
            Serial.println((int)WiFi.status());
            g_display.displayMessage(String("WiFi FAIL"));
        }
    }

    g_fetcher = new FlightDataFetcher(&g_openSky, &g_aeroApi);
}

// ---------------------------------------------------------------------------
// loop
// ---------------------------------------------------------------------------

void loop()
{
    const unsigned long now = millis();

    SerialConfig::tick();
    checkButtons();

    // --- MODE_SLEEP / MODE_SERIAL_CONFIG: do nothing (no fetch, no display) ---
    if (g_appMode == MODE_SLEEP)
    {
        delay(10);
        return;
    }

    if (g_appMode == MODE_SERIAL_CONFIG)
    {
        // Auto-exit serial config once the user closes the menu with 'x'.
        if (!SerialConfig::isMenuOpen())
        {
            g_appMode = g_prevActiveMode;
            g_lastFetchMs     = 0;
            g_lastTailFetchMs = 0;
            Serial.println(F("Serial config closed — resuming normal operation."));
        }
        delay(10);
        return;
    }

    // --- MODE_NEARBY: fetch nearby flights and cycle through them ---
    if (g_appMode == MODE_NEARBY)
    {
        const unsigned long intervalMs = TimingConfiguration::FETCH_INTERVAL_SECONDS * 1000UL;
        if ((g_lastFetchMs == 0 || now - g_lastFetchMs >= intervalMs) &&
            WiFi.status() == WL_CONNECTED)
        {
            std::vector<StateVector> states;
            std::vector<FlightInfo>  flights;
            size_t enriched = g_fetcher->fetchFlights(states, flights);

            Serial.print("OpenSky state vectors: ");
            Serial.println((int)states.size());
            Serial.print("AeroAPI enriched flights: ");
            Serial.println((int)enriched);

            for (const auto &s : states)
            {
                Serial.print(" ");
                Serial.print(s.callsign);
                Serial.print(" @ ");
                Serial.print(s.distance_km, 1);
                Serial.print("km bearing ");
                Serial.println(s.bearing_deg, 1);
            }

            for (const auto &f : flights)
            {
                Serial.println("=== FLIGHT INFO ===");
                Serial.print("Ident: ");
                Serial.println(f.ident);
                Serial.print("Airline: ");
                Serial.println(f.airline_display_name_full);
                Serial.print("Aircraft: ");
                Serial.println(f.aircraft_display_name_short.length()
                               ? f.aircraft_display_name_short : f.aircraft_code);
                Serial.print("Route: ");
                Serial.print(f.origin.code_icao);
                Serial.print(" > ");
                Serial.println(f.destination.code_icao);
                Serial.println("===================");
            }

            g_cachedFlights = flights;
            g_lastFetchMs   = millis();
        }

        // Re-check mode: it may have changed to SLEEP/CONFIG during the
        // blocking fetch above (via displayTick → checkButtons).
        if (g_appMode == MODE_NEARBY)
            g_display.displayFlights(g_cachedFlights);
    }

    // --- MODE_TAIL_TRACKER: fetch status for the configured tail number ---
    else if (g_appMode == MODE_TAIL_TRACKER)
    {
        const unsigned long tailIntervalMs =
            TailTrackerConfiguration::FETCH_INTERVAL_SECONDS * 1000UL;

        if ((g_lastTailFetchMs == 0 || now - g_lastTailFetchMs >= tailIntervalMs) &&
            WiFi.status() == WL_CONNECTED)
        {
            Serial.print("TailTracker: fetching status for ");
            Serial.println(SerialConfig::tailNumber);

            TailFlightStatus newStatus;
            if (g_tailFetcher.fetchStatus(
                    SerialConfig::tailNumber.c_str(), newStatus))
            {
                g_tailStatus = newStatus;
                Serial.print("TailTracker: status=");
                Serial.print(g_tailStatus.status);
                Serial.print(" progress=");
                Serial.print(g_tailStatus.progress_percent);
                Serial.print("% city=");
                Serial.print(g_tailStatus.city);
                Serial.print(" ");
                Serial.println(g_tailStatus.region);
            }
            else
            {
                Serial.println("TailTracker: fetch failed");
            }

            g_lastTailFetchMs = millis();
        }

        // Re-check mode for the same reason as above.
        if (g_appMode == MODE_TAIL_TRACKER)
        {
            if (g_tailStatus.valid)
                g_display.displayTailTracker(g_tailStatus);
            else
                g_display.displayTailLoading();
        }
    }

    delay(10);
}
