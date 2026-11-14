#include <WiFi.h>
#include <SD.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_AM2320.h>
#include <SPI.h>
#include <time.h>
#include <Preferences.h>
#include <NimBLEDevice.h>

#define SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"

#define LED_PIN 2
#define BLE_BUTTON_PIN 4
const int chipSelect = 5;

const char* mqtt_server = "192.168.0.103";
const int MQTT_PORT = 1883;
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 18000;
const int daylightOffset_sec = 0;

String wifi_ssid = "";
String wifi_password = "";

Adafruit_AM2320 am2320 = Adafruit_AM2320();
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
Preferences preferences;

portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;
volatile float latestTemp = 0.0;
volatile float latestHumidity = 0.0;
volatile bool newSensorReadingAvailable = false;
volatile bool isConnected = false;
volatile bool hasPendingSDData = false;
volatile bool newWifiCredentialsReceived = false;
volatile bool buttonPressedFlag = false;
String latestTimestamp = "";

bool bleActive = true;
unsigned long bleStartTime = 0;
const unsigned long BLE_TIMEOUT_MS = 30000;
volatile unsigned long lastDebounceTime = 0;

void IRAM_ATTR handleInterrupt() {
  buttonPressedFlag = true;
}

String getFormattedNativeTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "Offline Mode";
  }
  char timeStringBuff[10];
  strftime(timeStringBuff, sizeof(timeStringBuff), "%H:%M:%S", &timeinfo);
  return String(timeStringBuff);
}

void startBLE() {

  NimBLEDevice::getAdvertising()->start();
  bleStartTime = millis();

  bleActive = true;
  Serial.println("[BLE] Force restarted! Advertising active for 30s...");
}

void stopBLE() {
  if (bleActive) {
    NimBLEDevice::getAdvertising()->stop();
    bleActive = false;
    Serial.println("[BLE] Advertising STOPPED.");
  }
}

class DataReceiver : public NimBLECharacteristicCallbacks {
public:
  void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
    String text = pCharacteristic->getValue().c_str();
    text.trim();

    int colon = text.indexOf(':');
    if (colon != -1) {
      String newSSID = text.substring(0, colon);
      String newPass = text.substring(colon + 1);
      newSSID.trim();
      newPass.trim();

      if (newSSID.length() > 0) {
        preferences.begin("wifi_config", false);
        preferences.putString("ssid", newSSID);
        preferences.putString("password", newPass);
        preferences.end();

        portENTER_CRITICAL(&timerMux);
        wifi_ssid = newSSID;
        wifi_password = newPass;
        newWifiCredentialsReceived = true;
        portEXIT_CRITICAL(&timerMux);

        Serial.println("[BLE] New Wi-Fi credentials received & saved!");
        pCharacteristic->setValue("Status: Credentials Saved! Connecting...");
      }
    } else {
      Serial.println("[BLE] Format Error! Missing ':' delimiter.");
      pCharacteristic->setValue("Error! Format must be 'WiFiName:Password'");
    }
  }
};

