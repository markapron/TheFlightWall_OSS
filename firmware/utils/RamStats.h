#pragma once

#include <stddef.h>

// Best-effort free RAM. ESP32: ESP.getFreeHeap(). SAMD: stack–heap gap via sbrk.
// Returns 0 if unknown / unsupported.
size_t flightwallApproxFreeBytes();

// Second heap metric (platform-specific; use with "approx free" to spot fragmentation):
// - ESP32: largest single allocatable block (heap_caps); good fragmentation signal.
// - SAMD51: newlib mallinfo().fordblks = total free bytes in the malloc free-list
//   (or stack–heap sbrk margin if newlib-nano stub returns 0).  Not the same as ESP
//   largest block, but non-zero and useful on the main MCU.  On-board ESP
//   coprocessor is not running this firmware, so it cannot be queried here.
size_t flightwallLargestFreeBlockBytes();
