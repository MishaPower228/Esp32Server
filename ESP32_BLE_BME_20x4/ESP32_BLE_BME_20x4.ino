// ✅ ESP32 20x4 (BME280) — синхронізовано з 16x2 (BMP) версією
// - BLE reset + збереження конфігів у Preferences
// - BLE notify: відправляємо ChipId назад Android-у
// - LCD статус: BLE/WiFi, Hello, username (прокрутка)
// - Той самий JSON формат для /api/sensordata (boolean для GasDetected/Light, числові поля)
// - Єдина логіка інтервалів (display 10s, WiFi check 60s, POST 10 хв)
// - ⭐ Додано: періодичний GET /ownership/{chipId}/latest без авторизації + ETag (30 хв; бекоф 5 хв)

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

// ─────────────────────────────────────────────────────────────────────────────
// Апартна конфігурація
// ─────────────────────────────────────────────────────────────────────────────
#define DHT_PIN          4
#define Smoke_PIN        25
#define Light_PIN        26
#define I2C_SDA_PIN      21
#define I2C_SCL_PIN      22
#define MQ2_ANALOG_PIN   34
#define LIGHT_ANALOG_PIN 35

// LCD (I2C) 20x4
#define LCD_ADDRESS 0x27
#define LCD_WIDTH   20
#define LCD_HEIGHT  4
LiquidCrystal_I2C lcd(LCD_ADDRESS, LCD_WIDTH, LCD_HEIGHT);

// Датчики
#define SEALEVELPRESSURE_HPA (1013.25)
Adafruit_BME280 bme;            // BME280 (t, p, h, altitude)
DHT dht(DHT_PIN, DHT11);        // DHT11 (t, h)

// ─────────────────────────────────────────────────────────────────────────────
// HTTP сервери/ендпоінти
// ─────────────────────────────────────────────────────────────────────────────
// POST телеметрії
const char* serverName = "http://192.168.0.200:5210/api/sensordata";

// ⭐ БАЗА для GET ownership (без токена);
//    НЕ додаємо "/" в кінці. URL збиратиметься як OWNERSHIP_BASE + "/ownership/" + chipId + "/latest"
static const char* OWNERSHIP_BASE = "http://192.168.0.200:5210/api";

// ─────────────────────────────────────────────────────────────────────────────
// Глобальні стани/змінні
// ─────────────────────────────────────────────────────────────────────────────
bool statusDisplayed = true;  // перший екран стану (BLE/WiFi) + привітання
String uniqueId;              // ChipId (hex)
String savedSSID = "";
String savedPass = "";
String bleName;
bool   bleConfigured = false; // чи є збережена конфігурація
bool   bmeDetected = true;

AESLib aesLib;
Preferences preferences;      // NVS "config"

// ─────────────────────────────────────────────────────────────────────────────
// AES ключ (синхронізовано з Android)
// ─────────────────────────────────────────────────────────────────────────────
byte aes_key[] = { 'm','y','-','s','e','c','r','e','t','-','k','e','y','-','1','2' };
byte aes_iv[]  = { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };

// ─────────────────────────────────────────────────────────────────────────────
// BLE сервіс (повідомлення + конфіг)
// ─────────────────────────────────────────────────────────────────────────────
#define SERVICE_UUID        "12345678-1234-1234-1234-123456789abc"
#define CHARACTERISTIC_UUID "abcd1234-5678-1234-5678-abcdef123456"
BLECharacteristic *pCharacteristic;

// ─────────────────────────────────────────────────────────────────────────────
// Таймери/інтервали (мс)
// ─────────────────────────────────────────────────────────────────────────────
unsigned long lastTime = 0, displayRefreshTime = 0, wifiCheckTime = 0, nextSyncAt = 0;
const unsigned long timerDelay            = 10UL * 60UL * 1000UL; // POST кожні 10 хв
const unsigned long displayRefreshInterval= 10UL * 1000UL;        // LCD кожні 10 с
const unsigned long wifiCheckInterval     = 60UL * 1000UL;        // перевірка Wi-Fi 60 с

// ⭐ Синк ownership
static const unsigned long SYNC_OK_PERIOD_MS   = 30UL * 60UL * 1000UL; // раз на 30 хв
static const unsigned long SYNC_FAIL_PERIOD_MS = 5UL  * 60UL * 1000UL; // бекоф 5 хв

// ─────────────────────────────────────────────────────────────────────────────
// Утиліти
// ─────────────────────────────────────────────────────────────────────────────
String decryptPassword(String encrypted) {
  int inputLength = encrypted.length() + 1;
  char input[inputLength];
  encrypted.toCharArray(input, inputLength);

  byte decrypted[128];
  int len = aesLib.decrypt64(input, inputLength, decrypted, aes_key, 128, aes_iv);
  if (len <= 0) {
    Serial.println("❌ Помилка дешифрування!");
    return "";
  }
  decrypted[len] = '\0';
  String result = String((char*)decrypted);

  // PKCS7 padding (обережно з межами)
  if (len > 0) {
    int pad = decrypted[len - 1];
    if (pad > 0 && pad <= 16 && pad <= result.length()) {
      result.remove(result.length() - pad);
    }
  }
  return result;
}

