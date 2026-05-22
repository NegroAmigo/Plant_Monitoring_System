#include <Arduino.h>
#include <Wire.h>
#include <BH1750.h>
#include <stdarg.h>

constexpr int PIN_MOISTURE = 34;
constexpr int PIN_RELAY    = 26;   // active LOW

constexpr int MOISTURE_DRY_RAW = 3200;
constexpr int MOISTURE_WET_RAW = 1200;

constexpr float    PUMP_ON_THRESHOLD      = 35.0f;
constexpr float    PUMP_OFF_THRESHOLD     = 70.0f;
constexpr uint32_t PUMP_MAX_RUN_MS        = 15000;  
constexpr uint32_t PUMP_COOLDOWN_MS       = 100000;
constexpr uint32_t PUMP_FAIL_COOLDOWN_MS  = 300000;
constexpr uint8_t  PUMP_MAX_TRIES         = 3;       // max failed cycles

constexpr float PUMP_FLOW_RATE_L_PER_S = 100.0f / 3600.0f;

constexpr float   TANK_EMPTY_MOISTURE_RISE = 3.0f;
constexpr uint8_t TANK_EMPTY_STRIKES       = 3;


constexpr uint8_t AVG_WINDOW = 5;

constexpr uint32_t TELEMETRY_INTERVAL_MS = 10000;


BH1750 lightMeter;
bool lightSensorOk = false;

float moistureBuf[AVG_WINDOW] = {};
float luxBuf[AVG_WINDOW]      = {};
uint8_t bufIdx                = 0;
bool bufFull                  = false;

// pump state
bool     pumpRunning          = false;
uint32_t pumpStartMs          = 0;
uint32_t pumpLastOffMs        = 0;
uint32_t pumpLastFailMs       = 0;
float    totalWaterUsed_L     = 0.0f;
float    moistureBeforePump   = 0.0f;
uint8_t  tankEmptyStrikeCount = 0;
bool     tankEmpty            = false;
uint8_t  pumpFailCount        = 0; 
bool     pumpLocked           = false; 

// telemetry timer
uint32_t lastTelemetryMs = 0;

//void logPrintf(const char* format, ...);

void logPrintf(const char* format, ...) {
  char buf[256];
  va_list args;
  va_start(args, format);
  vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  Serial.println(buf);
}


void pumpOn() {
  if (!pumpRunning) {
    digitalWrite(PIN_RELAY, LOW); 
    pumpRunning = true;
    pumpStartMs = millis();
    logPrintf("[PUMP] ON");
  }
}

void pumpOff(const char* reason, bool isAlert = false) {
  if (pumpRunning) {
    uint32_t runMs     = millis() - pumpStartMs;
    float    runSec    = runMs / 1000.0f;
    float    waterUsed = PUMP_FLOW_RATE_L_PER_S * runSec;
    totalWaterUsed_L  += waterUsed;

    digitalWrite(PIN_RELAY, HIGH);
    pumpRunning   = false;
    pumpLastOffMs = millis();

    if (isAlert) {
      logPrintf("[ALERT] %s", reason);
    }

    logPrintf(
      "[PUMP] OFF - reason: %s | ran: %.1f s | used: %.4f L | total: %.3f L",
      reason,
      runSec,
      waterUsed,
      totalWaterUsed_L
    );
  }
}

float averagedMoisture() {
  uint8_t count = bufFull ? AVG_WINDOW : bufIdx;
  if (count == 0) return -1.0f;

  float sum = 0;
  for (uint8_t i = 0; i < count; i++) sum += moistureBuf[i];
  return sum / count;
}

float averagedLux() {
  uint8_t count = bufFull ? AVG_WINDOW : bufIdx;
  if (count == 0) return -1.0f;

  float sum = 0;
  for (uint8_t i = 0; i < count; i++) sum += luxBuf[i];
  return sum / count;
}

void sampleSensors() {
  int raw = analogRead(PIN_MOISTURE);

  float pct = (float)map(raw,
                         MOISTURE_DRY_RAW,
                         MOISTURE_WET_RAW,
                         0,
                         100);
  pct = constrain(pct, 0.0f, 100.0f);

  bool moistureFault = (raw <= 0 || raw >= 4095);
  float prevMoisture = averagedMoisture();
  if (prevMoisture < 0) prevMoisture = 0.0f;

  moistureBuf[bufIdx] = moistureFault ? prevMoisture : pct;

  float lux = -1.0f;
  if (lightSensorOk) {
    lux = lightMeter.readLightLevel();

    if (lux < 0) {
      float prevLux = averagedLux();
      lux = (prevLux >= 0) ? prevLux : -1.0f;
    }
  }

  luxBuf[bufIdx] = lux;

  bufIdx++;
  if (bufIdx >= AVG_WINDOW) {
    bufIdx = 0;
    bufFull = true;
  }
}

void printTelemetry(float moisture, float lux) {
  char buf[200];
  snprintf(buf, sizeof(buf),
    "[TELEM] Moist:%.1f%% Lux:%.0f Pump:%s Water:%.3fL Tank_OK:%s Locked:%s Uptime:%lus",
    moisture,
    lux,
    pumpRunning ? "ON" : "OFF",
    totalWaterUsed_L,
    tankEmpty  ? "NO"  : "YES",
    pumpLocked ? "YES" : "NO",
    millis() / 1000
  );
  Serial.println(buf);
}


