#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_AM2320.h>
#include <SPI.h>
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <SD.h>
#define BUTTON GPIO_NUM_4
#define LED_PIN 2

const int chipSelect = 5;
const char* ssid = "CARE-Guests";
const char* password = "C@r3pvtltd";
const char* mqtt_server = "192.168.0.103";
const int MQTT_PORT = 1883;

Adafruit_AM2320 am2320 = Adafruit_AM2320();
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

void connectWifi() {
  Serial.print("Connecting to WIFI");
  WiFi.begin(ssid, password);
  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED && attempt < 3) {
    delay(500);
    Serial.print(".");
    attempt++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected!");
  } else {
    Serial.println("\nWiFi Connection failed! Running offline.");
  }
}

void connectMQTT() {
  mqtt.setServer(mqtt_server, MQTT_PORT);
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
void publishSavedData() {
  if (SD.begin(chipSelect)) {
    if (SD.exists("/sensor_data.txt")) {
      Serial.println("Connection restored. Syncing offline backup from SD card...");
      File readFile = SD.open("/sensor_data.txt", FILE_READ);
      if (readFile) {
        while (readFile.available()) {
          String line = readFile.readStringUntil('\n');
          line.trim();
          if (line.length() > 0) {
            mqtt.publish("room/data", line.c_str());
            delay(100);
          }
        }
        readFile.close();
        SD.remove("/sensor_data.txt");
        Serial.println("Offline backup successfully published and file cleared.");
      }
    } else {
      Serial.println("No backup file found on SD.");
    }
    SD.end();
  }
}

void setup() {
  Serial.begin(115200);
  Wire.begin();
  am2320.begin();
  pinMode(BUTTON, INPUT_PULLDOWN);
  pinMode(2, OUTPUT);
  esp_sleep_enable_timer_wakeup(60000000);
  esp_sleep_enable_ext0_wakeup(BUTTON, 1);
  Serial.println("AM2320 sensor Started.");
  Serial.println("Initializing.");
  if (!SD.begin(chipSelect)) {
    Serial.println("Card mount failed.");
    return;
  } else {
    Serial.println("SD card Ready.");
    SD.end();
  }
  connectWifi();
  connectMQTT();
}

void loop() {
  connectWifi();
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqtt.connected()) {
      connectMQTT();
    }
    mqtt.loop();
    digitalWrite(LED_PIN, HIGH);
  }

  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) {
    Serial.println("Woke up from: Button Press.");
  } else if (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) {
    Serial.println("Woke up from: 60s Timer.");
  }

  float temp = am2320.readTemperature();
  float humidity = am2320.readHumidity();

  if (isnan(temp) || isnan(humidity)) {
    Serial.println("failed to connect to the AM2320 sensor. Check wiring.");
  } else {
    String packet = "Temperature: " + String(temp, 2) + ", " + "Humidity: " + String(humidity, 2);
    Serial.println(packet);

    if (WiFi.status() == WL_CONNECTED && mqtt.connected()) {
      Serial.println("WiFI is connected so trying to publish");
      char msg[50];
      packet.toCharArray(msg, 50);
      bool pubSuccess = false;

      Serial.println(msg);
      if (mqtt.publish("room/data", msg)) {
        Serial.println("Publish is successful.");
        pubSuccess = true;
        publishSavedData();
      } else {
        Serial.println("Not published.");
        File writeFile = SD.open("/sensor_data.txt", FILE_APPEND);
        writeFile.println(msg);
        writeFile.close();
        Serial.println("Uploaded to the sd card successfully.");
      }
    }

    else {
      delay(50);
      if (SD.begin(chipSelect)) {
        File writeFile = SD.open("/sensor_data.txt", FILE_APPEND);
        if (writeFile) {
          writeFile.println(packet);
          Serial.println("Data logged: " + packet);
          writeFile.close();
          SD.end();
          Serial.println("Data is stored in the SD card.");

          delay(100);
          

        } else {
          Serial.println("Error opening /sensor_data.txt");
          SD.end();
          delay(100);
          //esp_light_sleep_start();
        }
      } else {
        Serial.println("SD Card mount failed in loop.");

        delay(100);
      }
    }
  }
  delay(1000);
  digitalWrite(LED_PIN, LOW);
  Serial.println("GOING BACK TO SLEEP.");
  delay(100);
  esp_light_sleep_start();
}