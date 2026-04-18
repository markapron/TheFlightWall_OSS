#pragma once

#include <Arduino.h>
#include "Secrets.h"

namespace WiFiConfiguration
{
    static const char *WIFI_SSID = SECRET_WIFI_SSID;
    static const char *WIFI_PASSWORD = SECRET_WIFI_PASSWORD;
}
