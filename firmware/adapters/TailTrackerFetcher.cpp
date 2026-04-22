/*
Purpose: Fetch real-time status for a tracked tail number from AeroAPI and
         reverse-geocode the aircraft position using Nominatim.
Responsibilities:
- GET /flights/{ident} from AeroAPI; parse status, progress, timestamps, last position.
- GET Nominatim reverse-geocode for city/state/country from last position.
- Cache geocode results so Nominatim is not called more often than necessary.
*/
#include "adapters/TailTrackerFetcher.h"

#include <ArduinoJson.h>
#include <ArduinoHttpClient.h>
#include "config/APIConfiguration.h"
#include "config/TailTrackerConfiguration.h"
#include "utils/HttpUtils.h"
#include "utils/GeoUtils.h"

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
        return false;
    }

    // Build a filter so ArduinoJson only allocates the handful of fields we
    // actually use from flights[0].  The full response can be 15+ flights with
    // ~50 fields each; the filter cuts parse time and RAM use dramatically.
    JsonDocument filter;
    filter["flights"][0]["fa_flight_id"]     = true;
    filter["flights"][0]["ident"]            = true;
    filter["flights"][0]["status"]           = true;
    filter["flights"][0]["progress_percent"] = true;
    filter["flights"][0]["actual_off"]       = true;
    filter["flights"][0]["actual_on"]        = true;
    filter["flights"][0]["last_position"]["latitude"]  = true;
    filter["flights"][0]["last_position"]["longitude"] = true;
    filter["flights"][0]["last_position"]["altitude"]  = true;
    filter["flights"][0]["origin"]["city"]              = true;
    filter["flights"][0]["origin"]["name"]              = true;
    filter["flights"][0]["origin"]["state"]             = true;
    filter["flights"][0]["origin"]["country_code"]      = true;
    filter["flights"][0]["destination"]["city"]         = true;
    filter["flights"][0]["destination"]["name"]         = true;
    filter["flights"][0]["destination"]["code_iata"]    = true;
    filter["flights"][0]["destination"]["code_icao"]    = true;
    filter["flights"][0]["destination"]["code_lid"]     = true;
    filter["flights"][0]["destination"]["state"]        = true;
    filter["flights"][0]["destination"]["country_code"] = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload,
                                               DeserializationOption::Filter(filter));
    payload = String(); // free before using doc
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

    // For aircraft with multiple legs in the response (common on commercial/charter
    // tail numbers), prefer the leg that has departed but not yet landed — that is
    // the currently airborne flight.  Fall back to flights[0] if none qualifies.
    int bestIdx = 0;
    for (size_t i = 0; i < flights.size(); ++i)
    {
        // actual_off non-null  →  wheels have left the ground
        // actual_on  null      →  not yet landed
        if (!flights[i]["actual_off"].isNull() && flights[i]["actual_on"].isNull())
        {
            bestIdx = (int)i;
            break;
        }
    }
    Serial.print("TailTrackerFetcher: using flights[");
    Serial.print(bestIdx);
    Serial.println("]");

    JsonObject f = flights[bestIdx].as<JsonObject>();

    TailFlightStatus result;
    const String faFlightId = safeStr(f, "fa_flight_id");
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
    result.actual_off_epoch = offStr.length() > 0 ? parseISO8601(offStr) : 0;
    result.actual_on_epoch  = onStr.length()  > 0 ? parseISO8601(onStr)  : 0;

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
    }

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

    // Location resolution:
    //   Airborne with position  → reverse-geocode live lat/lon via Nominatim.
    //   Landed, no position     → fall back to destination city/state.
    //   On ground (pre-depart)  → fall back to origin city/state.
    //   Airborne, no position   → try GetLastTrack; leave blank if unavailable.
    if (!isnan(result.lat) && !isnan(result.lon))
    {
        Serial.print("TailTrackerFetcher: geocoding lat=");
        Serial.print(result.lat, 4);
        Serial.print(" lon=");
        Serial.println(result.lon, 4);
        fetchReverseGeocode(result.lat, result.lon, result.city, result.region);
    }
    else if (result.actual_on_epoch > 0)
    {
        // Flight has landed — show the destination as the current location.
        Serial.println("TailTrackerFetcher: landed, no position — using destination");
        if (!f["destination"].isNull())
        {
            JsonObject dest = f["destination"].as<JsonObject>();

            // city → airport name → ICAO code, whichever is non-null first.
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
    else
    {
        // No position and not yet landed.  If the flight has not departed yet,
        // show the origin airport city/region as the current location — the
        // same pattern used for landed flights showing the destination.
        if (result.actual_off_epoch == 0 && !f["origin"].isNull())
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
        else if (faFlightId.length() > 0)
        {
            // Airborne but last_position is absent — try GetLastTrack.
            Serial.println("TailTrackerFetcher: airborne, trying GetLastTrack");
            double trackLat = NAN, trackLon = NAN;
            int    trackAlt = 0;
            if (fetchTrackPosition(faFlightId, trackLat, trackLon, trackAlt)
                    && !isnan(trackLat) && !isnan(trackLon))
            {
                result.lat          = trackLat;
                result.lon          = trackLon;
                result.altitude_ft  = trackAlt;
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
        else
        {
            Serial.println("TailTrackerFetcher: no fa_flight_id, cannot fetch track");
        }
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
        if (!wifiClientRequest("GET", host, port, path, hdrs, "", code, payload))
        {
            Serial.println("TailTrackerFetcher: track request failed");
            return false;
        }
    }
#endif

    if (code != 200)
    {
        Serial.print("TailTrackerFetcher: track HTTP ");
        Serial.println(code);
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
    payload = String();
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

    if (isnan(lastLat) || isnan(lastLon)) return false;

    outLat = lastLat;
    outLon = lastLon;
    outAlt = lastAlt;
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
    payload = String();
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
