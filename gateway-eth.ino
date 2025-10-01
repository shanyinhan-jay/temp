#include <ETH.h>
#include <painlessMesh.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <map>
#include <vector>
#include <WebServer.h>
#include <Update.h>
#include <Adafruit_NeoPixel.h>
#include "esp_task_wdt.h"
#include <time.h>
#include "nvs_flash.h"
#include "backend_config.h"
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>

// =================================================================================
// Configuration - 现在从 backend_config.h 引入
// =================================================================================



String macToKey(const String& mac) {
  String key = mac;
  key.replace(":", "");
  return key;
}
// =================================================================================
// Global Objects & Data Structures
// =================================================================================
painlessMesh  mesh;
WiFiClient    ethClient;
PubSubClient  mqttClient(ethClient);
Preferences   preferences;
Scheduler     userScheduler;
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

char MQTT_SERVER[40];
char MQTT_PORT_STR[6];
char MQTT_USER[33];
char MQTT_PASS[65];
// 移除GATEWAY_ADDRESS变量，改用动态获取IP

enum LedState { STATE_NO_NETWORK, STATE_NO_MQTT, STATE_ALL_CONNECTED, STATE_DEVICE_OFFLINE, STATE_OTA_UPDATING };
Adafruit_NeoPixel statusPixel(STATUS_LED_COUNT, STATUS_LED_PIN, NEO_GRB + NEO_KHZ800);
LedState currentLedState = STATE_NO_NETWORK;

struct MeshDevice {
  uint32_t nodeId; String name; String macAddress; String type;
  unsigned long lastSeenTimestamp; bool isOnline; bool hasBeenDiscovered;
  String linkedEnvSensorName; uint32_t daylightThresholdLx; bool isDaytime;
  uint8_t on_r, on_g, on_b; uint8_t off_r, off_g, off_b;
  uint8_t brightness_on, brightness_off; uint8_t default_on_value, default_off_value;
  std::vector<String> sensorTypes; // 新增成员，用于存储传感器类型
};
std::map<String, MeshDevice> knownDevicesByMac;
std::map<uint32_t, String> nodeIdToMacMap;
std::map<String, String> nameToMacMap;

struct ActionStep { String deviceName; String statesJson; };
struct Action { String name; std::vector<ActionStep> steps; };
std::map<String, Action> actions;

// 全局变量用于存储POST请求的JSON数据
String networkConfigJsonData = "";
bool networkConfigDataReceived = false;

// =================================================================================
// Function Declarations
// =================================================================================
void WiFiEvent(WiFiEvent_t event);
void tryMqttReconnect();
void initStatusLed();
void setStatusLed(LedState newState);
void blinkLedCallback();
void updateSystemStatus();
void onMeshReceived(uint32_t from, String &msg);
void processMeshMessage(String &message);
void onMeshNewConnection(uint32_t nodeId);
void onMeshDroppedConnection(uint32_t nodeId);
void checkDeviceStatus();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void loadActions();
void triggerAction(const String& actionName);
void publishActionDiscovery();
void unpublishActionDiscovery(const String& actionName);
void publishGatewayDiscovery();
void publishGatewayStatus();
void publishDeviceDeletion(const MeshDevice& device);
void addCorsHeaders(AsyncWebServerRequest *request);
void sendCorsResponse(AsyncWebServerRequest *request, int code, const char* contentType, const String& content);
void handleRoot(AsyncWebServerRequest *request);
void handleAppJs(AsyncWebServerRequest *request);
void handleConfigJs(AsyncWebServerRequest *request);
void handleHtmlConfigJs(AsyncWebServerRequest *request);
void handleCssConfigJs(AsyncWebServerRequest *request);
void handleApiDevices(AsyncWebServerRequest *request);
void handleApiStatus(AsyncWebServerRequest *request);
void handleApiSystemMonitor(AsyncWebServerRequest *request);
void handleApiActions(AsyncWebServerRequest *request);
void handleApiSaveDeviceConfig(AsyncWebServerRequest *request);
void handleApiExportConfig(AsyncWebServerRequest *request);
void handleApiAvailableSensors(AsyncWebServerRequest *request);
void handleApiAddAction(AsyncWebServerRequest *request);
void handleApiSaveAction(AsyncWebServerRequest *request);
void handleApiDeleteAction(AsyncWebServerRequest *request);
void handleNotFound(AsyncWebServerRequest *request);
void handleOTAUpdatePage(AsyncWebServerRequest *request);
void handleOTAUpdateUpload(AsyncWebServerRequest *request, const String& filename, size_t index, uint8_t *data, size_t len, bool final);
void handleApiOTAUpload(AsyncWebServerRequest *request);
void handleApiOTAStatus(AsyncWebServerRequest *request);
void handleApiNetworkConfig(AsyncWebServerRequest *request);
void handleApiNetworkConfigPost(AsyncWebServerRequest *request);
void publishDiscoveryForDevice(const char* deviceType, const char* deviceName, int channels, const String& originalMessage);
void loadCredentials();
void saveCredentials();
String macToKey(const String& mac);
void loadAddressBook();
void saveToAddressBook(const MeshDevice& device);
void clearAllNvs();
bool saveServoInfoToNVS(const String& message);
bool saveActionToNVS(const String& actionName, const JsonDocument& actionDoc);
void listActionsFromNVS();
String getActionFromNVS(const String& actionName);
bool deleteActionFromNVS(const String& actionName);
void handleSerialCommands();
void listServoDevicesFromNVS();
void listLedDevicesFromNVS();
void onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len);
void broadcastWebSocketMessage(const String& message);
void sendDeviceStatusUpdate(const String& deviceMac, const String& status);
void sendActionStatusUpdate(const String& actionName, const String& status);

// =================================================================================
// OTA Status Variables
// =================================================================================
struct OTAStatus {
  bool inProgress = false;
  String filename = "";
  size_t totalSize = 0;
  size_t uploadedSize = 0;
  String status = "idle";
  String error = "";
  unsigned long startTime = 0;
} otaStatus;

// =================================================================================
// Tasks
// =================================================================================
Task taskCheckDevices(TASK_MINUTE, TASK_FOREVER, &checkDeviceStatus);
Task taskReconnectMQTT(TASK_SECOND * 5, TASK_FOREVER, &tryMqttReconnect);
Task taskBlinkLed(TASK_SECOND / 5, TASK_FOREVER, &blinkLedCallback);
Task taskUpdateSystemStatus(TASK_SECOND * 2, TASK_FOREVER, &updateSystemStatus);
Task taskPublishGatewayStatus(TASK_SECOND * 30, TASK_FOREVER, &publishGatewayStatus);

// =================================================================================
// SETUP
// =================================================================================
void setup() {
  Serial.begin(115200);

  // One-time check to fix non-JSON action_list
  preferences.begin("actions", false);
  String actionList = preferences.getString("action_list", "[]");
  if (!actionList.startsWith("[") && !actionList.endsWith("]")) {
    Serial.println("Detected non-JSON action_list. Resetting to '[]'.");
    preferences.putString("action_list", "[]");
  }
  preferences.end();
  
  esp_reset_reason_t reason = esp_reset_reason();
  Serial.printf("Booting... Reset reason: %d\n", reason);
  if (reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT) {
    Serial.println("Watchdog timer triggered the last reset!");
    preferences.begin("logs", false);
    preferences.putString("wdt_log_3", preferences.getString("wdt_log_2", ""));
    preferences.putString("wdt_log_2", preferences.getString("wdt_log_1", ""));
    char log_entry[50];
    snprintf(log_entry, sizeof(log_entry), "WDT Reset at Uptime: %lu s", millis() / 1000);
    preferences.putString("wdt_log_1", String(log_entry));
    preferences.end();
  }
  
  initStatusLed();
  Serial.println("\n--- PainlessMesh Ethernet Gateway (API Backend) ---");

  WiFi.onEvent(WiFiEvent);
  ETH.begin(ETH_ADDR, ETH_POWER_PIN, ETH_MDC_PIN, ETH_MDIO_PIN, ETH_TYPE, ETH_CLK_MODE);
  
  // 使用DHCP自动获取IP配置
  Serial.println("Using DHCP to obtain IP configuration...");

  loadCredentials();
  loadAddressBook();
  
  Serial.println("\n=== SETUP: Loading Actions ===");
  loadActions();
  Serial.printf("SETUP: Actions loaded count: %d\n", actions.size());
  for (auto const& [actionName, action] : actions) {
    Serial.printf("SETUP: Found action: %s with %d steps\n", actionName.c_str(), action.steps.size());
  }
  Serial.println("=== SETUP: Actions Loading Complete ===\n");

  mqttClient.setServer(MQTT_SERVER, atoi(MQTT_PORT_STR));
  mqttClient.setCallback(mqttCallback);

  mesh.init(MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT, WIFI_AP, 11);
  mesh.onReceive(&onMeshReceived);
  mesh.onNewConnection(&onMeshNewConnection);
  mesh.onDroppedConnection(&onMeshDroppedConnection);
  

  server.on("/update", HTTP_GET, handleOTAUpdatePage);
  
  // 添加基础的路由调试信息
  server.onNotFound([](AsyncWebServerRequest *request) {
    Serial.printf("*** 404 NOT FOUND *** URL: %s, Method: %d\n", 
                  request->url().c_str(), request->method());
  });
  
  server.on("/update_upload", HTTP_OPTIONS, [](AsyncWebServerRequest *request) { 
    Serial.printf("*** OPTIONS /update_upload called ***\n");
    addCorsHeaders(request); 
    AsyncWebServerResponse *response = request->beginResponse(204);
    response->addHeader("Access-Control-Allow-Origin", "*");
    response->addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    response->addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
    request->send(response);
  });
  
  Serial.println("Setting up /update_upload POST route...");
  server.on("/update_upload", HTTP_POST, 
    NULL,  // 基础处理器设为NULL
    [](AsyncWebServerRequest *request, const String& filename, size_t index, uint8_t *data, size_t len, bool final) {
      Serial.printf("*** FILE UPLOAD HANDLER CALLED *** Filename: %s, Index: %u, Len: %u, Final: %s\n", 
                    filename.c_str(), index, len, final ? "true" : "false");
      handleOTAUpdateUpload(request, filename, index, data, len, final);
    }
  );
  
  // Static file serving
  server.on("/", HTTP_GET, handleRoot);
  server.on("/index.html", HTTP_GET, handleRoot);
  server.on("/app.js", HTTP_GET, handleAppJs);
  server.on("/config.js", HTTP_GET, handleConfigJs);
  server.on("/html_config.js", HTTP_GET, handleHtmlConfigJs);
  server.on("/css_config.js", HTTP_GET, handleCssConfigJs);
  
  // API Routes
  server.on("/api/devices", HTTP_GET, handleApiDevices);
  server.on("/api/devices", HTTP_OPTIONS, [](AsyncWebServerRequest *request) { addCorsHeaders(request); request->send(204); });
  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.on("/api/status", HTTP_OPTIONS, [](AsyncWebServerRequest *request) { addCorsHeaders(request); request->send(204); });
  server.on("/api/system_monitor", HTTP_GET, handleApiSystemMonitor);
  server.on("/api/system_monitor", HTTP_OPTIONS, [](AsyncWebServerRequest *request) { addCorsHeaders(request); request->send(204); });
  server.on("/api/actions", HTTP_GET, handleApiActions);
  server.on("/api/actions", HTTP_OPTIONS, [](AsyncWebServerRequest *request) { addCorsHeaders(request); request->send(204); });
  server.on("/api/save_device_config", HTTP_POST, handleApiSaveDeviceConfig);
  server.on("/api/save_device_config", HTTP_OPTIONS, [](AsyncWebServerRequest *request) { addCorsHeaders(request); request->send(204); });
  server.on("/api/export_config", HTTP_GET, handleApiExportConfig);
  server.on("/api/export_config", HTTP_OPTIONS, [](AsyncWebServerRequest *request) { addCorsHeaders(request); request->send(204); });
  server.on("/api/available_sensors", HTTP_GET, handleApiAvailableSensors);
  server.on("/api/available_sensors", HTTP_OPTIONS, [](AsyncWebServerRequest *request) { addCorsHeaders(request); request->send(204); });
  server.on("/api/add_action", HTTP_POST, handleApiAddAction);
  server.on("/api/add_action", HTTP_OPTIONS, [](AsyncWebServerRequest *request) { addCorsHeaders(request); request->send(204); });
  server.on("/api/save_action", HTTP_POST, handleApiSaveAction);
  server.on("/api/save_action", HTTP_OPTIONS, [](AsyncWebServerRequest *request) { addCorsHeaders(request); request->send(204); });
  server.on("/api/delete_action", HTTP_POST, handleApiDeleteAction);
  server.on("/api/delete_action", HTTP_OPTIONS, [](AsyncWebServerRequest *request) { addCorsHeaders(request); request->send(204); });
  server.on("/api/ota_upload", HTTP_POST, handleApiOTAUpload);
  server.on("/api/ota_upload", HTTP_OPTIONS, [](AsyncWebServerRequest *request) { addCorsHeaders(request); request->send(204); });
  server.on("/api/ota_status", HTTP_GET, handleApiOTAStatus);
  server.on("/api/ota_status", HTTP_OPTIONS, [](AsyncWebServerRequest *request) { addCorsHeaders(request); request->send(204); });
  server.on("/api/network_config", HTTP_GET, handleApiNetworkConfig);
  server.on("/api/network_config", HTTP_POST, [](AsyncWebServerRequest *request) {
    // 空的请求处理函数，实际数据在onBody中处理
  }, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
    // onBody回调函数 - 处理POST请求体数据
    if (index == 0) {
      networkConfigJsonData = "";
      networkConfigDataReceived = false;
    }
    
    // 将接收到的数据追加到全局变量中
    for (size_t i = 0; i < len; i++) {
      networkConfigJsonData += (char)data[i];
    }
    
    // 如果这是最后一块数据，标记为接收完成并处理
    if (index + len == total) {
      networkConfigDataReceived = true;
      Serial.println("=== onBody回调 - 接收到完整JSON数据 ===");
      Serial.println("数据长度: " + String(total));
      Serial.println("JSON内容: " + networkConfigJsonData);
      
      // 调用处理函数
      handleApiNetworkConfigPost(request);
    }
  });
  server.on("/api/network_config", HTTP_OPTIONS, [](AsyncWebServerRequest *request) { 
    addCorsHeaders(request);
    AsyncWebServerResponse *response = request->beginResponse(204);
    response->addHeader("Access-Control-Allow-Origin", "*");
    response->addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    response->addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
    request->send(response);
  });
  server.on("/api/reboot", HTTP_POST, handleApiReboot);
  server.on("/api/reboot", HTTP_OPTIONS, [](AsyncWebServerRequest *request) { addCorsHeaders(request); request->send(204); });
  server.on("/api/clear_nvs", HTTP_POST, handleApiClearNvs);
  server.on("/api/clear_nvs", HTTP_OPTIONS, [](AsyncWebServerRequest *request) { addCorsHeaders(request); request->send(204); });
  
  server.onNotFound(handleNotFound);

  // Initialize WebSocket
  ws.onEvent(onWebSocketEvent);
  server.addHandler(&ws);

  server.begin();
  Serial.println("Web server started.");
  Serial.println("WebSocket server started on /ws");

  userScheduler.addTask(taskCheckDevices); taskCheckDevices.enable();
  userScheduler.addTask(taskReconnectMQTT); taskReconnectMQTT.enable();
  userScheduler.addTask(taskBlinkLed);
  userScheduler.addTask(taskUpdateSystemStatus); taskUpdateSystemStatus.enable();
  userScheduler.addTask(taskPublishGatewayStatus); taskPublishGatewayStatus.enable();

  Serial.printf("Initializing Task Watchdog Timer with a %d second timeout...\n", WDT_TIMEOUT_SECONDS);
  esp_task_wdt_init(WDT_TIMEOUT_SECONDS, true);
  esp_task_wdt_add(NULL);

  Serial.println("Gateway setup complete.");
}

// =================================================================================
// LOOP
// =================================================================================
void loop() {
  esp_task_wdt_reset();
  userScheduler.execute();
  mqttClient.loop();
  handleSerialCommands(); // 处理串口命令
}

// =================================================================================
// Status LED & System Status Functions
// =================================================================================
const uint32_t COLOR_RED = Adafruit_NeoPixel::Color(255, 0, 0);
const uint32_t COLOR_ORANGE = Adafruit_NeoPixel::Color(255, 100, 0);
const uint32_t COLOR_BLUE = Adafruit_NeoPixel::Color(0, 0, 255);
const uint32_t COLOR_OFF = Adafruit_NeoPixel::Color(0, 0, 0);

void blinkLedCallback() {
  static bool ledOn = false; 
  ledOn = !ledOn;
  uint32_t colorToShow = COLOR_OFF;
  if (ledOn) {
    switch (currentLedState) {
      case STATE_NO_NETWORK: colorToShow = COLOR_RED; break;
      case STATE_NO_MQTT: colorToShow = COLOR_ORANGE; break;
      case STATE_DEVICE_OFFLINE: colorToShow = COLOR_BLUE; break;
      default: break;
    }
  }
  statusPixel.setPixelColor(0, colorToShow); 
  statusPixel.show();
}

void setStatusLed(LedState newState) {
  if (newState == currentLedState) return;
  currentLedState = newState;
  taskBlinkLed.disable();
  switch (newState) {
    case STATE_NO_NETWORK: 
      taskBlinkLed.setInterval(TASK_SECOND / 5); 
      taskBlinkLed.enable(); 
      break;
    case STATE_NO_MQTT: 
      taskBlinkLed.setInterval(TASK_SECOND); 
      taskBlinkLed.enable(); 
      break;
    case STATE_DEVICE_OFFLINE: 
      taskBlinkLed.setInterval(TASK_SECOND * 1.5); 
      taskBlinkLed.enable(); 
      break;
    case STATE_OTA_UPDATING:
      statusPixel.setPixelColor(0, COLOR_BLUE);
      statusPixel.show();
      break;
    case STATE_ALL_CONNECTED: 
      statusPixel.setPixelColor(0, COLOR_OFF); 
      statusPixel.show(); 
      break;
  }
}

// =================================================================================
// OTA API Handlers
// =================================================================================
void handleApiOTAUpload(AsyncWebServerRequest *request) {
  addCorsHeaders(request);
  
  JsonDocument doc;
  doc["success"] = false;
  doc["error"] = "Use multipart upload endpoint /update_upload instead";
  String response;
  serializeJson(doc, response);
  sendCorsResponse(request, 400, "application/json", response);
}

void handleApiOTAStatus(AsyncWebServerRequest *request) {
  addCorsHeaders(request);
  
  JsonDocument doc;
  doc["inProgress"] = otaStatus.inProgress;
  doc["filename"] = otaStatus.filename;
  doc["totalSize"] = otaStatus.totalSize;
  doc["uploadedSize"] = otaStatus.uploadedSize;
  doc["status"] = otaStatus.status;
  doc["error"] = otaStatus.error;
  
  if (otaStatus.inProgress && otaStatus.totalSize > 0) {
    doc["progress"] = (float)otaStatus.uploadedSize / otaStatus.totalSize * 100.0;
  } else {
    doc["progress"] = 0;
  }
  
  if (otaStatus.startTime > 0) {
    doc["elapsedTime"] = millis() - otaStatus.startTime;
  } else {
    doc["elapsedTime"] = 0;
  }
  
  String response;
  serializeJson(doc, response);
  sendCorsResponse(request, 200, "application/json", response);
}

void initStatusLed() {
  statusPixel.begin(); 
  statusPixel.setBrightness(LED_BRIGHTNESS);
  statusPixel.clear(); 
  statusPixel.show();
  setStatusLed(STATE_NO_NETWORK);
  Serial.println("Status LED module initialized.");
}

void updateSystemStatus() {
  if (!ETH.linkUp()) { 
    setStatusLed(STATE_NO_NETWORK); 
    return; 
  }
  if (!mqttClient.connected()) { 
    setStatusLed(STATE_NO_MQTT); 
    taskReconnectMQTT.enableIfNot(); 
    return; 
  }
  bool foundOfflineDevice = false;
  unsigned long now = millis();
  for (auto& pair : knownDevicesByMac) {
    // 检查两种情况：1) 当前在线但超时的设备  2) 已经标记为离线的设备
    if ((pair.second.isOnline && (now - pair.second.lastSeenTimestamp) > DEVICE_OFFLINE_TIMEOUT) || 
        (!pair.second.isOnline)) {
      foundOfflineDevice = true; 
      break;
    }
  }
  if (foundOfflineDevice) { 
    setStatusLed(STATE_DEVICE_OFFLINE); 
  } else { 
    setStatusLed(STATE_ALL_CONNECTED); 
  }
}

void publishGatewayStatus() {
  if (!mqttClient.connected()) return;
  
  // 1. Publish uptime (formatted as days, hours, minutes)
  unsigned long s = millis() / 1000;
  String uptimeStr = String(s / 86400) + "d " + String((s % 86400) / 3600) + "h " + String((s % 3600) / 60) + "m";
  mqttClient.publish("PainlessMesh/Gateway/Status/uptime", uptimeStr.c_str(), true);
  
  // 2. Publish memory usage percentage
  size_t freeHeap = ESP.getFreeHeap();
  size_t totalHeap = ESP.getHeapSize();
  float memoryUsage = ((float)(totalHeap - freeHeap) / totalHeap) * 100.0;
  mqttClient.publish("PainlessMesh/Gateway/Status/memory_usage", String(memoryUsage, 1).c_str(), true);
  
  // 3. Publish connected devices count
  int connectedCount = 0;
  for (auto& pair : knownDevicesByMac) {
    if (pair.second.isOnline) connectedCount++;
  }
  mqttClient.publish("PainlessMesh/Gateway/Status/connected_devices", String(connectedCount).c_str(), true);
  
  // 4. Publish network status
  String networkStatus = "disconnected";
  if (ETH.linkUp()) {
    if (mqttClient.connected()) {
      networkStatus = "connected";
    } else {
      networkStatus = "ethernet_only";
    }
  }
  mqttClient.publish("PainlessMesh/Gateway/Status/network_status", networkStatus.c_str(), true);
  
  // 5. Publish CPU usage (approximated by free heap and task load)
  static unsigned long lastLoopTime = 0;
  unsigned long currentTime = millis();
  float cpuUsage = 0;
  if (lastLoopTime > 0) {
    // Simple approximation: higher memory usage + shorter loop intervals = higher CPU usage
    float memoryFactor = memoryUsage / 100.0;
    float loopInterval = currentTime - lastLoopTime;
    cpuUsage = min(100.0f, memoryFactor * 50 + (loopInterval < 10 ? 50 : 0));
  }
  lastLoopTime = currentTime;
  mqttClient.publish("PainlessMesh/Gateway/Status/cpu_usage", String(cpuUsage, 1).c_str(), true);
  
  // 6. Publish NVS usage percentage
  nvs_stats_t nvs_stats;
  esp_err_t err = nvs_get_stats(NULL, &nvs_stats);
  if (err == ESP_OK) {
    float nvsUsage = nvs_stats.total_entries > 0 ? (float)(nvs_stats.used_entries * 100) / nvs_stats.total_entries : 0;
    mqttClient.publish("PainlessMesh/Gateway/Status/nvs_usage", String(nvsUsage, 1).c_str(), true);
  }
  
  Serial.println("Gateway status published to MQTT");
}

// =================================================================================
// Network & MQTT Functions
// =================================================================================
void WiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START: 
      Serial.println("ETH Started"); 
      ETH.setHostname("painlessmesh-gateway"); 
      break;
    case ARDUINO_EVENT_ETH_CONNECTED: 
      Serial.println("ETH Connected"); 
      break;
    case ARDUINO_EVENT_ETH_GOT_IP: 
      Serial.print("ETH Got IP via DHCP: "); 
      Serial.println(ETH.localIP());
      Serial.print("Gateway: "); 
      Serial.println(ETH.gatewayIP());
      Serial.print("Subnet: "); 
      Serial.println(ETH.subnetMask());
      Serial.print("DNS: "); 
      Serial.println(ETH.dnsIP());
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED: 
      Serial.println("ETH Disconnected"); 
      break;
    case ARDUINO_EVENT_ETH_STOP: 
      Serial.println("ETH Stopped"); 
      break;
    default: break;
  }
}

