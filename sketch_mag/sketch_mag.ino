#include <Wire.h>
#include <DHT.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Arduino_JSON.h>
#include <SPI.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
//#include <WiFiClientSecure.h>

// === Піни ESP32 ===
#define DHT_PIN     4
#define Smoke_PIN   25
#define Light_PIN   26
#define I2C_SDA_PIN 21
#define I2C_SCL_PIN 22

// === LCD (I2C) ===
#define LCD_ADDRESS 0x27
#define LCD_WIDTH   20
#define LCD_HEIGHT  4
LiquidCrystal_I2C lcd(LCD_ADDRESS, LCD_WIDTH, LCD_HEIGHT);

#define SEALEVELPRESSURE_HPA (1013.25)
Adafruit_BME280 bme;

// === DHT11 ===
DHT dht(DHT_PIN, DHT11);

// === WiFi ===
const char* ssid = "Esp32_test";
const char* password = "87654321";
const char* serverName = "https://192.168.159.32:7180/api/sensordata";
// "http://192.168.159.32:80/api/sensordata"

// === Таймер ===
unsigned long lastTime = 0;
unsigned long timerDelay = 600000;

//WiFiClientSecure client;

void setup() {
  Serial.begin(115200);

  // Wi-Fi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.println("Підключення до WiFi...");
  }
  Serial.println("WiFi підключено");

  // I2C шина (LCD + BME280)
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  // LCD
  lcd.init();
  lcd.backlight();

  // DHT11 + MQ2 + світло
  dht.begin();
  pinMode(Smoke_PIN, INPUT);
  pinMode(Light_PIN, INPUT);
  Serial.println("Очікування MQ2...");
  delay(20000); // нагрів MQ2

  bool bme_status = bme.begin(0x76);
  if (!bme_status) {
    Serial.println("Не знайдено BME280!");
    while (1);
  }
}

void loop() {
  if ((millis() - lastTime) > timerDelay) {
    if (WiFi.status() == WL_CONNECTED) {
      float tempC = dht.readTemperature();
      float humi  = dht.readHumidity();
      int smokeState = digitalRead(Smoke_PIN);
      int lightState = digitalRead(Light_PIN);

      float bmeTemp = bme.readTemperature();
      float bmePressure = bme.readPressure() / 100.0F;
      float bmeAltitude = bme.readAltitude(SEALEVELPRESSURE_HPA);

      if (isnan(tempC) || isnan(humi) || isnan(bmeTemp)) {
        Serial.println("Помилка читання з сенсорів");
        delay(1000);
        return;
      }

      // === LCD ===
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("T:");
      lcd.print(tempC);
      lcd.print("C H:");
      lcd.print(humi);
      lcd.print("%");

      lcd.setCursor(0, 1);
      lcd.print("P:");
      lcd.print(bmePressure);
      lcd.print("hPa ");

      lcd.setCursor(0, 2);
      lcd.print("Gas:");
      lcd.print(smokeState == LOW ? "Detected" : "Not Detected");

      lcd.setCursor(0, 3);
      lcd.print("Light:");
      lcd.print(lightState == HIGH ? "It is Dark" : "It is Light");

      // === JSON ===
      JSONVar jsonData;
      jsonData["TemperatureDht"] = tempC;
      jsonData["HumidityDht"]    = humi;
      jsonData["GasDetected"]    = smokeState == LOW ? "yes" : "no";
      jsonData["Light"]          = lightState == HIGH ? "dark" : "light";
      jsonData["Pressure"]       = bmePressure;
      jsonData["Altitude"]       = bmeAltitude;

      String jsonString = JSON.stringify(jsonData);
      Serial.println("JSON:");
      Serial.println(jsonString);

      // === HTTP POST ===
      HTTPClient http;
      http.begin(serverName);
      http.addHeader("Content-Type", "application/json");
      int httpResponseCode = http.POST(jsonString);

      if (httpResponseCode > 0) {
        String response = http.getString();
        Serial.print("HTTP: ");
        Serial.println(httpResponseCode);
        Serial.println("Відповідь:");
        Serial.println(response);
      } else {
        Serial.print("POST помилка: ");
        Serial.println(httpResponseCode);
      }

      http.end();
    } else {
      Serial.println("Повторне підключення WiFi...");
      WiFi.begin(ssid, password);
    }

    lastTime = millis();
  }
}
