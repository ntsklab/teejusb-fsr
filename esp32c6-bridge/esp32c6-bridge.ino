/*
 * ESP32-C6 WiFi Bridge for teejusb-fsr
 *
 * Hosts the FSR WebUI over WiFi and communicates with the Arduino
 * via hardware UART. This allows managing FSR thresholds wirelessly
 * even when the Arduino's USB is connected to a game controller
 * converter (e.g. USB Joypad-to-PS converter).
 *
 * Wiring:
 *   ESP32-C6 TX_PIN -> Arduino RX (pin 0)
 *   ESP32-C6 RX_PIN -> Arduino TX (pin 1)
 *   ESP32-C6 GND    -> Arduino GND
 *
 * Required Libraries (install via Arduino Library Manager):
 *   - ESPAsyncWebServer (by mathieucarbou or me-no-dev)
 *   - AsyncTCP (matching version for your ESP32 core)
 *   - ArduinoJson (v7+, by Benoit Blanchon)
 *
 * Required Board Package:
 *   - esp32 by Espressif (v3.x+ for ESP32-C6 support)
 *
 * Before uploading:
 *   1. Run prepare_data.sh to copy WebUI build files into data/
 *   2. Upload the data/ directory to LittleFS using the
 *      ESP32 LittleFS upload tool or arduino-cli.
 *   3. Compile and upload this sketch.
 */

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

#if __has_include("wifi_secrets.h")
  #include "wifi_secrets.h"
  #define HAS_WIFI_SECRETS 1
#else
  #define HAS_WIFI_SECRETS 0
  #define STA_CANDIDATE_COUNT 0
#endif

// =============================================================
// Configuration — adjust these to match your setup
// =============================================================

// WiFi Access Point settings.
// Connect your phone/PC to this network to access the WebUI.
const char* AP_SSID     = "FSR-Pad";
const char* AP_PASSWORD = "fsrpad123";

// STA candidate list is loaded from wifi_secrets.h.
// If no valid STA credentials are present, the device runs in AP mode.
const unsigned long STA_CONNECT_TIMEOUT_MS = 12000;
const unsigned long STA_RETRY_INTERVAL_MS = 5UL * 60UL * 1000UL;

// XIAO ESP32C6 onboard user LED.
#define STATUS_LED_PIN 15
#define STATUS_LED_ACTIVE_LOW 1
const unsigned long LED_PATTERN_PERIOD_MS = 1000;
const unsigned long LED_PULSE_ON_MS = 100;
const unsigned long LED_ERROR_TOGGLE_MS = 80;

// UART pins to the Arduino.
// ESP32-C6 RX_PIN receives data FROM Arduino TX (pin 1).
// ESP32-C6 TX_PIN sends data TO Arduino RX (pin 0).
// XIAO ESP32C6: D7 = GPIO17 (RX), D8 = GPIO19 (TX)
#define ARDUINO_RX_PIN  17   // D7
#define ARDUINO_TX_PIN  19   // D8
#define ARDUINO_BAUD     115200

// Number of sensors (must match kNumSensors in teejusb-fsr.ino).
#define NUM_SENSORS 8

// Slot ID used by the WebUI protocol.
#define SLOT_ID "1p"

// How often to poll the Arduino for sensor values (milliseconds).
// 16ms ≈ 60 Hz, matching the Python server's UI interval.
#define UI_INTERVAL_MS 16

// Profile storage filename on LittleFS.
#define PROFILES_FILE "/profiles_1p.txt"

// [public] profile stored separately — applied on every boot.
#define PUBLIC_PROFILE_FILE "/public_profile.txt"
#define PUBLIC_PROFILE_NAME  "[public]"

// Max number of user-created profiles (NOT counting [public]).
#define MAX_USER_PROFILES 10

// Hardcoded admin password required to overwrite [public] profile.
#define ADMIN_PASSWORD "admin123"

