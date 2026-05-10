#include "utils/AirportNameCache.h"
#include "utils/GeoUtils.h"
#include <string.h>

#if defined(ARDUINO_ARCH_ESP32)
  #include <Preferences.h>
#endif

// ---------------------------------------------------------------------------
// Persistent storage layout (SAMD51)
// FlashStorage_SAMD is only included in SerialConfig.cpp to avoid duplicate
// symbol errors when both translation units would otherwise instantiate the
// same header-defined functions.  The two helpers below are defined there and
// declared here at global scope so the linker resolves them correctly.
// ---------------------------------------------------------------------------
#if !defined(ARDUINO_ARCH_ESP32)
extern void airportCacheFlashSave(const void *data, size_t len);
extern bool airportCacheFlashLoad(void *data, size_t len);
#endif

namespace AirportNameCache {

static const int kMaxEntries = 32; // in-memory capacity (both platforms)

// SAMD51 EEPROM is 1024 bytes; SerialConfig occupies 0-255, leaving 768 bytes.
// 8 entries × 64 bytes + 8 bytes header = 520 bytes → fits with headroom.
// ESP32 NVS has no meaningful size constraint at this scale.
#if !defined(ARDUINO_ARCH_ESP32)
static const int kMaxPersisted = 8;
#else
static const int kMaxPersisted = 32;
#endif

static AirportCacheEntry s_entries[kMaxEntries];
static uint8_t           s_count = 0;

#if !defined(ARDUINO_ARCH_ESP32)
static const uint32_t kMagic = 0xA1C10002UL; // bumped when kMaxEntries split from kMaxPersisted

struct SAMDAirportCache {
    uint32_t          magic;
    uint8_t           count;
    uint8_t           _pad[3]; // keep entries 4-byte-aligned
    AirportCacheEntry entries[kMaxPersisted]; // on-flash size fixed at 8 entries
};
#endif

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static void saveToPlatform()
{
#if defined(ARDUINO_ARCH_ESP32)
    Preferences prefs;
    prefs.begin("apCache", false);
    prefs.putUChar("cnt", s_count);
    for (int i = 0; i < s_count; ++i) {
        char key[4];
        snprintf(key, sizeof(key), "n%d", i);
        prefs.putBytes(key, &s_entries[i], sizeof(AirportCacheEntry));
    }
    prefs.end();
#else
    // Persist the kMaxPersisted most-recently-added entries (tail of the array).
    // static keeps the 520-byte struct in BSS rather than on the call stack.
    static SAMDAirportCache ac;
    memset(&ac, 0, sizeof(ac));
    ac.magic = kMagic;
    const int saveStart = (s_count > kMaxPersisted) ? s_count - kMaxPersisted : 0;
    const int saveN     = s_count - saveStart;
    ac.count = (uint8_t)saveN;
    memcpy(ac.entries, &s_entries[saveStart], saveN * sizeof(AirportCacheEntry));
    airportCacheFlashSave(&ac, sizeof(ac));
#endif
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

String abbreviateName(const String &raw)
{
    String s = raw;
    // Longest phrase first so "Children's Hospital" is caught before "Hospital".
    static const struct { const char *from; const char *to; } kSubs[] = {
        { "Children's Hospital", "CH" },
        { "Childrens Hospital",  "CH" },
        { "Hospital",            "H"  },
    };
    for (const auto &sub : kSubs) {
        int idx = s.indexOf(sub.from);
        if (idx < 0) {
            String upper = s;
            upper.toUpperCase();
            String fromUpper = sub.from;
            fromUpper.toUpperCase();
            idx = upper.indexOf(fromUpper);
        }
        if (idx >= 0) {
            s = s.substring(0, idx) + sub.to
                + s.substring(idx + (int)strlen(sub.from));
        }
    }
    s.trim();
    return s;
}

void begin()
{
    s_count = 0;
#if defined(ARDUINO_ARCH_ESP32)
    Preferences prefs;
    prefs.begin("apCache", true);
    const uint8_t cnt = prefs.getUChar("cnt", 0);
    const uint8_t lim = cnt < kMaxEntries ? cnt : (uint8_t)kMaxEntries;
    for (int i = 0; i < lim; ++i) {
        char key[4];
        snprintf(key, sizeof(key), "n%d", i);
        if (prefs.getBytes(key, &s_entries[i], sizeof(AirportCacheEntry))
                == sizeof(AirportCacheEntry)) {
            s_entries[i].code[sizeof(s_entries[i].code) - 1] = '\0';
            s_entries[i].name[sizeof(s_entries[i].name) - 1] = '\0';
            ++s_count;
        }
    }
    prefs.end();
#else
    static SAMDAirportCache ac; // static: BSS not stack
    if (airportCacheFlashLoad(&ac, sizeof(ac)) && ac.magic == kMagic) {
        const uint8_t lim = ac.count < kMaxPersisted ? ac.count : (uint8_t)kMaxPersisted;
        memcpy(s_entries, ac.entries, lim * sizeof(AirportCacheEntry));
        for (int i = 0; i < lim; ++i) {
            s_entries[i].code[sizeof(s_entries[i].code) - 1] = '\0';
            s_entries[i].name[sizeof(s_entries[i].name) - 1] = '\0';
        }
        s_count = lim;
    }
#endif
    Serial.print(F("[AirportNameCache] "));
    Serial.print(s_count);
    Serial.println(F(" entries loaded"));
}

bool findByCode(const char *code, AirportCacheEntry &out)
{
    for (int i = 0; i < s_count; ++i) {
        if (strcmp(s_entries[i].code, code) == 0) {
            out = s_entries[i];
            return true;
        }
    }
    return false;
}

bool findNearest(double lat, double lon, double threshKm, AirportCacheEntry &out)
{
    double bestDist = threshKm;
    int    bestIdx  = -1;
    for (int i = 0; i < s_count; ++i) {
        const double d = haversineKm(lat, lon,
                                     (double)s_entries[i].lat,
                                     (double)s_entries[i].lon);
        if (d < bestDist) {
            bestDist = d;
            bestIdx  = i;
        }
    }
    if (bestIdx >= 0) {
        out = s_entries[bestIdx];
        return true;
    }
    return false;
}

void store(const char *code, const char *name, float lat, float lon)
{
    for (int i = 0; i < s_count; ++i) {
        if (strcmp(s_entries[i].code, code) == 0) {
            strncpy(s_entries[i].name, name, sizeof(s_entries[i].name) - 1);
            s_entries[i].name[sizeof(s_entries[i].name) - 1] = '\0';
            s_entries[i].lat = lat;
            s_entries[i].lon = lon;
            saveToPlatform();
            return;
        }
    }

    int slot;
    if (s_count < kMaxEntries) {
        slot = s_count++;
    } else {
        // FIFO eviction: shift left, write last slot.
        memmove(&s_entries[0], &s_entries[1],
                (kMaxEntries - 1) * sizeof(AirportCacheEntry));
        slot = kMaxEntries - 1;
    }

    memset(&s_entries[slot], 0, sizeof(AirportCacheEntry));
    strncpy(s_entries[slot].code, code, sizeof(s_entries[slot].code) - 1);
    strncpy(s_entries[slot].name, name, sizeof(s_entries[slot].name) - 1);
    s_entries[slot].lat = lat;
    s_entries[slot].lon = lon;
    saveToPlatform();
}

} // namespace AirportNameCache
