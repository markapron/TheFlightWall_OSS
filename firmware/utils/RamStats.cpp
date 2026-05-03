#include "utils/RamStats.h"
#include <stdint.h>

#if defined(ARDUINO_ARCH_ESP32)
  #include <ESP.h>
  #include <esp_heap_caps.h>
#elif defined(ARDUINO_ARCH_SAMD)
  extern "C" char *sbrk(int incr);
  #include <malloc.h> // newlib: mallinfo()
#endif

#if defined(ARDUINO_ARCH_SAMD)
// Stack–heap gap: room before sbrk(0) collides with the stack (grows down).
// This is the same "approx free" number we already use; not the same as ESP32
// `heap_caps` largest block, but it is meaningful on a bare metal SAMD.
static size_t samd_stack_heap_gap_bytes()
{
    char         stackTop;
    char *const  heapEnd = (char *)sbrk(0);
    const uintptr_t s   = reinterpret_cast<uintptr_t>(&stackTop);
    const uintptr_t h   = reinterpret_cast<uintptr_t>(heapEnd);
    if (s <= h)
        return 0;
    return static_cast<size_t>(s - h);
}
#endif

size_t flightwallApproxFreeBytes()
{
#if defined(ARDUINO_ARCH_ESP32)
    return static_cast<size_t>(ESP.getFreeHeap());
#elif defined(ARDUINO_ARCH_SAMD)
    return samd_stack_heap_gap_bytes();
#else
    return 0;
#endif
}

size_t flightwallLargestFreeBlockBytes()
{
#if defined(ARDUINO_ARCH_ESP32)
    return static_cast<size_t>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
#elif defined(ARDUINO_ARCH_SAMD)
    {
        // newlib: total size of *free* chunks in the main heap (see mallinfo(3) /
        // newlib docs).  With newlib-nano, mallinfo is sometimes a stub (zeros) —
        // in that case fall back to the stack–heap sbrk margin.
        struct mallinfo mi = mallinfo();
        if (mi.fordblks > 0)
            return static_cast<size_t>(mi.fordblks);
        return samd_stack_heap_gap_bytes();
    }
#else
    return 0;
#endif
}
