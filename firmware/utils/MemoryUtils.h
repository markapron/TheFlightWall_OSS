#pragma once

#include <Arduino.h>
#include <vector>

// Best-effort: Arduino String can keep a large internal buffer after a parse or
// assignment. Assigning a fresh empty string returns that capacity to the pool on
// many cores (SAMD, ESP) and reduces the heap high-water mark from sticking.
static inline void flightwallStringDrop(String &s) { s = String(); }

template <class T>
static inline void flightwallVectorDrop(std::vector<T> &v)
{
    v.clear();
    v.shrink_to_fit();
}
