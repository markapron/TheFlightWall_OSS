# TheFlightWall Firmware

Arduino/PlatformIO firmware that fetches live ADS-B flight data and renders it on an LED matrix.

## Supported hardware

| Board | PlatformIO env | Display |
|---|---|---|
| **Adafruit Matrix Portal M4** (recommended) | `adafruit_matrix_portal_m4` | HUB75 RGB matrix via Adafruit Protomatter |
| ESP32 dev board | `esp32dev` | WS2812B NeoPixel matrix via FastLED NeoMatrix |

The Matrix Portal M4 is the recommended target. It has a built-in AirLift ESP32 WiFi co-processor, a direct HUB75 connector, and USB-C power — no external level shifter or wiring to a data pin required.

## What it does

1. **Fetch nearby aircraft** from OpenSky Network using OAuth (`states/all`), filtered by a configurable center point, radius, and bearing.
2. **Enrich flights** with readable airline name, aircraft type, and route (origin/destination airports) from FlightAware AeroAPI and TheFlightWall CDN.
3. **Render** a minimal three-line flight card on the LED matrix and cycle through the enriched flights at a configurable interval. The display keeps cycling even while a new fetch is in progress.

## Project layout

```
firmware/
├── src/
│   └── main.cpp                 # Entry point: WiFi, fetch loop, display tick
├── adapters/
│   ├── ProtomatterDisplay       # HUB75 display driver (Matrix Portal M4)
│   ├── NeoMatrixDisplay         # WS2812B display driver (ESP32)
│   ├── OpenSkyFetcher           # OpenSky OAuth + states/all query
│   ├── AeroAPIFetcher           # FlightAware AeroAPI flight details
│   └── FlightWallFetcher        # TheFlightWall CDN airline/aircraft names
├── core/
│   └── FlightDataFetcher        # Orchestrates fetch → enrich → return
├── utils/
│   ├── GeoUtils                 # Haversine distance, bounding boxes, bearing
│   └── HttpUtils                # URL parser + WiFiSSLClient request helper
├── config/
│   ├── UserConfiguration.h      # Location, brightness, max flights per cycle
│   ├── TimingConfiguration.h    # Fetch interval, display cycling speed
│   ├── HardwareConfiguration.h  # Display pin/dimension constants
│   ├── APIConfiguration.h       # API keys and base URLs
│   └── WiFiConfiguration.h      # SSID and password
├── models/                      # StateVector, FlightInfo, AirportInfo structs
├── interfaces/                  # Abstract base classes for fetchers/display
└── platformio.ini
```

## Configuration

| File | Key settings |
|---|---|
| `config/WiFiConfiguration.h` | `WIFI_SSID`, `WIFI_PASSWORD` |
| `config/UserConfiguration.h` | `CENTER_LAT`, `CENTER_LON`, `RADIUS_KM`, `MAX_ENRICHED_FLIGHTS`, `DISPLAY_BRIGHTNESS` |
| `config/TimingConfiguration.h` | `FETCH_INTERVAL_SECONDS`, `DISPLAY_CYCLE_SECONDS` |
| `config/APIConfiguration.h` | OpenSky `client_id`/`client_secret`, AeroAPI key |
| `config/HardwareConfiguration.h` | HUB75 pin assignments and matrix dimensions (Matrix Portal M4 defaults pre-set) |

## Build

```bash
# Matrix Portal M4
pio run -e adafruit_matrix_portal_m4 --target upload

# ESP32
pio run -e esp32dev --target upload
```

## Notes

- **HTTPS on Matrix Portal M4**: The AirLift co-processor handles TLS. The firmware uses `WiFiSSLClient` directly (not through `ArduinoHttpClient`) to avoid a known fragmentation issue with the AirLift's SPI transport.
- **Chunked transfer encoding**: The OpenSky and AeroAPI responses use chunked encoding; the `wifiClientRequest()` helper in `HttpUtils` decodes this transparently.
- **Rate limiting**: `MAX_ENRICHED_FLIGHTS` (default 3) caps the number of AeroAPI calls per cycle to stay within free-tier rate limits. State vectors are sorted **airborne first** (OpenSky `on_ground == false`), then by distance, so enriched flights (and AeroAPI route progress) favor aircraft in flight.
- **OAuth**: The OpenSky `client_credentials` token is cached and refreshed automatically 60 seconds before expiry.
- **Display cycling during fetch**: A `wifiClientTick` callback is invoked during all blocking waits inside `wifiClientRequest()` so the display keeps cycling through flight cards even while network requests are in progress.
