// ✅ ESP32 16x2 (BMP280) — синхронізовано з 20x4 (BME) версією:
// - BLE reset + збереження конфігів у Preferences
// - BLE notify: відправляємо ChipId назад Android-у
// - LCD статус: BLE/WiFi, привітання Hello, username (скрол для 16 символів)
// - Той самий JSON формат для /api/sensordata (boolean для GasDetected/Light, числові поля)
// - Єдина логіка інтервалів (display 10s, WiFi check 60s, POST 10 хв)

#include <Wire.h>
#include <DHT.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Arduino_JSON.h>
#include <SPI.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_Sensor.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <AESLib.h>

// === Піни ESP32 ===
#define DHT_PIN         4
#define Smoke_PIN       25
#define Light_PIN       26
#define I2C_SDA_PIN     21
#define I2C_SCL_PIN     22
#define MQ2_ANALOG_PIN  34
#define LIGHT_ANALOG_PIN 35

// === LCD (I2C) 16x2 ===
#define LCD_ADDRESS 0x3f
#define LCD_WIDTH   16
#define LCD_HEIGHT  2
LiquidCrystal_I2C lcd(LCD_ADDRESS, LCD_WIDTH, LCD_HEIGHT);

#define SEALEVELPRESSURE_HPA (1013.25)
Adafruit_BMP280 bmp;            // BMP280 (t, p, altitude)
DHT dht(DHT_PIN, DHT11);        // DHT11 (t, h)

const char* serverName = "http://192.168.92.32:5210/api/sensordata"; // ← як у 20×4
bool statusDisplayed = true;
String uniqueId;
String savedSSID = "";
String savedPass = "";
String bleName;
bool bleConfigured = false;
bool bmpDetected = true;

AESLib aesLib;

// === AES (як у Android, "my-secret-key-12") ===
byte aes_key[] = {
  'm','y','-','s','e','c','r','e','t','-','k','e','y','-','1','2'
};
byte aes_iv[]  = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};

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

  // PKCS7 padding (безпечна перевірка меж)
  if (len > 0) {
    int pad = decrypted[len - 1];
    if (pad > 0 && pad <= 16 && pad <= result.length()) {
      result.remove(result.length() - pad);
    }
  }
  return result;
}

// === BLE ===
Preferences preferences;
BLECharacteristic *pCharacteristic;
#define SERVICE_UUID        "12345678-1234-1234-1234-123456789abc"
#define CHARACTERISTIC_UUID "abcd1234-5678-1234-5678-abcdef123456"

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

    // 🔄 RESET
    bool shouldReset = data.hasOwnProperty("reset") && (bool)data["reset"];
    if (shouldReset) {
      Serial.println("🔄 Очищення Preferences через reset=true");
      preferences.begin("config", false);
      preferences.clear();
      preferences.end();
      delay(100);
    }

    // ✅ Збереження нових параметрів
    String username          = (const char*)data["username"];
    String imageName         = (const char*)data["imageName"];
    String ssid              = (const char*)data["ssid"];
    String encryptedPassword = (const char*)data["password"];
    String roomName          = (const char*)data["roomName"];

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

    // 📤 Як у 20x4: одразу надсилаємо ChipId назад в Android по notify
    pCharacteristic->setValue(uniqueId.c_str());
    pCharacteristic->notify();
    Serial.println("📤 Надіслано chipId назад: " + uniqueId);
  }
};

// === Таймери ===
unsigned long lastTime = 0, displayRefreshTime = 0, wifiCheckTime = 0;
const long timerDelay = 100000;             // 10 хв — як у 20x4
const long displayRefreshInterval = 10000;  // 10 с
const long wifiCheckInterval = 60000;       // 60 с

// Для чергування другого рядка між сенсорами та назвою кімнати (на 16 символах)
bool showRoomLine = false;

