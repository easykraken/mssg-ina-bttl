#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Update.h>
#include "FS.h"
#include "LittleFS.h"
#include <ArduinoJson.h>
#include <time.h>
#include <math.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include "wifi_qr_bitmap.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"

// ===================== TFT ===================== //
// Adafruit Feather ESP32-S2 TFT built-in display
#define TFT_CS 7
#define TFT_DC 39
#define TFT_RST 40
#define TFT_BACKLIGHT 45

// BOOT button on GPIO 0; pressing it toggles between network text and QR code.
#define QR_BUTTON_PIN 0

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

enum TftView
{
  VIEW_TEXT,
  VIEW_QR,
  VIEW_QR_INVERTED
};
TftView currentTftView = VIEW_TEXT;
bool qrButtonReading = HIGH;        // last raw reading
bool qrButtonState = HIGH;          // debounced state
unsigned long qrButtonLastDebounce = 0;

void drawTftText()
{
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextWrap(true);
  tft.setTextSize(2);

  int16_t lineHeight = 8 * 2;           // default GFX font is 8 px tall; text size = 2
  int16_t blockHeight = lineHeight * 3; // line1 + blank + line2
  int16_t y = (tft.height() - blockHeight) / 2;

  tft.setCursor(0, y);

  tft.setTextColor(ST77XX_BLUE, ST77XX_BLACK);
  tft.print("SSID: ");
  tft.setTextColor(ST77XX_ORANGE, ST77XX_BLACK);
  tft.println("mssg ina bttl");

  tft.println();

  tft.setTextColor(ST77XX_BLUE, ST77XX_BLACK);
  tft.print("PASS: ");
  tft.setTextColor(ST77XX_ORANGE, ST77XX_BLACK);
  tft.println("bottle123");
}

void drawTftQr(bool inverted)
{
  uint16_t bgColor = inverted ? ST77XX_BLACK : ST77XX_WHITE;
  uint16_t fgColor = inverted ? ST77XX_WHITE : ST77XX_BLACK;

  tft.fillScreen(bgColor);

  int16_t x = (tft.width() - WIFI_QR_WIDTH) / 2;
  int16_t y = (tft.height() - WIFI_QR_HEIGHT) / 2;

  for (int16_t row = 0; row < WIFI_QR_HEIGHT; row++)
  {
    for (int16_t col = 0; col < WIFI_QR_WIDTH; col++)
    {
      uint16_t byteIndex = row * ((WIFI_QR_WIDTH + 7) / 8) + (col / 8);
      uint8_t bitMask = 1 << (7 - (col % 8));
      bool blackModule = pgm_read_byte(&WIFI_QR_BITMAP[byteIndex]) & bitMask;
      tft.drawPixel(x + col, y + row, blackModule ? fgColor : bgColor);
    }
  }
}

void initTft()
{
  pinMode(TFT_BACKLIGHT, OUTPUT);
  digitalWrite(TFT_BACKLIGHT, HIGH);

  tft.init(135, 240);
  tft.setRotation(1);
  drawTftText();

  if (QR_BUTTON_PIN >= 0)
  {
    pinMode(QR_BUTTON_PIN, INPUT_PULLUP);
    qrButtonReading = digitalRead(QR_BUTTON_PIN);
    qrButtonState = qrButtonReading;
  }
}

void updateQrButton()
{
  if (QR_BUTTON_PIN < 0)
    return;

  bool reading = digitalRead(QR_BUTTON_PIN);
  if (reading != qrButtonReading)
    qrButtonLastDebounce = millis();

  if ((millis() - qrButtonLastDebounce) > 50)
  {
    if (reading != qrButtonState)
    {
      qrButtonState = reading;
      if (qrButtonState == LOW)
      {
        Serial.println("[TFT] BOOT button pressed — cycling view");
        if (currentTftView == VIEW_TEXT)
        {
          currentTftView = VIEW_QR;
          drawTftQr(false); // black on white
        }
        else if (currentTftView == VIEW_QR)
        {
          currentTftView = VIEW_QR_INVERTED;
          drawTftQr(true); // white on black
        }
        else
        {
          currentTftView = VIEW_TEXT;
          drawTftText();
        }
      }
    }
  }

  qrButtonReading = reading;
}

// ===================== CONFIG ===================== //
int led = LED_BUILTIN;

namespace Config
{
  //========= Defaults — overridable at runtime via admin panel ========//

  const char *LOCALITY_NAME = "mssg ina bttl";
  const char *BOARD_ICON = "";
  const char *BOARD_TAGLINE = "Take what you need • Share what you can";
  // const char *BOARD_RULES = "Be local • Be kind • No spam";
  const char *BOARD_RULES = "";
  const char *BOARD_FOOTER = "Powered locally — no internet required";

  const char *ADMIN_KEY = "lavish.meerkat"; // Please definintely change - either here or in the admin panel.

  const int LED_PIN = 4;

  const int LED_DAY_BRIGHTNESS = 80;
  const int LED_NIGHT_BRIGHTNESS = 20;
  const int NIGHT_START_HOUR = 20;
  const int DAY_START_HOUR = 7;

  // OTA safety: physical button + signed firmware
  // Default is the onboard SW38 button on GPIO 38 (Arduino BUTTON).
  // It has an on-board pull-up and is active-low.
  const int OTA_BUTTON_PIN = 38;
  const unsigned long OTA_ENABLE_MS = 300000UL;    // 5 minutes after button press
  const size_t OTA_MAX_SIZE = 0x1F0000;            // ~2 MB sanity cap

  //======= Settings that are NOT in the Admin Panel ==========//

  // Access Point settings
  // If you want to customize the AP info, this is the place to do it.
  // SSID is what neighbours see in their WiFi list.
  //
  // Hardening notes:
  // - WPA2-PSK with a published password is much better than an open AP,
  //   because each client gets its own pairwise key. Passive sniffing of
  //   another client's traffic goes from trivial to impractical.
  // - The password is baked into the SSID so it stays zero-friction to join.
  // - Keep AP_MAX_CONN as small as you can live with.
  // - AP station isolation is ideal, but the Arduino-ESP32 API does not
  //   expose it without dropping to ESP-IDF internals.
  const char *AP_SSID = "mssg ina bttl (key: bottle123)";
  const char *AP_PASS = "bottle123"; // 8-63 chars for WPA2-PSK; "" = open network
  const int AP_CHANNEL = 6;
  const int AP_MAX_CONN = 4;

  // Default message expiration time
  const int DEFAULT_EXPIRY_HOURS = 72;
  const int MIN_EXPIRY_HOURS = 1;
  const int MAX_EXPIRY_HOURS = 24 * 30; // 30 days
  const unsigned long MAX_EXPIRY_FUTURE_SECONDS = 86400UL * 365; // 1 year from now

  // Per-client rate limiting on /post
  const unsigned long POST_RATE_LIMIT_SECONDS = 60;
  const int RATE_LIMIT_TABLE_SIZE = 8;

  // !!!! DO NOT CHANGE THESE !!!!
  // The MAX_MSGS amount is not arbitrary. The heap for the array needs to be sized accordingly.
  // And why would you even need to change it? 200 messages is an absurd amount anyhow.
  const int MAX_MSGS = 200;
  const char *STORAGE_FILE = "/msgs.json";
  const char *TIME_FILE = "/time.json";
  const char *LEDCFG_FILE = "/led.json";

}

#include "ota_public_key.h"