// Писати в NVS, тільки якщо значення справді змінилось — економія ресурсу NVS
static bool putIfChanged(Preferences& p, const char* key, const String& v) {
  String old = p.getString(key, "");
  if (v != old) { p.putString(key, v); return true; }
  return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Ownership sync (GET /ownership/{chipId}/latest) без авторизації + ETag
// ─────────────────────────────────────────────────────────────────────────────
// Повертає:  1 — є оновлення (200 і було що записати), 0 — успішно, але змін немає (200/304),
//            -1 — помилка HTTP/парсингу.
int syncOwnershipNoAuth() {
  if (WiFi.status() != WL_CONNECTED) return -1;

  String url = String(OWNERSHIP_BASE) + "/ownership/" + uniqueId + "/latest";
  HTTPClient http;
  http.begin(url);

  // ETag → If-None-Match
  preferences.begin("config", true);
  String etag = preferences.getString("own_etag", "");
  preferences.end();
  if (etag.length()) http.addHeader("If-None-Match", etag);

  Serial.println("🔎 GET " + url + (etag.length() ? (" (If-None-Match: " + etag + ")") : ""));

  int code = http.GET();

  if (code == 304) { // немає змін
    Serial.println("✅ 304 Not Modified");
    http.end();

    // Скидаємо provisional-прапор, якщо був
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

  // 200 OK → парсимо
  String body    = http.getString();
  String newEtag = http.header("ETag");
  http.end();

  JSONVar obj = JSON.parse(body);
  if (JSON.typeof(obj) != "object") {
    Serial.println("❌ JSON від сервера некоректний");
    return -1;
  }

  // Очікуємо поля: username, roomName, imageName
  auto getIfPresent = [&](const char* key, bool& has) -> String {
    has = (JSON.typeof(obj[key]) != "undefined" && JSON.typeof(obj[key]) != "null");
    return has ? String((const char*)obj[key]) : String("");
  };

  bool hasUser = false, hasRoom = false, hasImg = false;
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

// Планувальник наступного синку (успіх → 30 хв, провал → 5 хв)
void scheduleNextSync(int rc) {
  unsigned long period = (rc >= 0) ? SYNC_OK_PERIOD_MS : SYNC_FAIL_PERIOD_MS;
  nextSyncAt = millis() + period;
  Serial.printf("⏱ Наступний синк через %lus\n", period / 1000UL);
}

// ─────────────────────────────────────────────────────────────────────────────
// BLE callbacks: прийом JSON конфігу → NVS + notify chipId
// ─────────────────────────────────────────────────────────────────────────────
class MyCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) override {
    String jsonStr = String(pCharacteristic->getValue().c_str());
    Serial.println("Отримано BLE JSON:");
    Serial.println(jsonStr);

    JSONVar data = JSON.parse(jsonStr);
    if (JSON.typeof(data) == "undefined") {
      Serial.println("JSON невірний");
      return;
    }

    // 1) reset=true → повне очищення "config"
    bool shouldReset = data.hasOwnProperty("reset") && (bool)data["reset"];
    if (shouldReset) {
      Serial.println("🔄 Очищення Preferences через reset=true");
      preferences.begin("config", false);
      preferences.clear();
      preferences.end();
      delay(100);
    }

    // 2) Збереження конфігу (повний JSON на первинному сетапі)
    String username          = (const char*)data["username"];
    String imageName         = (const char*)data["imageName"];
    String ssid              = (const char*)data["ssid"];
    String encryptedPassword = (const char*)data["password"];
    String roomName          = (const char*)data["roomName"];

    preferences.begin("config", false);
    preferences.putString("ssid", ssid);
    preferences.putString("enc_pwd", encryptedPassword); // зберігаємо шифрований
    preferences.putString("username", username);
    preferences.putString("imageName", imageName);
    preferences.putString("roomName", roomName);
    preferences.putBool("configured", true);
    preferences.putBool("own_provisional", true); // поки що до підтвердження з сервера
    preferences.end();

    bleConfigured = true;

    // 3) Відповідь у Android: chipId по notify
    pCharacteristic->setValue(uniqueId.c_str());
    pCharacteristic->notify();
    Serial.println("📤 Надіслано chipId назад: " + uniqueId);

    // 4) Якщо Wi-Fi ще не підключений — пробуємо одразу
    if (WiFi.status() != WL_CONNECTED) {
      scanAndConnectWiFi();
    }

    // 5) Перший синк ownership — через 5 с після отримання конфігу
    nextSyncAt = millis() + 5000;
  }
};

// ─────────────────────────────────────────────────────────────────────────────
// Wi-Fi
// ─────────────────────────────────────────────────────────────────────────────
void scanAndConnectWiFi() {
  Serial.println("Сканування Wi-Fi...");
  int n = WiFi.scanNetworks();
  if (n == 0) {
    Serial.println("Wi-Fi мереж не знайдено");
    return;
  }

  // Читаємо cfg
  preferences.begin("config", true); // read-only
  savedSSID = preferences.getString("ssid", "");
  String encrypted = preferences.getString("enc_pwd", "");
  preferences.end();

  savedPass = decryptPassword(encrypted);
  Serial.println("🔐 Зашифрований пароль: " + encrypted);
  Serial.println("🔓 Дешифровано пароль: " + savedPass);

  if (savedSSID.isEmpty() || savedPass.isEmpty()) {
    Serial.println("❌ Немає збереженого SSID або пароля. Пропуск Wi-Fi підключення.");
    return;
  }

  // Підключаємось до потрібної мережі
  for (int i = 0; i < n; ++i) {
    if (WiFi.SSID(i) == savedSSID) {
      WiFi.mode(WIFI_STA);
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
        delay(3000); // коротко показати IP

        // ⭐ Запланувати перший синк ownership через 5 секунд
        nextSyncAt = millis() + 5000;
        return;
      }
    }
  }
  Serial.println("Не знайдено збереженої мережі");
}

