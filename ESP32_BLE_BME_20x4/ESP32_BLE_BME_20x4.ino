/*  ===========================================================================
    ESP32 ENV NODE (20x4 LCD, BME280 + DHT11, BLE конфіг, Wi-Fi, HTTP)
    - BLE write JSON (full config або Wi-Fi patch) без затирання відсутніх полів
    - Негайний reconnect Wi-Fi після patch
    - Збереження у NVS (Preferences)
    - NOTIFY: повертає ChipId у відповідь на BLE write
    - GET /api/SensorData/sync/{chipId} з If-None-Match + X-Api-Key (ETag кеш)
    - POST /api/SensorData кожні 10 хв з X-Api-Key
   =========================================================================== */

#include <Wire.h>
#include <DHT.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Arduino_JSON.h>
#include <SPI.h>
#include <Adafruit_BME280.h>
#include <Adafruit_Sensor.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <AESLib.h>
#include <BLE2902.h>
#include <BLESecurity.h>

#define DHT_PIN          4
#define Smoke_PIN        25
#define Light_PIN        26
#define I2C_SDA_PIN      21
#define I2C_SCL_PIN      22
#define MQ2_ANALOG_PIN   34
#define LIGHT_ANALOG_PIN 35
#define DHT_TYPE DHT11

// LCD 20x4 (I2C)
#define LCD_ADDRESS 0x27
#define LCD_WIDTH   20
#define LCD_HEIGHT  4
LiquidCrystal_I2C lcd(LCD_ADDRESS, LCD_WIDTH, LCD_HEIGHT);

// Датчики
#define SEALEVELPRESSURE_HPA (1013.25)
Adafruit_BME280 bme;
DHT dht(DHT_PIN, DHT_TYPE);

// ───────────── HTTP: єдина база для всіх ендпоінтів ─────────────
const char* API_BASE = "http://192.168.0.200:5210/api/SensorData"; // ← заміни на свій домен

// ───────────── Глобальні стани ─────────────
bool   statusDisplayed = true;
String uniqueId, bleName;
bool   bleConfigured = false;
bool   bmeDetected   = true;

AESLib aesLib;
Preferences preferences;

// ───────────── AES (як в Android) ─────────────
byte aes_key[] = { 'm','y','-','s','e','c','r','e','t','-','k','e','y','-','1','2' }; // 16B AES-128
byte aes_iv[]  = { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };

// ───────────── BLE ─────────────
#define SERVICE_UUID        "12345678-1234-1234-1234-123456789abc"
#define CHARACTERISTIC_UUID "abcd1234-5678-1234-5678-abcdef123456"
BLECharacteristic *pCharacteristic = nullptr;
BLEServer* gServer = nullptr; // для рестарту реклами

// ───────────── Таймери ─────────────
unsigned long lastTime = 0, displayRefreshTime = 0, wifiCheckTime = 0, nextSyncAt = 0;
const unsigned long POST_INTERVAL_MS        = 10UL * 60UL * 1000UL; // 10 хв
const unsigned long LCD_REFRESH_MS          = 10UL * 1000UL;        // 10 с
const unsigned long WIFI_CHECK_MS           = 60UL * 1000UL;        // 60 с
const unsigned long SYNC_OK_PERIOD_MS       = 30UL * 60UL * 1000UL; // 30 хв
const unsigned long SYNC_FAIL_PERIOD_MS     = 5UL  * 60UL * 1000UL; // 5 хв

// ───────────── Утиліти ─────────────
String decryptPassword(const String& encrypted) {
  int inputLength = encrypted.length() + 1;
  char input[inputLength];
  encrypted.toCharArray(input, inputLength);

  byte out[256];
  int len = aesLib.decrypt64(input, inputLength, out, aes_key, 128, aes_iv);
  if (len <= 0) return "";
  out[len] = '\0';
  String res = String((char*)out);

  // strip PKCS7
  if (len > 0) {
    int pad = out[len - 1];
    if (pad > 0 && pad <= 16 && pad <= res.length()) res.remove(res.length() - pad);
  }
  return res;
}

static bool putIfChanged(Preferences& p, const char* key, const String& v) {
  String old = p.getString(key, "");
  if (v != old) { p.putString(key, v); return true; }
  return false;
}