// Sensor number mapping (logical index -> physical Arduino sensor number).
// Change this if your sensor wiring order differs from 0-7.
static const int sensorNumbers[NUM_SENSORS] = {0, 1, 2, 3, 4, 5, 6, 7};

// =============================================================
// Global objects
// =============================================================

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// --- Profile storage ---

struct Profile {
  String name;
  int thresholds[NUM_SENSORS];
  uint32_t seq;  // monotonically increasing use counter; higher = more recently used
};

#define MAX_PROFILES 20
static Profile profiles[MAX_PROFILES];
static int profileCount = 0;
static uint32_t nextSeq = 1;  // next sequence number to assign
static String currentProfileName = "";
static int currentThresholds[NUM_SENSORS] = {0};
static int publicThresholds[NUM_SENSORS]  = {0};

// --- Stats ---

static unsigned long vCount = 0;
static unsigned long lastStatTime = 0;
static int lastReadRate = 0;
static int lastScanRate = 0;
static int lastJoystickRate = 0;

// --- Timing ---

static unsigned long lastValueRequestTime = 0;
static unsigned long lastStaAttemptTime = 0;
static unsigned long ledLastToggleTime = 0;
static bool ledErrorState = false;

enum class WifiModeState {
  STA,
  AP,
};

static WifiModeState wifiModeState = WifiModeState::AP;
static bool hasError = false;

// --- Serial line buffer ---

#define SERIAL_BUF_SIZE 256
static char serialBuf[SERIAL_BUF_SIZE];
static int serialBufPos = 0;

// =============================================================
// Status LED and error handling
// =============================================================

static void setLed(bool on) {
#if STATUS_LED_ACTIVE_LOW
  digitalWrite(STATUS_LED_PIN, on ? LOW : HIGH);
#else
  digitalWrite(STATUS_LED_PIN, on ? HIGH : LOW);
#endif
}

static void setError(const char* message) {
  hasError = true;
  Serial.printf("ERROR: %s\n", message);
}

static void updateStatusLed() {
  if (hasError) {
    unsigned long now = millis();
    if (now - ledLastToggleTime >= LED_ERROR_TOGGLE_MS) {
      ledLastToggleTime = now;
      ledErrorState = !ledErrorState;
      setLed(ledErrorState);
    }
    return;
  }

  // Every 1s:
  // - STA: 3 blinks (100ms on, 100ms off)
  // - AP : 2 blinks (100ms on, 100ms off)
  unsigned long phase = millis() % LED_PATTERN_PERIOD_MS;
  int blinkCount = (wifiModeState == WifiModeState::STA) ? 3 : 2;
  bool on = false;
  for (int i = 0; i < blinkCount; i++) {
    unsigned long start = i * 200;
    if (phase >= start && phase < start + LED_PULSE_ON_MS) {
      on = true;
      break;
    }
  }
  setLed(on);
}

// =============================================================
// WiFi mode management
// =============================================================

static bool tryConnectStaCandidates(bool fromApMode) {
#if !HAS_WIFI_SECRETS
  (void)fromApMode;
  Serial.println("wifi_secrets.h not found. STA is disabled.");
  return false;
#else
  if (STA_CANDIDATE_COUNT == 0) {
    Serial.println("No STA candidates configured.");
    return false;
  }

  if (fromApMode) {
    WiFi.mode(WIFI_AP_STA);
  } else {
    WiFi.mode(WIFI_STA);
  }

  for (size_t i = 0; i < STA_CANDIDATE_COUNT; i++) {
    const char* ssid = STA_CANDIDATE_SSIDS[i];
    const char* password = STA_CANDIDATE_PASSWORDS[i];
    if (!ssid || ssid[0] == '\0') {
      continue;
    }

    Serial.printf("Trying STA candidate %u: %s\n", (unsigned)(i + 1), ssid);
    WiFi.begin(ssid, password ? password : "");

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < STA_CONNECT_TIMEOUT_MS) {
      delay(200);
      updateStatusLed();
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("STA connected: %s  IP=%s\n", ssid, WiFi.localIP().toString().c_str());
      return true;
    }

    WiFi.disconnect(false, false);
    delay(100);
  }

  return false;