void checkWiFiConnection() {
  if (millis() - wifiCheckTime >= wifiCheckInterval) {
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.disconnect();
      scanAndConnectWiFi();
    }
    wifiCheckTime = millis();
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// LCD
// ─────────────────────────────────────────────────────────────────────────────
void updateDisplay(float tempC, float humi, int smokeState, int lightState, float pressure) {
  lcd.clear();

  // Перший екран — статус
  if (statusDisplayed) {
    lcd.setCursor(0, 0);
    lcd.print("BLE:");
    lcd.print(bleConfigured ? "OK " : "WAIT");
    lcd.print(" WiFi:");
    lcd.print(WiFi.status() == WL_CONNECTED ? "OK" : "NO");

    if (bleConfigured && WiFi.status() == WL_CONNECTED) {
      statusDisplayed = false;
      // Привітання (Рядок 4)
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
    lcd.write(223); // символ °
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
  lcd.print("Gas:"); lcd.print(smokeState == HIGH ? "Yes" : "No ");
  lcd.print(" Light:"); lcd.print(lightState == HIGH ? "Dark" : "Light");

  // Рядок 4: назва кімнати
  lcd.setCursor(0, 3);
  preferences.begin("config", true); // лише читання
  String roomName = preferences.getString("roomName", "NoRoom");
  preferences.end();
  if (roomName.length() <= 20) {
    lcd.print(roomName);
  } else {
    lcd.print(roomName.substring(0, 20));
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// SETUP
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  // Унікальний ChipId + BLE ім'я
  uint64_t chipid = ESP.getEfuseMac();
  char id[13];
  sprintf(id, "%04X%08X", (uint32_t)(chipid >> 32), (uint32_t)chipid);
  uniqueId = String(id);
  String shortId = uniqueId.substring(uniqueId.length() - 6);
  bleName = "ESP32_" + shortId;

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

  // NVS: чи вже конфігуровано
  preferences.begin("config", false);
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

  // Wi-Fi (за наявності конфігу з BLE)
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

  // 3) ⭐ ПЕРІОДИЧНИЙ СИНК OWNERSHIP (GET .../ownership/{chipId}/latest, без токена)
  if (WiFi.status() == WL_CONNECTED && bleConfigured && millis() >= nextSyncAt) {
    int rc = syncOwnershipNoAuth();   // 1: оновлено, 0: успішно без змін, -1: помилка
    scheduleNextSync(rc);             // 30 хв або 5 хв (бекоф)
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
    float lightPercent     = 100 - (lightAnalogValue * 100.0 / 4095.0);

    // Формуємо JSON (узгоджено з 16x2)
    String json = "{";
    json += "\"ChipId\":\"" + uniqueId + "\",";

    json += "\"TemperatureDht\":";
    json += isnan(tempC) ? "null" : String(tempC, 2);
    json += ",";

    json += "\"HumidityDht\":";
    json += isnan(humi) ? "null" : String(humi, 2);
    json += ",";

    json += "\"TemperatureBme\":";  json += (!isnan(bmeTemp))     ? String(bmeTemp, 2)     : "null"; json += ",";
    json += "\"HumidityBme\":";     json += (!isnan(bmeHumi))     ? String(bmeHumi, 2)     : "null"; json += ",";
    json += "\"Pressure\":";        json += (!isnan(bmePressure)) ? String(bmePressure, 2) : "null"; json += ",";
    json += "\"Altitude\":";        json += (!isnan(bmeAltitude)) ? String(bmeAltitude, 2) : "null"; json += ",";

    json += "\"GasDetected\":";     json += (smokeState == HIGH ? "true" : "false"); json += ",";
    json += "\"Light\":";           json += (lightState == HIGH ? "true" : "false"); json += ",";

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
