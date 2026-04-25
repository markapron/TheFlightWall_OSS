#include "utils/RamStats.h"
#include <stdint.h>

#if defined(ARDUINO_ARCH_ESP32)
  #include <ESP.h>
#elif defined(ARDUINO_ARCH_SAMD)
  extern "C" char *sbrk(int incr);
#endif

size_t flightwallApproxFreeBytes()
{
#if defined(ARDUINO_ARCH_ESP32)
    return static_cast<size_t>(ESP.getFreeHeap());
#elif defined(ARDUINO_ARCH_SAMD)
    {
        // Classic newlib estimate: room between end of heap and current stack.
        char stackTop;
        char *const heapEnd = (char *)sbrk(0);
        const uintptr_t s = reinterpret_cast<uintptr_t>(&stackTop);
        const uintptr_t h = reinterpret_cast<uintptr_t>(heapEnd);
        if (s <= h)
            return 0;
        return static_cast<size_t>(s - h);
    }
#else
    return 0;
#endif
}
