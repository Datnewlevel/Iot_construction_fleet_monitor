#define TINY_GSM_MODEM_SIM7600
#define TINY_GSM_RX_BUFFER 1024

#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <TinyGsmClient.h>
#include <PubSubClient.h>
#include <TinyGPSPlus.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <HardwareSerial.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_INA219.h>

enum ConnMode { MODE_WIFI, MODE_4G, MODE_AP };
ConnMode currentMode = MODE_AP;

const char* WIFI_SSID = "Your_Wifi_SSID";
const char* WIFI_PASS = "Your_Wifi_Pass";
#define WIFI_CONNECT_TIMEOUT_MS  15000

const char* AP_SSID = "UTT-MayThiCong";
const char* AP_PASS = "12345678";

#define SIM_TX_PIN          32
#define SIM_RX_PIN          33
#define SIM_BAUDRATE        115200
#define GSM_APN             "v-internet" //Viettel kept the same, while other network providers changed it.

#define THINGSBOARD_SERVER  "Your_IP_thingsboard_sv" 
#define THINGSBOARD_PORT    1883
#define ACCESS_TOKEN        "Your_access_token" //access token "device", which is generated in thingsboard
#define TELEMETRY_TOPIC     "v1/devices/me/telemetry"
#define SEND_INTERVAL       5000

static const int PIN_VIB  = 27;
static const int GPS_RX   = 17;
static const int GPS_TX   = 16;
#define ONE_WIRE_BUS 4
static const uint32_t GPS_BAUD = 9600;

static const uint8_t I2C_SDA_PIN = 21;
static const uint8_t I2C_SCL_PIN = 22;
static const uint8_t OLED_I2C_ADDR = 0x3C;
static const uint8_t INA219_I2C_ADDR = 0x40;
static const int OLED_RESET = -1;
static const int SCREEN_WIDTH = 128;
static const int SCREEN_HEIGHT = 64;

static const float LIION_FULL_VOLTAGE = 4.20f;
static const float LIION_EMPTY_VOLTAGE = 3.20f;
static const float MAX_DISPLAY_CURRENT_A = 2.0f;

static const uint32_t HOLD_MS = 3000;
static const uint32_t SAVE_EVERY_MS = 60000;
static const float    MOVING_THRESHOLD_KMPH = 1.0;
static const uint32_t GPS_MAX_AGE_MS = 30000;

HardwareSerial simSerial(2);
HardwareSerial gpsSerial(1);

TinyGsm modem(simSerial);
TinyGsmClient gsmClient(modem);
WiFiClient wifiClient;
PubSubClient mqttClient;

WebServer server(80);
TinyGPSPlus gps;
Preferences prefs;
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature ds18b20(&oneWire);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Adafruit_INA219 ina219(INA219_I2C_ADDR);

float lastTemperature = NAN;
float lastBusVoltage = 0.0f;
float lastCurrentA = 0.0f;
float lastPowermW = 0.0f;
bool ina219Ready = false;
bool oledReady = false;
volatile bool vibPulse = false;

uint64_t totalRunMs  = 0;
uint64_t totalIdleMs = 0;
uint32_t lastLoopMs = 0;
uint32_t lastVibSeenMs = 0;
uint32_t lastSaveMs = 0;
unsigned long lastMqttSendTime = 0;
uint8_t publishFailCount = 0;
unsigned long lastRetry4GMs = 0;
#define RETRY_4G_INTERVAL 30000

void IRAM_ATTR onVibrationISR() {
  vibPulse = true;
}

bool isRunning(uint32_t nowMs) {
  return (nowMs - lastVibSeenMs) < HOLD_MS;
}

bool isGpsValid() {
  return gps.location.isValid() && gps.location.age() < GPS_MAX_AGE_MS;
}

bool isMoving() {
  return gps.speed.isValid() && gps.speed.age() < GPS_MAX_AGE_MS
         && (gps.speed.kmph() > MOVING_THRESHOLD_KMPH);
}

