#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <vector>

extern "C" {
#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
}

#if __has_include("wifi_secrets.h")
  #include "wifi_secrets.h"
  #define HAS_WIFI_SECRETS 1
#else
  #define HAS_WIFI_SECRETS 0
  #define STA_CANDIDATE_COUNT 0
#endif

namespace {

constexpr char kTag[] = "fsr-bridge";
constexpr char kApSsid[] = "FSR-Pad";
constexpr char kApPassword[] = "fsrpad123";
constexpr int kStatusLedPin = 15;
constexpr bool kStatusLedActiveLow = true;
constexpr uint32_t kLedPatternPeriodMs = 1000;
constexpr uint32_t kLedPulseOnMs = 100;
constexpr uint32_t kLedErrorToggleMs = 80;
constexpr uart_port_t kArduinoUart = UART_NUM_1;
constexpr int kArduinoRxPin = 22;
constexpr int kArduinoTxPin = 23;
constexpr int kArduinoBaud = 115200;
constexpr int kNumSensors = 8;
constexpr char kSlotId[] = "1p";
constexpr uint32_t kUiIntervalMs = 16;
constexpr char kProfilesFile[] = "/spiffs/profiles_1p.txt";
constexpr char kPublicProfileFile[] = "/spiffs/public_profile.txt";
constexpr char kPublicProfileName[] = "[public]";
constexpr int kMaxUserProfiles = 10;
constexpr char kAdminPassword[] = "admin123";
constexpr uint32_t kStaConnectTimeoutMs = 12000;
constexpr uint32_t kStaRetryIntervalMs = 5UL * 60UL * 1000UL;
constexpr size_t kSerialBufSize = 256;
constexpr size_t kMaxWsClients = 8;
constexpr size_t kFileChunkSize = 1024;
constexpr size_t kHttpServerStackSize = 8192;
constexpr char kMountPath[] = "/spiffs";

enum class WifiModeState {
  kSta,
  kAp,
};

struct Profile {
  std::string name;
  std::array<int, kNumSensors> thresholds{};
  uint32_t seq = 0;
};

std::mutex gStateMutex;
std::vector<Profile> gProfiles;
std::string gCurrentProfileName;
std::array<int, kNumSensors> gCurrentThresholds{};
std::array<int, kNumSensors> gPublicThresholds{};
uint32_t gNextSeq = 1;
uint64_t gVCount = 0;
uint64_t gLastStatTimeMs = 0;
int gLastReadRate = 0;
int gLastScanRate = 0;
int gLastJoystickRate = 0;
uint64_t gLastValueRequestTimeMs = 0;
uint64_t gLastStaAttemptTimeMs = 0;
uint64_t gLedLastToggleTimeMs = 0;
bool gLedErrorState = false;
bool gHasError = false;
WifiModeState gWifiModeState = WifiModeState::kAp;
std::atomic<bool> gStaConnected{false};
std::string gStaIpAddress;
httpd_handle_t gHttpServer = nullptr;
std::array<int, kNumSensors> gSensorNumbers{0, 1, 2, 3, 4, 5, 6, 7};
char gSerialBuf[kSerialBufSize] = {};
size_t gSerialBufPos = 0;

uint64_t millis64() {
  return static_cast<uint64_t>(esp_timer_get_time() / 1000ULL);
}

void set_led(bool on) {
  const int level = kStatusLedActiveLow ? (on ? 0 : 1) : (on ? 1 : 0);
  gpio_set_level(static_cast<gpio_num_t>(kStatusLedPin), level);
}

void set_error(const char* message) {
  gHasError = true;
  ESP_LOGE(kTag, "%s", message);
}

void update_status_led() {
  const uint64_t now = millis64();
  if (gHasError) {
    if (now - gLedLastToggleTimeMs >= kLedErrorToggleMs) {
      gLedLastToggleTimeMs = now;
      gLedErrorState = !gLedErrorState;
      set_led(gLedErrorState);
    }
    return;
  }

  const uint64_t phase = now % kLedPatternPeriodMs;
  const int blinkCount = gWifiModeState == WifiModeState::kSta ? 3 : 2;
  bool on = false;
  for (int i = 0; i < blinkCount; ++i) {
    const uint64_t start = static_cast<uint64_t>(i) * 200ULL;
    if (phase >= start && phase < start + kLedPulseOnMs) {
      on = true;
      break;
    }
  }
  set_led(on);
}

std::string trim_copy(const std::string& input) {
  const auto begin = input.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return "";
  }
  const auto end = input.find_last_not_of(" \t\r\n");
  return input.substr(begin, end - begin + 1);
}

