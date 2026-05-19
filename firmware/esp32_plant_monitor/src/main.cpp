#include <Arduino.h>
#include <Wire.h>
#include <BH1750.h>


constexpr int PIN_MOISTURE = 34;
constexpr int PIN_RELAY    = 26;   // active LOW


constexpr int MOISTURE_DRY_RAW = 3200;
constexpr int MOISTURE_WET_RAW = 1200;

constexpr unsigned long PUMP_ON_DURATION_MS  = 5000;   // 5 seconds ON
constexpr unsigned long PUMP_OFF_DURATION_MS = 10000;  // 10 seconds OFF

BH1750 lightMeter;
bool lightSensorOk = false;

unsigned long lastPumpToggle = 0;
bool pumpIsOn = false;

void setup() {
  Serial.begin(115200);
  delay(1000);

  analogReadResolution(12);
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, HIGH); 

  Wire.begin();
  if (lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
    lightSensorOk = true;
    Serial.println("BH1750 OK");
  } else {
    Serial.println("BH1750 not found");
  }

  lastPumpToggle = millis();
}

void loop() {
  unsigned long now = millis();

  if (pumpIsOn && (now - lastPumpToggle >= PUMP_ON_DURATION_MS)) {
    digitalWrite(PIN_RELAY, HIGH);   // OFF
    pumpIsOn = false;
    lastPumpToggle = now;
  } else if (!pumpIsOn && (now - lastPumpToggle >= PUMP_OFF_DURATION_MS)) {
    digitalWrite(PIN_RELAY, LOW);    // ON
    pumpIsOn = true;
    lastPumpToggle = now;
  }

  int raw = analogRead(PIN_MOISTURE);
  float moisture = map(raw, MOISTURE_DRY_RAW, MOISTURE_WET_RAW, 0, 100);
  moisture = constrain(moisture, 0.0f, 100.0f);

  float lux = -1.0f;
  if (lightSensorOk) {
    lux = lightMeter.readLightLevel();
  }

  Serial.print("Moisture: ");
  Serial.print(moisture);
  Serial.print("% | Lux: ");
  Serial.print(lux);
  Serial.print(" | Pump: ");
  Serial.println(pumpIsOn ? "ON" : "OFF");

  delay(1000);
}