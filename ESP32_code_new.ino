#include "Arduino.h"
#include <WiFi.h>
#include "Wire.h"
#include "SensirionI2CSen5x.h"
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// ========== WiFi & MQTT Configuration ==========
const char* ssid = "";
const char* password = "";

const char* mqttServer   = "";
const int   mqttPort     = ;
const char* mqttUser     = "";
const char* mqttPassword = "";
const char* mqttClientID = "ESP32Client";
const char* mqttTopic    = "";

// ========== Global Variables ==========
WiFiClientSecure espClient;
PubSubClient client(espClient);
SensirionI2CSen5x sen55;

const int SDA_PIN = 21;
const int SCL_PIN = 22;

unsigned long lastPublishTime = 0;
unsigned long lastGoodDataTime = 0;
const unsigned long publishInterval = 60000;   // 1 minute
const unsigned long maxNoDataDuration = 10UL * 60UL * 1000UL; // 10 minutes

int readFailCount = 0;
const int maxReadFails = 5;

// ========== Functions ==========

void setupWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 30) {
    delay(500);
    Serial.print(".");
    retry++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ WiFi connected");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n⚠️ WiFi failed, restarting...");
    delay(2000);
    ESP.restart();
  }
}

void connectMQTT() {
  espClient.setInsecure();
  while (!client.connected()) {
    Serial.println("Connecting to MQTT...");
    if (client.connect(mqttClientID, mqttUser, mqttPassword)) {
      Serial.println("✅ MQTT connected");
    } else {
      Serial.print("❌ MQTT failed, rc=");
      Serial.println(client.state());
      delay(5000);
    }
  }
}

// ----- I2C Recovery -----
void recoverI2CBus() {
  Serial.println("⚙️ Recovering I2C bus (software only)...");
  pinMode(SCL_PIN, OUTPUT);
  pinMode(SDA_PIN, INPUT_PULLUP);

  // Pulse SCL up to 9 times to release stuck SDA
  for (int i = 0; i < 9; i++) {
    digitalWrite(SCL_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(SCL_PIN, LOW);
    delayMicroseconds(10);
  }

  // Generate STOP condition manually
  pinMode(SDA_PIN, OUTPUT);
  digitalWrite(SDA_PIN, LOW);
  delayMicroseconds(10);
  digitalWrite(SCL_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(SDA_PIN, HIGH);
  delayMicroseconds(10);

  // Reinitialize I2C
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setTimeOut(50);
  Serial.println("✅ I2C recovered and reinitialized.");
}

// ----- Sensor Initialization -----
bool initSensor() {
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setTimeOut(50);
  sen55.begin(Wire);
  uint16_t error = sen55.startMeasurement();

  if (error) {
    Serial.print("❌ Sensor init failed: ");
    Serial.println(error);
    return false;
  }
  Serial.println("✅ SEN55 initialized successfully.");
  return true;
}

// ----- Publish to MQTT -----
void publishData(float pm1, float pm25, float pm4, float pm10,
                 float hum, float temp, float voc, float nox) {
  StaticJsonDocument<256> doc;
  doc["PM1P0"] = pm1;
  doc["PM2P5"] = pm25;
  doc["PM4P0"] = pm4;
  doc["PM10P0"] = pm10;
  doc["Humidity"] = hum;
  doc["Temperature"] = temp;
  doc["VOCIndex"] = voc;
  doc["NOXIndex"] = nox;
  doc["timestamp"] = millis() / 1000;

  char buffer[256];
  serializeJson(doc, buffer);
  client.publish(mqttTopic, buffer);
  Serial.println("✅ Data sent: ");
  Serial.println(buffer);
}

// ----- Read Sensor -----
bool readSensorData() {
  float pm1, pm25, pm4, pm10, hum, temp, voc, nox;
  uint16_t error = sen55.readMeasuredValues(pm1, pm25, pm4, pm10, hum, temp, voc, nox);

  if (error) {
    Serial.print("⚠️ Sensor read failed, code: ");
    Serial.println(error);
    readFailCount++;
    return false;
  }

  // Success
  readFailCount = 0;
  lastGoodDataTime = millis();
  publishData(pm1, pm25, pm4, pm10, hum, temp, voc, nox);
  return true;
}

// ========== Setup ==========
void setup() {
  Serial.begin(115200);
  setupWiFi();
  client.setServer(mqttServer, mqttPort);
  connectMQTT();

  if (!initSensor()) {
    delay(1000);
    recoverI2CBus();
    if (!initSensor()) {
      Serial.println("❌ Sensor failed to start after recovery. Restarting...");
      delay(2000);
      ESP.restart();
    }
  }

  lastPublishTime = millis();
  lastGoodDataTime = millis();
}

// ========== Loop ==========
void loop() {
  if (WiFi.status() != WL_CONNECTED) setupWiFi();
  if (!client.connected()) connectMQTT();
  client.loop();

  unsigned long now = millis();

  if (now - lastPublishTime >= publishInterval) {
    if (!readSensorData()) {
      if (readFailCount >= maxReadFails) {
        Serial.println("⚙️ Too many read fails, attempting recovery...");
        recoverI2CBus();
        if (!initSensor()) {
          Serial.println("🔁 Reinit failed, restarting ESP...");
          delay(2000);
          ESP.restart();
        }
        readFailCount = 0;
      }
    }
    lastPublishTime = now;
  }

  // If no good data for >10 minutes → restart
  if (now - lastGoodDataTime > maxNoDataDuration) {
    Serial.println("⏱️ No data for 10 minutes, restarting ESP...");
    delay(1000);
    ESP.restart();
  }
}