#endif
}

static void startApMode(const char* reason) {
  Serial.printf("Switching to AP mode (%s)\n", reason);
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(AP_SSID, AP_PASSWORD)) {
    setError("WiFi.softAP() failed");
  } else {
    Serial.printf("AP started: SSID=%s  IP=%s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  }
  wifiModeState = WifiModeState::AP;
  lastStaAttemptTime = millis();
}

static void switchToStaModeAfterConnect() {
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  wifiModeState = WifiModeState::STA;
  Serial.printf("Running in STA mode. IP: %s\n", WiFi.localIP().toString().c_str());
}

// =============================================================
// Profile management
// =============================================================

static int findProfile(const String& name) {
  for (int i = 0; i < profileCount; i++) {
    if (profiles[i].name == name) return i;
  }
  return -1;
}

static void loadProfiles() {
  profileCount = 0;
  currentProfileName = "";
  memset(currentThresholds, 0, sizeof(currentThresholds));

  if (!LittleFS.exists(PROFILES_FILE)) {
    File f = LittleFS.open(PROFILES_FILE, "w");
    if (f) f.close();
    return;
  }

  File f = LittleFS.open(PROFILES_FILE, "r");
  if (!f) return;

  while (f.available() && profileCount < MAX_PROFILES) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.isEmpty()) continue;

    // Format: "profileName seq t0 t1 t2 ... t7"
    int firstSpace = line.indexOf(' ');
    if (firstSpace < 0) continue;

    profiles[profileCount].name = line.substring(0, firstSpace);
    String rest = line.substring(firstSpace + 1);

    // Parse seq (first token after name).
    int seqEnd = rest.indexOf(' ');
    if (seqEnd < 0) continue;
    uint32_t seq = (uint32_t)rest.substring(0, seqEnd).toInt();
    profiles[profileCount].seq = seq;
    if (seq >= nextSeq) nextSeq = seq + 1;
    rest = rest.substring(seqEnd + 1);

    int si = 0, pos = 0;
    while (si < NUM_SENSORS && pos < (int)rest.length()) {
      int nextSpace = rest.indexOf(' ', pos);
      String val;
      if (nextSpace < 0) {
        val = rest.substring(pos);
        pos = rest.length();
      } else {
        val = rest.substring(pos, nextSpace);
        pos = nextSpace + 1;
      }
      profiles[profileCount].thresholds[si++] = val.toInt();
    }

    if (si == NUM_SENSORS) {
      if (profileCount == 0) {
        currentProfileName = profiles[0].name;
        memcpy(currentThresholds, profiles[0].thresholds, sizeof(currentThresholds));
      }
      profileCount++;
    }
  }
  f.close();
  Serial.printf("Loaded %d profile(s)\n", profileCount);
}

static void saveProfiles() {
  File f = LittleFS.open(PROFILES_FILE, "w");
  if (!f) return;
  for (int i = 0; i < profileCount; i++) {
    // Format: "profileName seq t0 t1 t2 ... t7"
    f.print(profiles[i].name);
    f.print(' ');
    f.print(profiles[i].seq);
    for (int j = 0; j < NUM_SENSORS; j++) {
      f.print(' ');
      f.print(profiles[i].thresholds[j]);
    }
    f.print('\n');
  }
  f.close();
}

// ----- [public] profile (boot default, admin-write-only) -----

