#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <sps30.h>
#include <DHT.h>
#include <ArduinoJson.h>

// ---------------------- Configuration ----------------------

const char* ssid = "";
const char* password = "";

// MQTT (HiveMQ Cloud)
const char* mqttServer = "";
const int mqttPort = 8883;
const char* mqttUser = "";
const char* mqttPassword = "";
const char* mqttClientID = "ESP32SPS30";
const char* mqttTopic = "";

// ---------------------- Sensors ----------------------

#define DHTPIN 26
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

// ---------------------- Networking ----------------------

WiFiClientSecure espClient;
PubSubClient client(espClient);

// ---------------------- Timers ----------------------

unsigned long lastPublishTime = 0;
const unsigned long publishInterval = 30000;  // 30s

unsigned long lastSuccessfulReadTime = 0;
bool hasEverReadData = false;
const unsigned long maxNoDataDuration = 10UL * 60UL * 1000UL; // 10 minutes

// ---------------------- Setup Functions ----------------------

void setupWiFi() {
    Serial.print("Connecting to Wi-Fi...");
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nConnected to Wi-Fi");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
}

void connectMQTT() {
    espClient.setInsecure();
    client.setBufferSize(1024);
    client.setKeepAlive(120);

    while (!client.connected()) {
        Serial.println("Connecting to MQTT (Secure)...");
        if (client.connect(mqttClientID, mqttUser, mqttPassword)) {
            Serial.println("Connected to MQTT broker");
        } else {
            Serial.print("Failed, rc=");
            Serial.print(client.state());
            Serial.println(" Retrying in 5s...");
            delay(5000);
        }
    }
}

bool initializeSensors() {
    Serial.println("Initializing SPS30 & DHT...");
    dht.begin();
    sensirion_i2c_init();

    int retries = 0;
    while (sps30_probe() != 0) {
        retries++;
        Serial.println("SPS30 sensor probing failed. Retrying...");
        delay(1000);
        if (retries >= 10) {
            Serial.println("SPS30 initialization failed after multiple attempts.");
            return false;
        }
    }

    Serial.println("SPS30 sensor detected successfully.");
    sps30_set_fan_auto_cleaning_interval_days(4);
    sps30_start_measurement();

    return true;
}

void publishRebootMessage() {
    StaticJsonDocument<128> jsonDoc;
    jsonDoc["message"] = "SPS30 Reboot";

    char jsonBuffer[128];
    serializeJson(jsonDoc, jsonBuffer);

    if (client.publish(mqttTopic, jsonBuffer)) {
        Serial.println("Reboot message published:");
        Serial.println(jsonBuffer);
    } else {
        Serial.println("Failed to publish reboot message.");
    }
}

// ---------------------- Data Publishing ----------------------

void publishSensorData() {
    struct sps30_measurement sps_data;
    float temp = NAN, humidity = NAN;

    if (sps30_read_measurement(&sps_data) < 0) {
        Serial.println("Error reading SPS30 measurement");
        return;
    }

    temp = dht.readTemperature();
    humidity = dht.readHumidity();

    if (isnan(temp) || isnan(humidity)) {
        Serial.println("Failed to read from DHT11 sensor");
        return;
    }

    lastSuccessfulReadTime = millis();
    hasEverReadData = true;

    StaticJsonDocument<512> jsonDoc;
    jsonDoc["PM1P0"] = sps_data.mc_1p0;
    jsonDoc["PM2P5"] = sps_data.mc_2p5;
    jsonDoc["PM4P0"] = sps_data.mc_4p0;
    jsonDoc["PM10P0"] = sps_data.mc_10p0;
    jsonDoc["NC_0N5"] = sps_data.nc_0p5;
    jsonDoc["NC_1N0"] = sps_data.nc_1p0;
    jsonDoc["NC_2N5"] = sps_data.nc_2p5;
    jsonDoc["NC_4N0"] = sps_data.nc_4p0;
    jsonDoc["NC_10N0"] = sps_data.nc_10p0;
    jsonDoc["TypicalParticleSize"] = sps_data.typical_particle_size;
    jsonDoc["temp_out"] = temp;
    jsonDoc["humidity_out"] = humidity;

    char jsonBuffer[512];
    size_t jsonLength = serializeJson(jsonDoc, jsonBuffer);

    if (jsonLength > 0) {
        if (client.publish(mqttTopic, jsonBuffer)) {
            Serial.println("Data published successfully:");
            Serial.println(jsonBuffer);
        } else {
            Serial.println("Error: Failed to publish data.");
        }
    } else {
        Serial.println("Error: Failed to serialize JSON.");
    }
}

// ---------------------- Main ----------------------

void setup() {
    Serial.begin(115200);
    delay(2000);

    setupWiFi();

    client.setServer(mqttServer, mqttPort);
    connectMQTT();

    bool sensorOK = initializeSensors();
    if (!sensorOK) {
        Serial.println("Reinitializing sensors failed. Restarting ESP...");
        delay(2000);
        ESP.restart();
    }

    publishRebootMessage();
}

void loop() {
    // Ensure Wi-Fi and MQTT connectivity
    if (WiFi.status() != WL_CONNECTED) setupWiFi();
    if (!client.connected()) connectMQTT();
    client.loop();

    unsigned long currentTime = millis();

    // Watchdog: reboot if no valid data for 10 minutes
    if (hasEverReadData && (currentTime - lastSuccessfulReadTime > maxNoDataDuration)) {
        Serial.println("No sensor data for 10 minutes. Restarting ESP32...");
        delay(1000);
        ESP.restart();
    }

    // Publish data at defined interval
    if (currentTime - lastPublishTime >= publishInterval) {
        publishSensorData();
        lastPublishTime = currentTime;
    }
}