bool parse_space_separated_values(const std::string& text, std::array<int, kNumSensors>& values) {
  std::istringstream stream(text);
  for (int i = 0; i < kNumSensors; ++i) {
    if (!(stream >> values[i])) {
      return false;
    }
  }
  return true;
}

std::string json_escape(std::string_view input) {
  std::string out;
  out.reserve(input.size() + 8);
  for (char ch : input) {
    switch (ch) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        out += ch;
        break;
    }
  }
  return out;
}

std::string json_array_from_ints(const std::array<int, kNumSensors>& values) {
  std::string out = "[";
  for (int i = 0; i < kNumSensors; ++i) {
    if (i != 0) {
      out += ",";
    }
    out += std::to_string(values[i]);
  }
  out += "]";
  return out;
}

std::string build_thresholds_message(const std::array<int, kNumSensors>& values) {
  return std::string{"[\"thresholds\",{\"slot\":\""} + kSlotId + "\",\"thresholds\":" +
         json_array_from_ints(values) + "}]";
}

std::string build_thresholds_persisted_message(const std::array<int, kNumSensors>& values) {
  return std::string{"[\"thresholds_persisted\",{\"slot\":\""} + kSlotId +
         "\",\"thresholds\":" + json_array_from_ints(values) + "}]";
}

std::string build_values_message(const std::array<int, kNumSensors>& values) {
  return std::string{"[\"values\",{\"slot\":\""} + kSlotId + "\",\"values\":" +
         json_array_from_ints(values) + "}]";
}

std::string build_current_profile_message(std::string_view profile_name) {
  return std::string{"[\"get_cur_profile\",{\"slot\":\""} + kSlotId +
         "\",\"cur_profile\":\"" + json_escape(profile_name) + "\"}]";
}

std::string build_profiles_message() {
  std::lock_guard<std::mutex> lock(gStateMutex);
  std::string out = std::string{"[\"get_profiles\",{\"slot\":\""} + kSlotId + "\",\"profiles\":[\"" +
                    json_escape(kPublicProfileName) + "\"";
  for (const auto& profile : gProfiles) {
    out += ",\"" + json_escape(profile.name) + "\"";
  }
  out += "]}]";
  return out;
}

std::string build_serial_port_message() {
  return std::string{"[\"serial_port\",{\"slot\":\""} + kSlotId +
         "\",\"serial_port\":\"UART\",\"serial_port_candidates\":[{\"path\":\"UART\",\"label\":\"UART (Hardware Serial)\",\"in_use_by\":\"\"}]}]";
}

std::string build_serial_port_candidates_message() {
  return std::string{"[\"serial_port_candidates\",{\"slot\":\""} + kSlotId +
         "\",\"serial_port_candidates\":[{\"path\":\"UART\",\"label\":\"UART (Hardware Serial)\",\"in_use_by\":\"\"}]}]";
}

std::string build_stats_message() {
  std::lock_guard<std::mutex> lock(gStateMutex);
  std::string out = std::string{"[\"stats\",{\"slot\":\""} + kSlotId +
                    "\",\"read_rate\":" + std::to_string(gLastReadRate);
  if (gLastScanRate > 0) {
    out += ",\"scan_rate\":" + std::to_string(gLastScanRate);
    out += ",\"joystick_rate\":" + std::to_string(gLastJoystickRate);
  }
  out += "}]";
  return out;
}

std::string build_admin_auth_error_message() {
  return std::string{"[\"admin_auth_error\",{\"slot\":\""} + kSlotId +
         "\",\"message\":\"パスワードが違います\"}]";
}

std::string build_public_profile_updated_message() {
  return std::string{"[\"public_profile_updated\",{\"slot\":\""} + kSlotId + "\"}]";
}

std::string build_defaults_json() {
  std::lock_guard<std::mutex> lock(gStateMutex);
  std::string out = std::string{"{\"slots\":{\""} + kSlotId + "\":{";
  out += "\"profiles\":[\"" + json_escape(kPublicProfileName) + "\"";
  for (const auto& profile : gProfiles) {
    out += ",\"" + json_escape(profile.name) + "\"";
  }
  out += "],";
  out += "\"cur_profile\":\"" + json_escape(gCurrentProfileName) + "\",";
  out += "\"thresholds\":" + json_array_from_ints(gCurrentThresholds) + ",";
  out += "\"serial_port\":\"UART\",";
  out += "\"serial_port_candidates\":[{\"path\":\"UART\",\"label\":\"UART (Hardware Serial)\",\"in_use_by\":\"\"}]";
  out += "}}}";
  return out;
}