void scanAndConnectWiFi() {
  Serial.println("Сканування Wi-Fi...");
  int n = WiFi.scanNetworks();
  if (n == 0) {
    Serial.println("Wi-Fi мереж не знайдено");
    return;
  }

  preferences.begin("config", true); // read-only
  savedSSID = preferences.getString("ssid", "");
  String encrypted = preferences.getString("enc_pwd", "");
  String username = preferences.getString("username", "");
  preferences.end();

  savedPass = decryptPassword(encrypted);
  Serial.println("🔐 Зашифрований пароль: " + encrypted);
  Serial.println("🔓 Дешифровано пароль: " + savedPass);

  if (savedSSID.isEmpty() || savedPass.isEmpty()) {
    Serial.println("❌ Немає збереженого SSID або пароля. Пропуск Wi-Fi підключення.");
    return;
  }

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
        delay(3000); // коротко показати IP
        return;
      }
    }
  }
  Serial.println("Не знайдено збереженої мережі");
}

void updateDisplay(float tempC, float humi, int smokeState, int lightState, float pressure) {
  lcd.clear();

  // Спершу — статусний екран (як у 20x4)
  if (statusDisplayed) {
    lcd.setCursor(0, 0);
    lcd.print("BLE:");
    lcd.print(bleConfigured ? "OK " : "WAIT");
    lcd.print(" WiFi:");
    lcd.print(WiFi.status() == WL_CONNECTED ? "OK" : "NO");

    if (bleConfigured && WiFi.status() == WL_CONNECTED) {
      statusDisplayed = false;
      // Після першого успішного екрану — привітання
      preferences.begin("config", true);
      String username = preferences.getString("username", "");
      preferences.end();

      if (username.length() > 0) {
        String hello = "Hello, " + username;
        if (hello.length() <= 16) {
          lcd.setCursor(0, 1);
          lcd.print(hello);
          delay(1500);
        } else {
          // примітивний скрол для 16 символів
          for (int i = 0; i <= hello.length() - 16; i++) {
            lcd.setCursor(0, 1);
            lcd.print(hello.substring(i, i + 16));
            delay(300);
          }
        }
      }
    }
    return;
  }

  // Основний екран (2 рядки)
  // 1-й рядок: T/H (цілі значення, як у 20x4)
  lcd.setCursor(0, 0);
  if (isnan(tempC) || isnan(humi)) {
    lcd.print("Temp/Hum: ERROR");
  } else {
    lcd.print("T:");
    lcd.print((int)tempC);
    lcd.write(223); // символ градуса
    lcd.print("C H:");
    lcd.print((int)humi);
    lcd.print("%");
  }

  // 2-й рядок: або сенсорна зведенка, або назва кімнати (чергується)
  lcd.setCursor(0, 1);
  if (!showRoomLine) {
    // Сенсори: P + Gas + Light (втиснуто в 16 симв.)
    if (!bmpDetected || isnan(pressure)) {
      lcd.print("P:ERR ");
    } else {
      // приклад: "P:1013hPa "
      int pInt = (int)pressure;
      lcd.print("P:");
      lcd.print(pInt);
      lcd.print("hPa ");
    }
    lcd.print("G:");
    lcd.print(smokeState == LOW ? "Y " : "N ");
    lcd.print("L:");
    lcd.print(lightState == HIGH ? "D" : "L"); // Dark/Light як D/L
  } else {
    preferences.begin("config", true);
    String roomName = preferences.getString("roomName", "NoRoom");
    preferences.end();
    if (roomName.length() <= 16) {
      lcd.print(roomName);
    } else {
      // показуємо останні 16 символів (або можна теж прокрутити)
      lcd.print(roomName.substring(0, 16));
    }
  }
  showRoomLine = !showRoomLine; // чергуємо екран раз на refresh
}

void setup() {
  Serial.begin(115200);

  // === Унікальний ChipId та BLE ім'я ===
  uint64_t chipid = ESP.getEfuseMac();
  char id[13];
  sprintf(id, "%04X%08X", (uint32_t)(chipid >> 32), (uint32_t)chipid);
  uniqueId = String(id);
  String shortId = uniqueId.substring(uniqueId.length() - 6);
  bleName = "ESP32_" + shortId;

  // === BLE init ===
  BLEDevice::init(bleName.c_str());
  Serial.println("=== BLE ІНІЦІАЛІЗАЦІЯ ===");
  Serial.println("ChipId: " + uniqueId);
  Serial.println("BLE Name: " + bleName);

  BLEServer *pServer = BLEDevice::createServer();
  BLEService *pService = pServer->createService(SERVICE_UUID);
  pCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_NOTIFY // ✅ notify
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

  // === Preferences ===
  preferences.begin("config", false);
  bleConfigured = preferences.getBool("configured", false);
  preferences.end();

  // === I2C/LCD/Sensors ===
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
    Serial.println("Помилка BMP280");   // ✅ виправили текст
    bmpDetected = false;
  }

  // === Wi-Fi ===
  if (bleConfigured) {
    scanAndConnectWiFi();
  }
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

