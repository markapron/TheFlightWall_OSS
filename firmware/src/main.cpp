/*
Purpose: Firmware entry point for ESP32.
Responsibilities:
- Initialize serial, connect to Wi‑Fi, and construct fetchers and display.
- Periodically fetch state vectors (OpenSky), enrich flights (AeroAPI), and render.
Configuration: UserConfiguration (location/filters/colors), TimingConfiguration (intervals),
               WiFiConfiguration (SSID/password), HardwareConfiguration (display specs).
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
#include "adapters/OpenSkyFetcher.h"
#include "adapters/AeroAPIFetcher.h"
#include "core/FlightDataFetcher.h"
#include "utils/HttpUtils.h"
#if defined(FLIGHTWALL_DISPLAY_NEOMATRIX)
#include "adapters/NeoMatrixDisplay.h"
using ActiveDisplay = NeoMatrixDisplay;
#else
// Default to HUB75/Protomatter (Matrix Portal M4)
#include "adapters/ProtomatterDisplay.h"
using ActiveDisplay = ProtomatterDisplay;
#endif

static OpenSkyFetcher g_openSky;
static AeroAPIFetcher g_aeroApi;
static FlightDataFetcher *g_fetcher = nullptr;
static ActiveDisplay g_display;

// Cached from the last successful fetch so the display can be updated every loop
// iteration for smooth flight cycling (not just once after each API round-trip).
static std::vector<FlightInfo> g_cachedFlights;
static unsigned long g_lastFetchMs = 0;

// Called from wifiClientRequest() during blocking waits (TLS connect, response read,
// body read) so the display keeps cycling even while a fetch is in progress.
static void displayTick()
{
    g_display.displayFlights(g_cachedFlights);
}

static const char *wifiStatusName(int st)
{
    switch (st)
    {
    case WL_IDLE_STATUS:
        return "IDLE";
    case WL_NO_SSID_AVAIL:
        return "NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED:
        return "SCAN_COMPLETED";
    case WL_CONNECTED:
        return "CONNECTED";
    case WL_CONNECT_FAILED:
        return "CONNECT_FAILED";
    case WL_CONNECTION_LOST:
        return "CONNECTION_LOST";
    case WL_DISCONNECTED:
        return "DISCONNECTED";
    default:
        return "UNKNOWN";
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

void setup()
{
    Serial.begin(115200);
    delay(200);

    g_display.initialize();
    g_display.displayMessage(String("FlightWall"));
    wifiClientTick = displayTick;

    if (strlen(WiFiConfiguration::WIFI_SSID) > 0)
    {
#if !defined(ARDUINO_ARCH_ESP32)
        Serial.print("WiFiNINA firmware: ");
        Serial.println(WiFi.firmwareVersion());
#endif
        Serial.print("WiFi MAC: ");
        Serial.println(getMacString());

        printWifiScan();

        // ESP32 uses WiFi.mode(WIFI_STA); WiFiNINA doesn't expose this and defaults to STA.
        g_display.displayMessage(String("WiFi: ") + WiFiConfiguration::WIFI_SSID);
        WiFi.begin(WiFiConfiguration::WIFI_SSID, WiFiConfiguration::WIFI_PASSWORD);
        Serial.print("Connecting to WiFi");
        int attempts = 0;
        int lastStatus = -999;
        while (WiFi.status() != WL_CONNECTED && attempts < 150) // ~30s
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
            String ipStr = String(ip[0]) + "." + String(ip[1]) + "." + String(ip[2]) + "." + String(ip[3]);
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

void loop()
{
    const unsigned long intervalMs = TimingConfiguration::FETCH_INTERVAL_SECONDS * 1000UL;
    const unsigned long now = millis();

    if (now - g_lastFetchMs >= intervalMs)
    {
        std::vector<StateVector> states;
        std::vector<FlightInfo> flights;
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
            Serial.println(f.aircraft_display_name_short.length() ? f.aircraft_display_name_short : f.aircraft_code);
            Serial.print("Route: ");
            Serial.print(f.origin.code_icao);
            Serial.print(" > ");
            Serial.println(f.destination.code_icao);
            Serial.println("===================");
        }

        // Always update the cache (even if empty, so the display shows loading screen).
        g_cachedFlights = flights;

        // Reset the timer AFTER the fetch completes so the display has a full
        // FETCH_INTERVAL_SECONDS window to show the results before the next fetch.
        g_lastFetchMs = millis();
    }

    // Update the display on every loop iteration so flight cycling is smooth
    // and the display doesn't go blank while waiting for the next fetch.
    g_display.displayFlights(g_cachedFlights);
    delay(10);
}