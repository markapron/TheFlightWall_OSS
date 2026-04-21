#include "utils/HttpUtils.h"

WifiClientTickFn wifiClientTick = nullptr;

static inline void tick()
{
    if (wifiClientTick)
        wifiClientTick();
}

#if !defined(ARDUINO_ARCH_ESP32)
#if defined(FLIGHTWALL_SKIP_TLS)
  #include <WiFiNINA.h> // WiFiClient
#else
  #include <WiFiNINA.h> // WiFiSSLClient
#endif

bool wifiClientRequest(const String &method, const String &host, uint16_t port,
                       const String &path, const String &extraHeaders,
                       const String &body, int &outCode, String &outPayload)
{
    outCode = -1;
    outPayload = "";

#if defined(FLIGHTWALL_SKIP_TLS)
    WiFiClient client;
#else
    WiFiSSLClient client;
#endif

    Serial.print("wifiClientRequest: ");
    Serial.print(method);
    Serial.print(" ");
    Serial.print(host);
    Serial.print(":");
    Serial.print(port);
    Serial.println(path);

    tick(); // update display before blocking on TLS handshake (can take 5-15 s)
    if (!client.connect(host.c_str(), port))
    {
        Serial.println("wifiClientRequest: connect() failed");
        return false;
    }
    tick(); // update display immediately after TLS completes
    Serial.println("wifiClientRequest: connected, sending request...");

    // Send the complete HTTP request in one logical block so the AirLift's SPI
    // transport delivers it as a single TCP segment rather than many tiny fragments.
    client.print(method);
    client.print(" ");
    client.print(path);
    client.println(" HTTP/1.1");
    client.print("Host: ");
    client.println(host);
    client.println("Connection: close");
    if (body.length() > 0)
    {
        client.print("Content-Length: ");
        client.println((int)body.length());
    }
    if (extraHeaders.length() > 0)
    {
        client.print(extraHeaders);
    }
    client.println(); // blank line — end of headers
    if (body.length() > 0)
    {
        client.print(body);
    }
    client.flush();

    Serial.println("wifiClientRequest: request sent, waiting for response...");

    // Wait up to 30 s for the server to start responding
    unsigned long timeout = millis() + 30000UL;
    while (!client.available())
    {
        if (millis() > timeout)
        {
            Serial.println("wifiClientRequest: timeout waiting for first byte");
            client.stop();
            return false;
        }
        tick();
        delay(50);
    }

    // Read status line: "HTTP/1.1 200 OK\r\n"
    client.setTimeout(10000);
    String statusLine = client.readStringUntil('\n');
    statusLine.trim();
    Serial.print("wifiClientRequest: status line: ");
    Serial.println(statusLine);
    int sp = statusLine.indexOf(' ');
    if (sp >= 0)
    {
        outCode = statusLine.substring(sp + 1, sp + 4).toInt();
    }

    // Read response headers, detect Transfer-Encoding: chunked
    bool isChunked = false;
    while (client.connected() || client.available())
    {
        String line = client.readStringUntil('\n');
        line.trim();
        if (line.length() == 0)
            break; // blank line = end of headers
        String lower = line;
        lower.toLowerCase();
        if (lower.indexOf("transfer-encoding") >= 0 && lower.indexOf("chunked") >= 0)
        {
            isChunked = true;
            Serial.println("wifiClientRequest: chunked transfer encoding detected");
        }
    }

    // For error responses, cap body at 512 bytes — enough for a useful error message
    // but prevents spending seconds reading a 27 KB CDN HTML error page.
    const bool is2xx = (outCode >= 200 && outCode < 300);
    const size_t maxBodyBytes = is2xx ? (size_t)65535 : (size_t)512;

    if (isChunked)
    {
        // Decode HTTP/1.1 chunked body:
        // Each chunk is preceded by its size in hex on its own line, followed by \r\n,
        // then the chunk data, then another \r\n. A zero-size chunk signals the end.
        while (client.connected() || client.available())
        {
            if (outPayload.length() >= maxBodyBytes)
                break; // body limit reached — close early

            String sizeLine = client.readStringUntil('\n');
            sizeLine.trim();
            if (sizeLine.length() == 0)
                continue; // skip blank lines between chunks

            // Strip optional chunk extensions (e.g. "1a2b;ext=val" → "1a2b")
            int semi = sizeLine.indexOf(';');
            if (semi >= 0)
                sizeLine = sizeLine.substring(0, semi);

            unsigned long chunkSize = strtoul(sizeLine.c_str(), nullptr, 16);
            if (chunkSize == 0)
                break; // terminal zero-length chunk

            // Pre-reserve space for this whole chunk before reading a single byte.
            // Without this, String::operator+= doubles its buffer on each overflow,
            // fragmenting the SAMD51 heap severely after several fetch cycles.
            {
                size_t available = maxBodyBytes - outPayload.length();
                size_t toReserve = (chunkSize < available) ? (size_t)chunkSize : available;
                outPayload.reserve(outPayload.length() + toReserve);
            }

            unsigned long read = 0;
            unsigned long chunkTimeout = millis() + 10000UL;
            // +1 so we can null-terminate for String::operator+= (JSON is ASCII, no embedded nulls)
            char buf[65];
            while (read < chunkSize)
            {
                if (outPayload.length() >= maxBodyBytes)
                    break; // body limit reached mid-chunk
                if (client.available())
                {
                    size_t canRead = (size_t)(chunkSize - read);
                    if (canRead > (sizeof(buf) - 1)) canRead = sizeof(buf) - 1;
                    size_t space = maxBodyBytes - outPayload.length();
                    if (canRead > space) canRead = space;
                    int n = client.read((uint8_t *)buf, canRead);
                    if (n > 0)
                    {
                        buf[n] = '\0';
                        outPayload += buf;
                        read += (unsigned long)n;
                        chunkTimeout = millis() + 5000UL;
                    }
                }
                else if (millis() > chunkTimeout)
                {
                    Serial.println("wifiClientRequest: timeout reading chunk data");
                    break;
                }
                else
                {
                    tick();
                    delay(1);
                }
            }
            client.readStringUntil('\n'); // consume trailing \r\n after chunk data
        }
    }
    else
    {
        // Non-chunked: read raw body until connection closes or limit reached
        unsigned long bodyTimeout = millis() + 15000UL;
        // +1 so we can null-terminate for String::operator+= (JSON is ASCII, no embedded nulls)
        char buf[65];
        while (client.connected() || client.available())
        {
            if (client.available() && outPayload.length() < maxBodyBytes)
            {
                size_t canRead = sizeof(buf) - 1;
                size_t space = maxBodyBytes - outPayload.length();
                if (canRead > space) canRead = space;
                int n = client.read((uint8_t *)buf, canRead);
                if (n > 0)
                {
                    buf[n] = '\0';
                    outPayload += buf;
                    bodyTimeout = millis() + 5000UL;
                }
            }
            if (outPayload.length() >= maxBodyBytes || millis() > bodyTimeout)
                break;
            tick();
            delay(10);
        }
    }
    client.stop();

    Serial.print("wifiClientRequest: done — code=");
    Serial.print(outCode);
    Serial.print(", body=");
    Serial.print(outPayload.length());
    Serial.println(" bytes");

    // Free memory used by large error-page bodies that the caller will discard anyway
    if (outCode < 200 || outCode >= 300)
        outPayload = String();

    return true;
}
#endif

