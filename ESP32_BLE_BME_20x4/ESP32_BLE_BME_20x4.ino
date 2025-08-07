// ✅ Оновлений код ESP32 з підтримкою BLE reset, LCD статусу, ChipId, imageName, username

#include <Wire.h>
#include <DHT.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Arduino_JSON.h>
#include <SPI.h>
#include <Adafruit_BME280.h>
#include <Adafruit_Sensor.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <AESLib.h>

// === Піни ESP32 ===
#define DHT_PIN     4
#define Smoke_PIN   25
#define Light_PIN   26
#define I2C_SDA_PIN 21
#define I2C_SCL_PIN 22
#define MQ2_ANALOG_PIN 34    
#define LIGHT_ANALOG_PIN 35

// === LCD (I2C) ===
#define LCD_ADDRESS 0x27
#define LCD_WIDTH   20
#define LCD_HEIGHT  4
LiquidCrystal_I2C lcd(LCD_ADDRESS, LCD_WIDTH, LCD_HEIGHT);

#define SEALEVELPRESSURE_HPA (1013.25)
Adafruit_BME280 bme;
DHT dht(DHT_PIN, DHT11);

const char* serverName = "http://192.168.0.200:5210/api/sensordata";
bool statusDisplayed = true;
String uniqueId;
String savedSSID = "";
String savedPass = "";
String bleName;
bool bleConfigured = false;
bool bmpDetected = true;

AESLib aesLib;

// === AES ===
// AES ключ — такий самий, як у Android ("my-secret-key-12")
byte aes_key[] = {
  'm', 'y', '-', 's', 'e', 'c', 'r', 'e',
  't', '-', 'k', 'e', 'y', '-', '1', '2'
};

// IV не використовується для ECB, але AESLib вимагає його передати
byte aes_iv[] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

String decryptPassword(String encrypted) {
  int inputLength = encrypted.length() + 1;
  char input[inputLength];
  encrypted.toCharArray(input, inputLength);

  byte decrypted[128];  // буфер для розшифрованих даних
  int len = aesLib.decrypt64(input, inputLength, decrypted, aes_key, 128, aes_iv);

  if (len <= 0) {
    Serial.println("❌ Помилка дешифрування!");
    return "";
  }

  decrypted[len] = '\0';
  String result = String((char*)decrypted);

  // Видаляємо PKCS7 паддінг
  int pad = decrypted[len - 1];  // останній байт — кількість байтів паддінгу
  if (pad > 0 && pad <= 16) {
    result.remove(result.length() - pad);
  }

  return result;
}

// === BLE ===
Preferences preferences;
BLECharacteristic *pCharacteristic;
#define SERVICE_UUID        "12345678-1234-1234-1234-123456789abc"
#define CHARACTERISTIC_UUID "abcd1234-5678-1234-5678-abcdef123456"

class MyCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    String jsonStr = String(pCharacteristic->getValue().c_str());
    Serial.println("Отримано BLE JSON:");
    Serial.println(jsonStr);

    JSONVar data = JSON.parse(jsonStr);
    if (JSON.typeof(data) == "undefined") {
      Serial.println("JSON невірний");
      return;
    }

    // 🔄 RESET
    bool shouldReset = data.hasOwnProperty("reset") && (bool)data["reset"];
    if (shouldReset) {
      Serial.println("🔄 Очищення Preferences через reset=true");
      preferences.begin("config", false);
      preferences.clear();
      preferences.end();
      delay(100); // коротка затримка для стабільності
    }

    // ✅ Збереження нових параметрів
    String username = (const char*)data["username"];
    String imageName = (const char*)data["imageName"];
    String ssid = (const char*)data["ssid"];
    String encryptedPassword = (const char*)data["password"];
    String roomName = (const char*)data["roomName"];

    preferences.begin("config", false);
    preferences.putString("ssid", ssid);
    preferences.putString("enc_pwd", encryptedPassword);
    preferences.putString("username", username);
    preferences.putString("imageName", imageName);
    preferences.putString("roomName", roomName);
    preferences.putBool("configured", true);

    // 🔎 Дебаг
    Serial.println("=== PREF CHECK ===");
    Serial.println("ssid: " + ssid);
    Serial.println("enc_pwd: " + encryptedPassword);
    Serial.println("username: " + username);
    Serial.println("imageName: " + imageName);
    Serial.println("roomName: " + roomName);
    Serial.println("reset: " + String(shouldReset));
    Serial.println("===================");
    preferences.end();

    bleConfigured = true;
    Serial.println("✅ Конфігурація збережена!");
  }
};


