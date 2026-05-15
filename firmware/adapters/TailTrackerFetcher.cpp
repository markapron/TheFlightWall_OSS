/*
Purpose: Fetch real-time status for a tracked tail number.
AeroAPI is called ONCE per flight leg to obtain route metadata (origin, destination,
timestamps).  All subsequent position updates use OpenSky via fetchByIcao24(), which
is free and requires no API key.  Flight progress is computed geometrically.
Nominatim reverse-geocoding is rate-limited by a distance threshold cache.
*/
#include "adapters/TailTrackerFetcher.h"

#include <ArduinoJson.h>
#include <ArduinoHttpClient.h>
#include "config/APIConfiguration.h"
#include "config/AirportNameConfiguration.h"
#include "config/TailTrackerConfiguration.h"
#include "utils/HttpUtils.h"
#include "utils/GeoUtils.h"
#include "utils/MemoryUtils.h"
#include "utils/AirportNameCache.h"
#include <stdlib.h>
#include <string.h>

#if defined(ARDUINO_ARCH_ESP32)
  #if defined(FLIGHTWALL_SKIP_TLS)
    #include <WiFi.h>
    using TailTlsClient = WiFiClient;
  #else
    #include <WiFi.h>
    #include <WiFiClientSecure.h>
    using TailTlsClient = WiFiClientSecure;
  #endif
#else
  #include <WiFiNINA.h>
#endif

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// How long to keep cached route data before forcing an AeroAPI refresh.
static const unsigned long ROUTE_REFRESH_INTERVAL_MS = 6UL * 3600UL * 1000UL; // 6 h

// ---------------------------------------------------------------------------
// Static route cache (shared across all TailTrackerFetcher instances)
// ---------------------------------------------------------------------------

static String        s_trackedIdent;         // ident this cache belongs to
static bool          s_routeValid     = false;
static bool          s_routeNeedsRefresh = true;
static unsigned long s_routeFetchMs   = 0;
static String        s_cachedIcao24;          // 6 hex chars; empty = unknown

// Route data from AeroAPI
static String        s_cachedIdent;
static String        s_cachedOriginCode;
static String        s_cachedDestCode;
static String        s_cachedDestName;   // From AirportNameCache / AeroAPI /airports/{id}
static double        s_cachedOriginLat = NAN;
static double        s_cachedOriginLon = NAN;
static double        s_cachedDestLat   = NAN;
static double        s_cachedDestLon   = NAN;
static String        s_cachedOriginCity;
static String        s_cachedOriginRegion;
static String        s_cachedDestCity;
static String        s_cachedDestRegion;
static String        s_cachedStatus;
static unsigned long s_cachedActualOff    = 0;
static unsigned long s_cachedActualOn     = 0;
static unsigned long s_cachedScheduledOff = 0;
static unsigned long s_cachedFetchEpoch   = 0;
static unsigned long s_cachedFetchMillis  = 0;

// Last known good aircraft position (across calls / leg transitions)
static double        s_stickyLat = NAN;
static double        s_stickyLon = NAN;
static int           s_stickyAlt = 0;

// Was the aircraft airborne on the previous successful OpenSky poll?
static bool          s_wasAirborne = false;

// fa_flight_id from AeroAPI route fetch (needed for /position endpoint).
static String        s_cachedFaFlightId;

// Last aircraft position obtained from AeroAPI GET /flights/{id}/position.
static double        s_aeroApiLat           = NAN;
static double        s_aeroApiLon           = NAN;
static int           s_aeroApiAltFt         = 0;
static int           s_aeroApiSpeedKt       = -1;  // -1 = unknown
static bool          s_aeroApiPosValid      = false;
static unsigned long s_lastAeroApiPosFetchMs = 0;

// Telemetry-based landing/takeoff inference state.
static int           s_inferLandCount    = 0;  // consecutive polls with low alt+speed
static int           s_inferFlyCount     = 0;  // consecutive polls with high alt+speed
static bool          s_telemetryOnGround = false;

// AeroAPI cost tracking — reset daily with the tally counter.
static uint16_t      s_costRouteCalls     = 0; // GET /flights/{ident}   today
static uint16_t      s_costPosCalls       = 0; // GET /flights/{id}/pos  today
static uint16_t      s_costAirportCalls   = 0; // GET /airports/{code}   today
static uint16_t      s_prevCostRouteCalls   = 0; // GET /flights/{ident}   yesterday
static uint16_t      s_prevCostPosCalls     = 0; // GET /flights/{id}/pos  yesterday
static uint16_t      s_prevCostAirportCalls = 0; // GET /airports/{code}   yesterday
static uint32_t      s_pollCount          = 0; // total fetchStatus calls (never reset)

