#include <WiFi.h>
#include <SD.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_AM2320.h>
#include <SPI.h>
#include <time.h>

#define LED_PIN 2
const int chipSelect = 5;

const char* ssid = "CARE-Guests";
const char* password = "C@r3pvtltd";
const char* mqtt_server = "192.168.0.103";
const int MQTT_PORT = 1883;

const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 18000;
const int daylightOffset_sec = 0;

Adafruit_AM2320 am2320 = Adafruit_AM2320();
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

volatile float latestTemp = 0.0;
volatile float latestHumidity = 0.0;
volatile unsigned long latestTimestamp = 0;
volatile bool newSensorReadingAvailable = false;
volatile bool isConnected = false;
volatile bool hasPendingSDData = false;

unsigned long getEpochTime() {
  time_t now;
  time(&now);
  return (unsigned long)now;
}
bool isTimeSynced(){
  return getEpochTime() > 1704067200;
}
void TaskConnectivity(void* pvParameters) {
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      digitalWrite(LED_PIN, LOW);
      isConnected = false;
      Serial.println("[Connectivity] Connecting to Wifi.....");
      WiFi.begin(ssid, password);

      int attempt = 0;
      while (WiFi.status() != WL_CONNECTED && attempt < 2) {
        vTaskDelay(pdMS_TO_TICKS(500));
        attempt++;
      }

      if (WiFi.status() == WL_CONNECTED) {
        configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
      }
    }

    if (WiFi.status() == WL_CONNECTED && !mqtt.connected()) {
      mqtt.setServer(mqtt_server, MQTT_PORT);
      Serial.print("Connecting to MQTT broker...");
      if (mqtt.connect("esp32-am2320")) {
        Serial.println(" connected!");
      } else {
        Serial.print(" failed, rc=");
        Serial.println(mqtt.state());
      }
    }

    if (mqtt.connected()) {
      mqtt.loop();
      digitalWrite(LED_PIN, HIGH);
      isConnected = true;
    } else {
      digitalWrite(LED_PIN, LOW);
      isConnected = false;
    }

    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

void TaskSensor(void* pvParameters) {
  for (;;) {
    float temp = am2320.readTemperature();
    float humidity = am2320.readHumidity();

    if (isnan(temp) || isnan(humidity)) {
      Serial.println("[Sensor] Failed to read AM2320 sensor.");
    } else {
      latestTemp = temp;
      latestHumidity = humidity;
      latestTimestamp = getEpochTime();
      newSensorReadingAvailable = true;
      Serial.printf("[Sensor Read] Temp: %.2f, Humidity: %.2f\n", temp, humidity);
    }
    
    vTaskDelay(pdMS_TO_TICKS(5000)); 
  }
}

void TaskSDSync(void* pvParameters) {
  for (;;) {
    if (newSensorReadingAvailable) {
      float temp = latestTemp;
      float humidity = latestHumidity;
      unsigned long ts = latestTimestamp;
      newSensorReadingAvailable = false;

      String packet = "TS: " + String(ts) + ", Temperature: " + String(temp, 2) + ", Humidity: " + String(humidity, 2);

      if (isConnected && !hasPendingSDData) {
        if (mqtt.publish("room/data", packet.c_str())) {
          Serial.println("[Direct MQTT] Published directly: " + packet);
        }
      } 
      else {
        if (SD.begin(chipSelect)) {
          File writeFile = SD.open("/sensor_data.txt", FILE_APPEND);
          if (writeFile) {
            writeFile.println(packet);
            writeFile.close();
            hasPendingSDData = true;
            Serial.println("[SD Append] Appended new sensor reading to SD backlog.");
          } else {
            Serial.println("[SD Error] Failed to open file on SD card.");
          }

          if (isConnected && hasPendingSDData) {
            Serial.println("[SD Sync] Online restored! Syncing full SD backlog to MQTT...");
            File readFile = SD.open("/sensor_data.txt", FILE_READ);

            if (readFile) {
              while (readFile.available() && isConnected) {
                String line = readFile.readStringUntil('\n');
                line.trim();

                if (line.length() > 0) {
                  if (mqtt.publish("room/data", line.c_str())) {
                    Serial.println("[SD Sync] Published: " + line);
                  }
                }
                vTaskDelay(pdMS_TO_TICKS(100));
              }
              readFile.close();

              SD.remove("/sensor_data.txt");
              hasPendingSDData = false;
              Serial.println("[SD Sync] Backlog sync complete. SD file removed.");
            }
          }

          SD.end();
        } else {
          Serial.println("[SD Mount Error] Failed to mount SD card inside loop.");
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(500)); 
  }
}

void setup() {
  Serial.begin(115200);
  Wire.begin();
  am2320.begin();

  pinMode(LED_PIN, OUTPUT);

  if (SD.begin(chipSelect)) {
    if (SD.exists("/sensor_data.txt")) {
      hasPendingSDData = true;
      Serial.println("[Boot Check] Existing offline file found on SD card.");
    }
    SD.end();
  }

  xTaskCreatePinnedToCore(TaskConnectivity, "TaskConnectivity", 4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(TaskSensor, "TaskSensor", 4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(TaskSDSync, "TaskSync", 4096, NULL, 1, NULL, 1);
}

void loop() {

}