void tryMqttReconnect() {
  if (mqttClient.connected()) { 
    taskReconnectMQTT.disable(); 
    return; 
  }
  Serial.print("Attempting MQTT connection...");
  
  bool result;
  if (strlen(MQTT_USER) > 0) {
    result = mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS, AVAILABILITY_TOPIC, 0, true, PAYLOAD_NOT_AVAILABLE);
  } else {
    result = mqttClient.connect(MQTT_CLIENT_ID, NULL, NULL, AVAILABILITY_TOPIC, 0, true, PAYLOAD_NOT_AVAILABLE);
  }
  
  if (result) {
    Serial.println("connected!");
    mqttClient.publish(AVAILABILITY_TOPIC, PAYLOAD_AVAILABLE, true);
    mqttClient.subscribe("PainlessMesh/Command/#");
    mqttClient.subscribe("PainlessMesh/Config/#");
    mqttClient.subscribe(MQTT_GATEWAY_COMMAND_TOPIC);
    mqttClient.subscribe("PainlessMesh/GroupCommand/ActionControl");
    publishActionDiscovery();
    publishGatewayDiscovery();
    taskReconnectMQTT.disable();
  } else { 
    Serial.print("failed, rc="); 
    Serial.println(mqttClient.state()); 
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String topic_str = String(topic);
  String payload_str; 
  payload_str.reserve(length);
  for (int i = 0; i < length; i++) { payload_str += (char)payload[i]; }

  if (topic_str.equals(MQTT_GATEWAY_COMMAND_TOPIC)) {
    if (payload_str.equals("CLEAR_NVS")) { 
      clearAllNvs(); 
      delay(100); 
      ESP.restart(); 
    }
    return;
  }

  if (topic_str.equals("PainlessMesh/GroupCommand/ActionControl")) {
    Serial.println("--- Received Action Command from HA ---");
    
    // 添加原始payload调试信息
    Serial.printf("  -> Raw payload length: %d\n", length);
    Serial.printf("  -> Raw payload content: %.*s\n", length, (char*)payload);
    
    StaticJsonDocument<1536> finalMeshCommand;
    finalMeshCommand["cmd"] = "groupState";

    // Parse incoming doc from HA
    DynamicJsonDocument inDoc(4096);
    DeserializationError derr = deserializeJson(inDoc, payload, length);
    if (derr) {
      Serial.printf("  [ERROR] Invalid JSON from HA: %s\n", derr.c_str());
      return;
    }
    
    // 调试输入文档结构
    String inDocStr;
    serializeJson(inDoc, inDocStr);
    Serial.printf("  -> Parsed input document: %s\n", inDocStr.c_str());
    
    // 检查payload字段
     if (inDoc.containsKey("payload")) {
       Serial.printf("  -> Input contains 'payload' key\n");
       if (inDoc["payload"].is<JsonArray>()) {
         Serial.printf("  -> Input payload is JsonArray with %d elements\n", inDoc["payload"].as<JsonArray>().size());
       } else {
         Serial.printf("  -> Input payload is NOT JsonArray\n");
         String payloadStr;
         serializeJson(inDoc["payload"], payloadStr);
         Serial.printf("  -> Input payload content: %s\n", payloadStr.c_str());
       }
     } else {
       Serial.printf("  -> Input does NOT contain 'payload' key\n");
     }

    JsonArray inPayload = inDoc["payload"].is<JsonArray>() ? inDoc["payload"].as<JsonArray>() : JsonArray();
    JsonArray outPayload = finalMeshCommand.createNestedArray("payload");
    
    Serial.printf("  -> Input payload array size: %d\n", inPayload.size());

    if (inPayload) {
      Serial.printf("  -> Processing %d actions from input payload\n", inPayload.size());
      for (JsonObject inAction : inPayload) {
        Serial.printf("  -> Processing action: ");
        String actionStr;
        serializeJson(inAction, actionStr);
        Serial.printf("%s\n", actionStr.c_str());
        
        JsonObject outAction = outPayload.createNestedObject();
        if (inAction.containsKey("name")) {
          outAction["name"] = inAction["name"].as<const char*>();
          Serial.printf("    -> Action name: %s\n", inAction["name"].as<const char*>());
        }

        // Normalize states into ["on"|"off"|"toggle"|"ignore", ...]
        JsonArray outStates = outAction.createNestedArray("states");
        if (inAction.containsKey("states")) {
          Serial.printf("    -> Action contains 'states' field\n");
          JsonVariant src = inAction["states"];
          if (src.is<JsonArray>()) {
            JsonArray arr = src.as<JsonArray>();
            Serial.printf("    -> States is JsonArray with %d elements\n", arr.size());
            if (arr.size() == 1 && arr[0].is<const char*>()) {
              const char* s = arr[0].as<const char*>();
              Serial.printf("    -> Single string element: '%s'\n", s ? s : "null");
              if (s && strchr(s, ',') != nullptr) {
                Serial.printf("    -> Splitting comma-separated string\n");
                String s2 = String(s);
                int start = 0;
                while (true) {
                  int comma = s2.indexOf(',', start);
                  String part = (comma == -1) ? s2.substring(start) : s2.substring(start, comma);
                  part.trim();
                  if (part.length() > 0) {
                    outStates.add(part);
                    Serial.printf("      -> Added state: '%s'\n", part.c_str());
                  }
                  if (comma == -1) break;
                  start = comma + 1;
                }
              } else {
                outStates.add(s ? String(s) : "");
                Serial.printf("      -> Added single state: '%s'\n", s ? s : "");
              }
            } else {
              Serial.printf("    -> Processing array elements individually\n");
              for (JsonVariant v : arr) {
                if (v.is<const char*>()) {
                  outStates.add(String(v.as<const char*>()));
                  Serial.printf("      -> Added state: '%s'\n", v.as<const char*>());
                } else if (v.is<String>()) {
                  outStates.add(v.as<String>());
                  Serial.printf("      -> Added state: '%s'\n", v.as<String>().c_str());
                }
              }
            }
          } else if (src.is<const char*>()) {
            const char* s = src.as<const char*>();
            Serial.printf("    -> States is string: '%s'\n", s ? s : "null");
            if (s) {
              String s2 = String(s);
              int start = 0;
              while (true) {
                int comma = s2.indexOf(',', start);
                String part = (comma == -1) ? s2.substring(start) : s2.substring(start, comma);
                part.trim();
                if (part.length() > 0) {
                  outStates.add(part);
                  Serial.printf("      -> Added state: '%s'\n", part.c_str());
                }
                if (comma == -1) break;
                start = comma + 1;
              }
            }
          } else {
             Serial.printf("    -> States field has unexpected type\n");
           }
        } else {
          Serial.printf("    -> Action does NOT contain 'states' field\n");
        }
        
        // 输出处理后的action结构
        String outActionStr;
        serializeJson(outAction, outActionStr);
        Serial.printf("    -> Final action: %s\n", outActionStr.c_str());
      }
    } else {
      Serial.printf("  -> Input payload is null or empty\n");
    }

    String commandString;
    serializeJson(finalMeshCommand, commandString);
    mesh.sendBroadcast(commandString);
    Serial.printf("  -> Broadcasted Action Command: %s\n", commandString.c_str());
    return;
  }
  
  if (topic_str.startsWith("PainlessMesh/Command/") || topic_str.startsWith("PainlessMesh/Config/")) {
    String deviceName, deviceType;
    bool is_config = topic_str.startsWith("PainlessMesh/Config/");
    topic_str.replace(is_config ? "PainlessMesh/Config/" : "PainlessMesh/Command/", "");
    int firstSlash = topic_str.indexOf('/'); 
    if (firstSlash == -1) return;
    deviceType = topic_str.substring(0, firstSlash);
    deviceName = topic_str.substring(firstSlash + 1);

    if (nameToMacMap.count(deviceName)) {
      String macAddress = nameToMacMap[deviceName];
      if (knownDevicesByMac.count(macAddress)) {
        MeshDevice& device = knownDevicesByMac[macAddress];
        if (!device.isOnline) { 
          Serial.printf("Command for offline device '%s' ignored.\n", deviceName.c_str()); 
          return; 
        }
        uint32_t targetNodeId = device.nodeId;
        String meshCommandType;

        if (is_config) { 
          meshCommandType = "configLed";
        } else if (deviceType == "ServoController") {
          StaticJsonDocument<256> ha_payload_doc; 
          deserializeJson(ha_payload_doc, payload, length);
          
          // Check if this is an angle control command from HA number entity
          if (ha_payload_doc.containsKey("angle") && !ha_payload_doc.containsKey("action") && !ha_payload_doc.containsKey("cmd")) {
            // This is a direct angle control from HA number entity
            meshCommandType = "setAngle";
          } else if (ha_payload_doc.containsKey("action") || ha_payload_doc.containsKey("cmd")) {
            // This is an action command (button press or stored action)
            meshCommandType = "runAction";
          } else {
            // Default to setAngle for backward compatibility
            meshCommandType = "setAngle";
          }
        } else { 
          meshCommandType = "setState"; 
        }

        StaticJsonDocument<512> mesh_cmd_doc; 
        mesh_cmd_doc["to"] = targetNodeId; 
        mesh_cmd_doc["cmd"] = meshCommandType;
        
        if (meshCommandType == "runAction" && deviceType == "ServoController") {
          // Parse the HA button payload and reconstruct for servo
          StaticJsonDocument<512> ha_payload_doc; 
          deserializeJson(ha_payload_doc, payload, length);
          
          // Extract the nested payload from HA button command
          JsonObject servo_payload = mesh_cmd_doc.createNestedObject("payload");
          
          if (ha_payload_doc.containsKey("payload")) {
            JsonObject ha_inner_payload = ha_payload_doc["payload"];
            
            // Copy operation type (action/save)
            if (ha_inner_payload.containsKey("type")) {
              servo_payload["type"] = ha_inner_payload["type"];
            }
            
            // Copy action name
            if (ha_inner_payload.containsKey("actionName")) {
              servo_payload["actionName"] = ha_inner_payload["actionName"];
            }
            
            // Map action_type to action_type (servo expects action_type field)
            if (ha_inner_payload.containsKey("action_type")) {
              servo_payload["action_type"] = ha_inner_payload["action_type"];
            }
            
            // Copy params object
            if (ha_inner_payload.containsKey("params")) {
              servo_payload["params"] = ha_inner_payload["params"];
            }
          } else {
            // Fallback: copy the entire payload as-is
            StaticJsonDocument<256> payload_doc;
            deserializeJson(payload_doc, payload, length);
            servo_payload = payload_doc.as<JsonObject>();
          }
        } else {
          // For other commands, use original logic
          StaticJsonDocument<256> payload_doc;
          deserializeJson(payload_doc, payload, length);
          mesh_cmd_doc["payload"] = payload_doc.as<JsonObject>();
        }
        
        String mesh_cmd_str; 
        serializeJson(mesh_cmd_doc, mesh_cmd_str);
        mesh.sendSingle(targetNodeId, mesh_cmd_str);
        Serial.printf("  -> Forwarded command to '%s' (NodeID: %u, Cmd: %s)\n", 
                      deviceName.c_str(), targetNodeId, meshCommandType.c_str());
        Serial.printf("  -> Command payload: %s\n", mesh_cmd_str.c_str());
      }
    }
  }
}

// =================================================================================
// Core Logic & Callbacks
// =================================================================================
void onMeshReceived(uint32_t from, String &msg) {
  Serial.printf("Received from %u: %s\n", from, msg.c_str());
  processMeshMessage(msg);
}

void processMeshMessage(String &message) {
  if (!mqttClient.connected()) return;
  StaticJsonDocument<512> doc; 
  if (deserializeJson(doc, message)) { return; }
  
  // 处理配置响应消息
  if (doc.containsKey("type") && doc["type"].as<String>() == "configResponse") {
    Serial.println("Received servo configuration response:");
    Serial.println(message);
    return; // 配置响应不需要进一步处理
  }
  
  // 处理舵机状态消息并存储到NVS
  if (doc.containsKey("type") && doc["type"].as<String>() == "ServoController" && 
      doc.containsKey("saved_actions")) {
    bool configChanged = saveServoInfoToNVS(message);
    // 只有当配置发生变化时才发布自动发现消息
    if (configChanged) {
      String deviceName = doc["name"].as<String>();
      int channels = doc.containsKey("channels") ? doc["channels"].as<int>() : 0;
      publishDiscoveryForDevice("ServoController", deviceName.c_str(), channels, message);
      Serial.printf("[DISCOVERY] Published discovery for servo '%s' due to config change\n", deviceName.c_str());
    }
  }
  
  if (!doc.containsKey("mac") || !doc.containsKey("name") || !doc.containsKey("nodeId")) { return; }
  String macAddress = doc["mac"].as<String>();
  uint32_t newNodeId = doc["nodeId"];
  String deviceName = doc["name"].as<String>();
  MeshDevice* device = nullptr; 
  bool isNewDevice = false; 
  bool needsSave = false;

  if (knownDevicesByMac.count(macAddress)) { 
    device = &knownDevicesByMac[macAddress];
  } else {
    isNewDevice = true; 
    needsSave = true; 
    MeshDevice newDevice;
    newDevice.macAddress = macAddress; 
    newDevice.nodeId = newNodeId; 
    newDevice.name = deviceName;
    newDevice.type = doc.containsKey("type") ? doc["type"].as<const char*>() : "Unknown";
    newDevice.hasBeenDiscovered = false;
    knownDevicesByMac[macAddress] = newDevice; 
    device = &knownDevicesByMac[macAddress];
    Serial.printf("New device discovered by MAC: %s\n", macAddress.c_str());
  }

  if (device->nodeId != newNodeId) {
    if (device->nodeId != 0 && nodeIdToMacMap.count(device->nodeId)) { 
      nodeIdToMacMap.erase(device->nodeId); 
    }
    device->nodeId = newNodeId; 
    needsSave = true;
  }

  bool wasOffline = !device->isOnline; 
  device->lastSeenTimestamp = millis(); 
  device->isOnline = true;
  nodeIdToMacMap[newNodeId] = macAddress; 
  nameToMacMap[deviceName] = macAddress;
  
  // Send WebSocket update if device came online
  if (wasOffline) {
    sendDeviceStatusUpdate(macAddress, "online");
  }

  // 只有非心跳包的状态上报消息才触发自动发现
  if (!device->hasBeenDiscovered && !doc.containsKey("heartbeat")) {
    Serial.printf("[DISCOVERY] Device '%s' needs discovery. Running...\n", device->name.c_str());
    int channels = doc.containsKey("channels") ? doc["channels"].as<int>() : 0;
    publishDiscoveryForDevice(device->type.c_str(), device->name.c_str(), channels, message);
    device->hasBeenDiscovered = true; 
    needsSave = true;
    
    // Send WebSocket update for discovery status change
    sendDeviceStatusUpdate(macAddress, "discovered");
    
    // Print all device discovery status after status change
    printAllDeviceDiscoveryStatus();
  }

  if (needsSave || wasOffline) { 
    saveToAddressBook(*device); 
  }

  if (wasOffline || isNewDevice) {
    char availability_topic[128];
    snprintf(availability_topic, sizeof(availability_topic), "PainlessMesh/Status/%s/availability", device->name.c_str());
    mqttClient.publish(availability_topic, PAYLOAD_AVAILABLE, true);
    Serial.printf("[STATUS] Device '%s' is now online.\n", device->name.c_str());
  }
  
  if (!doc.containsKey("heartbeat")) {
    // 检查是否包含LED配置信息（针对所有支持LED的开关设备）
    if ((device->type == "MultiSwitch" || device->type == "Switch" || device->type.indexOf("Switch") != -1) && 
        doc.containsKey("payload") && doc["payload"].containsKey("ledConfig")) {
      JsonObject ledConfig = doc["payload"]["ledConfig"];
      
      // *** 新增：将LED配置信息同步更新到设备缓存中 ***
      bool configUpdated = false;
      
      // 更新ON颜色
      if (ledConfig.containsKey("onColor")) {
        JsonObject onColor = ledConfig["onColor"];
        if (onColor.containsKey("r") && onColor.containsKey("g") && onColor.containsKey("b")) {
          uint8_t new_r = onColor["r"].as<uint8_t>();
          uint8_t new_g = onColor["g"].as<uint8_t>();
          uint8_t new_b = onColor["b"].as<uint8_t>();
          if (device->on_r != new_r || device->on_g != new_g || device->on_b != new_b) {
            device->on_r = new_r;
            device->on_g = new_g;
            device->on_b = new_b;
            configUpdated = true;
          }
        }
      }
      
      // 更新OFF颜色
      if (ledConfig.containsKey("offColor")) {
        JsonObject offColor = ledConfig["offColor"];
        if (offColor.containsKey("r") && offColor.containsKey("g") && offColor.containsKey("b")) {
          uint8_t new_r = offColor["r"].as<uint8_t>();
          uint8_t new_g = offColor["g"].as<uint8_t>();
          uint8_t new_b = offColor["b"].as<uint8_t>();
          if (device->off_r != new_r || device->off_g != new_g || device->off_b != new_b) {
            device->off_r = new_r;
            device->off_g = new_g;
            device->off_b = new_b;
            configUpdated = true;
          }
        }
      }
      
      // 更新亮度设置
      if (ledConfig.containsKey("brightnessOn") || ledConfig.containsKey("onBrightness")) {
        uint8_t newBrightnessOn = ledConfig.containsKey("brightnessOn") ? 
                                  ledConfig["brightnessOn"].as<uint8_t>() : 
                                  ledConfig["onBrightness"].as<uint8_t>();
        if (device->brightness_on != newBrightnessOn) {
          device->brightness_on = newBrightnessOn;
          device->default_on_value = newBrightnessOn;
          configUpdated = true;
        }
      }
      
      if (ledConfig.containsKey("brightnessOff") || ledConfig.containsKey("offBrightness")) {
        uint8_t newBrightnessOff = ledConfig.containsKey("brightnessOff") ? 
                                   ledConfig["brightnessOff"].as<uint8_t>() : 
                                   ledConfig["offBrightness"].as<uint8_t>();
        if (device->brightness_off != newBrightnessOff) {
          device->brightness_off = newBrightnessOff;
          device->default_off_value = newBrightnessOff;
          configUpdated = true;
        }
      }
      
      // 更新关联传感器
      if (ledConfig.containsKey("linkedSensor")) {
        String newLinkedSensor = ledConfig["linkedSensor"].as<String>();
        if (device->linkedEnvSensorName != newLinkedSensor) {
          device->linkedEnvSensorName = newLinkedSensor;
          configUpdated = true;
        }
      }
      
      // 更新日光阈值
      if (ledConfig.containsKey("daylightThreshold")) {
        uint32_t newDaylightThreshold = ledConfig["daylightThreshold"].as<uint32_t>();
        if (device->daylightThresholdLx != newDaylightThreshold) {
          device->daylightThresholdLx = newDaylightThreshold;
          configUpdated = true;
        }
      }
      
      // 更新白天/夜晚状态
      if (ledConfig.containsKey("isDaytime")) {
        bool newIsDaytime = ledConfig["isDaytime"].as<bool>();
        if (device->isDaytime != newIsDaytime) {
          device->isDaytime = newIsDaytime;
          configUpdated = true;
        }
      }
      
      // 如果配置有更新，持久化到NVS
      if (configUpdated) {
        saveToAddressBook(*device);
        Serial.printf("[LED_CACHE] Updated and saved LED config cache for device '%s'\n", deviceName.c_str());
      }
      
      // 发送包含LED配置信息的WebSocket更新
      sendDeviceStatusUpdateWithLedConfig(macAddress, "led_status_update", ledConfig);
      Serial.printf("[LED_STATUS] Sent LED config update for device '%s'\n", deviceName.c_str());
    }
    
    doc.remove("mac"); 
    String modifiedJsonString; 
    serializeJson(doc, modifiedJsonString);
    String topic = "PainlessMesh/Status/" + String(device->type) + "/" + String(deviceName);
    mqttClient.publish(topic.c_str(), modifiedJsonString.c_str());
  }
}

void onMeshNewConnection(uint32_t nodeId) { 
  Serial.printf("New connection: %u\n", nodeId); 
}

void onMeshDroppedConnection(uint32_t nodeId) {
  Serial.printf("Dropped connection: %u\n", nodeId);
  if (nodeIdToMacMap.count(nodeId)) {
    String mac = nodeIdToMacMap[nodeId];
    if (knownDevicesByMac.count(mac)) {
      MeshDevice& device = knownDevicesByMac[mac];
      device.isOnline = false;
      char availability_topic[128];
      snprintf(availability_topic, sizeof(availability_topic), "PainlessMesh/Status/%s/availability", device.name.c_str());
      mqttClient.publish(availability_topic, PAYLOAD_NOT_AVAILABLE, true);
      Serial.printf("[STATUS] Device '%s' marked offline due to dropped connection.\n", device.name.c_str());
      
      // Send WebSocket update
      sendDeviceStatusUpdate(mac, "offline");
    }
  }
}

void checkDeviceStatus() {
  unsigned long now = millis();
  for (auto& pair : knownDevicesByMac) {
    MeshDevice& device = pair.second;
    if (device.isOnline && (now - device.lastSeenTimestamp) > DEVICE_OFFLINE_TIMEOUT) {
      Serial.printf("[STATUS] Device '%s' has gone offline (timeout).\n", device.name.c_str());
      device.isOnline = false;
      char availability_topic[128];
      snprintf(availability_topic, sizeof(availability_topic), "PainlessMesh/Status/%s/availability", device.name.c_str());
      mqttClient.publish(availability_topic, PAYLOAD_NOT_AVAILABLE, true);
      saveToAddressBook(device);
      
      // Send WebSocket update
      sendDeviceStatusUpdate(pair.first, "offline");
    }
  }
}

// =================================================================================
// Web Server (API) Handlers
// =================================================================================
void addCorsHeaders(AsyncWebServerRequest *request) {
  // 调试信息：记录HTTP请求
  Serial.println("\n=== HTTP Request Debug ===");
  Serial.printf("URL: %s\n", request->url().c_str());
  Serial.printf("Method Value: %d\n", request->method());
  Serial.printf("HTTP_OPTIONS Value: %d\n", HTTP_OPTIONS);
  String methodStr = "OTHER";
  switch(request->method()) {
    case 1: methodStr = "GET"; break;
    case 2: methodStr = "POST"; break;
    case 4: methodStr = "DELETE"; break;
    case 8: methodStr = "PUT"; break;
    case 16: methodStr = "PATCH"; break;
    case 32: methodStr = "HEAD"; break;
    case 64: methodStr = "OPTIONS"; break;
    default: methodStr = "OTHER(" + String(request->method()) + ")"; break;
  }
  Serial.printf("Method: %s\n", methodStr.c_str());
  Serial.printf("Client IP: %s\n", request->client()->remoteIP().toString().c_str());
  
  // 打印请求参数
  if (request->params() > 0) {
    Serial.printf("Request Parameters (%d):\n", request->params());
    for (int i = 0; i < request->params(); i++) {
      AsyncWebParameter* p = request->getParam(i);
      Serial.printf("  %s = %s\n", p->name().c_str(), p->value().c_str());
    }
  }
  
  // 打印请求头
  Serial.printf("Request Headers (%d):\n", request->headers());
  for (int i = 0; i < request->headers(); i++) {
    AsyncWebHeader* h = request->getHeader(i);
    Serial.printf("  %s: %s\n", h->name().c_str(), h->value().c_str());
  }
  Serial.println("=== End HTTP Request Debug ===\n");
  
  // 注意：这个函数只用于调试和记录，不应该发送响应
  // CORS头将在sendCorsResponse函数中添加
}

void sendCorsResponse(AsyncWebServerRequest *request, int code, const char* contentType, const String& content) {
    // 调试信息：记录HTTP响应
    Serial.println("\n=== HTTP Response Debug ===");
    Serial.printf("URL: %s\n", request->url().c_str());
    String methodStr = "OTHER";
    switch(request->method()) {
      case 1: methodStr = "GET"; break;
      case 2: methodStr = "POST"; break;
      case 4: methodStr = "DELETE"; break;
      case 8: methodStr = "PUT"; break;
      case 16: methodStr = "PATCH"; break;
      case 32: methodStr = "HEAD"; break;
      case 64: methodStr = "OPTIONS"; break;
      default: methodStr = "OTHER(" + String(request->method()) + ")"; break;
    }
    Serial.printf("Method: %s\n", methodStr.c_str());
    Serial.printf("Response Code: %d\n", code);
    Serial.printf("Content-Type: %s\n", contentType);
    Serial.printf("Content Length: %d bytes\n", content.length());
    
    // 如果内容不太长，打印完整内容；否则只打印前200个字符
    if (content.length() <= 500) {
        Serial.printf("Response Content: %s\n", content.c_str());
    } else {
        Serial.printf("Response Content (first 200 chars): %s...\n", content.substring(0, 200).c_str());
    }
    Serial.println("=== End HTTP Response Debug ===\n");
    
    AsyncWebServerResponse *response = request->beginResponse(code, contentType, content);
    response->addHeader("Access-Control-Allow-Origin", "*");
    response->addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    response->addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
    request->send(response);
}

void handleApiImportConfig(AsyncWebServerRequest *request) {
  addCorsHeaders(request);
  
  if (!request->hasParam("plain", true)) {
    sendCorsResponse(request, 400, "application/json", "{\"error\":\"No configuration data provided\"}");
    return;
  }
  
  String configData = request->getParam("plain", true)->value();
  DynamicJsonDocument doc(8192);
  DeserializationError error = deserializeJson(doc, configData);
  
  if (error) {
    sendCorsResponse(request, 400, "application/json", "{\"error\":\"Invalid JSON format\"}");
    return;
  }
  
  JsonArray devices = doc["devices"];
  if (!devices) {
    sendCorsResponse(request, 400, "application/json", "{\"error\":\"No devices array found\"}");
    return;
  }
  
  int importedCount = 0;
  int errorCount = 0;
  
  for (JsonObject deviceObj : devices) {
    String deviceName = deviceObj["name"];
    String deviceType = deviceObj["type"];
    
    if (!nameToMacMap.count(deviceName)) {
      errorCount++;
      continue;
    }
    
    MeshDevice& device = knownDevicesByMac[nameToMacMap[deviceName]];
    
    if (device.type != deviceType) {
      errorCount++;
      continue;
    }
    
    JsonObject config = deviceObj["config"];
    if (!config) {
      continue;
    }
    
    // 导入MultiSwitch配置
    if (deviceType == "MultiSwitch") {
      String onColorHex = config["on_color"];
      String offColorHex = config["off_color"];
      
      if (onColorHex.length() == 7 && onColorHex[0] == '#') {
        long on_number = strtol(&onColorHex.c_str()[1], NULL, 16);
        device.on_r = (on_number >> 16) & 0xFF;
        device.on_g = (on_number >> 8) & 0xFF;
        device.on_b = on_number & 0xFF;
      }
      
      if (offColorHex.length() == 7 && offColorHex[0] == '#') {
        long off_number = strtol(&offColorHex.c_str()[1], NULL, 16);
        device.off_r = (off_number >> 16) & 0xFF;
        device.off_g = (off_number >> 8) & 0xFF;
        device.off_b = off_number & 0xFF;
      }
      
      device.brightness_on = config["brightness_on"] | device.brightness_on;
      device.brightness_off = config["brightness_off"] | device.brightness_off;
      device.linkedEnvSensorName = config["linkedSensor"] | device.linkedEnvSensorName;
      device.daylightThresholdLx = config["daylightThreshold"] | device.daylightThresholdLx;
      device.default_on_value = device.brightness_on;
      device.default_off_value = device.brightness_off;
      
      // 发送配置到设备
      StaticJsonDocument<384> payloadDoc;
      payloadDoc["led_on_color"]["r"] = device.on_r;
      payloadDoc["led_on_color"]["g"] = device.on_g;
      payloadDoc["led_on_color"]["b"] = device.on_b;
      payloadDoc["led_off_color"]["r"] = device.off_r;
      payloadDoc["led_off_color"]["g"] = device.off_g;
      payloadDoc["led_off_color"]["b"] = device.off_b;
      payloadDoc["brightness_on"] = device.brightness_on;
      payloadDoc["brightness_off"] = device.brightness_off;
      payloadDoc["linked_env_sensor_name"] = device.linkedEnvSensorName;
      payloadDoc["daylight_threshold_lx"] = device.daylightThresholdLx;
      
      StaticJsonDocument<512> mesh_cmd_doc;
      mesh_cmd_doc["to"] = device.nodeId;
      mesh_cmd_doc["cmd"] = "configLed";
      mesh_cmd_doc["payload"] = payloadDoc.as<JsonObject>();
      String mesh_cmd_str;
      serializeJson(mesh_cmd_doc, mesh_cmd_str);
      mesh.sendSingle(device.nodeId, mesh_cmd_str);
    }
    // 导入ServoController配置
    else if (deviceType == "ServoController") {
      int sweepAngle = config["sweep_angle"] | 30;
      int actionDelay = config["action_delay"] | 500;
      
      device.default_on_value = sweepAngle;
      device.default_off_value = actionDelay / 10;
      
      StaticJsonDocument<256> payloadDoc;
      payloadDoc["sweep_angle"] = sweepAngle;
      payloadDoc["action_delay"] = actionDelay;
      
      StaticJsonDocument<384> mesh_cmd_doc;
      mesh_cmd_doc["to"] = device.nodeId;
      mesh_cmd_doc["cmd"] = "configServo";
      mesh_cmd_doc["payload"] = payloadDoc.as<JsonObject>();
      String mesh_cmd_str;
      serializeJson(mesh_cmd_doc, mesh_cmd_str);
      mesh.sendSingle(device.nodeId, mesh_cmd_str);
    }
    
    // 保存到NVS
    saveToAddressBook(device);
    importedCount++;
  }
  
  String result = "{\"status\":\"success\", \"imported\":" + String(importedCount) + ", \"errors\":" + String(errorCount) + "}";
  sendCorsResponse(request, 200, "application/json", result);
}

void handleApiStatus(AsyncWebServerRequest *request) {
  addCorsHeaders(request);
  DynamicJsonDocument doc(1024);
  unsigned long s = millis() / 1000;
  String uptimeStr = String(s / 86400) + "d " + String((s % 86400) / 3600) + "h " + String((s % 3600) / 60) + "m";
  doc["uptime"] = uptimeStr;
  doc["freeHeap"] = ESP.getFreeHeap();
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    doc["systemTime"] = "NTP not synced yet";
  } else {
    char time_buffer[20];
    strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
    doc["systemTime"] = String(time_buffer);
  }
  JsonObject net = doc.createNestedObject("network");
  net["type"] = "Ethernet";
  net["ipAddress"] = ETH.localIP().toString();
  net["macAddress"] = ETH.macAddress();
  JsonObject mqtt = doc.createNestedObject("mqtt");
  mqtt["connected"] = mqttClient.connected();
  mqtt["server"] = String(MQTT_SERVER);
  doc["deviceCount"] = knownDevicesByMac.size();
  
  // 添加NVS存储使用情况
  JsonObject nvs = doc.createNestedObject("nvs");
  size_t totalBytes = 0;
  size_t usedBytes = 0;
  size_t freeBytes = 0;
  
  // 获取NVS分区信息
  nvs_stats_t nvs_stats;
  esp_err_t err = nvs_get_stats(NULL, &nvs_stats);
  if (err == ESP_OK) {
    totalBytes = nvs_stats.total_entries;
    usedBytes = nvs_stats.used_entries;
    freeBytes = nvs_stats.free_entries;
    nvs["totalBytes"] = totalBytes;
    nvs["usedBytes"] = usedBytes;
    nvs["freeBytes"] = freeBytes;
    nvs["usagePercent"] = totalBytes > 0 ? (usedBytes * 100 / totalBytes) : 0;
  } else {
    nvs["error"] = "Failed to get NVS stats";
  }
  
  JsonObject wdt = doc.createNestedObject("watchdog");
  preferences.begin("logs", true);
  wdt["log1"] = preferences.getString("wdt_log_1", "No record");
  wdt["log2"] = preferences.getString("wdt_log_2", "No record");
  wdt["log3"] = preferences.getString("wdt_log_3", "No record");
  preferences.end();
  String response;
  serializeJson(doc, response);
  Serial.printf("Before sendCorsResponse (actions): Free Heap = %u\n", ESP.getFreeHeap());
  sendCorsResponse(request, 200, "application/json", response);
}

