#pragma once

#include <Arduino.h>

// Minimal URL parser: supports http:// and https:// URLs.
// Returns false if the URL can't be parsed.
bool parseUrl(const String &url, bool &outHttps, String &outHost, uint16_t &outPort, String &outPath);

// Optional tick callback invoked during blocking waits inside wifiClientRequest().
// Register a function here (e.g. to update a display) so it keeps running while
// the CPU is blocked on TLS connect, waiting for the first response byte, or reading body data.
typedef void (*WifiClientTickFn)();
extern WifiClientTickFn wifiClientTick;

// Direct WiFiSSLClient (or plain WiFiClient when FLIGHTWALL_SKIP_TLS is defined) HTTP request
// for SAMD/AirLift boards. Bypasses ArduinoHttpClient, which is incompatible with WiFiSSLClient
// on the AirLift SPI transport (small fragmented writes never flush as a complete request).
//
// extraHeaders: zero or more headers, each line already formatted as "Key: Value\r\n".
//               Do NOT include a trailing blank line — the function adds it automatically.
// body:         request body string. Empty string for GET requests.
//
// Returns true if a response was received from the server. outCode is the HTTP status code.
// Returns false on connection failure or response timeout.
#if !defined(ARDUINO_ARCH_ESP32)
bool wifiClientRequest(const String &method, const String &host, uint16_t port,
                       const String &path, const String &extraHeaders,
                       const String &body, int &outCode, String &outPayload);
#endif