static void loadPublicProfile() {
  memset(publicThresholds, 0, sizeof(publicThresholds));
  if (!LittleFS.exists(PUBLIC_PROFILE_FILE)) {
    Serial.println("[public] profile not found, using all-zero defaults");
    return;
  }
  File f = LittleFS.open(PUBLIC_PROFILE_FILE, "r");
  if (!f) return;
  String line = f.readStringUntil('\n');
  f.close();
  line.trim();
  if (line.isEmpty()) return;
  int idx = 0, pos = 0;
  while (idx < NUM_SENSORS && pos < (int)line.length()) {
    int nextSpace = line.indexOf(' ', pos);
    String val;
    if (nextSpace < 0) {
      val = line.substring(pos);
      pos  = line.length();
    } else {
      val = line.substring(pos, nextSpace);
      pos  = nextSpace + 1;
    }
    publicThresholds[idx++] = val.toInt();
  }
  Serial.println("[public] profile loaded");
}

static void savePublicProfile() {
  File f = LittleFS.open(PUBLIC_PROFILE_FILE, "w");
  if (!f) return;
  for (int i = 0; i < NUM_SENSORS; i++) {
    if (i > 0) f.print(' ');
    f.print(publicThresholds[i]);
  }
  f.print('\n');
  f.close();
}

// =============================================================
// WebSocket broadcast helpers
// =============================================================

static void wsBroadcast(const String& json) {
  ws.textAll(json);
}

static void broadcastThresholds() {
  JsonDocument doc;
  doc[0] = "thresholds";
  JsonObject msg = doc[1].to<JsonObject>();
  msg["slot"] = SLOT_ID;
  JsonArray arr = msg["thresholds"].to<JsonArray>();
  for (int i = 0; i < NUM_SENSORS; i++) arr.add(currentThresholds[i]);
  String out;
  serializeJson(doc, out);
  wsBroadcast(out);
}

static void broadcastProfiles() {
  JsonDocument doc;
  doc[0] = "get_profiles";
  JsonObject msg = doc[1].to<JsonObject>();
  msg["slot"] = SLOT_ID;
  JsonArray arr = msg["profiles"].to<JsonArray>();
  arr.add(PUBLIC_PROFILE_NAME);                         // always first
  for (int i = 0; i < profileCount; i++) arr.add(profiles[i].name);
  String out;
  serializeJson(doc, out);
  wsBroadcast(out);
}

static void broadcastCurrentProfile() {
  JsonDocument doc;
  doc[0] = "get_cur_profile";
  JsonObject msg = doc[1].to<JsonObject>();
  msg["slot"] = SLOT_ID;
  msg["cur_profile"] = currentProfileName;
  String out;
  serializeJson(doc, out);
  wsBroadcast(out);
}

static void broadcastValues(const int* values) {
  JsonDocument doc;
  doc[0] = "values";
  JsonObject msg = doc[1].to<JsonObject>();
  msg["slot"] = SLOT_ID;
  JsonArray arr = msg["values"].to<JsonArray>();
  for (int i = 0; i < NUM_SENSORS; i++) arr.add(values[i]);
  String out;
  serializeJson(doc, out);
  wsBroadcast(out);
}

static void broadcastStats() {
  JsonDocument doc;
  doc[0] = "stats";
  JsonObject msg = doc[1].to<JsonObject>();
  msg["slot"] = SLOT_ID;
  msg["read_rate"] = lastReadRate;
  if (lastScanRate > 0) {
    msg["scan_rate"] = lastScanRate;
    msg["joystick_rate"] = lastJoystickRate;
  }
  String out;
  serializeJson(doc, out);
  wsBroadcast(out);
}

static void broadcastThresholdsPersisted(const int* values) {
  JsonDocument doc;
  doc[0] = "thresholds_persisted";
  JsonObject msg = doc[1].to<JsonObject>();
  msg["slot"] = SLOT_ID;
  JsonArray arr = msg["thresholds"].to<JsonArray>();
  for (int i = 0; i < NUM_SENSORS; i++) arr.add(values[i]);
  String out;
  serializeJson(doc, out);
  wsBroadcast(out);
}

// Build the serial_port info JSON that the WebUI expects.
// The ESP32 is always connected via hardwired UART, so this is static.
static void addSerialPortInfo(JsonObject& slot) {
  slot["serial_port"] = "UART";
  JsonArray candidates = slot["serial_port_candidates"].to<JsonArray>();
  JsonObject c = candidates.add<JsonObject>();
  c["path"] = "UART";
  c["label"] = "UART (Hardware Serial)";
  c["in_use_by"] = "";
}