static bool parsePort(const String &s, uint16_t &outPort)
{
    if (s.length() == 0)
        return false;
    for (size_t i = 0; i < s.length(); ++i)
    {
        if (s[i] < '0' || s[i] > '9')
            return false;
    }
    long p = s.toInt();
    if (p <= 0 || p > 65535)
        return false;
    outPort = (uint16_t)p;
    return true;
}

bool parseUrl(const String &url, bool &outHttps, String &outHost, uint16_t &outPort, String &outPath)
{
    String s = url;
    s.trim();
    if (s.length() == 0)
        return false;

    outHttps = true;

    const String httpsPrefix = "https://";
    const String httpPrefix = "http://";

    if (s.startsWith(httpsPrefix))
    {
        outHttps = true;
        s = s.substring(httpsPrefix.length());
    }
    else if (s.startsWith(httpPrefix))
    {
        outHttps = false;
        s = s.substring(httpPrefix.length());
    }

    int slashIdx = s.indexOf('/');
    String hostPort = (slashIdx >= 0) ? s.substring(0, slashIdx) : s;
    outPath = (slashIdx >= 0) ? s.substring(slashIdx) : String("/");

    if (hostPort.length() == 0)
        return false;

    int colonIdx = hostPort.lastIndexOf(':');
    if (colonIdx >= 0)
    {
        outHost = hostPort.substring(0, colonIdx);
        String portStr = hostPort.substring(colonIdx + 1);
        if (!parsePort(portStr, outPort))
            return false;
    }
    else
    {
        outHost = hostPort;
        outPort = outHttps ? 443 : 80;
    }

    outHost.trim();
    if (outHost.length() == 0)
        return false;

    if (!outPath.startsWith("/"))
        outPath = String("/") + outPath;

    return true;
}

