/*  ===========================================================================
    ESP32 ENV NODE (16x2 LCD, BMP280 + DHT11, BLE конфіг, Wi-Fi, HTTP)
    - BLE write JSON: часткове оновлення (ssid, password[encrypted], username,
      imageName, roomName, apiKey), reset=true очищає NVS
    - Збереження в NVS (Preferences), негайне Wi-Fi reconnect після патча
    - Періодичний GET  /api/SensorData/sync/{chipId} (If-None-Match + X-Api-Key)
    - Періодичний POST /api/SensorData          (повний JSON + X-Api-Key)
    =========================================================================== */

#include <Wire.h>
#include <DHT.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Arduino_JSON.h>
#include <SPI.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_Sensor.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <AESLib.h>
#include <BLE2902.h>
#include <BLESecurity.h>

// ────────────────────────── Піни ──────────────────────────
#define DHT_PIN          4
#define Smoke_PIN        25
#define Light_PIN        26
#define I2C_SDA_PIN      21
#define I2C_SCL_PIN      22
#define MQ2_ANALOG_PIN   34
#define LIGHT_ANALOG_PIN 35
#define DHT_TYPE DHT11

// LCD 16x2 (I2C)
#define LCD_ADDRESS 0x3F
#define LCD_WIDTH   16
#define LCD_HEIGHT  2
LiquidCrystal_I2C lcd(LCD_ADDRESS, LCD_WIDTH, LCD_HEIGHT);

// Датчики
#define SEALEVELPRESSURE_HPA (1013.25)
Adafruit_BMP280 bmp;
DHT dht(DHT_PIN, DHT_TYPE);

// ────────────────────────── HTTP ──────────────────────────
// БАЗА (лише її підправ, шляхи нижче фіксовані під ТВОЇ ендпоінти)
static const char* SERVER_BASE = "http://192.168.0.200:5210";

// POST:  SERVER_BASE + /api/SensorData
// GET :  SERVER_BASE + /api/SensorData/sync/{chipId}

// ────────────────────────── Глобальні ──────────────────────────
bool   statusDisplayed = true;
String uniqueId;
String bleName;
bool   bleConfigured = false;
bool   bmpDetected   = true;

AESLib aesLib;
Preferences preferences;

// AES-128 ключ для локального дешифру Wi-Fi пароля
byte aes_key[] = { 'm','y','-','s','e','c','r','e','t','-','k','e','y','-','1','2' }; // 16B
byte aes_iv[]  = { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };

#define SERVICE_UUID        "12345678-1234-1234-1234-123456789abc"
#define CHARACTERISTIC_UUID "abcd1234-5678-1234-5678-abcdef123456"
BLECharacteristic *pCharacteristic = nullptr;
BLEServer* gServer = nullptr;

// Таймери
unsigned long lastTime = 0, displayRefreshTime = 0, wifiCheckTime = 0, nextSyncAt = 0;
const unsigned long POST_PERIOD_MS         = 10UL * 60UL * 1000UL; // 10 хв
const unsigned long LCD_REFRESH_MS         = 10UL * 1000UL;        // 10 с
const unsigned long WIFI_CHECK_MS          = 60UL * 1000UL;        // 60 с
const unsigned long SYNC_OK_PERIOD_MS      = 30UL * 60UL * 1000UL; // 30 хв
const unsigned long SYNC_FAIL_PERIOD_MS    = 5UL  * 60UL * 1000UL; // 5 хв
bool showRoomLine = false;

// ────────────────────────── Утиліти ──────────────────────────
String decryptPassword(const String& encrypted) {
  int n = encrypted.length() + 1;
  char input[n]; encrypted.toCharArray(input, n);

  byte out[256];
  int len = aesLib.decrypt64(input, n, out, aes_key, 128, aes_iv);
  if (len <= 0) return "";

  out[len] = '\0';
  String s = String((char*)out);
  if (len > 0) {
    int pad = out[len - 1];
    if (pad > 0 && pad <= 16 && pad <= (int)s.length()) s.remove(s.length() - pad);
  }
  return s;
}