bool file_exists(const std::string& path) {
  struct stat st {};
  return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool ends_with(const std::string& value, const char* suffix) {
  const size_t suffix_len = strlen(suffix);
  return value.size() >= suffix_len &&
         value.compare(value.size() - suffix_len, suffix_len, suffix) == 0;
}

int find_profile_index(std::string_view name) {
  for (size_t i = 0; i < gProfiles.size(); ++i) {
    if (gProfiles[i].name == name) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void save_profiles_locked() {
  FILE* file = fopen(kProfilesFile, "w");
  if (!file) {
    ESP_LOGE(kTag, "Failed to open profiles file for writing: errno=%d", errno);
    return;
  }
  for (const auto& profile : gProfiles) {
    fprintf(file, "%s %" PRIu32, profile.name.c_str(), profile.seq);
    for (int value : profile.thresholds) {
      fprintf(file, " %d", value);
    }
    fputc('\n', file);
  }
  fclose(file);
}

void load_profiles() {
  std::lock_guard<std::mutex> lock(gStateMutex);
  gProfiles.clear();
  gCurrentProfileName.clear();
  gCurrentThresholds.fill(0);
  gNextSeq = 1;

  FILE* file = fopen(kProfilesFile, "r");
  if (!file) {
    file = fopen(kProfilesFile, "w");
    if (file) {
      fclose(file);
    }
    return;
  }

  char line[256];
  while (fgets(line, sizeof(line), file)) {
    std::string text = trim_copy(line);
    if (text.empty()) {
      continue;
    }
    std::istringstream stream(text);
    Profile profile;
    if (!(stream >> profile.name >> profile.seq)) {
      continue;
    }
    bool ok = true;
    for (int& value : profile.thresholds) {
      if (!(stream >> value)) {
        ok = false;
        break;
      }
    }
    if (!ok) {
      continue;
    }
    gNextSeq = std::max(gNextSeq, profile.seq + 1);
    gProfiles.push_back(profile);
  }
  fclose(file);
}

void load_public_profile() {
  std::lock_guard<std::mutex> lock(gStateMutex);
  gPublicThresholds.fill(0);
  FILE* file = fopen(kPublicProfileFile, "r");
  if (!file) {
    return;
  }
  char line[256];
  if (fgets(line, sizeof(line), file)) {
    std::array<int, kNumSensors> values{};
    if (parse_space_separated_values(line, values)) {
      gPublicThresholds = values;
    }
  }
  fclose(file);
}

void save_public_profile_locked() {
  FILE* file = fopen(kPublicProfileFile, "w");
  if (!file) {
    ESP_LOGE(kTag, "Failed to open public profile file for writing: errno=%d", errno);
    return;
  }
  for (int i = 0; i < kNumSensors; ++i) {
    if (i != 0) {
      fputc(' ', file);
    }
    fprintf(file, "%d", gPublicThresholds[i]);
  }
  fputc('\n', file);
  fclose(file);
}

void uart_send(std::string_view text) {
  uart_write_bytes(kArduinoUart, text.data(), text.size());
}

void send_threshold_to_arduino(int index, int value) {
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%d %d\n", gSensorNumbers[index], value);
  uart_send(buffer);
}

void apply_all_thresholds() {
  std::array<int, kNumSensors> thresholds{};
  {
    std::lock_guard<std::mutex> lock(gStateMutex);
    thresholds = gCurrentThresholds;
  }

  bool has_non_zero = false;
  for (int value : thresholds) {
    if (value != 0) {
      has_non_zero = true;
      break;
    }
  }

  if (!has_non_zero) {
    uart_send("t\n");
    return;
  }

  for (int i = 0; i < kNumSensors; ++i) {
    send_threshold_to_arduino(i, thresholds[i]);
  }
}

void ws_broadcast_work(void* arg) {
  std::unique_ptr<std::string> payload(static_cast<std::string*>(arg));
  if (!gHttpServer) {
    return;
  }

  int client_fds[kMaxWsClients] = {};
  size_t fd_count = kMaxWsClients;
  if (httpd_get_client_list(gHttpServer, &fd_count, client_fds) != ESP_OK) {
    return;
  }

  httpd_ws_frame_t frame = {};
  frame.type = HTTPD_WS_TYPE_TEXT;
  frame.payload = reinterpret_cast<uint8_t*>(payload->data());
  frame.len = payload->size();

  for (size_t i = 0; i < fd_count; ++i) {
    if (httpd_ws_get_fd_info(gHttpServer, client_fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
      httpd_ws_send_frame_async(gHttpServer, client_fds[i], &frame);
    }
  }
}

void queue_ws_broadcast(const std::string& payload) {
  if (!gHttpServer) {
    return;
  }
  auto* copy = new std::string(payload);
  if (httpd_queue_work(gHttpServer, ws_broadcast_work, copy) != ESP_OK) {
    delete copy;
  }
}

void send_ws_text(httpd_req_t* req, const std::string& payload) {
  httpd_ws_frame_t frame = {};
  frame.type = HTTPD_WS_TYPE_TEXT;
  frame.payload = reinterpret_cast<uint8_t*>(const_cast<char*>(payload.data()));
  frame.len = payload.size();
  httpd_ws_send_frame(req, &frame);
}

size_t websocket_client_count() {
  if (!gHttpServer) {
    return 0;
  }
  int client_fds[kMaxWsClients] = {};
  size_t fd_count = kMaxWsClients;
  if (httpd_get_client_list(gHttpServer, &fd_count, client_fds) != ESP_OK) {
    return 0;
  }
  size_t count = 0;
  for (size_t i = 0; i < fd_count; ++i) {
    if (httpd_ws_get_fd_info(gHttpServer, client_fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
      ++count;
    }
  }
  return count;
}

void change_profile(const std::string& name) {
  std::array<int, kNumSensors> thresholds{};
  std::string current_profile;
  {
    std::lock_guard<std::mutex> lock(gStateMutex);
    if (name == kPublicProfileName) {
      gCurrentProfileName = kPublicProfileName;
      gCurrentThresholds = gPublicThresholds;
    } else {
      const int index = find_profile_index(name);
      if (index >= 0) {
        gCurrentProfileName = gProfiles[index].name;
        gCurrentThresholds = gProfiles[index].thresholds;
        gProfiles[index].seq = gNextSeq++;
      } else {
        gCurrentProfileName.clear();
        gCurrentThresholds.fill(0);
      }
    }
    thresholds = gCurrentThresholds;
    current_profile = gCurrentProfileName;
  }
  queue_ws_broadcast(build_thresholds_message(thresholds));
  queue_ws_broadcast(build_current_profile_message(current_profile));
}

void add_profile(const std::string& name, const std::array<int, kNumSensors>& thresholds) {
  {
    std::lock_guard<std::mutex> lock(gStateMutex);
    const int existing = find_profile_index(name);
    if (existing >= 0) {
      gProfiles[existing].thresholds = thresholds;
      gProfiles[existing].seq = gNextSeq++;
    } else {
      if (static_cast<int>(gProfiles.size()) >= kMaxUserProfiles) {
        auto lru = std::min_element(
          gProfiles.begin(),
          gProfiles.end(),
          [](const Profile& lhs, const Profile& rhs) { return lhs.seq < rhs.seq; }
        );
        if (lru != gProfiles.end()) {
          gProfiles.erase(lru);
        }
      }
      Profile profile;
      profile.name = name;
      profile.thresholds = thresholds;
      profile.seq = gNextSeq++;
      gProfiles.push_back(profile);
    }
    gCurrentProfileName = name;
    gCurrentThresholds = thresholds;
    save_profiles_locked();
  }
  queue_ws_broadcast(build_thresholds_message(thresholds));
  queue_ws_broadcast(build_current_profile_message(name));
  queue_ws_broadcast(build_profiles_message());
}

void remove_profile(const std::string& name) {
  bool removed = false;
  bool should_reset = false;
  {
    std::lock_guard<std::mutex> lock(gStateMutex);
    const int index = find_profile_index(name);
    if (index < 0) {
      return;
    }
    gProfiles.erase(gProfiles.begin() + index);
    removed = true;
    should_reset = gCurrentProfileName == name;
    if (should_reset) {
      gCurrentProfileName.clear();
      gCurrentThresholds.fill(0);
    }
    save_profiles_locked();
  }
  if (!removed) {
    return;
  }
  queue_ws_broadcast(build_profiles_message());
  if (should_reset) {
    std::array<int, kNumSensors> zeroes{};
    queue_ws_broadcast(build_thresholds_message(zeroes));
    queue_ws_broadcast(build_current_profile_message(""));
  }
}

void process_serial_line(const std::string& line) {
  if (line.empty()) {
    return;
  }

  const char command = line.front();
  if (command == 'v') {
    std::array<int, kNumSensors> raw{};
    if (!parse_space_separated_values(line.substr(1), raw)) {
      return;
    }
    std::array<int, kNumSensors> actual{};
    for (int i = 0; i < kNumSensors; ++i) {
      actual[i] = raw[gSensorNumbers[i]];
    }
    queue_ws_broadcast(build_values_message(actual));

    const uint64_t now = millis64();
    bool should_request_loop = false;
    {
      std::lock_guard<std::mutex> lock(gStateMutex);
      ++gVCount;
      const uint64_t elapsed = now - gLastStatTimeMs;
      if (elapsed >= 1000) {
        gLastReadRate = static_cast<int>(gVCount * 1000ULL / elapsed);
        gVCount = 0;
        gLastStatTimeMs = now;
        should_request_loop = true;
      }
    }
    if (should_request_loop) {
      queue_ws_broadcast(build_stats_message());
      uart_send("r\n");
    }
    return;
  }

  if (command == 'r') {
    long loop_time_us = std::strtol(line.c_str() + 1, nullptr, 10);
    if (loop_time_us > 0) {
      {
        std::lock_guard<std::mutex> lock(gStateMutex);
        gLastScanRate = static_cast<int>(1000000L / loop_time_us);
        gLastJoystickRate = std::min(gLastScanRate, 1000);
      }
      queue_ws_broadcast(build_stats_message());
    }
    return;
  }

  if (command == 't') {
    std::array<int, kNumSensors> raw{};
    if (!parse_space_separated_values(line.substr(1), raw)) {
      return;
    }
    std::array<int, kNumSensors> actual{};
    for (int i = 0; i < kNumSensors; ++i) {
      actual[i] = raw[gSensorNumbers[i]];
    }
    {
      std::lock_guard<std::mutex> lock(gStateMutex);
      gCurrentThresholds = actual;
    }
    queue_ws_broadcast(build_thresholds_message(actual));
    return;
  }

  if (command == 's' || command == 'p') {
    std::array<int, kNumSensors> values{};
    if (!parse_space_separated_values(line.substr(1), values)) {
      return;
    }
    queue_ws_broadcast(build_thresholds_persisted_message(values));
  }
}

void uart_task(void*) {
  while (true) {
    uint8_t byte = 0;
    const int read = uart_read_bytes(kArduinoUart, &byte, 1, pdMS_TO_TICKS(20));
    if (read <= 0) {
      continue;
    }

    if (byte == '\n') {
      gSerialBuf[gSerialBufPos] = '\0';
      if (gSerialBufPos > 0) {
        process_serial_line(gSerialBuf);
      }
      gSerialBufPos = 0;
      continue;
    }

    if (byte != '\r' && gSerialBufPos + 1 < kSerialBufSize) {
      gSerialBuf[gSerialBufPos++] = static_cast<char>(byte);
    }
  }
}

const char* content_type_for_path(const std::string& path) {
  if (ends_with(path, ".html") || ends_with(path, ".html.gz")) return "text/html";
  if (ends_with(path, ".js") || ends_with(path, ".js.gz")) return "application/javascript";
  if (ends_with(path, ".css") || ends_with(path, ".css.gz")) return "text/css";
  if (ends_with(path, ".json") || ends_with(path, ".json.gz")) return "application/json";
  if (ends_with(path, ".txt")) return "text/plain";
  if (ends_with(path, ".svg")) return "image/svg+xml";
  if (ends_with(path, ".png")) return "image/png";
  if (ends_with(path, ".jpg") || ends_with(path, ".jpeg")) return "image/jpeg";
  if (ends_with(path, ".ico")) return "image/x-icon";
  return "application/octet-stream";
}

esp_err_t send_file_response(httpd_req_t* req, const std::string& path, bool gzip) {
  FILE* file = fopen(path.c_str(), "rb");
  if (!file) {
    return ESP_FAIL;
  }

  if (gzip) {
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  }
  httpd_resp_set_type(req, content_type_for_path(path));

  std::vector<char> buffer(kFileChunkSize);
  while (true) {
    const size_t read = fread(buffer.data(), 1, buffer.size(), file);
    if (read > 0) {
      if (httpd_resp_send_chunk(req, buffer.data(), read) != ESP_OK) {
        fclose(file);
        httpd_resp_sendstr_chunk(req, nullptr);
        return ESP_FAIL;
      }
    }
    if (read < buffer.size()) {
      break;
    }
  }

  fclose(file);
  httpd_resp_sendstr_chunk(req, nullptr);
  return ESP_OK;
}

esp_err_t defaults_handler(httpd_req_t* req) {
  const std::string body = build_defaults_json();
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, body.c_str(), body.size());
}

esp_err_t static_handler(httpd_req_t* req) {
  std::string uri = req->uri;
  if (uri == "/defaults") {
    return defaults_handler(req);
  }

  std::string path;
  if (uri == "/" || uri == "/plot") {
    path = std::string{kMountPath} + "/www/index.html.gz";
    return send_file_response(req, path, true);
  }

  path = std::string{kMountPath} + "/www" + uri;
  std::string gzip_path = path + ".gz";
  if (file_exists(gzip_path)) {
    return send_file_response(req, gzip_path, true);
  }
  if (file_exists(path)) {
    return send_file_response(req, path, false);
  }

  const std::string spa_path = std::string{kMountPath} + "/www/index.html.gz";
  return send_file_response(req, spa_path, true);
}

void handle_update_threshold(cJSON* root) {
  cJSON* thresholds_item = cJSON_GetArrayItem(root, 2);
  cJSON* index_item = cJSON_GetArrayItem(root, 3);
  if (!cJSON_IsArray(thresholds_item) || !cJSON_IsNumber(index_item)) {
    return;
  }
  const int index = index_item->valueint;
  if (index < 0 || index >= kNumSensors) {
    return;
  }
  cJSON* value_item = cJSON_GetArrayItem(thresholds_item, index);
  if (!cJSON_IsNumber(value_item)) {
    return;
  }
  const int value = value_item->valueint;
  {
    std::lock_guard<std::mutex> lock(gStateMutex);
    gCurrentThresholds[index] = value;
    const int profile_index = find_profile_index(gCurrentProfileName);
    if (profile_index >= 0) {
      gProfiles[profile_index].thresholds[index] = value;
      save_profiles_locked();
    }
  }
  send_threshold_to_arduino(index, value);
}

bool parse_threshold_array(cJSON* thresholds_item, std::array<int, kNumSensors>& thresholds) {
  if (!cJSON_IsArray(thresholds_item)) {
    return false;
  }
  thresholds.fill(0);
  const int size = cJSON_GetArraySize(thresholds_item);
  for (int i = 0; i < kNumSensors && i < size; ++i) {
    cJSON* value_item = cJSON_GetArrayItem(thresholds_item, i);
    if (cJSON_IsNumber(value_item)) {
      thresholds[i] = value_item->valueint;
    }
  }
  return true;
}

esp_err_t ws_handler(httpd_req_t* req) {
  if (req->method == HTTP_GET) {
    std::array<int, kNumSensors> thresholds{};
    std::string current_profile;
    {
      std::lock_guard<std::mutex> lock(gStateMutex);
      thresholds = gCurrentThresholds;
      current_profile = gCurrentProfileName;
    }
    send_ws_text(req, build_thresholds_message(thresholds));
    send_ws_text(req, build_serial_port_message());
    send_ws_text(req, build_profiles_message());
    send_ws_text(req, build_current_profile_message(current_profile));
    uart_send("t\n");
    return ESP_OK;
  }

  httpd_ws_frame_t frame = {};
  frame.type = HTTPD_WS_TYPE_TEXT;
  esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
  if (err != ESP_OK) {
    return err;
  }

  std::vector<uint8_t> payload(frame.len + 1, 0);
  frame.payload = payload.data();
  err = httpd_ws_recv_frame(req, &frame, frame.len);
  if (err != ESP_OK) {
    return err;
  }

  cJSON* root = cJSON_Parse(reinterpret_cast<const char*>(payload.data()));
  if (!root || !cJSON_IsArray(root)) {
    cJSON_Delete(root);
    return ESP_OK;
  }

  cJSON* action_item = cJSON_GetArrayItem(root, 0);
  const char* action = cJSON_IsString(action_item) ? action_item->valuestring : nullptr;
  if (!action) {
    cJSON_Delete(root);
    return ESP_OK;
  }

  if (strcmp(action, "update_threshold") == 0) {
    handle_update_threshold(root);
  } else if (strcmp(action, "save_thresholds") == 0) {
    uart_send("s\n");
  } else if (strcmp(action, "add_profile") == 0) {
    cJSON* name_item = cJSON_GetArrayItem(root, 2);
    cJSON* thresholds_item = cJSON_GetArrayItem(root, 3);
    std::array<int, kNumSensors> thresholds{};
    if (cJSON_IsString(name_item) && name_item->valuestring &&
        strcmp(name_item->valuestring, kPublicProfileName) != 0 &&
        parse_threshold_array(thresholds_item, thresholds)) {
      add_profile(name_item->valuestring, thresholds);
    }
  } else if (strcmp(action, "remove_profile") == 0) {
    cJSON* name_item = cJSON_GetArrayItem(root, 2);
    if (cJSON_IsString(name_item) && name_item->valuestring &&
        strcmp(name_item->valuestring, kPublicProfileName) != 0) {
      remove_profile(name_item->valuestring);
    }
  } else if (strcmp(action, "update_public_profile") == 0) {
    cJSON* password_item = cJSON_GetArrayItem(root, 2);
    cJSON* thresholds_item = cJSON_GetArrayItem(root, 3);
    std::array<int, kNumSensors> thresholds{};
    if (!cJSON_IsString(password_item) || !password_item->valuestring ||
        strcmp(password_item->valuestring, kAdminPassword) != 0) {
      send_ws_text(req, build_admin_auth_error_message());
    } else if (parse_threshold_array(thresholds_item, thresholds)) {
      bool current_public = false;
      {
        std::lock_guard<std::mutex> lock(gStateMutex);
        gPublicThresholds = thresholds;
        current_public = gCurrentProfileName == kPublicProfileName;
        if (current_public) {
          gCurrentThresholds = gPublicThresholds;
        }
        save_public_profile_locked();
      }
      if (current_public) {
        queue_ws_broadcast(build_thresholds_message(thresholds));
      }
      send_ws_text(req, build_public_profile_updated_message());
      queue_ws_broadcast(build_profiles_message());
    }
  } else if (strcmp(action, "change_profile") == 0) {
    cJSON* name_item = cJSON_GetArrayItem(root, 2);
    if (cJSON_IsString(name_item) && name_item->valuestring) {
      change_profile(name_item->valuestring);
      std::array<int, kNumSensors> thresholds{};
      {
        std::lock_guard<std::mutex> lock(gStateMutex);
        thresholds = gCurrentThresholds;
      }
      for (int i = 0; i < kNumSensors; ++i) {
        send_threshold_to_arduino(i, thresholds[i]);
      }
    }
  } else if (strcmp(action, "set_serial_port") == 0) {
    send_ws_text(req, build_serial_port_message());
  } else if (strcmp(action, "refresh_serial_port_candidates") == 0) {
    send_ws_text(req, build_serial_port_candidates_message());
  }

  cJSON_Delete(root);
  return ESP_OK;
}

void wifi_event_handler(void*, esp_event_base_t event_base, int32_t event_id, void* event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    gStaConnected.store(false);
    gStaIpAddress.clear();
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    auto* got_ip = static_cast<ip_event_got_ip_t*>(event_data);
    char ip[16] = {};
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&got_ip->ip_info.ip));
    gStaIpAddress = ip;
    gStaConnected.store(true);
  }
}

esp_err_t set_ap_config() {
  wifi_config_t ap_config = {};
  strlcpy(reinterpret_cast<char*>(ap_config.ap.ssid), kApSsid, sizeof(ap_config.ap.ssid));
  strlcpy(reinterpret_cast<char*>(ap_config.ap.password), kApPassword, sizeof(ap_config.ap.password));
  ap_config.ap.ssid_len = strlen(kApSsid);
  ap_config.ap.channel = 1;
  ap_config.ap.max_connection = 4;
  ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
  if (strlen(kApPassword) == 0) {
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
  }
  return esp_wifi_set_config(WIFI_IF_AP, &ap_config);
}

bool try_connect_sta_candidates(bool from_ap_mode) {
#if !HAS_WIFI_SECRETS
  (void)from_ap_mode;
  ESP_LOGI(kTag, "wifi_secrets.h not found. STA disabled.");
  return false;
#else
  if (STA_CANDIDATE_COUNT == 0) {
    ESP_LOGI(kTag, "No STA candidates configured.");
    return false;
  }

  esp_wifi_set_mode(from_ap_mode ? WIFI_MODE_APSTA : WIFI_MODE_STA);
  if (from_ap_mode) {
    set_ap_config();
  }

  for (size_t i = 0; i < STA_CANDIDATE_COUNT; ++i) {
    const char* ssid = STA_CANDIDATE_SSIDS[i];
    const char* password = STA_CANDIDATE_PASSWORDS[i];
    if (!ssid || ssid[0] == '\0') {
      continue;
    }

    wifi_config_t sta_config = {};
    strlcpy(reinterpret_cast<char*>(sta_config.sta.ssid), ssid, sizeof(sta_config.sta.ssid));
    strlcpy(reinterpret_cast<char*>(sta_config.sta.password), password ? password : "", sizeof(sta_config.sta.password));
    sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    gStaConnected.store(false);
    gStaIpAddress.clear();
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    esp_wifi_connect();

    const uint64_t start = millis64();
    while (!gStaConnected.load() && millis64() - start < kStaConnectTimeoutMs) {
      vTaskDelay(pdMS_TO_TICKS(200));
      update_status_led();
    }

    if (gStaConnected.load()) {
      ESP_LOGI(kTag, "STA connected: %s ip=%s", ssid, gStaIpAddress.c_str());
      gWifiModeState = WifiModeState::kSta;
      return true;
    }
  }

  return false;
#endif
}

void start_ap_mode(const char* reason) {
  ESP_LOGI(kTag, "Switching to AP mode (%s)", reason);
  esp_wifi_disconnect();
  esp_wifi_set_mode(WIFI_MODE_AP);
  if (set_ap_config() != ESP_OK) {
    set_error("Failed to configure SoftAP");
  }
  gWifiModeState = WifiModeState::kAp;
  gLastStaAttemptTimeMs = millis64();
}

void switch_to_sta_mode_after_connect() {
  esp_wifi_set_mode(WIFI_MODE_STA);
  gWifiModeState = WifiModeState::kSta;
  ESP_LOGI(kTag, "Running in STA mode. IP: %s", gStaIpAddress.c_str());
}

void init_wifi() {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();
  esp_netif_create_default_wifi_ap();

  wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&config));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_start());
}