// =============================================================
// Profile operations (with broadcast)
// =============================================================

static void sendThresholdToArduino(int index, int value);

static void changeProfile(const String& name) {
  if (name == PUBLIC_PROFILE_NAME) {
    currentProfileName = PUBLIC_PROFILE_NAME;
    memcpy(currentThresholds, publicThresholds, sizeof(currentThresholds));
    broadcastThresholds();
    broadcastCurrentProfile();
    return;
  }
  int idx = findProfile(name);
  if (idx >= 0) {
    currentProfileName = profiles[idx].name;
    memcpy(currentThresholds, profiles[idx].thresholds, sizeof(currentThresholds));
    profiles[idx].seq = nextSeq++;  // mark as recently used
  } else {
    currentProfileName = "";
    memset(currentThresholds, 0, sizeof(currentThresholds));
  }
  broadcastThresholds();
  broadcastCurrentProfile();
}

static void addProfile(const String& name, const int* thresholds) {
  int existing = findProfile(name);
  if (existing >= 0) {
    // Update existing profile in place.
    memcpy(profiles[existing].thresholds, thresholds, sizeof(int) * NUM_SENSORS);
    profiles[existing].seq = nextSeq++;
  } else {
    if (profileCount >= MAX_USER_PROFILES) {
      // Evict the least recently used profile (lowest seq).
      int lruIdx = 0;
      for (int i = 1; i < profileCount; i++) {
        if (profiles[i].seq < profiles[lruIdx].seq) lruIdx = i;
      }
      Serial.printf("LRU evict: %s\n", profiles[lruIdx].name.c_str());
      for (int i = lruIdx; i < profileCount - 1; i++) profiles[i] = profiles[i + 1];
      profileCount--;
    }
    profiles[profileCount].name = name;
    memcpy(profiles[profileCount].thresholds, thresholds, sizeof(int) * NUM_SENSORS);
    profiles[profileCount].seq = nextSeq++;
    profileCount++;
  }
  currentProfileName = name;
  memcpy(currentThresholds, thresholds, sizeof(int) * NUM_SENSORS);
  saveProfiles();
  broadcastThresholds();
  broadcastCurrentProfile();
  broadcastProfiles();
}

static void removeProfile(const String& name) {
  int idx = findProfile(name);
  if (idx < 0) return;

  for (int i = idx; i < profileCount - 1; i++) {
    profiles[i] = profiles[i + 1];
  }
  profileCount--;

  if (currentProfileName == name) {
    changeProfile("");
  }
  saveProfiles();
  broadcastProfiles();
  broadcastThresholds();
  broadcastCurrentProfile();
}

// =============================================================
// Serial communication with Arduino
// =============================================================

static void sendToArduino(const char* cmd) {
  Serial1.print(cmd);
}

static void sendThresholdToArduino(int index, int value) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%d %d\n", sensorNumbers[index], value);
  sendToArduino(buf);
}

static void applyAllThresholds() {
  bool hasNonZero = false;
  for (int i = 0; i < NUM_SENSORS; i++) {
    if (currentThresholds[i] != 0) { hasNonZero = true; break; }
  }
  if (hasNonZero) {
    for (int i = 0; i < NUM_SENSORS; i++) {
      sendThresholdToArduino(i, currentThresholds[i]);
    }
  } else {
    // No profile thresholds — ask Arduino for its EEPROM values.
    sendToArduino("t\n");
  }
}