// ===================== RUNTIME IDENTITY =====================
// These shadow the Config defaults and can be changed via the admin panel.
// Persisted to /identity.json on the SD card.

String id_name = Config::LOCALITY_NAME;
String id_icon = Config::BOARD_ICON;
String id_tagline = Config::BOARD_TAGLINE;
String id_rules = Config::BOARD_RULES;
String id_footer = Config::BOARD_FOOTER;

void saveIdentityConfig()
{
  DynamicJsonDocument doc(1024);
  doc["name"] = id_name;
  doc["icon"] = id_icon;
  doc["tagline"] = id_tagline;
  doc["rules"] = id_rules;
  doc["footer"] = id_footer;

  File tmp = LittleFS.open("/id.tmp", FILE_WRITE);
  if (!tmp)
    return;
  serializeJson(doc, tmp);
  tmp.close();
  LittleFS.remove("/identity.json");
  LittleFS.rename("/id.tmp", "/identity.json");
}

void loadIdentityConfig()
{
  if (!LittleFS.exists("/identity.json"))
    return;
  File f = LittleFS.open("/identity.json");
  if (!f)
    return;
  DynamicJsonDocument doc(1024);
  if (!deserializeJson(doc, f))
  {
    if (doc["name"].as<String>().length())
      id_name = doc["name"].as<String>();
    if (doc["icon"].as<String>().length())
      id_icon = doc["icon"].as<String>();
    if (doc["tagline"].as<String>().length())
      id_tagline = doc["tagline"].as<String>();
    if (doc["rules"].as<String>().length())
      id_rules = doc["rules"].as<String>();
    if (doc["footer"].as<String>().length())
      id_footer = doc["footer"].as<String>();
  }
  f.close();
}

// ===================== RUNTIME ADMIN KEY =====================
// The admin key is stored as a PBKDF2-HMAC-SHA256 hash, not plaintext.
// Config::ADMIN_KEY is the run-time fallback if no hashed key is stored.
// On first boot after an older firmware version, a legacy plaintext
// /adminkey.json will be migrated to the hashed format if it is long enough.

const int ADMIN_KEY_MIN_LEN = 12;
const int ADMIN_KEY_PBKDF2_ITERS = 10000;
const int ADMIN_KEY_SALT_BYTES = 16;

String adminKeySalt = "";
String adminKeyHash = "";
int adminKeyIters = ADMIN_KEY_PBKDF2_ITERS;

String sessionToken = "";          // set on successful auth, cleared on reboot/logout/key-change
unsigned long tokenIssuedAt = 0;   // millis() when token was generated
#define TOKEN_LIFETIME_MS 300000UL // 5 minutes

// Brute-force lockout state
const int AUTH_MAX_FAILURES = 5;
const unsigned long AUTH_LOCKOUT_MS = 300000UL; // 5 minutes
const unsigned long AUTH_DELAY_MAX_MS = 10000UL;
int failedAuthAttempts = 0;
unsigned long lastFailedAuthAt = 0;

String bytesToHexString(const uint8_t *bytes, size_t len)
{
  String out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; i++)
  {
    char buf[3];
    snprintf(buf, sizeof(buf), "%02x", bytes[i]);
    out += buf;
  }
  return out;
}

bool hexStringToBytes(const String &hex, uint8_t *out, size_t maxLen, size_t &outLen)
{
  outLen = 0;
  size_t hexLen = hex.length();
  if (hexLen % 2 != 0)
    return false;
  if (hexLen / 2 > maxLen)
    return false;
  for (size_t i = 0; i < hexLen; i += 2)
  {
    char buf[3] = {hex[i], hex[i + 1], '\0'};
    char *end = nullptr;
    long v = strtol(buf, &end, 16);
    if (end != buf + 2)
      return false;
    out[outLen++] = (uint8_t)v;
  }
  return true;
}

void generateSalt(uint8_t *salt, size_t len)
{
  for (size_t i = 0; i < len; i++)
    salt[i] = (uint8_t)esp_random();
}

bool pbkdf2Hash(const String &password, const uint8_t *salt, size_t saltLen,
                int iterations, uint8_t hash[32])
{
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!md_info)
  {
    mbedtls_md_free(&ctx);
    return false;
  }
  if (mbedtls_md_setup(&ctx, md_info, 1) != 0)
  {
    mbedtls_md_free(&ctx);
    return false;
  }
  int ret = mbedtls_pkcs5_pbkdf2_hmac(&ctx,
                                      (const uint8_t *)password.c_str(), password.length(),
                                      salt, saltLen,
                                      iterations,
                                      32, hash);
  mbedtls_md_free(&ctx);
  return ret == 0;
}

// Constant-time compare for two equal-length byte arrays.
bool constantTimeEqualBytes(const uint8_t *a, const uint8_t *b, size_t len)
{
  uint8_t diff = 0;
  for (size_t i = 0; i < len; i++)
    diff |= a[i] ^ b[i];
  return diff == 0;
}

// Constant-time compare for two Arduino Strings.
bool constantTimeEqualString(const String &a, const String &b)
{
  if (a.length() != b.length())
    return false;
  return constantTimeEqualBytes((const uint8_t *)a.c_str(),
                                (const uint8_t *)b.c_str(), a.length());
}

bool setAdminKey(const String &newKey)
{
  if ((int)newKey.length() < ADMIN_KEY_MIN_LEN)
    return false;

  uint8_t salt[ADMIN_KEY_SALT_BYTES];
  generateSalt(salt, sizeof(salt));

  uint8_t hash[32];
  if (!pbkdf2Hash(newKey, salt, sizeof(salt), ADMIN_KEY_PBKDF2_ITERS, hash))
    return false;

  adminKeySalt = bytesToHexString(salt, sizeof(salt));
  adminKeyHash = bytesToHexString(hash, sizeof(hash));
  adminKeyIters = ADMIN_KEY_PBKDF2_ITERS;
  return true;
}

bool verifyAdminKey(const String &submitted)
{
  if (adminKeySalt.length() == 0 || adminKeyHash.length() == 0)
    return false;

  uint8_t salt[ADMIN_KEY_SALT_BYTES];
  size_t saltLen = 0;
  if (!hexStringToBytes(adminKeySalt, salt, sizeof(salt), saltLen) || saltLen != sizeof(salt))
    return false;

  uint8_t storedHash[32];
  size_t storedHashLen = 0;
  if (!hexStringToBytes(adminKeyHash, storedHash, sizeof(storedHash), storedHashLen) || storedHashLen != sizeof(storedHash))
    return false;

  uint8_t computedHash[32];
  if (!pbkdf2Hash(submitted, salt, saltLen, adminKeyIters, computedHash))
    return false;

  return constantTimeEqualBytes(storedHash, computedHash, sizeof(storedHash));
}

void saveAdminKeyHash()
{
  DynamicJsonDocument doc(512);
  doc["iter"] = adminKeyIters;
  doc["salt"] = adminKeySalt;
  doc["hash"] = adminKeyHash;
  File tmp = LittleFS.open("/adminkey.tmp", FILE_WRITE);
  if (!tmp)
    return;
  serializeJson(doc, tmp);
  tmp.close();
  LittleFS.remove("/adminkey.json");
  LittleFS.rename("/adminkey.tmp", "/adminkey.json");
}