void handleApiSystemMonitor(AsyncWebServerRequest *request) {
  Serial.println("=== System Monitor API Called ===");
  
  // 创建JSON文档
  DynamicJsonDocument doc(4096);
  
  // 系统基本信息
  JsonObject system = doc.createNestedObject("system");
  unsigned long s = millis() / 1000;
  String uptimeStr = String(s / 86400) + "d " + String((s % 86400) / 3600) + "h " + String((s % 3600) / 60) + "m " + String(s % 60) + "s";
  system["uptime"] = uptimeStr;
  system["uptimeSeconds"] = s;
  system["chipModel"] = ESP.getChipModel();
  system["chipRevision"] = ESP.getChipRevision();
  system["cpuFreqMHz"] = ESP.getCpuFreqMHz();
  system["flashChipSize"] = ESP.getFlashChipSize();
  system["flashChipSpeed"] = ESP.getFlashChipSpeed();
  
  // 内存信息
  JsonObject memory = doc.createNestedObject("memory");
  memory["totalHeap"] = ESP.getHeapSize();
  memory["freeHeap"] = ESP.getFreeHeap();
  memory["usedHeap"] = ESP.getHeapSize() - ESP.getFreeHeap();
  memory["usagePercent"] = ((ESP.getHeapSize() - ESP.getFreeHeap()) * 100) / ESP.getHeapSize();
  memory["largestFreeBlock"] = ESP.getMaxAllocHeap();
  memory["minFreeHeap"] = ESP.getMinFreeHeap();
  
  // 网络信息
  JsonObject network = doc.createNestedObject("network");
  network["type"] = "Ethernet";
  network["ipAddress"] = ETH.localIP().toString();
  network["macAddress"] = ETH.macAddress();
  network["linkUp"] = ETH.linkUp();
  network["fullDuplex"] = ETH.fullDuplex();
  network["linkSpeed"] = ETH.linkSpeed();
  
  // MQTT信息
  JsonObject mqtt = doc.createNestedObject("mqtt");
  mqtt["connected"] = mqttClient.connected();
  mqtt["server"] = String(MQTT_SERVER);
  mqtt["port"] = String(MQTT_PORT_STR);
  
  // Mesh网络信息
  JsonObject mesh = doc.createNestedObject("mesh");
  mesh["connectedDevices"] = knownDevicesByMac.size();
  mesh["onlineDevices"] = 0;
  for (auto const& [mac, device] : knownDevicesByMac) {
    if (device.isOnline) {
      mesh["onlineDevices"] = mesh["onlineDevices"].as<int>() + 1;
    }
  }
  
  // NVS存储信息
  JsonObject nvs = doc.createNestedObject("nvs");
  nvs_stats_t nvs_stats;
  esp_err_t err = nvs_get_stats(NULL, &nvs_stats);
  if (err == ESP_OK) {
    nvs["totalEntries"] = nvs_stats.total_entries;
    nvs["usedEntries"] = nvs_stats.used_entries;
    nvs["freeEntries"] = nvs_stats.free_entries;
    nvs["usagePercent"] = nvs_stats.total_entries > 0 ? (nvs_stats.used_entries * 100 / nvs_stats.total_entries) : 0;
  } else {
    nvs["error"] = "Failed to get NVS stats";
  }
  
  // 系统运行时间（毫秒）
  JsonObject timeObj = doc.createNestedObject("time");
  timeObj["uptimeMs"] = millis();
  
  // 重置看门狗以防止超时
  esp_task_wdt_reset();
  
  // 看门狗日志
  JsonObject watchdog = doc.createNestedObject("watchdog");
  preferences.begin("logs", true);
  watchdog["log1"] = preferences.getString("wdt_log_1", "No record");
  watchdog["log2"] = preferences.getString("wdt_log_2", "No record");
  watchdog["log3"] = preferences.getString("wdt_log_3", "No record");
  preferences.end();
  
  // 再次重置看门狗
  esp_task_wdt_reset();
  
  // 序列化JSON
  String response;
  size_t jsonSize = serializeJson(doc, response);
  Serial.printf("System Monitor API: JSON size = %d, Free Heap = %u\n", jsonSize, ESP.getFreeHeap());
  
  // 最后一次重置看门狗
  esp_task_wdt_reset();
  
  // 直接发送响应
  AsyncWebServerResponse *webResponse = request->beginResponse(200, "application/json", response);
  webResponse->addHeader("Access-Control-Allow-Origin", "*");
  webResponse->addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
  webResponse->addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
  request->send(webResponse);
  
  Serial.println("=== System Monitor Response sent ===");
}

void handleApiDevices(AsyncWebServerRequest *request) {
  addCorsHeaders(request);

  if (knownDevicesByMac.empty()) {
    sendCorsResponse(request, 200, "application/json", "[]");
    return;
  }

  DynamicJsonDocument doc(4096);
  JsonArray devices = doc.to<JsonArray>();
  
  for (auto const& [mac, device] : knownDevicesByMac) {
    JsonObject devObj = devices.createNestedObject();
    devObj["name"] = device.name;
    devObj["macAddress"] = device.macAddress;
    devObj["type"] = device.type;
    devObj["nodeId"] = device.nodeId;
    devObj["online"] = device.isOnline;
    devObj["lastSeen"] = device.lastSeenTimestamp;
    
    // 根据设备类型添加特定配置
    if (device.type == "MultiSwitch") {
      JsonObject config = devObj.createNestedObject("config");
      
      char on_color_hex[8], off_color_hex[8];
      snprintf(on_color_hex, sizeof(on_color_hex), "#%02x%02x%02x", device.on_r, device.on_g, device.on_b);
      snprintf(off_color_hex, sizeof(off_color_hex), "#%02x%02x%02x", device.off_r, device.off_g, device.off_b);
      
      config["on_color"] = on_color_hex;
      config["brightness_on"] = device.brightness_on;
      config["off_color"] = off_color_hex;
      config["brightness_off"] = device.brightness_off;
      config["linkedEnvSensorName"] = device.linkedEnvSensorName;
      config["daylightThresholdLx"] = device.daylightThresholdLx;
      config["isDaytime"] = device.isDaytime;
    }
    else if (device.type == "ServoController") {
      JsonObject config = devObj.createNestedObject("config");
      
      int sweepAngle = device.default_on_value > 0 ? device.default_on_value : 30;
      int actionDelay = device.default_off_value > 0 ? device.default_off_value * 10 : 500;
      
      config["sweep_angle"] = sweepAngle;
      config["action_delay"] = actionDelay;
    }
  }
  
  String response;
  serializeJson(doc, response);
  Serial.printf("Before sendCorsResponse (devices): Free Heap = %u\n", ESP.getFreeHeap());
  sendCorsResponse(request, 200, "application/json", response);
}

void handleApiActions(AsyncWebServerRequest *request) {
    Serial.println("\n=== 收到 Actions 页面请求 ===");
    Serial.print("请求URL: ");
    Serial.println(request->url());
    Serial.print("请求方法: ");
    Serial.println(request->method() == HTTP_GET ? "GET" : "POST");
    
    // 从NVS获取action_list
    preferences.begin("actions", true);
    String action_list = preferences.getString("action_list", "[]");
    Serial.println("从NVS获取的action_list: " + action_list);
    Serial.printf("action_list长度: %d\n", action_list.length());
    
    // 调试：列出NVS中所有的键
    Serial.println("=== NVS调试信息 ===");
    
    // 尝试读取一些可能的键名
    String possibleKeys[] = {"1", "2", "BedroomSocket", "action_1", "action_2", "test", "socket1"};
    for (String key : possibleKeys) {
        String value = preferences.getString(key.c_str(), "");
        if (value.length() > 0) {
            Serial.println("找到键: " + key + " = " + value);
        }
    }
    preferences.end();
    Serial.println("=== NVS调试结束 ===");
    
    preferences.begin("actions", true);
    
    // 解析action_list JSON
    DynamicJsonDocument actionListDoc(2048);
    DeserializationError error = deserializeJson(actionListDoc, action_list);
    if (error) {
        Serial.println("解析action_list失败: " + String(error.c_str()));
    }
    
    // 创建响应JSON数组
    DynamicJsonDocument responseDoc(4096);
    JsonArray actionsArray = responseDoc.to<JsonArray>();
    
    // 如果action_list为空，尝试直接查找已知的动作名称
    if (actionListDoc.as<JsonArray>().size() == 0) {
        Serial.println("action_list为空，尝试直接读取已知动作");
        // 尝试读取一些常见的动作名称
        String testNames[] = {"1", "BedroomSocket", "action_1"};
        for (String testName : testNames) {
            String testData = preferences.getString(testName.c_str(), "");
            if (testData.length() > 0 && testData != "not_found") {
                Serial.println("找到动作数据: " + testName + " = " + testData);
                DynamicJsonDocument actionDoc(1024);
                if (deserializeJson(actionDoc, testData) == DeserializationError::Ok) {
                    actionsArray.add(actionDoc.as<JsonObject>());
                    Serial.println("成功添加动作到响应: " + testName);
                }
            }
        }
    } else {
        // 遍历每个动作名称 - 修复：直接使用actionName作为键，不添加"action_"前缀
        Serial.println("遍历action_list中的动作");
        Serial.printf("action_list数组大小: %d\n", actionListDoc.as<JsonArray>().size());
        for (JsonVariant actionName : actionListDoc.as<JsonArray>()) {
            String actionKey = actionName.as<String>(); // 修复：直接使用actionName
            Serial.printf("正在读取动作: '%s'\n", actionKey.c_str());
            String actionData = preferences.getString(actionKey.c_str(), "{}");
            Serial.println("读取 " + actionKey + ": " + actionData);
            
            // 解析单个动作数据
            DynamicJsonDocument actionDoc(1024);
            if (deserializeJson(actionDoc, actionData) == DeserializationError::Ok) {
                actionsArray.add(actionDoc.as<JsonObject>());
                Serial.printf("成功添加动作 '%s' 到响应数组\n", actionKey.c_str());
            } else {
                Serial.printf("解析动作 '%s' 数据失败\n", actionKey.c_str());
            }
        }
    }
    
    // 序列化为JSON字符串
    String jsonResponse;
    serializeJson(responseDoc, jsonResponse);
    
    Serial.println("发送响应: " + jsonResponse);
    Serial.printf("响应长度: %d 字节\n", jsonResponse.length());
    Serial.printf("响应数组大小: %d\n", actionsArray.size());
    
    // 关闭NVS命名空间
    preferences.end();
    
    // 直接使用sendCorsResponse函数发送响应（不调用addCorsHeaders）
    sendCorsResponse(request, 200, "application/json", jsonResponse);
}



void handleApiGetMqtt(AsyncWebServerRequest *request) {
  addCorsHeaders(request);
  DynamicJsonDocument doc(512);
  doc["mqtt_server"] = String(MQTT_SERVER);
  doc["mqtt_port"] = String(MQTT_PORT_STR);
  doc["mqtt_user"] = String(MQTT_USER);
  doc["mqtt_pass"] = String(MQTT_PASS);
  doc["mqtt_client_id"] = String(MQTT_CLIENT_ID);
  doc["connected"] = mqttClient.connected();
  String response;
  serializeJson(doc, response);
  Serial.printf("Before sendCorsResponse (status): Free Heap = %u\n", ESP.getFreeHeap());
  sendCorsResponse(request, 200, "application/json", response);
}

void handleApiTestMqtt(AsyncWebServerRequest *request) {
  addCorsHeaders(request);
  
  if (request->method() == HTTP_OPTIONS) {
    sendCorsResponse(request, 200, "text/plain", "");
    return;
  }
  
  DynamicJsonDocument doc(512);
  
  // Test MQTT connection with current settings
  bool testResult = false;
  String testMessage = "";
  
  if (strlen(MQTT_SERVER) == 0) {
    testMessage = "MQTT server not configured";
  } else {
    // Disconnect current MQTT client if connected
    if (mqttClient.connected()) {
      mqttClient.disconnect();
    }
    
    // Try to connect with current settings
     mqttClient.setServer(MQTT_SERVER, atoi(MQTT_PORT_STR));
     
     unsigned long startTime = millis();
     const unsigned long timeout = 10000; // 10 seconds timeout
     
     if (strlen(MQTT_USER) > 0 && strlen(MQTT_PASS) > 0) {
       testResult = mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS);
     } else {
       testResult = mqttClient.connect(MQTT_CLIENT_ID);
     }
    
    if (testResult) {
      testMessage = "MQTT connection test successful";
      // Disconnect after test
      mqttClient.disconnect();
      // Reconnect with normal flow
      tryMqttReconnect();
    } else {
      testMessage = "MQTT connection test failed. Check server settings.";
      // Try to reconnect with normal flow anyway
      tryMqttReconnect();
    }
  }
  
  doc["success"] = testResult;
  doc["message"] = testMessage;
  
  String response;
  serializeJson(doc, response);
  sendCorsResponse(request, 200, "application/json", response);
}

void handleApiSaveMqtt(AsyncWebServerRequest *request) {
  addCorsHeaders(request);
  strncpy(MQTT_SERVER, request->getParam("mqtt_server", true)->value().c_str(), sizeof(MQTT_SERVER) - 1);
  strncpy(MQTT_PORT_STR, request->getParam("mqtt_port", true)->value().c_str(), sizeof(MQTT_PORT_STR) - 1);
  strncpy(MQTT_USER, request->getParam("mqtt_user", true)->value().c_str(), sizeof(MQTT_USER) - 1);
  strncpy(MQTT_PASS, request->getParam("mqtt_pass", true)->value().c_str(), sizeof(MQTT_PASS) - 1);
  saveCredentials();
  sendCorsResponse(request, 200, "application/json", "{\"status\":\"success\", \"message\":\"MQTT settings saved. Reconnecting...\"}");
  delay(100);
  if (mqttClient.connected()) { mqttClient.disconnect(); }
  mqttClient.setServer(MQTT_SERVER, atoi(MQTT_PORT_STR));
  taskReconnectMQTT.enable();
}

void handleApiReboot(AsyncWebServerRequest *request) {
  addCorsHeaders(request);
  sendCorsResponse(request, 200, "application/json", "{\"status\":\"rebooting\"}");
  delay(100);
  ESP.restart();
}

void handleApiClearNvs(AsyncWebServerRequest *request) {
  addCorsHeaders(request);
  clearAllNvs();
  sendCorsResponse(request, 200, "application/json", "{\"status\":\"success\", \"message\":\"NVS cleared. Rebooting...\"}");
  delay(200);
  ESP.restart();
}

void handleApiNetworkConfig(AsyncWebServerRequest *request) {
  Serial.println("=== handleApiNetworkConfig 开始 ===");
  Serial.print("请求方法: ");
  Serial.println(request->method() == HTTP_GET ? "GET" : 
                 request->method() == HTTP_POST ? "POST" : 
                 request->method() == 2 ? "POST(2)" : "OTHER");
  
  addCorsHeaders(request);
  
  if (request->method() == HTTP_GET) {
    Serial.println("处理GET请求 - 返回当前网络配置");
    // 返回当前网络配置
    DynamicJsonDocument doc(1024);
    
    // 以太网配置
    doc["eth_connected"] = ETH.linkUp();
    doc["eth_ip"] = ETH.localIP().toString();
    doc["eth_gateway"] = ETH.gatewayIP().toString();
    doc["eth_subnet"] = ETH.subnetMask().toString();
    doc["eth_dns"] = ETH.dnsIP().toString();
    doc["eth_mac"] = ETH.macAddress();
    
    // MQTT配置
    doc["mqtt_server"] = String(MQTT_SERVER);
    doc["mqtt_port"] = String(MQTT_PORT_STR);
    doc["mqtt_user"] = String(MQTT_USER);
    doc["mqtt_pass"] = String(MQTT_PASS);
    doc["mqtt_client_id"] = String(MQTT_CLIENT_ID);
    doc["mqtt_connected"] = mqttClient.connected();
    
    // 网关地址配置 - 使用动态获取的IP地址
    doc["gateway_address"] = ETH.localIP().toString();
    
    String response;
    serializeJson(doc, response);
    Serial.println("发送GET响应: " + response);
    sendCorsResponse(request, 200, "application/json", response);
  }
  Serial.println("=== handleApiNetworkConfig 结束 ===");
}

void handleApiNetworkConfigPost(AsyncWebServerRequest *request) {
  Serial.println("=== handleApiNetworkConfigPost 开始 ===");
  Serial.println("处理POST请求 - 保存MQTT配置");
  
  addCorsHeaders(request);
  
  // 检查是否接收到JSON数据
  if (!networkConfigDataReceived || networkConfigJsonData.isEmpty()) {
    Serial.println("错误: 没有接收到JSON数据");
    sendCorsResponse(request, 400, "application/json", "{\"error\":\"No JSON data received\"}");
    return;
  }
  
  Serial.println("收到的JSON数据: " + networkConfigJsonData);
  
  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, networkConfigJsonData);
  
  if (error) {
    Serial.println("错误: JSON格式无效 - " + String(error.c_str()));
    sendCorsResponse(request, 400, "application/json", "{\"error\":\"Invalid JSON format\"}");
    // 清理数据
    networkConfigJsonData = "";
    networkConfigDataReceived = false;
    return;
  }
  
  bool configChanged = false;
  
  // 只处理MQTT配置 - 网关地址由前端本地管理
  if (doc.containsKey("mqttServer")) {
    String newServer = doc["mqttServer"].as<String>();
    Serial.println("更新MQTT服务器: " + newServer);
    strncpy(MQTT_SERVER, newServer.c_str(), sizeof(MQTT_SERVER) - 1);
    MQTT_SERVER[sizeof(MQTT_SERVER) - 1] = '\0';
    configChanged = true;
  }
  if (doc.containsKey("mqttPort")) {
    String portStr = String(doc["mqttPort"].as<int>());
    Serial.println("更新MQTT端口: " + portStr);
    strncpy(MQTT_PORT_STR, portStr.c_str(), sizeof(MQTT_PORT_STR) - 1);
    MQTT_PORT_STR[sizeof(MQTT_PORT_STR) - 1] = '\0';
    configChanged = true;
  }
  if (doc.containsKey("mqttUsername")) {
    String newUser = doc["mqttUsername"].as<String>();
    Serial.println("更新MQTT用户名: " + newUser);
    strncpy(MQTT_USER, newUser.c_str(), sizeof(MQTT_USER) - 1);
    MQTT_USER[sizeof(MQTT_USER) - 1] = '\0';
    configChanged = true;
  }
  if (doc.containsKey("mqttPassword")) {
    String newPass = doc["mqttPassword"].as<String>();
    Serial.println("更新MQTT密码: [已隐藏]");
    strncpy(MQTT_PASS, newPass.c_str(), sizeof(MQTT_PASS) - 1);
    MQTT_PASS[sizeof(MQTT_PASS) - 1] = '\0';
    configChanged = true;
  }
  
  // 注意：不再处理gatewayAddress，因为网关地址由前端本地管理
  if (doc.containsKey("gatewayAddress")) {
    Serial.println("忽略网关地址配置 - 由前端本地管理");
  }
  
  if (configChanged) {
    Serial.println("MQTT配置已更改，保存并重新连接MQTT");
    saveCredentials();
    
    sendCorsResponse(request, 200, "application/json", "{\"status\":\"success\", \"message\":\"MQTT configuration saved. Reconnecting MQTT...\"}");
    
    // 重新连接MQTT
    delay(100);
    if (mqttClient.connected()) { 
      mqttClient.disconnect(); 
    }
    mqttClient.setServer(MQTT_SERVER, atoi(MQTT_PORT_STR));
    taskReconnectMQTT.enable();
  } else {
    Serial.println("没有MQTT配置参数被提供");
    sendCorsResponse(request, 400, "application/json", "{\"error\":\"No MQTT configuration parameters provided\"}");
  }
  
  // 清理全局数据
  networkConfigJsonData = "";
  networkConfigDataReceived = false;
  
  Serial.println("=== handleApiNetworkConfigPost 结束 ===");
}

// --- 请用这个完整的函数替换您 gateway-eth.ino 中的旧版本 ---

void handleApiSaveDeviceConfig(AsyncWebServerRequest *request) {
  addCorsHeaders(request);
  
  // 1. 从 Web 请求中获取所有数据
  String deviceName = request->getParam("deviceName", true)->value();
  if (!nameToMacMap.count(deviceName)) { 
    sendCorsResponse(request, 404, "application/json", "{\"error\":\"Device not found\"}"); 
    return; 
  }
  MeshDevice& device = knownDevicesByMac[nameToMacMap[deviceName]];

  // 处理MultiSwitch设备配置
  if (device.type == "MultiSwitch") {
    String onColorHex = request->getParam("on_color", true)->value(); 
    long on_number = strtol(&onColorHex.c_str()[1], NULL, 16);
    device.on_r = (on_number >> 16) & 0xFF; 
    device.on_g = (on_number >> 8) & 0xFF; 
    device.on_b = on_number & 0xFF;
    
    String offColorHex = request->getParam("off_color", true)->value(); 
    long off_number = strtol(&offColorHex.c_str()[1], NULL, 16);
    device.off_r = (off_number >> 16) & 0xFF; 
    device.off_g = (off_number >> 8) & 0xFF; 
    device.off_b = off_number & 0xFF;
    
    device.brightness_on = request->getParam("brightness_on", true)->value().toInt();
    device.brightness_off = request->getParam("brightness_off", true)->value().toInt();
    device.linkedEnvSensorName = request->getParam("linked_sensor_name", true)->value();
    device.daylightThresholdLx = request->getParam("daylight_threshold", true)->value().toInt();
    device.default_on_value = device.brightness_on;
    device.default_off_value = device.brightness_off;
    
    // 2. 将完整的配置保存到网关自己的 NVS
    saveToAddressBook(device);

    // *** 3. THIS IS THE FIX: 构建包含所有字段的完整 payload ***
    StaticJsonDocument<384> payloadDoc;
    payloadDoc["led_on_color"]["r"] = device.on_r;
    payloadDoc["led_on_color"]["g"] = device.on_g;
    payloadDoc["led_on_color"]["b"] = device.on_b;
    payloadDoc["led_off_color"]["r"] = device.off_r;
    payloadDoc["led_off_color"]["g"] = device.off_g;
    payloadDoc["led_off_color"]["b"] = device.off_b;
    payloadDoc["brightness_on"] = device.brightness_on;
    payloadDoc["brightness_off"] = device.brightness_off;
    payloadDoc["linked_env_sensor_name"] = device.linkedEnvSensorName;
    payloadDoc["daylight_threshold_lx"] = device.daylightThresholdLx;

    // 4. 将完整的命令发送给开关设备
    StaticJsonDocument<512> mesh_cmd_doc;
    mesh_cmd_doc["to"] = device.nodeId; 
    mesh_cmd_doc["cmd"] = "configLed";
    mesh_cmd_doc["payload"] = payloadDoc.as<JsonObject>();
    String mesh_cmd_str; 
    serializeJson(mesh_cmd_doc, mesh_cmd_str);
    mesh.sendSingle(device.nodeId, mesh_cmd_str);

    Serial.printf("Sent FULL config command to '%s': %s\n", deviceName.c_str(), mesh_cmd_str.c_str());
  }
  // 处理ServoController设备配置
  else if (device.type == "ServoController") {
    // 获取servo参数
    int sweepAngle = request->getParam("sweep_angle", true)->value().toInt();
    int actionDelay = request->getParam("action_delay", true)->value().toInt();
    
    // 设置默认值
    if (sweepAngle == 0) sweepAngle = DEFAULT_SWEEP_ANGLE;
    if (actionDelay == 0) actionDelay = DEFAULT_ACTION_DELAY;
    
    // 保存到设备配置中（使用现有字段存储）
    device.default_on_value = sweepAngle;
    device.default_off_value = actionDelay / 10; // 除以10存储，因为字段是uint8_t
    
    // 保存到NVS
    saveToAddressBook(device);
    
    // 构建servo配置命令
    StaticJsonDocument<256> payloadDoc;
    payloadDoc["sweep_angle"] = sweepAngle;
    payloadDoc["action_delay"] = actionDelay;
    
    StaticJsonDocument<384> mesh_cmd_doc;
    mesh_cmd_doc["to"] = device.nodeId;
    mesh_cmd_doc["cmd"] = "configServo";
    mesh_cmd_doc["payload"] = payloadDoc.as<JsonObject>();
    String mesh_cmd_str;
    serializeJson(mesh_cmd_doc, mesh_cmd_str);
    mesh.sendSingle(device.nodeId, mesh_cmd_str);
    
    Serial.printf("Sent servo config command to '%s': %s\n", deviceName.c_str(), mesh_cmd_str.c_str());
  }

  sendCorsResponse(request, 200, "application/json", "{\"status\":\"success\", \"message\":\"Configuration sent.\"}");
}

void handleApiTriggerAction(AsyncWebServerRequest *request) {
  addCorsHeaders(request);
  String actionName = request->getParam("actionName", true)->value();
  if (actionName.length() > 0) {
    triggerAction(actionName);
    sendCorsResponse(request, 200, "application/json", "{\"status\":\"success\", \"message\":\"Action triggered.\"}");
  } else {
    sendCorsResponse(request, 400, "application/json", "{\"error\":\"Missing actionName\"}");
  }
}

void handleApiSaveAction(AsyncWebServerRequest *request) {
  addCorsHeaders(request);
  if (!request->hasParam("plain", true)) { sendCorsResponse(request, 400, "application/json", "{\"error\":\"Bad Request\"}"); return; }
  String body = request->getParam("plain", true)->value();

  // Parse JSON body: { actionName: string, actions: [ { name: string, states: [..] }, ... ] }
  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, body);
  if (err) { sendCorsResponse(request, 400, "application/json", "{\"error\":\"Invalid JSON\"}"); return; }

  String actionName = doc["actionName"].as<String>();
  if (actionName.length() == 0) { sendCorsResponse(request, 400, "application/json", "{\"error\":\"Missing actionName\"}"); return; }
  if (!doc.containsKey("actions") || !doc["actions"].is<JsonArray>()) {
    sendCorsResponse(request, 400, "application/json", "{\"error\":\"Missing actions array\"}"); return;
  }

  // We store ONLY the actions array JSON string under the key = actionName
  String actionsJsonStr; serializeJson(doc["actions"].as<JsonArray>(), actionsJsonStr);

  preferences.begin("actions", false);
  // Update the action_list CSV (append if not present)
  String actionList = preferences.getString("action_list", "");
  bool found = false;
  String newList = "";
  if (actionList.length() > 0) {
    char list_cstr[actionList.length() + 1]; strcpy(list_cstr, actionList.c_str());
    char* token = strtok(list_cstr, ",");
    while (token != NULL) {
      String name = String(token);
      if (name == actionName) found = true;
      if (name.length() > 0) {
        if (newList.length() > 0) newList += ",";
        newList += name;
      }
      token = strtok(NULL, ",");
    }
  }
  if (!found) {
    if (newList.length() > 0) newList += ",";
    newList += actionName;
  }
  preferences.putString("action_list", newList);

  // Save the actions array string for this action
  preferences.putString(actionName.c_str(), actionsJsonStr);
  preferences.end();

  // Refresh in-memory actions and publish discovery
  loadActions();
  publishActionDiscovery();

  sendCorsResponse(request, 200, "application/json", "{\"status\":\"success\", \"message\":\"Action saved.\"}");
}

void handleApiDeleteAction(AsyncWebServerRequest *request) {
  addCorsHeaders(request);
  String actionName = request->getParam("actionName", true)->value();
  if (actionName.length() == 0) { sendCorsResponse(request, 400, "application/json", "{\"error\":\"Missing actionName\"}"); return; }

  // Clean from "actions" namespace
  preferences.begin("actions", false);
  String actionList = preferences.getString("action_list", "");
  String newList = "";
  if (actionList.length() > 0) {
    char list_cstr[actionList.length() + 1]; strcpy(list_cstr, actionList.c_str());
    char* token = strtok(list_cstr, ",");
    while (token != NULL) {
      String name = String(token);
      if (name != actionName && name.length() > 0) {
        if (newList.length() > 0) newList += ",";
        newList += name;
      }
      token = strtok(NULL, ",");
    }
  }
  preferences.putString("action_list", newList);
  preferences.remove(actionName.c_str());
  preferences.end();

  // Update in-memory and HA discovery
  if (actions.count(actionName)) { actions.erase(actionName); }
  unpublishActionDiscovery(actionName);

  Serial.printf("Action '%s' deleted from actions namespace\n", actionName.c_str());
  sendCorsResponse(request, 200, "application/json", "{\"status\":\"success\", \"message\":\"Action deleted.\"}");
}

