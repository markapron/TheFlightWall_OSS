#include "SerialConfig.h"
#include "Secrets.h"
#include <string.h>

// ---------------------------------------------------------------------------
// Platform-specific persistent storage
// ---------------------------------------------------------------------------

#if defined(ARDUINO_ARCH_ESP32)
  // NVS via Preferences — already included through SerialConfig.h

#else
  // SAMD51 (Matrix Portal M4): emulate EEPROM in internal flash using
  // FlashStorage_SAMD.  The library provides EEPROM.get / put / commit.
  #include <FlashStorage_SAMD.h>

  // All config is stored as a single flat struct starting at address 0.
  // A magic number at the front distinguishes an initialised block from
  // blank flash (0xFFFF…).  Bump the version nibble if the layout changes
  // so old data is safely ignored after a firmware update.
  static const uint32_t kSAMDConfigMagic = 0xF17E0001UL;

  struct SAMDPersistedConfig
  {
      uint32_t magic;
      char     ssid[64];
      char     pass[64];
      char     tail[32];
  };
#endif

namespace SerialConfig
{

// ---------------------------------------------------------------------------
// Public runtime values — start from compile-time defaults in Secrets.h
// ---------------------------------------------------------------------------

String wifiSSID     = SECRET_WIFI_SSID;
String wifiPassword = SECRET_WIFI_PASSWORD;
String tailNumber   = SECRET_TRACKED_TAIL_NUMBER;

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

enum class MenuState
{
    IDLE,
    MENU,
    ENTER_SSID,
    ENTER_PASS,
    ENTER_TAIL,
};

static MenuState s_state    = MenuState::IDLE;
static String    s_inputBuf;

#if defined(ARDUINO_ARCH_ESP32)
static Preferences s_prefs;
static const char *kNS = "flightwall";
#endif

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void printMenu()
{
    Serial.println();
    Serial.println(F("=== FlightWall Config ==="));
    Serial.print  (F("  WiFi SSID  : ")); Serial.println(wifiSSID);
    Serial.print(F("  WiFi Pass  : "));
    if (wifiPassword.length())
    {
        Serial.print(wifiPassword.length());
        Serial.println(F(" chars (hidden)"));
    }
    else
    {
        Serial.println(F("(none)"));
    }
    Serial.print  (F("  Tail Number: ")); Serial.println(tailNumber);
    Serial.println(F("-------------------------"));
    Serial.println(F("  1) Change WiFi SSID"));
    Serial.println(F("  2) Change WiFi Password"));
    Serial.println(F("  3) Change Tail Number"));
    Serial.println(F("  r) Save & restart  <-- required after WiFi changes"));
    Serial.println(F("  x) Close menu"));
    Serial.println(F("========================="));
    Serial.print  (F("> "));
}

static void persistValues()
{
#if defined(ARDUINO_ARCH_ESP32)
    s_prefs.begin(kNS, false);
    s_prefs.putString("ssid", wifiSSID);
    s_prefs.putString("pass", wifiPassword);
    s_prefs.putString("tail", tailNumber);
    s_prefs.end();
    Serial.println(F("[saved to NVS flash]"));

#else
    SAMDPersistedConfig cfg;
    cfg.magic = kSAMDConfigMagic;
    memset(cfg.ssid, 0, sizeof(cfg.ssid));
    memset(cfg.pass, 0, sizeof(cfg.pass));
    memset(cfg.tail, 0, sizeof(cfg.tail));
    strncpy(cfg.ssid, wifiSSID.c_str(),     sizeof(cfg.ssid) - 1);
    strncpy(cfg.pass, wifiPassword.c_str(), sizeof(cfg.pass) - 1);
    strncpy(cfg.tail, tailNumber.c_str(),   sizeof(cfg.tail) - 1);
    EEPROM.put(0, cfg);
    EEPROM.commit();
    Serial.println(F("[saved to flash]"));
#endif
}

static void loadPersistedValues()
{
#if defined(ARDUINO_ARCH_ESP32)
    s_prefs.begin(kNS, true);
    String s = s_prefs.getString("ssid", "");
    String p = s_prefs.getString("pass", "");
    String t = s_prefs.getString("tail", "");
    s_prefs.end();
    if (s.length()) wifiSSID     = s;
    if (p.length()) wifiPassword = p;
    if (t.length()) tailNumber   = t;

#else
    SAMDPersistedConfig cfg;
    EEPROM.get(0, cfg);
    if (cfg.magic == kSAMDConfigMagic)
    {
        // Ensure null-termination before converting to String.
        cfg.ssid[sizeof(cfg.ssid) - 1] = '\0';
        cfg.pass[sizeof(cfg.pass) - 1] = '\0';
        cfg.tail[sizeof(cfg.tail) - 1] = '\0';
        if (strlen(cfg.ssid) > 0) wifiSSID     = cfg.ssid;
        if (strlen(cfg.pass) > 0) wifiPassword = cfg.pass;
        if (strlen(cfg.tail) > 0) tailNumber   = cfg.tail;
        Serial.println(F("[config loaded from flash]"));
    }
#endif
}

static void rebootDevice()
{
    Serial.println(F("Saving & rebooting..."));
    Serial.flush();
    delay(300);
#if defined(ARDUINO_ARCH_ESP32)
    ESP.restart();
#else
    NVIC_SystemReset();
#endif
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void begin()
{
    loadPersistedValues();
    Serial.println(F("Type 'm' + Enter for the config menu."));
}

void tick()
{
    while (Serial.available())
    {
        const char c = (char)Serial.read();

        // ---- IDLE: only wake on 'm' ----------------------------------------
        if (s_state == MenuState::IDLE)
        {
            if (c == 'm' || c == 'M')
            {
                s_state = MenuState::MENU;
                printMenu();
            }
            continue;
        }

        // ---- MENU: single-key selection ------------------------------------
        if (s_state == MenuState::MENU)
        {
            if (c == '\r' || c == '\n') continue;
            s_inputBuf = "";
            switch (c)
            {
            case '1':
                s_state = MenuState::ENTER_SSID;
                Serial.println();
                Serial.print(F("New WiFi SSID (Enter to keep current): "));
                break;
            case '2':
                s_state = MenuState::ENTER_PASS;
                Serial.println();
                Serial.print(F("New WiFi Password (Enter to keep current): "));
                break;
            case '3':
                s_state = MenuState::ENTER_TAIL;
                Serial.println();
                Serial.print(F("New Tail Number (Enter to keep current): "));
                break;
            case 'r': case 'R':
                persistValues();
                rebootDevice();
                break;
            case 'x': case 'X':
                s_state = MenuState::IDLE;
                Serial.println(F("\n[Config menu closed]"));
                break;
            default:
                Serial.print(F("> "));
                break;
            }
            continue;
        }

        // ---- ENTER_* states: accumulate a line of text ---------------------
        if (c == '\r') continue;

        if (c == '\n')
        {
            s_inputBuf.trim();
            if (s_inputBuf.length() > 0)
            {
                switch (s_state)
                {
                case MenuState::ENTER_SSID:
                    wifiSSID = s_inputBuf;
                    persistValues();
                    Serial.println(F("WiFi SSID saved. Press 'r' to restart and connect."));
                    break;
                case MenuState::ENTER_PASS:
                    wifiPassword = s_inputBuf;
                    persistValues();
                    Serial.println(F("WiFi password saved. Press 'r' to restart and connect."));
                    break;
                case MenuState::ENTER_TAIL:
                    tailNumber = s_inputBuf;
                    persistValues();
                    Serial.print(F("Tail number set to "));
                    Serial.print(tailNumber);
                    Serial.println(F(" — takes effect on next fetch."));
                    break;
                default:
                    break;
                }
            }
            else
            {
                Serial.println(F("[no change]"));
            }
            s_inputBuf = "";
            s_state    = MenuState::MENU;
            printMenu();
            continue;
        }

        // Echo the character and add it to the input buffer
        Serial.print(c);
        s_inputBuf += c;
    }
}

void openMenu()
{
    s_inputBuf = "";
    s_state    = MenuState::MENU;
    printMenu();
}

bool isMenuOpen()
{
    return s_state != MenuState::IDLE;
}

} // namespace SerialConfig