void loadAdminKeyHash()
{
  if (!LittleFS.exists("/adminkey.json"))
    return;
  File f = LittleFS.open("/adminkey.json");
  if (!f)
    return;
  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, f))
  {
    f.close();
    return;
  }

  if (doc["hash"])
  {
    // New hashed format
    adminKeyIters = doc["iter"] | ADMIN_KEY_PBKDF2_ITERS;
    adminKeySalt = doc["salt"] | "";
    adminKeyHash = doc["hash"] | "";
  }
  else if (doc["key"])
  {
    // Legacy plaintext format — migrate if it meets the new minimum length
    String legacyKey = doc["key"] | "";
    if ((int)legacyKey.length() >= ADMIN_KEY_MIN_LEN)
    {
      if (setAdminKey(legacyKey))
        saveAdminKeyHash();
    }
  }
  f.close();
}

String generateToken()
{
  String token = "";
  for (int i = 0; i < 4; i++)
  {
    uint32_t r = esp_random();
    char chunk[9];
    snprintf(chunk, sizeof(chunk), "%08x", r);
    token += chunk;
  }
  tokenIssuedAt = millis();
  return token;
}

// ===================== OTA PHYSICAL ENABLE & VERSION =====================
int otaLastVersion = 0;
const char *OTA_VERSION_FILE = "/otaversion.json";

void loadOtaVersion()
{
  if (!LittleFS.exists(OTA_VERSION_FILE))
    return;
  File f = LittleFS.open(OTA_VERSION_FILE);
  if (!f)
    return;
  DynamicJsonDocument doc(128);
  if (!deserializeJson(doc, f))
    otaLastVersion = doc["version"] | 0;
  f.close();
}

void saveOtaVersion(int v)
{
  DynamicJsonDocument doc(128);
  doc["version"] = v;
  File tmp = LittleFS.open("/otaversion.tmp", FILE_WRITE);
  if (!tmp)
    return;
  serializeJson(doc, tmp);
  tmp.close();
  LittleFS.remove(OTA_VERSION_FILE);
  LittleFS.rename("/otaversion.tmp", OTA_VERSION_FILE);
}

bool otaEnabled = false;
unsigned long otaEnabledUntil = 0;
unsigned long lastOtaButtonCheck = 0;
volatile bool otaSuccess = false;
String otaMessage = "";

void updateOtaButton()
{
  if (Config::OTA_BUTTON_PIN < 0)
    return;
  if (millis() - lastOtaButtonCheck < 250)
    return;
  lastOtaButtonCheck = millis();
  if (digitalRead(Config::OTA_BUTTON_PIN) == LOW)
  {
    otaEnabled = true;
    otaEnabledUntil = millis() + Config::OTA_ENABLE_MS;
    Serial.println("[OTA] button pressed — OTA enabled for 5 min");
  }
}

bool otaWindowOpen()
{
  return otaEnabled && millis() < otaEnabledUntil;
}

// ===================== OTA CRYPTO HELPERS =====================
bool base64DecodeString(const String &b64, uint8_t *&out, size_t &outLen)
{
  size_t needed = 0;
  int ret = mbedtls_base64_decode(nullptr, 0, &needed,
                                  (const uint8_t *)b64.c_str(), b64.length());
  if (ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || needed == 0)
    return false;
  out = (uint8_t *)malloc(needed);
  if (!out)
    return false;
  ret = mbedtls_base64_decode(out, needed, &outLen,
                              (const uint8_t *)b64.c_str(), b64.length());
  if (ret != 0)
  {
    free(out);
    out = nullptr;
    return false;
  }
  return true;
}

void bytesToHex(const uint8_t *in, size_t len, char *out)
{
  for (size_t i = 0; i < len; i++)
    sprintf(out + i * 2, "%02x", in[i]);
  out[len * 2] = '\0';
}

bool sha256String(const String &msg, uint8_t hash[32])
{
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  if (mbedtls_sha256_starts_ret(&ctx, 0) != 0)
  {
    mbedtls_sha256_free(&ctx);
    return false;
  }
  if (mbedtls_sha256_update_ret(&ctx, (const uint8_t *)msg.c_str(), msg.length()) != 0)
  {
    mbedtls_sha256_free(&ctx);
    return false;
  }
  if (mbedtls_sha256_finish_ret(&ctx, hash) != 0)
  {
    mbedtls_sha256_free(&ctx);
    return false;
  }
  mbedtls_sha256_free(&ctx);
  return true;
}

bool verifyOtaSignature(const uint8_t *msgHash, size_t msgHashLen,
                        const uint8_t *sig, size_t sigLen)
{
  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  int ret = mbedtls_pk_parse_public_key(&pk,
                                        (const uint8_t *)OTA_PUBLIC_KEY_PEM,
                                        strlen(OTA_PUBLIC_KEY_PEM) + 1);
  if (ret != 0)
  {
    mbedtls_pk_free(&pk);
    return false;
  }
  ret = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, msgHash, msgHashLen, sig, sigLen);
  mbedtls_pk_free(&pk);
  return ret == 0;
}

// ===================== RUNTIME LED SETTINGS =====================
// These start from Config defaults but can be changed via the admin panel and are persisted to /led.json on the SD card.

int led_day_brightness = Config::LED_DAY_BRIGHTNESS;
int led_night_brightness = Config::LED_NIGHT_BRIGHTNESS;
int led_night_start = Config::NIGHT_START_HOUR;
int led_day_start = Config::DAY_START_HOUR;
int led_pin = Config::LED_PIN;
bool led_enabled = true;
bool led_pulse_enabled = true;    // sine-wave pulsing on/off
bool led_activity_enabled = true; // faster pulse on recent post activity

void saveLedConfig()
{
  DynamicJsonDocument doc(512);
  doc["day_br"] = led_day_brightness;
  doc["night_br"] = led_night_brightness;
  doc["night_st"] = led_night_start;
  doc["day_st"] = led_day_start;
  doc["pin"] = led_pin;
  doc["enabled"] = led_enabled;
  doc["pulse"] = led_pulse_enabled;
  doc["activity"] = led_activity_enabled;

  File tmp = LittleFS.open("/led.tmp", FILE_WRITE);
  if (!tmp)
    return;
  serializeJson(doc, tmp);
  tmp.close();
  LittleFS.remove(Config::LEDCFG_FILE);
  LittleFS.rename("/led.tmp", Config::LEDCFG_FILE);
}
void loadLedConfig()
{
  if (!LittleFS.exists(Config::LEDCFG_FILE))
    return;
  File f = LittleFS.open(Config::LEDCFG_FILE);
  if (!f)
    return;
  DynamicJsonDocument doc(512);
  if (!deserializeJson(doc, f))
  {
    led_day_brightness = doc["day_br"] | Config::LED_DAY_BRIGHTNESS;
    led_night_brightness = doc["night_br"] | Config::LED_NIGHT_BRIGHTNESS;
    led_night_start = doc["night_st"] | Config::NIGHT_START_HOUR;
    led_day_start = doc["day_st"] | Config::DAY_START_HOUR;
    led_pin = doc["pin"] | Config::LED_PIN;
    led_enabled = doc["enabled"] | true;
    led_pulse_enabled = doc["pulse"] | true;
    led_activity_enabled = doc["activity"] | true;
  }
  f.close();
}

// Allowed GPIOs for the LED pin. Board-specific: excludes SPI flash, input-only,
// and UART pins. The default LED_PIN must be in this list.
const int ALLOWED_LED_PINS[] = {0, 2, 4, 12, 13, 14, 15, 21, 22, 25, 26, 32, 33};
const int ALLOWED_LED_PIN_COUNT = sizeof(ALLOWED_LED_PINS) / sizeof(ALLOWED_LED_PINS[0]);

