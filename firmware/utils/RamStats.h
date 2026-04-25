#pragma once

#include <stddef.h>

// Best-effort free RAM. ESP32: ESP.getFreeHeap(). SAMD: stack–heap gap via sbrk.
// Returns 0 if unknown / unsupported.
size_t flightwallApproxFreeBytes();