static bool putIfChanged(Preferences& p, const char* key, const String& v) {
  String old = p.getString(key, "");
  if (v != old) { p.putString(key, v); return true; }
  return false;
}

// ────────────────────────── Wi-Fi ──────────────────────────
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

// ────────────────────────── SYNC GET (/api/SensorData/sync/{chipId}) ──────────────────────────
// Повертає JSON (username, roomName, imageName). Використовує If-None-Match + X-Api-Key.
int syncFromServer() {
  if (WiFi.status() != WL_CONNECTED) return -1;

  String url = String(SERVER_BASE) + "/api/SensorData/sync/" + uniqueId;

  preferences.begin("config", true);
  String etag  = preferences.getString("sync_etag", "");
  String apiKey= preferences.getString("api_key", "");
  preferences.end();

  HTTPClient http;
  http.begin(url);

  if (etag.length())   http.addHeader("If-None-Match", etag);
  if (apiKey.length()) http.addHeader("X-Api-Key", apiKey);

  int code = http.GET();

  if (code == 304) {
    http.end();
    return 0; // без змін
  }
  if (code != 200) {
    http.end();
    return -1; // помилка
  }

  String body    = http.getString();
  String newEtag = http.header("ETag");
  http.end();

  JSONVar obj = JSON.parse(body);
  if (JSON.typeof(obj) != "object") return -1;

  auto getIf = [&](const char* k, bool &has) -> String {
    has = (JSON.typeof(obj[k]) != "undefined" && JSON.typeof(obj[k]) != "null");
    return has ? String((const char*)obj[k]) : String("");
  };

  bool hu=false, hr=false, hi=false;
  String u = getIf("username",  hu);
  String r = getIf("roomName",  hr);
  String i = getIf("imageName", hi);

  bool changed = false;
  preferences.begin("config", false);
  if (hu) changed |= putIfChanged(preferences, "username",  u);
  if (hr) changed |= putIfChanged(preferences, "roomName",  r);
  if (hi) changed |= putIfChanged(preferences, "imageName", i);
  if (newEtag.length()) preferences.putString("sync_etag", newEtag);
  preferences.end();

  return changed ? 1 : 0;
}

void scheduleNextSync(int rc) {
  unsigned long period = (rc >= 0) ? SYNC_OK_PERIOD_MS : SYNC_FAIL_PERIOD_MS;
  nextSyncAt = millis() + period;
}

// ────────────────────────── LCD ──────────────────────────
void updateDisplay(float tempC, float humi, int smokeState, int lightState, float pressure) {
  lcd.clear();

  if (statusDisplayed) {
    lcd.setCursor(0, 0);
    lcd.print("BLE:"); lcd.print(bleConfigured ? "OK " : "WAIT");
    lcd.print("W:");   lcd.print(WiFi.status() == WL_CONNECTED ? "OK" : "NO");

    preferences.begin("config", true);
    bool hasKey = preferences.getString("api_key", "").length() > 0;
    preferences.end();

    lcd.setCursor(0, 1);
    lcd.print("API:"); lcd.print(hasKey ? "OK" : "NO");
    if (bleConfigured && WiFi.status() == WL_CONNECTED) statusDisplayed = false;
    return;
  }

  // 1-й рядок
  lcd.setCursor(0, 0);
  if (isnan(tempC) || isnan(humi)) lcd.print("Temp/Hum: ERR");
  else {
    lcd.print("T:"); lcd.print((int)tempC); lcd.write(223); lcd.print("C ");
    lcd.print("H:"); lcd.print((int)humi);  lcd.print("%");
  }

  // 2-й рядок (чергується)
  lcd.setCursor(0, 1);
  if (!showRoomLine) {
    if (isnan(pressure)) lcd.print("P:ERR ");
    else { lcd.print("P:"); lcd.print((int)pressure); lcd.print("hPa "); }
    lcd.print("G:"); lcd.print((smokeState == LOW) ? "Y " : "N ");
    lcd.print("L:"); lcd.print((lightState == LOW) ? "L" : "D");
  } else {
    preferences.begin("config", true);
    String roomName = preferences.getString("roomName", "NoRoom");
    preferences.end();
    if (roomName.length() <= 16) lcd.print(roomName); else lcd.print(roomName.substring(0,16));
  }
  showRoomLine = !showRoomLine;
}