const char* getModeName() {
  switch (currentMode) {
    case MODE_WIFI: return "WiFi";
    case MODE_4G:   return "4G SIM7680C";
    case MODE_AP:   return "AP Mode (Offline)";
    default:        return "Unknown";
  }
}

int calculateBatteryPercent(float voltage) {
  struct VoltagePoint {
    float voltage;
    int percent;
  };

  static const VoltagePoint socTable[] = {
    {4.20f, 100},
    {4.15f, 95},
    {4.08f, 90},
    {4.00f, 80},
    {3.92f, 70},
    {3.86f, 60},
    {3.80f, 50},
    {3.74f, 40},
    {3.68f, 30},
    {3.58f, 20},
    {3.46f, 10},
    {3.30f, 5},
    {3.20f, 0}
  };

  if (voltage >= LIION_FULL_VOLTAGE) return 100;
  if (voltage <= LIION_EMPTY_VOLTAGE) return 0;

  for (size_t i = 0; i < (sizeof(socTable) / sizeof(socTable[0])) - 1; i++) {
    float upperVoltage = socTable[i].voltage;
    float lowerVoltage = socTable[i + 1].voltage;
    int upperPercent = socTable[i].percent;
    int lowerPercent = socTable[i + 1].percent;

    if (voltage <= upperVoltage && voltage >= lowerVoltage) {
      float spanVoltage = upperVoltage - lowerVoltage;
      float ratio = (voltage - lowerVoltage) / spanVoltage;
      float percent = lowerPercent + ratio * (upperPercent - lowerPercent);
      return (int)(percent + 0.5f);
    }
  }

  return 0;
}

void drawBatteryIcon(int x, int y, int percent) {
  display.drawRect(x, y, 27, 15, WHITE);
  display.fillRect(x + 27, y + 5, 3, 5, WHITE);

  int numBars = 0;
  if (percent >= 100) numBars = 4;
  else if (percent >= 75) numBars = 3;
  else if (percent >= 50) numBars = 2;
  else if (percent >= 25) numBars = 1;

  for (int i = 0; i < numBars; i++) {
    display.fillRect(x + 3 + (i * 6), y + 3, 4, 9, WHITE);
  }
}

void drawOledDisplay(float voltage, float current, int percent) {
  if (!oledReady) return;

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);

  display.setCursor(0, 0);
  display.print(voltage, 1);
  display.print("V");

  display.setCursor(0, 12);
  display.print(current, 2);
  display.print("A");

  drawBatteryIcon(95, 1, percent);

  display.setTextSize(2);
  if (percent >= 100) {
    display.setCursor(42, 40);
  } else if (percent >= 10) {
    display.setCursor(50, 40);
  } else {
    display.setCursor(52, 40);
  }

  display.print(percent);
  display.print("%");
  display.display();
}

void setupInaAndOled() {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  oledReady = display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDR);
  if (oledReady) {
    display.clearDisplay();
    display.display();
  } else {
    Serial.println("[OLED] Khong tim thay OLED SSD1306 tai 0x3C");
  }

  ina219Ready = ina219.begin();
  if (ina219Ready) {
    Serial.println("[INA219] Da ket noi tai 0x40");
  } else {
    Serial.println("[INA219] Khong tim thay INA219 tai 0x40");
  }

  drawOledDisplay(0.0f, 0.0f, 0);
}

void updateInaAndOled() {
  if (!ina219Ready) return;

  lastBusVoltage = ina219.getBusVoltage_V();
  lastCurrentA = ina219.getCurrent_mA() / 1000.0f;
  if (lastCurrentA > MAX_DISPLAY_CURRENT_A) {
    lastCurrentA = MAX_DISPLAY_CURRENT_A;
  }
  lastPowermW = ina219.getPower_mW();

  int batteryPercent = calculateBatteryPercent(lastBusVoltage);
  drawOledDisplay(lastBusVoltage, lastCurrentA, batteryPercent);
}

