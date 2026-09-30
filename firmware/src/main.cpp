#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <DHTesp.h>
#include <math.h>
#include <sys/time.h>
#include <time.h>
#include <esp_sntp.h>

// ---------------- Forward declarations ----------------
bool timeIsValid();
uint64_t epochMillis();
void bufferPush(const char* payload);
void flushBuffer();
void publishStatus(bool online);
void maintainConnections();
float readDistanceCm();
float readLightLux();
void appendNumber(char* out, size_t cap, const char* key, float value, int decimals);

// ---------------- Cau hinh ----------------
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASSWORD = "";
const char* MQTT_SERVER = "172.11.92.176";     // Host Wi-Fi IPv4; update if DHCP changes it
const uint16_t MQTT_PORT = 1883;
const char* TOPIC_ROOT = "iot";              // ← SỬA
const char* SITE_ID = "bai2";                // ← SỬA
const char* DEVICE_ID = "esp32";             // ← SỬA
const char* FW_VERSION = "2.0.0";

const uint8_t DHT_PIN = 15;
const uint8_t TRIG_PIN = 5;
const uint8_t ECHO_PIN = 18;
const uint8_t LDR_PIN = 34;
const uint8_t LED_PIN = 2;

const unsigned long SAMPLE_INTERVAL_MS = 5000;
const unsigned long WIFI_RETRY_MS = 10000;
const unsigned long MQTT_RETRY_MS = 5000;
const uint32_t NTP_RESYNC_MS = 60000;
const size_t BUFFER_CAPACITY = 120;
const size_t FLUSH_PER_LOOP = 10;
const size_t PAYLOAD_SIZE = 320;

// ---------------- Trang thai ----------------
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
DHTesp dht;
char telemetryTopic[128];
char statusTopic[128];

char ring[BUFFER_CAPACITY][PAYLOAD_SIZE];
size_t ringHead = 0;
size_t ringCount = 0;
unsigned long droppedFromBuffer = 0;

unsigned long lastSample = 0;
unsigned long lastWiFiAttempt = 0;
unsigned long lastMqttAttempt = 0;
unsigned long sequenceNo = 0;
bool wifiWasConnected = false;
bool ntpStarted = false;

// ---------------- Thoi gian (NTP) ----------------
bool timeIsValid() {
  return time(nullptr) > 1700000000;
}

uint64_t epochMillis() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000);
}

// ---------------- Bo dem store-and-forward ----------------
void bufferPush(const char* payload) {
  if (ringCount == BUFFER_CAPACITY) {
    ringHead = (ringHead + 1) % BUFFER_CAPACITY;
    ringCount--;
    droppedFromBuffer++;
  }
  const size_t tail = (ringHead + ringCount) % BUFFER_CAPACITY;
  strncpy(ring[tail], payload, PAYLOAD_SIZE - 1);
  ring[tail][PAYLOAD_SIZE - 1] = '\0';
  ringCount++;
}

void flushBuffer() {
  size_t sent = 0;
  while (ringCount > 0 && sent < FLUSH_PER_LOOP && mqttClient.connected()) {
    if (!mqttClient.publish(telemetryTopic, ring[ringHead])) break;
    ringHead = (ringHead + 1) % BUFFER_CAPACITY;
    ringCount--;
    sent++;
    mqttClient.loop();
  }
  if (sent > 0) {
    Serial.printf("Flushed %u buffered sample(s), %u left\n", (unsigned)sent, (unsigned)ringCount);
  }
}

// ---------------- Ket noi ----------------
void publishStatus(bool online) {
  char msg[160];
  snprintf(msg, sizeof(msg),
    "{\"device_id\":\"%s\",\"status\":\"%s\",\"fw\":\"%s\",\"ip\":\"%s\",\"dropped\":%lu}",
    DEVICE_ID, online ? "online" : "offline", FW_VERSION,
    WiFi.localIP().toString().c_str(), droppedFromBuffer);
  mqttClient.publish(statusTopic, msg, true);
}

void maintainConnections() {
  const unsigned long now = millis();
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiWasConnected) {
      Serial.println("WiFi disconnected - samples are buffered");
      wifiWasConnected = false;
      mqttClient.disconnect();
    }
    if (now - lastWiFiAttempt >= WIFI_RETRY_MS) {
      lastWiFiAttempt = now;
      Serial.println("Connecting WiFi...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD, 6);
    }
    return;
  }

  if (!wifiWasConnected) {
    wifiWasConnected = true;
    Serial.print("WiFi connected | IP: ");
    Serial.println(WiFi.localIP());
    lastMqttAttempt = now - MQTT_RETRY_MS;
    if (!ntpStarted) {
      sntp_set_sync_interval(NTP_RESYNC_MS);
      configTime(0, 0, "pool.ntp.org", "time.google.com");
      ntpStarted = true;
      Serial.println("NTP sync started");
    }
  }

  if (mqttClient.connected()) {
    mqttClient.loop();
    flushBuffer();
    return;
  }
  if (now - lastMqttAttempt < MQTT_RETRY_MS) return;
  lastMqttAttempt = now;

  String clientId = String("lab2-") + DEVICE_ID + "-" + String((uint32_t)esp_random(), HEX);
  Serial.printf("Connecting MQTT %s:%u ...", MQTT_SERVER, MQTT_PORT);
  const char* lwt = "{\"status\":\"offline\"}";
  if (mqttClient.connect(clientId.c_str(), nullptr, nullptr, statusTopic, 0, true, lwt)) {
    Serial.println(" connected");
    publishStatus(true);
  } else {
    Serial.printf(" failed, rc=%d. Retry in 5 s\n", mqttClient.state());
  }
}