static void processSerialLine(const char* line) {
  if (line[0] == '\0') return;
  char cmd = line[0];

  // Helper lambda to parse space-separated integers after the command char.
  auto parseValues = [](const char* p, int* out, int count) -> bool {
    for (int i = 0; i < count; i++) {
      while (*p == ' ') p++;
      if (*p == '\0') return false;
      out[i] = atoi(p);
      while (*p && *p != ' ') p++;
    }
    return true;
  };

  if (cmd == 'v') {
    int raw[NUM_SENSORS];
    if (!parseValues(line + 1, raw, NUM_SENSORS)) return;
    // Reorder by sensorNumbers mapping.
    int actual[NUM_SENSORS];
    for (int i = 0; i < NUM_SENSORS; i++) actual[i] = raw[sensorNumbers[i]];
    broadcastValues(actual);

    vCount++;
    unsigned long now = millis();
    unsigned long elapsed = now - lastStatTime;
    if (elapsed >= 1000) {
      lastReadRate = (int)(vCount * 1000UL / elapsed);
      vCount = 0;
      lastStatTime = now;
      broadcastStats();
      sendToArduino("r\n");
    }
  }
  else if (cmd == 'r') {
    const char* p = line + 1;
    while (*p == ' ') p++;
    long loopTimeUs = atol(p);
    if (loopTimeUs > 0) {
      lastScanRate = (int)(1000000L / loopTimeUs);
      lastJoystickRate = lastScanRate < 1000 ? lastScanRate : 1000;
      broadcastStats();
    }
  }
  else if (cmd == 't') {
    int raw[NUM_SENSORS];
    if (!parseValues(line + 1, raw, NUM_SENSORS)) return;
    int actual[NUM_SENSORS];
    for (int i = 0; i < NUM_SENSORS; i++) actual[i] = raw[sensorNumbers[i]];
    for (int i = 0; i < NUM_SENSORS; i++) currentThresholds[i] = actual[i];
    broadcastThresholds();
  }
  else if (cmd == 's' || cmd == 'p') {
    int raw[NUM_SENSORS];
    if (!parseValues(line + 1, raw, NUM_SENSORS)) return;
    broadcastThresholdsPersisted(raw);
  }
}

static void readSerial() {
  while (Serial1.available()) {
    char c = Serial1.read();
    if (c == '\n') {
      serialBuf[serialBufPos] = '\0';
      if (serialBufPos > 0) {
        processSerialLine(serialBuf);
      }
      serialBufPos = 0;
    } else if (c != '\r' && serialBufPos < SERIAL_BUF_SIZE - 1) {
      serialBuf[serialBufPos++] = c;
    }
  }
}

// =============================================================
// WebSocket event handler
// =============================================================

static void sendSerialPortState(AsyncWebSocketClient* client) {
  JsonDocument doc;
  doc[0] = "serial_port";
  JsonObject msg = doc[1].to<JsonObject>();
  msg["slot"] = SLOT_ID;
  addSerialPortInfo(msg);
  String out;
  serializeJson(doc, out);
  client->text(out);
}