// === Таймер ===
unsigned long lastTime = 0, displayRefreshTime = 0, wifiCheckTime = 0;
const long timerDelay = 10000, displayRefreshInterval = 10000, wifiCheckInterval = 60000;
wl_status_t lastWiFiStatus = WL_IDLE_STATUS;

void scanAndConnectWiFi() {
  Serial.println("Сканування Wi-Fi...");
  int n = WiFi.scanNetworks();
  if (n == 0) {
    Serial.println("Wi-Fi мереж не знайдено");
    return;
  }

  preferences.begin("config", false);
  savedSSID = preferences.getString("ssid", "");
  String encrypted = preferences.getString("enc_pwd", "");
  savedPass = decryptPassword(encrypted);
  preferences.end();

  Serial.println("🔐 Зашифрований пароль: " + encrypted);
  Serial.println("🔓 Дешифровано пароль: " + savedPass);

  if (savedSSID == "" || savedPass == "") {
    Serial.println("❌ Немає збереженого SSID або пароля. Пропуск Wi-Fi підключення.");
    return;
  }
  preferences.end();

  for (int i = 0; i < n; ++i) {
    String ssid = WiFi.SSID(i);
    if (ssid == savedSSID) {
      WiFi.begin(savedSSID.c_str(), savedPass.c_str());
      unsigned long startAttempt = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 15000) {
        delay(500);
      }
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("Wi-Fi OK!");
        Serial.println(WiFi.localIP());
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("IP: ");
        lcd.print(WiFi.localIP());
        delay(3000);
        return;
      }
    }
  }

  Serial.println("Не знайдено збереженої мережі");
}

void updateDisplay(float tempC, float humi, int smokeState, int lightState, float bmePressure) {
  lcd.clear();
  if (statusDisplayed) {
    lcd.setCursor(0, 0);
    lcd.print("BLE:");
    lcd.print(bleConfigured ? "OK " : "WAIT");
    lcd.print(" WiFi:");
    lcd.print(WiFi.status() == WL_CONNECTED ? "OK" : "NO");

    if (bleConfigured && WiFi.status() == WL_CONNECTED) {
      statusDisplayed = false;
    }
    return;
  }

  lcd.setCursor(0, 0);
  if (isnan(tempC) || isnan(humi)) {
    lcd.print("Temp/Hum: ERROR");
  } else {
    lcd.print("T:"); lcd.print(tempC); lcd.print(" H:"); lcd.print(humi);
  }

  lcd.setCursor(0, 1);
  if (!bmpDetected || isnan(bmePressure)) {
    lcd.print("Pres: ERROR");
  } else {
    lcd.print("P:"); lcd.print(bmePressure); lcd.print("hPa");
  }

  lcd.setCursor(0, 2);
  lcd.print("Gas:"); lcd.print(smokeState == LOW ? "Yes" : "No");
  lcd.print(" Light:"); lcd.print(lightState == HIGH ? "Dark" : "Light");

  lcd.setCursor(0, 3);
  String roomName = preferences.getString("roomName", "NoRoom");
  lcd.print(roomName.substring(0, 20));
}

void setup() {
  Serial.begin(115200);

  // === Ідентифікатор та BLE-ім’я ===
  uint64_t chipid = ESP.getEfuseMac();
  char id[13];
  sprintf(id, "%04X%08X", (uint32_t)(chipid >> 32), (uint32_t)chipid);
  uniqueId = String(id);
  String shortId = uniqueId.substring(uniqueId.length() - 6);
  bleName = "ESP32_" + shortId;

  // === Ініціалізація BLE ===
  BLEDevice::init(bleName.c_str());
  Serial.println("=== BLE ІНІЦІАЛІЗАЦІЯ ===");
  Serial.println("ChipId: " + uniqueId);
  Serial.println("BLE Name: " + bleName);

  BLEServer *pServer = BLEDevice::createServer();
  BLEService *pService = pServer->createService(SERVICE_UUID);
  pCharacteristic = pService->createCharacteristic(CHARACTERISTIC_UUID, BLECharacteristic::PROPERTY_WRITE);
  pCharacteristic->setCallbacks(new MyCallbacks());
  pService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);  // необов’язково, але допомагає

  BLEDevice::startAdvertising();
  Serial.println("BLE advertising started!");
  Serial.println("==========================");

  // === Preferences ===
  preferences.begin("config", false);
  bleConfigured = preferences.getBool("configured", false);
  preferences.end();

  // === Дисплей, сенсори ===
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print(bleConfigured ? "Init sensors..." : "BLE config mode");

  dht.begin();
  pinMode(Smoke_PIN, INPUT);
  pinMode(Light_PIN, INPUT);
  delay(2000);

  if (!bme.begin(0x76)) {
    Serial.println("Помилка BME280");
    bmpDetected = false;
  }

  // === Wi-Fi ===
  if (bleConfigured) {
    scanAndConnectWiFi();
  }
}