bool isAllowedLedPin(int pin)
{
  for (int i = 0; i < ALLOWED_LED_PIN_COUNT; i++)
  {
    if (ALLOWED_LED_PINS[i] == pin)
      return true;
  }
  return false;
}

// ===================== TIME =====================
unsigned long baseEpoch = 0; // We use UNIX time in this house, son.
unsigned long baseMillis = 0;
unsigned long lastTimeSave = 0;

unsigned long nowSecs()
{
  return baseEpoch + (millis() - baseMillis) / 1000;
}

bool setTimeFromString(String t)
{
  if (t.length() != 13)
    return false;
  struct tm tm;
  memset(&tm, 0, sizeof(tm));
  tm.tm_mday = t.substring(0, 2).toInt();
  tm.tm_mon = t.substring(2, 4).toInt() - 1;
  tm.tm_year = t.substring(4, 8).toInt() - 1900;
  tm.tm_hour = t.substring(9, 11).toInt();
  tm.tm_min = t.substring(11, 13).toInt();
  time_t epoch = mktime(&tm);
  if (epoch <= 0)
    return false;
  baseEpoch = epoch;
  baseMillis = millis();
  return true;
}

void saveTime()
{ // "I save more time with this one lifehack than any other way! Like and subscribe for more hastag relateable content."
  DynamicJsonDocument doc(256);
  doc["epoch"] = nowSecs();
  File tmp = LittleFS.open("/time.tmp", FILE_WRITE);
  if (!tmp)
    return;
  serializeJson(doc, tmp);
  tmp.close();
  LittleFS.remove(Config::TIME_FILE);
  LittleFS.rename("/time.tmp", Config::TIME_FILE);
}

void loadTime()
{
  if (!LittleFS.exists(Config::TIME_FILE))
    return;
  File f = LittleFS.open(Config::TIME_FILE);
  if (!f)
    return;
  DynamicJsonDocument doc(256);
  if (!deserializeJson(doc, f))
  {
    baseEpoch = doc["epoch"];
    baseMillis = millis();
  }
  f.close();
}

int currentHour()
{
  time_t t = nowSecs();
  struct tm *tm = localtime(&t);
  return tm ? tm->tm_hour : 12;
}

// ===================== UPTIME =====================
unsigned long bootMillis = 0;

String formatUptime()
{
  unsigned long secs = (millis() - bootMillis) / 1000;
  unsigned long days = secs / 86400;
  secs %= 86400;
  unsigned long hours = secs / 3600;
  secs %= 3600;
  unsigned long mins = secs / 60;
  char buf[32];
  if (days > 0)
    snprintf(buf, sizeof(buf), "↑ %lud %luh %lum", days, hours, mins);
  else if (hours > 0)
    snprintf(buf, sizeof(buf), "↑ %luh %lum", hours, mins);
  else
    snprintf(buf, sizeof(buf), "↑ %lum", mins);
  return String(buf);
}

// ===================== MESSAGES =====================
struct Message
{
  uint16_t id;
  String author;
  String type;
  String text;
  unsigned long expires;
};

Message msgs[Config::MAX_MSGS];
int msgCount = 0;
uint16_t nextMsgId = 1;
unsigned long lastPostTime = 0;

bool msgsDirty = false; // Ooh you're so dirty.
unsigned long lastMsgDirtyTime = 0;

// Per-client rate limiting state for /post
struct RateLimitEntry
{
  IPAddress clientIp;
  unsigned long lastPostSecs;
};
RateLimitEntry postRateLimit[Config::RATE_LIMIT_TABLE_SIZE];

// Returns true if the client may post now, false if they are rate-limited.
bool checkPostRateLimit(IPAddress ip)
{
  unsigned long now = nowSecs();
  int emptySlot = -1;
  int oldestSlot = -1;
  unsigned long oldestTime = ULONG_MAX;

  for (int i = 0; i < Config::RATE_LIMIT_TABLE_SIZE; i++)
  {
    if (postRateLimit[i].clientIp == ip)
    {
      if (now - postRateLimit[i].lastPostSecs < Config::POST_RATE_LIMIT_SECONDS)
        return false;
      postRateLimit[i].lastPostSecs = now;
      return true;
    }

    if (postRateLimit[i].clientIp == IPAddress(0, 0, 0, 0))
    {
      if (emptySlot < 0)
        emptySlot = i;
    }
    else if (postRateLimit[i].lastPostSecs < oldestTime)
    {
      oldestTime = postRateLimit[i].lastPostSecs;
      oldestSlot = i;
    }
  }

  int slot = (emptySlot >= 0) ? emptySlot : oldestSlot;
  if (slot < 0)
    return true; // Should never happen, but fail open rather than block posting

  postRateLimit[slot].clientIp = ip;
  postRateLimit[slot].lastPostSecs = now;
  return true;
}

void saveMessages()
{
  DynamicJsonDocument doc(81920); // ~80KB; sized for 200 worst-case messages
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < msgCount; i++)
  {
    JsonObject o = arr.createNestedObject();
    o["id"] = msgs[i].id;
    o["author"] = msgs[i].author;
    o["type"] = msgs[i].type;
    o["text"] = msgs[i].text;
    o["expires"] = msgs[i].expires;
  }
  File tmp = LittleFS.open("/msgs.tmp", FILE_WRITE);
  if (!tmp)
    return;
  serializeJson(doc, tmp);
  tmp.close();
  LittleFS.remove(Config::STORAGE_FILE);
  LittleFS.rename("/msgs.tmp", Config::STORAGE_FILE);
}

void loadMessages()
{
  if (!LittleFS.exists(Config::STORAGE_FILE))
    return;
  File f = LittleFS.open(Config::STORAGE_FILE);
  if (!f)
    return;
  DynamicJsonDocument doc(81920); // ~80KB; sized for 200 worst-case messages
  if (deserializeJson(doc, f))
  {
    f.close();
    return;
  }
  JsonArray arr = doc.as<JsonArray>();
  msgCount = 0;
  for (JsonObject o : arr)
  {
    if (msgCount >= Config::MAX_MSGS)
      break;
    msgs[msgCount].id = o["id"] | nextMsgId;
    msgs[msgCount].author = (const char *)o["author"];
    msgs[msgCount].type = (const char *)o["type"];
    msgs[msgCount].text = (const char *)o["text"];
    msgs[msgCount].expires = o["expires"];
    if (msgs[msgCount].id >= nextMsgId)
      nextMsgId = msgs[msgCount].id + 1;
    msgCount++;
  }
  f.close();
}

void addMessage(String author, String type, String text, int expiryHours)
{
  if (msgCount >= Config::MAX_MSGS)
  {
    // Find the oldest expired post and evict it
    unsigned long now = nowSecs();
    int evict = -1;
    unsigned long oldest = ULONG_MAX;
    for (int i = 0; i < msgCount; i++)
    {
      if (msgs[i].expires <= now && msgs[i].expires < oldest)
      {
        oldest = msgs[i].expires;
        evict = i;
      }
    }
    if (evict < 0)
      return; // No expired posts — board is genuinely full
    // Shift everything above the evicted slot down one
    for (int i = evict; i < msgCount - 1; i++)
      msgs[i] = msgs[i + 1];
    msgCount--;
  }
  msgs[msgCount].id = nextMsgId++;
  msgs[msgCount].author = author;
  msgs[msgCount].type = type;
  msgs[msgCount].text = text;
  msgs[msgCount].expires = nowSecs() + expiryHours * 3600;
  msgCount++;
  lastPostTime = nowSecs();
  if (!msgsDirty)
  {
    msgsDirty = true;
    lastMsgDirtyTime = millis();
  }
}