// ───────────── Wi-Fi ─────────────
bool reconnectWithSavedWifi() {
  preferences.begin("config", true);
  String ssid   = preferences.getString("ssid", "");
  String encPwd = preferences.getString("enc_pwd", "");
  preferences.end();

  String pass = decryptPassword(encPwd);
  if (ssid.isEmpty() || pass.isEmpty()) return false;

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(200);
  WiFi.begin(ssid.c_str(), pass.c_str());

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) { delay(500); }

  return WiFi.status() == WL_CONNECTED;
}

void checkWiFiConnection() {
  if (millis() - wifiCheckTime >= WIFI_CHECK_MS) {
    if (WiFi.status() != WL_CONNECTED && bleConfigured) reconnectWithSavedWifi();
    wifiCheckTime = millis();
  }
}

// ───────────── SYNC: GET /api/SensorData/sync/{chipId} ─────────────
int syncMetadata() {
  if (WiFi.status() != WL_CONNECTED) return -1;

  String url = String(API_BASE) + "/sync/" + uniqueId;
  HTTPClient http;
  http.begin(url);

  preferences.begin("config", true);
  String etag  = preferences.getString("own_etag", "");
  String apiKey= preferences.getString("api_key", "");
  preferences.end();

  if (etag.length())  http.addHeader("If-None-Match", etag);
  if (apiKey.length()) http.addHeader("X-Api-Key", apiKey);

  int code = http.GET();

  if (code == 304) { // не змінилось
    http.end();
    preferences.begin("config", false);
    preferences.putBool("own_provisional", false);
    preferences.end();
    return 0;
  }
  if (code != 200) { http.end(); return -1; }

  String body    = http.getString();
  String newEtag = http.header("ETag");
  http.end();

  JSONVar obj = JSON.parse(body);
  if (JSON.typeof(obj) != "object") return -1;

  auto getIfPresent = [&](const char* k, bool& has) -> String {
    has = (JSON.typeof(obj[k]) != "undefined" && JSON.typeof(obj[k]) != "null");
    return has ? String((const char*)obj[k]) : String("");
  };

  bool hasU=false, hasR=false, hasI=false;
  String username = getIfPresent("username",  hasU);
  String roomName = getIfPresent("roomName",  hasR);
  String image    = getIfPresent("imageName", hasI);

  bool changed = false;
  preferences.begin("config", false);
  if (hasU) changed |= putIfChanged(preferences, "username",  username);
  if (hasR) changed |= putIfChanged(preferences, "roomName",  roomName);
  if (hasI) changed |= putIfChanged(preferences, "imageName", image);
  if (newEtag.length()) preferences.putString("own_etag", newEtag);
  preferences.putBool("own_provisional", false);
  preferences.end();

  return changed ? 1 : 0;
}

void scheduleNextSync(int rc) {
  unsigned long period = (rc >= 0) ? SYNC_OK_PERIOD_MS : SYNC_FAIL_PERIOD_MS;
  nextSyncAt = millis() + period;
}

// ───────────── LCD ─────────────
void updateDisplay(float t, float h, int gasPin, int lightPin, float p) {
  lcd.clear();

  if (statusDisplayed) {
    lcd.setCursor(0,0);
    lcd.print("BLE:");  lcd.print(bleConfigured ? "OK " : "WAIT");
    lcd.print(" WiFi:"); lcd.print(WiFi.status() == WL_CONNECTED ? "OK" : "NO");

    preferences.begin("config", true);
    bool hasKey = preferences.getString("api_key", "").length() > 0;
    preferences.end();
    lcd.setCursor(0,1);
    lcd.print("API:"); lcd.print(hasKey ? "OK " : "NO ");

    if (bleConfigured && WiFi.status() == WL_CONNECTED) {
      statusDisplayed = false;
      preferences.begin("config", true);
      String username = preferences.getString("username", "");
      preferences.end();
      if (username.length()) {
        lcd.setCursor(0,3);
        String hello = "Hello, " + username;
        if (hello.length() <= 20) lcd.print(hello);
        else {
          for (int i = 0; i <= hello.length() - 20; i++) {
            lcd.setCursor(0,3); lcd.print(hello.substring(i, i + 20)); delay(280);
          }
        }
      }
    }
    return;
  }

  lcd.setCursor(0,0);
  if (isnan(t) || isnan(h)) lcd.print("Temp/Hum: ERROR");
  else {
    lcd.print("Temp:"); lcd.print((int)t); lcd.write(223); lcd.print("C ");
    lcd.print("Hum:");  lcd.print((int)h);  lcd.print("%");
  }

  lcd.setCursor(0,1);
  if (!bmeDetected || isnan(p)) { lcd.print("Pres: ERROR"); }
  else { lcd.print("P:"); lcd.print(p); lcd.print("hPa"); }

  lcd.setCursor(0,2);
  lcd.print("Gas:");   lcd.print(gasPin   == LOW ? "Yes"  : "No ");
  lcd.print(" Light:"); lcd.print(lightPin == LOW ? "Light": "Dark");

  lcd.setCursor(0,3);
  preferences.begin("config", true);
  String roomName = preferences.getString("roomName", "NoRoom");
  preferences.end();
  if (roomName.length() <= 20) lcd.print(roomName); else lcd.print(roomName.substring(0,20));
}