void TaskConnectivity(void* pvParameters) {
  for (;;) {
    if (buttonPressedFlag) {
      buttonPressedFlag = false;
      Serial.println("[BUTTON DETECTED] GPIO 4 triggered!");
      startBLE();
    }

    bool credentialsUpdated = false;

    portENTER_CRITICAL(&timerMux);
    if (newWifiCredentialsReceived) {
      credentialsUpdated = true;
      newWifiCredentialsReceived = false;
    }
    portEXIT_CRITICAL(&timerMux);

    if (credentialsUpdated || WiFi.status() != WL_CONNECTED) {
      if (wifi_ssid.length() > 0) {
        digitalWrite(LED_PIN, LOW);
        isConnected = false;

        if (credentialsUpdated) {
          WiFi.disconnect();
          Serial.printf("[Connectivity] Switched to new Network: %s\n", wifi_ssid.c_str());
        } else {
          Serial.printf("[Connectivity] Reconnecting to %s...\n", wifi_ssid.c_str());
        }

        WiFi.begin(wifi_ssid.c_str(), wifi_password.c_str());

        int attempt = 0;
        while (WiFi.status() != WL_CONNECTED && attempt < 10) {
          if (buttonPressedFlag) {
            buttonPressedFlag = false;
            Serial.println("[BUTTON DETECTED] Force extending BLE window...");
            startBLE();
          }
          vTaskDelay(pdMS_TO_TICKS(500));
          attempt++;
        }
      } else {
        Serial.println("[Connectivity] No Wi-Fi credentials found! Press button on GPIO 4 to enable BLE.");
        startBLE();
      }
    }

    if (WiFi.status() == WL_CONNECTED) {
      if (!mqtt.connected()) {
        mqtt.setServer(mqtt_server, MQTT_PORT);
        Serial.print("[MQTT] Connecting to broker...");
        if (mqtt.connect("esp32-am2320")) {
          Serial.println(" Connected!");
        } else {
          Serial.printf(" Failed, rc=%d\n", mqtt.state());
        }
      }
    }

    if (bleActive && (millis() - bleStartTime > BLE_TIMEOUT_MS)) {
      Serial.println("[BLE] 30s timeout reached.");
      stopBLE();
    }

    if (mqtt.connected()) {
      mqtt.loop();
      digitalWrite(LED_PIN, HIGH);
      isConnected = true;
    } else {
      digitalWrite(LED_PIN, LOW);
      isConnected = false;
    }

    vTaskDelay(pdMS_TO_TICKS(200));
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

      if (WiFi.status() == WL_CONNECTED) {
        latestTimestamp = getFormattedNativeTime();
      } else {
        latestTimestamp = "Offline Mode";
      }

      newSensorReadingAvailable = true;
      Serial.printf("[Sensor Read] Time: %s | Temp: %.2f, Humidity: %.2f\n", latestTimestamp.c_str(), temp, humidity);
    }

    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

void TaskSDSync(void* pvParameters) {
  for (;;) {
    if (newSensorReadingAvailable) {
      float temp = latestTemp;
      float humidity = latestHumidity;
      String timestamp = latestTimestamp;
      newSensorReadingAvailable = false;

      String packet = "Time: " + timestamp + ", Temperature: " + String(temp, 2) + ", Humidity: " + String(humidity, 2);

      if (isConnected && !hasPendingSDData) {
        if (mqtt.publish("room/data", packet.c_str())) {
          Serial.println("[Direct MQTT] Published directly: " + packet);
        }
      } else {
        File writeFile = SD.open("/sensor_data.txt", FILE_APPEND);
        if (writeFile) {
          writeFile.println(packet);
          writeFile.close();
          hasPendingSDData = true;
          Serial.println("[SD Append] Saved offline data packet to SD.");
        } else {
          Serial.println("[SD Error] Failed to open file for writing.");
        }
      }
    }

    if (isConnected && hasPendingSDData) {
      Serial.println("[SD Sync] Online connection restored! Flushing SD backlog to MQTT...");
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
        Serial.println("[SD Sync] Backlog synced successfully. Local log purged.");
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

  pinMode(BLE_BUTTON_PIN, INPUT_PULLDOWN);
  attachInterrupt(digitalPinToInterrupt(BLE_BUTTON_PIN), handleInterrupt, FALLING);
  Serial.println("--- Button Test Ready. Press button on GPIO 4 -");

  preferences.begin("wifi_config", true);
  wifi_ssid = preferences.getString("ssid", "");
  wifi_password = preferences.getString("password", "");
  preferences.end();

  if (wifi_ssid.length() > 0) {
    Serial.printf("[Boot] Found saved Wi-Fi: %s\n", wifi_ssid.c_str());
  } else {
    Serial.println("[Boot] No saved Wi-Fi credentials found.");
  }

  if (SD.begin(chipSelect)) {
    if (SD.exists("/sensor_data.txt")) {
      hasPendingSDData = true;
      Serial.println("[Boot Check] Backlog file found on SD card.");
    }
  } else {
    Serial.println("[SD Init] SD Card initialization failed!");
  }

  NimBLEDevice::init("My_ESP32");
  NimBLEServer* pServer = NimBLEDevice::createServer();
  NimBLEService* pService = pServer->createService(SERVICE_UUID);
  NimBLECharacteristic* pRxCharacteristic = pService->createCharacteristic(CHARACTERISTIC_UUID_RX, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);

  pRxCharacteristic->setValue("Format = WiFiName:Password");
  pRxCharacteristic->setCallbacks(new DataReceiver());
  pService->start();

  NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);

  bleStartTime = millis();
  pAdvertising->start();

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  xTaskCreatePinnedToCore(TaskConnectivity, "TaskConnectivity", 4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(TaskSensor, "TaskSensor", 4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(TaskSDSync, "TaskSync", 4096, NULL, 1, NULL, 1);
}

void loop() {
}