static void onWsEvent(AsyncWebSocket* /*server*/, AsyncWebSocketClient* client,
                       AwsEventType type, void* arg, uint8_t* data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.printf("WS client #%u connected\n", client->id());

    // Send current thresholds to the newly connected client.
    {
      JsonDocument doc;
      doc[0] = "thresholds";
      JsonObject msg = doc[1].to<JsonObject>();
      msg["slot"] = SLOT_ID;
      JsonArray arr = msg["thresholds"].to<JsonArray>();
      for (int i = 0; i < NUM_SENSORS; i++) arr.add(currentThresholds[i]);
      String out;
      serializeJson(doc, out);
      client->text(out);
    }
    // Also ask Arduino for its current thresholds.
    sendToArduino("t\n");
  }
  else if (type == WS_EVT_DISCONNECT) {
    Serial.printf("WS client #%u disconnected\n", client->id());
  }
  else if (type == WS_EVT_DATA) {
    AwsFrameInfo* info = (AwsFrameInfo*)arg;
    if (!(info->final && info->index == 0 && info->len == len
          && info->opcode == WS_TEXT)) {
      return;
    }

    // Null-terminate for JSON parsing.
    data[len] = '\0';

    JsonDocument doc;
    if (deserializeJson(doc, (char*)data)) return;

    String action = doc[0].as<String>();

    if (action == "update_threshold") {
      // ["update_threshold", slot, [thresholds...], index]
      JsonArray values = doc[2].as<JsonArray>();
      int index = doc[3].as<int>();
      if (index >= 0 && index < NUM_SENSORS) {
        int value = values[index].as<int>();
        currentThresholds[index] = value;

        int pIdx = findProfile(currentProfileName);
        if (pIdx >= 0) {
          profiles[pIdx].thresholds[index] = value;
          saveProfiles();
        }
        sendThresholdToArduino(index, value);
      }
    }
    else if (action == "save_thresholds") {
      sendToArduino("s\n");
    }
    else if (action == "add_profile") {
      // ["add_profile", slot, name, [thresholds...]]
      String name = doc[2].as<String>();
      if (name == PUBLIC_PROFILE_NAME) return;         // [public] is managed by admin only
      // No hard limit check here: addProfile() evicts LRU automatically.
      JsonArray threshArr = doc[3].as<JsonArray>();
      int th[NUM_SENSORS] = {0};
      for (int i = 0; i < NUM_SENSORS && i < (int)threshArr.size(); i++) {
        th[i] = threshArr[i].as<int>();
      }
      addProfile(name, th);
    }
    else if (action == "remove_profile") {
      String name = doc[2].as<String>();
      if (name == PUBLIC_PROFILE_NAME) return;         // [public] cannot be deleted
      removeProfile(name);
    }
    else if (action == "update_public_profile") {
      // ["update_public_profile", slot, admin_password, [thresholds...]]
      String password = doc[2].as<String>();
      if (password != ADMIN_PASSWORD) {
        JsonDocument errDoc;
        errDoc[0] = "admin_auth_error";
        JsonObject errMsg = errDoc[1].to<JsonObject>();
        errMsg["slot"] = SLOT_ID;
        errMsg["message"] = "パスワードが違います";
        String eout;
        serializeJson(errDoc, eout);
        client->text(eout);
        return;
      }
      JsonArray threshArr = doc[3].as<JsonArray>();
      for (int i = 0; i < NUM_SENSORS && i < (int)threshArr.size(); i++) {
        publicThresholds[i] = threshArr[i].as<int>();
      }
      savePublicProfile();
      // If [public] is currently active, update live thresholds too.
      if (currentProfileName == PUBLIC_PROFILE_NAME) {
        memcpy(currentThresholds, publicThresholds, sizeof(currentThresholds));
        broadcastThresholds();
      }
      // Notify client of success.
      JsonDocument okDoc;
      okDoc[0] = "public_profile_updated";
      okDoc[1]["slot"] = SLOT_ID;
      String okout;
      serializeJson(okDoc, okout);
      client->text(okout);
      broadcastProfiles();
    }
    else if (action == "change_profile") {
      String name = doc[2].as<String>();
      changeProfile(name);
      // Apply the new thresholds to the Arduino.
      for (int i = 0; i < NUM_SENSORS; i++) {
        sendThresholdToArduino(i, currentThresholds[i]);
      }
    }
    else if (action == "set_serial_port") {
      // No-op — always connected via UART. Reply with current state.
      sendSerialPortState(client);
    }
    else if (action == "refresh_serial_port_candidates") {
      JsonDocument resp;
      resp[0] = "serial_port_candidates";
      JsonObject msg = resp[1].to<JsonObject>();
      msg["slot"] = SLOT_ID;
      JsonArray candidates = msg["serial_port_candidates"].to<JsonArray>();
      JsonObject c = candidates.add<JsonObject>();
      c["path"] = "UART";
      c["label"] = "UART (Hardware Serial)";
      c["in_use_by"] = "";
      String out;
      serializeJson(resp, out);
      client->text(out);
    }
  }
}

// =============================================================
// HTTP handler: /defaults
// =============================================================