// ───────────── BLE write → save JSON частково + notify ChipId ─────────────
class MyCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pChar) override {
    String s = String(pChar->getValue().c_str());
    JSONVar data = JSON.parse(s);
    if (JSON.typeof(data) == "undefined") return;

    // reset=true → очищення NVS
    bool needReset = (data.hasOwnProperty("reset") && (bool)data["reset"]);
    if (needReset) { preferences.begin("config", false); preferences.clear(); preferences.end(); delay(100); }

    auto getStr = [&](const char* k, bool &has) -> String {
      has = data.hasOwnProperty(k) && JSON.typeof(data[k]) != "undefined" && JSON.typeof(data[k]) != "null";
      return has ? String((const char*)data[k]) : String("");
    };

    bool hasSSID=false, hasPwd=false, hasU=false, hasI=false, hasR=false, hasK=false;
    String ssid   = getStr("ssid",       hasSSID);
    String encPwd = getStr("password",   hasPwd);
    String user   = getStr("username",   hasU);
    String img    = getStr("imageName",  hasI);
    String room   = getStr("roomName",   hasR);
    String key    = getStr("apiKey",     hasK);

    preferences.begin("config", false);
    if (hasSSID) preferences.putString("ssid",      ssid);
    if (hasPwd)  preferences.putString("enc_pwd",   encPwd);
    if (hasU)    preferences.putString("username",  user);
    if (hasI)    preferences.putString("imageName", img);
    if (hasR)    preferences.putString("roomName",  room);
    if (hasK && key.length() >= 16 && key.length() <= 128) preferences.putString("api_key", key);

    bool cfgReady = preferences.getString("ssid","").length() && preferences.getString("enc_pwd","").length();
    preferences.putBool("configured", cfgReady);
    preferences.end();

    bleConfigured = cfgReady;

    // відповідаємо chipId
    pChar->setValue(uniqueId.c_str());
    pChar->notify();

    // якщо оновили Wi-Fi — пробуємо одразу підключитись
    if (hasSSID && hasPwd) {
      String plain = decryptPassword(encPwd);
      if (plain.length()) {
        WiFi.mode(WIFI_STA); WiFi.disconnect(true,true); delay(200);
        WiFi.begin(ssid.c_str(), plain.c_str());
      }
      nextSyncAt = millis() + 5000;
    } else if (cfgReady && WiFi.status() != WL_CONNECTED) {
      reconnectWithSavedWifi();
      nextSyncAt = millis() + 5000;
    }
  }
};

class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* s) override { }
  void onDisconnect(BLEServer* s) override { s->startAdvertising(); }
};