// ---------------- Cam bien ----------------
float readDistanceCm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  const unsigned long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  if (duration == 0) return NAN;
  return duration * 0.0343f / 2.0f;
}

float readLightLux() {
  uint32_t sum = 0;
  for (int i = 0; i < 16; ++i) sum += analogRead(LDR_PIN);
  const float voltage = sum / 16.0f / 4095.0f * 3.3f;
  if (voltage <= 0.001f || voltage >= 3.299f) return NAN;
  const float resistance = 10000.0f * voltage / (3.3f - voltage);
  return powf(50e3f * powf(10.0f, 0.7f) / resistance, 1.0f / 0.7f);
}

void appendNumber(char* out, size_t cap, const char* key, float value, int decimals) {
  const size_t len = strlen(out);
  if (isfinite(value)) {
    snprintf(out + len, cap - len, ",\"%s\":%.*f", key, decimals, value);
  } else {
    snprintf(out + len, cap - len, ",\"%s\":null", key);
  }
}

// ---------------- setup / loop ----------------
void setup() {
  Serial.begin(115200);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(TRIG_PIN, LOW);
  digitalWrite(LED_PIN, LOW);
  analogReadResolution(12);
  dht.setup(DHT_PIN, DHTesp::DHT22);

  snprintf(telemetryTopic, sizeof(telemetryTopic), "%s/%s/%s/telemetry", TOPIC_ROOT, SITE_ID, DEVICE_ID);
  snprintf(statusTopic, sizeof(statusTopic), "%s/%s/%s/status", TOPIC_ROOT, SITE_ID, DEVICE_ID);
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setBufferSize(512);
  mqttClient.setSocketTimeout(3);
  mqttClient.setKeepAlive(30);

  Serial.println("\n=== BAI 2: ESP32 -> MQTT -> InfluxDB ===");
  Serial.printf("Device %s | topic %s | interval %lu ms\n", DEVICE_ID, telemetryTopic, SAMPLE_INTERVAL_MS);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD, 6);
  lastWiFiAttempt = millis();
  Serial.println("Connecting WiFi...");
}

void loop() {
  maintainConnections();
  const unsigned long now = millis();
  if (now - lastSample < SAMPLE_INTERVAL_MS) {
    delay(10);
    return;
  }
  lastSample = now;

  if (!timeIsValid()) {
    Serial.println("Waiting for NTP time - sample skipped");
    return;
  }

  const TempAndHumidity th = dht.getTempAndHumidity();
  if (dht.getStatus() != DHTesp::ERROR_NONE) {
    Serial.printf("DHT22 read failed: %s\n", dht.getStatusString());
  }
  const float lux = readLightLux();
  const float distance = readDistanceCm();
  const long rssi = WiFi.status() == WL_CONNECTED ? (long)WiFi.RSSI() : 0L;

  ++sequenceNo;
  char payload[PAYLOAD_SIZE];
  snprintf(payload, sizeof(payload), "{\"device_id\":\"%s\",\"ts\":%llu,\"seq\":%lu",
           DEVICE_ID, (unsigned long long)epochMillis(), sequenceNo);
  appendNumber(payload, sizeof(payload), "temperature", th.temperature, 2);
  appendNumber(payload, sizeof(payload), "humidity", th.humidity, 2);
  appendNumber(payload, sizeof(payload), "light_lux", lux, 1);
  appendNumber(payload, sizeof(payload), "distance_cm", distance, 2);
  const size_t len = strlen(payload);
  const int tail = snprintf(payload + len, sizeof(payload) - len, ",\"rssi\":%ld,\"uptime_s\":%lu}",
                            rssi, millis() / 1000UL);
  if (tail < 0 || len + tail >= sizeof(payload)) {
    Serial.println("Payload too large - skipped");
    return;
  }

  Serial.print(payload);
  if (!mqttClient.connected() || ringCount > 0) {
    bufferPush(payload);
    Serial.printf(" | BUFFERED (%u)\n", (unsigned)ringCount);
    if (mqttClient.connected()) flushBuffer();
    return;
  }
  const bool ok = mqttClient.publish(telemetryTopic, payload);
  Serial.printf(" | publish=%s\n", ok ? "OK" : "FAILED->BUFFER");
  if (!ok) {
    bufferPush(payload);
    return;
  }
  digitalWrite(LED_PIN, HIGH);
  delay(50);
  digitalWrite(LED_PIN, LOW);
}