void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("");
  Serial.println("=== Plant Monitor Boot ===");

  analogReadResolution(12);

  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, HIGH);

  Serial.println("[INIT] Relay pin configured - pump OFF");

  
  Wire.begin(); // SDA=21, SCL=22

  if (lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
    lightSensorOk = true;
    Serial.println("[INIT] BH1750 OK");
  } else {
    lightSensorOk = false;
    Serial.println("[WARN] BH1750 not found - light data unavailable");
  }

  Serial.println("[INIT] Priming sensor averages...");
  for (uint8_t i = 0; i < AVG_WINDOW; i++) {
    sampleSensors();
    delay(200);
  }

  Serial.println("[TEST] Starting system self-test...");

  int testRaw = analogRead(PIN_MOISTURE);
  if (testRaw <= 0 || testRaw >= 4095) {
    logPrintf("[TEST] WARN: Moisture sensor may be disconnected (raw=%d)", testRaw);
  } else {
    logPrintf("[TEST] Moisture sensor OK (raw=%d)", testRaw);
  }

  if (lightSensorOk) {
    float testLux = lightMeter.readLightLevel();
    if (testLux < 0) {
      logPrintf("[TEST] WARN: BH1750 read failed during self-test");
    } else {
      logPrintf("[TEST] BH1750 OK (%.1f lux)", testLux);
    }
  } else {
    logPrintf("[TEST] SKIP: BH1750 not available");
  }

  logPrintf("[TEST] Relay ON...");
  digitalWrite(PIN_RELAY, LOW);
  delay(500);
  digitalWrite(PIN_RELAY, HIGH);
  logPrintf("[TEST] Relay OFF - check pump twitched");

  logPrintf("[TEST] Self-test complete");
  logPrintf("[INIT] Boot complete");
}

void loop() {
  uint32_t now = millis();


  sampleSensors();

  float moisture = averagedMoisture();
  float lux      = averagedLux();

  if (pumpRunning &&
      (now - pumpStartMs >= PUMP_MAX_RUN_MS)) {

    pumpOff("MAX_RUNTIME_EXCEEDED", /*isAlert=*/true);

    float rise = averagedMoisture() - moistureBeforePump;

    if (rise < TANK_EMPTY_MOISTURE_RISE) {
      tankEmptyStrikeCount++;
      pumpFailCount++;
      pumpLastFailMs = millis();

      logPrintf(
        "[TANK] No moisture rise detected (rise=%.1f%%). Strike %d/%d | Fail %d/%d",
        rise,
        tankEmptyStrikeCount,
        TANK_EMPTY_STRIKES,
        pumpFailCount,
        PUMP_MAX_TRIES
      );

      if (tankEmptyStrikeCount >= TANK_EMPTY_STRIKES) {
        tankEmpty = true;
        logPrintf("[TANK] *** TANK EMPTY - pumping disabled until reset ***");
      }

      if (pumpFailCount >= PUMP_MAX_TRIES && !tankEmpty) {
        pumpLocked = true;
        logPrintf(
          "[PUMP] *** PUMP LOCKED after %d consecutive failures - manual reset required ***",
          PUMP_MAX_TRIES
        );
      }
    } else {
      tankEmptyStrikeCount = 0;
      pumpFailCount        = 0;
    }
  }

  if (!tankEmpty && !pumpLocked) {
    bool failCooledDown =
      (now - pumpLastFailMs >= PUMP_FAIL_COOLDOWN_MS) ||
      (pumpLastFailMs == 0);

    bool cooledDown =
      ((now - pumpLastOffMs >= PUMP_COOLDOWN_MS) ||
       (pumpLastOffMs == 0)) &&
      failCooledDown;

    if (!pumpRunning &&
        moisture < PUMP_ON_THRESHOLD &&
        cooledDown) {
      moistureBeforePump = moisture;
      pumpOn();
    }

    if (pumpRunning &&
        moisture >= PUMP_OFF_THRESHOLD) {
      pumpOff("TARGET_MOISTURE_REACHED", /*isAlert=*/false);

      float rise = moisture - moistureBeforePump;
      if (rise >= TANK_EMPTY_MOISTURE_RISE) {
        tankEmptyStrikeCount = 0;
        pumpFailCount        = 0;
      }
    }
  } else {
    if (pumpRunning) {
      if (tankEmpty) {
        pumpOff("TANK_EMPTY_SAFETY", /*isAlert=*/true);
      } else if (pumpLocked) {
        pumpOff("PUMP_LOCKED_SAFETY", /*isAlert=*/true);
      }
    }
  }

  logPrintf(
    "[SENS] Moisture: %.1f%% | Lux: %.1f | Pump: %s | Water: %.3f L | Tank OK: %s | Locked: %s",
    moisture,
    lux,
    pumpRunning ? "ON" : "OFF",
    totalWaterUsed_L,
    tankEmpty  ? "NO"  : "YES",
    pumpLocked ? "YES" : "NO"
  );

  if (now - lastTelemetryMs >= TELEMETRY_INTERVAL_MS) {
    printTelemetry(moisture, lux);
    lastTelemetryMs = now;
  }

  delay(1000); 
}