void handleApiExportConfig(AsyncWebServerRequest *request) {
  addCorsHeaders(request);
  
  DynamicJsonDocument doc(8192);
  JsonObject root = doc.to<JsonObject>();
  
  // 添加导出时间戳
  root["export_timestamp"] = millis();
  root["gateway_version"] = "1.0";
  
  // 创建设备配置数组
  JsonArray devices = root.createNestedArray("devices");
  
  for (auto const& [mac, device] : knownDevicesByMac) {
    JsonObject devObj = devices.createNestedObject();
    devObj["name"] = device.name;
    devObj["macAddress"] = device.macAddress;
    devObj["type"] = device.type;
    devObj["nodeId"] = device.nodeId;
    
    // 根据设备类型添加配置
    if (device.type == "MultiSwitch") {
      JsonObject config = devObj.createNestedObject("config");
      
      char on_color_hex[8], off_color_hex[8];
      snprintf(on_color_hex, sizeof(on_color_hex), "#%02x%02x%02x", device.on_r, device.on_g, device.on_b);
      snprintf(off_color_hex, sizeof(off_color_hex), "#%02x%02x%02x", device.off_r, device.off_g, device.off_b);
      
      uint8_t on_brightness = device.default_on_value > 0 ? device.default_on_value : device.brightness_on;
      uint8_t off_brightness = device.default_off_value > 0 ? device.default_off_value : device.brightness_off;
      
      config["on_color"] = on_color_hex;
      config["brightness_on"] = on_brightness;
      config["off_color"] = off_color_hex;
      config["brightness_off"] = off_brightness;
      config["linkedSensor"] = device.linkedEnvSensorName;
      config["daylightThreshold"] = device.daylightThresholdLx;
    }
    else if (device.type == "ServoController") {
      JsonObject config = devObj.createNestedObject("config");
      
      int sweepAngle = device.default_on_value > 0 ? device.default_on_value : 30;
      int actionDelay = device.default_off_value > 0 ? device.default_off_value * 10 : 500;
      
      config["sweep_angle"] = sweepAngle;
      config["action_delay"] = actionDelay;
    }
  }
  
  String response;
  serializeJson(doc, response);
  
  sendCorsResponse(request, 200, "application/json", response);
}

void handleApiAvailableSensors(AsyncWebServerRequest *request) {
  addCorsHeaders(request);
  DynamicJsonDocument doc(1024);
  JsonArray sensors = doc.to<JsonArray>();
  for (const auto& pair : knownDevicesByMac) {
    if (pair.second.type == "Env_Sensor" && pair.second.isOnline) {
      sensors.add(pair.second.name);
    }
  }
  String response; serializeJson(sensors, response);
  sendCorsResponse(request, 200, "application/json", response);
}

void handleApiAddAction(AsyncWebServerRequest *request) {
  addCorsHeaders(request);
  
  if (request->hasParam("plain", true)) {
    String body = request->getParam("plain", true)->value();
    Serial.println("请求体: " + body);
    
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, body);
    
    if (error) {
      sendCorsResponse(request, 400, "application/json", "{\"error\":\"Invalid JSON\"}");
      return;
    }
    
    // 获取设备信息
    uint32_t nodeId = doc["nodeId"];
    String deviceName = doc["name"];
    String deviceMac = doc["mac"].as<String>();
    
    // 适配新的数据格式：从payload字段获取动作数据
    JsonObject payloadData;
    if (doc.containsKey("payload")) {
      payloadData = doc["payload"];
    } else if (doc.containsKey("actionData")) {
      // 兼容旧格式
      payloadData = doc["actionData"];
    }
    
    // 检查是否为储存命令
    bool isStoreCommand = false;
    if (payloadData.containsKey("type") && strcmp(payloadData["type"], "save") == 0) {
      isStoreCommand = true;
    }
    
    // 如果是储存命令，直接存入NVS
    if (isStoreCommand) {
      Serial.printf("收到舵机动作储存命令，存入NVS，设备: %s (NodeID: %u)\n", deviceName.c_str(), nodeId);
      
      // 创建用于NVS存储的JSON对象
      StaticJsonDocument<1024> storageDoc;
      storageDoc["name"] = deviceName;
      storageDoc["mac"] = deviceMac;
      storageDoc["nodeId"] = nodeId;
      
      // 如果有动作数据，添加到存储文档中
      if (payloadData.containsKey("actionName") && payloadData.containsKey("action_type")) {
        String actionName = payloadData["actionName"].as<String>();
        String actionType = payloadData["action_type"].as<String>();
        
        // 读取现有的舵机信息
        preferences.begin("servo_info", true);
        String key = macToKey(deviceMac);
        String existingData = preferences.getString(key.c_str(), "");
        preferences.end();
        
        StaticJsonDocument<1024> existingDoc;
        JsonArray savedActions;
        
        if (existingData.length() > 0) {
          DeserializationError err = deserializeJson(existingDoc, existingData);
          if (!err) {
            // 复制现有数据
            storageDoc["timestamp"] = existingDoc["timestamp"];
            
            // 获取现有的saved_actions数组
            if (existingDoc.containsKey("saved_actions")) {
              savedActions = storageDoc.createNestedArray("saved_actions");
              
              // 复制现有动作，但排除同名动作（如果存在）
              for (JsonObject action : existingDoc["saved_actions"].as<JsonArray>()) {
                if (action["name"].as<String>() != actionName) {
                  savedActions.add(action);
                }
              }
            } else {
              savedActions = storageDoc.createNestedArray("saved_actions");
            }
          } else {
            // 如果解析失败，创建新的数组
            savedActions = storageDoc.createNestedArray("saved_actions");
            storageDoc["timestamp"] = millis();
          }
        } else {
          // 如果没有现有数据，创建新的数组
          savedActions = storageDoc.createNestedArray("saved_actions");
          storageDoc["timestamp"] = millis();
        }
        
        // 创建新的动作对象
        JsonObject newAction = savedActions.createNestedObject();
        newAction["name"] = actionName;
        newAction["type"] = actionType;
        
        // 添加动作参数
        JsonObject params = newAction.createNestedObject("params");
        if (payloadData.containsKey("angle")) {
          params["angle"] = payloadData["angle"];
        }
        if (payloadData.containsKey("angles")) {
          params["angles"] = payloadData["angles"];
        }
        if (payloadData.containsKey("delay")) {
          params["delay"] = payloadData["delay"];
        }
        if (payloadData.containsKey("delays")) {
          params["delays"] = payloadData["delays"];
        }
        if (payloadData.containsKey("return_delay")) {
          params["return_delay"] = payloadData["return_delay"];
        }
        if (payloadData.containsKey("repeat_count")) {
          params["repeat_count"] = payloadData["repeat_count"];
        }
        
        // 将完整的JSON对象转换为字符串
        String storageStr;
        serializeJson(storageDoc, storageStr);
        
        // 存储到NVS
        bool saved = saveServoInfoToNVS(storageStr);
        
        if (saved) {
          sendCorsResponse(request, 200, "application/json", "{\"success\":true,\"message\":\"动作已成功存储到NVS\"}");
        } else {
          sendCorsResponse(request, 500, "application/json", "{\"success\":false,\"message\":\"存储动作到NVS失败\"}");
        }
      } else {
        sendCorsResponse(request, 400, "application/json", "{\"success\":false,\"message\":\"缺少必要的动作参数\"}");
      }
      return;
    }
    
    // 构建发送到mesh网络的命令（非储存命令）
    StaticJsonDocument<512> meshCommand;
    meshCommand["to"] = nodeId;
    
    // 根据payload中的type字段决定命令类型
    String commandType = "runAction"; // 默认值
    meshCommand["cmd"] = commandType;
    
    JsonObject payload = meshCommand.createNestedObject("payload");
    
    // 传递操作类型给舵机设备
    if (payloadData.containsKey("type")) {
      payload["type"] = payloadData["type"]; // save 或 action
    }
    
    // 从新格式中获取action_type作为动作类型
    String actionType;
    if (payloadData.containsKey("action_type")) {
      actionType = payloadData["action_type"].as<const char*>();
    }
    payload["action"] = actionType;
    
    // 传递actionName给舵机设备
    if (payloadData.containsKey("actionName")) {
      payload["actionName"] = payloadData["actionName"];
    }
    
    JsonObject params = payload.createNestedObject("params");
    if (payloadData.containsKey("angle")) {
      params["angle"] = payloadData["angle"];
    }
    if (payloadData.containsKey("angles")) {
      params["angles"] = payloadData["angles"];
    }
    if (payloadData.containsKey("delay")) {
      params["delay"] = payloadData["delay"];
    }
    if (payloadData.containsKey("delays")) {
      params["delays"] = payloadData["delays"];
    }
    if (payloadData.containsKey("return_delay")) {
      params["return_delay"] = payloadData["return_delay"];
    }
    if (payloadData.containsKey("repeat_count")) {
      params["repeat_count"] = payloadData["repeat_count"];
    }
    
    // 发送到mesh网络
    String meshCommandStr;
    serializeJson(meshCommand, meshCommandStr);
    mesh.sendSingle(nodeId, meshCommandStr);
    
    Serial.printf("转发命令到设备 %s (NodeID: %u): %s\n", deviceName.c_str(), nodeId, meshCommandStr.c_str());
    
    sendCorsResponse(request, 200, "application/json", "{\"success\":true,\"message\":\"动作命令已转发到设备\"}");
  } else {
    sendCorsResponse(request, 400, "application/json", "{\"error\":\"No data provided\"}");
  }
}

// Static file handlers
void handleRoot(AsyncWebServerRequest *request) {
  addCorsHeaders(request);
  String html = R"(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>PainlessMesh ETH Gateway</title>
    <link rel="icon" type="image/svg+xml" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'%3E%3Ccircle cx='50' cy='50' r='40' fill='%23007bff'/%3E%3Ctext x='50' y='60' text-anchor='middle' fill='white' font-size='40' font-family='Arial'%3EM%3C/text%3E%3C/svg%3E">
    <style>
        * {
            margin: 0;
            padding: 0;
            box-sizing: border-box;
        }
        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            min-height: 100vh;
            color: #333;
        }
        .container {
            max-width: 1200px;
            margin: 0 auto;
            padding: 20px;
        }
        .header {
            text-align: center;
            margin-bottom: 30px;
            color: white;
        }
        .nav {
            display: flex;
            justify-content: center;
            margin-bottom: 30px;
            flex-wrap: wrap;
            gap: 10px;
        }
        .nav-link {
            padding: 12px 24px;
            background: rgba(255, 255, 255, 0.1);
            color: white;
            text-decoration: none;
            border-radius: 25px;
            transition: all 0.3s ease;
            backdrop-filter: blur(10px);
            border: 1px solid rgba(255, 255, 255, 0.2);
        }
        .nav-link:hover, .nav-link.active {
            background: rgba(255, 255, 255, 0.2);
            transform: translateY(-2px);
        }
        #app {
            background: rgba(255, 255, 255, 0.95);
            border-radius: 15px;
            padding: 30px;
            box-shadow: 0 20px 40px rgba(0, 0, 0, 0.1);
            backdrop-filter: blur(10px);
        }
    </style>
</head>
<body>
    <div class="container">
        <div class="header">
            <h1>PainlessMesh ETH Gateway</h1>
            <p>智能设备管理控制台</p>
        </div>
        <nav class="nav">
            <a href="#devices" class="nav-link active" data-page="devices">设备</a>
            <a href="#status" class="nav-link" data-page="status">系统状态</a>
            <a href="#actions" class="nav-link" data-page="actions">动作</a>
            <a href="#config" class="nav-link" data-page="config">前端配置</a>
        </nav>
        <div id="app">
            <div class="loading">加载中...</div>
        </div>
    </div>
    <script src="config.js"></script>
    <script src="html_config.js"></script>
    <script src="css_config.js"></script>
    <script src="app.js"></script>
</body>
</html>
)";
  sendCorsResponse(request, 200, "text/html", html);
}

void handleAppJs(AsyncWebServerRequest *request) {
  addCorsHeaders(request);
  String appJs = R"(
// Simplified app.js for testing
console.log('Loading app.js...');

// Basic functionality to test the connection
let allDevices = [];
let allActions = [];

// Fetch data function with debug logging
async function fetchData(endpoint) {
    try {
        console.log('=== Web端发送请求 ===');
        console.log('请求URL:', UI_CONFIG.API_BASE_URL + endpoint);
        console.log('请求时间:', new Date().toISOString());
        
        const response = await fetch(UI_CONFIG.API_BASE_URL + endpoint);
        
        console.log('=== Web端收到响应 ===');
        console.log('响应状态:', response.status);
        console.log('响应状态文本:', response.statusText);
        console.log('响应头:', Object.fromEntries(response.headers.entries()));
        
        if (!response.ok) {
            throw new Error(`Network response was not ok: ${response.status} ${response.statusText}`);
        }
        
        const responseText = await response.text();
        console.log('响应内容:', responseText);
        
        const jsonData = JSON.parse(responseText);
        console.log('解析后的JSON:', jsonData);
        console.log('=== Web端请求完成 ===');
        
        return jsonData;
    } catch (error) {
        console.error('=== Web端请求错误 ===');
        console.error('错误详情:', error);
        console.error('错误堆栈:', error.stack);
        showToast('Error: Could not connect to the gateway.', 'error');
        throw error;
    }
}

// Show toast message
function showToast(message, type = 'info') {
    console.log(`Toast [${type}]: ${message}`);
    // Simple alert for now
    alert(message);
}

// Render actions page
async function renderActionsPage() {
    const app = document.getElementById('app');
    app.innerHTML = '<h2>动作管理</h2><div id="actions-container">加载中...</div>';
    
    try {
        const actions = await fetchData('/api/actions');
        const devices = await fetchData('/api/devices');
        
        let html = '<h3>动作列表</h3>';
        if (actions && actions.length > 0) {
            actions.forEach(action => {
                html += `<div class="action-card">
                    <h4>${action.name}</h4>
                    <p>${action.description || '无描述'}</p>
                </div>`;
            });
        } else {
            html += '<p>暂无动作</p>';
        }
        
        html += '<h3>设备列表</h3>';
        if (devices && devices.length > 0) {
            devices.forEach(device => {
                html += `<div class="device-card">
                    <h4>${device.name}</h4>
                    <p>状态: ${device.online ? '在线' : '离线'}</p>
                </div>`;
            });
        } else {
            html += '<p>暂无设备</p>';
        }
        
        document.getElementById('actions-container').innerHTML = html;
    } catch (error) {
        document.getElementById('actions-container').innerHTML = '<p>加载失败</p>';
    }
}

// Simple router
const routes = {
    'devices': () => { document.getElementById('app').innerHTML = '<h2>设备管理</h2><p>设备页面开发中...</p>'; },
    'status': () => { document.getElementById('app').innerHTML = '<h2>系统状态</h2><p>状态页面开发中...</p>'; },
    'actions': renderActionsPage,
    'config': () => { document.getElementById('app').innerHTML = '<h2>前端配置</h2><p>配置页面开发中...</p>'; }
};

function handleRouteChange() {
    const hash = window.location.hash.substring(1) || 'devices';
    const renderFunction = routes[hash];
    
    // Update nav active state
    document.querySelectorAll('.nav-link').forEach(link => {
        link.classList.toggle('active', link.dataset.page === hash);
    });
    
    if (renderFunction) {
        renderFunction();
    }
}

// Initialize
function init() {
    console.log('Initializing app...');
    
    // 确保CONFIG对象已加载后再尝试发现网关地址
    if (typeof CONFIG !== 'undefined' && CONFIG.discoverGateway) {
        CONFIG.discoverGateway().then(() => {
            console.log('Gateway discovery completed during initialization');
        }).catch(error => {
            console.warn('Gateway discovery failed during initialization:', error);
        });
    } else {
        console.warn('CONFIG object not available during initialization');
    }
    
    // Setup navigation
    document.querySelectorAll('.nav-link').forEach(link => {
        link.addEventListener('click', (e) => {
            e.preventDefault();
            window.location.hash = link.dataset.page;
        });
    });
    
    // Setup router
    window.addEventListener('hashchange', handleRouteChange);
    
    // Initial route
    handleRouteChange();
}

// Start when DOM is ready
if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
} else {
    init();
}
)";
  sendCorsResponse(request, 200, "application/javascript", appJs);
}

void handleConfigJs(AsyncWebServerRequest *request) {
  addCorsHeaders(request);
  String configJs = R"(
// =============================================================================
//                           前端配置文件 (config.js)
// =============================================================================
//
//  这个文件集中了所有前端可以自定义的参数。
//  修改这里的配置后，前端代码无需任何改动。
//

// --- API 配置 ---
const UI_CONFIG = {
    // 网关 IP 地址和端口
    API_BASE_URL: 'http://192.168.1.66',
    
    // --- 网络请求配置 ---
    // 请求超时时间 (毫秒)
    REQUEST_TIMEOUT: 10000,
    
    // 缓存破坏参数名称
    CACHE_BUSTER_PARAM: '_ts',
    
    // --- 轮询和刷新配置 ---
    // 设备状态轮询间隔 (毫秒)
    DEVICE_POLLING_INTERVAL: 5000,
    
    // 系统状态刷新间隔 (毫秒)
    STATUS_REFRESH_INTERVAL: 10000,
    
    // --- UI 配置 ---
    // Toast 消息显示时间 (毫秒)
    TOAST_DURATION: 3000,
    
    // 模态对话框动画时间 (毫秒)
    MODAL_ANIMATION_DURATION: 300,
    
    // --- 设备配置默认值 ---
    // LED 亮度默认值
    DEFAULT_BRIGHTNESS_ON: 80,
    DEFAULT_BRIGHTNESS_OFF: 10,
    
    // LED 颜色默认值
    DEFAULT_ON_COLOR: '#00ff00',
    DEFAULT_OFF_COLOR: '#ff0000',
    
    // 环境传感器默认值
    DEFAULT_DAYLIGHT_THRESHOLD: 0,
    
    // 舵机配置默认值
    DEFAULT_SERVO_SWEEP_ANGLE: 30,
    DEFAULT_SERVO_ACTION_DELAY: 500,
    
    // 光照阈值默认值 (勒克斯)
    DEFAULT_LIGHT_THRESHOLD: 500,
    
    // 温度阈值默认值 (摄氏度)
    DEFAULT_TEMPERATURE_THRESHOLD: 25.0,
    
    // 湿度阈值默认值 (%)
    DEFAULT_HUMIDITY_THRESHOLD: 60.0,
    
    // 压力阈值默认值 (hPa)
    DEFAULT_PRESSURE_THRESHOLD: 1013.25,
    
    // 气体传感器阈值默认值 (ppm)
    DEFAULT_GAS_THRESHOLD: 400,
    
    // 土壤湿度阈值默认值 (%)
    DEFAULT_SOIL_MOISTURE_THRESHOLD: 30.0,
    
    // PIR 传感器延迟默认值 (毫秒)
    DEFAULT_PIR_DELAY: 5000,
    
    // 超声波传感器距离阈值默认值 (厘米)
    DEFAULT_DISTANCE_THRESHOLD: 20,
    
    // 声音传感器阈值默认值 (分贝)
    DEFAULT_SOUND_THRESHOLD: 60,
    
    // 振动传感器阈值默认值
    DEFAULT_VIBRATION_THRESHOLD: 512,
    
    // 火焰传感器阈值默认值
    DEFAULT_FLAME_THRESHOLD: 512
};
)";
  sendCorsResponse(request, 200, "application/javascript", configJs);
}

void handleHtmlConfigJs(AsyncWebServerRequest *request) {
  addCorsHeaders(request);
  String htmlConfigJs = R"(
// HTML Configuration
console.log('Loading html_config.js...');
// Placeholder for HTML configuration
)";
  sendCorsResponse(request, 200, "application/javascript", htmlConfigJs);
}

void handleCssConfigJs(AsyncWebServerRequest *request) {
  addCorsHeaders(request);
  String cssConfigJs = R"(
// CSS Configuration
console.log('Loading css_config.js...');
// Placeholder for CSS configuration
)";
  sendCorsResponse(request, 200, "application/javascript", cssConfigJs);
}

void handleNotFound(AsyncWebServerRequest *request) {
  addCorsHeaders(request);
  
  // 特殊处理OPTIONS请求 - 检查数值64
  if (request->method() == HTTP_OPTIONS || request->method() == 64) {
    AsyncWebServerResponse *response = request->beginResponse(204);
    response->addHeader("Access-Control-Allow-Origin", "*");
    response->addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    response->addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
    request->send(response);
    return;
  }
  
  // 特殊处理POST请求到/api/network_config - 检查数值2
  if ((request->method() == HTTP_POST || request->method() == 2) && 
      request->url() == "/api/network_config") {
    Serial.println("Redirecting POST request to handleApiNetworkConfig");
    handleApiNetworkConfig(request);
    return;
  }
  
  sendCorsResponse(request, 404, "application/json", "{\"error\":\"Not Found\"}");
}

// Keep OTA handlers as they are, but add CORS headers
void handleOTAUpdatePage(AsyncWebServerRequest *request) {
  addCorsHeaders(request);
  String html = "<html><body><h1>Gateway OTA Update</h1><form method='POST' action='/update_upload' enctype='multipart/form-data'><input type='file' name='update'><input type='submit' value='Update'></form></body></html>";
  sendCorsResponse(request, 200, "text/html", html);
}

void handleOTAUpdateUpload(AsyncWebServerRequest *request, const String& filename, size_t index, uint8_t *data, size_t len, bool final) {
  static bool otaStarted = false;
  
  // 添加最基础的调试信息
  Serial.printf("*** handleOTAUpdateUpload CALLED *** Index: %u, Len: %u, Final: %s\n", 
                index, len, final ? "true" : "false");
  Serial.printf("*** Filename: %s, Data pointer: %p\n", filename.c_str(), data);
  Serial.printf("*** Request content length: %u\n", request->contentLength());
  Serial.printf("*** OTA Started flag: %s\n", otaStarted ? "true" : "false");
  
  Serial.printf("OTA Upload Handler Called - Index: %u, Len: %u, Final: %s\n", 
                index, len, final ? "true" : "false");
  
  if (index == 0) {
    Serial.printf("OTA Update Start: %s\n", filename.c_str());
    Serial.printf("Request Content Length: %u bytes\n", request->contentLength());
    
    // 检查可用空间
    size_t freeSketchSpace = ESP.getFreeSketchSpace();
    Serial.printf("Free sketch space: %u bytes\n", freeSketchSpace);
    
    if (freeSketchSpace < 100000) { // 至少需要100KB空间
      String errorMsg = "Insufficient space for OTA. Available: " + String(freeSketchSpace) + " bytes";
      Serial.println(errorMsg);
      otaStatus.inProgress = false;
      otaStatus.status = "error";
      otaStatus.error = errorMsg;
      return;
    }
    
    setStatusLed(STATE_OTA_UPDATING);
    
    // 尝试开始OTA更新
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
      Update.printError(Serial);
      String errorMsg = "OTA Begin Failed. Error: " + String(Update.getError());
      Serial.println(errorMsg);
      otaStatus.inProgress = false;
      otaStatus.status = "error";
      otaStatus.error = errorMsg;
      return;
    }
    
    otaStatus.inProgress = true;
    otaStatus.filename = filename;
    otaStatus.totalSize = request->contentLength();
    otaStatus.uploadedSize = 0;
    otaStatus.status = "uploading";
    otaStatus.error = "";
    otaStatus.startTime = millis();
    Serial.println("OTA Update initialized successfully");
    otaStarted = true;
  }
  
  if (otaStarted && len) {
    Serial.printf("Writing %u bytes (index: %u)...\n", len, index);
    
    size_t written = Update.write(data, len);
    if (written != len) {
      Update.printError(Serial);
      String errorMsg = "OTA Write Failed. Expected: " + String(len) + ", Written: " + String(written);
      Serial.println(errorMsg);
      otaStatus.inProgress = false;
      otaStatus.status = "error";
      otaStatus.error = errorMsg;
      otaStarted = false;
      return;
    }
    
    otaStatus.uploadedSize += len;
    Serial.printf("OTA Progress: %u/%u bytes (%.1f%%)\n", 
                  otaStatus.uploadedSize, otaStatus.totalSize, 
                  (float)otaStatus.uploadedSize / otaStatus.totalSize * 100.0);
  }
  
  if (final) {
    Serial.printf("*** FINAL BLOCK PROCESSING ***\n");
    Serial.printf("OTA Update End: %u bytes total\n", index + len);
    Serial.printf("Final status before processing - InProgress: %s, Status: %s\n", 
                  otaStatus.inProgress ? "true" : "false", otaStatus.status.c_str());
    Serial.printf("*** Update.hasError(): %s\n", Update.hasError() ? "true" : "false");
    if (Update.hasError()) {
      Serial.printf("*** Update error code: %u\n", Update.getError());
    }
    
    if (index + len == 0) {
      Serial.println("Error: No data received");
      otaStatus.inProgress = false;
      otaStatus.status = "error";
      otaStatus.error = "No data received";
      Serial.printf("Status updated to error: %s\n", otaStatus.error.c_str());
      sendCorsResponse(request, 400, "application/json", "{\"success\":false,\"status\":\"error\",\"error\":\"No data received\"}");
      Update.end();
      otaStarted = false;
      return;
    }
    
    Serial.printf("*** Calling Update.end(true) ***\n");
    
    if (Update.end(true)) {
      Serial.println("*** OTA Update.end(true) returned SUCCESS ***");
      Serial.println("OTA Update Success! Rebooting...");
      otaStatus.inProgress = false;
      otaStatus.status = "success";
      otaStatus.error = "";
      Serial.println("Status updated to success");
      
      // 发送成功响应
      sendCorsResponse(request, 200, "application/json", "{\"success\":true,\"status\":\"success\",\"message\":\"OTA update completed successfully\"}");
      
      Serial.println("*** About to call ESP.restart() in 2 seconds ***");
      delay(2000);  // 增加延迟时间确保状态更新
      Serial.println("*** Calling ESP.restart() NOW ***");
      ESP.restart();
      Serial.println("*** This line should never be reached ***");
    } else {
      Update.printError(Serial);
      String errorMsg = "OTA End Failed. Error: " + String(Update.getError());
      Serial.println(errorMsg);
      otaStatus.inProgress = false;
      otaStatus.status = "error";
      otaStatus.error = errorMsg;
      Serial.printf("Status updated to error: %s\n", otaStatus.error.c_str());
      
      // 发送错误响应
      String errorResponse = "{\"success\":false,\"status\":\"error\",\"error\":\"" + errorMsg + "\"}";
      sendCorsResponse(request, 400, "application/json", errorResponse);
    }
    otaStarted = false;
    Serial.println("OTA Upload Handler completed final processing");
  }
}




void clearAllNvs() {
  Serial.println("\n\n!!! NVS CLEAR TRIGGERED !!!");
  preferences.begin("AddrBook", false); if (preferences.clear()) { Serial.println("  - Cleared Device Address Book."); } preferences.end();
  preferences.begin("Discovered", false); if (preferences.clear()) { Serial.println("  - Cleared HA Discovery Records."); } preferences.end();
  preferences.begin("actions", false); if (preferences.clear()) { Serial.println("  - Cleared Actions."); } preferences.end();
  preferences.begin("logs", false); if (preferences.clear()) { Serial.println("  - Cleared Logs."); } preferences.end();
  preferences.begin("gateway-creds", false); if (preferences.clear()) { Serial.println("  - Cleared Credentials."); } preferences.end();
  preferences.begin("servo_info", false); if (preferences.clear()) { Serial.println("  - Cleared Servo Info."); } preferences.end();
  Serial.println("NVS CLEAR COMPLETE. Rebooting...");
}