void loop() {
  checkWiFiConnection();

  if (millis() - displayRefreshTime >= displayRefreshInterval) {
    float tempC      = dht.readTemperature();
    float humi       = dht.readHumidity();
    int smokeState   = digitalRead(Smoke_PIN);
    int lightState   = digitalRead(Light_PIN);
    float pressure   = bmp.readPressure() / 100.0F;
    updateDisplay(tempC, humi, smokeState, lightState, pressure);
    displayRefreshTime = millis();
  }

  // Відправка на сервер раз на 10 хв (як у 20x4), лише якщо BLE конфіг є і Wi-Fi підключено
  if ((millis() - lastTime) > timerDelay && WiFi.status() == WL_CONNECTED && bleConfigured) {
    float tempC      = dht.readTemperature();
    float humi       = dht.readHumidity();
    int smokeState   = digitalRead(Smoke_PIN);
    int lightState   = digitalRead(Light_PIN);

    float bmeTemp    = bmp.readTemperature();        // тут BMP температура
    float bmePressure= bmp.readPressure() / 100.0F;  // тут BMP тиск
    float bmeHumi    = NAN;                          // BMP не має вологості
    float bmeAltitude= bmp.readAltitude(SEALEVELPRESSURE_HPA);

    int mq2AnalogValue  = analogRead(MQ2_ANALOG_PIN);
    int lightAnalogValue= analogRead(LIGHT_ANALOG_PIN);
    float mq2Percent    = mq2AnalogValue  * 100.0 / 4095.0;
    float lightPercent  = 100 - (lightAnalogValue * 100.0 / 4095.0);

    // Формат JSON — такий самий, як у 20×4
    String json = "{";

    json += "\"ChipId\":\"" + uniqueId + "\",";

    // DHT
    json += "\"TemperatureDht\":";
    json += isnan(tempC) ? "null" : String(tempC, 2);
    json += ",";

    json += "\"HumidityDht\":";
    json += isnan(humi) ? "null" : String(humi, 2);
    json += ",";

    // “BME” поля заповнюємо з BMP там, де можливо
    json += "\"TemperatureBme\":";
    json += (!isnan(bmeTemp)) ? String(bmeTemp, 2) : "null";
    json += ",";

    json += "\"HumidityBme\":";
    json += "null,"; // BMP не має вологості

    json += "\"Pressure\":";
    json += (!isnan(bmePressure)) ? String(bmePressure, 2) : "null";
    json += ",";

    json += "\"Altitude\":";
    json += (!isnan(bmeAltitude)) ? String(bmeAltitude, 2) : "null";
    json += ",";

    // boolean як у 20×4 (не "yes"/"no", не "dark"/"light")
    json += "\"GasDetected\":";
    json += (smokeState == HIGH ? "true" : "false");
    json += ",";

    json += "\"Light\":";
    json += (lightState == HIGH ? "true" : "false");
    json += ",";

    // Аналогові
    // MQ2Analog
    json += "\"MQ2Analog\":" + String(mq2AnalogValue) + ",";

    // MQ2AnalogPercent
    json += "\"MQ2AnalogPercent\":" + String(mq2Percent, 2) + ",";

    // LightAnalog
    json += "\"LightAnalog\":" + String(lightAnalogValue) + ",";

    // LightAnalogPercent
    json += "\"LightAnalogPercent\":" + String(lightPercent, 2);

    json += "}";

    // 🔹 Відправлення
    HTTPClient http;
    http.begin(serverName);
    http.addHeader("Content-Type", "application/json");

    Serial.println("➡️ Надсилається JSON:");
    Serial.println(json);

    int code = http.POST(json);
    if (code > 0) Serial.println("POST OK: " + String(code));
    else Serial.println("POST ERR: " + String(code));
    http.end();

    lastTime = millis();
  }
}
