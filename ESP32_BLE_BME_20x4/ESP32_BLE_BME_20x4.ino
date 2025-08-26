/*  ===========================================================================
    ESP32 ENV NODE (20x4 LCD, BME280 + DHT11, BLE конфіг, Wi-Fi, HTTP)
    - BLE write JSON (повний конфіг або Wi-Fi patch)
    - Негайне перепідключення Wi-Fi після patch
    - Збереження у NVS (Preferences), без затирання відсутніх полів
    - NOTIFY: надсилає ChipId назад Android
    - Періодичний GET /sensordata/ownership/{chipId}/latest (ETag)
    - POST телеметрії кожні 10 хв
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

// ─────────────────────────────────────────────────────────────────────────────
// Піни/датчики
// ─────────────────────────────────────────────────────────────────────────────
#define DHT_PIN          4
#define Smoke_PIN        25
#define Light_PIN        26
#define I2C_SDA_PIN      21
#define I2C_SCL_PIN      22
#define MQ2_ANALOG_PIN   34
#define LIGHT_ANALOG_PIN 35

#define DHT_TYPE DHT11

// LCD (I2C) 20x4
#define LCD_ADDRESS 0x27
#define LCD_WIDTH   20
#define LCD_HEIGHT  4
LiquidCrystal_I2C lcd(LCD_ADDRESS, LCD_WIDTH, LCD_HEIGHT);

// Датчики
#define SEALEVELPRESSURE_HPA (1013.25)
Adafruit_BME280 bme;     // BME280 (t, p, h, altitude)
DHT dht(DHT_PIN, DHT_TYPE);

// ─────────────────────────────────────────────────────────────────────────────
// HTTP / API
// ─────────────────────────────────────────────────────────────────────────────
const char* serverName = "http://192.168.251.32:5210/api/sensordata";     // POST
const char* apiBase    = "http://192.168.251.32:5210/api/sensordata";     // GET base (…/ownership/{chipId}/latest)

// ─────────────────────────────────────────────────────────────────────────────
// Глобальні стани
// ─────────────────────────────────────────────────────────────────────────────
bool statusDisplayed = true;      // перший екран (BLE/WiFi) + привітання
String uniqueId;                  // 12-символьний HEX ChipId
String bleName;                   // Ім’я BLE
bool   bleConfigured = false;     // чи є валідна Wi-Fi конфігурація в NVS
bool   bmeDetected   = true;

AESLib aesLib;
Preferences preferences;

// ─────────────────────────────────────────────────────────────────────────────
// AES (має збігатися з Android)
// ─────────────────────────────────────────────────────────────────────────────
byte aes_key[] = { 'm','y','-','s','e','c','r','e','t','-','k','e','y','-','1','2' }; // 16 байт → AES-128
byte aes_iv[]  = { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };                                 // 16 нулів

// ─────────────────────────────────────────────────────────────────────────────
// BLE
// ─────────────────────────────────────────────────────────────────────────────
#define SERVICE_UUID        "12345678-1234-1234-1234-123456789abc"
#define CHARACTERISTIC_UUID "abcd1234-5678-1234-5678-abcdef123456"
BLECharacteristic *pCharacteristic;

// ─────────────────────────────────────────────────────────────────────────────
// Таймери
// ─────────────────────────────────────────────────────────────────────────────
unsigned long lastTime = 0, displayRefreshTime = 0, wifiCheckTime = 0, nextSyncAt = 0;
const unsigned long timerDelay             = 10UL * 60UL * 1000UL; // POST кожні 10 хв
const unsigned long displayRefreshInterval = 10UL * 1000UL;        // LCD кожні 10 с
const unsigned long wifiCheckInterval      = 60UL * 1000UL;        // Перевірка Wi-Fi 60 с

// Ownership sync
static const unsigned long SYNC_OK_PERIOD_MS   = 30UL * 60UL * 1000UL; // раз на 30 хв
static const unsigned long SYNC_FAIL_PERIOD_MS = 5UL  * 60UL * 1000UL; // бекоф 5 хв

// ─────────────────────────────────────────────────────────────────────────────
// Утиліти
// ─────────────────────────────────────────────────────────────────────────────
String decryptPassword(const String& encrypted) {
  int inputLength = encrypted.length() + 1;
  char input[inputLength];
  encrypted.toCharArray(input, inputLength);

  byte decrypted[256];
  int len = aesLib.decrypt64(input, inputLength, decrypted, aes_key, 128, aes_iv);
  if (len <= 0) {
    Serial.println("❌ Помилка дешифрування!");
    return "";
  }
  decrypted[len] = '\0';
  String result = String((char*)decrypted);

  // PKCS7 padding (обережно)
  if (len > 0) {
    int pad = decrypted[len - 1];
    if (pad > 0 && pad <= 16 && pad <= result.length()) {
      result.remove(result.length() - pad);
    }
  }
  return result;
}

static bool putIfChanged(Preferences& p, const char* key, const String& v) {
  String old = p.getString(key, "");
  if (v != old) { p.putString(key, v); return true; }
  return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Wi-Fi
// ─────────────────────────────────────────────────────────────────────────────
bool reconnectWithSavedWifi() {
  preferences.begin("config", true);
  String ssid   = preferences.getString("ssid", "");
  String encPwd = preferences.getString("enc_pwd", "");
  preferences.end();

  String pass = decryptPassword(encPwd);
  Serial.println("🔐 enc: " + encPwd);
  Serial.println("🔓 dec: " + pass);

  if (ssid.isEmpty() || pass.isEmpty()) {
    Serial.println("❌ Немає збережених SSID/пароля для підключення");
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true); // повний ресет Wi-Fi стека
  delay(200);
  Serial.printf("📶 Підключення до SSID='%s'...\n", ssid.c_str());
  WiFi.begin(ssid.c_str(), pass.c_str());

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ Wi-Fi підключено: " + WiFi.localIP().toString());
    lcd.clear(); lcd.setCursor(0,0); lcd.print("IP: "); lcd.print(WiFi.localIP());
    delay(1500);
    return true;
  } else {
    Serial.println("\n❌ Не вдалося підключитись до нової мережі");
    return false;
  }
}

void scanAndConnectWiFi() {
  // Залишено сумісність з попереднім кодом — зараз викликає пряме підключення
  reconnectWithSavedWifi();
}

void checkWiFiConnection() {
  if (millis() - wifiCheckTime >= wifiCheckInterval) {
    if (WiFi.status() != WL_CONNECTED && bleConfigured) {
      reconnectWithSavedWifi();
    }
    wifiCheckTime = millis();
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Ownership sync (ETag)
// ─────────────────────────────────────────────────────────────────────────────
int syncOwnershipNoAuth() {
  if (WiFi.status() != WL_CONNECTED) return -1;

  String url = String(apiBase) + "/ownership/" + uniqueId + "/latest";
  HTTPClient http;
  http.begin(url);

  // ETag → If-None-Match
  preferences.begin("config", true);
  String etag = preferences.getString("own_etag", "");
  preferences.end();
  if (etag.length()) http.addHeader("If-None-Match", etag);

  Serial.println("🔎 GET " + url + (etag.length() ? (" (If-None-Match: " + etag + ")") : ""));

  int code = http.GET();

  if (code == 304) {
    Serial.println("✅ 304 Not Modified");
    http.end();

    preferences.begin("config", false);
    preferences.putBool("own_provisional", false);
    preferences.end();

    return 0;
  }

  if (code != 200) {
    Serial.println("❌ GET помилка: " + String(code));
    http.end();
    return -1;
  }

  String body    = http.getString();
  String newEtag = http.header("ETag");
  http.end();

  JSONVar obj = JSON.parse(body);
  if (JSON.typeof(obj) != "object") {
    Serial.println("❌ JSON від сервера некоректний");
    return -1;
  }

  auto getIfPresent = [&](const char* key, bool& has) -> String {
    has = (JSON.typeof(obj[key]) != "undefined" && JSON.typeof(obj[key]) != "null");
    return has ? String((const char*)obj[key]) : String("");
  };

  bool hasUser=false, hasRoom=false, hasImg=false;
  String srvUsername = getIfPresent("username",  hasUser);
  String srvRoomName = getIfPresent("roomName",  hasRoom);
  String srvImage    = getIfPresent("imageName", hasImg);

  bool changed = false;
  preferences.begin("config", false);
  if (hasUser) changed |= putIfChanged(preferences, "username",  srvUsername);
  if (hasRoom) changed |= putIfChanged(preferences, "roomName",  srvRoomName);
  if (hasImg)  changed |= putIfChanged(preferences, "imageName", srvImage);

  if (newEtag.length()) preferences.putString("own_etag", newEtag);
  preferences.putBool("own_provisional", false);
  preferences.end();

  Serial.println(changed ? "✅ Ownership оновлено (200 OK)" : "✅ 200 OK без змін");
  return changed ? 1 : 0;
}

void scheduleNextSync(int rc) {
  unsigned long period = (rc >= 0) ? SYNC_OK_PERIOD_MS : SYNC_FAIL_PERIOD_MS;
  nextSyncAt = millis() + period;
  Serial.printf("⏱ Наступний синк через %lus\n", period / 1000UL);
}

// ─────────────────────────────────────────────────────────────────────────────
// LCD
// ─────────────────────────────────────────────────────────────────────────────
void updateDisplay(float tempC, float humi, int smokeState, int lightState, float pressure) {
  lcd.clear();

  if (statusDisplayed) {
    lcd.setCursor(0, 0);
    lcd.print("BLE:");
    lcd.print(bleConfigured ? "OK " : "WAIT");
    lcd.print(" WiFi:");
    lcd.print(WiFi.status() == WL_CONNECTED ? "OK" : "NO");

    if (bleConfigured && WiFi.status() == WL_CONNECTED) {
      statusDisplayed = false;
      preferences.begin("config", true);
      String username = preferences.getString("username", "");
      preferences.end();
      if (username.length() > 0) {
        String hello = "Hello, " + username;
        lcd.setCursor(0, 3);
        if (hello.length() <= 20) {
          lcd.print(hello);
        } else {
          for (int i = 0; i <= hello.length() - 20; i++) {
            lcd.setCursor(0, 3);
            lcd.print(hello.substring(i, i + 20));
            delay(300);
          }
        }
      }
    }
    return;
  }

  // Рядок 1: температура/вологість
  lcd.setCursor(0, 0);
  if (isnan(tempC) || isnan(humi)) {
    lcd.print("Temp/Hum: ERROR");
  } else {
    lcd.print("Temp:");
    lcd.print((int)tempC);
    lcd.write(223); // °
    lcd.print("C ");
    lcd.print("Hum:");
    lcd.print((int)humi);
    lcd.print("%");
  }

  // Рядок 2: тиск
  lcd.setCursor(0, 1);
  if (!bmeDetected || isnan(pressure)) {
    lcd.print("Pres: ERROR");
  } else {
    lcd.print("P:"); lcd.print(pressure); lcd.print("hPa");
  }

  // Рядок 3: газ/світло
  lcd.setCursor(0, 2);
  lcd.print("Gas:");   lcd.print(smokeState == LOW ? "Yes" : "No ");
  lcd.print(" Light:"); lcd.print(lightState == HIGH ? "Dark" : "Light");

  // Рядок 4: назва кімнати
  lcd.setCursor(0, 3);
  preferences.begin("config", true);
  String roomName = preferences.getString("roomName", "NoRoom");
  preferences.end();
  if (roomName.length() <= 20) lcd.print(roomName);
  else                         lcd.print(roomName.substring(0, 20));
}

// ─────────────────────────────────────────────────────────────────────────────
// BLE: прийом JSON → NVS + notify chipId + негайний Wi-Fi reconnect (якщо patch)
// ─────────────────────────────────────────────────────────────────────────────
class MyCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pChar) override {
    String jsonStr = String(pChar->getValue().c_str());
    Serial.println("📥 Отримано BLE JSON:");
    Serial.println(jsonStr);

    JSONVar data = JSON.parse(jsonStr);
    if (JSON.typeof(data) == "undefined") {
      Serial.println("❌ JSON невірний");
      return;
    }

    // 1) reset=true → повне очищення
    bool shouldReset = (data.hasOwnProperty("reset") && (bool)data["reset"]);
    if (shouldReset) {
      Serial.println("🔄 Очищення Preferences через reset=true");
      preferences.begin("config", false);
      preferences.clear();
      preferences.end();
      delay(100);
    }

    // 2) Акуратний парсинг: пишемо тільки надіслані ключі
    auto getIfStr = [&](const char* key, bool &has) -> String {
      has = data.hasOwnProperty(key) &&
            JSON.typeof(data[key]) != "undefined" &&
            JSON.typeof(data[key]) != "null";
      return has ? String((const char*)data[key]) : String("");
    };

    bool hasSSID=false, hasPwd=false, hasUser=false, hasImg=false, hasRoom=false;
    String ssid       = getIfStr("ssid",       hasSSID);
    String encPwd     = getIfStr("password",   hasPwd);
    String username   = getIfStr("username",   hasUser);
    String imageName  = getIfStr("imageName",  hasImg);
    String roomName   = getIfStr("roomName",   hasRoom);

    preferences.begin("config", false);
    bool changed = false;
    if (hasSSID) changed |= putIfChanged(preferences, "ssid",    ssid);
    if (hasPwd)  changed |= putIfChanged(preferences, "enc_pwd", encPwd);
    if (hasUser) changed |= putIfChanged(preferences, "username",  username);
    if (hasImg)  changed |= putIfChanged(preferences, "imageName", imageName);
    if (hasRoom) changed |= putIfChanged(preferences, "roomName",  roomName);

    // configured=true якщо маємо і ssid, і enc_pwd
    String curSsid = preferences.getString("ssid", "");
    String curEnc  = preferences.getString("enc_pwd", "");
    bool cfgReady  = (curSsid.length() && curEnc.length());
    preferences.putBool("configured", cfgReady);
    preferences.end();

    bleConfigured = cfgReady;

    // 3) Відповідь у Android (NOTIFY chipId) — і для patch, і для повного конфігу
    pChar->setValue(uniqueId.c_str());
    pChar->notify();
    Serial.println("📤 Надіслано chipId назад: " + uniqueId);

    // 4) Якщо Wi-Fi patch → негайно перепідключаємось
    if (hasSSID || hasPwd) {
      reconnectWithSavedWifi();
      nextSyncAt = millis() + 5000; // перший sync ownership за 5с
      return;
    }

    // 5) Інакше (повний конфіг) → якщо Wi-Fi ще не під’єднаний — під’єднатись
    if (cfgReady && WiFi.status() != WL_CONNECTED) {
      reconnectWithSavedWifi();
      nextSyncAt = millis() + 5000;
    }
  }
};

// ─────────────────────────────────────────────────────────────────────────────
// SETUP
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  // Унікальний ChipId + BLE ім’я
  uint64_t chipid = ESP.getEfuseMac();
  char id[13];
  sprintf(id, "%04X%08X", (uint32_t)(chipid >> 32), (uint32_t)chipid);
  uniqueId = String(id);

  // Ім’я BLE з останніми 6 символами (зручно для відображення), але можна і повний
  String last6 = uniqueId.substring(uniqueId.length() - 6);
  bleName = "ESP32_" + last6;

  // BLE init
  BLEDevice::init(bleName.c_str());
  Serial.println("=== BLE ІНІЦІАЛІЗАЦІЯ ===");
  Serial.println("ChipId: " + uniqueId);
  Serial.println("BLE Name: " + bleName);

  BLEServer *pServer = BLEDevice::createServer();
  BLEService *pService = pServer->createService(SERVICE_UUID);
  pCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_NOTIFY
  );
  pCharacteristic->setCallbacks(new MyCallbacks());
  pService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  BLEDevice::startAdvertising();
  Serial.println("BLE advertising started!");
  Serial.println("==========================");

  // NVS: чи вже була конфігурація
  preferences.begin("config", true);
  bleConfigured = preferences.getBool("configured", false);
  preferences.end();

  // I2C/LCD/Sensors
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
    bmeDetected = false;
  }

  // Wi-Fi (якщо конфіг уже був)
  if (bleConfigured) {
    scanAndConnectWiFi();
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// LOOP
// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  // 1) Підтримка Wi-Fi
  checkWiFiConnection();

  // 2) Оновлення LCD
  if (millis() - displayRefreshTime >= displayRefreshInterval) {
    float tempC      = dht.readTemperature();
    float humi       = dht.readHumidity();
    int smokeState   = digitalRead(Smoke_PIN);
    int lightState   = digitalRead(Light_PIN);
    float pressure   = bme.readPressure() / 100.0F;
    updateDisplay(tempC, humi, smokeState, lightState, pressure);
    displayRefreshTime = millis();
  }

  // 3) Періодичний синк ownership (без токена)
  if (WiFi.status() == WL_CONNECTED && bleConfigured && millis() >= nextSyncAt) {
    int rc = syncOwnershipNoAuth();   // 1: оновлено, 0: без змін, -1: помилка
    scheduleNextSync(rc);
  }

  // 4) Відправка телеметрії на сервер раз на 10 хв
  if ((millis() - lastTime) > timerDelay && WiFi.status() == WL_CONNECTED && bleConfigured) {
    float tempC      = dht.readTemperature();
    float humi       = dht.readHumidity();
    int smokeState   = digitalRead(Smoke_PIN);
    int lightState   = digitalRead(Light_PIN);

    float bmeTemp     = bme.readTemperature();
    float bmePressure = bme.readPressure() / 100.0F;
    float bmeHumi     = bme.readHumidity();
    float bmeAltitude = bme.readAltitude(SEALEVELPRESSURE_HPA);

    int   mq2AnalogValue   = analogRead(MQ2_ANALOG_PIN);
    int   lightAnalogValue = analogRead(LIGHT_ANALOG_PIN);
    float mq2Percent       = mq2AnalogValue   * 100.0 / 4095.0;
    float lightPercent     = 100.0 - (lightAnalogValue * 100.0 / 4095.0);

    // JSON (узгоджено з беком)
    String json = "{";
    json += "\"ChipId\":\"" + uniqueId + "\",";

    json += "\"TemperatureDht\":"; json += isnan(tempC) ? "null" : String(tempC, 2); json += ",";
    json += "\"HumidityDht\":";    json += isnan(humi)  ? "null" : String(humi, 2) ; json += ",";

    json += "\"TemperatureBme\":"; json += (!isnan(bmeTemp))     ? String(bmeTemp, 2)     : "null"; json += ",";
    json += "\"HumidityBme\":";    json += (!isnan(bmeHumi))     ? String(bmeHumi, 2)     : "null"; json += ",";
    json += "\"Pressure\":";       json += (!isnan(bmePressure)) ? String(bmePressure, 2) : "null"; json += ",";
    json += "\"Altitude\":";       json += (!isnan(bmeAltitude)) ? String(bmeAltitude, 2) : "null"; json += ",";

    json += "\"GasDetected\":";    json += (smokeState == LOW ? "true" : "false"); json += ",";
    json += "\"Light\":";          json += (lightState == HIGH ? "true" : "false"); json += ",";

    json += "\"MQ2Analog\":"           + String(mq2AnalogValue)  + ",";
    json += "\"MQ2AnalogPercent\":"    + String(mq2Percent, 2)   + ",";
    json += "\"LightAnalog\":"         + String(lightAnalogValue)+ ",";
    json += "\"LightAnalogPercent\":"  + String(lightPercent, 2);
    json += "}";

    HTTPClient http;
    http.begin(serverName);
    http.addHeader("Content-Type", "application/json");

    Serial.println("➡️ Надсилається JSON:");
    Serial.println(json);

    int code = http.POST(json);
    if (code > 0) Serial.println("POST OK: " + String(code));
    else          Serial.println("POST ERR: " + String(code));
    http.end();

    lastTime = millis();
  }
}