void checkWiFiConnection() {
  wl_status_t currentStatus = WiFi.status();
  if (millis() - wifiCheckTime >= wifiCheckInterval) {
    if (currentStatus != WL_CONNECTED) {
      WiFi.disconnect();
      scanAndConnectWiFi();
    }
    wifiCheckTime = millis();
  }
}

void loop() {

  checkWiFiConnection();

  if (millis() - displayRefreshTime >= displayRefreshInterval) {
    float tempC = dht.readTemperature();
    float humi = dht.readHumidity();
    int smokeState = digitalRead(Smoke_PIN);
    int lightState = digitalRead(Light_PIN);
    float bmePressure = bme.readPressure() / 100.0F;
    updateDisplay(tempC, humi, smokeState, lightState, bmePressure);
    displayRefreshTime = millis();
  }

  if ((millis() - lastTime) > timerDelay && WiFi.status() == WL_CONNECTED && bleConfigured) {
    float tempC = dht.readTemperature();
    float humi = dht.readHumidity();
    int smokeState = digitalRead(Smoke_PIN);
    int lightState = digitalRead(Light_PIN);
    float bmeTemp = bme.readTemperature();
    float bmePressure = bme.readPressure() / 100.0F;
    float bmeHumi = bme.readHumidity();
    float bmeAltitude = bme.readAltitude(SEALEVELPRESSURE_HPA);
    int mq2AnalogValue = analogRead(MQ2_ANALOG_PIN);
    int lightAnalogValue = analogRead(LIGHT_ANALOG_PIN);
    float mq2Percent = mq2AnalogValue * 100.0 / 4095.0;
    float lightPercent = 100 - (lightAnalogValue * 100.0 / 4095.0);

    preferences.begin("config", false);
    String username = preferences.getString("username", "");
    String imageName = preferences.getString("imageName", "");
    String roomName = preferences.getString("roomName", "");
    preferences.end();

    String json = "{";

    json += "\"Username\":\"" + String(username) + "\",";
    json += "\"ChipId\":" + String(uniqueId) + ",";
    json += "\"ImageName\":\"" + String(imageName) + "\",";
    json += "\"RoomName\":\"" + String(roomName) + "\",";

    // TemperatureDht
    json += "\"TemperatureDht\":";
    json += isnan(tempC) ? "null" : String(tempC, 2);
    json += ",";

    // HumidityDht
    json += "\"HumidityDht\":";
    json += isnan(humi) ? "null" : String(humi, 2);
    json += ",";

    // TemperatureBme
    json += "\"TemperatureBme\":";
    json += (bmpDetected && !isnan(bmeTemp)) ? String(bmeTemp, 2) : "null";
    json += ",";

    // HumidityBme — завжди null
    json += "\"HumidityBme\":";
    json += (bmpDetected && !isnan(bmeHumi)) ? String(bmeHumi, 2) : "null";
    json += ",";
    
    // Pressure
    json += "\"Pressure\":";
    json += (bmpDetected && !isnan(bmePressure)) ? String(bmePressure, 2) : "null";
    json += ",";

    // Altitude
    json += "\"Altitude\":";
    json += (bmpDetected && !isnan(bmeAltitude)) ? String(bmeAltitude, 2) : "null";
    json += ",";

    // GasDetected
    json += "\"GasDetected\":\"";
    json += (smokeState == LOW) ? "yes" : "no";
    json += "\",";

    // Light
    json += "\"Light\":\"";
    json += (lightState == HIGH) ? "dark" : "light";
    json += "\",";

    // MQ2Analog
    json += "\"MQ2Analog\":" + String(mq2AnalogValue, 2) + ",";

    // MQ2AnalogPercent
    json += "\"MQ2AnalogPercent\":" + String(mq2Percent, 2) + ",";

    // LightAnalog
    json += "\"LightAnalog\":" + String(lightAnalogValue, 2) + ",";

    // LightAnalogPercent
    json += "\"LightAnalogPercent\":" + String(lightPercent, 2);

    // кінець JSON
    json += "}";

    // 🔹 Відправлення
    HTTPClient http;
    http.begin(serverName);
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(json);

    if (code > 0) Serial.println("POST OK: " + String(code));
    else Serial.println("POST ERR: " + String(code));

    http.end();
    lastTime = millis();

  }
}