void loadCredentials() {
  preferences.begin("gateway-creds", true);
  String server = preferences.getString("mqtt_server", DEFAULT_MQTT_SERVER);
  String port = preferences.getString("mqtt_port", DEFAULT_MQTT_PORT);
  String user = preferences.getString("mqtt_user", "");
  String pass = preferences.getString("mqtt_pass", "");
  preferences.end();
  strncpy(MQTT_SERVER, server.c_str(), sizeof(MQTT_SERVER) - 1);
  strncpy(MQTT_PORT_STR, port.c_str(), sizeof(MQTT_PORT_STR) - 1);
  strncpy(MQTT_USER, user.c_str(), sizeof(MQTT_USER) - 1);
  strncpy(MQTT_PASS, pass.c_str(), sizeof(MQTT_PASS) - 1);
  Serial.println("Loaded MQTT credentials from NVS.");
}

void saveCredentials() {
  preferences.begin("gateway-creds", false);
  preferences.putString("mqtt_server", MQTT_SERVER);
  preferences.putString("mqtt_port", MQTT_PORT_STR);
  preferences.putString("mqtt_user", MQTT_USER);
  preferences.putString("mqtt_pass", MQTT_PASS);
  preferences.end();
  Serial.println("Saved new MQTT credentials to NVS.");
}

void loadAddressBook() {
  knownDevicesByMac.clear(); nodeIdToMacMap.clear(); nameToMacMap.clear();
  preferences.begin("AddrBook", true); 
  String macList = preferences.getString("mac_list", "");
  if (macList.length() > 0) {
    char list_cstr[macList.length() + 1]; strcpy(list_cstr, macList.c_str());
    char* mac_cstr = strtok(list_cstr, ",");
    while (mac_cstr != NULL) {
      String macAddress = String(mac_cstr); String baseKey = macToKey(macAddress);      
      MeshDevice device;
      device.macAddress = macAddress;
      device.nodeId = preferences.getUInt((baseKey + "i").c_str(), 0);
      device.name = preferences.getString((baseKey + "n").c_str(), "Unknown");
      device.type = preferences.getString((baseKey + "t").c_str(), "Unknown");
      device.isOnline = false; device.lastSeenTimestamp = 0;
      device.hasBeenDiscovered = preferences.getBool((baseKey + "d").c_str(), false);
      device.linkedEnvSensorName = preferences.getString((baseKey + "s").c_str(), "");
      device.daylightThresholdLx = preferences.getUInt((baseKey + "x").c_str(), 0);
      uint32_t defaultValues = preferences.getUInt((baseKey + "v").c_str(), 0x500A);
      device.default_on_value = (defaultValues >> 8) & 0xFF;
      device.default_off_value = defaultValues & 0xFF;
      String led_config_str = preferences.getString((baseKey + "l").c_str(), "");
      if (led_config_str.length() > 0) {
        // 尝试解析JSON格式的LED配置
        DynamicJsonDocument ledConfigDoc(512);
        DeserializationError error = deserializeJson(ledConfigDoc, led_config_str);
        
        if (error == DeserializationError::Ok) {
          // 新的JSON格式
          if (ledConfigDoc.containsKey("onColor")) {
            JsonObject onColor = ledConfigDoc["onColor"];
            device.on_r = onColor["r"] | 0;
            device.on_g = onColor["g"] | 255;
            device.on_b = onColor["b"] | 0;
          }
          if (ledConfigDoc.containsKey("offColor")) {
            JsonObject offColor = ledConfigDoc["offColor"];
            device.off_r = offColor["r"] | 255;
            device.off_g = offColor["g"] | 0;
            device.off_b = offColor["b"] | 0;
          }
          device.brightness_on = ledConfigDoc["brightnessOn"] | DEFAULT_BRIGHTNESS_ON;
          device.brightness_off = ledConfigDoc["brightnessOff"] | DEFAULT_BRIGHTNESS_OFF;
          
          // 更新关联传感器和日光阈值
          if (ledConfigDoc.containsKey("linkedSensor")) {
            device.linkedEnvSensorName = ledConfigDoc["linkedSensor"].as<String>();
          }
          if (ledConfigDoc.containsKey("daylightThreshold")) {
            device.daylightThresholdLx = ledConfigDoc["daylightThreshold"] | device.daylightThresholdLx;
          }
          if (ledConfigDoc.containsKey("isDaytime")) {
            device.isDaytime = ledConfigDoc["isDaytime"].as<bool>();
          }
        } else {
          // 兼容旧的逗号分隔格式
          sscanf(led_config_str.c_str(), "%hhu,%hhu,%hhu,%hhu,%hhu,%hhu,%hhu,%hhu",
                 &device.on_r, &device.on_g, &device.on_b, &device.off_r, &device.off_g, &device.off_b,
                 &device.brightness_on, &device.brightness_off);
        }
      } else {
        // Set default colors to match switch device defaults
        // device.on_r = 10; device.on_g = 255; device.on_b = 10;    // #0aff0a (green)
        // device.off_r = 255; device.off_g = 10; device.off_b = 10;  // #ff0a0a (red)
        device.brightness_on = DEFAULT_BRIGHTNESS_ON; device.brightness_off = DEFAULT_BRIGHTNESS_OFF;
      }
      knownDevicesByMac[macAddress] = device;
      if (device.nodeId != 0) nodeIdToMacMap[device.nodeId] = macAddress;
      if (device.name != "Unknown") nameToMacMap[device.name] = macAddress;
      mac_cstr = strtok(NULL, ",");
    }
  }
  preferences.end();
}

void listLedDevicesFromNVS() {
  preferences.begin("AddrBook", true);
  
  String macList = preferences.getString("mac_list", "");
  if (macList.length() == 0) {
    Serial.println("No devices found in NVS.");
    preferences.end();
    return;
  }
  
  Serial.println("\n=== LED Devices and Configurations in NVS ===");
  
  // 分割MAC地址列表
  char* mac_cstr = strtok(const_cast<char*>(macList.c_str()), ",");
  int deviceCount = 0;
  int ledDeviceCount = 0;
  
  while (mac_cstr != NULL) {
    String macAddress = String(mac_cstr);
    String baseKey = macToKey(macAddress);
    
    deviceCount++;
    
    // 读取设备基本信息
    String deviceName = preferences.getString((baseKey + "n").c_str(), "Unknown");
    String deviceType = preferences.getString((baseKey + "t").c_str(), "Unknown");
    uint32_t nodeId = preferences.getUInt((baseKey + "i").c_str(), 0);
    
    // 只显示支持LED的设备类型
    if (deviceType == "MultiSwitch" || deviceType == "Switch" || deviceType.indexOf("Switch") != -1) {
      ledDeviceCount++;
      
      Serial.printf("Device #%d:\n", ledDeviceCount);
      Serial.printf("  Name: %s\n", deviceName.c_str());
      Serial.printf("  MAC: %s\n", macAddress.c_str());
      Serial.printf("  Type: %s\n", deviceType.c_str());
      Serial.printf("  NodeID: %u\n", nodeId);
      
      // 读取LED配置
      String led_config_str = preferences.getString((baseKey + "l").c_str(), "");
      if (led_config_str.length() > 0) {
        Serial.printf("  LED Config: %s\n", led_config_str.c_str());
        
        // 尝试解析JSON格式的LED配置
        DynamicJsonDocument ledConfigDoc(512);
        DeserializationError error = deserializeJson(ledConfigDoc, led_config_str);
        
        if (error == DeserializationError::Ok) {
          Serial.println("  Parsed LED Configuration:");
          if (ledConfigDoc.containsKey("onColor")) {
            JsonObject onColor = ledConfigDoc["onColor"];
            Serial.printf("    On Color: RGB(%d, %d, %d)\n", 
                         onColor["r"].as<int>(), onColor["g"].as<int>(), onColor["b"].as<int>());
          }
          if (ledConfigDoc.containsKey("offColor")) {
            JsonObject offColor = ledConfigDoc["offColor"];
            Serial.printf("    Off Color: RGB(%d, %d, %d)\n", 
                         offColor["r"].as<int>(), offColor["g"].as<int>(), offColor["b"].as<int>());
          }
          Serial.printf("    Brightness On: %d\n", ledConfigDoc["brightnessOn"].as<int>());
          Serial.printf("    Brightness Off: %d\n", ledConfigDoc["brightnessOff"].as<int>());
          if (ledConfigDoc.containsKey("linkedSensor")) {
            Serial.printf("    Linked Sensor: %s\n", ledConfigDoc["linkedSensor"].as<const char*>());
          }
          if (ledConfigDoc.containsKey("daylightThreshold")) {
            Serial.printf("    Daylight Threshold: %d lx\n", ledConfigDoc["daylightThreshold"].as<int>());
          }
          if (ledConfigDoc.containsKey("isDaytime")) {
            Serial.printf("    Is Daytime: %s\n", ledConfigDoc["isDaytime"].as<bool>() ? "Yes" : "No");
          }
        } else {
          Serial.println("    (Legacy format - could not parse as JSON)");
        }
      } else {
        Serial.println("  LED Config: Not configured");
      }
      
      // 读取其他相关配置
      String linkedSensor = preferences.getString((baseKey + "s").c_str(), "");
      uint32_t daylightThreshold = preferences.getUInt((baseKey + "x").c_str(), 0);
      
      if (linkedSensor.length() > 0) {
        Serial.printf("  Linked Env Sensor: %s\n", linkedSensor.c_str());
      }
      if (daylightThreshold > 0) {
        Serial.printf("  Daylight Threshold (legacy): %u lx\n", daylightThreshold);
      }
      
      Serial.println("  ---");
    }
    
    mac_cstr = strtok(NULL, ",");
  }
  
  Serial.printf("Total devices in NVS: %d\n", deviceCount);
  Serial.printf("LED-capable devices: %d\n", ledDeviceCount);
  
  preferences.end();
}

void saveToAddressBook(const MeshDevice& device) {
  preferences.begin("AddrBook", false);
  String baseKey = macToKey(device.macAddress);
  preferences.putUInt((baseKey + "i").c_str(), device.nodeId);
  preferences.putString((baseKey + "n").c_str(), device.name);
  preferences.putString((baseKey + "t").c_str(), device.type);
  preferences.putBool((baseKey + "d").c_str(), device.hasBeenDiscovered);
  preferences.putString((baseKey + "s").c_str(), device.linkedEnvSensorName);
  preferences.putUInt((baseKey + "x").c_str(), device.daylightThresholdLx);
  preferences.putUInt((baseKey + "v").c_str(), (device.default_on_value << 8) | device.default_off_value);
  
  // 只对支持LED的设备类型保存LED配置
  bool supportsLED = (device.type == "MultiSwitch" || 
                      device.type == "Switch" || 
                      device.type.indexOf("Switch") != -1 || 
                      device.type == "SmartSocket");
  
  if (supportsLED) {
    // 存储完整的LED配置为JSON格式
    DynamicJsonDocument ledConfigDoc(512);
    JsonObject onColor = ledConfigDoc.createNestedObject("onColor");
    onColor["r"] = device.on_r;
    onColor["g"] = device.on_g;
    onColor["b"] = device.on_b;
    
    JsonObject offColor = ledConfigDoc.createNestedObject("offColor");
    offColor["r"] = device.off_r;
    offColor["g"] = device.off_g;
    offColor["b"] = device.off_b;
    
    ledConfigDoc["brightnessOn"] = device.brightness_on;
    ledConfigDoc["brightnessOff"] = device.brightness_off;
    ledConfigDoc["linkedSensor"] = device.linkedEnvSensorName;
    ledConfigDoc["daylightThreshold"] = device.daylightThresholdLx;
    ledConfigDoc["isDaytime"] = device.isDaytime;
    
    String led_config_buffer;
    serializeJson(ledConfigDoc, led_config_buffer);
    preferences.putString((baseKey + "l").c_str(), led_config_buffer);
    
    // 添加NVS保存成功的调试信息
    Serial.printf("[NVS] Saved device '%s' to NVS - LED Config: %s\n", device.name.c_str(), led_config_buffer.c_str());
  } else {
    // 对于不支持LED的设备，不保存LED配置，并输出调试信息
    Serial.printf("[NVS] Saved device '%s' (%s) to NVS - No LED Config (device type does not support LED)\n", device.name.c_str(), device.type.c_str());
  }
  
  String macList = ""; bool first = true;
  for (auto const& [mac, dev] : knownDevicesByMac) {
    if (!first) { macList += ","; } macList += mac; first = false;
  }
  preferences.putString("mac_list", macList);
  preferences.end();
}

void printAllDeviceDiscoveryStatus() {
  Serial.println("=== NVS Auto-Discovery Status Report ===");
  preferences.begin("AddrBook", true);
  String macList = preferences.getString("mac_list", "");
  
  if (macList.length() == 0) {
    Serial.println("No devices found in NVS");
    preferences.end();
    return;
  }
  
  char list_cstr[macList.length() + 1];
  strcpy(list_cstr, macList.c_str());
  char* mac_cstr = strtok(list_cstr, ",");
  
  while (mac_cstr != NULL) {
    String mac = String(mac_cstr);
    String baseKey = macToKey(mac);
    String deviceName = preferences.getString((baseKey + "n").c_str(), "Unknown");
    bool hasBeenDiscovered = preferences.getBool((baseKey + "d").c_str(), false);
    
    Serial.printf("Device: %s (MAC: %s) - Auto-Discovery: %s\n", 
                  deviceName.c_str(), 
                  mac.c_str(), 
                  hasBeenDiscovered ? "✓ Discovered" : "✗ Not Discovered");
    
    mac_cstr = strtok(NULL, ",");
  }
  
  preferences.end();
  Serial.println("========================================");
}

void loadActions() {
  Serial.println("=== loadActions START ===");
  actions.clear();
  preferences.begin("actions", true);
  String actionList = preferences.getString("action_list", "");
  Serial.printf("Action list from NVS: %s\n", actionList.c_str());
  
  if (actionList.length() > 0) {
    // 检查是否为JSON数组格式
    if (actionList.startsWith("[") && actionList.endsWith("]")) {
      Serial.println("Parsing JSON array format action list");
      StaticJsonDocument<512> listDoc;
      if (deserializeJson(listDoc, actionList) == DeserializationError::Ok) {
        JsonArray actionArray = listDoc.as<JsonArray>();
        for (JsonVariant actionNameVar : actionArray) {
          String actionName = actionNameVar.as<String>();
          Serial.printf("Loading action: %s\n", actionName.c_str());
          String actionJsonStr = preferences.getString(actionName.c_str(), "");
          Serial.printf("Action JSON length: %d\n", actionJsonStr.length());
          
          if (actionJsonStr.length() > 0) {
            StaticJsonDocument<1024> doc;
            if (deserializeJson(doc, actionJsonStr) == DeserializationError::Ok) {
              Action action; action.name = actionName;
              
              // 检查是否有payload.message.payload结构
              if (doc.containsKey("payload") && doc["payload"].containsKey("message") && 
                  doc["payload"]["message"].containsKey("payload")) {
                JsonArray stepsArray = doc["payload"]["message"]["payload"];
                Serial.printf("Found steps array with %d elements\n", stepsArray.size());
                
                for (JsonObject stepObj : stepsArray) {
                  ActionStep step;
                  step.deviceName = stepObj["name"].as<String>();
                  serializeJson(stepObj["states"], step.statesJson);
                  action.steps.push_back(step);
                  Serial.printf("  Added step: device='%s', states='%s'\n", 
                               step.deviceName.c_str(), step.statesJson.c_str());
                }
              } else {
                Serial.printf("Action data missing expected payload structure\n");
              }
              
              actions[actionName] = action;
              Serial.printf("Action '%s' loaded successfully with %d steps\n", actionName.c_str(), action.steps.size());
            } else {
              Serial.printf("Failed to parse JSON for action: %s\n", actionName.c_str());
            }
          } else {
            Serial.printf("No JSON data found for action: %s\n", actionName.c_str());
          }
        }
      } else {
        Serial.println("Failed to parse action list JSON array");
      }
    } else {
      // 兼容旧的逗号分隔格式
      Serial.println("Parsing comma-separated format action list");
      char list_cstr[actionList.length() + 1]; strcpy(list_cstr, actionList.c_str());
      char* actionName_cstr = strtok(list_cstr, ",");
      while (actionName_cstr != NULL) {
        String actionName = String(actionName_cstr);
        Serial.printf("Loading action: %s\n", actionName.c_str());
        String actionJsonStr = preferences.getString(actionName.c_str(), "");
        Serial.printf("Action JSON length: %d\n", actionJsonStr.length());
        
        if (actionJsonStr.length() > 0) {
          StaticJsonDocument<1024> doc;
          if (deserializeJson(doc, actionJsonStr) == DeserializationError::Ok) {
            Action action; action.name = actionName;
            
            // 检查是否有payload.message.payload结构
            if (doc.containsKey("payload") && doc["payload"].containsKey("message") && 
                doc["payload"]["message"].containsKey("payload")) {
              JsonArray stepsArray = doc["payload"]["message"]["payload"];
              Serial.printf("Found steps array with %d elements\n", stepsArray.size());
              
              for (JsonObject stepObj : stepsArray) {
                ActionStep step;
                step.deviceName = stepObj["name"].as<String>();
                serializeJson(stepObj["states"], step.statesJson);
                action.steps.push_back(step);
                Serial.printf("  Added step: device='%s', states='%s'\n", 
                             step.deviceName.c_str(), step.statesJson.c_str());
              }
            } else {
              Serial.printf("Action data missing expected payload structure\n");
            }
            
            actions[actionName] = action;
            Serial.printf("Action '%s' loaded successfully with %d steps\n", actionName.c_str(), action.steps.size());
          } else {
            Serial.printf("Failed to parse JSON for action: %s\n", actionName.c_str());
          }
        } else {
          Serial.printf("No JSON data found for action: %s\n", actionName.c_str());
        }
        actionName_cstr = strtok(NULL, ",");
      }
    }
  } else {
    Serial.println("No action list found in NVS");
  }
  preferences.end();
  Serial.printf("=== loadActions END: Loaded %d actions ===\n", actions.size());
}

void triggerAction(const String& actionName) {
  if (!actions.count(actionName)) return;
  Action& action = actions[actionName];
  StaticJsonDocument<1536> finalMeshCommand;
  finalMeshCommand["cmd"] = "groupState";
  JsonArray payloadArray = finalMeshCommand.createNestedArray("payload");
  for (const auto& step : action.steps) {
    JsonObject stepObj = payloadArray.createNestedObject();
    stepObj["name"] = step.deviceName;

    // 解析存储的 statesJson
    JsonDocument tempStatesDoc;
    DeserializationError err = deserializeJson(tempStatesDoc, step.statesJson);

    // 统一构建规范的 states 数组: ["on","off",...] 而不是 ["on,off,..."]
    JsonArray outStates = stepObj.createNestedArray("states");
    if (!err && tempStatesDoc.is<JsonArray>()) {
      JsonArray inStates = tempStatesDoc.as<JsonArray>();
      // 情况1: 只有一个元素且是逗号分隔的字符串 -> 拆分为数组
      if (inStates.size() == 1 && inStates[0].is<const char*>()) {
        const char* s = inStates[0].as<const char*>();
        if (s && strchr(s, ',') != nullptr) {
          String s2 = String(s);
          int start = 0;
          while (true) {
            int comma = s2.indexOf(',', start);
            String part = (comma == -1) ? s2.substring(start) : s2.substring(start, comma);
            part.trim();
            if (part.length() > 0) outStates.add(part);
            if (comma == -1) break;
            start = comma + 1;
          }
        } else {
          // 单个元素且不含逗号，直接使用
          outStates.add(s ? String(s) : "");
        }
      } else {
        // 情况2: 原本就是数组，逐个复制
        for (JsonVariant v : inStates) {
          if (v.is<const char*>()) {
            outStates.add(String(v.as<const char*>()));
          } else if (v.is<String>()) {
            outStates.add(v.as<String>());
          }
        }
      }
    } else if (!err && tempStatesDoc.is<const char*>()) {
      // 情况3: 直接就是字符串，按逗号拆分
      const char* s = tempStatesDoc.as<const char*>();
      if (s) {
        String s2 = String(s);
        int start = 0;
        while (true) {
          int comma = s2.indexOf(',', start);
          String part = (comma == -1) ? s2.substring(start) : s2.substring(start, comma);
          part.trim();
          if (part.length() > 0) outStates.add(part);
          if (comma == -1) break;
          start = comma + 1;
        }
      }
    }
  }
  String commandString;
  serializeJson(finalMeshCommand, commandString);
  mesh.sendBroadcast(commandString);
  Serial.printf("Broadcasted Action Command: %s\n", commandString.c_str());
  
  // Send WebSocket update
  sendActionStatusUpdate(actionName, "triggered");
}

void publishActionDiscovery() {
  if (!mqttClient.connected()) {
    Serial.println("MQTT not connected, skipping HA discovery");
    return;
  }
  Serial.println("\n--- Publishing HA Discovery for all Actions ---");
  Serial.printf("Actions count: %d\n", actions.size());
  
  String base_id = MQTT_CLIENT_ID;
  base_id.replace(".", "_");
  StaticJsonDocument<256> deviceDoc;
  deviceDoc["identifiers"][0] = base_id;
  deviceDoc["name"] = GATEWAY_DEVICE_NAME;
  deviceDoc["model"] = HA_DEVICE_MODEL;
  deviceDoc["manufacturer"] = HA_DEVICE_MANUFACTURER;

  for (auto const& [actionName, action] : actions) {
    Serial.printf("Processing action: %s\n", actionName.c_str());
    Serial.printf("  Action steps count: %d\n", action.steps.size());
    
    StaticJsonDocument<1536> config_doc;
    String unique_id = "action_" + actionName;
    unique_id.replace(" ", "_"); unique_id.toLowerCase();
    
    JsonDocument payloadDoc;
    JsonObject payloadObj = payloadDoc.to<JsonObject>();
    JsonArray steps = payloadObj.createNestedArray("payload");
    
    int stepIndex = 0;
    for (const auto& step : action.steps) {
      Serial.printf("  Step %d: device='%s', statesJson='%s'\n", stepIndex++, step.deviceName.c_str(), step.statesJson.c_str());
      JsonObject stepObj = steps.createNestedObject();
      stepObj["name"] = step.deviceName;
      JsonDocument tempStatesDoc;
      deserializeJson(tempStatesDoc, step.statesJson.c_str());
      stepObj["states"] = tempStatesDoc.as<JsonArray>();
    }
    String payload_press_str; serializeJson(payloadDoc, payload_press_str);
    Serial.printf("  Generated payload_press: %s\n", payload_press_str.c_str());

    config_doc["unique_id"] = unique_id;
    config_doc["name"] = actionName;
    config_doc["object_id"] = unique_id;
    config_doc["command_topic"] = "PainlessMesh/GroupCommand/ActionControl";
    config_doc["payload_press"] = payload_press_str;
    config_doc["availability_topic"] = AVAILABILITY_TOPIC;
    config_doc["device"] = deviceDoc;
    config_doc["icon"] = "mdi:button-pointer";

    char config_topic[256];
    snprintf(config_topic, sizeof(config_topic), "homeassistant/button/%s/config", unique_id.c_str());
    String output; serializeJson(config_doc, output);
    bool published = mqttClient.publish(config_topic, output.c_str(), true);
    Serial.printf("  - Published config for action: %s (success: %s)\n", actionName.c_str(), published ? "true" : "false");
    Serial.printf("    Topic: %s\n", config_topic);
    Serial.printf("    Message: %s\n", output.c_str());
  }
  Serial.println("--- HA Discovery Publishing Complete ---");
}

void unpublishActionDiscovery(const String& actionName) {
  if (!mqttClient.connected()) return;
  String unique_id = "action_" + actionName;
  unique_id.replace(" ", "_"); unique_id.toLowerCase();
  char config_topic[256];
  snprintf(config_topic, sizeof(config_topic), "homeassistant/button/%s/config", unique_id.c_str());
  mqttClient.publish(config_topic, "", true);
  Serial.printf("Unpublished HA discovery for action: %s\n", actionName.c_str());
  Serial.printf("  Topic: %s\n", config_topic);
  Serial.printf("  Message: (empty)\n");
}

void unpublishServoActionDiscovery(const String& deviceName, const String& actionName) {
  if (!mqttClient.connected()) return;
  String action_unique_id = String(deviceName) + "_action_" + actionName;
  char action_config_topic[256];
  snprintf(action_config_topic, sizeof(action_config_topic), "homeassistant/button/%s/config", action_unique_id.c_str());
  mqttClient.publish(action_config_topic, "", true);
  Serial.printf("[HA] Unpublished servo action button: %s\n", actionName.c_str());
}

void publishGatewayDiscovery() {
  if (!mqttClient.connected()) return;
  Serial.println("\n--- Publishing HA Discovery for Gateway Sensors ---");
  
  String base_id = MQTT_CLIENT_ID;
  base_id.replace(".", "_");
  
  // Gateway device info
  StaticJsonDocument<256> deviceDoc;
  deviceDoc["identifiers"][0] = base_id;
  deviceDoc["name"] = GATEWAY_DEVICE_NAME;
  deviceDoc["model"] = "ESP32 PainlessMesh Gateway";
  deviceDoc["manufacturer"] = "DIY";
  
  // 1. Uptime Sensor
  {
    StaticJsonDocument<1024> config_doc;
    String unique_id = base_id + "_uptime";
    config_doc["unique_id"] = unique_id;
    config_doc["name"] = "Gateway Uptime";
    config_doc["object_id"] = unique_id;
    config_doc["state_topic"] = "PainlessMesh/Gateway/Status/uptime";
    config_doc["availability_topic"] = AVAILABILITY_TOPIC;
    config_doc["device"] = deviceDoc;
    config_doc["icon"] = "mdi:clock-outline";
    
    char config_topic[256];
    snprintf(config_topic, sizeof(config_topic), "homeassistant/sensor/%s/config", unique_id.c_str());
    String output; serializeJson(config_doc, output);
    mqttClient.publish(config_topic, output.c_str(), true);
    Serial.printf("  - Published uptime sensor config\n");
  }
  
  // 2. Memory Usage Sensor
  {
    StaticJsonDocument<1024> config_doc;
    String unique_id = base_id + "_memory_usage";
    config_doc["unique_id"] = unique_id;
    config_doc["name"] = "Gateway Memory Usage";
    config_doc["object_id"] = unique_id;
    config_doc["state_topic"] = "PainlessMesh/Gateway/Status/memory_usage";
    config_doc["unit_of_measurement"] = "%";
    config_doc["availability_topic"] = AVAILABILITY_TOPIC;
    config_doc["device"] = deviceDoc;
    config_doc["icon"] = "mdi:memory";
    
    char config_topic[256];
    snprintf(config_topic, sizeof(config_topic), "homeassistant/sensor/%s/config", unique_id.c_str());
    String output; serializeJson(config_doc, output);
    mqttClient.publish(config_topic, output.c_str(), true);
    Serial.printf("  - Published memory usage sensor config\n");
  }
  
  // 3. Connected Devices Count Sensor
  {
    StaticJsonDocument<1024> config_doc;
    String unique_id = base_id + "_connected_devices";
    config_doc["unique_id"] = unique_id;
    config_doc["name"] = "Connected Devices Count";
    config_doc["object_id"] = unique_id;
    config_doc["state_topic"] = "PainlessMesh/Gateway/Status/connected_devices";
    config_doc["availability_topic"] = AVAILABILITY_TOPIC;
    config_doc["device"] = deviceDoc;
    config_doc["icon"] = "mdi:devices";
    
    char config_topic[256];
    snprintf(config_topic, sizeof(config_topic), "homeassistant/sensor/%s/config", unique_id.c_str());
    String output; serializeJson(config_doc, output);
    mqttClient.publish(config_topic, output.c_str(), true);
    Serial.printf("  - Published connected devices sensor config\n");
  }
  
  // 4. Network Status Sensor
  {
    StaticJsonDocument<1024> config_doc;
    String unique_id = base_id + "_network_status";
    config_doc["unique_id"] = unique_id;
    config_doc["name"] = "Gateway Network Status";
    config_doc["object_id"] = unique_id;
    config_doc["state_topic"] = "PainlessMesh/Gateway/Status/network_status";
    config_doc["availability_topic"] = AVAILABILITY_TOPIC;
    config_doc["device"] = deviceDoc;
    config_doc["icon"] = "mdi:network";
    
    char config_topic[256];
    snprintf(config_topic, sizeof(config_topic), "homeassistant/sensor/%s/config", unique_id.c_str());
    String output; serializeJson(config_doc, output);
    mqttClient.publish(config_topic, output.c_str(), true);
    Serial.printf("  - Published network status sensor config\n");
  }
  
  // 5. CPU Usage Sensor (approximated by loop time)
  {
    StaticJsonDocument<1024> config_doc;
    String unique_id = base_id + "_cpu_usage";
    config_doc["unique_id"] = unique_id;
    config_doc["name"] = "Gateway CPU Usage";
    config_doc["object_id"] = unique_id;
    config_doc["state_topic"] = "PainlessMesh/Gateway/Status/cpu_usage";
    config_doc["unit_of_measurement"] = "%";
    config_doc["availability_topic"] = AVAILABILITY_TOPIC;
    config_doc["device"] = deviceDoc;
    config_doc["icon"] = "mdi:cpu-64-bit";
    
    char config_topic[256];
    snprintf(config_topic, sizeof(config_topic), "homeassistant/sensor/%s/config", unique_id.c_str());
    String output; serializeJson(config_doc, output);
    mqttClient.publish(config_topic, output.c_str(), true);
    Serial.printf("  - Published CPU usage sensor config\n");
  }
  
  // 6. NVS Usage Sensor
  {
    StaticJsonDocument<1024> config_doc;
    String unique_id = base_id + "_nvs_usage";
    config_doc["unique_id"] = unique_id;
    config_doc["name"] = "Gateway NVS Usage";
    config_doc["object_id"] = unique_id;
    config_doc["state_topic"] = "PainlessMesh/Gateway/Status/nvs_usage";
    config_doc["unit_of_measurement"] = "%";
    config_doc["availability_topic"] = AVAILABILITY_TOPIC;
    config_doc["device"] = deviceDoc;
    config_doc["icon"] = "mdi:harddisk";
    
    char config_topic[256];
    snprintf(config_topic, sizeof(config_topic), "homeassistant/sensor/%s/config", unique_id.c_str());
    String output; serializeJson(config_doc, output);
    mqttClient.publish(config_topic, output.c_str(), true);
    Serial.printf("  - Published NVS usage sensor config\n");
  }
}