// ────────────────────────── BLE (конфіг JSON) ──────────────────────────
class MyCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    String s = String(c->getValue().c_str());
    JSONVar data = JSON.parse(s);
    if (JSON.typeof(data) == "undefined") return;

    bool reset = data.hasOwnProperty("reset") && (bool)data["reset"];
    if (reset) { preferences.begin("config", false); preferences.clear(); preferences.end(); delay(100); }

    auto getIf = [&](const char* k, bool &has) -> String {
      has = data.hasOwnProperty(k) &&
            JSON.typeof(data[k]) != "undefined" &&
            JSON.typeof(data[k]) != "null";
      return has ? String((const char*)data[k]) : String("");
    };

    bool hS=false,hP=false,hU=false,hI=false,hR=false,hK=false;
    String ssid   = getIf("ssid",      hS);
    String encPwd = getIf("password",  hP);
    String user   = getIf("username",  hU);
    String img    = getIf("imageName", hI);
    String room   = getIf("roomName",  hR);
    String key    = getIf("apiKey",    hK);

    preferences.begin("config", false);
    if (hS) preferences.putString("ssid", ssid);
    if (hP) preferences.putString("enc_pwd", encPwd);
    if (hU) preferences.putString("username", user);
    if (hI) preferences.putString("imageName", img);
    if (hR) preferences.putString("roomName",  room);
    if (hK && key.length() >= 16 && key.length() <= 128) preferences.putString("api_key", key);

    String curSsid = preferences.getString("ssid", "");
    String curEnc  = preferences.getString("enc_pwd", "");
    bool cfgReady  = (curSsid.length() && curEnc.length());
    preferences.putBool("configured", cfgReady);
    preferences.end();

    bleConfigured = cfgReady;

    // notify chipId назад
    c->setValue(uniqueId.c_str());
    c->notify();

    // миттєве перепідключення Wi-Fi при оновленні даних
    if (hS && hP) {
      String plain = decryptPassword(encPwd);
      if (plain.length()) {
        WiFi.mode(WIFI_STA);
        WiFi.disconnect(true, true);
        delay(200);
        WiFi.begin(ssid.c_str(), plain.c_str());
      }
      nextSyncAt = millis() + 3000;
      return;
    }
    if (cfgReady && WiFi.status() != WL_CONNECTED) {
      reconnectWithSavedWifi();
      nextSyncAt = millis() + 3000;
    }
  }
};

class MyServerCallbacks : public BLEServerCallbacks {
  void onDisconnect(BLEServer* s) override { s->startAdvertising(); }
};

// ────────────────────────── SETUP ──────────────────────────
void setup() {
  Serial.begin(115200);

  // ChipId
  uint64_t chipid = ESP.getEfuseMac();
  char id[13]; sprintf(id, "%04X%08X", (uint32_t)(chipid >> 32), (uint32_t)chipid);
  uniqueId = String(id);
  bleName  = "ESP32_" + uniqueId;

  // BLE
  BLEDevice::init(bleName.c_str());
  gServer = BLEDevice::createServer();
  gServer->setCallbacks(new MyServerCallbacks());
  BLEService *svc = gServer->createService(SERVICE_UUID);
  pCharacteristic = svc->createCharacteristic(
      CHARACTERISTIC_UUID,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR | BLECharacteristic::PROPERTY_NOTIFY);
  pCharacteristic->setCallbacks(new MyCallbacks());
  pCharacteristic->addDescriptor(new BLE2902());
  svc->start();
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(true);
  adv->setMinPreferred(0x06);
  gServer->startAdvertising();
  BLESecurity *sec = new BLESecurity();
  sec->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
  sec->setCapability(ESP_IO_CAP_OUT);
  sec->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

  // NVS
  preferences.begin("config", true);
  bleConfigured = preferences.getBool("configured", false);
  preferences.end();

  // I2C/LCD/Sensors
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  lcd.init(); lcd.backlight();
  dht.begin(); pinMode(Smoke_PIN, INPUT); pinMode(Light_PIN, INPUT);
  delay(20000); // MQ-2 прогрів

  if (!bmp.begin(0x76)) bmpDetected = false;

  if (bleConfigured) reconnectWithSavedWifi();
}