static bool isAeroApiBudgetExhausted() {
    const uint32_t spent = (uint32_t)s_costRouteCalls   *  5u
                         + (uint32_t)s_costPosCalls     * 10u
                         + (uint32_t)s_costAirportCalls * 15u;
    return spent >= APIConfiguration::AEROAPI_DAILY_BUDGET_MILLIDOLLARS;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static String safeStr(JsonVariant v, const char *key)
{
    if (v[key].isNull()) return String("");
    return String(v[key].as<const char *>());
}

static bool hasPlausibleLatLon(double lat, double lon)
{
    if (isnan(lat) || isnan(lon))   return false;
    if (lat == 0.0 && lon == 0.0)  return false;
    return true;
}

// Parse ISO 8601 UTC "YYYY-MM-DDTHH:MM:SSZ" → Unix epoch.  Returns 0 on failure.
static unsigned long parseISO8601(const String &s)
{
    if (s.length() < 19) return 0;
    int year  = s.substring(0,  4).toInt();
    int month = s.substring(5,  7).toInt();
    int day   = s.substring(8, 10).toInt();
    int hour  = s.substring(11,13).toInt();
    int mn    = s.substring(14,16).toInt();
    int sec   = s.substring(17,19).toInt();
    if (year < 2020 || month < 1 || month > 12 || day < 1 || day > 31) return 0;
    static const int kDim[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    bool leap = (year%4==0) && (year%100!=0 || year%400==0);
    long days = (long)(year-1970)*365L + (long)((year-1969)/4) - (long)((year-1901)/100) + (long)((year-1601)/400);
    for (int m=1; m<month; ++m) { days += kDim[m]; if (m==2 && leap) ++days; }
    days += day-1;
    return (unsigned long)(days*86400L + (long)hour*3600L + (long)mn*60L + (long)sec);
}

static void extractAirportLatLon(JsonObject obj, double &outLat, double &outLon)
{
    auto tryGet = [](JsonVariant v, const char *key, double &out) -> bool {
        if (v.isNull() || v[key].isNull()) return false;
        out = v[key].as<double>();
        return true;
    };
    if (tryGet(obj, "latitude", outLat) && tryGet(obj, "longitude", outLon)) return;
    if (tryGet(obj, "lat", outLat) && (tryGet(obj, "lon", outLon) || tryGet(obj, "lng", outLon))) return;
    if (!obj["position"].isNull() && obj["position"].is<JsonObject>()) {
        JsonObject pos = obj["position"].as<JsonObject>();
        tryGet(pos, "lat", outLat); tryGet(pos, "lon", outLon); tryGet(pos, "lng", outLon);
        tryGet(pos, "latitude", outLat); tryGet(pos, "longitude", outLon);
    }
    if (!obj["airport"].isNull() && obj["airport"].is<JsonObject>()) {
        JsonObject inner = obj["airport"].as<JsonObject>();
        tryGet(inner, "latitude", outLat); tryGet(inner, "longitude", outLon);
        tryGet(inner, "lat", outLat); tryGet(inner, "lon", outLon); tryGet(inner, "lng", outLon);
    }
}

// ---------------------------------------------------------------------------
// N-number → ICAO24 conversion (US FAA registrations only, no API needed)
// ---------------------------------------------------------------------------
//
// The FAA allocates Mode S addresses 0xA00001–0xAFFFFF for US-registered aircraft.
// N-numbers map sequentially in the order:
//   N1, N1A, N1AA, N1AB, …, N1AX, N1B, …, N1XX, N2, N2A, … N99999XX
// Each numeric group has 601 slots (1 bare + 24 first-letters × 25 sub-slots).
// Letters used: ABCDEFGHJKLMNPQRSTUVWXYZ (24; I and O excluded).
//
// Returns true and writes a 6-char lowercase hex string into outHex[7].

static const char N_LETTERS[] = "ABCDEFGHJKLMNPQRSTUVWXYZ"; // 24 letters

bool TailTrackerFetcher::nNumberToIcao24(const String &nNumber, char outHex[7])
{
    const char *s = nNumber.c_str();
    if (s[0] != 'N' && s[0] != 'n') return false;
    ++s;
    if (!isdigit((unsigned char)s[0])) return false;

    uint32_t num = 0;
    while (*s && isdigit((unsigned char)*s)) {
        num = num * 10u + (uint32_t)(*s - '0');
        ++s;
    }
    if (num < 1u || num > 99999u) return false;

    int idx1 = -1, idx2 = -1;
    if (*s) {
        const char *p = strchr(N_LETTERS, (char)toupper((unsigned char)*s));
        if (!p) return false;
        idx1 = (int)(p - N_LETTERS);
        ++s;
        if (*s) {
            const char *p2 = strchr(N_LETTERS, (char)toupper((unsigned char)*s));
            if (!p2) return false;
            idx2 = (int)(p2 - N_LETTERS);
            ++s;
        }
    }
    if (*s) return false; // unexpected trailing chars

    uint32_t offset = 0;
    if      (idx1 < 0)  offset = 0;
    else if (idx2 < 0)  offset = 1u + (uint32_t)idx1 * 25u;
    else                offset = 2u + (uint32_t)idx1 * 25u + (uint32_t)idx2;

    const uint32_t icao = 0xA00001u + (num - 1u) * 601u + offset;
    snprintf(outHex, 7, "%06x", (unsigned)icao);
    return true;
}

// ---------------------------------------------------------------------------
// URL-encode a string for a Nominatim query parameter
// ---------------------------------------------------------------------------

static void appendUrlQueryEncoded(const String &s, String &out)
{
    for (size_t i = 0; i < s.length(); ++i) {
        const unsigned char c = (unsigned char)s[i];
        if ((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.') {
            out += (char)c;
        } else if (c == ' ') {
            out += '+';
        } else {
            char buf[5];
            snprintf(buf, sizeof(buf), "%%%02X", (unsigned)c);
            out += buf;
        }
    }
}

// ---------------------------------------------------------------------------
// Telemetry-based landing/takeoff inference
// ---------------------------------------------------------------------------
// Updates the consecutive-poll counters and flips s_telemetryOnGround when
// thresholds are met. altFt and speedKt use -1 to mean "unknown"; unknown
// values don't advance either counter so inference stays neutral.

static void updateTelemetryInference(int altFt, int speedKt)
{
    using namespace TailTrackerConfiguration;
    const bool altKnown   = (altFt   >= 0);
    const bool speedKnown = (speedKt >= 0);

    const bool lowAlt    = altKnown   && altFt   < INFER_LAND_ALT_FT;
    const bool lowSpeed  = speedKnown && speedKt < INFER_LAND_SPEED_KT;
    const bool highAlt   = altKnown   && altFt   >= INFER_FLY_ALT_FT;
    const bool highSpeed = speedKnown && speedKt >= INFER_FLY_SPEED_KT;

    if (lowAlt && lowSpeed) {
        ++s_inferLandCount;
        s_inferFlyCount = 0;
    } else if (highAlt && highSpeed) {
        ++s_inferFlyCount;
        s_inferLandCount = 0;
    } else {
        s_inferLandCount = 0;
        s_inferFlyCount  = 0;
    }

    if (s_inferLandCount >= INFER_LAND_CONSECUTIVE && !s_telemetryOnGround) {
        s_telemetryOnGround = true;
        Serial.print(F("TailTracker: telemetry inference -> LANDED (alt="));
        Serial.print(altFt);
        Serial.print(F("ft spd="));
        Serial.print(speedKt);
        Serial.println(F("kt)"));
    }
    if (s_inferFlyCount >= INFER_FLY_CONSECUTIVE && s_telemetryOnGround) {
        s_telemetryOnGround = false;
        Serial.print(F("TailTracker: telemetry inference -> FLYING (alt="));
        Serial.print(altFt);
        Serial.print(F("ft spd="));
        Serial.print(speedKt);
        Serial.println(F("kt)"));
    }
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

TailTrackerFetcher::TailTrackerFetcher(OpenSkyFetcher *openSky)
    : _openSky(openSky) {}

// ---------------------------------------------------------------------------
// fetchAirportInfoForCache — one-time AeroAPI GET /airports/{code} for name+coords
// ---------------------------------------------------------------------------

static bool fetchAirportInfoForCache(const String &airportCode,
                                     String &outName, double &outLat, double &outLon)
{
    if (airportCode.length() == 0) return false;
    if (isAeroApiBudgetExhausted()) {
        Serial.println(F("TailTracker: AeroAPI daily budget exhausted — skipping /airports/{code}"));
        return false;
    }

    const String url = String(APIConfiguration::AEROAPI_BASE_URL) + "/airports/" + airportCode;
    bool https = true;
    String host;
    uint16_t port = 443;
    String path;
    if (!parseUrl(url, https, host, port, path)) return false;

#if defined(FLIGHTWALL_SKIP_TLS)
    https = false; port = 80;
#else
    if (!https) return false;
#endif

    int    code = -1;
    String payload;

#if defined(ARDUINO_ARCH_ESP32)
    {
        TailTlsClient net;
#if !defined(FLIGHTWALL_SKIP_TLS)
        if (APIConfiguration::AEROAPI_INSECURE_TLS) net.setInsecure();
#endif
        HttpClient http(net, host.c_str(), port);
        http.setHttpResponseTimeout(30000);
        http.beginRequest();
        http.get(path);
        http.sendHeader("x-apikey", APIConfiguration::AEROAPI_KEY);
        http.sendHeader("Accept",   "application/json");
        http.endRequest();
        code    = http.responseStatusCode();
        payload = http.responseBody();
    }
#else
    {
        const String hdrs = String("x-apikey: ") + APIConfiguration::AEROAPI_KEY
                          + "\r\nAccept: application/json\r\n";
        if (!wifiClientRequest("GET", host, port, path, hdrs, "", code, payload))
            return false;
    }
#endif

    if (code != 200) {
        Serial.print(F("TailTracker: /airports/ HTTP "));
        Serial.println(code);
        flightwallStringDrop(payload);
        return false;
    }
    ++s_costAirportCalls;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    flightwallStringDrop(payload);
    if (err) { doc.clear(); return false; }

    if (!doc["name"].isNull())
        outName = doc["name"].as<const char *>();

    outLat = NAN; outLon = NAN;
    if (!doc["latitude"].isNull())  outLat = doc["latitude"].as<double>();
    if (!doc["longitude"].isNull()) outLon = doc["longitude"].as<double>();
    if (isnan(outLat) && !doc["lat"].isNull()) outLat = doc["lat"].as<double>();
    if (isnan(outLon)) {
        if (!doc["lon"].isNull()) outLon = doc["lon"].as<double>();
        if (!doc["lng"].isNull()) outLon = doc["lng"].as<double>();
    }

    doc.clear();
    return outName.length() > 0;
}

// ---------------------------------------------------------------------------
// fetchRouteFromAeroAPI — called once per flight leg; caches route metadata
// ---------------------------------------------------------------------------

bool TailTrackerFetcher::fetchRouteFromAeroAPI(const String &ident)
{
    if (strlen(APIConfiguration::AEROAPI_KEY) == 0) {
        Serial.println(F("TailTrackerFetcher: No AeroAPI key configured"));
        return false;
    }
    if (isAeroApiBudgetExhausted()) {
        Serial.println(F("TailTracker: AeroAPI daily budget exhausted — skipping /flights/{ident}"));
        return false;
    }

    const String url = String(APIConfiguration::AEROAPI_BASE_URL) + "/flights/" + ident;
    bool https = true;
    String host;
    uint16_t port = 443;
    String path;
    if (!parseUrl(url, https, host, port, path)) {
        Serial.println(F("TailTrackerFetcher: Failed to parse AeroAPI URL"));
        return false;
    }
#if defined(FLIGHTWALL_SKIP_TLS)
    https = false; port = 80;
#else
    if (!https) { Serial.println(F("TailTrackerFetcher: Refusing non-HTTPS URL")); return false; }
#endif

    int    code = -1;
    String payload;

#if defined(ARDUINO_ARCH_ESP32)
    {
        TailTlsClient net;
#if !defined(FLIGHTWALL_SKIP_TLS)
        if (APIConfiguration::AEROAPI_INSECURE_TLS) net.setInsecure();
#endif
        HttpClient http(net, host.c_str(), port);
        http.setHttpResponseTimeout(30000);
        http.beginRequest();
        http.get(path);
        http.sendHeader("x-apikey", APIConfiguration::AEROAPI_KEY);
        http.sendHeader("Accept",   "application/json");
        http.endRequest();
        code    = http.responseStatusCode();
        payload = http.responseBody();
    }
#else
    {
        const String hdrs = String("x-apikey: ") + APIConfiguration::AEROAPI_KEY
                          + "\r\nAccept: application/json\r\n";
        if (!wifiClientRequest("GET", host, port, path, hdrs, "", code, payload)) {
            Serial.println(F("TailTrackerFetcher: AeroAPI request failed"));
            return false;
        }
    }
#endif

    if (code != 200) {
        Serial.print(F("TailTrackerFetcher: AeroAPI HTTP "));
        Serial.println(code);
        flightwallStringDrop(payload);
        return false;
    }

    // Filter to only the fields we need; reduces parse time and RAM.
    JsonDocument filter;
    filter["flights"][0]["fa_flight_id"]     = true;
    filter["flights"][0]["ident"]            = true;
    filter["flights"][0]["status"]           = true;
    filter["flights"][0]["progress_percent"] = true;
    filter["flights"][0]["actual_off"]       = true;
    filter["flights"][0]["actual_on"]        = true;
    filter["flights"][0]["scheduled_off"]    = true;
    filter["flights"][0]["origin"]["city"]         = true;
    filter["flights"][0]["origin"]["name"]         = true;
    filter["flights"][0]["origin"]["state"]        = true;
    filter["flights"][0]["origin"]["country_code"] = true;
    filter["flights"][0]["origin"]["latitude"]     = true;
    filter["flights"][0]["origin"]["longitude"]    = true;
    filter["flights"][0]["origin"]["code_iata"]    = true;
    filter["flights"][0]["origin"]["code_icao"]    = true;
    filter["flights"][0]["origin"]["code_lid"]     = true;
    filter["flights"][0]["destination"]["city"]         = true;
    filter["flights"][0]["destination"]["name"]         = true;
    filter["flights"][0]["destination"]["code_iata"]    = true;
    filter["flights"][0]["destination"]["code_icao"]    = true;
    filter["flights"][0]["destination"]["code_lid"]     = true;
    filter["flights"][0]["destination"]["state"]        = true;
    filter["flights"][0]["destination"]["country_code"] = true;
    filter["flights"][0]["destination"]["latitude"]     = true;
    filter["flights"][0]["destination"]["longitude"]    = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload,
                                               DeserializationOption::Filter(filter));
    flightwallStringDrop(payload);
    if (err) {
        Serial.print(F("TailTrackerFetcher: JSON parse error: "));
        Serial.println(err.c_str());
        return false;
    }

    JsonArray flights = doc["flights"].as<JsonArray>();
    if (flights.isNull() || flights.size() == 0) {
        Serial.println(F("TailTrackerFetcher: No flights in response"));
        return false;
    }

    // Current epoch for leg selection.
#if !defined(ARDUINO_ARCH_ESP32)
    unsigned long nowEpoch = (unsigned long)WiFi.getTime();
#else
    unsigned long nowEpoch = (unsigned long)time(nullptr);
#endif

    // Pass 1: prefer the leg currently airborne (actual_off set, actual_on not set).
    int  bestIdx       = 0;
    bool foundAirborne = false;
    for (size_t i = 0; i < flights.size(); ++i) {
        if (!flights[i]["actual_off"].isNull() && flights[i]["actual_on"].isNull()) {
            bestIdx = (int)i; foundAirborne = true; break;
        }
    }

    // Pass 2: no airborne leg found — pick the most recently-departed un-landed leg
    // that departs after the last known landing, or fall back to closest-to-now.
    if (!foundAirborne) {
        unsigned long lastLandEpoch = 0;
        for (size_t i = 0; i < flights.size(); ++i) {
            const String onStr = safeStr(flights[i], "actual_on");
            if (onStr.length() == 0) continue;
            const unsigned long t = parseISO8601(onStr);
            if (t > lastLandEpoch) lastLandEpoch = t;
        }

        int           bestAfterLand = -1;
        unsigned long bestDepSo     = 0;
        for (size_t i = 0; i < flights.size(); ++i) {
            if (!flights[i]["actual_on"].isNull()) continue;
            const String soStr = safeStr(flights[i], "scheduled_off");
            if (soStr.length() == 0) continue;
            const unsigned long soEp = parseISO8601(soStr);
            if (soEp == 0) continue;
            if (nowEpoch > 0 && soEp >= nowEpoch) continue;
            if (soEp <= lastLandEpoch) continue;
            if (soEp > bestDepSo) { bestDepSo = soEp; bestAfterLand = (int)i; }
        }

        if (bestAfterLand >= 0) {
            bestIdx = bestAfterLand;
        } else if (nowEpoch > 0) {
            // Only run the closest-to-now fallback when the clock is valid.
            // With nowEpoch==0, d = ep - 0 = ep, so the oldest leg would win
            // (smallest Unix timestamp), which is the opposite of what we want.
            // When the clock is invalid, leave bestIdx=0 — AeroAPI returns legs
            // in descending chronological order so index 0 is always the most recent.
            unsigned long bestDiff = ULONG_MAX;
            for (size_t i = 0; i < flights.size(); ++i) {
                unsigned long minDiff = ULONG_MAX;
                auto tryTs = [&](const char *key) {
                    const String ts = safeStr(flights[i], key);
                    if (ts.length() == 0) return;
                    const unsigned long ep = parseISO8601(ts);
                    if (ep == 0) return;
                    const unsigned long d = ep > nowEpoch ? ep - nowEpoch : nowEpoch - ep;
                    if (d < minDiff) minDiff = d;
                };
                tryTs("scheduled_off");
                tryTs("scheduled_on");
                if (minDiff < bestDiff) { bestDiff = minDiff; bestIdx = (int)i; }
            }
        }
    }

    Serial.print(F("TailTrackerFetcher: using flights["));
    Serial.print(bestIdx);
    Serial.println(F("] (AeroAPI route fetch)"));

    JsonObject f = flights[bestIdx].as<JsonObject>();

    // Cache extracted data.
    s_cachedFaFlightId = safeStr(f, "fa_flight_id");
    s_cachedIdent      = safeStr(f, "ident");
    s_cachedStatus = safeStr(f, "status");
    if (s_cachedStatus == "Arrived")                  s_cachedStatus = "Landed";
    if (s_cachedStatus.startsWith("En Route"))        s_cachedStatus = "Flying";
    if (s_cachedStatus.startsWith("Arriving"))        s_cachedStatus = "Arriving";

    {
        const String offStr    = safeStr(f, "actual_off");
        const String onStr     = safeStr(f, "actual_on");
        const String schOffStr = safeStr(f, "scheduled_off");
        s_cachedActualOff    = offStr.length()    > 0 ? parseISO8601(offStr)    : 0;
        s_cachedActualOn     = onStr.length()     > 0 ? parseISO8601(onStr)     : 0;
        s_cachedScheduledOff = schOffStr.length() > 0 ? parseISO8601(schOffStr) : 0;
    }
    if (s_cachedActualOn > 0) s_cachedStatus = "Landed";

    s_cachedOriginCode = "";
    s_cachedDestCode   = "";
    s_cachedDestName   = "";
    s_cachedOriginLat  = NAN; s_cachedOriginLon = NAN;
    s_cachedDestLat    = NAN; s_cachedDestLon   = NAN;
    s_cachedOriginCity = ""; s_cachedOriginRegion = "";
    s_cachedDestCity   = ""; s_cachedDestRegion   = "";

    if (!f["origin"].isNull()) {
        JsonObject o = f["origin"].as<JsonObject>();
        if (!o["code_iata"].isNull())      s_cachedOriginCode = o["code_iata"].as<const char *>();
        else if (!o["code_lid"].isNull())  s_cachedOriginCode = o["code_lid"].as<const char *>();
        else if (!o["code_icao"].isNull()) s_cachedOriginCode = o["code_icao"].as<const char *>();
        extractAirportLatLon(o, s_cachedOriginLat, s_cachedOriginLon);

        if (!o["city"].isNull())        s_cachedOriginCity = o["city"].as<const char *>();
        else if (!o["name"].isNull())   s_cachedOriginCity = o["name"].as<const char *>();
        if (!o["state"].isNull()) {
            const char *abbr = usStateAbbrev(o["state"].as<const char *>());
            s_cachedOriginRegion = abbr ? String(abbr) : safeStr(o, "state");
        } else if (!o["country_code"].isNull()) {
            s_cachedOriginRegion = o["country_code"].as<const char *>();
            s_cachedOriginRegion.toUpperCase();
        }
    }

    if (!f["destination"].isNull()) {
        JsonObject d = f["destination"].as<JsonObject>();
        if (!d["code_iata"].isNull())      s_cachedDestCode = d["code_iata"].as<const char *>();
        else if (!d["code_lid"].isNull())  s_cachedDestCode = d["code_lid"].as<const char *>();
        else if (!d["code_icao"].isNull()) s_cachedDestCode = d["code_icao"].as<const char *>();
        extractAirportLatLon(d, s_cachedDestLat, s_cachedDestLon);

        if (!d["city"].isNull())        s_cachedDestCity = d["city"].as<const char *>();
        else if (!d["name"].isNull())   s_cachedDestCity = d["name"].as<const char *>();
        if (!d["state"].isNull()) {
            const char *abbr = usStateAbbrev(d["state"].as<const char *>());
            s_cachedDestRegion = abbr ? String(abbr) : safeStr(d, "state");
        } else if (!d["country_code"].isNull()) {
            s_cachedDestRegion = d["country_code"].as<const char *>();
            s_cachedDestRegion.toUpperCase();
        }
    }

    // Resolve destination airport display name (three tiers, first hit wins).
    s_cachedDestName = "";
    if (s_cachedDestCode.length() > 0) {
        // 1. Compile-time override — wins unconditionally, no API call.
        for (int i = 0; i < AirportNameConfiguration::kOverrideCount; ++i) {
            if (s_cachedDestCode == AirportNameConfiguration::kOverrides[i].code) {
                s_cachedDestName = AirportNameConfiguration::kOverrides[i].name;
                Serial.print(F("TailTracker: airport override -> "));
                Serial.println(s_cachedDestName);
                break;
            }
        }
        // 2. Persistent cache — no API call.
        if (s_cachedDestName.length() == 0) {
            AirportCacheEntry ace;
            if (AirportNameCache::findByCode(s_cachedDestCode.c_str(), ace))
                s_cachedDestName = ace.name;
        }
        // 3. AeroAPI GET /airports/{code} ($0.015, result stored for future use).
        if (s_cachedDestName.length() == 0) {
            String apName;
            double apLat = NAN, apLon = NAN;
            if (fetchAirportInfoForCache(s_cachedDestCode, apName, apLat, apLon)) {
                apName = AirportNameCache::abbreviateName(apName);
                // Promote coords if the /flights response omitted them.
                if (!hasPlausibleLatLon(s_cachedDestLat, s_cachedDestLon)
                        && hasPlausibleLatLon(apLat, apLon)) {
                    s_cachedDestLat = apLat;
                    s_cachedDestLon = apLon;
                }
                const float storeLat = (float)(hasPlausibleLatLon(s_cachedDestLat, s_cachedDestLon)
                                               ? s_cachedDestLat : apLat);
                const float storeLon = (float)(hasPlausibleLatLon(s_cachedDestLat, s_cachedDestLon)
                                               ? s_cachedDestLon : apLon);
                AirportNameCache::store(s_cachedDestCode.c_str(), apName.c_str(),
                                        storeLat, storeLon);
                s_cachedDestName = apName;
                Serial.print(F("TailTracker: cached airport name '"));
                Serial.print(apName);
                Serial.print(F("' for "));
                Serial.println(s_cachedDestCode);
            }
        }
    }

    s_cachedFetchEpoch  = nowEpoch;
    s_cachedFetchMillis = millis();
    s_routeValid        = true;

    Serial.print(F("TailTrackerFetcher: route cached origin="));
    Serial.print(s_cachedOriginCode);
    Serial.print(F(" dest="));
    Serial.print(s_cachedDestCode);
    Serial.print(F(" status="));
    Serial.println(s_cachedStatus);

    // If airport lat/lon missing, forward-geocode from city/code so progress
    // computation has real coordinates.
    if (!hasPlausibleLatLon(s_cachedOriginLat, s_cachedOriginLon)) {
        String q = s_cachedOriginCity.length() ? s_cachedOriginCity : s_cachedOriginCode;
        if (s_cachedOriginCity.length() && s_cachedOriginRegion.length())
            q += ", " + s_cachedOriginRegion;
        double la, lo;
        if (q.length() && fetchForwardGeocodeForDestination(q, la, lo)) {
            s_cachedOriginLat = la; s_cachedOriginLon = lo;
        }
    }
    if (!hasPlausibleLatLon(s_cachedDestLat, s_cachedDestLon)) {
        String q = s_cachedDestCity.length() ? s_cachedDestCity : s_cachedDestCode;
        if (s_cachedDestCity.length() && s_cachedDestRegion.length())
            q += ", " + s_cachedDestRegion;
        double la, lo;
        if (q.length() && fetchForwardGeocodeForDestination(q, la, lo)) {
            s_cachedDestLat = la; s_cachedDestLon = lo;
        }
    }

    ++s_costRouteCalls;
    return true;
}

// ---------------------------------------------------------------------------
// fetchPositionFromAeroAPI — lightweight position-only AeroAPI call
// ---------------------------------------------------------------------------

bool TailTrackerFetcher::fetchPositionFromAeroAPI(const String &faFlightId)
{
    if (strlen(APIConfiguration::AEROAPI_KEY) == 0 || faFlightId.length() == 0)
        return false;
    if (isAeroApiBudgetExhausted()) {
        Serial.println(F("TailTracker: AeroAPI daily budget exhausted — skipping /flights/{id}/pos"));
        return false;
    }

    const String url = String(APIConfiguration::AEROAPI_BASE_URL)
                     + "/flights/" + faFlightId + "/position";
    bool https = true;
    String host;
    uint16_t port = 443;
    String path;
    if (!parseUrl(url, https, host, port, path)) {
        Serial.println(F("TailTrackerFetcher: Failed to parse AeroAPI position URL"));
        return false;
    }
#if defined(FLIGHTWALL_SKIP_TLS)
    https = false; port = 80;
#else
    if (!https) { Serial.println(F("TailTrackerFetcher: Refusing non-HTTPS URL")); return false; }
#endif

    int    code = -1;
    String payload;

#if defined(ARDUINO_ARCH_ESP32)
    {
        TailTlsClient net;
#if !defined(FLIGHTWALL_SKIP_TLS)
        if (APIConfiguration::AEROAPI_INSECURE_TLS) net.setInsecure();
#endif
        HttpClient http(net, host.c_str(), port);
        http.setHttpResponseTimeout(30000);
        http.beginRequest();
        http.get(path);
        http.sendHeader("x-apikey", APIConfiguration::AEROAPI_KEY);
        http.sendHeader("Accept",   "application/json");
        http.endRequest();
        code    = http.responseStatusCode();
        payload = http.responseBody();
    }
#else
    {
        const String hdrs = String("x-apikey: ") + APIConfiguration::AEROAPI_KEY
                          + "\r\nAccept: application/json\r\n";
        if (!wifiClientRequest("GET", host, port, path, hdrs, "", code, payload)) {
            Serial.println(F("TailTrackerFetcher: AeroAPI /position request failed"));
            s_lastAeroApiPosFetchMs = millis(); // advance throttle even on transport failure
            return false;
        }
    }
#endif

    s_lastAeroApiPosFetchMs = millis(); // advance throttle regardless of HTTP outcome

    if (code != 200) {
        Serial.print(F("TailTracker: AeroAPI /position HTTP "));
        Serial.println(code);
        flightwallStringDrop(payload);
        return false;
    }

    JsonDocument filter;
    filter["last_position"]["latitude"]    = true;
    filter["last_position"]["longitude"]   = true;
    filter["last_position"]["altitude"]    = true;
    filter["last_position"]["groundspeed"] = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload,
                                               DeserializationOption::Filter(filter));
    flightwallStringDrop(payload);
    if (err) {
        Serial.print(F("TailTrackerFetcher: /position JSON error: "));
        Serial.println(err.c_str());
        return false;
    }

    if (doc["last_position"].isNull()) {
        Serial.println(F("TailTracker: AeroAPI /position: no last_position in response"));
        return false;
    }
    JsonObject lp = doc["last_position"].as<JsonObject>();

    double lpLat = NAN, lpLon = NAN;
    if (!lp["latitude"].isNull())  lpLat = lp["latitude"].as<double>();
    if (!lp["longitude"].isNull()) lpLon = lp["longitude"].as<double>();
    if (!hasPlausibleLatLon(lpLat, lpLon)) {
        Serial.println(F("TailTracker: AeroAPI /position: implausible coordinates"));
        return false;
    }

    s_aeroApiLat      = lpLat;
    s_aeroApiLon      = lpLon;
    // altitude in AeroAPI last_position is in flight levels (hundreds of feet)
    s_aeroApiAltFt    = lp["altitude"].isNull()    ? 0  : lp["altitude"].as<int>() * 100;
    s_aeroApiSpeedKt  = lp["groundspeed"].isNull() ? -1 : lp["groundspeed"].as<int>();
    s_aeroApiPosValid = true;
    ++s_costPosCalls;

    Serial.print(F("TailTracker: AeroAPI pos fallback "));
    Serial.print(lpLat, 4);
    Serial.print(',');
    Serial.print(lpLon, 4);
    Serial.print(F(" alt="));
    Serial.print(s_aeroApiAltFt);
    Serial.print(F("ft spd="));
    Serial.print(s_aeroApiSpeedKt);
    Serial.println(F("kt"));
    return true;
}

// ---------------------------------------------------------------------------
// fetchStatus — main public entry point
// ---------------------------------------------------------------------------

bool TailTrackerFetcher::fetchStatus(const String &ident, TailFlightStatus &out,
                                     const String &icao24Override)
{
    const unsigned long nowMs = millis();

    // --- Reset when the tracked ident changes ---
    if (ident != s_trackedIdent) {
        s_trackedIdent      = ident;
        s_routeValid        = false;
        s_routeNeedsRefresh = true;
        s_routeFetchMs      = 0;
        s_cachedIcao24      = "";
        s_stickyLat              = NAN;
        s_stickyLon              = NAN;
        s_stickyAlt              = 0;
        s_wasAirborne            = false;
        s_cachedFaFlightId       = "";
        s_aeroApiLat             = NAN;
        s_aeroApiLon             = NAN;
        s_aeroApiAltFt           = 0;
        s_aeroApiSpeedKt         = -1;
        s_aeroApiPosValid        = false;
        s_lastAeroApiPosFetchMs  = 0;
        s_inferLandCount         = 0;
        s_inferFlyCount          = 0;
        s_telemetryOnGround      = false;
        Serial.print(F("TailTracker: tracking new ident "));
        Serial.println(ident);
    }

    // --- Resolve ICAO24 ---
    // Explicit override (from config) takes priority over the N-number formula.
    // The formula works for most US registrations but the FAA does not strictly
    // follow it for all aircraft (e.g. older or re-registered tail numbers).
    if (icao24Override.length() > 0) {
        if (s_cachedIcao24 != icao24Override) {
            s_cachedIcao24 = icao24Override;
            Serial.print(F("TailTracker: ICAO24 override="));
            Serial.println(s_cachedIcao24);
        }
    } else if (s_cachedIcao24.length() == 0) {
        char hex[7];
        if (nNumberToIcao24(ident, hex)) {
            s_cachedIcao24 = String(hex);
            Serial.print(F("TailTracker: ICAO24 (formula)="));
            Serial.println(s_cachedIcao24);
        }
    }

    // --- Refresh route from AeroAPI if needed ---
    const bool routeExpired  = (s_routeFetchMs == 0 ||
                                nowMs - s_routeFetchMs >= ROUTE_REFRESH_INTERVAL_MS);
    const bool needsRefresh  = !s_routeValid || s_routeNeedsRefresh || routeExpired;

    if (needsRefresh) {
        Serial.println(F("TailTracker: fetching route from AeroAPI"));
        if (!fetchRouteFromAeroAPI(ident)) {
            if (!s_routeValid) {
                Serial.println(F("TailTracker: No cached route, cannot display"));
                return false;
            }
            Serial.println(F("TailTracker: AeroAPI failed — using cached route"));
        } else {
            s_routeFetchMs      = nowMs;
            s_routeNeedsRefresh = false;
        }
    }

    // --- Build result from cached route ---
    TailFlightStatus result;
    result.ident            = s_cachedIdent;
    result.status           = s_cachedStatus;
    result.origin_code      = s_cachedOriginCode;
    result.dest_code        = s_cachedDestCode;
    result.dest_name        = s_cachedDestName;
    result.origin_lat       = s_cachedOriginLat;
    result.origin_lon       = s_cachedOriginLon;
    result.dest_lat         = s_cachedDestLat;
    result.dest_lon         = s_cachedDestLon;
    result.actual_off_epoch    = s_cachedActualOff;
    result.actual_on_epoch     = s_cachedActualOn;
    result.scheduled_off_epoch = s_cachedScheduledOff;
    result.fetch_epoch      = s_cachedFetchEpoch;
    result.fetch_millis     = s_cachedFetchMillis;
    result.progress_percent = 0;

    // --- Try OpenSky for current position ---
    bool gotPosition = false;

    if (s_cachedIcao24.length() > 0 && _openSky != nullptr) {
        StateVector sv;
        if (_openSky->fetchByIcao24(s_cachedIcao24, sv)
                && hasPlausibleLatLon(sv.lat, sv.lon)) {

            result.lat            = sv.lat;
            result.lon            = sv.lon;
            result.altitude_ft    = isnan(sv.baro_altitude) ? 0
                                  : (int)(sv.baro_altitude * 3.28084f);
            gotPosition           = true;
            result.positionSource = TailPositionSource::OpenSky;

            // Compute geometric progress from cached origin/dest coordinates.
            if (hasPlausibleLatLon(s_cachedOriginLat, s_cachedOriginLon) &&
                hasPlausibleLatLon(s_cachedDestLat,   s_cachedDestLon))
            {
                const double total = haversineKm(s_cachedOriginLat, s_cachedOriginLon,
                                                 s_cachedDestLat,   s_cachedDestLon);
                if (total > 10.0) {
                    const double traveled = haversineKm(s_cachedOriginLat, s_cachedOriginLon,
                                                        sv.lat, sv.lon);
                    int prog = (int)lround((traveled * 100.0) / total);
                    if (prog < 0)   prog = 0;
                    if (prog > 100) prog = 100;
                    result.progress_percent = prog;
                }
            }

            // Infer flight status from on_ground flag and airborne history.
            const bool wasAirborne = s_wasAirborne;
            if (!sv.on_ground) {
                result.status    = "Flying";
                if (!wasAirborne) {
                    // Ground → air transition: refresh route to get the new flight leg.
                    s_routeNeedsRefresh = true;
                    s_cachedActualOn    = 0;
                    Serial.println(F("TailTracker: takeoff detected via OpenSky"));
                }
                s_wasAirborne    = true;
                result.actual_on_epoch = 0; // not yet landed
            } else {
                if (wasAirborne) {
                    // Transition: just landed → fetch next leg from AeroAPI.
                    result.status       = "Landed";
                    s_wasAirborne       = false;
                    s_routeNeedsRefresh = true;
                    Serial.println(F("TailTracker: landing detected via OpenSky"));
                } else {
                    result.status = (s_cachedActualOn > 0) ? "Landed" : "On Ground";
                }
            }
            if (result.progress_percent == 100 && sv.on_ground)
                result.status = "Landed";

            // Telemetry inference overrides when primary sources are ambiguous.
            if (s_telemetryOnGround && result.status == "Flying") {
                result.status  = "Landed";
                s_cachedStatus = "Landed";
                if (!s_routeNeedsRefresh) {
                    s_routeNeedsRefresh = true;
                    Serial.println(F("TailTracker: telemetry inference triggered route refresh"));
                }
            } else if (!s_telemetryOnGround && result.status == "Landed"
                       && result.actual_on_epoch == 0) {
                result.status = "Flying";
                s_wasAirborne = true;
                if (!s_routeNeedsRefresh) {
                    s_routeNeedsRefresh = true;
                    s_cachedActualOn    = 0;
                    Serial.println(F("TailTracker: takeoff inferred via telemetry — route refresh triggered"));
                }
            }

            // Update sticky position.
            s_stickyLat = sv.lat;
            s_stickyLon = sv.lon;
            s_stickyAlt = result.altitude_ft;

            // Update telemetry inference with OpenSky speed (m/s → kt).
            {
                const int altFt   = isnan(sv.baro_altitude) ? -1
                                  : (int)(sv.baro_altitude * 3.28084f);
                const int speedKt = isnan(sv.velocity) ? -1
                                  : (int)(sv.velocity * 1.944f);
                updateTelemetryInference(altFt, speedKt);
            }

            // Reverse-geocode current location.
            fetchReverseGeocode(sv.lat, sv.lon, result.city, result.region);

        } else {
            Serial.print(F("TailTracker: not found on OpenSky (icao24="));
            Serial.print(s_cachedIcao24);
            Serial.println(')');
        }
    } else if (s_cachedIcao24.length() == 0) {
        Serial.println(F("TailTracker: ICAO24 unknown — OpenSky position unavailable"));
    }

    // --- No OpenSky position: try AeroAPI fallback, then sticky ---
    if (!gotPosition) {
        const bool definitivelyLanded = (s_cachedStatus == "Landed");
        const bool believedAirborne   = !definitivelyLanded && (s_wasAirborne || s_cachedStatus == "Flying");
        const bool openSkyThrottled   = !definitivelyLanded && OpenSkyFetcher::isRateLimited();
        const bool fallbackAllowed    = believedAirborne || openSkyThrottled;
        const unsigned long fallbackInterval = believedAirborne
            ? TailTrackerConfiguration::AEROAPI_POSITION_FALLBACK_INTERVAL_MS
            : TailTrackerConfiguration::AEROAPI_GROUND_FALLBACK_INTERVAL_MS;
        const bool fallbackDue = fallbackAllowed && s_cachedFaFlightId.length() > 0 &&
            (s_lastAeroApiPosFetchMs == 0 ||
             nowMs - s_lastAeroApiPosFetchMs >= fallbackInterval);

        if (fallbackDue) {
            Serial.println(F("TailTracker: OpenSky miss — trying AeroAPI position fallback"));
            fetchPositionFromAeroAPI(s_cachedFaFlightId);
        }

        // Position priority: AeroAPI last_position > sticky (last good OpenSky reading).
        bool usingAeroApi = false;
        if (s_aeroApiPosValid && hasPlausibleLatLon(s_aeroApiLat, s_aeroApiLon)) {
            result.lat            = s_aeroApiLat;
            result.lon            = s_aeroApiLon;
            result.altitude_ft    = s_aeroApiAltFt;
            result.positionSource = TailPositionSource::AeroApi;
            usingAeroApi          = true;
            updateTelemetryInference(s_aeroApiAltFt, s_aeroApiSpeedKt);
        } else if (hasPlausibleLatLon(s_stickyLat, s_stickyLon)) {
            result.lat            = s_stickyLat;
            result.lon            = s_stickyLon;
            result.altitude_ft    = s_stickyAlt;
            result.positionSource = TailPositionSource::Sticky;
        }

        // Compute geometric progress from whichever position we have.
        if (hasPlausibleLatLon(result.lat, result.lon) &&
            hasPlausibleLatLon(s_cachedOriginLat, s_cachedOriginLon) &&
            hasPlausibleLatLon(s_cachedDestLat,   s_cachedDestLon))
        {
            const double total = haversineKm(s_cachedOriginLat, s_cachedOriginLon,
                                             s_cachedDestLat,   s_cachedDestLon);
            if (total > 10.0) {
                const double traveled = haversineKm(s_cachedOriginLat, s_cachedOriginLon,
                                                    result.lat, result.lon);
                int prog = (int)lround((traveled * 100.0) / total);
                if (prog < 0)   prog = 0;
                if (prog > 100) prog = 100;
                result.progress_percent = prog;
            }
        }

        // Line 3 city/region: prefer Nominatim reverse-geocode of airport coordinates
        // when landed; fall back to AeroAPI-supplied strings only if coordinates unknown.
        if (usingAeroApi) {
            fetchReverseGeocode(s_aeroApiLat, s_aeroApiLon, result.city, result.region);
        } else if (result.actual_on_epoch > 0 || result.status == "Landed") {
            if (hasPlausibleLatLon(s_cachedDestLat, s_cachedDestLon))
                fetchReverseGeocode(s_cachedDestLat, s_cachedDestLon, result.city, result.region);
            else {
                result.city   = s_cachedDestCity;
                result.region = s_cachedDestRegion;
            }
        } else if (result.actual_off_epoch == 0) {
            result.city   = s_cachedOriginCity;
            result.region = s_cachedOriginRegion;
        }
    }

    // Apply telemetry inference to status when OpenSky position is unavailable.
    if (s_telemetryOnGround && result.status == "Flying") {
        result.status  = "Landed";
        s_cachedStatus = "Landed";
        s_wasAirborne  = false;
        if (!s_routeNeedsRefresh) {
            s_routeNeedsRefresh = true;
            Serial.println(F("TailTracker: telemetry inference triggered route refresh (no OpenSky)"));
        }
    }

    // Force progress to 100% in all landed states.
    if (result.actual_on_epoch > 0 || result.status == "Landed")
        result.progress_percent = 100;

    // If landed with no known airport name, search persistent cache by position.
    if (result.status == "Landed" && result.dest_name.length() == 0
            && hasPlausibleLatLon(result.lat, result.lon)) {
        AirportCacheEntry ace;
        if (AirportNameCache::findNearest(result.lat, result.lon, 5.0, ace))
            result.dest_name = ace.name;
    }

    result.valid = true;
    out = result;
    return true;
}

// ---------------------------------------------------------------------------
// Forward geocoding via Nominatim /search
// ---------------------------------------------------------------------------

bool TailTrackerFetcher::fetchForwardGeocodeForDestination(const String &searchQuery,
                                                            double &outLat, double &outLon)
{
    if (searchQuery.length() == 0)
        return false;

    if (searchQuery == _lastForwardQuery
            && !isnan(_lastForwardLat) && !isnan(_lastForwardLon)) {
        outLat = _lastForwardLat;
        outLon = _lastForwardLon;
        return true;
    }

    String path = String("/search?format=json&limit=1&q=");
    appendUrlQueryEncoded(searchQuery, path);
    if (path.length() > 512) return false;

    const String   host = "nominatim.openstreetmap.org";
    const uint16_t port = 443;
    int    code = -1;
    String payload;

#if defined(ARDUINO_ARCH_ESP32)
    {
        TailTlsClient net;
#if !defined(FLIGHTWALL_SKIP_TLS)
        net.setInsecure();
#endif
        HttpClient http(net, host.c_str(), port);
        http.setHttpResponseTimeout(15000);
        http.beginRequest();
        http.get(path);
        http.sendHeader("User-Agent", "FlightWallFirmware/1.0");
        http.sendHeader("Accept",     "application/json");
        http.endRequest();
        code    = http.responseStatusCode();
        payload = http.responseBody();
    }
#else
    {
        const String hdrs = "User-Agent: FlightWallFirmware/1.0\r\nAccept: application/json\r\n";
        if (!wifiClientRequest("GET", host, port, path, hdrs, "", code, payload)) {
            Serial.println(F("TailTrackerFetcher: Nominatim /search failed"));
            return false;
        }
    }
#endif

    if (code != 200) {
        Serial.print(F("TailTrackerFetcher: Nominatim /search HTTP "));
        Serial.println(code);
        return false;
    }

    JsonDocument searchFilter;
    searchFilter[0]["lat"] = true;
    searchFilter[0]["lon"] = true;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload,
                                               DeserializationOption::Filter(searchFilter));
    flightwallStringDrop(payload);
    if (err) return false;

    JsonArray hits = doc.as<JsonArray>();
    if (hits.isNull() || hits.size() == 0) { doc.clear(); return false; }
    JsonObject first = hits[0].as<JsonObject>();
    if (first["lat"].isNull() || first["lon"].isNull()) { doc.clear(); return false; }
    outLat = first["lat"].as<double>();
    outLon = first["lon"].as<double>();
    doc.clear();
    if (isnan(outLat) || isnan(outLon)) return false;
    _lastForwardQuery = searchQuery;
    _lastForwardLat   = outLat;
    _lastForwardLon   = outLon;
    return true;
}

// ---------------------------------------------------------------------------
// Reverse geocoding via Nominatim
// ---------------------------------------------------------------------------

bool TailTrackerFetcher::fetchReverseGeocode(double lat, double lon,
                                              String &outCity, String &outRegion)
{
    if (!isnan(_lastGeoLat) && !isnan(_lastGeoLon)) {
        if (haversineKm(_lastGeoLat, _lastGeoLon, lat, lon)
                < TailTrackerConfiguration::GEO_CACHE_THRESHOLD_KM) {
            outCity   = _lastCity;
            outRegion = _lastRegion;
            return true;
        }
    }

    const String   host = "nominatim.openstreetmap.org";
    const uint16_t port = 443;
    char pathBuf[88];
    snprintf(pathBuf, sizeof(pathBuf),
             "/reverse?format=json&lat=%.4f&lon=%.4f&zoom=10", lat, lon);
    const String path = pathBuf;

    int    code = -1;
    String payload;

#if defined(ARDUINO_ARCH_ESP32)
    {
        TailTlsClient net;
#if !defined(FLIGHTWALL_SKIP_TLS)
        net.setInsecure();
#endif
        HttpClient http(net, host.c_str(), port);
        http.setHttpResponseTimeout(15000);
        http.beginRequest();
        http.get(path);
        http.sendHeader("User-Agent", "FlightWallFirmware/1.0");
        http.sendHeader("Accept",     "application/json");
        http.endRequest();
        code    = http.responseStatusCode();
        payload = http.responseBody();
    }
#else
    {
        const String hdrs = "User-Agent: FlightWallFirmware/1.0\r\nAccept: application/json\r\n";
        if (!wifiClientRequest("GET", host, port, path, hdrs, "", code, payload)) {
            Serial.println(F("TailTrackerFetcher: Nominatim reverse failed"));
            return false;
        }
    }
#endif

    if (code != 200) {
        Serial.print(F("TailTrackerFetcher: Nominatim reverse HTTP "));
        Serial.println(code);
        return false;
    }

    JsonDocument geo;
    DeserializationError err = deserializeJson(geo, payload);
    flightwallStringDrop(payload);
    if (err) return false;

    JsonObject addr = geo["address"].as<JsonObject>();
    String city;
    if      (!addr["city"].isNull())        city = addr["city"].as<const char *>();
    else if (!addr["town"].isNull())         city = addr["town"].as<const char *>();
    else if (!addr["village"].isNull())      city = addr["village"].as<const char *>();
    else if (!addr["municipality"].isNull()) city = addr["municipality"].as<const char *>();
    else if (!addr["county"].isNull())       city = addr["county"].as<const char *>();

    String cc = addr["country_code"].isNull() ? String("")
              : String(addr["country_code"].as<const char *>());
    cc.toUpperCase();

    String region;
    if (cc == "US") {
        String state = addr["state"].isNull() ? String("")
                     : String(addr["state"].as<const char *>());
        const char *abbr = usStateAbbrev(state.c_str());
        region = abbr ? String(abbr) : cc;
    } else {
        region = cc;
    }

    outCity   = city;
    outRegion = region;
    _lastGeoLat = lat; _lastGeoLon = lon;
    _lastCity   = city; _lastRegion = region;
    geo.clear();
    return true;
}

// ---------------------------------------------------------------------------
// US state name → 2-letter abbreviation
// ---------------------------------------------------------------------------

const char *TailTrackerFetcher::usStateAbbrev(const char *fullName)
{
    static const struct { const char *full; const char *abbr; } kTable[] = {
        {"Alabama","AL"},         {"Alaska","AK"},          {"Arizona","AZ"},
        {"Arkansas","AR"},        {"California","CA"},       {"Colorado","CO"},
        {"Connecticut","CT"},     {"Delaware","DE"},         {"Florida","FL"},
        {"Georgia","GA"},         {"Hawaii","HI"},           {"Idaho","ID"},
        {"Illinois","IL"},        {"Indiana","IN"},           {"Iowa","IA"},
        {"Kansas","KS"},          {"Kentucky","KY"},         {"Louisiana","LA"},
        {"Maine","ME"},           {"Maryland","MD"},         {"Massachusetts","MA"},
        {"Michigan","MI"},        {"Minnesota","MN"},        {"Mississippi","MS"},
        {"Missouri","MO"},        {"Montana","MT"},          {"Nebraska","NE"},
        {"Nevada","NV"},          {"New Hampshire","NH"},    {"New Jersey","NJ"},
        {"New Mexico","NM"},      {"New York","NY"},         {"North Carolina","NC"},
        {"North Dakota","ND"},    {"Ohio","OH"},             {"Oklahoma","OK"},
        {"Oregon","OR"},          {"Pennsylvania","PA"},     {"Rhode Island","RI"},
        {"South Carolina","SC"},  {"South Dakota","SD"},     {"Tennessee","TN"},
        {"Texas","TX"},           {"Utah","UT"},             {"Vermont","VT"},
        {"Virginia","VA"},        {"Washington","WA"},       {"West Virginia","WV"},
        {"Wisconsin","WI"},       {"Wyoming","WY"},
    };
    for (const auto &e : kTable) {
        if (strcmp(fullName, e.full) == 0) return e.abbr;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// resetCostWindow — called from main.cpp at Eastern midnight (tally reset)
// ---------------------------------------------------------------------------

void TailTrackerFetcher::resetCostWindow(unsigned long /*newEasternEpochDay*/)
{
    s_prevCostRouteCalls   = s_costRouteCalls;
    s_prevCostPosCalls     = s_costPosCalls;
    s_prevCostAirportCalls = s_costAirportCalls;
    s_costRouteCalls       = 0;
    s_costPosCalls         = 0;
    s_costAirportCalls     = 0;
    Serial.println(F("TailTracker: AeroAPI cost window reset (new Eastern day)"));
}

// ---------------------------------------------------------------------------
// printPollSummary — structured serial status block
// ---------------------------------------------------------------------------

static void printAeroApiCostBlock(const char *label,
                                  uint16_t routeCalls, uint16_t posCalls, uint16_t airportCalls)
{
    const uint32_t totalMd = (uint32_t)routeCalls   *  5u
                           + (uint32_t)posCalls     * 10u
                           + (uint32_t)airportCalls * 15u;
    char cbuf[8];
    Serial.print(F(" AEROAPI SPEND  ")); Serial.println(label);
    if (routeCalls > 0) {
        Serial.print(F("   /flights/{ident}   "));
        Serial.print(routeCalls); Serial.print(F(" calls  x $0.005 = $"));
        snprintf(cbuf, sizeof(cbuf), "%u.%03u", (routeCalls*5u)/1000u, (routeCalls*5u)%1000u);
        Serial.println(cbuf);
    }
    if (posCalls > 0) {
        Serial.print(F("   /flights/{id}/pos  "));
        Serial.print(posCalls); Serial.print(F(" calls  x $0.010 = $"));
        snprintf(cbuf, sizeof(cbuf), "%u.%03u", (posCalls*10u)/1000u, (posCalls*10u)%1000u);
        Serial.println(cbuf);
    }
    if (airportCalls > 0) {
        Serial.print(F("   /airports/{code}   "));
        Serial.print(airportCalls); Serial.print(F(" calls  x $0.015 = $"));
        snprintf(cbuf, sizeof(cbuf), "%u.%03u", (airportCalls*15u)/1000u, (airportCalls*15u)%1000u);
        Serial.println(cbuf);
    }
    if (routeCalls == 0 && posCalls == 0 && airportCalls == 0)
        Serial.println(F("   (no calls)"));
    Serial.print(F("                          TOTAL           $"));
    snprintf(cbuf, sizeof(cbuf), "%u.%03u", (unsigned)(totalMd/1000u), (unsigned)(totalMd%1000u));
    Serial.println(cbuf);
    const uint32_t budget = APIConfiguration::AEROAPI_DAILY_BUDGET_MILLIDOLLARS;
    if (totalMd >= budget) {
        Serial.println(F("                          BUDGET  EXHAUSTED"));
    } else {
        const uint32_t rem = budget - totalMd;
        snprintf(cbuf, sizeof(cbuf), "%u.%03u", (unsigned)(rem / 1000u), (unsigned)(rem % 1000u));
        Serial.print(F("                          BUDGET  $"));
        Serial.print(cbuf); Serial.println(F(" remaining"));
    }
}

void TailTrackerFetcher::printPollSummary(const TailFlightStatus &st,
                                          uint16_t tallyCount,
                                          bool aeroApiCalledThisPoll)
{
    ++s_pollCount;
    const bool rlActive = OpenSkyFetcher::isRateLimited();
    const int  rlRemain = OpenSkyFetcher::getRateLimitRemaining();

    Serial.println(F("============================================================"));
    Serial.print(F(" TailTracker Poll #")); Serial.print(s_pollCount);
    if (st.fetch_epoch > 0) {
        const uint8_t hh = (st.fetch_epoch % 86400UL) / 3600UL;
        const uint8_t mm = (st.fetch_epoch % 3600UL)  / 60UL;
        const uint8_t ss =  st.fetch_epoch % 60UL;
        char tbuf[16];
        snprintf(tbuf, sizeof(tbuf), "  %02u:%02u:%02u UTC", hh, mm, ss);
        Serial.print(tbuf);
    }
    Serial.println();
    Serial.println(F("============================================================"));

    // Aircraft block
    Serial.print(F(" AIRCRAFT  ")); Serial.print(st.ident);
    if (s_cachedFaFlightId.length() > 0) {
        Serial.print(F("   fa=")); Serial.print(s_cachedFaFlightId);
    }
    if (s_cachedIcao24.length() > 0) {
        Serial.print(F("   ICAO24: ")); Serial.print(s_cachedIcao24);
    }
    Serial.println();

    Serial.print(F(" STATUS    ")); Serial.print(st.status);
    Serial.print(F("   TALLY: ")); Serial.print(tallyCount); Serial.println(F(" today"));

    if (st.origin_code.length() > 0 && st.dest_code.length() > 0) {
        Serial.print(F(" ROUTE     ")); Serial.print(st.origin_code);
        Serial.print(F(" -> ")); Serial.print(st.dest_code);
        Serial.print(F("   ")); Serial.print(st.progress_percent); Serial.println(F("% complete"));
    }

    if (!isnan(st.lat) && !isnan(st.lon)) {
        char posBuf[36];
        snprintf(posBuf, sizeof(posBuf), " POSITION  %.4f, %.4f",
                 (double)st.lat, (double)st.lon);
        Serial.println(posBuf);
    }

    if (st.altitude_ft > 0 || st.positionSource != TailPositionSource::None) {
        Serial.print(F(" ALT/SPD   "));
        Serial.print(st.altitude_ft); Serial.print(F(" ft  /  "));
        if (s_aeroApiSpeedKt >= 0 && st.positionSource == TailPositionSource::AeroApi)
            { Serial.print(s_aeroApiSpeedKt); Serial.println(F(" kt")); }
        else
            Serial.println(F("-- kt"));
    }

    if (st.city.length() > 0) {
        Serial.print(F(" LOCATION  ")); Serial.print(st.city);
        if (st.region.length() > 0) { Serial.print(F(" ")); Serial.print(st.region); }
        Serial.println();
    }

    // Data sources
    Serial.println(F("------------------------------------------------------------"));
    Serial.println(F(" DATA SOURCES"));
    Serial.print(F("   Position : "));
    switch (st.positionSource) {
        case TailPositionSource::OpenSky:
            Serial.println(F("OpenSky (live)"));
            break;
        case TailPositionSource::AeroApi:
            if (rlActive)
                Serial.println(F("AeroAPI /position (fallback, OpenSky throttled)"));
            else
                Serial.println(F("AeroAPI /position (airborne fallback, OpenSky miss)"));
            break;
        case TailPositionSource::Sticky:
            Serial.println(F("Sticky (stale OpenSky)"));
            break;
        default:
            Serial.println(F("None"));
            break;
    }

    Serial.print(F("   Route    : AeroAPI cached "));
    if (s_routeFetchMs > 0) {
        const unsigned long ageS = (millis() - s_routeFetchMs) / 1000UL;
        char agebuf[16];
        snprintf(agebuf, sizeof(agebuf), "%uh %02um ago",
                 (unsigned)(ageS / 3600UL), (unsigned)((ageS % 3600UL) / 60UL));
        Serial.println(agebuf);
    } else {
        Serial.println(F("never"));
    }

    Serial.print(F("   Pos API  : "));
    if (aeroApiCalledThisPoll)
        Serial.println(F("called this poll"));
    else if (!rlActive && st.positionSource == TailPositionSource::OpenSky)
        Serial.println(F("not called (OpenSky OK)"));
    else
        Serial.println(F("skipped (interval)"));

    // Rate limits
    Serial.println(F("------------------------------------------------------------"));
    Serial.println(F(" RATE LIMITS"));
    Serial.print(F("   OpenSky  : "));
    if (rlActive) {
        const unsigned long resumeMs = OpenSkyFetcher::getRateLimitResumeMs();
        const unsigned long nowMs    = millis();
        const unsigned long secsLeft = (resumeMs > nowMs) ? (resumeMs - nowMs) / 1000UL : 0;
        Serial.print(F("THROTTLED — resume in "));
        Serial.print(secsLeft); Serial.println(F("s"));
    } else {
        Serial.print(F("OK"));
        if (rlRemain >= 0) {
            Serial.print(F("  (remaining: ")); Serial.print(rlRemain); Serial.print(F(" credits)"));
        }
        Serial.println();
    }
    Serial.println(F("   AeroAPI  : OK"));

    // Cost
    Serial.println(F("------------------------------------------------------------"));
    printAeroApiCostBlock("today", s_costRouteCalls, s_costPosCalls, s_costAirportCalls);
    if (s_prevCostRouteCalls > 0 || s_prevCostPosCalls > 0 || s_prevCostAirportCalls > 0)
        printAeroApiCostBlock("yesterday", s_prevCostRouteCalls, s_prevCostPosCalls, s_prevCostAirportCalls);
    Serial.println(F("============================================================"));
}