String jsonStatus() {
  uint32_t nowMs = millis();
  bool running  = isRunning(nowMs);
  bool gpsValid = isGpsValid();
  bool moving   = isMoving();
  bool idleWarning = running && !moving;

  StaticJsonDocument<512> doc;
  doc["mode"]           = getModeName();
  doc["engine_on"]      = running ? 1 : 0;
  doc["moving"]         = moving;
  doc["idle_warning"]   = idleWarning;
  doc["totalRunMinutes"]  = (unsigned long)(totalRunMs / 60000ULL);
  doc["total_run_sec"]    = (unsigned long)(totalRunMs / 1000ULL);
  doc["totalIdleMinutes"] = (unsigned long)(totalIdleMs / 60000ULL);
  doc["gps_valid"]      = gpsValid;
  doc["latitude"]       = gpsValid ? gps.location.lat() : 0.0;
  doc["longitude"]      = gpsValid ? gps.location.lng() : 0.0;
  doc["speed_kmph"]     = gps.speed.isValid() ? gps.speed.kmph() : 0.0;
  doc["sat"]            = gps.satellites.isValid() ? (int)gps.satellites.value() : 0;
  doc["temperature"]    = isnan(lastTemperature) ? 0.0 : lastTemperature;
  doc["power_mw"]       = lastPowermW;
  doc["uptime_ms"]      = nowMs;

  String payload;
  serializeJson(doc, payload);
  return payload;
}

#include "dashboard.h"

void handleDashboard() {
  server.send_P(200, "text/html", DASHBOARD_HTML);
}

void handleApiStatus() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", jsonStatus());
}

void handleReset() {
  if (!server.hasArg("key") || server.arg("key") != "utt") {
    server.send(403, "text/plain", "Forbidden");
    return;
  }
  totalRunMs  = 0;
  totalIdleMs = 0;
  prefs.putULong64("totalRunMs",  totalRunMs);
  prefs.putULong64("totalIdleMs", totalIdleMs);
  server.send(200, "text/plain", "OK - counters reset");
}

void setupWebServer() {
  server.on("/",           handleDashboard);
  server.on("/api/status", handleApiStatus);
  server.on("/reset",      handleReset);
  server.begin();
  Serial.println("[WEB] Dashboard đã khởi động");
}

bool tryWiFi() {
  Serial.print("[WiFi] Dang ket noi " + String(WIFI_SSID));
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_CONNECT_TIMEOUT_MS) {
      Serial.println(" HET THOI GIAN");
      WiFi.disconnect(true);
      return false;
    }
    delay(300);
    Serial.print(".");
  }
  Serial.println(" OK");
  Serial.println("[WiFi] IP: " + WiFi.localIP().toString());
  return true;
}

bool tryModem4G() {
  Serial.println("[SIM] Khoi dong SIM7680C...");
  simSerial.begin(SIM_BAUDRATE, SERIAL_8N1, SIM_RX_PIN, SIM_TX_PIN);
  delay(3000);

  Serial.println("[SIM] Kiem tra AT...");
  if (!modem.testAT(10000)) {
    Serial.println("[SIM] Khong co phan hoi AT");
    return false;
  }

  String info = modem.getModemInfo();
  Serial.println("[SIM] Modem: " + info);

  SimStatus ss = modem.getSimStatus();
  Serial.print("[SIM] Trang thai SIM: ");
  Serial.println(ss);

  Serial.print("[SIM] Cho ket noi mang...");
  if (!modem.waitForNetwork(60000L)) {
    Serial.println(" THAT BAI");
    return false;
  }
  Serial.println(" OK");

  int csq = modem.getSignalQuality();
  Serial.println("[SIM] Tin hieu: " + String(csq));

  Serial.print("[SIM] Ket noi APN: " + String(GSM_APN) + "...");
  if (!modem.gprsConnect(GSM_APN, "", "")) {
    Serial.println(" THAT BAI");
    return false;
  }
  Serial.println(" OK");
  Serial.println("[SIM] IP: " + modem.getLocalIP());
  return true;
}