void init_spiffs() {
  esp_vfs_spiffs_conf_t conf = {};
  conf.base_path = kMountPath;
  conf.partition_label = "storage";
  conf.max_files = 8;
  conf.format_if_mount_failed = true;
  ESP_ERROR_CHECK(esp_vfs_spiffs_register(&conf));
}

void init_uart() {
  uart_config_t config = {};
  config.baud_rate = kArduinoBaud;
  config.data_bits = UART_DATA_8_BITS;
  config.parity = UART_PARITY_DISABLE;
  config.stop_bits = UART_STOP_BITS_1;
  config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  config.source_clk = UART_SCLK_DEFAULT;
  ESP_ERROR_CHECK(uart_driver_install(kArduinoUart, 2048, 0, 0, nullptr, 0));
  ESP_ERROR_CHECK(uart_param_config(kArduinoUart, &config));
  ESP_ERROR_CHECK(uart_set_pin(kArduinoUart, kArduinoTxPin, kArduinoRxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

void init_led() {
  gpio_config_t config = {};
  config.pin_bit_mask = 1ULL << kStatusLedPin;
  config.mode = GPIO_MODE_OUTPUT;
  config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  config.pull_up_en = GPIO_PULLUP_DISABLE;
  config.intr_type = GPIO_INTR_DISABLE;
  ESP_ERROR_CHECK(gpio_config(&config));
  set_led(false);
}

void init_nvs() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
}

httpd_handle_t start_http_server() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.uri_match_fn = httpd_uri_match_wildcard;
  config.max_uri_handlers = 12;
  config.stack_size = kHttpServerStackSize;

  httpd_handle_t server = nullptr;
  if (httpd_start(&server, &config) != ESP_OK) {
    return nullptr;
  }

  httpd_uri_t defaults_uri = {};
  defaults_uri.uri = "/defaults";
  defaults_uri.method = HTTP_GET;
  defaults_uri.handler = defaults_handler;
  httpd_register_uri_handler(server, &defaults_uri);

  httpd_uri_t ws_uri = {};
  ws_uri.uri = "/ws";
  ws_uri.method = HTTP_GET;
  ws_uri.handler = ws_handler;
  ws_uri.is_websocket = true;
  httpd_register_uri_handler(server, &ws_uri);

  httpd_uri_t static_uri = {};
  static_uri.uri = "/*";
  static_uri.method = HTTP_GET;
  static_uri.handler = static_handler;
  httpd_register_uri_handler(server, &static_uri);

  return server;
}

void bridge_task(void*) {
  while (true) {
    update_status_led();

    if (gWifiModeState == WifiModeState::kSta) {
      if (!gStaConnected.load()) {
        start_ap_mode("STA disconnected");
      }
    } else {
      const uint64_t now = millis64();
      if (now - gLastStaAttemptTimeMs >= kStaRetryIntervalMs) {
        gLastStaAttemptTimeMs = now;
        if (try_connect_sta_candidates(true)) {
          switch_to_sta_mode_after_connect();
        } else {
          start_ap_mode("periodic STA retry failed");
        }
      }
    }

    if (websocket_client_count() > 0) {
      const uint64_t now = millis64();
      if (now - gLastValueRequestTimeMs >= kUiIntervalMs) {
        uart_send("v\n");
        gLastValueRequestTimeMs = now;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

}  // namespace

extern "C" void app_main(void) {
  init_nvs();
  init_led();
  init_spiffs();
  load_profiles();
  load_public_profile();

  {
    std::lock_guard<std::mutex> lock(gStateMutex);
    gCurrentProfileName = kPublicProfileName;
    gCurrentThresholds = gPublicThresholds;
  }

  init_uart();
  init_wifi();

  if (try_connect_sta_candidates(false)) {
    gWifiModeState = WifiModeState::kSta;
    ESP_LOGI(kTag, "Running in STA mode. IP: %s", gStaIpAddress.c_str());
  } else {
    start_ap_mode("initial STA connection failed");
  }

  gHttpServer = start_http_server();
  if (!gHttpServer) {
    set_error("Failed to start HTTP server");
  }

  vTaskDelay(pdMS_TO_TICKS(500));
  apply_all_thresholds();
  gLastStatTimeMs = millis64();

  xTaskCreate(uart_task, "fsr_uart", 4096, nullptr, 5, nullptr);
  xTaskCreate(bridge_task, "fsr_bridge", 4096, nullptr, 4, nullptr);
}