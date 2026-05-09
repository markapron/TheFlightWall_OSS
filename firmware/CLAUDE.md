# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Project Is

TheFlightWall is Arduino/PlatformIO firmware for an LED matrix flight tracker. It fetches live ADS-B data from OpenSky Network, enriches it via FlightAware AeroAPI and a custom CDN, and renders cycling flight cards on a HUB75 RGB matrix (or WS2812B NeoPixel matrix on ESP32). Two operating modes: **MODE_NEARBY** cycles through flights within a configurable radius, **MODE_TAIL_TRACKER** tracks a single aircraft by tail number.

### Cost reduction

AeroAPI calls are expensive and should be limited to less than 500 calls per month. Use data from OpenSky whenever possible.

## git
The git repo is in one directory up from here. Always prompt the user for approval with commit messages to ensure testing was successful.

## Build & Flash

Built with **PlatformIO**. No separate install step — dependencies are declared in `platformio.ini` and fetched automatically.

To build in the terminal use this command:
PowerShell(cd "E:\ws\TheFlightWall_OSS\firmware"; & "C:\Users\Mark\.platformio\penv\Scripts\pio.exe" run -e adafruit_matrix_portal_m4 2>&1)

```bash
# Matrix Portal M4 (primary target)
pio run -e adafruit_matrix_portal_m4 --target upload

# ESP32
pio run -e esp32dev --target upload

# Build only (no upload)
pio run -e adafruit_matrix_portal_m4

# Serial monitor
pio device monitor
```

Tests use Unity (`test_framework = unity`) but no test files exist yet.

## Architecture

### Layers

- **`interfaces/`** — Abstract base classes (`BaseDisplay`, `BaseStateVectorFetcher`, `BaseFlightFetcher`). Core logic depends only on these.
- **`adapters/`** — Concrete implementations injected at runtime: `OpenSkyFetcher`, `AeroAPIFetcher`, `TailTrackerFetcher`, `FlightWallFetcher`, `ProtomatterDisplay` (HUB75), `NeoMatrixDisplay` (NeoPixel).
- **`core/FlightDataFetcher`** — Orchestrates the fetch → enrich pipeline using injected adapters.
- **`models/`** — Plain data structures (`StateVector`, `FlightInfo`, `AirportInfo`, `TailFlightStatus`).
- **`utils/`** — `GeoUtils` (Haversine, bounding boxes, bearing), `HttpUtils` (URL parsing, WiFiSSLClient wrapper), `MemoryUtils`, `SerialConfig`.
- **`config/`** — Per-concern header-only configuration. Edit these to change behavior without touching logic files.
- **`src/main.cpp`** — Top-level: WiFi setup, fetch loop, `displayTick()`, button ISRs, mode switching.

### Data Flow (MODE_NEARBY)

1. `OpenSkyFetcher` → OAuth token → `/states/all` → filter by bounding box and bearing cone
2. Sort: airborne first (`on_ground == false`), then by distance — ensures enrichment quota targets en-route flights
3. `AeroAPIFetcher` → enriches up to `MAX_ENRICHED_FLIGHTS` (default 3) with route, aircraft type
4. `FlightWallFetcher` → CDN lookup for display-friendly airline and aircraft names
5. Route progress computed from airport coordinates if AeroAPI omits `progress_percent`
6. Display renders flight cards with airline, aircraft, route, progress bar, bearing compass

### Display Responsiveness During Network I/O

`wifiClientRequest()` in `HttpUtils` accepts two callbacks:
- `wifiClientTick` — called during blocking waits, drives `displayTick()` so the matrix keeps cycling and buttons stay responsive
- `wifiClientShouldAbort` — checked each iteration; user mode switches can cancel in-flight requests

This is the key mechanism keeping the UI alive during long HTTPS operations on the AirLift co-processor.

### Matrix Portal M4 HTTPS Workaround

The M4 target uses a direct `WiFiSSLClient` wrapper (`HttpUtils`) rather than `ArduinoHttpClient`. ArduinoHttpClient causes SPI fragmentation issues over the AirLift transport on this board. The wrapper handles chunked transfer encoding manually.

### Memory Constraints

The SAMD51 on the Matrix Portal M4 has limited SRAM. Key practices already in place:
- Only one mode's dataset in memory at a time — dropping the inactive one when switching modes
- `flightwallVectorDrop()` / `flightwallStringDrop()` force immediate deallocation (avoid relying on destructor timing)
- `RamStats` / `MemoryUtils` for heap monitoring during development

### OpenSky OAuth

Client-credentials flow. Token is cached and refreshed 60 seconds before expiry. Consecutive fetch failures trigger a forced refresh. Monthly quota is ~4000 requests; a 30-second fetch interval stays within it.

## Configuration

All configuration lives in `config/`. The files that are most commonly edited:

| File | Controls |
|------|----------|
| `UserConfiguration.h` | Center lat/lon, radius (km), max enriched flights, brightness |
| `TimingConfiguration.h` | Fetch interval (s), display cycle speed (s) |
| `TailTrackerConfiguration.h` | Tail tracker fetch interval, geocoding cache threshold |
| `HardwareConfiguration.h` | HUB75 pin assignments, matrix dimensions, button pins |
| `Secrets.h` | WiFi credentials, OpenSky OAuth, AeroAPI key, tracked tail number |

`Secrets.h` is in `.gitignore` one directory up from here — credentials are currently hardcoded there. The serial config menu (`SerialConfig`) at runtime can update WiFi/API credentials stored in flash, but `Secrets.h` remains the compile-time fallback.

## Button Behavior (Matrix Portal M4)

Buttons are interrupt-driven (FALLING edge, active-LOW) so presses are latched during blocking network calls.

- **UP** — Switch between MODE_NEARBY ↔ MODE_TAIL_TRACKER; wake from sleep/config
- **DOWN** (1 tap, 700 ms timeout) — Enter MODE_SLEEP
- **DOWN** (3 taps within 700 ms) — Enter MODE_SERIAL_CONFIG (serial credentials menu)
- **Any tap while sleeping** — Wake up

## Build Flags

Defined in `platformio.ini` per environment:

- `-DFLIGHTWALL_DISPLAY_PROTOMATTER=1` — HUB75 via Protomatter (Matrix Portal M4)
- `-DFLIGHTWALL_DISPLAY_NEOMATRIX=1` — NeoPixel via FastLED (ESP32)
- `-DFLIGHTWALL_SKIP_TLS=1` — Bypass TLS (commented out; development only)