// ────────────────────────── LOOP ──────────────────────────
void loop() {
  checkWiFiConnection();

  if (millis() - displayRefreshTime >= LCD_REFRESH_MS) {
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    int   g = digitalRead(Smoke_PIN);
    int   l = digitalRead(Light_PIN);
    float p = bmp.readPressure() / 100.0F;
    updateDisplay(t, h, g, l, p);
    displayRefreshTime = millis();
  }

  if (WiFi.status() == WL_CONNECTED && bleConfigured && millis() >= nextSyncAt) {
    int rc = syncFromServer();   // 1 змінено, 0 без змін, -1 помилка
    scheduleNextSync(rc);
  }

  if ((millis() - lastTime) > POST_PERIOD_MS && WiFi.status() == WL_CONNECTED && bleConfigured) {
    // — зчитування
    float tDht = dht.readTemperature();
    float hDht = dht.readHumidity();
    int   smokeState = digitalRead(Smoke_PIN);
    int   lightState = digitalRead(Light_PIN);

    float tBmp = bmp.readTemperature();
    float pBmp = bmp.readPressure() / 100.0F;
    float aBmp = bmp.readAltitude(SEALEVELPRESSURE_HPA);

    int   mq2Raw  = analogRead(MQ2_ANALOG_PIN);
    int   lightRaw= analogRead(LIGHT_ANALOG_PIN);
    float mq2Pct  = mq2Raw   * 100.0 / 4095.0;
    float lightPc = 100.0 - (lightRaw * 100.0 / 4095.0);

    // — формуємо JSON
    String json = "{";
    json += "\"ChipId\":\"" + uniqueId + "\",";
    json += "\"TemperatureDht\":"; json += isnan(tDht) ? "null" : String(tDht, 2); json += ",";
    json += "\"HumidityDht\":";    json += isnan(hDht) ? "null" : String(hDht, 2); json += ",";
    json += "\"TemperatureBme\":"; json += isnan(tBmp) ? "null" : String(tBmp, 2); json += ",";
    json += "\"HumidityBme\":null,";
    json += "\"Pressure\":";       json += isnan(pBmp) ? "null" : String(pBmp, 2); json += ",";
    json += "\"Altitude\":";       json += isnan(aBmp) ? "null" : String(aBmp, 2); json += ",";
    json += "\"GasDetected\":";    json += (smokeState == LOW ? "true" : "false"); json += ",";
    json += "\"Light\":";          json += (lightState == LOW ? "true" : "false"); json += ",";
    json += "\"MQ2Analog\":" + String(mq2Raw) + ",";
    json += "\"MQ2AnalogPercent\":" + String(mq2Pct, 2) + ",";
    json += "\"LightAnalog\":" + String(lightRaw) + ",";
    json += "\"LightAnalogPercent\":" + String(lightPc, 2);
    json += "}";

    // — POST /api/SensorData з X-Api-Key
    String url = String(SERVER_BASE) + "/api/SensorData";

    preferences.begin("config", true);
    String apiKey = preferences.getString("api_key", "");
    preferences.end();

    HTTPClient http;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    if (apiKey.length()) http.addHeader("X-Api-Key", apiKey);

    int code = http.POST(json);
    http.end();

    lastTime = millis();
  }
}