bool checkModemConnection() {
  if (!modem.isNetworkConnected()) {
    Serial.println("[SIM] Mat mang, dang ket noi lai...");
    if (!modem.waitForNetwork(30000L)) return false;
  }
  if (!modem.isGprsConnected()) {
    Serial.println("[SIM] Mat GPRS, dang ket noi lai...");
    if (!modem.gprsConnect(GSM_APN, "", "")) return false;
  }
  return true;
}

void startAPMode() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  String ip = WiFi.softAPIP().toString();
  Serial.println("==========================================");
  Serial.println("[AP MODE] WiFi va 4G khong kha dung");
  Serial.println("[AP MODE] Ket noi WiFi:");
  Serial.println("          SSID     : " + String(AP_SSID));
  Serial.println("          Password : " + String(AP_PASS));
  Serial.println("[AP MODE] Mo trinh duyet va vao:");
  Serial.println("          http://" + ip);
  Serial.println("==========================================");
}

void reconnectMQTT() {
  int retries = 0;
  while (!mqttClient.connected() && retries < 5) {
    Serial.print("[MQTT] Dang ket noi ThingsBoard...");
    if (mqttClient.connect("ESP32_UTT", ACCESS_TOKEN, NULL)) {
      Serial.println(" Thanh cong!");
      return;
    }
    retries++;
    Serial.print(" That bai, rc=");
    Serial.print(mqttClient.state());
    Serial.println(" (" + String(retries) + "/5)");
    delay(3000);
  }
}

