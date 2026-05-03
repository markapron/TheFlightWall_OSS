/*
Purpose: Fetch real-time status for a tracked tail number from AeroAPI and
         use Nominatim for both reverse- (aircraft) and forward- (arrival) geocodes.
Responsibilities:
- GET /flights/{ident} from AeroAPI; parse status, progress, timestamps, last position.
- GET Nominatim /reverse for city/state from live lat/lon when available.
- GET Nominatim /search to resolve arrival city/dest code to dest_lat/dest_lon when
  landed and the API omits destination airport coordinates (compass on Matrix display).
- Cache results where appropriate to limit rate to Nominatim.
- Sticky per-tail (request ident) last lat/lon/alt when a poll omits last_position;
  not cleared for new flight legs for the same tail; cleared when the configured
  tail ident changes.
*/
#include "adapters/TailTrackerFetcher.h"

#include <ArduinoJson.h>
#include <ArduinoHttpClient.h>
#include "config/APIConfiguration.h"
#include "config/TailTrackerConfiguration.h"
#include "utils/HttpUtils.h"
#include "utils/GeoUtils.h"
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
// Helpers
// ---------------------------------------------------------------------------

// Parse ISO 8601 UTC timestamps of the form "YYYY-MM-DDTHH:MM:SSZ" (or with
// a numeric timezone offset) into a Unix epoch value (seconds since 1970).
// Returns 0 on parse failure or for dates before 2020 (likely junk).
static unsigned long parseISO8601(const String &s)
{
    if (s.length() < 19) return 0;

    int year  = s.substring(0,  4).toInt();
    int month = s.substring(5,  7).toInt();
    int day   = s.substring(8,  10).toInt();
    int hour  = s.substring(11, 13).toInt();
    int mn    = s.substring(14, 16).toInt();
    int sec   = s.substring(17, 19).toInt();

    if (year < 2020 || month < 1 || month > 12 || day < 1 || day > 31) return 0;

    static const int kDim[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (year % 4 == 0) && (year % 100 != 0 || year % 400 == 0);

    // Days elapsed from 1970-01-01 to the start of `year`.
    long days = (long)(year - 1970) * 365L
              + (long)((year - 1969) / 4)
              - (long)((year - 1901) / 100)
              + (long)((year - 1601) / 400);

    for (int m = 1; m < month; ++m)
    {
        days += kDim[m];
        if (m == 2 && leap) ++days;
    }
    days += day - 1;

    return (unsigned long)(days * 86400L
                         + (long)hour * 3600L
                         + (long)mn   * 60L
                         + (long)sec);
}

static String safeStr(JsonVariant v, const char *key)
{
    if (v[key].isNull()) return String("");
    return String(v[key].as<const char *>());
}

// AeroAPI sometimes omits a real last_position and sends 0,0. That is not a valid
// aircraft fix; treat it as missing so we fall through to GetLastTrack and pick up
// city/region from the track + Nominatim path instead of geocoding the Gulf of Guinea.
static bool hasPlausibleAircraftPosition(double lat, double lon)
{
    if (isnan(lat) || isnan(lon))
        return false;
    if (lat == 0.0 && lon == 0.0)
        return false;
    return true;
}

#if !defined(ARDUINO_ARCH_ESP32)
struct TrackStreamParser
{
    double currentLat = NAN;
    double currentLon = NAN;
    int    currentAlt = 0;
    double lastLat = NAN;
    double lastLon = NAN;
    int    lastAlt = 0;

    const char *pendingKey = nullptr;
    int matchLat = 0;
    int matchLon = 0;
    int matchAlt = 0;
    bool waitingColon = false;
    bool readingValue = false;
    char valueBuf[24];
    uint8_t valueLen = 0;
};

static void trackParserCommitValue(TrackStreamParser &p)
{
    p.valueBuf[p.valueLen] = '\0';
    const double v = atof(p.valueBuf);
    if (strcmp(p.pendingKey, "lat") == 0)
        p.currentLat = v;
    else if (strcmp(p.pendingKey, "lon") == 0)
    {
        p.currentLon = v;
        if (hasPlausibleAircraftPosition(p.currentLat, p.currentLon))
        {
            p.lastLat = p.currentLat;
            p.lastLon = p.currentLon;
            p.lastAlt = p.currentAlt;
        }
    }
    else if (strcmp(p.pendingKey, "alt") == 0)
    {
        // Track endpoint altitude is in hundreds of feet.
        p.currentAlt = (int)lround(v * 100.0);
        if (hasPlausibleAircraftPosition(p.currentLat, p.currentLon))
            p.lastAlt = p.currentAlt;
    }

    p.pendingKey = nullptr;
    p.waitingColon = false;
    p.readingValue = false;
    p.valueLen = 0;
}

static void updateKeyMatch(char c, const char *pattern, int &idx, const char *key,
                           TrackStreamParser &p)
{
    if (c == pattern[idx])
    {
        idx++;
        if (pattern[idx] == '\0')
        {
            p.pendingKey = key;
            p.waitingColon = true;
            p.readingValue = false;
            p.valueLen = 0;
            idx = 0;
        }
    }
    else
    {
        idx = (c == pattern[0]) ? 1 : 0;
    }
}

static bool trackStreamOnChunk(const char *data, size_t len, void *ctx)
{
    TrackStreamParser &p = *static_cast<TrackStreamParser *>(ctx);
    static const char kLat[] = "\"latitude\"";
    static const char kLon[] = "\"longitude\"";
    static const char kAlt[] = "\"altitude\"";

    for (size_t i = 0; i < len; ++i)
    {
        const char c = data[i];

        if (p.pendingKey != nullptr)
        {
            if (p.waitingColon)
            {
                if (c == ':')
                    p.waitingColon = false;
                continue;
            }

            const bool valueChar =
                (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.';
            if (valueChar)
            {
                p.readingValue = true;
                if (p.valueLen < sizeof(p.valueBuf) - 1)
                    p.valueBuf[p.valueLen++] = c;
                continue;
            }

            if (p.readingValue)
                trackParserCommitValue(p);
            continue;
        }

        updateKeyMatch(c, kLat, p.matchLat, "lat", p);
        updateKeyMatch(c, kLon, p.matchLon, "lon", p);
        updateKeyMatch(c, kAlt, p.matchAlt, "alt", p);
    }

    return true;
}
#endif

static void extractAirportLatLon(JsonObject airportObj, double &outLat, double &outLon)
{
    // AeroAPI field shapes can vary; try common layouts.
    double lat = NAN, lon = NAN;

    auto tryGet = [](JsonVariant v, const char *key, double &out) -> bool {
        if (v.isNull() || v[key].isNull()) return false;
        out = v[key].as<double>();
        return true;
    };

    if (tryGet(airportObj, "latitude", lat))  outLat = lat;
    if (tryGet(airportObj, "longitude", lon)) outLon = lon;
    if (!isnan(outLat) && !isnan(outLon)) return;

    if (tryGet(airportObj, "lat", lat)) outLat = lat;
    if (tryGet(airportObj, "lon", lon)) outLon = lon;
    if (tryGet(airportObj, "lng", lon)) outLon = lon;
    if (!isnan(outLat) && !isnan(outLon)) return;

    if (!airportObj["position"].isNull() && airportObj["position"].is<JsonObject>())
    {
        JsonObject pos = airportObj["position"].as<JsonObject>();
        if (tryGet(pos, "lat", lat)) outLat = lat;
        if (tryGet(pos, "lon", lon)) outLon = lon;
        if (tryGet(pos, "lng", lon)) outLon = lon;
        if (tryGet(pos, "latitude", lat)) outLat = lat;
        if (tryGet(pos, "longitude", lon)) outLon = lon;
        if (!isnan(outLat) && !isnan(outLon)) return;
    }

    if (!airportObj["airport"].isNull() && airportObj["airport"].is<JsonObject>())
    {
        JsonObject inner = airportObj["airport"].as<JsonObject>();
        if (tryGet(inner, "latitude", lat))  outLat = lat;
        if (tryGet(inner, "longitude", lon)) outLon = lon;
        if (tryGet(inner, "lat", lat)) outLat = lat;
        if (tryGet(inner, "lon", lon)) outLon = lon;
        if (tryGet(inner, "lng", lon)) outLon = lon;
    }
}

// Last good aircraft fix for the current request ident (config tail). Survives
// API responses with no last_position; intentionally not reset between flight
// legs for the same tail. Reset in fetchStatus when `ident` changes.
static String  s_stickyTailKey;
static double  s_stickyLat  = NAN;
static double  s_stickyLon  = NAN;
static int     s_stickyAlt  = 0;
static unsigned long s_lastTrackFetchMs = 0;

// /track responses are commonly 30+ KB. If AeroAPI /flights omits last_position
// for several polls in a row, reuse the last track fix and refresh occasionally
// instead of allocating another large track payload every fetch cycle.
static const unsigned long kTrackFallbackRefreshMs = 5UL * 60UL * 1000UL;

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------

bool TailTrackerFetcher::fetchStatus(const String &ident, TailFlightStatus &out)
{
    if (strlen(APIConfiguration::AEROAPI_KEY) == 0)
    {
        Serial.println("TailTrackerFetcher: No AeroAPI key configured");
        return false;
    }

    if (ident != s_stickyTailKey)
    {
        s_stickyTailKey = ident;
        s_stickyLat     = NAN;
        s_stickyLon     = NAN;
        s_stickyAlt     = 0;
        s_lastTrackFetchMs = 0;
    }

    const String url = String(APIConfiguration::AEROAPI_BASE_URL) + "/flights/" + ident;
    bool   https = true;
    String host;
    uint16_t port = 443;
    String path;
    if (!parseUrl(url, https, host, port, path))
    {
        Serial.println("TailTrackerFetcher: Failed to parse AeroAPI URL");
        return false;
    }

#if defined(FLIGHTWALL_SKIP_TLS)
    https = false;
    port  = 80;
    Serial.println("TailTrackerFetcher: SKIP_TLS — forcing HTTP on port 80");
#else
    if (!https)
    {
        Serial.println("TailTrackerFetcher: Refusing non-HTTPS AeroAPI URL");
        return false;
    }
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
            Serial.println("TailTrackerFetcher: AeroAPI request failed");
            return false;
        }
    }
#endif

    if (code != 200)
    {
        Serial.print("TailTrackerFetcher: AeroAPI HTTP ");
        Serial.println(code);
        flightwallStringDrop(payload);
        return false;
    }

    // Parsed /flights document can be 10–30 KB.  GetLastTrack and Nominatim each
    // add another large buffer — keep those requests out of the same live range
    // as `doc` so the heap is not left ~30 KB below the pre-fetch watermark.
    TailFlightStatus result;
    String           faIdForTrack;

    // Build a filter so ArduinoJson only allocates the handful of fields we
    // actually use from flights[0].  The full response can be 15+ flights with
    // ~50 fields each; the filter cuts parse time and RAM use dramatically.
    {
    JsonDocument filter;
    filter["flights"][0]["fa_flight_id"]     = true;
    filter["flights"][0]["ident"]            = true;
    filter["flights"][0]["status"]           = true;
    filter["flights"][0]["progress_percent"] = true;
    filter["flights"][0]["actual_off"]       = true;
    filter["flights"][0]["actual_on"]        = true;
    filter["flights"][0]["scheduled_off"]    = true;
    filter["flights"][0]["scheduled_on"]     = true;
    filter["flights"][0]["last_position"]["latitude"]  = true;
    filter["flights"][0]["last_position"]["longitude"] = true;
    filter["flights"][0]["last_position"]["altitude"]  = true;
    filter["flights"][0]["origin"]["city"]              = true;
    filter["flights"][0]["origin"]["name"]              = true;
    filter["flights"][0]["origin"]["state"]             = true;
    filter["flights"][0]["origin"]["country_code"]      = true;
    filter["flights"][0]["origin"]["latitude"]          = true;
    filter["flights"][0]["origin"]["longitude"]         = true;
    filter["flights"][0]["origin"]["code_iata"]         = true;
    filter["flights"][0]["origin"]["code_icao"]         = true;
    filter["flights"][0]["origin"]["code_lid"]          = true;
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
    if (err)
    {
        Serial.print("TailTrackerFetcher: JSON parse error: ");
        Serial.println(err.c_str());
        return false;
    }

    JsonArray flights = doc["flights"].as<JsonArray>();
    if (flights.isNull() || flights.size() == 0)
    {
        Serial.println("TailTrackerFetcher: No flights in response");
        return false;
    }

    // Current epoch needed to rank upcoming scheduled legs.
#if !defined(ARDUINO_ARCH_ESP32)
    unsigned long nowEpoch = (unsigned long)WiFi.getTime();
#else
    unsigned long nowEpoch = (unsigned long)time(nullptr);
#endif

    // Pass 1: prefer the leg that is currently airborne (departed, not yet landed).
    bool foundAirborne = false;
    int  bestIdx       = 0;
    for (size_t i = 0; i < flights.size(); ++i)
    {
        // actual_off non-null  →  wheels have left the ground
        // actual_on  null      →  not yet landed
        if (!flights[i]["actual_off"].isNull() && flights[i]["actual_on"].isNull())
        {
            bestIdx      = (int)i;
            foundAirborne = true;
            break;
        }
    }

    // Pass 2: no airborne leg (Pass 1) — still need a best row when actual_off
    // is missing from the API (common briefly after takeoff).
    //
    // (A) Prefer a not-yet-landed leg whose scheduled *departure* is already in
    //     the past and after the most recent actual arrival in the list.  That
    //     is the "current" segment (en route or API lag) without relying on
    //     "closest schedule to now" which can wrongly favour an old *landed* row
    //     or a *future* next-day row in edge cases.
    // (B) Otherwise pick the leg whose scheduled window is closest to now over
    //     *all* rows including completed.  That restores "just landed" when the
    //     next leg is only scheduled in the future — unlike ranking *only*
    //     incomplete rows, which always hid the most recent arrival.
    if (!foundAirborne)
    {
        unsigned long lastLandEpoch = 0;
        for (size_t i = 0; i < flights.size(); ++i)
        {
            const String onStr = safeStr(flights[i], "actual_on");
            if (onStr.length() == 0)
                continue;
            const unsigned long t = parseISO8601(onStr);
            if (t > lastLandEpoch)
                lastLandEpoch = t;
        }

        int            bestAfterLand = -1;
        unsigned long  bestDepSo     = 0;
        for (size_t i = 0; i < flights.size(); ++i)
        {
            if (!flights[i]["actual_on"].isNull())
                continue;

            const String soStr = safeStr(flights[i], "scheduled_off");
            if (soStr.length() == 0)
                continue;
            const unsigned long soEp = parseISO8601(soStr);
            if (soEp == 0)
                continue;
            if (nowEpoch > 0 && soEp >= nowEpoch)
                continue; // departure not in the past — next flight, not "current"
            if (soEp <= lastLandEpoch)
                continue; // not the segment that follows the last known landing
            if (soEp > bestDepSo)
            {
                bestDepSo     = soEp;
                bestAfterLand = (int)i;
            }
        }

        if (bestAfterLand >= 0)
        {
            bestIdx = bestAfterLand;
        }
        else
        {
            unsigned long bestDiff = ULONG_MAX;
            for (size_t i = 0; i < flights.size(); ++i)
            {
                unsigned long minDiff = ULONG_MAX;

                const String scheduledOff = safeStr(flights[i], "scheduled_off");
                if (scheduledOff.length() > 0)
                {
                    const unsigned long offEpoch = parseISO8601(scheduledOff);
                    if (offEpoch > 0)
                    {
                        const unsigned long d = offEpoch > nowEpoch
                                                ? offEpoch - nowEpoch
                                                : nowEpoch - offEpoch;
                        if (d < minDiff)
                            minDiff = d;
                    }
                }

                const String scheduledOn = safeStr(flights[i], "scheduled_on");
                if (scheduledOn.length() > 0)
                {
                    const unsigned long onEpoch = parseISO8601(scheduledOn);
                    if (onEpoch > 0)
                    {
                        const unsigned long d = onEpoch > nowEpoch
                                                ? onEpoch - nowEpoch
                                                : nowEpoch - onEpoch;
                        if (d < minDiff)
                            minDiff = d;
                    }
                }

                if (minDiff < bestDiff)
                {
                    bestDiff = minDiff;
                    bestIdx  = (int)i;
                }
            }
        }
    }

    Serial.print("TailTrackerFetcher: using flights[");
    Serial.print(bestIdx);
    Serial.println("]");

    JsonObject f = flights[bestIdx].as<JsonObject>();

    result.ident  = safeStr(f, "ident");
    result.status = safeStr(f, "status");

    // Normalise status variants returned by different AeroAPI versions.
    if (result.status == "Arrived")               result.status = "Landed";
    if (result.status.startsWith("En Route"))     result.status = "Flying";
    if (result.status.startsWith("Arriving"))     result.status = "Arriving";

    result.progress_percent = 0;
    if (!f["progress_percent"].isNull())
        result.progress_percent = f["progress_percent"].as<int>();

    String offStr = safeStr(f, "actual_off");
    String onStr  = safeStr(f, "actual_on");
    String schOffStr = safeStr(f, "scheduled_off");
    result.actual_off_epoch    = offStr.length()    > 0 ? parseISO8601(offStr)    : 0;
    result.actual_on_epoch     = onStr.length()     > 0 ? parseISO8601(onStr)     : 0;
    result.scheduled_off_epoch = schOffStr.length() > 0 ? parseISO8601(schOffStr) : 0;

    // AeroAPI omits progress_percent once the flight has landed; treat as 100.
    if (result.actual_on_epoch > 0 && result.progress_percent == 0)
        result.progress_percent = 100;

    // Best available destination airport code: IATA (3-letter) preferred,
    // then local ID (3-letter for most US airports), then ICAO (4-letter).
    if (!f["destination"].isNull())
    {
        JsonObject dest = f["destination"].as<JsonObject>();
        if (!dest["code_iata"].isNull())
            result.dest_code = dest["code_iata"].as<const char *>();
        else if (!dest["code_lid"].isNull())
            result.dest_code = dest["code_lid"].as<const char *>();
        else if (!dest["code_icao"].isNull())
            result.dest_code = dest["code_icao"].as<const char *>();
        extractAirportLatLon(dest, result.dest_lat, result.dest_lon);
    }

    if (!f["origin"].isNull())
    {
        JsonObject orig = f["origin"].as<JsonObject>();
        extractAirportLatLon(orig, result.origin_lat, result.origin_lon);

        // Best available origin airport code: IATA preferred, then LID, then ICAO.
        if (!orig["code_iata"].isNull())
            result.origin_code = orig["code_iata"].as<const char *>();
        else if (!orig["code_lid"].isNull())
            result.origin_code = orig["code_lid"].as<const char *>();
        else if (!orig["code_icao"].isNull())
            result.origin_code = orig["code_icao"].as<const char *>();
    }

    // Scheduled / on-ground: AeroAPI often omits airport lat/lon in the /flights
    // payload. Use the same Nominatim forward-geocode approach as the landed fix
    // so the compass has a real bearing instead of defaulting to north.

    result.lat = NAN;
    result.lon = NAN;
    result.altitude_ft = 0;
    if (!f["last_position"].isNull())
    {
        JsonObject pos = f["last_position"].as<JsonObject>();
        if (!pos["latitude"].isNull())  result.lat          = pos["latitude"].as<double>();
        if (!pos["longitude"].isNull()) result.lon          = pos["longitude"].as<double>();
        if (!pos["altitude"].isNull())  result.altitude_ft  = pos["altitude"].as<int>();
    }
    Serial.print("TailTrackerFetcher: lat=");
    Serial.print(isnan(result.lat) ? 0.0 : result.lat, 4);
    Serial.print(" lon=");
    Serial.print(isnan(result.lon) ? 0.0 : result.lon, 4);
    Serial.print(" alt=");
    Serial.println(result.altitude_ft);

    // Capture a time reference so the display can compute elapsed time without
    // hitting WiFi.getTime() on every frame.
#if !defined(ARDUINO_ARCH_ESP32)
    result.fetch_epoch = (unsigned long)WiFi.getTime();
#else
    result.fetch_epoch = (unsigned long)time(nullptr);
#endif
    result.fetch_millis = millis();

    // City/region from JSON only, while `f` and JsonDocument are still valid
    // (GetLastTrack + Nominatim must not run in the same scope as /flights doc).
    {
        const bool hasLivePos = hasPlausibleAircraftPosition(result.lat, result.lon);

        if (!hasLivePos && result.actual_on_epoch > 0)
        {
            Serial.println("TailTrackerFetcher: landed, no position — using destination");
            if (!f["destination"].isNull())
            {
                JsonObject dest = f["destination"].as<JsonObject>();
                if (!dest["city"].isNull())
                    result.city = dest["city"].as<const char *>();
                else if (!dest["name"].isNull())
                    result.city = dest["name"].as<const char *>();
                else if (!dest["code_icao"].isNull())
                    result.city = dest["code_icao"].as<const char *>();

                if (!dest["state"].isNull())
                {
                    const char *abbr = usStateAbbrev(dest["state"].as<const char *>());
                    result.region = abbr ? String(abbr) : safeStr(dest, "state");
                }
                else if (!dest["country_code"].isNull())
                {
                    result.region = String(dest["country_code"].as<const char *>());
                    result.region.toUpperCase();
                }

                Serial.print("TailTrackerFetcher: dest city=");
                Serial.print(result.city);
                Serial.print(" region=");
                Serial.println(result.region);
            }
            else
            {
                Serial.println("TailTrackerFetcher: destination is null");
            }
        }
        else if (!hasLivePos && result.actual_off_epoch == 0
                 && !f["origin"].isNull())
        {
            JsonObject orig = f["origin"].as<JsonObject>();
            Serial.println("TailTrackerFetcher: on ground, using origin location");

            if (!orig["city"].isNull())
                result.city = orig["city"].as<const char *>();
            else if (!orig["name"].isNull())
                result.city = orig["name"].as<const char *>();

            if (!orig["state"].isNull())
            {
                const char *abbr = usStateAbbrev(orig["state"].as<const char *>());
                result.region = abbr ? String(abbr) : safeStr(orig, "state");
            }
            else if (!orig["country_code"].isNull())
            {
                result.region = String(orig["country_code"].as<const char *>());
                result.region.toUpperCase();
            }

            Serial.print("TailTrackerFetcher: origin city=");
            Serial.print(result.city);
            Serial.print(" region=");
            Serial.println(result.region);
        }
    }

    faIdForTrack = safeStr(f, "fa_flight_id");
    } // end scope: free filter + doc before /track or Nominatim (heap watermark)

    // Location resolution (network) — /flights JsonDocument is gone.
    if (hasPlausibleAircraftPosition(result.lat, result.lon))
    {
        Serial.print("TailTrackerFetcher: geocoding lat=");
        Serial.print(result.lat, 4);
        Serial.print(" lon=");
        Serial.println(result.lon, 4);
        fetchReverseGeocode(result.lat, result.lon, result.city, result.region);
    }
    else if (result.actual_on_epoch > 0)
    {
        // city/region from destination in JSON, set above
    }
    else
    {
        if (result.actual_off_epoch == 0)
        {
            // on ground: city/region from origin in JSON, set above (if any)
        }
        else if (faIdForTrack.length() > 0)
        {
            const unsigned long trackAgeMs =
                (s_lastTrackFetchMs == 0) ? ULONG_MAX : (millis() - s_lastTrackFetchMs);
            if (hasPlausibleAircraftPosition(s_stickyLat, s_stickyLon)
                && trackAgeMs < kTrackFallbackRefreshMs)
            {
                result.lat = s_stickyLat;
                result.lon = s_stickyLon;
                if (result.altitude_ft == 0 && s_stickyAlt > 0)
                    result.altitude_ft = s_stickyAlt;
                Serial.print("TailTrackerFetcher: using cached track lat=");
                Serial.print(result.lat, 4);
                Serial.print(" lon=");
                Serial.print(result.lon, 4);
                Serial.print(" alt=");
                Serial.println(result.altitude_ft);
                fetchReverseGeocode(result.lat, result.lon,
                                    result.city, result.region);
            }
            else
            {
                Serial.println("TailTrackerFetcher: airborne, trying GetLastTrack");
                double trackLat = NAN, trackLon = NAN;
                int    trackAlt = 0;
                if (fetchTrackPosition(faIdForTrack, trackLat, trackLon, trackAlt)
                        && !isnan(trackLat) && !isnan(trackLon))
                {
                    s_lastTrackFetchMs = millis();
                    result.lat         = trackLat;
                    result.lon         = trackLon;
                    result.altitude_ft = trackAlt;
                    Serial.print("TailTrackerFetcher: track lat=");
                    Serial.print(result.lat, 4);
                    Serial.print(" lon=");
                    Serial.print(result.lon, 4);
                    Serial.print(" alt=");
                    Serial.println(result.altitude_ft);
                    fetchReverseGeocode(result.lat, result.lon,
                                        result.city, result.region);
                }
                else
                {
                    Serial.println("TailTrackerFetcher: track unavailable");
                }
            }
        }
        else
        {
            Serial.println("TailTrackerFetcher: no fa_flight_id, cannot fetch track");
        }
    }

    // Landed: destination airport may omit lat/lon in the AeroAPI payload; the
    // line-3 text is still set from the destination city/region. Forward-geocode
    // that (or the dest code) so dest_lat/dest_lon feed the compass fallback.
    if (result.actual_on_epoch > 0
        && (isnan(result.dest_lat) || isnan(result.dest_lon)))
    {
        String gq;
        if (result.city.length() > 0)
        {
            gq = result.city;
            if (result.region.length() > 0)
            {
                gq += ", ";
                gq += result.region;
            }
        }
        else if (result.dest_code.length() > 0)
        {
            gq = result.dest_code;
        }
        if (gq.length() > 0)
        {
            double fla, flo;
            if (fetchForwardGeocodeForDestination(gq, fla, flo)
                    && hasPlausibleAircraftPosition(fla, flo))
            {
                result.dest_lat = fla;
                result.dest_lon = flo;
                Serial.print("TailTrackerFetcher: forward geocode dest ");
                Serial.print(fla, 4);
                Serial.print(",");
                Serial.println(flo, 4);
            }
        }
    }

    // On-ground / scheduled: forward-geocode the origin location (line-3 text)
    // so origin_lat/origin_lon feed the compass fallback.
    if (result.actual_off_epoch == 0 && result.actual_on_epoch == 0
        && (isnan(result.origin_lat) || isnan(result.origin_lon)))
    {
        String gq;
        if (result.city.length() > 0)
        {
            gq = result.city;
            if (result.region.length() > 0)
            {
                gq += ", ";
                gq += result.region;
            }
        }
        else if (result.origin_code.length() > 0)
        {
            gq = result.origin_code;
        }

        if (gq.length() > 0)
        {
            double ola, olo;
            if (fetchForwardGeocodeForDestination(gq, ola, olo)
                    && hasPlausibleAircraftPosition(ola, olo))
            {
                result.origin_lat = ola;
                result.origin_lon = olo;
                Serial.print("TailTrackerFetcher: forward geocode origin ");
                Serial.print(ola, 4);
                Serial.print(",");
                Serial.println(olo, 4);
            }
        }
    }

    if (hasPlausibleAircraftPosition(result.lat, result.lon))
    {
        s_stickyLat = result.lat;
        s_stickyLon = result.lon;
        s_stickyAlt = result.altitude_ft;
    }
    else if (hasPlausibleAircraftPosition(s_stickyLat, s_stickyLon))
    {
        result.lat = s_stickyLat;
        result.lon = s_stickyLon;
        if (result.altitude_ft == 0 && s_stickyAlt > 0)
            result.altitude_ft = s_stickyAlt;
    }

    result.valid = true;
    out = result;
    return true;
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
        TrackStreamParser parser;
        if (!wifiClientRequestStream("GET", host, port, path, hdrs, "", code,
                                     trackStreamOnChunk, &parser))
        {
            Serial.println("TailTrackerFetcher: track request failed");
            return false;
        }
        if (parser.pendingKey != nullptr && parser.readingValue)
            trackParserCommitValue(parser);
        if (code != 200)
        {
            Serial.print("TailTrackerFetcher: track HTTP ");
            Serial.println(code);
            return false;
        }
        if (!hasPlausibleAircraftPosition(parser.lastLat, parser.lastLon))
        {
            Serial.println("TailTrackerFetcher: track positions array empty");
            return false;
        }
        outLat = parser.lastLat;
        outLon = parser.lastLon;
        outAlt = parser.lastAlt;
        return true;
    }