void publishDeviceDeletion(const MeshDevice& device) {
  if (!mqttClient.connected()) {
    Serial.println("[DEBUG] publishDeviceDeletion: MQTT client not connected, aborting.");
    return;
  }
  
  Serial.printf("\n--- [DEBUG] Publishing HA Device Deletion for: %s ---\n", device.name.c_str());
  
  // 根据设备类型删除相应的HA实体
  if (device.type == "MultiSwitch") {
    // 删除开关实体
    String switch_unique_id = String(device.name) + "_switch";
    char switch_config_topic[256];
    snprintf(switch_config_topic, sizeof(switch_config_topic), "homeassistant/switch/%s/config", switch_unique_id.c_str());
    Serial.printf("[DEBUG] Publishing to topic: %s, payload: \"\"\n", switch_config_topic);
    mqttClient.publish(switch_config_topic, "", true);
    
    // 删除亮度传感器实体
    String brightness_unique_id = String(device.name) + "_brightness";
    char brightness_config_topic[256];
    snprintf(brightness_config_topic, sizeof(brightness_config_topic), "homeassistant/sensor/%s/config", brightness_unique_id.c_str());
    Serial.printf("[DEBUG] Publishing to topic: %s, payload: \"\"\n", brightness_config_topic);
    mqttClient.publish(brightness_config_topic, "", true);
    
  } else if (device.type == "ServoController") {
    // 删除伺服控制器角度数字实体
    String servo_unique_id = String(device.name) + "_angle";
    char servo_config_topic[256];
    snprintf(servo_config_topic, sizeof(servo_config_topic), "homeassistant/number/%s/config", servo_unique_id.c_str());
    Serial.printf("[DEBUG] Publishing to topic: %s, payload: \"\"\n", servo_config_topic);
    mqttClient.publish(servo_config_topic, "", true);
    
    // 删除所有保存的动作按钮实体
    // 读取NVS中存储的舵机配置来获取所有动作名称
    preferences.begin("servo_info", true);
    String servoKey = "";
    if (nameToMacMap.count(String(device.name))) {
        String macAddress = nameToMacMap[String(device.name)];
        servoKey = macToKey(macAddress);
    } else {
        servoKey = macToKey(String(device.name));
    }
    String servoInfoStr = preferences.getString(servoKey.c_str(), "");
    preferences.end();
    
    if (servoInfoStr.length() > 0) {
        StaticJsonDocument<2048> servoDoc;
        DeserializationError error = deserializeJson(servoDoc, servoInfoStr);
        if (!error && servoDoc.containsKey("saved_actions")) {
            JsonArray savedActions = servoDoc["saved_actions"];
            for (JsonObject action : savedActions) {
                String actionName = action["name"].as<String>();
                String action_unique_id = String(device.name) + "_action_" + actionName;
                char action_config_topic[256];
                snprintf(action_config_topic, sizeof(action_config_topic), "homeassistant/button/%s/config", action_unique_id.c_str());
                Serial.printf("[DEBUG] Deleting servo action button: %s by publishing to topic: %s\n", actionName.c_str(), action_config_topic);
                mqttClient.publish(action_config_topic, "", true);
            }
        }
    }
    
  } else if (device.type == "Env_Sensor") {
    // 遍历设备的所有传感器类型并发布空消息以删除它们
    for (const auto& sensorType : device.sensorTypes) {
      String unique_id = device.name + "_" + sensorType;
      char config_topic[256];
      snprintf(config_topic, sizeof(config_topic), "homeassistant/sensor/%s/config", unique_id.c_str());
      Serial.printf("[DEBUG] Deleting Env_Sensor entity: %s by publishing to topic: %s\n", sensorType.c_str(), config_topic);
      mqttClient.publish(config_topic, "", true);
    }
  } else if (device.type == "PIR_Sensor") {
    // 删除PIR传感器实体
    String pir_unique_id = String(device.name) + "_motion";
    char pir_config_topic[256];
    snprintf(pir_config_topic, sizeof(pir_config_topic), "homeassistant/binary_sensor/%s/config", pir_unique_id.c_str());
    Serial.printf("[DEBUG] Publishing to topic: %s, payload: \"\"\n", pir_config_topic);
    mqttClient.publish(pir_config_topic, "", true);
    
  } else if (device.type == "Contact_Sensor") {
    // 删除接触传感器实体
    String contact_unique_id = String(device.name) + "_contact";
    char contact_config_topic[256];
    snprintf(contact_config_topic, sizeof(contact_config_topic), "homeassistant/binary_sensor/%s/config", contact_unique_id.c_str());
    Serial.printf("[DEBUG] Publishing to topic: %s, payload: \"\"\n", contact_config_topic);
    mqttClient.publish(contact_config_topic, "", true);
  } else if (device.type == "SmartSocket") {
    // 删除智能插座实体
    String socket_unique_id = String(device.name) + "_relay";
    char socket_config_topic[256];
    snprintf(socket_config_topic, sizeof(socket_config_topic), "homeassistant/switch/%s/config", socket_unique_id.c_str());
    Serial.printf("[DEBUG] Publishing to topic: %s, payload: \"\"\n", socket_config_topic);
    mqttClient.publish(socket_config_topic, "", true);
  }
  
  // 发布设备离线状态到可用性主题
  String availability_topic = "PainlessMesh/Status/" + device.name;
  Serial.printf("[DEBUG] Publishing to topic: %s, payload: offline\n", availability_topic.c_str());
  mqttClient.publish(availability_topic.c_str(), "offline", true);
  
  Serial.println("[DEBUG] Device deletion published to HA successfully.");
}

// =================================================================================
// HA & Persistence Functions
// =================================================================================
void publishDiscoveryForDevice(const char* deviceType, const char* deviceName, int channels, const String& originalMessage) {
  if (!mqttClient.connected()) return;

  // --- 1. (核心修正) “先删除”逻辑 ---
  // 无论如何，都先尝试删除所有可能的旧实体，确保一个干净的开始
  Serial.printf("  -> Step 1: Un-publishing any old entities for '%s'...\n", deviceName);
  if (strcmp(deviceType, "SmartSocket") == 0) {
    String unique_id = String(deviceName) + "_relay";
    char config_topic[256];
    snprintf(config_topic, sizeof(config_topic), "homeassistant/switch/%s/config", unique_id.c_str());
    mqttClient.publish(config_topic, "", true);
  } 
  else if (strcmp(deviceType, "MultiSwitch") == 0) {
    const char* suffixes[] = {"_relay", "_relay_L", "_relay_R", "_relay_M"};
    for (int i=0; i<4; i++) {
        String unique_id = String(deviceName) + suffixes[i];
        char config_topic[256];
        snprintf(config_topic, sizeof(config_topic), "homeassistant/switch/%s/config", unique_id.c_str());
        mqttClient.publish(config_topic, "", true);
    }
  }
  else if (strcmp(deviceType, "ServoController") == 0) {
    // 删除舵机的数字控制实体
    String servo_unique_id = String(deviceName) + "_servo";
    char servo_config_topic[256];
    snprintf(servo_config_topic, sizeof(servo_config_topic), "homeassistant/number/%s/config", servo_unique_id.c_str());
    mqttClient.publish(servo_config_topic, "", true);
    
    // 删除所有可能的动作按钮实体
    // 读取NVS中存储的旧配置来获取所有动作名称
    preferences.begin("servo_info", true);
    String servoKey = "";
    if (nameToMacMap.count(String(deviceName))) {
        String macAddress = nameToMacMap[String(deviceName)];
        servoKey = macToKey(macAddress);
    } else {
        servoKey = macToKey(String(deviceName));
    }
    String oldServoInfoStr = preferences.getString(servoKey.c_str(), "");
    preferences.end();
    
    if (oldServoInfoStr.length() > 0) {
        StaticJsonDocument<2048> oldServoDoc;
        DeserializationError error = deserializeJson(oldServoDoc, oldServoInfoStr);
        if (!error && oldServoDoc.containsKey("saved_actions")) {
            JsonArray oldActions = oldServoDoc["saved_actions"];
            for (JsonObject oldAction : oldActions) {
                String oldActionName = oldAction["name"].as<String>();
                String old_action_unique_id = String(deviceName) + "_action_" + oldActionName;
                char old_action_config_topic[256];
                snprintf(old_action_config_topic, sizeof(old_action_config_topic), "homeassistant/button/%s/config", old_action_unique_id.c_str());
                mqttClient.publish(old_action_config_topic, "", true);
                Serial.printf("  - Deleted old action button: %s\n", oldActionName.c_str());
            }
        }
    }
  }
  // ... 您可以为 PIR, Env_Sensor 添加类似的删除逻辑 ...
  if (strcmp(deviceType, "Env_Sensor") == 0) {
    if (nameToMacMap.count(deviceName)) {
      String macAddress = nameToMacMap[deviceName];
      if (knownDevicesByMac.count(macAddress)) {
        MeshDevice& device = knownDevicesByMac[macAddress];
        device.sensorTypes.clear(); // 清空旧的传感器类型

        StaticJsonDocument<512> doc;
        if (deserializeJson(doc, originalMessage) == DeserializationError::Ok) {
          if (doc.containsKey("payload")) {
            JsonObject payload = doc["payload"];
            for (JsonPair kv : payload) {
              device.sensorTypes.push_back(kv.key().c_str());
            }
          }
        }
      }
    }
  }

  // 短暂延时，给 Broker 一点处理时间
  delay(50); 
  
  // --- 2. “后创建”逻辑 ---
  Serial.printf("  -> Step 2: Publishing new discovery config for '%s'...\n", deviceName);

  // --- 1. 准备所有实体都会共用的信息 ---
  char device_availability_topic[128];
  snprintf(device_availability_topic, sizeof(device_availability_topic), "PainlessMesh/Status/%s/availability", deviceName);
  char state_topic_buffer[128];
  snprintf(state_topic_buffer, sizeof(state_topic_buffer), "PainlessMesh/Status/%s/%s", deviceType, deviceName);
  StaticJsonDocument<256> deviceDoc;
  deviceDoc["identifiers"][0] = deviceName;
  deviceDoc["name"] = deviceName;
  deviceDoc["manufacturer"] = "PainlessMesh DIY";

  // --- 2. 根据设备类型执行不同的发现逻辑 ---
  if (strcmp(deviceType, "SmartSocket") == 0) {
    deviceDoc["model"] = HA_DEVICE_MODEL_SOCKET;
    StaticJsonDocument<1024> config_doc;
    String unique_id = String(deviceName) + "_relay";
    config_doc["unique_id"] = unique_id;
    config_doc["name"] = "Relay";
    String objectIdStr = unique_id;
    objectIdStr.toLowerCase();
    config_doc["object_id"] = objectIdStr;
    char command_topic_buffer[128];
    snprintf(command_topic_buffer, sizeof(command_topic_buffer), "PainlessMesh/Command/%s/%s", deviceType, deviceName);
    config_doc["state_topic"] = state_topic_buffer;
    config_doc["value_template"] = "{{ value_json.payload.states[0] }}";
    config_doc["command_topic"] = command_topic_buffer;
    config_doc["payload_on"] = "{\"states\":[\"on\"]}";
    config_doc["payload_off"] = "{\"states\":[\"off\"]}";
    config_doc["state_on"] = "on";
    config_doc["state_off"] = "off";
    JsonArray availability = config_doc.createNestedArray("availability");
    JsonObject gatewayAvail = availability.createNestedObject();
    gatewayAvail["topic"] = AVAILABILITY_TOPIC;
    JsonObject deviceAvail = availability.createNestedObject();
    deviceAvail["topic"] = device_availability_topic;
    config_doc["availability_mode"] = "all";
    config_doc["payload_available"] = PAYLOAD_AVAILABLE;
    config_doc["payload_not_available"] = PAYLOAD_NOT_AVAILABLE;
    config_doc["device"] = deviceDoc;
    char config_topic[256];
    snprintf(config_topic, sizeof(config_topic), "homeassistant/switch/%s/config", unique_id.c_str());
    String output; serializeJson(config_doc, output);
    mqttClient.publish(config_topic, output.c_str(), true);
  } 
  else if (strcmp(deviceType, "MultiSwitch") == 0) {
    if (channels <= 0 || channels > 3) return;
    deviceDoc["model"] = "MultiSwitch_" + String(channels) + "ch";
    for (int i = 0; i < channels; i++) {
        String suffix = (channels == 1) ? "_relay" : ((channels == 2) ? (i == 0 ? "_relay_L" : "_relay_R") : (i == 0 ? "_relay_L" : (i == 1 ? "_relay_M" : "_relay_R")));
        String friendly_name = (channels == 1) ? "Relay" : ((channels == 2) ? (i == 0 ? "Relay L" : "Relay R") : (i == 0 ? "Relay L" : (i == 1 ? "Relay M" : "Relay R")));
        StaticJsonDocument<1024> config_doc;
        String unique_id = String(deviceName) + suffix;
        config_doc["unique_id"] = unique_id;
        config_doc["name"] = friendly_name;
        String objectIdStr = unique_id;
        objectIdStr.toLowerCase();
        config_doc["object_id"] = objectIdStr;
        char command_topic_buffer[128];
        snprintf(command_topic_buffer, sizeof(command_topic_buffer), "PainlessMesh/Command/%s/%s", deviceType, deviceName);
        String on_p = "{\"states\":[";
        for(int j=0; j<channels; j++) { on_p += (i==j ? "\"on\"" : "\"ignore\""); if(j < channels-1) on_p += ","; }
        on_p += "]}";
        String off_p = "{\"states\":[";
        for(int j=0; j<channels; j++) { off_p += (i==j ? "\"off\"" : "\"ignore\""); if(j < channels-1) off_p += ","; }
        off_p += "]}";
        config_doc["state_topic"] = state_topic_buffer;
        config_doc["value_template"] = "{{ value_json.payload.states[" + String(i) + "] }}";
        config_doc["command_topic"] = command_topic_buffer;
        config_doc["payload_on"] = on_p;
        config_doc["payload_off"] = off_p;
        config_doc["state_on"] = "on";
        config_doc["state_off"] = "off";
        JsonArray availability = config_doc.createNestedArray("availability");
        JsonObject gatewayAvail = availability.createNestedObject();
        gatewayAvail["topic"] = AVAILABILITY_TOPIC;
        JsonObject deviceAvail = availability.createNestedObject();
        deviceAvail["topic"] = device_availability_topic;
        config_doc["availability_mode"] = "all";
        config_doc["payload_available"] = PAYLOAD_AVAILABLE;
        config_doc["payload_not_available"] = PAYLOAD_NOT_AVAILABLE;
        config_doc["device"] = deviceDoc;
        char config_topic[256];
        snprintf(config_topic, sizeof(config_topic), "homeassistant/switch/%s/config", unique_id.c_str());
        String output; serializeJson(config_doc, output);
        mqttClient.publish(config_topic, output.c_str(), true);
    }
  }
  else if (strcmp(deviceType, "PIR_Sensor") == 0 || strcmp(deviceType, "Contact_Sensor") == 0) {
      bool isPir = (strcmp(deviceType, "PIR_Sensor") == 0);
      deviceDoc["model"] = isPir ? "ESP-NOW PIR Sensor" : "ESP-NOW Contact Sensor";
      {
          StaticJsonDocument<1024> config_doc;
          String unique_id = String(deviceName) + (isPir ? "_motion" : "_contact");
          config_doc["unique_id"] = unique_id;
          config_doc["name"] = isPir ? "Motion" : "Contact";
          String objectIdStr = unique_id;
          objectIdStr.toLowerCase();
          config_doc["object_id"] = objectIdStr;
          config_doc["device_class"] = isPir ? "motion" : "door";
          config_doc["state_topic"] = state_topic_buffer;
          config_doc["value_template"] = "{{ value_json.payload.states[0] }}";
          config_doc["payload_on"] = "on";
          config_doc["payload_off"] = "off";
          JsonArray availability = config_doc.createNestedArray("availability");
          JsonObject gatewayAvail = availability.createNestedObject();
          gatewayAvail["topic"] = AVAILABILITY_TOPIC;
          JsonObject deviceAvail = availability.createNestedObject();
          deviceAvail["topic"] = device_availability_topic;
          config_doc["availability_mode"] = "all";
          config_doc["payload_available"] = PAYLOAD_AVAILABLE;
          config_doc["payload_not_available"] = PAYLOAD_NOT_AVAILABLE;
          config_doc["device"] = deviceDoc;
          char config_topic[256];
          snprintf(config_topic, sizeof(config_topic), "homeassistant/binary_sensor/%s/config", unique_id.c_str());
          String output; serializeJson(config_doc, output);
          mqttClient.publish(config_topic, output.c_str(), true);
      }
      {
          StaticJsonDocument<1024> config_doc;
          String unique_id = String(deviceName) + "_battery";
          config_doc["unique_id"] = unique_id;
          config_doc["name"] = "Battery";
          String objectIdStr = unique_id;
          objectIdStr.toLowerCase();
          config_doc["object_id"] = objectIdStr;
          config_doc["device_class"] = "voltage";
          config_doc["state_class"] = "measurement";
          config_doc["unit_of_measurement"] = "V";
          config_doc["state_topic"] = state_topic_buffer;
          config_doc["value_template"] = "{{ value_json.payload.battery }}";
          JsonArray availability = config_doc.createNestedArray("availability");
          JsonObject gatewayAvail = availability.createNestedObject();
          gatewayAvail["topic"] = AVAILABILITY_TOPIC;
          JsonObject deviceAvail = availability.createNestedObject();
          deviceAvail["topic"] = device_availability_topic;
          config_doc["availability_mode"] = "all";
          config_doc["payload_available"] = PAYLOAD_AVAILABLE;
          config_doc["payload_not_available"] = PAYLOAD_NOT_AVAILABLE;
          config_doc["device"] = deviceDoc;
          char config_topic[256];
          snprintf(config_topic, sizeof(config_topic), "homeassistant/sensor/%s/config", unique_id.c_str());
          String output; serializeJson(config_doc, output);
          mqttClient.publish(config_topic, output.c_str(), true);
      }
  }
  else if (strcmp(deviceType, "Env_Sensor") == 0) {
    deviceDoc["model"] = HA_DEVICE_MODEL_SENSOR;
    Serial.printf("\n--- Publishing HA Discovery for Env_Sensor (%s) ---\n", deviceName);

    // 在本函数作用域内重新解析消息，以确保 payload 对象的生命周期安全
    StaticJsonDocument<512> doc;
    deserializeJson(doc, originalMessage);
    JsonObject payload = doc["payload"];
    
    // 如果消息中没有 payload，则无法继续，直接退出
    if (!payload) {
      Serial.println("  [ERROR] Env_Sensor message has no payload for discovery. Aborting.");
      return;
    }

    // --- 按需创建 Battery 实体 ---
    if (payload.containsKey("battery")) {
      StaticJsonDocument<1024> config_doc;
      String unique_id = String(deviceName) + "_battery";
      config_doc["unique_id"] = unique_id;
      config_doc["name"] = "Battery";
      String objectIdStr = unique_id;
      objectIdStr.toLowerCase();
      config_doc["object_id"] = objectIdStr;
      config_doc["device_class"] = "voltage";
      config_doc["state_class"] = "measurement";
      config_doc["unit_of_measurement"] = "V";
      config_doc["state_topic"] = state_topic_buffer;
      config_doc["value_template"] = "{{ value_json.payload.battery }}";
      JsonArray availability = config_doc.createNestedArray("availability");
      JsonObject gatewayAvail = availability.createNestedObject();
      gatewayAvail["topic"] = AVAILABILITY_TOPIC;
      JsonObject deviceAvail = availability.createNestedObject();
      deviceAvail["topic"] = device_availability_topic;
      config_doc["availability_mode"] = "all";
      config_doc["payload_available"] = PAYLOAD_AVAILABLE;
      config_doc["payload_not_available"] = PAYLOAD_NOT_AVAILABLE;
      config_doc["device"] = deviceDoc;
      char config_topic[256];
      snprintf(config_topic, sizeof(config_topic), "homeassistant/sensor/%s/config", unique_id.c_str());
      String output; serializeJson(config_doc, output);
      mqttClient.publish(config_topic, output.c_str(), true);
      Serial.println("  - Published config for: Battery");
    }

    // --- 按需创建 Illuminance 实体 ---
    if (payload.containsKey("illuminance")) {
      StaticJsonDocument<1024> config_doc;
      String unique_id = String(deviceName) + "_illuminance";
      config_doc["unique_id"] = unique_id;
      config_doc["name"] = "Illuminance";
      String objectIdStr = unique_id;
      objectIdStr.toLowerCase();
      config_doc["object_id"] = objectIdStr;
      config_doc["device_class"] = "illuminance";
      config_doc["state_class"] = "measurement";
      config_doc["unit_of_measurement"] = "lx";
      config_doc["state_topic"] = state_topic_buffer;
      config_doc["value_template"] = "{{ value_json.payload.illuminance }}";
      JsonArray availability = config_doc.createNestedArray("availability");
      JsonObject gatewayAvail = availability.createNestedObject();
      gatewayAvail["topic"] = AVAILABILITY_TOPIC;
      JsonObject deviceAvail = availability.createNestedObject();
      deviceAvail["topic"] = device_availability_topic;
      config_doc["availability_mode"] = "all";
      config_doc["payload_available"] = PAYLOAD_AVAILABLE;
      config_doc["payload_not_available"] = PAYLOAD_NOT_AVAILABLE;
      config_doc["device"] = deviceDoc;
      char config_topic[256];
      snprintf(config_topic, sizeof(config_topic), "homeassistant/sensor/%s/config", unique_id.c_str());
      String output; serializeJson(config_doc, output);
      mqttClient.publish(config_topic, output.c_str(), true);
      Serial.println("  - Published config for: Illuminance");
    }

    // --- 按需创建 Temperature 实体 ---
    if (payload.containsKey("temperature")) {
      StaticJsonDocument<1024> config_doc;
      String unique_id = String(deviceName) + "_temperature";
      config_doc["unique_id"] = unique_id;
      config_doc["name"] = "Temperature";
      String objectIdStr = unique_id;
      objectIdStr.toLowerCase();
      config_doc["object_id"] = objectIdStr;
      config_doc["device_class"] = "temperature";
      config_doc["state_class"] = "measurement";
      config_doc["unit_of_measurement"] = "°C";
      config_doc["state_topic"] = state_topic_buffer;
      config_doc["value_template"] = "{{ value_json.payload.temperature }}";
      JsonArray availability = config_doc.createNestedArray("availability");
      JsonObject gatewayAvail = availability.createNestedObject();
      gatewayAvail["topic"] = AVAILABILITY_TOPIC;
      JsonObject deviceAvail = availability.createNestedObject();
      deviceAvail["topic"] = device_availability_topic;
      config_doc["availability_mode"] = "all";
      config_doc["payload_available"] = PAYLOAD_AVAILABLE;
      config_doc["payload_not_available"] = PAYLOAD_NOT_AVAILABLE;
      config_doc["device"] = deviceDoc;
      char config_topic[256];
      snprintf(config_topic, sizeof(config_topic), "homeassistant/sensor/%s/config", unique_id.c_str());
      String output; serializeJson(config_doc, output);
      mqttClient.publish(config_topic, output.c_str(), true);
      Serial.println("  - Published config for: Temperature");
    }

    // --- 按需创建 Humidity 实体 ---
    if (payload.containsKey("humidity")) {
      StaticJsonDocument<1024> config_doc;
      String unique_id = String(deviceName) + "_humidity";
      config_doc["unique_id"] = unique_id;
      config_doc["name"] = "Humidity";
      String objectIdStr = unique_id;
      objectIdStr.toLowerCase();
      config_doc["object_id"] = objectIdStr;
      config_doc["device_class"] = "humidity";
      config_doc["state_class"] = "measurement";
      config_doc["unit_of_measurement"] = "%";
      config_doc["state_topic"] = state_topic_buffer;
      config_doc["value_template"] = "{{ value_json.payload.humidity }}";
      JsonArray availability = config_doc.createNestedArray("availability");
      JsonObject gatewayAvail = availability.createNestedObject();
      gatewayAvail["topic"] = AVAILABILITY_TOPIC;
      JsonObject deviceAvail = availability.createNestedObject();
      deviceAvail["topic"] = device_availability_topic;
      config_doc["availability_mode"] = "all";
      config_doc["payload_available"] = PAYLOAD_AVAILABLE;
      config_doc["payload_not_available"] = PAYLOAD_NOT_AVAILABLE;
      config_doc["device"] = deviceDoc;
      char config_topic[256];
      snprintf(config_topic, sizeof(config_topic), "homeassistant/sensor/%s/config", unique_id.c_str());
      String output; serializeJson(config_doc, output);
      mqttClient.publish(config_topic, output.c_str(), true);
      Serial.println("  - Published config for: Humidity");
    }
  }
  else if (strcmp(deviceType, "ServoController") == 0) {
      deviceDoc["model"] = HA_DEVICE_MODEL_SERVO;
      { // Angle number entity
          StaticJsonDocument<1024> config_doc;
          String unique_id = String(deviceName) + "_angle";
          config_doc["unique_id"] = unique_id;
          config_doc["name"] = "Angle";
          String objectIdStr = unique_id;
          objectIdStr.toLowerCase();
          config_doc["object_id"] = objectIdStr;
          char command_topic_buffer[128];
          snprintf(command_topic_buffer, sizeof(command_topic_buffer), "PainlessMesh/Command/%s/%s", deviceType, deviceName);
          config_doc["command_topic"] = command_topic_buffer;
          config_doc["command_template"] = "{\"angle\":{{value}}}";
          config_doc["state_topic"] = state_topic_buffer;
          config_doc["value_template"] = "{{ value_json.currentAngle }}";
          config_doc["min"] = 0;
          config_doc["max"] = 180;
          config_doc["unit_of_measurement"] = "°";
          JsonArray availability = config_doc.createNestedArray("availability");
          JsonObject gatewayAvail = availability.createNestedObject();
          gatewayAvail["topic"] = AVAILABILITY_TOPIC;
          JsonObject deviceAvail = availability.createNestedObject();
          deviceAvail["topic"] = device_availability_topic;
          config_doc["availability_mode"] = "all";
          config_doc["payload_available"] = PAYLOAD_AVAILABLE;
          config_doc["payload_not_available"] = PAYLOAD_NOT_AVAILABLE;
          config_doc["device"] = deviceDoc;
          char config_topic[256];
          snprintf(config_topic, sizeof(config_topic), "homeassistant/number/%s/config", unique_id.c_str());
          String output; serializeJson(config_doc, output);
          mqttClient.publish(config_topic, output.c_str(), true);
      }
      // Sweep button entity removed - no longer needed
      
      // Generate button entities for stored actions
      preferences.begin("servo_info", true);
      String servoKey = "";
      // Find MAC address by device name
      if (nameToMacMap.count(String(deviceName))) {
          String macAddress = nameToMacMap[String(deviceName)];
          servoKey = macToKey(macAddress);
      } else {
          // Fallback: try using deviceName directly as key
          servoKey = macToKey(String(deviceName));
      }
      String servoInfoStr = preferences.getString(servoKey.c_str(), "");
      preferences.end();
      
      if (servoInfoStr.length() > 0) {
          StaticJsonDocument<2048> servoDoc;
          DeserializationError error = deserializeJson(servoDoc, servoInfoStr);
          if (!error && servoDoc.containsKey("saved_actions")) {
              JsonArray actions = servoDoc["saved_actions"];
              for (JsonObject action : actions) {
                  String actionName = action["name"].as<String>();
                  String actionType = action["type"].as<String>();
                  
                  // Create button entity for each stored action
                  StaticJsonDocument<1024> action_config_doc;
                  String action_unique_id = String(deviceName) + "_action_" + actionName;
                  action_config_doc["unique_id"] = action_unique_id;
                  action_config_doc["name"] = "Action: " + actionName;
                  String action_objectIdStr = action_unique_id;
                  action_objectIdStr.toLowerCase();
                  action_config_doc["object_id"] = action_objectIdStr;
                  
                  char action_command_topic_buffer[128];
                  snprintf(action_command_topic_buffer, sizeof(action_command_topic_buffer), "PainlessMesh/Command/%s/%s", deviceType, deviceName);
                  action_config_doc["command_topic"] = action_command_topic_buffer;
                  
                  // Build payload for executing the stored action
                  String payload = "{\"cmd\":\"runAction\",\"payload\":{\"type\":\"action\",\"actionName\":\"" + actionName + "\",\"action_type\":\"" + actionType + "\",\"params\":{";
                  
                  // Add action parameters in params object
                  bool hasParams = false;
                  if (action.containsKey("angle")) {
                      if (hasParams) payload += ",";
                      payload += "\"angle\":" + String(action["angle"].as<int>());
                      hasParams = true;
                  }
                  if (action.containsKey("angles")) {
                      if (hasParams) payload += ",";
                      payload += "\"angles\":[";
                      JsonArray angles = action["angles"];
                      for (size_t i = 0; i < angles.size(); i++) {
                          if (i > 0) payload += ",";
                          payload += String(angles[i].as<int>());
                      }
                      payload += "]";
                      hasParams = true;
                  }
                  if (action.containsKey("delay")) {
                      if (hasParams) payload += ",";
                      payload += "\"delay\":" + String(action["delay"].as<int>());
                      hasParams = true;
                  }
                  if (action.containsKey("delays")) {
                      if (hasParams) payload += ",";
                      payload += "\"delays\":[";
                      JsonArray delays = action["delays"];
                      for (size_t i = 0; i < delays.size(); i++) {
                          if (i > 0) payload += ",";
                          payload += String(delays[i].as<int>());
                      }
                      payload += "]";
                      hasParams = true;
                  }
                  if (action.containsKey("returnDelay")) {
                      if (hasParams) payload += ",";
                      payload += "\"return_delay\":" + String(action["returnDelay"].as<int>());
                      hasParams = true;
                  }
                  if (action.containsKey("repeatCount")) {
                      if (hasParams) payload += ",";
                      payload += "\"repeat_count\":" + String(action["repeatCount"].as<int>());
                      hasParams = true;
                  }
                  
                  payload += "}}}";
                  action_config_doc["payload_press"] = payload;
                  
                  JsonArray action_availability = action_config_doc.createNestedArray("availability");
                  JsonObject action_gatewayAvail = action_availability.createNestedObject();
                  action_gatewayAvail["topic"] = AVAILABILITY_TOPIC;
                  JsonObject action_deviceAvail = action_availability.createNestedObject();
                  action_deviceAvail["topic"] = device_availability_topic;
                  action_config_doc["availability_mode"] = "all";
                  action_config_doc["payload_available"] = PAYLOAD_AVAILABLE;
                  action_config_doc["payload_not_available"] = PAYLOAD_NOT_AVAILABLE;
                  action_config_doc["device"] = deviceDoc;
                  
                  char action_config_topic[256];
                  snprintf(action_config_topic, sizeof(action_config_topic), "homeassistant/button/%s/config", action_unique_id.c_str());
                  String action_output; serializeJson(action_config_doc, action_output);
                  mqttClient.publish(action_config_topic, action_output.c_str(), true);
                  
                  Serial.printf("  - Published action button: %s\n", actionName.c_str());
              }
          }
      }
  }
  else if (strcmp(deviceType, "Wireless_Button") == 0) {
    deviceDoc["model"] = "Wireless Button";
    
    // Create button event sensor
    StaticJsonDocument<1024> config_doc;
    String unique_id = String(deviceName) + "_button_event";
    config_doc["unique_id"] = unique_id;
    config_doc["name"] = "Button Event";
    String objectIdStr = unique_id;
    objectIdStr.toLowerCase();
    config_doc["object_id"] = objectIdStr;
    config_doc["state_topic"] = state_topic_buffer;
    config_doc["value_template"] = "{{ value_json.payload.button_event }}";
    JsonArray availability = config_doc.createNestedArray("availability");
    JsonObject gatewayAvail = availability.createNestedObject();
    gatewayAvail["topic"] = AVAILABILITY_TOPIC;
    JsonObject deviceAvail = availability.createNestedObject();
    deviceAvail["topic"] = device_availability_topic;
    config_doc["availability_mode"] = "all";
    config_doc["payload_available"] = PAYLOAD_AVAILABLE;
    config_doc["payload_not_available"] = PAYLOAD_NOT_AVAILABLE;
    config_doc["device"] = deviceDoc;
    
    char config_topic[256];
    snprintf(config_topic, sizeof(config_topic), "homeassistant/sensor/%s/config", unique_id.c_str());
    String output; serializeJson(config_doc, output);
    
    // 打印MQTT主题和消息内容
    Serial.printf("[MQTT DEBUG] Publishing to topic: %s\n", config_topic);
    Serial.printf("[MQTT DEBUG] Message content: %s\n", output.c_str());
    
    mqttClient.publish(config_topic, output.c_str(), true);
    Serial.println("  - Published config for: Button Event");
    
    // Create battery sensor
    StaticJsonDocument<1024> battery_config_doc;
    String battery_unique_id = String(deviceName) + "_battery";
    battery_config_doc["unique_id"] = battery_unique_id;
    battery_config_doc["name"] = "Battery";
    String battery_objectIdStr = battery_unique_id;
    battery_objectIdStr.toLowerCase();
    battery_config_doc["object_id"] = battery_objectIdStr;
    battery_config_doc["device_class"] = "battery";
    battery_config_doc["state_class"] = "measurement";
    battery_config_doc["unit_of_measurement"] = "V";
    battery_config_doc["state_topic"] = state_topic_buffer;
    battery_config_doc["value_template"] = "{{ value_json.payload.battery }}";
    JsonArray battery_availability = battery_config_doc.createNestedArray("availability");
    JsonObject battery_gatewayAvail = battery_availability.createNestedObject();
    battery_gatewayAvail["topic"] = AVAILABILITY_TOPIC;
    JsonObject battery_deviceAvail = battery_availability.createNestedObject();
    battery_deviceAvail["topic"] = device_availability_topic;
    battery_config_doc["availability_mode"] = "all";
    battery_config_doc["payload_available"] = PAYLOAD_AVAILABLE;
    battery_config_doc["payload_not_available"] = PAYLOAD_NOT_AVAILABLE;
    battery_config_doc["device"] = deviceDoc;
    
    char battery_config_topic[256];
    snprintf(battery_config_topic, sizeof(battery_config_topic), "homeassistant/sensor/%s/config", battery_unique_id.c_str());
    String battery_output; serializeJson(battery_config_doc, battery_output);
    
    // 打印MQTT主题和消息内容
    Serial.printf("[MQTT DEBUG] Publishing to topic: %s\n", battery_config_topic);
    Serial.printf("[MQTT DEBUG] Message content: %s\n", battery_output.c_str());
    
    mqttClient.publish(battery_config_topic, battery_output.c_str(), true);
    Serial.println("  - Published config for: Battery");
  }

  preferences.begin("Discovered", false);
  preferences.putBool(deviceName, true);
  preferences.end();
}

