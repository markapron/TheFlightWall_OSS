#pragma once

#include <Arduino.h>
#include "Secrets.h"

namespace WiFiConfiguration
{
    static const char *WIFI_SSID = SECRET_WIFI_SSID;
    static const char *WIFI_PASSWORD = SECRET_WIFI_PASSWORD;

    // If the association drops (AP idle timeout, DHCP, etc.), retry at most this
    // often so loop() is not stuck with WiFi.status() != WL_CONNECTED forever.
    static const unsigned long RECONNECT_COOLDOWN_MS = 30000UL;
}