// ===================== LED =====================
// Non-blocking blink so the web/DNS servers never stall.
// Preserves the old pattern: 100 ms on, 3 s off.
unsigned long ledLastChange = 0;
bool ledState = false;

void updateLED()
{
  unsigned long now = millis();
  unsigned long interval = ledState ? 100UL : 2000UL;
  if (now - ledLastChange >= interval)
  {
    ledState = !ledState;
    digitalWrite(led, ledState ? HIGH : LOW);
    ledLastChange = now;
  }
}

// ===================== HTML: MAIN BOARD =====================
// Imported from frontend.html

// Escapes a string for safe injection into a JS double-quoted string literal.
String jsEscape(const String &s)
{
  String out;
  out.reserve(s.length());
  for (unsigned int i = 0; i < s.length(); i++)
  {
    char c = s.charAt(i);
    if (c == '"')
      out += "\\\"";
    else if (c == '\\')
      out += "\\\\";
    else if (c == '\n')
      out += "\\n";
    else if (c == '\r')
      out += "\\r";
    else
      out += c;
  }
  return out;
}

// ===================== HTML: ADMIN PANEL =====================
// The admin key is NEVER sent to the browser.
// The gate POSTs the key to /admin/auth which returns a session token.
// All subsequent admin calls send the token in the Authorization header.

// String buildAdminPage()
// {
//   String page = F(R"rawliteral(

// )rawliteral");

//   return page;
// }

String buildAdminPage()
{
  // 1. Open the admin.html file from LittleFS
  File file = LittleFS.open("/admin.html", "r");
  if (!file)
  {
    return "<h1>Error: admin.html not found in LittleFS</h1>";
  }

  // 2. Read the entire file into a String
  String html = file.readString();
  file.close();

  // 3. Build the dynamic JavaScript string (the part you wanted to keep in C++)
  String jsData = "let SESSION_TOKEN = '';\n";
  jsData += "window.addEventListener('DOMContentLoaded', () => {\n";
  jsData += "  document.getElementById('idName').value    = \"" + jsEscape(id_name) + "\";\n";
  jsData += "  document.getElementById('idIcon').value    = \"" + jsEscape(id_icon) + "\";\n";
  jsData += "  document.getElementById('idTagline').value = \"" + jsEscape(id_tagline) + "\";\n";
  jsData += "  document.getElementById('idRules').value   = \"" + jsEscape(id_rules) + "\";\n";
  jsData += "  document.getElementById('idFooter').value  = \"" + jsEscape(id_footer) + "\";\n";
  jsData += "});\n";

  // 4. Replace the placeholder in the HTML with the dynamic JS
  html.replace("<!-- JS_INJECTION -->", jsData);

  return html;
}

// ===================== WEB SERVER =====================
DNSServer dnsServer;
WebServer server(80);

bool checkKey()
{ // WOTS DA PASSWARD?
  if (sessionToken.length() == 0)
    return false;
  if (millis() - tokenIssuedAt > TOKEN_LIFETIME_MS)
    return false;
  String authHeader = server.header("Authorization");
  if (!authHeader.startsWith("Bearer "))
    return false;
  return constantTimeEqualString(authHeader.substring(7), sessionToken);
}

// Strip angle brackets and trim whitespace to prevent HTML injection.
// Applied to all user-supplied text before storage.
// Because users are hostile, whether they mean to be or not.
String sanitize(const String &s, int maxLen)
{
  String out;
  out.reserve(s.length());
  for (unsigned int i = 0; i < s.length(); i++)
  {
    char c = s.charAt(i);
    if (c != '<' && c != '>')
      out += c;
  }
  out.trim();
  if ((int)out.length() > maxLen)
    out = out.substring(0, maxLen);
  return out;
}

// Accept only the four known post types; fall back to "Notice".
String validateType(const String &t)
{
  if (t == "Notice" || t == "Offer" || t == "Need" || t == "Event")
    return t;
  return "Notice";
}

void handleRoot()
{
  File file = LittleFS.open("/frontend.html", "r");
  if (!file)
  {
    server.send(404, "text/plain", "File not found");
    return;
  }
  server.streamFile(file, "text/html");
  file.close();
}

void handleAdmin()
{
  File file = LittleFS.open("/admin.html", "r");
  if (!file)
  {
    server.send(404, "text/plain", "admin.html not found");
    return;
  }
  server.streamFile(file, "text/html");
  file.close();
}

bool isAuthLockedOut()
{
  if (failedAuthAttempts >= AUTH_MAX_FAILURES)
  {
    if (millis() - lastFailedAuthAt < AUTH_LOCKOUT_MS)
      return true;
    // Lockout window expired; reset the counter.
    failedAuthAttempts = 0;
  }
  return false;
}

void recordAuthFailure()
{
  failedAuthAttempts++;
  lastFailedAuthAt = millis();
  // Exponential response delay capped at AUTH_DELAY_MAX_MS.
  unsigned long shift = (failedAuthAttempts > 10) ? 10 : (unsigned long)failedAuthAttempts;
  unsigned long delayMs = 250UL * (1UL << shift);
  if (delayMs > AUTH_DELAY_MAX_MS)
    delayMs = AUTH_DELAY_MAX_MS;
  delay(delayMs);
}

void recordAuthSuccess()
{
  failedAuthAttempts = 0;
}

void handleAdminAuth()
{
  // Key submitted via POST body as JSON: {"key":"..."}
  // Never echoed back — only a token is returned on success.
  if (isAuthLockedOut())
  {
    server.send(429, "text/plain", "too many failed attempts — try again later");
    return;
  }

  DynamicJsonDocument doc(256);
  if (deserializeJson(doc, server.arg("plain")))
  {
    server.send(400, "text/plain", "bad request");
    return;
  }
  String submitted = doc["key"] | "";

  // If no hashed key has been stored yet, fall back to the compile-time default.
  bool keyOk = false;
  if (adminKeyHash.length() == 0)
    keyOk = submitted == String(Config::ADMIN_KEY);
  else
    keyOk = verifyAdminKey(submitted);

  if (keyOk)
  {
    sessionToken = generateToken();
    recordAuthSuccess();
    server.send(200, "text/plain", sessionToken);
  }
  else
  {
    recordAuthFailure();
    server.send(403, "text/plain", "forbidden");
  }
}

