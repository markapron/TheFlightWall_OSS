# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Project Is

TheFlightWall is Arduino/PlatformIO firmware for an LED matrix flight tracker. It fetches live ADS-B data from OpenSky Network, enriches it via FlightAware AeroAPI and a custom CDN, and renders cycling flight cards on a HUB75 RGB matrix (or WS2812B NeoPixel matrix on ESP32). Two operating modes: **MODE_NEARBY** cycles through flights within a configurable radius, **MODE_TAIL_TRACKER** tracks a single aircraft by tail number. FlightAware AeroAPI calls can get expensive so the goal is to reduce usage when possible and stay below the $5/month limit

### AeroAPI Query fees breakdown

GET /flights/search/positions   $0.050/result set
GET /flights/{id}/position      $0.010/result set
GET /flights/{ident}            $0.005/result set
GET /airports/{id}              $0.015/result set

### OpenSky Query Limits

Credits                         4,000/day
GET /states/all                 1 credit/result

## Build & Flash

Built with **PlatformIO**. No separate install step — dependencies are declared in `platformio.ini` and fetched automatically.

Use this command format to build to verify

cd "E:\ws\TheFlightWall_OSS\firmware"; & "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e adafruit_matrix_portal_m4 2>&1

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

`Secrets.h` is **not** in `.gitignore` — credentials are currently hardcoded there. The serial config menu (`SerialConfig`) at runtime can update WiFi/API credentials stored in flash, but `Secrets.h` remains the compile-time fallback.

### API Response Fields Reference

What each external source actually returns, as used by this project:

**OpenSky `GET /api/states/all` — state vector array `[0..16]`**
- `[0]` icao24 — 6-char hex transponder address (the authoritative ICAO24 for that aircraft)
- `[1]` callsign — ATC callsign padded to 8 chars; for GA aircraft often matches registration, but not guaranteed
- `[5/6]` longitude / latitude
- `[7]` baro_altitude (metres)
- `[8]` on_ground (bool)
- `[9]` velocity (m/s), `[10]` heading (°), `[11]` vertical_rate (m/s)
- `[13]` geo_altitude (metres)

Query filters available: `?icao24=` (one or more hex codes) or bounding box (`lamin`/`lamax`/`lomin`/`lomax`). There is no callsign or registration filter — you must already know the ICAO24 to query a specific aircraft.

**AeroAPI `GET /flights/{ident}` — flight object fields used**
- `ident`, `ident_icao`, `ident_iata` — flight identifiers
- `operator`, `operator_icao`, `operator_iata` — airline/operator codes
- `aircraft_type` — ICAO type code (e.g. `B738`, `C172`)
- `fa_flight_id` — FlightAware internal ID
- `status` — e.g. `En Route / On Time`, `Arrived`, `Scheduled`
- `progress_percent` — 0–100; often omitted after landing (treat null as 100 if `actual_on` is set)
- `actual_off`, `actual_on`, `scheduled_off` — ISO 8601 UTC wheel-off / wheel-on timestamps
- `origin` / `destination` — each has: `code_iata`, `code_icao`, `code_lid`, `city`, `name`, `state`, `country_code`, `latitude`, `longitude`

Note: the response does **not** include the aircraft's Mode S / ICAO24 transponder hex address. There is no AeroAPI v4 endpoint that provides registration → ICAO24 lookup.

**AeroAPI `GET /airports/{id}` — fallback when `/flights` omits coordinates**
- `latitude` / `longitude` (also `lat`/`lon`/`lng` variants accepted)

**FlightWall CDN `/oss/lookup/airline/{icao}.json`**
- `display_name_full` — human-readable airline name for display

**FlightWall CDN `/oss/lookup/aircraft/{icao}.json`**
- `display_name_short`, `display_name_full` — human-readable aircraft type name

**Nominatim `GET /reverse` (tail tracker reverse geocoding)**
- `address.city` / `town` / `village` / `municipality` / `county` — nearest populated place
- `address.state` — full US state name (mapped to 2-letter abbreviation internally)
- `address.country_code` — 2-letter ISO country code

**Nominatim `GET /search` (tail tracker forward geocoding)**
- `lat`, `lon` — coordinates of best-match result

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
