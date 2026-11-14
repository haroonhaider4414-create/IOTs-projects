#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_AM2320.h>
#include <WiFi.h>            // ESP32 WiFi
#include <PubSubClient.h>    // MQTT

// ====== CHANGE THESE 4 VALUES ======
const char* WIFI_SSID     = "CARE-Guests";
const char* WIFI_PASSWORD = "C@r3pvtltd";
const char* MQTT_BROKER   = "192.168.0.97";   // your laptop's IP
const int   MQTT_PORT     = 1883;
// ===================================

// ONE topic carrying both values together as a JSON message
const char* TOPIC_DATA = "am2320/data";

Adafruit_AM2320 am2320 = Adafruit_AM2320();
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

void connectWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.print("\nWiFi connected. IP: ");
  Serial.println(WiFi.localIP());
}

void connectMQTT() {
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  while (!mqtt.connected()) {
    Serial.print("Connecting to MQTT broker...");
    if (mqtt.connect("esp32-am2320")) {
      Serial.println(" connected!");
    } else {
      Serial.print(" failed, rc=");
      Serial.print(mqtt.state());
      Serial.println(" — retrying in 2s");
      delay(2000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial) { delay(10); }

  Wire.begin(21, 22);
  am2320.begin();
  Serial.println("AM2320 sensor started.");

  connectWiFi();
  connectMQTT();
}

void loop() {
  if (!mqtt.connected()) connectMQTT();  // reconnect if dropped
  mqtt.loop();                           // let MQTT do its housekeeping

  float temperature = am2320.readTemperature();
  float humidity    = am2320.readHumidity();

  if (isnan(temperature) || isnan(humidity)) {
    Serial.println("Failed to read from AM2320 sensor! Check wiring.");
  } else {
    // Print to Serial (as before)
    Serial.print("Temperature: ");
    Serial.print(temperature, 1);
    Serial.print(" C   Humidity: ");
    Serial.print(humidity, 1);
    Serial.println(" %");

    // Build ONE JSON message holding BOTH values, e.g.
    //   {"temperature":23.8,"humidity":52.0}
    char payload[64];
    snprintf(payload, sizeof(payload),
             "{\"temperature\":%.1f,\"humidity\":%.1f}",
             temperature, humidity);

    // Publish the whole thing to a single topic
    mqtt.publish(TOPIC_DATA, payload);

    Serial.print("  -> published to MQTT: ");
    Serial.println(payload);
  }

  delay(2000);
}