void handleInfo()
{
  DynamicJsonDocument doc(512);
  doc["name"] = id_name;
  doc["icon"] = id_icon;
  doc["tagline"] = id_tagline;
  doc["rules"] = id_rules;
  doc["footer"] = id_footer;
  doc["uptime"] = formatUptime();
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleMessages()
{
  DynamicJsonDocument doc(8192);
  JsonArray arr = doc.to<JsonArray>();
  unsigned long now = nowSecs();
  for (int i = 0; i < msgCount; i++)
  {
    if (msgs[i].expires < now)
      continue;
    JsonObject o = arr.createNestedObject();
    o["id"] = msgs[i].id;
    o["author"] = msgs[i].author;
    o["type"] = msgs[i].type;
    o["text"] = msgs[i].text;
    o["expires"] = msgs[i].expires;
  }
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handlePost()
{
  if (!checkPostRateLimit(server.client().remoteIP()))
  {
    server.send(429, "text/plain", "rate limited");
    return;
  }

  DynamicJsonDocument doc(1024);
  if (deserializeJson(doc, server.arg("plain")))
  {
    server.send(400, "text/plain", "bad json");
    return;
  }
  String author = sanitize(doc["author"] | "neighbor", 24); // Won't you be my neighbor?
  String type = validateType(doc["type"] | "Notice");
  String text = sanitize(doc["text"] | "", 300);
  int expiry = doc["expiry"] | Config::DEFAULT_EXPIRY_HOURS;
  if (expiry < Config::MIN_EXPIRY_HOURS)
    expiry = Config::MIN_EXPIRY_HOURS;
  if (expiry > Config::MAX_EXPIRY_HOURS)
    expiry = Config::MAX_EXPIRY_HOURS;

  if (author.isEmpty())
    author = "neighbor";
  if (text.isEmpty())
  {
    server.send(400, "text/plain", "empty message");
    return;
  }

  addMessage(author, type, text, expiry);
  saveMessages();    // persist immediately on POST
  msgsDirty = false; // just saved, clear deferred-dirty flag
  server.send(200, "text/plain", "ok");
}

// ── Admin handlers ────────────────────────────────────────────────────────────
void handleAdminIdentityGet()
{
  if (!checkKey())
  {
    server.send(403, "text/plain", "forbidden");
    return;
  }
  DynamicJsonDocument doc(1024);
  doc["name"] = id_name;
  doc["icon"] = id_icon;
  doc["tagline"] = id_tagline;
  doc["rules"] = id_rules;
  doc["footer"] = id_footer;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleAdminIdentitySet()
{
  if (!checkKey())
  {
    server.send(403, "text/plain", "forbidden");
    return;
  }
  if (server.hasArg("name") && server.arg("name").length())
    id_name = sanitize(server.arg("name"), 48);
  if (server.hasArg("icon") && server.arg("icon").length())
    id_icon = sanitize(server.arg("icon"), 8);
  if (server.hasArg("tagline"))
    id_tagline = sanitize(server.arg("tagline"), 100);
  if (server.hasArg("rules"))
    id_rules = sanitize(server.arg("rules"), 100);
  if (server.hasArg("footer"))
    id_footer = sanitize(server.arg("footer"), 100);
  saveIdentityConfig();
  server.send(200, "text/plain", "identity saved");
}

void handleAdminTime()
{
  if (!checkKey())
  {
    server.send(403, "text/plain", "forbidden");
    return;
  }
  if (!setTimeFromString(server.arg("time")))
  {
    server.send(400, "text/plain", "bad format — use DDMMYYYY-HHMM");
    return;
  }
  saveTime();
  server.send(200, "text/plain", "time set");
}
// bored
void handleAdminLedGet()
{
  if (!checkKey())
  {
    server.send(403, "text/plain", "forbidden");
    return;
  }
  DynamicJsonDocument doc(512);
  doc["day_br"] = led_day_brightness;
  doc["night_br"] = led_night_brightness;
  doc["day_st"] = led_day_start;
  doc["night_st"] = led_night_start;
  doc["pin"] = led_pin;
  doc["enabled"] = led_enabled;
  doc["pulse"] = led_pulse_enabled;
  doc["activity"] = led_activity_enabled;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}
// so bored
void handleAdminLedSet()
{
  if (!checkKey())
  {
    server.send(403, "text/plain", "forbidden");
    return;
  }
  if (server.hasArg("day_br"))
    led_day_brightness = constrain(server.arg("day_br").toInt(), 0, 100);
  if (server.hasArg("night_br"))
    led_night_brightness = constrain(server.arg("night_br").toInt(), 0, 100);
  if (server.hasArg("day_st"))
    led_day_start = constrain(server.arg("day_st").toInt(), 0, 23);
  if (server.hasArg("night_st"))
    led_night_start = constrain(server.arg("night_st").toInt(), 0, 23);
  if (server.hasArg("pin"))
  {
    int newPin = server.arg("pin").toInt();
    if (newPin != led_pin && isAllowedLedPin(newPin))
    {
      analogWrite(led_pin, 0);
      pinMode(led_pin, INPUT);
      led_pin = newPin;
      pinMode(led_pin, OUTPUT);
    }
  }
  if (server.hasArg("enabled"))
    led_enabled = server.arg("enabled") == "1";
  if (server.hasArg("pulse"))
    led_pulse_enabled = server.arg("pulse") == "1";
  if (server.hasArg("activity"))
    led_activity_enabled = server.arg("activity") == "1";
  saveLedConfig();
  server.send(200, "text/plain", "LED settings saved");
}

void handleAdminBackup()
{ // C'mon shawty, back that data up!
  if (!checkKey())
  {
    server.send(403, "text/plain", "forbidden");
    return;
  }
  if (msgsDirty)
  {
    saveMessages();
    msgsDirty = false;
  }
  File f = LittleFS.open(Config::STORAGE_FILE);
  if (!f)
  {
    server.send(500, "text/plain", "no file");
    return;
  }
  String out = f.readString();
  f.close();
  server.send(200, "application/json", out);
}

void handleAdminRestore()
{
  if (!checkKey())
  {
    server.send(403, "text/plain", "forbidden");
    return;
  }
  DynamicJsonDocument doc(81920); // same capacity as saveMessages()/loadMessages()
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err)
  {
    if (err.code() == DeserializationError::NoMemory)
      server.send(400, "text/plain", "backup too large");
    else
      server.send(400, "text/plain", "bad json");
    return;
  }
  JsonArray arr = doc.as<JsonArray>();
  if (!arr)
  {
    server.send(400, "text/plain", "expected a JSON array");
    return;
  }
  msgCount = 0;
  unsigned long now = nowSecs();
  unsigned long maxExpires = now + Config::MAX_EXPIRY_FUTURE_SECONDS;
  for (JsonObject o : arr)
  {
    if (msgCount >= Config::MAX_MSGS)
      break;
    String author = sanitize(o["author"] | "neighbor", 24);
    if (author.isEmpty())
      author = "neighbor";
    String text = sanitize(o["text"] | "", 300);
    if (text.isEmpty())
      continue; // Skip empty messages; same rule as /post.
    String type = validateType(o["type"] | "Notice");
    unsigned long expires = o["expires"] | 0;
    if (expires > maxExpires)
      expires = maxExpires;
    msgs[msgCount].id = nextMsgId++;
    msgs[msgCount].author = author;
    msgs[msgCount].type = type;
    msgs[msgCount].text = text;
    msgs[msgCount].expires = expires;
    msgCount++;
  }
  saveMessages();
  server.send(200, "text/plain", "restored " + String(msgCount) + " messages");
}

void handleAdminSetKey()
{
  if (!checkKey())
  {
    server.send(403, "text/plain", "forbidden");
    return;
  }
  String newKey = server.arg("newkey");
  newKey.trim();
  if ((int)newKey.length() < ADMIN_KEY_MIN_LEN)
  {
    server.send(400, "text/plain", "key must be at least 12 characters");
    return;
  }
  if (!setAdminKey(newKey))
  {
    server.send(500, "text/plain", "failed to hash key");
    return;
  }
  saveAdminKeyHash();
  // Invalidate any existing session so captured/old tokens die immediately.
  sessionToken = "";
  server.send(200, "text/plain", "key updated — log in again");
}


void handleAdminFlush()
{
  if (!checkKey())
  {
    server.send(403, "text/plain", "forbidden");
    return;
  }
  saveMessages();
  msgsDirty = false;
  saveTime();
  server.send(200, "text/plain", "flushed");
}

void handleAdminLogout()
{
  // Allow logout even if the token just expired — if the header matches the
  // current session token we still clear it so the client-side gate returns.
  String authHeader = server.header("Authorization");
  if (authHeader.startsWith("Bearer ") && authHeader.substring(7) == sessionToken)
    sessionToken = "";
  server.send(200, "text/plain", "logged out");
}

void handleAdminDeletePost()
{
  if (!checkKey())
  {
    server.send(403, "text/plain", "forbidden");
    return;
  }
  if (!server.hasArg("id"))
  {
    server.send(400, "text/plain", "missing id");
    return;
  }
  uint16_t targetId = (uint16_t)server.arg("id").toInt();
  for (int i = 0; i < msgCount; i++)
  {
    if (msgs[i].id == targetId)
    {
      // Shift remaining messages down to fill the gap
      for (int j = i; j < msgCount - 1; j++)
        msgs[j] = msgs[j + 1];
      msgCount--;
      if (!msgsDirty)
      {
        msgsDirty = true;
        lastMsgDirtyTime = millis();
      }
      server.send(200, "text/plain", "deleted");
      return;
    }
  }
  server.send(404, "text/plain", "not found");
}

void handleAdminClear()
{
  if (!checkKey())
  {
    server.send(403, "text/plain", "forbidden");
    return;
  }
  msgCount = 0;
  saveMessages();
  server.send(200, "text/plain", "cleared");
}

// ===================== OTA HANDLERS =====================
size_t otaExpectedSize = 0;
size_t otaReceivedSize = 0;
bool otaUpdateStarted = false;
mbedtls_sha256_context otaShaCtx;

void handleAdminOTA()
{
  if (!checkKey())
  {
    server.send(403, "text/plain", "forbidden");
    return;
  }
  if (!otaWindowOpen())
  {
    server.send(403, "text/plain", "ota disabled — press the OTA button");
    return;
  }
  if (otaSuccess)
  {
    server.send(200, "text/plain", otaMessage);
    delay(500);
    ESP.restart();
  }
  else
  {
    server.send(400, "text/plain", otaMessage.length() ? otaMessage : String("UPDATE FAILED"));
  }
}

void handleAdminOTAUpload()
{
  if (!checkKey() || !otaWindowOpen())
    return;
  HTTPUpload &upload = server.upload();

  if (upload.status == UPLOAD_FILE_START)
  {
    otaSuccess = false;
    otaMessage = "";
    otaReceivedSize = 0;
    otaUpdateStarted = false;

    // Debug: print every query arg so we can see what actually arrived.
    Serial.printf("[OTA] %d query arg(s)\n", server.args());
    for (int i = 0; i < server.args(); i++)
      Serial.printf("  %s=%s\n", server.argName(i).c_str(), server.arg(i).c_str());

    String sizeArg = server.arg("size");
    otaExpectedSize = (size_t)sizeArg.toInt();
    Serial.printf("[OTA] size arg '%s' parsed as %u\n", sizeArg.c_str(), otaExpectedSize);

    if (sizeArg.length() == 0)
    {
      otaMessage = "missing size";
      return;
    }
    if (otaExpectedSize == 0)
    {
      otaMessage = "bad firmware size";
      return;
    }
    if (otaExpectedSize > Config::OTA_MAX_SIZE)
    {
      Serial.printf("[OTA] size %u > max %u\n", otaExpectedSize, Config::OTA_MAX_SIZE);
      otaMessage = "firmware too large";
      return;
    }
    mbedtls_sha256_init(&otaShaCtx);
    if (mbedtls_sha256_starts_ret(&otaShaCtx, 0) != 0)
    {
      otaMessage = "hash init failed";
      return;
    }
    Serial.printf("[OTA] starting: %s (%u bytes)\n", upload.filename.c_str(), otaExpectedSize);
    if (!Update.begin(otaExpectedSize))
    {
      Update.printError(Serial);
      otaMessage = "Update.begin failed";
      return;
    }
    otaUpdateStarted = true;
  }
  else if (upload.status == UPLOAD_FILE_WRITE)
  {
    if (!otaUpdateStarted)
      return;
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
    {
      Update.printError(Serial);
      otaMessage = "flash write failed";
      otaUpdateStarted = false;
      return;
    }
    if (mbedtls_sha256_update_ret(&otaShaCtx, upload.buf, upload.currentSize) != 0)
    {
      otaMessage = "hash update failed";
      otaUpdateStarted = false;
      return;
    }
    otaReceivedSize += upload.currentSize;
  }
  else if (upload.status == UPLOAD_FILE_END)
  {
    if (!otaUpdateStarted)
      return;
    if (otaReceivedSize != otaExpectedSize)
    {
      otaMessage = "size mismatch";
      Update.abort();
      otaUpdateStarted = false;
      return;
    }
    uint8_t fwHash[32];
    if (mbedtls_sha256_finish_ret(&otaShaCtx, fwHash) != 0)
    {
      otaMessage = "hash finish failed";
      Update.abort();
      otaUpdateStarted = false;
      return;
    }
    mbedtls_sha256_free(&otaShaCtx);

    char fwHashHex[65];
    bytesToHex(fwHash, 32, fwHashHex);

    String versionStr = server.arg("version");
    int version = versionStr.toInt();
    if (versionStr.length() == 0 || version <= 0)
    {
      otaMessage = "bad version";
      Update.abort();
      otaUpdateStarted = false;
      return;
    }
    if (version <= otaLastVersion)
    {
      otaMessage = "downgrade rejected";
      Update.abort();
      otaUpdateStarted = false;
      return;
    }

    String sigB64 = server.arg("sig");
    uint8_t *sig = nullptr;
    size_t sigLen = 0;
    if (!base64DecodeString(sigB64, sig, sigLen) || sigLen == 0)
    {
      otaMessage = "bad signature encoding";
      Update.abort();
      otaUpdateStarted = false;
      return;
    }

    String msg = versionStr + "|" + String(fwHashHex);
    uint8_t msgHash[32];
    if (!sha256String(msg, msgHash))
    {
      free(sig);
      otaMessage = "message hash failed";
      Update.abort();
      otaUpdateStarted = false;
      return;
    }

    if (!verifyOtaSignature(msgHash, 32, sig, sigLen))
    {
      free(sig);
      otaMessage = "signature verify failed";
      Update.abort();
      otaUpdateStarted = false;
      return;
    }
    free(sig);

    if (Update.end(true))
    {
      saveOtaVersion(version);
      otaSuccess = true;
      otaMessage = "UPDATE OK — rebooting";
      Serial.printf("[OTA] success, version %d\n", version);
    }
    else
    {
      Update.printError(Serial);
      otaMessage = "Update.end failed";
    }
    otaUpdateStarted = false;
  }
}

// ===================== SETUP =====================
void setup()
{
  pinMode(led, OUTPUT);

  bootMillis = millis();
  Serial.begin(115200);
  delay(500);

  initTft();

  Serial.println(); 
  Serial.println("╔═══════════════════════════════╗");
  Serial.println("║       ·················       ║");
  Serial.println("║       · mssg ina bttl ·       ║");
  Serial.println("║       ·················       ║");
  Serial.println("╚═══════════════════════════════╝");

  pinMode(led_pin, OUTPUT);

  // ── OTA physical enable button ──
  if (Config::OTA_BUTTON_PIN >= 0)
  {
    // SW38 on GPIO 38 already has an onboard pull-up; other pins need one.
    if (Config::OTA_BUTTON_PIN == 38)
      pinMode(Config::OTA_BUTTON_PIN, INPUT);
    else
      pinMode(Config::OTA_BUTTON_PIN, INPUT_PULLUP);
  }

  // ── WiFi Access Point ──
  WiFi.mode(WIFI_AP);

  IPAddress apIP(10, 0, 0, 10);
  IPAddress apGW(10, 0, 0, 10);
  IPAddress apSN(255, 255, 255, 0);
  WiFi.softAPConfig(apIP, apGW, apSN);

  WiFi.softAP(Config::AP_SSID, Config::AP_PASS[0] ? Config::AP_PASS : nullptr,
              Config::AP_CHANNEL, 0, Config::AP_MAX_CONN);
  delay(200); // softAP needs a moment to settle
  Serial.println("✓ Access Point started.");
  Serial.print("  SSID : ");
  Serial.println(Config::AP_SSID);
  Serial.print("  IP   : ");
  Serial.println(apIP);
  if (Config::AP_PASS[0])
  {
    Serial.print("  Pass : ");
    Serial.println(Config::AP_PASS);
    Serial.println("  Sec  : WPA2-PSK (password is in the SSID)");
  }
  else
  {
    Serial.println("  Sec  : OPEN — anyone can join and sniff traffic");
  }
  Serial.print("  Admin: http://");
  Serial.print(apIP);
  Serial.println("/admin");

  if (String(Config::ADMIN_KEY) == "lavish.meerkat")
  {
    Serial.println("  ⚠ WARNING: using factory default admin key.");
    Serial.println("    Change Config::ADMIN_KEY in src/main.cpp before deploying.");
  }

  // ── DNS — redirect every hostname to us ──
  dnsServer.start(53, "*", apIP);

  // ── LittleFS ──
  if (!LittleFS.begin(true))
  {
    Serial.println("⚠  LittleFS init failed — running without persistence."); // if this fails, we got problems.
  }
  else
  {
    Serial.println("✓ LittleFS mounted.");
    loadTime();
    loadLedConfig();
    loadAdminKeyHash();
    loadOtaVersion();
    loadIdentityConfig();
    loadMessages();
    Serial.printf("  Loaded %d message(s).\n", msgCount);

    // Sanity check: Just check if the file exists
    if (!LittleFS.exists("/frontend.html"))
    {
      Serial.println("⚠ ERROR: /frontend.html is missing from LittleFS!");
    }
    else
    {
      Serial.println("✓ frontend.html found.");
    }
  }

  // ── Routes ──
  server.on("/", handleRoot);
  server.on("/admin", handleAdmin);
  server.on("/admin/auth", HTTP_POST, handleAdminAuth);
  server.on("/info", handleInfo);
  server.on("/messages", handleMessages);
  server.on("/post", HTTP_POST, handlePost);
  server.on("/api/status", HTTP_GET, []()
            {
    unsigned long now = nowSecs();
    bool hasExpired = false;
    for (int i = 0; i < msgCount; i++) {
      if (msgs[i].expires <= now) { hasExpired = true; break; }
    }
    bool boardFull = (msgCount >= Config::MAX_MSGS) && !hasExpired;
    DynamicJsonDocument doc(64);
    doc["full"] = boardFull;
    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out); });

  // ── Captive portal detection endpoints ──────────────────────────────────────
  // DNS resolves ALL hostnames to our IP, so only the path matters.
  // We redirect each known probe URL to "/" to trigger the portal popup.
  // Even with all this, doesn't always work. Samsung devices are especially persnickety.

  // Apple (iOS / macOS)
  server.on("/hotspot-detect.html", []()
            { server.sendHeader("Location", "/"); server.send(302); });
  server.on("/library/test/success.html", []()
            { server.sendHeader("Location", "/"); server.send(302); });
  server.on("/success.html", []()
            { server.sendHeader("Location", "/"); server.send(302); });
  server.on("/captive.apple.com", []()
            { server.sendHeader("Location", "/"); server.send(302); });

  // Android / Google
  server.on("/generate_204", []()
            { server.sendHeader("Location", "/"); server.send(302); });
  server.on("/gen_204", []()
            { server.sendHeader("Location", "/"); server.send(302); });
  server.on("/connectivitycheck", []()
            { server.sendHeader("Location", "/"); server.send(302); });
  server.on("/connectivity-check", []()
            { server.sendHeader("Location", "/"); server.send(302); });

  // Windows (NCSI + connecttest)
  server.on("/connecttest.txt", []()
            { server.sendHeader("Location", "/"); server.send(302); });
  server.on("/ncsi.txt", []()
            { server.sendHeader("Location", "/"); server.send(302); });
  server.on("/redirect", []()
            { server.sendHeader("Location", "/"); server.send(302); });
  server.on("/fwlink/", []()
            { server.sendHeader("Location", "/"); server.send(302); });
  server.on("/fwlink", []()
            { server.sendHeader("Location", "/"); server.send(302); });

  // Firefox browser
  server.on("/success.txt", []()
            { server.sendHeader("Location", "/"); server.send(302); });
  server.on("/canonical.html", []()
            { server.sendHeader("Location", "/"); server.send(302); });

  server.on("/admin/identity/get", handleAdminIdentityGet);
  server.on("/admin/identity/set", HTTP_POST, handleAdminIdentitySet);
  server.on("/admin/time", HTTP_POST, handleAdminTime);
  server.on("/admin/led/get", handleAdminLedGet);
  server.on("/admin/led/set", HTTP_POST, handleAdminLedSet);
  server.on("/admin/backup", handleAdminBackup);
  server.on("/admin/restore", HTTP_POST, handleAdminRestore);
  server.on("/admin/setkey", HTTP_POST, handleAdminSetKey);
  server.on("/admin/flush", HTTP_POST, handleAdminFlush);
  server.on("/admin/logout", HTTP_POST, handleAdminLogout);
  server.on("/admin/clear", HTTP_POST, handleAdminClear);
  server.on("/admin/delete/post", HTTP_POST, handleAdminDeletePost);
  server.on("/admin/ota", HTTP_POST, handleAdminOTA, handleAdminOTAUpload);

  // Serve related files for the admin page
  server.on("/admin.js", []()
            { 
              File file = LittleFS.open("/admin.js", "r");
              server.streamFile(file, "application/javascript"); });
  server.on("/admin.css", []()
            { 
              File file = LittleFS.open("/admin.css", "r");
              server.streamFile(file, "text/css"); });

  // Serve related files for frontend
  server.on("/frontend.js", []()
            { 
              File file = LittleFS.open("/frontend.js", "r");
              server.streamFile(file, "application/javascript"); });
  server.on("/styles.css", []()
            { 
              File file = LittleFS.open("/styles.css", "r");
              server.streamFile(file, "text/css"); });

  // Catch-all: redirect everything else to the board (required for captive portal)
  server.onNotFound([]()
                    { server.sendHeader("Location", "/"); server.send(302); });

  server.begin();
  Serial.println("✓ HTTP server started.\n");
}

// ===================== LOOP =====================
void loop()
{
  dnsServer.processNextRequest();
  server.handleClient();
  updateLED();
  updateOtaButton();
  updateQrButton();

  unsigned long now = millis();

  if (msgsDirty && (now - lastMsgDirtyTime) >= 60000)
  {
    saveMessages();
    msgsDirty = false;
  }

  if (now - lastTimeSave > 1800000)
  {
    saveTime();
    lastTimeSave = now;
  }
}