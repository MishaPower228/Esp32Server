// ✅ Оновлений код ESP32 з підтримкою BLE reset, LCD статусу, ChipId, imageName, username

#include <Wire.h>
#include <DHT.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Arduino_JSON.h>
#include <SPI.h>
#include <Adafruit_BMP280.h>
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
#define LCD_ADDRESS 0x3f
#define LCD_WIDTH   16
#define LCD_HEIGHT  2
LiquidCrystal_I2C lcd(LCD_ADDRESS, LCD_WIDTH, LCD_HEIGHT);

#define SEALEVELPRESSURE_HPA (1013.25)
Adafruit_BMP280 bmp;
DHT dht(DHT_PIN, DHT11);

const char* serverName = "http://192.168.249.32:5210/api/sensordata";
bool statusDisplayed = true;
String uniqueId;
String savedSSID = "";
String savedPass = "";
String bleName;
bool bleConfigured = false;
bool bmpDetected = true;

// === AES ===
AESLib aesLib;
byte aes_key[] = { 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16 };
byte aes_iv[]  = { 1,2,3,4,5,6,7,8,1,2,3,4,5,6,7,8 };

String decryptPassword(String encrypted) {
  int inputLength = encrypted.length() + 1;
  char input[inputLength];
  encrypted.toCharArray(input, inputLength);
  byte output[128];
  int len = aesLib.decrypt64(input, inputLength, output, aes_key, 128, aes_iv);
  output[len] = '\0';
  return String((char*)output);
}

// === BLE ===
Preferences preferences;
BLECharacteristic *pCharacteristic;
#define SERVICE_UUID        "12345678-1234-1234-1234-123456789abc"
#define CHARACTERISTIC_UUID "abcd1234-5678-1234-5678-abcdef123456"

class MyCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    String jsonStr = String(pCharacteristic->getValue().c_str());
    JSONVar data = JSON.parse(jsonStr);
  // ✅ Обробка reset-команди
  if (data.hasOwnProperty("reset") && (bool)data["reset"] == true) {
    preferences.begin("config", false);
    preferences.clear();
    preferences.end();
    Serial.println("Preferences очищено. Перезапуск...");
    delay(500);
    ESP.restart();
    return;
  }

    if (JSON.typeof(data) == "undefined") return;

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
    //preferences.end();

    Serial.println("=== PREF CHECK ===");
    Serial.println("ssid: " + preferences.getString("ssid", "none"));
    Serial.println("enc_pwd: " + preferences.getString("enc_pwd", "none"));
    Serial.println("username: " + preferences.getString("username", "none"));
    Serial.println("imageName: " + preferences.getString("imageName", "none"));
    Serial.println("roomName: " + preferences.getString("roomName", "none"));
    Serial.println("configured: " + String(preferences.getBool("configured", false)));
    Serial.println("===================");
    preferences.end();

    bleConfigured = true;
    Serial.println("BLE збережено!");
    Serial.println("=== BLE Data ===");
    Serial.println(jsonStr);
    Serial.println("================");
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
  savedPass = decryptPassword(preferences.getString("enc_pwd", ""));
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

  if (!bmp.begin(0x76)) {
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
    float bmePressure = bmp.readPressure() / 100.0F;
    updateDisplay(tempC, humi, smokeState, lightState, bmePressure);
    displayRefreshTime = millis();
  }

  if ((millis() - lastTime) > timerDelay && WiFi.status() == WL_CONNECTED && bleConfigured) {
    float tempC = dht.readTemperature();
    float humi = dht.readHumidity();
    int smokeState = digitalRead(Smoke_PIN);
    int lightState = digitalRead(Light_PIN);
    float bmeTemp = bmp.readTemperature();
    float bmePressure = bmp.readPressure() / 100.0F;
    //float bmeHumi = bme.readHumidity();
    float bmeAltitude = bmp.readAltitude(SEALEVELPRESSURE_HPA);
    int mq2AnalogValue = analogRead(MQ2_ANALOG_PIN);
    int lightAnalogValue = analogRead(LIGHT_ANALOG_PIN);
    float mq2Percent = mq2AnalogValue * 100.0 / 4095.0;
    float lightPercent = 100 - (lightAnalogValue * 100.0 / 4095.0);

    preferences.begin("config", false);
    String username = preferences.getString("username", "");
    String imageName = preferences.getString("imageName", "");
    String roomName = preferences.getString("roomName", "");
    preferences.end();

    JSONVar payload;
    payload["Username"] = username;
    payload["ChipId"] = uniqueId;
    payload["ImageName"] = imageName;
    payload["roomName"] = roomName;
    payload["TemperatureDht"] = isnan(tempC) ? JSON::null : tempC;
    payload["HumidityDht"] = isnan(humi) ? JSON::null : humi;
    payload["TemperatureBme"] = (bmpDetected && !isnan(bmeTemp)) ? bmeTemp : JSON::null;
    payload["HumidityBme"] = JSON::null;
    payload["Pressure"] = (bmpDetected && !isnan(bmePressure)) ? bmePressure : JSON::null;
    payload["Altitude"] = (bmpDetected && !isnan(bmeAltitude)) ? bmeAltitude : JSON::null;
    payload["GasDetected"] = (smokeState == LOW ? "yes" : "no");
    payload["Light"] = (lightState == HIGH ? "dark" : "light");
    payload["MQ2Analog"] = mq2AnalogValue;
    payload["MQ2AnalogPercent"] = mq2Percent;
    payload["LightAnalog"] = lightAnalogValue;
    payload["LightAnalogPercent"] = lightPercent;

    String json = JSON.stringify(payload);
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
