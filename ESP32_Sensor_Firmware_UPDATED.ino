// ✅ Оновлений код ESP32 з підтримкою BLE reset, LCD статусу, ChipId, imageName, username

#include <Wire.h>
#include <DHT.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Arduino_JSON.h>
#include <SPI.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
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

const char* serverName = "http://192.168.249.32:5210/api/sensordata";
bool statusDisplayed = true;
String uniqueId;
String savedSSID = "";
String savedPass = "";
bool bleConfigured = false;

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
    int roomId = int(data["roomId"]);
    String ssid = (const char*)data["ssid"];
    String encryptedPassword = (const char*)data["password"];
    String roomName = (const char*)data["roomName"];

    preferences.begin("config", false);
    preferences.putInt("RoomId", roomId);
    preferences.putString("ssid", ssid);
    preferences.putString("enc_pwd", encryptedPassword);
    preferences.putString("username", username);
    preferences.putString("imageName", imageName);
    preferences.putString("RoomName", roomName);
    preferences.putBool("configured", true);
    //preferences.end();

    Serial.println("=== PREF CHECK ===");
    Serial.println("RoomId: " + String(preferences.getInt("RoomId", -1)));
    Serial.println("ssid: " + preferences.getString("ssid", "none"));
    Serial.println("enc_pwd: " + preferences.getString("enc_pwd", "none"));
    Serial.println("username: " + preferences.getString("username", "none"));
    Serial.println("imageName: " + preferences.getString("imageName", "none"));
    Serial.println("RoomName: " + preferences.getString("RoomName", "none"));
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
  lcd.print("T:"); lcd.print(tempC); lcd.print("C H:"); lcd.print(humi); lcd.print("%");

  lcd.setCursor(0, 1);
  lcd.print("P:"); lcd.print(bmePressure); lcd.print("hPa");

  lcd.setCursor(0, 2);
  lcd.print("Gas:"); lcd.print(smokeState == LOW ? "Yes" : "No");
  lcd.print(" Light:"); lcd.print(lightState == HIGH ? "Dark" : "Light");

  lcd.setCursor(0, 3);
  String roomName = preferences.getString("RoomName", "NoRoom");
  lcd.print(roomName.substring(0, 20));
}
void setup() {
  Serial.begin(115200);
  preferences.begin("config", false);
  bleConfigured = preferences.getBool("configured", false);
  preferences.end();

  uint64_t chipid = ESP.getEfuseMac();
  char id[13];
  sprintf(id, "%04X%08X", (uint32_t)(chipid >> 32), (uint32_t)chipid);
  uniqueId = String(id);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  if (!bleConfigured) {
    lcd.print("BLE config mode");
  } else {
    lcd.print("Init sensors...");
  }

  dht.begin();
  pinMode(Smoke_PIN, INPUT); pinMode(Light_PIN, INPUT);
  delay(2000);
  if (!bme.begin(0x76)) while (1);

  if (bleConfigured) scanAndConnectWiFi();

  BLEDevice::init("ESP32_SENSOR");
  BLEServer *pServer = BLEDevice::createServer();
  BLEService *pService = pServer->createService(SERVICE_UUID);
  pCharacteristic = pService->createCharacteristic(CHARACTERISTIC_UUID, BLECharacteristic::PROPERTY_WRITE);
  pCharacteristic->setCallbacks(new MyCallbacks());
  pService->start();
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->start();
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

  Serial.println("WiFi SSID: " + savedSSID);
  Serial.println("WiFi PASS: " + String(savedPass.length(), '*'));
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
    int RoomId = preferences.getInt("RoomId", 0);
    String username = preferences.getString("username", "");
    String imageName = preferences.getString("imageName", "");
    preferences.end();

    JSONVar payload;
    payload["Username"] = username;
    payload["ChipId"] = uniqueId;
    payload["ImageName"] = imageName;
    payload["RoomId"] = RoomId;
    payload["TemperatureDht"] = tempC;
    payload["HumidityDht"] = humi;
    payload["TemperatureBme"] = bmeTemp;
    payload["HumidityBme"] = bmeHumi;
    payload["Pressure"] = bmePressure;
    payload["Altitude"] = bmeAltitude;
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
