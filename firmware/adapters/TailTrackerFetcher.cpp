/*
Purpose: Fetch real-time status for a tracked tail number.
AeroAPI is called ONCE per flight leg to obtain route metadata (origin, destination,
timestamps).  Position is maintained exclusively by the 30-second OpenSky timer in
main.cpp via updateStickyPosition().  Flight progress is computed geometrically.
Nominatim reverse-geocoding is rate-limited by a time-interval cache.
*/
#include "adapters/TailTrackerFetcher.h"

#include <ArduinoJson.h>
#include <ArduinoHttpClient.h>
#include "config/APIConfiguration.h"
#include "config/TailTrackerConfiguration.h"
#include "utils/GeoUtils.h"
#include "utils/HttpUtils.h"
#include "utils/MemoryUtils.h"
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
// flagRouteNeedsRefresh — called from main.cpp landing detection
// ---------------------------------------------------------------------------

void TailTrackerFetcher::flagRouteNeedsRefresh()
{
    s_routeNeedsRefresh = true;
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

    // Debug: dump all flights so the selection decision is visible in the serial log.
    for (size_t i = 0; i < flights.size(); ++i) {
        Serial.print(F("  flight[")); Serial.print(i); Serial.print(F("]: status="));
        Serial.print(safeStr(flights[i], "status"));
        Serial.print(F(" actual_off="));
        Serial.print(flights[i]["actual_off"].isNull() ? "(null)" : safeStr(flights[i], "actual_off").c_str());
        Serial.print(F(" actual_on="));
        Serial.println(flights[i]["actual_on"].isNull() ? "(null)" : safeStr(flights[i], "actual_on").c_str());
    }

    // Pass 1: prefer the leg currently airborne.
    // Two indicators: timestamp pair (actual_off set, actual_on absent) OR AeroAPI status
    // string ("En Route…" / "Arriving…"), which is set before actual_off is confirmed and
    // also when actual_on is pre-populated with an estimated arrival time.
    int  bestIdx       = 0;
    bool foundAirborne = false;
    for (size_t i = 0; i < flights.size(); ++i) {
        const bool offSet   = !flights[i]["actual_off"].isNull();
        const bool onAbsent =  flights[i]["actual_on"].isNull();
        const String st     = safeStr(flights[i], "status");
        const bool enRoute  = st.startsWith("En Route") || st.startsWith("Arriving");
        if ((offSet && onAbsent) || enRoute) {
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
        } else {
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
    s_cachedIdent  = safeStr(f, "ident");
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

    return true;
}

// ---------------------------------------------------------------------------
// fetchStatus — main public entry point
// ---------------------------------------------------------------------------

bool TailTrackerFetcher::fetchStatus(const String &ident, TailFlightStatus &out)
{
    const unsigned long nowMs = millis();

    // --- Reset when the tracked ident changes ---
    if (ident != s_trackedIdent) {
        s_trackedIdent      = ident;
        s_routeValid        = false;
        s_routeNeedsRefresh = true;
        s_routeFetchMs      = 0;
        s_cachedIcao24      = "";
        s_stickyLat         = NAN;
        s_stickyLon         = NAN;
        s_stickyAlt         = 0;
        Serial.print(F("TailTracker: tracking new ident "));
        Serial.println(ident);
    }

    // --- Compute ICAO24 from N-number (US aircraft; instant, no API call) ---
    if (s_cachedIcao24.length() == 0) {
        char hex[7];
        if (nNumberToIcao24(ident, hex)) {
            s_cachedIcao24 = String(hex);
            Serial.print(F("TailTracker: ICAO24="));
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

    // --- Position from sticky (maintained by the 30-second OpenSky timer in main.cpp) ---
    if (hasPlausibleLatLon(s_stickyLat, s_stickyLon))
    {
        result.lat         = s_stickyLat;
        result.lon         = s_stickyLon;
        result.altitude_ft = s_stickyAlt;

        if (hasPlausibleLatLon(s_cachedOriginLat, s_cachedOriginLon) &&
            hasPlausibleLatLon(s_cachedDestLat,   s_cachedDestLon))
        {
            const double total = haversineKm(s_cachedOriginLat, s_cachedOriginLon,
                                             s_cachedDestLat,   s_cachedDestLon);
            if (total > 10.0) {
                const double traveled = haversineKm(s_cachedOriginLat, s_cachedOriginLon,
                                                    s_stickyLat, s_stickyLon);
                int prog = (int)lround((traveled * 100.0) / total);
                if (prog < 0)   prog = 0;
                if (prog > 100) prog = 100;
                result.progress_percent = prog;
            }
        }
    }

    // City/region: landed → destination; pre-departure → origin; airborne → last geocode.
    if (result.actual_on_epoch > 0) {
        result.city   = s_cachedDestCity;
        result.region = s_cachedDestRegion;
    } else if (result.actual_off_epoch == 0) {
        result.city   = s_cachedOriginCity;
        result.region = s_cachedOriginRegion;
    } else {
        result.city   = _lastCity;
        result.region = _lastRegion;
    }

    // AeroAPI lands set progress to 100.
    if (result.actual_on_epoch > 0 && result.progress_percent == 0)
        result.progress_percent = 100;

    result.valid = true;
    out = result;
    return true;
}

// ---------------------------------------------------------------------------
// updateStickyPosition — keep the sticky cache current from OpenSky fixes
// ---------------------------------------------------------------------------

void TailTrackerFetcher::updateStickyPosition(double lat, double lon, int altFt)
{
    if (isnan(lat) || isnan(lon) || (lat == 0.0 && lon == 0.0))
        return;
    s_stickyLat = lat;
    s_stickyLon = lon;
    s_stickyAlt = altFt;
}

// ---------------------------------------------------------------------------
// GetLastTrack — fallback position when last_position is absent
// ---------------------------------------------------------------------------

bool TailTrackerFetcher::fetchTrackPosition(const String &faFlightId,
                                             double &outLat, double &outLon,
                                             int &outAlt)
{
    if (strlen(APIConfiguration::AEROAPI_KEY) == 0) return false;

    const String url = String(APIConfiguration::AEROAPI_BASE_URL)
                     + "/flights/" + faFlightId + "/track";
    bool   https = true;
    String host;
    uint16_t port = 443;
    String path;
    if (!parseUrl(url, https, host, port, path)) return false;

#if defined(FLIGHTWALL_SKIP_TLS)
    https = false;
    port  = 80;
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
        {
            Serial.println(F("TailTrackerFetcher: track request failed"));
            return false;
        }
    }
#endif

    if (code != 200)
    {
        Serial.print("TailTrackerFetcher: track HTTP ");
        Serial.println(code);
        flightwallStringDrop(payload);
        return false;
    }

    JsonDocument trackFilter;
    trackFilter["positions"][0]["latitude"]  = true;
    trackFilter["positions"][0]["longitude"] = true;
    trackFilter["positions"][0]["altitude"]  = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload,
                                               DeserializationOption::Filter(trackFilter));
    flightwallStringDrop(payload);
    if (err)
    {
        Serial.print("TailTrackerFetcher: track JSON parse error: ");
        Serial.println(err.c_str());
        return false;
    }

    JsonArray positions = doc["positions"].as<JsonArray>();
    if (positions.isNull() || positions.size() == 0)
    {
        Serial.println("TailTrackerFetcher: track positions array empty");
        doc.clear();
        return false;
    }

    // Positions are ascending in time; last entry is most recent.
    // Track altitude is in hundreds of feet (e.g. 360 = FL360 = 36,000 ft).
    double lastLat = NAN, lastLon = NAN;
    int    lastAlt = 0;
    for (JsonObject pos : positions)
    {
        if (!pos["latitude"].isNull() && !pos["longitude"].isNull())
        {
            lastLat = pos["latitude"].as<double>();
            lastLon = pos["longitude"].as<double>();
            if (!pos["altitude"].isNull())
                lastAlt = pos["altitude"].as<int>() * 100;
        }
    }

    if (isnan(lastLat) || isnan(lastLon))
    {
        doc.clear();
        return false;
    }

    outLat = lastLat;
    outLon = lastLon;
    outAlt = lastAlt;
    doc.clear();
    return true;
}

// ---------------------------------------------------------------------------
// Forward geocoding via Nominatim /search (arrival city → lat/lon for compass)
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
    // Re-query Nominatim at most once per POSITION_FETCH_INTERVAL_SECONDS*2.
    const unsigned long geocodeIntervalMs =
        TailTrackerConfiguration::POSITION_FETCH_INTERVAL_SECONDS * 2UL * 1000UL;
    if (_lastGeocodeMs != 0 && (millis() - _lastGeocodeMs) < geocodeIntervalMs)
    {
        outCity   = _lastCity;
        outRegion = _lastRegion;
        return true;
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
    _lastGeocodeMs = millis();
    _lastCity      = city;
    _lastRegion    = region;
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