// ───────────── SETUP ─────────────
void setup() {
  Serial.begin(115200);

  uint64_t chipid = ESP.getEfuseMac();
  char id[13]; sprintf(id, "%04X%08X", (uint32_t)(chipid >> 32), (uint32_t)chipid);
  uniqueId = String(id); bleName = "ESP32_" + uniqueId;

  BLEDevice::init(bleName.c_str());
  gServer = BLEDevice::createServer();
  gServer->setCallbacks(new MyServerCallbacks());
  BLEService *svc = gServer->createService(SERVICE_UUID);

  pCharacteristic = svc->createCharacteristic(
    CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR | BLECharacteristic::PROPERTY_NOTIFY
  );
  pCharacteristic->setCallbacks(new MyCallbacks());
  pCharacteristic->addDescriptor(new BLE2902());
  svc->start();

  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(true);
  adv->setMinPreferred(0x06);
  gServer->startAdvertising();

  // (опційно) безпечні з’єднання
  BLESecurity *sec = new BLESecurity();
  sec->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
  sec->setCapability(ESP_IO_CAP_OUT);
  sec->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

  preferences.begin("config", true);
  bleConfigured = preferences.getBool("configured", false);
  preferences.end();

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  lcd.init(); lcd.backlight();
  lcd.setCursor(0,0);
  lcd.print(bleConfigured ? "Init sensors..." : "BLE config mode");

  dht.begin();
  pinMode(Smoke_PIN, INPUT);
  pinMode(Light_PIN, INPUT);

  delay(20000); // прогрів MQ-2

  if (!bme.begin(0x76)) { bmeDetected = false; }

  if (bleConfigured) reconnectWithSavedWifi();
}

// ───────────── LOOP ─────────────
void loop() {
  checkWiFiConnection();

  if (millis() - displayRefreshTime >= LCD_REFRESH_MS) {
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    int gas  = digitalRead(Smoke_PIN);
    int lite = digitalRead(Light_PIN);
    float p  = bme.readPressure() / 100.0F; // hPa
    updateDisplay(t, h, gas, lite, p);
    displayRefreshTime = millis();
  }

  if (WiFi.status() == WL_CONNECTED && bleConfigured && millis() >= nextSyncAt) {
    int rc = syncMetadata();
    scheduleNextSync(rc);
  }

  if (WiFi.status() == WL_CONNECTED && bleConfigured && (millis() - lastTime) > POST_INTERVAL_MS) {
    // читання сенсорів
    float tDht = dht.readTemperature();
    float hDht = dht.readHumidity();
    int gas    = digitalRead(Smoke_PIN);
    int lite   = digitalRead(Light_PIN);

    float tBme = bme.readTemperature();
    float hBme = bme.readHumidity();
    float pBme = bme.readPressure() / 100.0F;
    float aBme = bme.readAltitude(SEALEVELPRESSURE_HPA);

    int   mq2Val   = analogRead(MQ2_ANALOG_PIN);
    int   lightVal = analogRead(LIGHT_ANALOG_PIN);
    float mq2Pct   = mq2Val   * 100.0 / 4095.0;
    float lightPct = 100.0 - (lightVal * 100.0 / 4095.0);

    // JSON
    String json = "{";
    json += "\"ChipId\":\"" + uniqueId + "\",";
    json += "\"TemperatureDht\":"; json += isnan(tDht) ? "null" : String(tDht, 2); json += ",";
    json += "\"HumidityDht\":";    json += isnan(hDht) ? "null" : String(hDht, 2); json += ",";
    json += "\"TemperatureBme\":"; json += isnan(tBme) ? "null" : String(tBme, 2); json += ",";
    json += "\"HumidityBme\":";    json += isnan(hBme) ? "null" : String(hBme, 2); json += ",";
    json += "\"Pressure\":";       json += isnan(pBme) ? "null" : String(pBme, 2); json += ",";
    json += "\"Altitude\":";       json += isnan(aBme) ? "null" : String(aBme, 2); json += ",";
    json += "\"GasDetected\":";    json += (gas  == LOW ? "true" : "false"); json += ",";
    json += "\"Light\":";          json += (lite == LOW ? "true" : "false"); json += ",";
    json += "\"MQ2Analog\":"           + String(mq2Val)   + ",";
    json += "\"MQ2AnalogPercent\":"    + String(mq2Pct, 2)+ ",";
    json += "\"LightAnalog\":"         + String(lightVal) + ",";
    json += "\"LightAnalogPercent\":"  + String(lightPct, 2);
    json += "}";

    // POST /api/SensorData з X-Api-Key
    HTTPClient http;
    http.begin(String(API_BASE));            // ← саме BASE (без /sync)
    http.addHeader("Content-Type", "application/json");

    preferences.begin("config", true);
    String apiKey = preferences.getString("api_key", "");
    preferences.end();
    if (apiKey.length()) http.addHeader("X-Api-Key", apiKey);

    int code = http.POST(json);
    http.end();

    lastTime = millis();
  }
}