void sendToThingsBoard() {
  if (currentMode == MODE_WIFI && WiFi.status() != WL_CONNECTED) {
    Serial.println("[MQTT] Mat WiFi, bo qua...");
    return;
  }
  if (currentMode == MODE_4G && !checkModemConnection()) {
    Serial.println("[MQTT] Mat 4G, bo qua...");
    return;
  }

  if (!mqttClient.connected()) {
    reconnectMQTT();
    if (!mqttClient.connected()) return;
  }

  uint32_t nowMs = millis();
  bool running  = isRunning(nowMs);
  bool gpsValid = isGpsValid();

  StaticJsonDocument<256> doc;
  doc["engine_on"]    = running ? 1 : 0;
  doc["totalRunMinutes"]  = (unsigned long)(totalRunMs / 60000ULL);
  doc["totalIdleMinutes"] = (unsigned long)(totalIdleMs / 60000ULL);
  doc["latitude"]     = gpsValid ? gps.location.lat() : 0.0;
  doc["longitude"]    = gpsValid ? gps.location.lng() : 0.0;
  doc["temperature"]  = isnan(lastTemperature) ? 0.0 : lastTemperature;
  doc["power_mw"]     = lastPowermW;

  String payload;
  serializeJson(doc, payload);

  if (mqttClient.publish(TELEMETRY_TOPIC, payload.c_str())) {
    Serial.println("[MQTT] Da gui: " + payload);
    publishFailCount = 0;
  } else {
    publishFailCount++;
    Serial.print("[MQTT] Loi gui! (" + String(publishFailCount) + "/3)");
    if (publishFailCount >= 3) {
      Serial.println(" -> Ket noi lai buoc buoc");
      mqttClient.disconnect();
      publishFailCount = 0;
    } else {
      Serial.println();
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n\n-----------------------");
  Serial.println("UTT - Giam sat may thi cong");
  Serial.println("Uu tien ket noi: WiFi -> 4G -> AP");

  pinMode(PIN_VIB, INPUT_PULLDOWN);
  attachInterrupt(digitalPinToInterrupt(PIN_VIB), onVibrationISR, RISING);

  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX, GPS_TX);

  ds18b20.begin();
  ds18b20.setResolution(12);
  Serial.println("[DS18B20] Khoi dong tai GPIO" + String(ONE_WIRE_BUS));

  setupInaAndOled();

  prefs.begin("utt_machine", false);
  totalRunMs  = 0;
  totalIdleMs = 0;

  if (tryWiFi()) {
    currentMode = MODE_WIFI;
    mqttClient.setClient(wifiClient);
    Serial.println("[CHE DO] >>> WiFi <<<");
  }
  else if (tryModem4G()) {
    currentMode = MODE_4G;
    mqttClient.setClient(gsmClient);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.println("[CHE DO] >>> 4G + AP dashboard <<<");
    Serial.println("[AP] Dashboard: http://" + WiFi.softAPIP().toString());
  }
  else {
    currentMode = MODE_AP;
    startAPMode();
    Serial.println("[CHE DO] >>> AP (ngoai tuyen) <<<");
  }

  if (currentMode != MODE_AP) {
    mqttClient.setServer(THINGSBOARD_SERVER, THINGSBOARD_PORT);
    Serial.println("[MQTT] Server: " + String(THINGSBOARD_SERVER) + ":" + String(THINGSBOARD_PORT));
    reconnectMQTT();
  }

  setupWebServer();

  lastLoopMs    = millis();
  lastSaveMs    = millis();
  lastVibSeenMs = 0;

  Serial.println("========== READY ==========");
  Serial.println("[Mode] " + String(getModeName()));
}

void loop() {
  while (gpsSerial.available()) {
    gps.encode(gpsSerial.read());
  }

  static unsigned long lastTempRead = 0;
  static unsigned long lastInaRead = 0;
  uint32_t nowMs = millis();
  if (nowMs - lastTempRead >= SEND_INTERVAL) {
    lastTempRead = nowMs;
    ds18b20.requestTemperatures();
    float t = ds18b20.getTempCByIndex(0);
    if (t != DEVICE_DISCONNECTED_C) {
      lastTemperature = t;
    }
  }

  if (nowMs - lastInaRead >= 500) {
    lastInaRead = nowMs;
    updateInaAndOled();
  }

  if (vibPulse) {
    vibPulse = false;
    lastVibSeenMs = nowMs;
  }

  uint32_t dt = nowMs - lastLoopMs;
  lastLoopMs = nowMs;
  if (isRunning(nowMs)) {
    totalRunMs += dt;
    if (!isMoving()) totalIdleMs += dt;
  }

  if ((nowMs - lastSaveMs) >= SAVE_EVERY_MS) {
    lastSaveMs = nowMs;
    prefs.putULong64("totalRunMs",  totalRunMs);
    prefs.putULong64("totalIdleMs", totalIdleMs);
  }

  if (currentMode != MODE_AP) {
    if (!mqttClient.connected()) {
      reconnectMQTT();
    }
    mqttClient.loop();

    if (nowMs - lastMqttSendTime >= SEND_INTERVAL) {
      lastMqttSendTime = nowMs;
      sendToThingsBoard();
    }
  }

  if (currentMode == MODE_AP && (nowMs - lastRetry4GMs) >= RETRY_4G_INTERVAL) {
    lastRetry4GMs = nowMs;
    Serial.println("[THU LAI] Dang ket noi 4G...");
    if (tryModem4G()) {
      currentMode = MODE_4G;
      mqttClient.setClient(gsmClient);
      mqttClient.setServer(THINGSBOARD_SERVER, THINGSBOARD_PORT);
      WiFi.mode(WIFI_AP);
      WiFi.softAP(AP_SSID, AP_PASS);
      Serial.println("[THU LAI] >>> Chuyen sang 4G thanh cong! <<<");
      Serial.println("[AP] Dashboard van truy cap: http://" + WiFi.softAPIP().toString());
      reconnectMQTT();
    } else {
      Serial.println("[THU LAI] 4G chua san sang, giu AP mode");
    }
  }

  server.handleClient();
}