#endif

    if (code != 200)
    {
        Serial.print("TailTrackerFetcher: track HTTP ");
        Serial.println(code);
        flightwallStringDrop(payload);
        return false;
    }

    // Filter: keep lat/lon/altitude from every position entry.
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

    // Positions are in ascending time order; the last element is the most recent.
    // Note: the track endpoint returns altitude in hundreds of feet (flight
    // levels), e.g. 360 = FL360 = 36,000 ft.  Multiply by 100 to get feet.
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
// URL-encode a string for a Nominatim query= parameter.
// ---------------------------------------------------------------------------

static void appendUrlQueryEncoded(const String &s, String &out)
{
    for (size_t i = 0; i < s.length(); ++i)
    {
        const unsigned char c = (unsigned char)s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
            || c == '-' || c == '_' || c == '.')
        {
            out += (char)c;
        }
        else if (c == ' ')
        {
            out += '+';
        }
        else
        {
            char buf[5];
            snprintf(buf, sizeof(buf), "%%%02X", (unsigned int)c);
            out += buf;
        }
    }
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
        && !isnan(_lastForwardLat) && !isnan(_lastForwardLon))
    {
        outLat = _lastForwardLat;
        outLon = _lastForwardLon;
        return true;
    }

    String path = String("/search?format=json&limit=1&q=");
    appendUrlQueryEncoded(searchQuery, path);
    if (path.length() > 512)
    {
        Serial.println("TailTrackerFetcher: forward geocode query too long");
        return false;
    }

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
        const String hdrs =
            "User-Agent: FlightWallFirmware/1.0\r\nAccept: application/json\r\n";
        if (!wifiClientRequest("GET", host, port, path, hdrs, "", code, payload))
        {
            Serial.println("TailTrackerFetcher: Nominatim /search failed");
            return false;
        }
    }