// =================================================================================
// 舵机信息NVS存储功能
// =================================================================================
bool saveServoInfoToNVS(const String& message) {
  StaticJsonDocument<1024> doc;
  DeserializationError error = deserializeJson(doc, message);
  if (error) {
    Serial.println("Failed to parse servo message for NVS storage");
    return false;
  }
  
  String servoName = doc["name"].as<String>();
  String servoMac = doc["mac"].as<String>();
  uint32_t nodeId = doc["nodeId"];
  
  preferences.begin("servo_info", false);
  
  // 使用MAC地址作为键来存储舵机信息
  // NVS键名最大15字符，所以使用简化的键名格式
  String key = macToKey(servoMac); // 直接使用MAC地址（去掉冒号）作为键
  
  // 读取旧配置进行比较
  String oldConfigStr = preferences.getString(key.c_str(), "");
  bool configChanged = false;
  
  if (oldConfigStr.length() > 0) {
    StaticJsonDocument<1024> oldDoc;
    DeserializationError oldError = deserializeJson(oldDoc, oldConfigStr);
    if (!oldError) {
      // 比较saved_actions数组是否发生变化
      String newActionsStr, oldActionsStr;
      if (doc.containsKey("saved_actions")) {
        serializeJson(doc["saved_actions"], newActionsStr);
      }
      if (oldDoc.containsKey("saved_actions")) {
        serializeJson(oldDoc["saved_actions"], oldActionsStr);
      }
      configChanged = (newActionsStr != oldActionsStr);
      if (configChanged) {
        Serial.printf("Servo '%s' actions configuration changed\n", servoName.c_str());
      } else {
        Serial.printf("Servo '%s' actions configuration unchanged, skipping discovery\n", servoName.c_str());
      }
    } else {
      configChanged = true; // 如果旧配置解析失败，认为配置已变化
    }
  } else {
    configChanged = true; // 如果没有旧配置，认为配置已变化
    Serial.printf("New servo '%s' detected, will publish discovery\n", servoName.c_str());
  }
  
  // 创建存储的JSON对象
  StaticJsonDocument<1024> storageDoc;
  storageDoc["name"] = servoName;
  storageDoc["mac"] = servoMac;
  storageDoc["nodeId"] = nodeId;
  storageDoc["timestamp"] = millis();
  
  // 复制saved_actions数组
  if (doc.containsKey("saved_actions")) {
    storageDoc["saved_actions"] = doc["saved_actions"];
  }
  
  String storageStr;
  serializeJson(storageDoc, storageStr);
  
  // 检查数据大小
  Serial.printf("Storage data size: %d bytes\n", storageStr.length());
  
  // ESP32 NVS单个键值对最大约4000字节，但建议不超过1900字节
  if (storageStr.length() > 1900) {
    Serial.printf("Warning: Data too large (%d bytes), storing basic info only\n", storageStr.length());
    
    // 创建简化版本，只保存基本信息和动作数量
    StaticJsonDocument<512> basicDoc;
    basicDoc["name"] = servoName;
    basicDoc["mac"] = servoMac;
    basicDoc["nodeId"] = nodeId;
    basicDoc["timestamp"] = millis();
    
    if (doc.containsKey("saved_actions")) {
      JsonArray actions = doc["saved_actions"];
      basicDoc["action_count"] = actions.size();
      // 只保存动作名称列表
      JsonArray actionNames = basicDoc.createNestedArray("action_names");
      for (JsonObject action : actions) {
        actionNames.add(action["name"].as<String>());
      }
    }
    
    storageStr = "";
    serializeJson(basicDoc, storageStr);
    Serial.printf("Reduced storage data size: %d bytes\n", storageStr.length());
  }
  
  // 检查NVS可用空间
  size_t usedBytes = preferences.getBytesLength(key.c_str());
  Serial.printf("Current NVS entry size for key '%s': %d bytes\n", key.c_str(), usedBytes);
  
  // 存储到NVS
  size_t written = preferences.putString(key.c_str(), storageStr);
  if (written > 0) {
    Serial.printf("Servo info saved to NVS: %s (%d bytes written)\n", servoName.c_str(), written);
    
    // 更新舵机列表
    String servoList = preferences.getString("servo_list", "");
    if (servoList.indexOf(key) == -1) {
      if (servoList.length() > 0) servoList += ",";
      servoList += key;
      if (preferences.putString("servo_list", servoList) == 0) {
        Serial.println("Warning: Failed to update servo list in NVS");
      }
    }
  } else {
    Serial.printf("Failed to save servo info to NVS: %s\n", servoName.c_str());
    Serial.printf("Attempted to write %d bytes with key '%s'\n", storageStr.length(), key.c_str());
    
    // 尝试获取NVS统计信息
    Serial.println("NVS Debug Info:");
    Serial.printf("Key length: %d\n", key.length());
    Serial.printf("Data length: %d\n", storageStr.length());
    
    // 检查是否是键名问题
    if (key.length() > 15) {
      Serial.println("Warning: NVS key might be too long (max 15 chars)");
    }
  }
  
  preferences.end();
  return configChanged;
}

// Action信息NVS存储功能
// =================================================================================
bool saveActionToNVS(const String& actionName, const JsonDocument& actionDoc) {
  Serial.printf("=== saveActionToNVS START ===\n");
  Serial.printf("Attempting to save action '%s' to NVS\n", actionName.c_str());
  Serial.printf("Input actionDoc size: %d bytes\n", measureJson(actionDoc));
  
  if (actionName.length() == 0 || actionName.length() > 15) {
    Serial.printf("Invalid action name length: %d (must be 1-15 chars)\n", actionName.length());
    return false;
  }
  
  if (!preferences.begin("actions", false)) {
    Serial.println("Failed to open NVS namespace 'actions'");
    return false;
  }
  Serial.println("NVS namespace 'actions' opened successfully");
  
  // 构建要存储的JSON数据
  DynamicJsonDocument storageDoc(2048);
  storageDoc["actionName"] = actionName;
  storageDoc["timestamp"] = millis();
  
  // 复制payload数据
  if (actionDoc.containsKey("payload")) {
    storageDoc["payload"] = actionDoc["payload"];
  }
  
  // 序列化为字符串
  String actionDataStr;
  serializeJson(storageDoc, actionDataStr);
  
  // 检查数据大小（NVS单个键值对建议不超过1900字节）
  if (actionDataStr.length() > 1900) {
    Serial.printf("Action data too large: %d bytes (max 1900)\n", actionDataStr.length());
    preferences.end();
    return false;
  }
  
  // 存储到NVS
  size_t written = preferences.putString(actionName.c_str(), actionDataStr);
  if (written == 0) {
    Serial.printf("Failed to save action '%s' to NVS (putString returned 0)\n", actionName.c_str());
    Serial.printf("Action data length: %d bytes\n", actionDataStr.length());
    Serial.printf("Action data: %s\n", actionDataStr.c_str());
    preferences.end();
    return false;
  }
  
  Serial.printf("Action data saved successfully: %d bytes written\n", written);
  
  // 更新action列表
  String actionListJson = preferences.getString("action_list", "[]");
  DynamicJsonDocument actionListDoc(1024);
  deserializeJson(actionListDoc, actionListJson);
  JsonArray actionArray = actionListDoc.as<JsonArray>();

  bool found = false;
  for (JsonVariant v : actionArray) {
      if (v.as<String>() == actionName) {
          found = true;
          break;
      }
  }

  // 如果不存在，添加到列表
  if (!found) {
      actionArray.add(actionName);
      String newActionListJson;
      serializeJson(actionArray, newActionListJson);
      size_t listWritten = preferences.putString("action_list", newActionListJson);
      if (listWritten == 0) {
          Serial.println("Warning: Failed to update action list in NVS");
      } else {
          Serial.printf("Action list updated successfully: %s (%d bytes)\n", newActionListJson.c_str(), listWritten);
      }
  } else {
      Serial.printf("Action '%s' already exists in action_list\n", actionName.c_str());
  }
  
  preferences.end();
  Serial.printf("=== saveActionToNVS END: SUCCESS ===\n");
  Serial.printf("Action '%s' saved to NVS successfully (%d bytes)\n", actionName.c_str(), written);
  return true;
}

// 列出所有保存的Actions
void listActionsFromNVS() {
  preferences.begin("actions", true);
  
  String actionList = preferences.getString("action_list", "");
  if (actionList.length() == 0) {
    Serial.println("No actions found in NVS.");
    preferences.end();
    return;
  }
  
  Serial.println("\n=== Saved Actions in NVS ===");
  
  int actionCount = 0;
  
  // 检查是否是JSON格式的action_list
  if (actionList.startsWith("[") && actionList.endsWith("]")) {
    DynamicJsonDocument actionListDoc(1024);
    DeserializationError error = deserializeJson(actionListDoc, actionList);
    
    if (!error) {
      JsonArray actionArray = actionListDoc.as<JsonArray>();
      
      // 遍历JSON数组中的每个action名称
      for (JsonVariant actionVar : actionArray) {
        String actionName = actionVar.as<String>();
        if (actionName.length() > 0) {
          actionCount++;
          String actionData = preferences.getString(actionName.c_str(), "");
          
          if (actionData.length() > 0) {
            DynamicJsonDocument doc(2048);
            DeserializationError actionError = deserializeJson(doc, actionData);
            
            if (!actionError) {
              Serial.printf("%d. Action: %s\n", actionCount, actionName.c_str());
              Serial.printf("   Timestamp: %lu\n", doc["timestamp"].as<unsigned long>());
              
              if (doc.containsKey("payload") && doc["payload"].containsKey("message")) {
                JsonObject message = doc["payload"]["message"];
                if (message.containsKey("payload")) {
                  JsonArray devices = message["payload"];
                  Serial.printf("   Devices: %d\n", devices.size());
                  
                  for (JsonObject device : devices) {
                    Serial.printf("     - %s (%s): %s\n", 
                      device["name"].as<String>().c_str(),
                      device["mac"].as<String>().c_str(),
                      device["command"].as<String>().c_str());
                  }
                }
              }
              Serial.printf("   Data size: %d bytes\n", actionData.length());
            } else {
              Serial.printf("%d. Action: %s (corrupted data)\n", actionCount, actionName.c_str());
            }
          } else {
            Serial.printf("%d. Action: %s (no data found)\n", actionCount, actionName.c_str());
          }
          Serial.println();
        }
      }
    } else {
      Serial.println("Failed to parse action_list JSON: " + String(error.c_str()));
    }
  } else {
    // 兼容旧的CSV格式
    int startIndex = 0;
    int commaIndex = actionList.indexOf(',');
    
    while (startIndex < actionList.length()) {
      String actionName;
      if (commaIndex == -1) {
        actionName = actionList.substring(startIndex);
        startIndex = actionList.length();
      } else {
        actionName = actionList.substring(startIndex, commaIndex);
        startIndex = commaIndex + 1;
        commaIndex = actionList.indexOf(',', startIndex);
      }
      
      if (actionName.length() > 0) {
        actionCount++;
        String actionData = preferences.getString(actionName.c_str(), "");
        
        if (actionData.length() > 0) {
          DynamicJsonDocument doc(2048);
          DeserializationError error = deserializeJson(doc, actionData);
          
          if (!error) {
            Serial.printf("%d. Action: %s\n", actionCount, actionName.c_str());
            Serial.printf("   Timestamp: %lu\n", doc["timestamp"].as<unsigned long>());
            
            if (doc.containsKey("payload") && doc["payload"].containsKey("message")) {
              JsonObject message = doc["payload"]["message"];
              if (message.containsKey("payload")) {
                JsonArray devices = message["payload"];
                Serial.printf("   Devices: %d\n", devices.size());
                
                for (JsonObject device : devices) {
                  Serial.printf("     - %s (%s): %s\n", 
                    device["name"].as<String>().c_str(),
                    device["mac"].as<String>().c_str(),
                    device["command"].as<String>().c_str());
                }
              }
            }
            Serial.printf("   Data size: %d bytes\n", actionData.length());
          } else {
            Serial.printf("%d. Action: %s (corrupted data)\n", actionCount, actionName.c_str());
          }
        } else {
          Serial.printf("%d. Action: %s (no data found)\n", actionCount, actionName.c_str());
        }
        Serial.println();
      }
    }
  }
  
  Serial.printf("Total actions in NVS: %d\n", actionCount);
  preferences.end();
}

// 从NVS获取指定Action的数据
String getActionFromNVS(const String& actionName) {
  if (actionName.length() == 0) {
    return "";
  }
  
  preferences.begin("actions", true);
  String actionData = preferences.getString(actionName.c_str(), "");
  preferences.end();
  
  return actionData;
}

// 从NVS删除指定Action
bool deleteActionFromNVS(const String& actionName) {
  if (actionName.length() == 0) {
    return false;
  }
  
  preferences.begin("actions", false);
  
  // 检查action是否存在
  String actionData = preferences.getString(actionName.c_str(), "");
  bool actionExists = (actionData.length() > 0);
  if (!actionExists) {
    Serial.printf("Action '%s' not found in NVS\n", actionName.c_str());
    // 即使动作数据不存在，也继续处理以从列表中移除该动作名称
  }
  
  // 从action列表中移除
  String actionList = preferences.getString("action_list", "");
  
  if (actionList.length() > 0) {
    // 检查是否是JSON格式
    if (actionList.startsWith("[") && actionList.endsWith("]")) {
      // JSON格式处理
      DynamicJsonDocument actionListDoc(1024);
      DeserializationError error = deserializeJson(actionListDoc, actionList);
      
      if (!error) {
        JsonArray actionArray = actionListDoc.as<JsonArray>();
        DynamicJsonDocument newDoc(1024);
        JsonArray newArray = newDoc.to<JsonArray>();
        
        // 复制除了要删除的动作之外的所有动作
        for (JsonVariant actionVar : actionArray) {
          String existingAction = actionVar.as<String>();
          if (existingAction != actionName && existingAction.length() > 0) {
            newArray.add(existingAction);
          }
        }
        
        String newList;
        serializeJson(newArray, newList);
        preferences.putString("action_list", newList);
        Serial.printf("Updated action_list (JSON): %s\n", newList.c_str());
      } else {
        Serial.println("Failed to parse action_list JSON for deletion");
      }
    } else {
      // CSV格式处理（向后兼容）
      String newList = "";
      int startIndex = 0;
      int commaIndex = actionList.indexOf(',');
      
      while (startIndex < actionList.length()) {
        String existingAction;
        if (commaIndex == -1) {
          existingAction = actionList.substring(startIndex);
          startIndex = actionList.length();
        } else {
          existingAction = actionList.substring(startIndex, commaIndex);
          startIndex = commaIndex + 1;
          commaIndex = actionList.indexOf(',', startIndex);
        }
        
        if (existingAction != actionName && existingAction.length() > 0) {
          if (newList.length() > 0) {
            newList += ",";
          }
          newList += existingAction;
        }
      }
      preferences.putString("action_list", newList);
      Serial.printf("Updated action_list (CSV): %s\n", newList.c_str());
    }
  } else {
    Serial.println("action_list is empty, nothing to remove");
  }
  
  // 更新action列表
  // preferences.putString("action_list", newList); // 已在上面处理
  
  // 删除action数据
  bool removed = true;
  if (actionExists) {
    removed = preferences.remove(actionName.c_str());
  }
  preferences.end();
  
  if (removed) {
    Serial.printf("Action '%s' deleted from NVS successfully\n", actionName.c_str());
    
    // 发送HA取消发现消息（空payload到相同主题）
    unpublishActionDiscovery(actionName);
    Serial.printf("HA discovery cancellation message sent for action: %s\n", actionName.c_str());
  } else {
    Serial.printf("Failed to delete action '%s' from NVS\n", actionName.c_str());
  }
  
  return removed;
}

// 重建action_list以包含所有现有动作
bool rebuildActionList() {
  preferences.begin("actions", false);
  
  Serial.println("=== 重建action_list ===");
  
  // 获取所有可能的键
  DynamicJsonDocument newDoc(1024);
  JsonArray newArray = newDoc.to<JsonArray>();
  
  // 检查常见的动作名称（数字1-50）
  for (int i = 1; i <= 50; i++) {
    String actionName = String(i);
    String actionData = preferences.getString(actionName.c_str(), "");
    if (actionData.length() > 0) {
      // 验证是否为有效的JSON
      DynamicJsonDocument testDoc(2048);
      DeserializationError error = deserializeJson(testDoc, actionData);
      if (!error) {
        newArray.add(actionName);
        Serial.printf("找到有效动作: %s\n", actionName.c_str());
      }
    }
  }
  
  // 检查一些常见的字符串动作名称
  String commonNames[] = {"test", "socket1", "BedroomSocket", "LivingRoom", "Kitchen", "Bathroom"};
  for (int i = 0; i < 6; i++) {
    String actionData = preferences.getString(commonNames[i].c_str(), "");
    if (actionData.length() > 0) {
      DynamicJsonDocument testDoc(2048);
      DeserializationError error = deserializeJson(testDoc, actionData);
      if (!error) {
        newArray.add(commonNames[i]);
        Serial.printf("找到有效动作: %s\n", commonNames[i].c_str());
      }
    }
  }
  
  // 序列化新的action_list
  String newActionList;
  serializeJson(newArray, newActionList);
  
  // 保存到NVS
  size_t written = preferences.putString("action_list", newActionList);
  preferences.end();
  
  if (written > 0) {
    Serial.printf("成功重建action_list: %s\n", newActionList.c_str());
    Serial.printf("包含 %d 个动作\n", newArray.size());
    return true;
  } else {
    Serial.println("重建action_list失败");
    return false;
  }
}

void listServoDevicesFromNVS() {
  preferences.begin("servo_info", true);
  
  String servoList = preferences.getString("servo_list", "");
  if (servoList.length() == 0) {
    Serial.println("No servo devices found in NVS.");
    preferences.end();
    return;
  }
  
  Serial.println("\n=== Servo Devices in NVS ===");
  
  // 分割舵机列表
  int startIndex = 0;
  int commaIndex = servoList.indexOf(',');
  
  while (startIndex < servoList.length()) {
    String key;
    if (commaIndex == -1) {
      key = servoList.substring(startIndex);
    } else {
      key = servoList.substring(startIndex, commaIndex);
    }
    
    String servoData = preferences.getString(key.c_str(), "");
    if (servoData.length() > 0) {
      StaticJsonDocument<1024> doc;
      if (deserializeJson(doc, servoData) == DeserializationError::Ok) {
        Serial.printf("Name: %s\n", doc["name"].as<const char*>());
        Serial.printf("MAC: %s\n", doc["mac"].as<const char*>());
        Serial.printf("NodeID: %u\n", doc["nodeId"].as<uint32_t>());
        Serial.printf("Timestamp: %lu\n", doc["timestamp"].as<unsigned long>());
        
        if (doc.containsKey("saved_actions")) {
          JsonArray actions = doc["saved_actions"];
          Serial.printf("Saved Actions (%d):\n", actions.size());
          for (JsonObject action : actions) {
            Serial.printf("  - %s (type: %s)\n", 
                         action["name"].as<const char*>(), 
                         action["type"].as<const char*>());
          }
        }
        Serial.println("---");
      }
    }
    
    if (commaIndex == -1) break;
    startIndex = commaIndex + 1;
    commaIndex = servoList.indexOf(',', startIndex);
  }
  
  preferences.end();
}