static void handleDefaults(AsyncWebServerRequest* request) {
  JsonDocument doc;
  JsonObject slots = doc["slots"].to<JsonObject>();
  JsonObject slot = slots[SLOT_ID].to<JsonObject>();

  JsonArray profileArr = slot["profiles"].to<JsonArray>();
  profileArr.add(PUBLIC_PROFILE_NAME);  // always present
  for (int i = 0; i < profileCount; i++) profileArr.add(profiles[i].name);

  slot["cur_profile"] = currentProfileName;

  JsonArray threshArr = slot["thresholds"].to<JsonArray>();
  for (int i = 0; i < NUM_SENSORS; i++) threshArr.add(currentThresholds[i]);

  addSerialPortInfo(slot);

  String out;
  serializeJson(doc, out);
  request->send(200, "application/json", out);
}

// =============================================================
// setup() & loop()
// =============================================================

void setup() {
  // Debug console (USB serial on ESP32-C6).
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== ESP32-C6 FSR WiFi Bridge ===");

  pinMode(STATUS_LED_PIN, OUTPUT);
  setLed(false);

  // UART to Arduino.
  Serial1.begin(ARDUINO_BAUD, SERIAL_8N1, ARDUINO_RX_PIN, ARDUINO_TX_PIN);

  // Mount LittleFS (format on first use).
  if (!LittleFS.begin(true)) {
    setError("LittleFS mount failed");
  }
  Serial.println("LittleFS mounted");

  // Load saved profiles.
  loadProfiles();

  // Load [public] profile and apply it on boot (always).
  loadPublicProfile();
  currentProfileName = PUBLIC_PROFILE_NAME;
  memcpy(currentThresholds, publicThresholds, sizeof(currentThresholds));

  // --- WiFi ---
  // Prefer STA. If no candidate succeeds, fallback to AP mode.
  if (tryConnectStaCandidates(false)) {
    wifiModeState = WifiModeState::STA;
    Serial.printf("Running in STA mode. IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    startApMode("initial STA connection failed");
  }

  // --- Web server ---
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  server.on("/defaults", HTTP_GET, handleDefaults);

  // Serve WebUI static files from LittleFS.
  // ESPAsyncWebServer automatically serves .gz variants with
  // Content-Encoding: gzip when the client supports it.
  server.serveStatic("/", LittleFS, "/www/").setDefaultFile("index.html");

  // SPA fallback: /plot is handled by the React Router inside index.html.
  server.on("/plot", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(LittleFS, "/www/index.html.gz", "text/html");
  });

  server.begin();
  Serial.println("HTTP server started on port 80");

  // Give the Arduino a moment to finish its own setup, then push
  // saved profile thresholds (or fetch from EEPROM if none saved).
  delay(500);
  applyAllThresholds();

  lastStatTime = millis();
}

void loop() {
  updateStatusLed();

  if (wifiModeState == WifiModeState::STA) {
    if (WiFi.status() != WL_CONNECTED) {
      startApMode("STA disconnected");
    }
  } else {
    unsigned long now = millis();
    if (now - lastStaAttemptTime >= STA_RETRY_INTERVAL_MS) {
      lastStaAttemptTime = now;
      Serial.println("AP mode: retrying STA candidates...");
      if (tryConnectStaCandidates(true)) {
        switchToStaModeAfterConnect();
      } else {
        // Ensure AP is up even if retry path touched WiFi state.
        startApMode("periodic STA retry failed");
      }
    }
  }

  // Disconnect idle WebSocket clients.
  ws.cleanupClients();

  // Read and process serial data from the Arduino.
  readSerial();

  // Poll the Arduino for sensor values when at least one WebUI client
  // is connected.
  if (ws.count() > 0) {
    unsigned long now = millis();
    if (now - lastValueRequestTime >= UI_INTERVAL_MS) {
      sendToArduino("v\n");
      lastValueRequestTime = now;
    }
  }
}