#endif

    if (code != 200)
    {
        Serial.print("TailTrackerFetcher: Nominatim /search HTTP ");
        Serial.println(code);
        return false;
    }

    // First hit only: keep parse cost and RAM small.
    JsonDocument searchFilter;
    searchFilter[0]["lat"] = true;
    searchFilter[0]["lon"] = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload,
                                             DeserializationOption::Filter(searchFilter));
    flightwallStringDrop(payload);
    if (err)
    {
        Serial.println("TailTrackerFetcher: Nominatim /search JSON parse error");
        return false;
    }

    JsonArray hits = doc.as<JsonArray>();
    if (hits.isNull() || hits.size() == 0)
    {
        Serial.println("TailTrackerFetcher: Nominatim /search: no results");
        doc.clear();
        return false;
    }

    JsonObject first = hits[0].as<JsonObject>();
    if (first["lat"].isNull() || first["lon"].isNull())
    {
        doc.clear();
        return false;
    }

    outLat = first["lat"].as<double>();
    outLon = first["lon"].as<double>();
    doc.clear();

    if (isnan(outLat) || isnan(outLon))
        return false;

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
    // Reuse cached result when the aircraft hasn't moved significantly.
    if (!isnan(_lastGeoLat) && !isnan(_lastGeoLon))
    {
        if (haversineKm(_lastGeoLat, _lastGeoLon, lat, lon)
                < TailTrackerConfiguration::GEO_CACHE_THRESHOLD_KM)
        {
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
        const String hdrs =
            "User-Agent: FlightWallFirmware/1.0\r\nAccept: application/json\r\n";
        if (!wifiClientRequest("GET", host, port, path, hdrs, "", code, payload))
        {
            Serial.println("TailTrackerFetcher: Nominatim request failed");
            return false;
        }
    }
#endif

    if (code != 200)
    {
        Serial.print("TailTrackerFetcher: Nominatim HTTP ");
        Serial.println(code);
        return false;
    }

    JsonDocument geo;
    DeserializationError err = deserializeJson(geo, payload);
    flightwallStringDrop(payload);
    if (err)
    {
        Serial.println("TailTrackerFetcher: Nominatim JSON parse error");
        return false;
    }

    JsonObject addr = geo["address"].as<JsonObject>();

    // City fallback chain: city → town → village → municipality → county.
    String city;
    if      (!addr["city"].isNull())         city = addr["city"].as<const char *>();
    else if (!addr["town"].isNull())          city = addr["town"].as<const char *>();
    else if (!addr["village"].isNull())       city = addr["village"].as<const char *>();
    else if (!addr["municipality"].isNull())  city = addr["municipality"].as<const char *>();
    else if (!addr["county"].isNull())        city = addr["county"].as<const char *>();

    String cc = addr["country_code"].isNull()
                ? String("") : String(addr["country_code"].as<const char *>());
    cc.toUpperCase();

    String region;
    if (cc == "US")
    {
        String state = addr["state"].isNull()
                       ? String("") : String(addr["state"].as<const char *>());
        const char *abbr = usStateAbbrev(state.c_str());
        region = abbr ? String(abbr) : cc;
    }
    else
    {
        region = cc;
    }

    outCity   = city;
    outRegion = region;

    _lastGeoLat = lat;
    _lastGeoLon = lon;
    _lastCity   = city;
    _lastRegion = region;

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
    for (const auto &e : kTable)
    {
        if (strcmp(fullName, e.full) == 0) return e.abbr;
    }
    return nullptr;
}