void handleApiServoActions(AsyncWebServerRequest *request) {
  addCorsHeaders(request);
  
  String macAddress = request->getParam("mac", true)->value();
  if (macAddress.isEmpty()) {
    sendCorsResponse(request, 400, "application/json", "{\"error\":\"Missing mac parameter\"}");
    return;
  }
  
  preferences.begin("servo_info", true);
  String key = macToKey(macAddress);
  String servoData = preferences.getString(key.c_str(), "");
  preferences.end();
  
  if (servoData.isEmpty()) {
    sendCorsResponse(request, 404, "application/json", "{\"error\":\"Servo not found\"}");
    return;
  }
  
  StaticJsonDocument<1024> doc;
  DeserializationError error = deserializeJson(doc, servoData);
  if (error) {
    sendCorsResponse(request, 500, "application/json", "{\"error\":\"Failed to parse servo data\"}");
    return;
  }
  
  // 构建响应JSON
  DynamicJsonDocument response(2048);
  response["name"] = doc["name"];
  response["mac"] = doc["mac"];
  response["nodeId"] = doc["nodeId"];
  response["timestamp"] = doc["timestamp"];
  
  if (doc.containsKey("saved_actions")) {
    response["saved_actions"] = doc["saved_actions"];
  } else if (doc.containsKey("action_count")) {
    // 处理简化存储的情况
    response["action_count"] = doc["action_count"];
    if (doc.containsKey("action_names")) {
      response["action_names"] = doc["action_names"];
    }
  } else {
    JsonArray emptyActions = response.createNestedArray("saved_actions");
  }
  
  String responseStr;
  serializeJson(response, responseStr);
  sendCorsResponse(request, 200, "application/json", responseStr);
}

void handleApiDeleteServoAction(AsyncWebServerRequest *request) {
  addCorsHeaders(request);
  
  if (!request->hasParam("plain", true)) {
    sendCorsResponse(request, 400, "application/json", "{\"error\":\"No data provided\"}");
    return;
  }
  
  String body = request->getParam("plain", true)->value();
  DynamicJsonDocument doc(512);
  DeserializationError error = deserializeJson(doc, body);
  
  if (error) {
    sendCorsResponse(request, 400, "application/json", "{\"error\":\"Invalid JSON\"}");
    return;
  }
  
  String macAddress = doc["mac"];
  int actionIndex = doc["index"];
  
  if (macAddress.isEmpty()) {
    sendCorsResponse(request, 400, "application/json", "{\"error\":\"Missing mac address\"}");
    return;
  }
  
  // 查找对应的舵机设备
  MeshDevice* targetDevice = nullptr;
  for (auto& pair : knownDevicesByMac) {
    if (pair.second.macAddress == macAddress && pair.second.type == "ServoController") {
      targetDevice = &pair.second;
      break;
    }
  }
  
  if (!targetDevice) {
    sendCorsResponse(request, 404, "application/json", "{\"error\":\"Servo device not found\"}");
    return;
  }
  
  // 构建发送到mesh网络的删除命令
  StaticJsonDocument<256> meshCommand;
  meshCommand["to"] = targetDevice->nodeId;
  meshCommand["cmd"] = "deleteAction";
  
  JsonObject payload = meshCommand.createNestedObject("payload");
  payload["index"] = actionIndex;
  
  // 在发送删除命令之前，先获取要删除的动作名称用于HA实体删除
  String actionNameToDelete = "";
  preferences.begin("servo_info", true);
  String servoKey = macToKey(macAddress);
  String servoInfoStr = preferences.getString(servoKey.c_str(), "");
  preferences.end();
  
  if (servoInfoStr.length() > 0) {
    StaticJsonDocument<2048> servoDoc;
    DeserializationError parseError = deserializeJson(servoDoc, servoInfoStr);
    if (!parseError && servoDoc.containsKey("saved_actions")) {
      JsonArray actions = servoDoc["saved_actions"];
      if (actionIndex >= 0 && actionIndex < actions.size()) {
        actionNameToDelete = actions[actionIndex]["name"].as<String>();
      }
    }
  }
  
  // 发送到mesh网络
  String meshCommandStr;
  serializeJson(meshCommand, meshCommandStr);
  mesh.sendSingle(targetDevice->nodeId, meshCommandStr);
  
  Serial.printf("发送删除命令到舵机 %s (NodeID: %u): %s\n", 
                targetDevice->name.c_str(), targetDevice->nodeId, meshCommandStr.c_str());
  
  // 立即从HA中删除对应的按钮实体
  if (actionNameToDelete.length() > 0) {
    unpublishServoActionDiscovery(targetDevice->name, actionNameToDelete);
  }
  
  sendCorsResponse(request, 200, "application/json", "{\"success\":true,\"message\":\"删除命令已发送到舵机设备\"}");
}

void handleSerialCommands() {
  if (Serial.available()) {
    String command = Serial.readStringUntil('\n');
    command.trim();
    
    if (command.equalsIgnoreCase("list_servos") || command.equalsIgnoreCase("ls")) {
      listServoDevicesFromNVS();
    }
    else if (command.equalsIgnoreCase("list_leds") || command.equalsIgnoreCase("ll")) {
      listLedDevicesFromNVS();
    }
    else if (command.equalsIgnoreCase("list_actions") || command.equalsIgnoreCase("la")) {
      listActionsFromNVS();
    }
    else if (command.startsWith("get_action ")) {
      String actionName = command.substring(11);
      actionName.trim();
      if (actionName.length() > 0) {
        String actionData = getActionFromNVS(actionName);
        if (actionData.length() > 0) {
          Serial.printf("Action '%s' data:\n%s\n", actionName.c_str(), actionData.c_str());
        } else {
          Serial.printf("Action '%s' not found in NVS\n", actionName.c_str());
        }
      } else {
        Serial.println("Usage: get_action <action_name>");
      }
    }
    else if (command.startsWith("delete_action ")) {
      String actionName = command.substring(14);
      actionName.trim();
      if (actionName.length() > 0) {
        bool deleted = deleteActionFromNVS(actionName);
        if (deleted) {
          Serial.printf("Action '%s' deleted successfully\n", actionName.c_str());
        } else {
          Serial.printf("Failed to delete action '%s'\n", actionName.c_str());
        }
      } else {
        Serial.println("Usage: delete_action <action_name>");
      }
    }
    else if (command.equalsIgnoreCase("clear_servos")) {
      preferences.begin("servo_info", false);
      if (preferences.clear()) {
        Serial.println("All servo info cleared from NVS.");
      } else {
        Serial.println("Failed to clear servo info from NVS.");
      }
      preferences.end();
    }
    else if (command.equalsIgnoreCase("clear_actions")) {
      preferences.begin("actions", false);
      if (preferences.clear()) {
        Serial.println("All actions cleared from NVS.");
      } else {
        Serial.println("Failed to clear actions from NVS.");
      }
      preferences.end();
    }
    else if (command.equalsIgnoreCase("clean_orphaned_actions")) {
      Serial.println("Cleaning orphaned actions from NVS...");
      
      // Get action list from actions namespace
      preferences.begin("actions", false);
      String actionList = preferences.getString("action_list", "");
      
      if (actionList.length() > 0) {
        char action_list_cstr[actionList.length() + 1]; 
        strcpy(action_list_cstr, actionList.c_str());
        char* token = strtok(action_list_cstr, ",");
        int removedCount = 0;
        String validActions = "";
        
        while (token != NULL) {
          String actionName = String(token);
          // Check if this action data exists in NVS
          String actionData = preferences.getString(actionName.c_str(), "");
          if (actionData.length() > 0) {
            // Action data exists, keep it in the list
            if (validActions.length() > 0) validActions += ",";
            validActions += actionName;
          } else {
            // Action data missing, remove from list
            Serial.printf("  - Removed orphaned action reference: %s\n", actionName.c_str());
            removedCount++;
          }
          token = strtok(NULL, ",");
        }
        
        // Update action_list with only valid actions
        preferences.putString("action_list", validActions);
        Serial.printf("Cleaned %d orphaned action references. Updated action_list.\n", removedCount);
      } else {
        Serial.println("No actions found in actions namespace.");
      }
      preferences.end();
    }
    else if (command.equalsIgnoreCase("rebuild_action_list")) {
      Serial.println("重建action_list...");
      if (rebuildActionList()) {
        Serial.println("action_list重建成功！");
      } else {
        Serial.println("action_list重建失败！");
      }
    }
    else if (command.equalsIgnoreCase("help")) {
      Serial.println("\nAvailable commands:");
      Serial.println("  list_servos (or ls) - List all servo devices in NVS");
      Serial.println("  list_leds (or ll) - List all LED devices and their configurations in NVS");
      Serial.println("  list_actions (or la) - List all saved actions in NVS");
      Serial.println("  get_action <name> - Get specific action data from NVS");
      Serial.println("  delete_action <name> - Delete specific action from NVS");
      Serial.println("  clear_servos - Clear all servo info from NVS");
      Serial.println("  clear_actions - Clear all actions from NVS");
      Serial.println("  clean_orphaned_actions - Remove orphaned actions not in action_list");
      Serial.println("  rebuild_action_list - Rebuild action_list from existing action data");
      Serial.println("  help - Show this help message");
    }
  }
}

// =================================================================================
// WebSocket Functions
// =================================================================================

void onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT:
      Serial.printf("WebSocket client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
      break;
      
    case WS_EVT_DISCONNECT:
      Serial.printf("WebSocket client #%u disconnected\n", client->id());
      break;
      
    case WS_EVT_DATA:
      {
        AwsFrameInfo *info = (AwsFrameInfo*)arg;
        if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
          data[len] = 0;
          String message = (char*)data;
          Serial.printf("WebSocket message received: %s\n", message.c_str());
          
          // Parse and handle WebSocket commands
          DynamicJsonDocument doc(512);
          DeserializationError error = deserializeJson(doc, message);
          if (!error) {
            String type = doc.containsKey("type") ? doc["type"].as<String>() : doc["command"].as<String>();
            if (type == "ping") {
              client->text("{\"type\":\"pong\"}");
            } else if (type == "get_devices") {
              // 发送设备列表
              DynamicJsonDocument devicesDoc(4096);
              devicesDoc["type"] = "devices_list";
              JsonArray devices = devicesDoc.createNestedArray("devices");

              for (auto const& [mac, device] : knownDevicesByMac) {
                JsonObject devObj = devices.createNestedObject();
                
                // 添加所有设备共有的基础信息
                devObj["nodeId"] = device.nodeId;
                devObj["name"] = device.name;
                devObj["macAddress"] = device.macAddress;
                devObj["type"] = device.type;
                devObj["lastSeenTimestamp"] = device.lastSeenTimestamp;
                devObj["isOnline"] = device.isOnline;
                devObj["hasBeenDiscovered"] = device.hasBeenDiscovered;
                
                // 如果设备是 MultiSwitch, 添加详细的 "config" 对象
                if (device.type == "MultiSwitch") {
                  JsonObject config = devObj.createNestedObject("config");
                  
                  // 准备用于 JSON 的颜色和亮度值
                  char on_color_hex[8];
                  char off_color_hex[8];
                  snprintf(on_color_hex, sizeof(on_color_hex), "#%02x%02x%02x", device.on_r, device.on_g, device.on_b);
                  snprintf(off_color_hex, sizeof(off_color_hex), "#%02x%02x%02x", device.off_r, device.off_g, device.off_b);
                  
                  // 使用已保存的默认值（如果存在），否则使用设备上报的当前值
                  uint8_t on_brightness = device.default_on_value > 0 ? device.default_on_value : device.brightness_on;
                  uint8_t off_brightness = device.default_off_value > 0 ? device.default_off_value : device.brightness_off;
                  
                  // 将所有配置信息添加到 "config" 对象中
                  config["on_color"] = on_color_hex;
                  config["brightness_on"] = on_brightness;
                  config["off_color"] = off_color_hex;
                  config["brightness_off"] = off_brightness;
                  config["linkedSensor"] = device.linkedEnvSensorName;
                  config["daylightThreshold"] = device.daylightThresholdLx;
                  
                  // 添加 ledConfig 对象以保持前端兼容性
                  JsonObject ledConfig = devObj.createNestedObject("ledConfig");
                  
                  // 添加RGB格式的颜色对象（前端期望的格式）
                  JsonObject onColor = ledConfig.createNestedObject("onColor");
                  onColor["r"] = device.on_r;
                  onColor["g"] = device.on_g;
                  onColor["b"] = device.on_b;
                  
                  JsonObject offColor = ledConfig.createNestedObject("offColor");
                  offColor["r"] = device.off_r;
                  offColor["g"] = device.off_g;
                  offColor["b"] = device.off_b;
                  
                  // 添加亮度和其他配置
                  ledConfig["brightnessOn"] = on_brightness;
    ledConfig["brightnessOff"] = off_brightness;
                  ledConfig["linkedSensorName"] = device.linkedEnvSensorName;
                  ledConfig["daylightThresholdLx"] = device.daylightThresholdLx;
                  ledConfig["isDaytime"] = device.isDaytime;
                }
                // 如果设备是 ServoController, 添加servo配置对象
                else if (device.type == "ServoController") {
                  JsonObject config = devObj.createNestedObject("config");
                  
                  // 从存储的字段中恢复servo参数
                  int sweepAngle = device.default_on_value > 0 ? device.default_on_value : 30;
                  int actionDelay = device.default_off_value > 0 ? device.default_off_value * 10 : 500;
                  
                  config["sweep_angle"] = sweepAngle;
                  config["action_delay"] = actionDelay;
                }
              }

              String response;
              serializeJson(devicesDoc, response);
              client->text(response);
            } else if (type == "delete_device") {
              String deviceName = doc["deviceName"];
              String macAddress = doc["macAddress"];
              
              if (deviceName.length() > 0 && macAddress.length() > 0) {
                // 检查设备是否存在
                if (nameToMacMap.count(deviceName) && knownDevicesByMac.count(macAddress)) {
                  // 保存设备信息用于发布删除消息
                  MeshDevice deviceToDelete = knownDevicesByMac[macAddress];
                  
                  // 发布MQTT消息通知HA删除设备实体
                  if (mqttClient.connected()) {
                    publishDeviceDeletion(deviceToDelete);
                  }
                  
                  // 从内存中删除设备
                  knownDevicesByMac.erase(macAddress);
                  nameToMacMap.erase(deviceName);
                  
                  // 清理nodeIdToMacMap映射表
                  if (deviceToDelete.nodeId != 0 && nodeIdToMacMap.count(deviceToDelete.nodeId)) {
                    nodeIdToMacMap.erase(deviceToDelete.nodeId);
                  }
                  
                  // 从NVS中删除设备信息
                  preferences.begin("devices", false);
                  preferences.clear();
                  
                  // 重新保存剩余的设备
                  int deviceIndex = 0;
                  for (auto& pair : knownDevicesByMac) {
                    MeshDevice& device = pair.second;
                    String key = "device_" + String(deviceIndex);
                    
                    StaticJsonDocument<512> deviceDoc;
                    deviceDoc["nodeId"] = device.nodeId;
                    deviceDoc["name"] = device.name;
                    deviceDoc["type"] = device.type;
                    deviceDoc["macAddress"] = device.macAddress;
                    deviceDoc["lastSeen"] = device.lastSeenTimestamp;
                    deviceDoc["isOnline"] = device.isOnline;
                    
                    if (device.type == "MultiSwitch") {
                      deviceDoc["on_r"] = device.on_r;
                      deviceDoc["on_g"] = device.on_g;
                      deviceDoc["on_b"] = device.on_b;
                      deviceDoc["off_r"] = device.off_r;
                      deviceDoc["off_g"] = device.off_g;
                      deviceDoc["off_b"] = device.off_b;
                      deviceDoc["brightness_on"] = device.brightness_on;
                      deviceDoc["brightness_off"] = device.brightness_off;
                      deviceDoc["linkedEnvSensorName"] = device.linkedEnvSensorName;
                      deviceDoc["daylightThresholdLx"] = device.daylightThresholdLx;
                    } else if (device.type == "ServoController") {
                      deviceDoc["default_on_value"] = device.default_on_value;
                      deviceDoc["default_off_value"] = device.default_off_value;
                    }
                    
                    String deviceJson;
                    serializeJson(deviceDoc, deviceJson);
                    preferences.putString(key.c_str(), deviceJson);
                    deviceIndex++;
                  }
                  
                  preferences.putInt("deviceCount", deviceIndex);
                  preferences.end();
                  
                  Serial.printf("Deleted device via WebSocket: %s (%s)\n", deviceName.c_str(), macAddress.c_str());
                  
                  // 发送成功响应
                  DynamicJsonDocument response(256);
                  response["type"] = "delete_device_response";
                  response["status"] = "success";
                  response["message"] = "Device deleted successfully";
                  response["deviceName"] = deviceName;
                  response["macAddress"] = macAddress;
                  String responseStr;
                  serializeJson(response, responseStr);
                  client->text(responseStr);
                  
                  // 广播设备删除事件
                  DynamicJsonDocument broadcast(256);
                  broadcast["type"] = "device_deleted";
                  broadcast["deviceName"] = deviceName;
                  broadcast["macAddress"] = macAddress;
                  String broadcastStr;
                  serializeJson(broadcast, broadcastStr);
                  broadcastWebSocketMessage(broadcastStr);
                  
                } else {
                  // 发送错误响应
                  DynamicJsonDocument response(256);
                  response["type"] = "delete_device_response";
                  response["status"] = "error";
                  response["message"] = "Device not found";
                  response["deviceName"] = deviceName;
                  response["macAddress"] = macAddress;
                  String responseStr;
                  serializeJson(response, responseStr);
                  client->text(responseStr);
                }
              } else {
                // 发送错误响应
                DynamicJsonDocument response(256);
                response["type"] = "delete_device_response";
                response["status"] = "error";
                response["message"] = "Missing deviceName or macAddress";
                String responseStr;
                serializeJson(response, responseStr);
                client->text(responseStr);
              }
            } else if (type == "deleteAction") {
              // 处理deleteAction命令
              Serial.printf("Received deleteAction command: %s\n", message.c_str());
              
              // 检查是否包含actionName
              String actionName = "";
              if (doc.containsKey("actionName")) {
                actionName = doc["actionName"].as<String>();
                
                // 从NVS中删除action
                bool deleted = deleteActionFromNVS(actionName);
                
                // 发送响应
                DynamicJsonDocument response(256);
                response["type"] = "deleteAction_response";
                response["status"] = deleted ? "success" : "error";
                response["message"] = deleted ? "Action deleted successfully" : "Failed to delete action";
                response["actionName"] = actionName;
                
                String responseStr;
                serializeJson(response, responseStr);
                client->text(responseStr);
                
                // 如果删除成功，广播通知所有客户端
                if (deleted) {
                  DynamicJsonDocument broadcast(256);
                  broadcast["type"] = "action_deleted";
                  broadcast["actionName"] = actionName;
                  String broadcastStr;
                  serializeJson(broadcast, broadcastStr);
                  broadcastWebSocketMessage(broadcastStr);
                }
                
                Serial.printf("Delete action result: %s\n", deleted ? "success" : "failed");
              } else {
                // 发送错误响应
                DynamicJsonDocument response(256);
                response["type"] = "deleteAction_response";
                response["status"] = "error";
                response["message"] = "Missing actionName";
                String responseStr;
                serializeJson(response, responseStr);
                client->text(responseStr);
              }
            } else if (type == "saveAction") {
              // 处理saveAction命令
              Serial.printf("Received saveAction command: %s\n", message.c_str());
              
              // 检查是否包含actionName
              String actionName = "";
              if (doc.containsKey("actionName")) {
                actionName = doc["actionName"].as<String>();
              }
              
              // 检查payload结构
              if (doc.containsKey("payload")) {
                JsonObject payload = doc["payload"];
                if (payload.containsKey("message")) {
                  JsonObject messageObj = payload["message"];
                  if (messageObj.containsKey("cmd") && messageObj["cmd"] == "groupState") {
                    // 提取并广播groupState命令
                    JsonObject groupStatePayload = messageObj["payload"];
                    String command;
                    serializeJson(groupStatePayload, command);
                    Serial.printf("Broadcasting saveAction group state command: %s\n", command.c_str());
                    mesh.sendBroadcast(command);
                    
                    // 保存action到NVS (如果提供了actionName)
                    bool savedToNVS = false;
                    if (actionName.length() > 0) {
                      savedToNVS = saveActionToNVS(actionName, doc);
                      if (savedToNVS) {
                        Serial.printf("Action '%s' saved to NVS successfully\n", actionName.c_str());
                        // 刷新内存中的动作并发布HA自动发现
                        loadActions();
                        publishActionDiscovery();
                      } else {
                        Serial.printf("Failed to save action '%s' to NVS\n", actionName.c_str());
                      }
                    }
                    
                    // 发送成功响应
                    DynamicJsonDocument response(512);
                    response["type"] = "saveAction_response";
                    response["status"] = "success";
                    response["message"] = "SaveAction command processed and broadcasted";
                    if (actionName.length() > 0) {
                      response["actionName"] = actionName;
                      response["savedToNVS"] = savedToNVS;
                    }
                    String responseStr;
                    serializeJson(response, responseStr);
                    client->text(responseStr);
                  } else {
                    Serial.println("saveAction command: invalid message cmd");
                    // 发送错误响应
                    DynamicJsonDocument response(256);
                    response["type"] = "saveAction_response";
                    response["status"] = "error";
                    response["message"] = "Invalid message cmd, expected groupState";
                    String responseStr;
                    serializeJson(response, responseStr);
                    client->text(responseStr);
                  }
                } else {
                  Serial.println("saveAction command: missing message in payload");
                  // 发送错误响应
                  DynamicJsonDocument response(256);
                  response["type"] = "saveAction_response";
                  response["status"] = "error";
                  response["message"] = "Missing message in payload";
                  String responseStr;
                  serializeJson(response, responseStr);
                  client->text(responseStr);
                }
              } else {
                Serial.println("saveAction command: missing payload");
                // 发送错误响应
                DynamicJsonDocument response(256);
                response["type"] = "saveAction_response";
                response["status"] = "error";
                response["message"] = "Missing payload";
                String responseStr;
                serializeJson(response, responseStr);
                client->text(responseStr);
              }
            } else if (type == "rediscovery") {
              String deviceName = doc["deviceName"];
              if (deviceName.length() > 0 && nameToMacMap.count(deviceName)) {
                String deviceMac = nameToMacMap[deviceName];
                MeshDevice& device = knownDevicesByMac[deviceMac];
                device.hasBeenDiscovered = false;
                saveToAddressBook(device);
                
                // Send WebSocket update for discovery status reset
                sendDeviceStatusUpdate(deviceMac, "rediscovery_initiated");
                
                // Print all device discovery status after rediscovery
                printAllDeviceDiscoveryStatus();
                
                // Send success response
                DynamicJsonDocument response(256);
                response["type"] = "rediscovery_response";
                response["status"] = "success";
                response["message"] = "Rediscovery initiated";
                response["deviceName"] = deviceName;
                String responseStr;
                serializeJson(response, responseStr);
                client->text(responseStr);
              } else {
                // Send error response
                DynamicJsonDocument response(256);
                response["type"] = "rediscovery_response";
                response["status"] = "error";
                response["message"] = "Device not found";
                response["deviceName"] = deviceName;
                String responseStr;
                serializeJson(response, responseStr);
                client->text(responseStr);
              }
            } else if (type == "mesh_command") {
              String macAddress = doc["mac"];
              JsonObject payload = doc["payload"];

              if (macAddress.length() > 0 && !payload.isNull()) {
                if (knownDevicesByMac.count(macAddress)) {
                  uint32_t dest = knownDevicesByMac[macAddress].nodeId;
                  
                  // Create the command structure that the switch expects
                  DynamicJsonDocument meshCommand(1024);
                  meshCommand["cmd"] = payload["cmd"];
                  meshCommand["to"] = dest;
                  meshCommand["payload"] = payload["payload"];
                  
                  String command;
                  serializeJson(meshCommand, command);
                  mesh.sendSingle(dest, command);
                  Serial.printf("Forwarded mesh command to %s (%u): %s\n", macAddress.c_str(), dest, command.c_str());

                  // Send success response
                  DynamicJsonDocument response(256);
                  response["type"] = "mesh_command_response";
                  response["status"] = "success";
                  response["message"] = "Command forwarded to mesh network";
                  String responseStr;
                  serializeJson(response, responseStr);
                  client->text(responseStr);
                } else {
                  // Send error response
                  DynamicJsonDocument response(256);
                  response["type"] = "mesh_command_response";
                  response["status"] = "error";
                  response["message"] = "Device not found for MAC address";
                  response["mac"] = macAddress;
                  String responseStr;
                  serializeJson(response, responseStr);
                  client->text(responseStr);
                }
              } else {
                // Send error response
                DynamicJsonDocument response(256);
                response["type"] = "mesh_command_response";
                response["status"] = "error";
                response["message"] = "Missing mac or payload";
                String responseStr;
                serializeJson(response, responseStr);
                client->text(responseStr);
              }
            } else if (type == "sendMessage") {
              JsonObject payload = doc["payload"];
              if (payload["isBroadcast"]) {
                String command;
                serializeJson(payload["message"], command);
                Serial.printf("Broadcasting command: %s\n", command.c_str());
                mesh.sendBroadcast(command);
              } else {
                uint32_t dest = payload["dest"];
                String command = payload["message"];
                mesh.sendSingle(dest, command);
              }
            } else if (type == "groupState") {
              JsonObject payload = doc["payload"];
              if (!payload.isNull()) {
                String command;
                serializeJson(payload, command);
                Serial.printf("Broadcasting group state command: %s\n", command.c_str());
                mesh.sendBroadcast(command);
                Serial.printf("Broadcasted group state command: %s\n", command.c_str());

                // Send success response
                DynamicJsonDocument response(256);
                response["type"] = "groupState_response";
                response["status"] = "success";
                response["message"] = "Group state command broadcasted";
                String responseStr;
                serializeJson(response, responseStr);
                client->text(responseStr);
              } else {
                Serial.println("groupState command missing payload");
                // Send error response
                DynamicJsonDocument response(256);
                response["type"] = "groupState_response";
                response["status"] = "error";
                response["message"] = "Missing payload";
                String responseStr;
                serializeJson(response, responseStr);
                client->text(responseStr);
              }
            }
            // Add more command handlers here as needed
          }
        }
      }
      break;
      
    case WS_EVT_PONG:
    case WS_EVT_ERROR:
      break;
  }
}

void broadcastWebSocketMessage(const String& message) {
  ws.textAll(message);
  Serial.printf("WebSocket broadcast: %s\n", message.c_str());
}

void sendDeviceStatusUpdate(const String& deviceMac, const String& status) {
  if (knownDevicesByMac.find(deviceMac) != knownDevicesByMac.end()) {
    const MeshDevice& device = knownDevicesByMac[deviceMac];
    
    DynamicJsonDocument doc(1024); // 增大容量以容纳LED配置信息
    doc["type"] = "device_status";
    doc["mac"] = deviceMac;
    doc["name"] = device.name;
    doc["status"] = status;
    doc["online"] = device.isOnline;
    doc["hasBeenDiscovered"] = device.hasBeenDiscovered;
    doc["timestamp"] = millis();
    
    String message;
    serializeJson(doc, message);
    broadcastWebSocketMessage(message);
  }
}

// 新增：发送包含LED配置信息的设备状态更新
void sendDeviceStatusUpdateWithLedConfig(const String& deviceMac, const String& status, const JsonObject& ledConfig) {
  if (knownDevicesByMac.find(deviceMac) != knownDevicesByMac.end()) {
    const MeshDevice& device = knownDevicesByMac[deviceMac];
    
    DynamicJsonDocument doc(1024);
    doc["type"] = "device_status";
    doc["mac"] = deviceMac;
    doc["name"] = device.name;
    doc["status"] = status;
    doc["online"] = device.isOnline;
    doc["hasBeenDiscovered"] = device.hasBeenDiscovered;
    doc["timestamp"] = millis();
    
    // 添加LED配置信息
    if (!ledConfig.isNull()) {
      doc["ledConfig"] = ledConfig;
    }
    
    String message;
    serializeJson(doc, message);
    broadcastWebSocketMessage(message);
  }
}

void sendActionStatusUpdate(const String& actionName, const String& status) {
  DynamicJsonDocument doc(256);
  doc["type"] = "action_status";
  doc["action"] = actionName;
  doc["status"] = status;
  doc["timestamp"] = millis();
  
  String message;
  serializeJson(doc, message);
  broadcastWebSocketMessage(message);
}