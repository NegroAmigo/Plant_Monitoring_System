#include <Arduino.h>
#include <Wire.h>
#include <BH1750.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <Adafruit_MQTT.h>
#include <Adafruit_MQTT_Client.h>
#include <stdarg.h>

#ifndef WIFI_SSID
  #error "WIFI_SSID not defined. Add -DWIFI_SSID=\\\"yourSSID\\\" to build flags."
#endif
#ifndef WIFI_PASSWORD
  #error "WIFI_PASSWORD not defined."
#endif
#ifndef AIO_USERNAME
  #error "AIO_USERNAME not defined."
#endif
#ifndef AIO_KEY
  #error "AIO_KEY not defined."
#endif

// work around to convert secrets into str
#define _STR(x) #x
#define STR(x) _STR(x)

#define AIO_SERVER "io.adafruit.com"
#define AIO_SERVERPORT 1883
#define AIO_TOPIC(feed) STR(AIO_USERNAME) "/feeds/" feed

// hardcoded feed names on Adafruitio
#define FEED_MOISTURE "plant-monitoring-feed.plant-dot-moisture"
#define FEED_LIGHT "plant-monitoring-feed.plant-dot-light"
#define FEED_PUMP_STATE "plant-monitoring-feed.plant-dot-pump-state"
#define FEED_TANK_STATE "plant-monitoring-feed.plant-dot-tank-state"
#define FEED_USED_WATER "plant-monitoring-feed.plant-dot-used-water"
#define FEED_ALERT "plant-monitoring-feed.plant-dot-alert"
#define FEED_PUMP_CONTROL "plant-monitoring-feed.plant-dot-pump-control"

constexpr gpio_num_t PIN_RELAY = GPIO_NUM_26; // active low
constexpr int PIN_MOISTURE = 34;

// moisture edge values
constexpr int MOISTURE_DRY_RAW = 3200;
constexpr int MOISTURE_WET_RAW = 1200;

// moisture read limits
constexpr int MOISTURE_FAULT_LOW = 50;
constexpr int MOISTURE_FAULT_HIGH = 4050;

constexpr uint8_t AVG_WINDOW = 5;

constexpr float PUMP_ON_THRESHOLD = 35.0f;
constexpr float PUMP_OFF_THRESHOLD = 70.0f;
constexpr uint32_t PUMP_MAX_RUN_MS = 15000UL; // hard cutoff so we dont flood the pot
constexpr uint32_t PUMP_COOLDOWN_MS = 100000UL;
constexpr uint32_t PUMP_FAIL_COOLDOWN_MS = 300000UL;
constexpr uint8_t PUMP_MAX_TRIES = 3;

// approximate pump flow
constexpr float PUMP_FLOW_LPS = 100.0f / 3600.0f;

// consts in case of tank issues
constexpr float TANK_RISE_MIN_PCT = 3.0f; // if moisture didnt rose by this - tank might be empty
constexpr uint8_t TANK_EMPTY_STRIKES = 3; // retry policy for tank issues

// wifi and Adafruit consts
constexpr uint8_t WIFI_MAX_RETRIES = 30;
constexpr uint8_t MQTT_MAX_RETRIES = 5;         // TODO: mqtt retry count
constexpr uint32_t MQTT_KEEPALIVE_MS = 30000UL; // TODO: alive policy for mqtt

constexpr uint32_t TELEMETRY_INTERVAL_MS = 30000UL; // interval of send to Adafruit
constexpr uint32_t LOOP_DELAY_MS = 1000UL;          // delay for loop()

// alert namespace
namespace Alert
{
  constexpr char NONE[] = "none";
  constexpr char TANK_EMPTY[] = "TANK_EMPTY";
  constexpr char PUMP_LOCKED[] = "PUMP_LOCKED";
  constexpr char MAX_RUNTIME[] = "MAX_RUNTIME_EXCEEDED";
  constexpr char FORCE_IGNORED_TANK[] = "FORCE_IGNORED_TANK_EMPTY";
  constexpr char FORCE_IGNORED_LOCKED[] = "FORCE_IGNORED_LOCKED";
  constexpr char MOISTURE_SENSOR_FAULT[] = "MOISTURE_SENSOR_FAULT";
}

// pump stops reasons
enum class PumpStop : uint8_t
{
  NORMAL, // reached target moisture
  FAULT,  // hit safety limit
};

BH1750 lightMeter;
bool lightSensorOk = false; //
bool moistureFault = false; // goes true when adc reads near the limits


// buffers to average the readings from sensors
float moistureBuf[AVG_WINDOW] = {};
float luxBuf[AVG_WINDOW] = {};
uint8_t bufIdx = 0;
bool bufFull = false;


bool pumpRunning = false;
uint32_t pumpStartMs = 0;
uint32_t pumpLastOffMs = 0;
uint32_t pumpLastFailMs = 0;
float totalWaterUsed_L = 0.0f;
float moistureAtStart = 0.0f; // snapshot when pump starts running

uint8_t tankStrikeCount = 0;
bool tankEmpty = false;
uint8_t failCount = 0;
bool pumpLocked = false;

volatile bool forcePumpRequested = false; // updated from Adafruit

uint32_t lastTelemetryMs = 0;
uint32_t lastMqttPingMs = 0;
const char *pendingAlert = Alert::NONE;

WiFiClient wifiClient;
Adafruit_MQTT_Client mqtt(&wifiClient, AIO_SERVER, AIO_SERVERPORT, STR(AIO_USERNAME), STR(AIO_KEY));
Adafruit_MQTT_Publish moisturePub(&mqtt, AIO_TOPIC(FEED_MOISTURE));
Adafruit_MQTT_Publish lightPub(&mqtt, AIO_TOPIC(FEED_LIGHT));
Adafruit_MQTT_Publish pumpStatePub(&mqtt, AIO_TOPIC(FEED_PUMP_STATE));
Adafruit_MQTT_Publish tankStatePub(&mqtt, AIO_TOPIC(FEED_TANK_STATE));
Adafruit_MQTT_Publish usedWaterPub(&mqtt, AIO_TOPIC(FEED_USED_WATER));
Adafruit_MQTT_Publish alertPub(&mqtt, AIO_TOPIC(FEED_ALERT));
Adafruit_MQTT_Publish ctrlPub(&mqtt, AIO_TOPIC(FEED_PUMP_CONTROL));

Adafruit_MQTT_Subscribe pumpControlSub(&mqtt, AIO_TOPIC(FEED_PUMP_CONTROL));


static float bufAverage(const float *buf)
{
  uint8_t n = bufFull ? AVG_WINDOW : bufIdx;
  if (n == 0)
    return -1.0f;
  float sum = 0.0f;
  for (uint8_t i = 0; i < n; i++)
    sum += buf[i];
  return sum / n;
}

float avgMoisture() { return bufAverage(moistureBuf); }
float avgLux() { return bufAverage(luxBuf); }

void sampleSensors()
{
  // avaraging moisture sensor to deal with noise
  int32_t rawSum = 0;
  for (uint8_t i = 0; i < 8; i++) rawSum += analogRead(PIN_MOISTURE);
  int raw = (int)(rawSum / 8);

  moistureFault = (raw <= MOISTURE_FAULT_LOW || raw >= MOISTURE_FAULT_HIGH);

  float pct;
  if (moistureFault)
  {
    // dont corrupt the average with a bad reading, hold the last known value
    float prev = avgMoisture();
    pct = (prev >= 0.0f) ? prev : 50.0f;
  }
  else
  {
    // mapping moisture into 0-100%
    pct = (float)map(raw, MOISTURE_DRY_RAW, MOISTURE_WET_RAW, 0, 100);
    pct = constrain(pct, 0.0f, 100.0f);
  }
  moistureBuf[bufIdx] = pct;

  float lux = -1.0f;
  if (lightSensorOk)
  {
    float reading = lightMeter.readLightLevel();
    if (reading >= 0.0f)
    {
      lux = reading;
    }
    else
    {
      // dont corrupt the average with a bad reading, hold the last known value
      float prev = avgLux();
      lux = (prev >= 0.0f) ? prev : -1.0f;
    }
  }
  luxBuf[bufIdx] = lux;

  if (++bufIdx >= AVG_WINDOW)
  {
    bufIdx = 0;
    bufFull = true;
  }
}

void pumpOn()
{
  if (pumpRunning)
    return;
  digitalWrite(PIN_RELAY, LOW); // turn on relay
  pumpRunning = true;
  pumpStartMs = millis();
  Serial.printf("[PUMP] ON\n");
  pumpStatePub.publish("1.0"); //send to Adafruit new state of pump
}

void pumpOff(const char *reason, PumpStop stopType)
{
  if (!pumpRunning)
    return;

  uint32_t runMs = millis() - pumpStartMs;
  float runSec = runMs / 1000.0f;
  float usedNow = PUMP_FLOW_LPS * runSec;
  totalWaterUsed_L += usedNow;

  digitalWrite(PIN_RELAY, HIGH); // turn off relay
  pumpRunning = false;
  pumpLastOffMs = millis();

  pumpStatePub.publish("0.0"); // send to Adafruit new state of pump
  ctrlPub.publish("OFF"); // turn off toggle on Adafruit

  Serial.printf("[PUMP] OFF | reason: %s | ran: %.1fs | used: %.4fL | total: %.3fL\n", reason, runSec, usedNow, totalWaterUsed_L);

  if (stopType == PumpStop::FAULT)
  {
    float rise = avgMoisture() - moistureAtStart;
    bool noRise = (rise < TANK_RISE_MIN_PCT);

    Serial.printf("[PUMP] Post-run moisture rise: %.1f%% (threshold: %.1f%%)\n", rise, TANK_RISE_MIN_PCT);
    
    //handling cases when moisture didnt rise
    if (noRise)
    {
      tankStrikeCount++;
      failCount++;
      pumpLastFailMs = millis();

      Serial.printf("[TANK] No rise - strike %d/%d | fail %d/%d\n", tankStrikeCount, TANK_EMPTY_STRIKES, failCount, PUMP_MAX_TRIES);

      if (tankStrikeCount >= TANK_EMPTY_STRIKES)
      {
        tankEmpty = true;
        pendingAlert = Alert::TANK_EMPTY;
        Serial.printf("[TANK] looks empty, disabling pump until tank is refilled\n");
      }
      else if (failCount >= PUMP_MAX_TRIES && !tankEmpty)
      {
        pumpLocked = true;
        pendingAlert = Alert::PUMP_LOCKED;
        Serial.printf("[PUMP] locked after %d failures\n", PUMP_MAX_TRIES);
      }
      else
      {
        pendingAlert = Alert::MAX_RUNTIME;
      }
    }
    else
    {
      // moisture rose - reset the strike counters
      tankStrikeCount = 0;
      failCount = 0;
      Serial.printf("[TANK] rise looks good, counters reset\n");
    }
  }
}

void handlePump()
{
  uint32_t now = millis();
  float moisture = avgMoisture();

  // if somehow the pump is running while blocked - kill it immediately
  if (pumpRunning && (tankEmpty || pumpLocked))
  {
    pumpOff(tankEmpty ? "TANK_EMPTY_SAFETY" : "PUMP_LOCKED_SAFETY", PumpStop::FAULT);
    return;
  }

  // if pump reaches maximum runtime - turn it off
  if (pumpRunning && (now - pumpStartMs >= PUMP_MAX_RUN_MS))
  {
    // pumpOff("MAX_RUNTIME", PumpStop::FAULT);
    pumpOff(Alert::MAX_RUNTIME, PumpStop::FAULT);
    return;
  }

  // if pump run and moisture reached upper limit - turn it off
  if (pumpRunning && moisture >= PUMP_OFF_THRESHOLD)
  {
    pumpOff("TARGET_REACHED", PumpStop::NORMAL);
    tankStrikeCount = 0;
    failCount = 0;
    return;
  }

  // do nothing if pump is running or tank empty or pump locked
  if (tankEmpty || pumpLocked || pumpRunning)
    return;

  // enough time passed since last run
  bool failCooldownOk = (pumpLastFailMs == 0) || (now - pumpLastFailMs >= PUMP_FAIL_COOLDOWN_MS);
  // enough time passed since last failure
  bool cooldownOk = ((pumpLastOffMs == 0) || (now - pumpLastOffMs >= PUMP_COOLDOWN_MS)) && failCooldownOk;

  // turn on pump if required and pump allowed to run
  if (moisture < PUMP_ON_THRESHOLD && cooldownOk && !moistureFault)
  {
    moistureAtStart = moisture;
    pumpOn();
    return;
  }

  // handling the request of pump run from Adafruit
  if (forcePumpRequested)
  {
    forcePumpRequested = false;
    if (tankEmpty)
    {
      Serial.printf("[PUMP] force ignored, tank is empty\n");
      pendingAlert = Alert::FORCE_IGNORED_TANK;
    }
    else if (pumpLocked)
    {
      Serial.printf("[PUMP] force ignored, pump is locked\n");
      pendingAlert = Alert::FORCE_IGNORED_LOCKED;
    }
    else if (moistureFault)
    {
      Serial.printf("[PUMP] force ignored, moisture sensor is faulted\n");
    }
    else
    {
      Serial.printf("[PUMP] force start via mqtt\n");
      moistureAtStart = moisture;
      pumpOn();
    }
  }
}


// wifi handler
void wifiConnect()
{
  if (WiFi.status() == WL_CONNECTED)
    return;
  Serial.printf("[WIFI] connecting to %s\n", STR(WIFI_SSID));
  WiFi.persistent(false); // dont write credentials to flash every boot
  WiFi.mode(WIFI_STA);
  WiFi.begin(STR(WIFI_SSID), STR(WIFI_PASSWORD));

  for (uint8_t i = 0; i < WIFI_MAX_RETRIES; i++)
  {
    esp_task_wdt_reset();
    delay(500);
    if (WiFi.status() == WL_CONNECTED)
    {
      Serial.printf("[WIFI] connected, ip: %s\n", WiFi.localIP().toString().c_str());
      return;
    }
    Serial.print('.');
  }
  Serial.println();
  Serial.printf("[WIFI] gave up, telemetry wont work this session\n");
}



bool mqttConnect()
{
  if (mqtt.connected())
    return true;
  if (WiFi.status() != WL_CONNECTED)
    return false;

  Serial.printf("[MQTT] connecting...\n");
  for (uint8_t i = 0; i < MQTT_MAX_RETRIES; i++)
  {
    esp_task_wdt_reset();
    int8_t ret = mqtt.connect();
    if (ret == 0)
    {
      Serial.printf("[MQTT] connected\n");
      return true;
    }
    Serial.printf("[MQTT] attempt %d failed: %s\n", i + 1, mqtt.connectErrorString(ret));
    mqtt.disconnect();
    delay(3000);
  }
  Serial.printf("[MQTT] all retries used up, will try again next cycle\n");
  return false;
}
void handleMqttRx()
{
  // nothing to read if we're not connected
  if (!mqtt.connected())
    return;

  Adafruit_MQTT_Subscribe *sub;
  // readSubscription(10) waits up to 10ms for an incoming message, returns null if nothing came in
  while ((sub = mqtt.readSubscription(10)) != nullptr)
  {
    // check which feed sent the message
    if (sub == &pumpControlSub)
    {
      // lastread is a raw byte buffer, cast it to a char* so String can read it
      String cmd(reinterpret_cast<char *>(pumpControlSub.lastread));
      // clean up whitespace and normalize to uppercase so "on", "On", "ON" all work
      cmd.trim();
      cmd.toUpperCase();

      if (cmd == "ON")
      {
        Serial.printf("[MQTT] got pump ON command\n");
        //set start pump
        forcePumpRequested = true;
      }
      else if (cmd == "OFF")
      {
        Serial.printf("[MQTT] got pump OFF command\n");
        // stop immediately if running
        if (pumpRunning)
          pumpOff("MQTT_COMMAND", PumpStop::NORMAL);
        forcePumpRequested = false;
      }
      else
      {
        Serial.printf("[MQTT] unknown pump command: %s\n", cmd.c_str());
      }
    }
  }
}

void publishTelemetry(float moisture, float lux)
{
  if (!mqtt.connected())
    return;

  // publish all sensor and state data to their respective adafruit io feeds
  // each publish() returns false if the message didnt get through
  if (!moisturePub.publish(moisture))
    Serial.printf("[MQTT] publish failed: moisture\n");

  // lux can be -1 when the sensor is missing, clamp to 0 so the dashboard doesnt break
  if (!lightPub.publish(lux < 0.0f ? 0.0f : lux))
    Serial.printf("[MQTT] publish failed: lux\n");

  // adafruit io expects numeric strings for toggle/indicator widgets
  if (!pumpStatePub.publish(pumpRunning ? "1.0" : "0.0"))
    Serial.printf("[MQTT] publish failed: pump state\n");

  if (!tankStatePub.publish(tankEmpty ? "1.0" : "0.0"))
    Serial.printf("[MQTT] publish failed: tank state\n");

  // total water used 
  if (!usedWaterPub.publish(totalWaterUsed_L))
    Serial.printf("[MQTT] publish failed: used water\n");

  // sends the current alert string, or "none" if everything is fine
  if (!alertPub.publish(pendingAlert))
    Serial.printf("[MQTT] publish failed: alert\n");

  // reset so we dont keep resending the same alert every 30s
  pendingAlert = Alert::NONE;
}

void runSelfTest()
{
  Serial.printf("[TEST] starting self test\n");

  // take 8 readings and average them to get a stable baseline for the check
  int32_t rawSum = 0;
  for (uint8_t i = 0; i < 8; i++)
    rawSum += analogRead(PIN_MOISTURE);
  int raw = (int)(rawSum / 8);

  // off limit values almost always mean sensor is unplugged or broken
  if (raw <= MOISTURE_FAULT_LOW || raw >= MOISTURE_FAULT_HIGH)
    Serial.printf("[TEST] moisture probe might be disconnected (raw=%d)\n", raw);
  else
    Serial.printf("[TEST] moisture probe ok (raw=%d)\n", raw);

  // lightSensorOk was set during Wire.begin() in setup, skip if it never responded
  if (lightSensorOk)
  {
    float lux = lightMeter.readLightLevel();
    if (lux < 0.0f)
      Serial.printf("[TEST] bh1750 read error\n");
    else
      Serial.printf("[TEST] bh1750 ok (%.1f lux)\n", lux);
  }
  else
  {
    Serial.printf("[TEST] bh1750 not found, skipping\n");
  }

  // pulse the relay so we can physically hear the click and confirm the pump wiring
  // 500ms is long enough to feel but short enough not to water anything
  Serial.printf("[TEST] pulsing relay...\n");
  digitalWrite(PIN_RELAY, LOW);
  delay(500);
  digitalWrite(PIN_RELAY, HIGH);
  Serial.printf("[TEST] relay done, pump should have twitched\n");

  Serial.printf("[TEST] done\n");
}


void setup()
{
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Plant Monitor Boot ===\n");

  // 30s is generous enough to survive wifi and mqtt init without tripping
  esp_task_wdt_init(30, true);
  esp_task_wdt_add(NULL);  // watch the main arduino task
  Serial.printf("[WDT] watchdog set to 30s\n");

  analogReadResolution(12);  // esp32 adc is 12-bit, 0-4095
  // gpio34 is input only, no pinmode needed
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, HIGH);  // active low relay, HIGH = pump off
  Serial.printf("[INIT] relay init, pump off\n");

  Wire.begin();  // sda=21 scl=22, esp32 defaults
  lightSensorOk = lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
  Serial.printf("[INIT] bh1750: %s\n", lightSensorOk ? "ok" : "not found");

  wifiConnect();
  esp_task_wdt_reset();  // wifi init can be slow, kick the watchdog after

  mqtt.subscribe(&pumpControlSub);  // register subscription before connecting
  mqttConnect();
  esp_task_wdt_reset();

  // fill the rolling average buffer before entering the loop
  // without this the first few readings would be averaged against zeros
  Serial.printf("[INIT] priming sensor buffer with %d samples...\n", AVG_WINDOW);
  for (uint8_t i = 0; i < AVG_WINDOW; i++)
  {
    sampleSensors();
    esp_task_wdt_reset();  // kick watchdog each iteration, this loop takes ~1s total
    delay(200);
  }

  runSelfTest();
  esp_task_wdt_reset();

  Serial.printf("[INIT] ready\n");
}

void loop()
{
  esp_task_wdt_reset();  // must be called regularly or the watchdog resets the board
  uint32_t now = millis();

  // reconnect if dropped, both functions are non-blocking on failure
  if (WiFi.status() != WL_CONNECTED)
    wifiConnect();
  if (!mqtt.connected())
    mqttConnect();

  // check for incoming pump commands from the Adafruit
  handleMqttRx();

  // push new readings into the rolling average buffer
  sampleSensors();
  float moisture = avgMoisture();
  float lux = avgLux();

  // run pump handler - decides whether to start/stop based on moisture
  handlePump();

  // print a snapshot of everything to serial every second
  Serial.printf(
      "[SENS] M:%.1f%%%s | Lux:%.0f | Pump:%s | Water:%.3fL | Tank:%s | Lock:%s\n",
      moisture,
      moistureFault ? "(FAULT)" : "",
      lux < 0.0f ? 0.0f : lux,
      pumpRunning ? "ON"    : "OFF",
      totalWaterUsed_L,
      tankEmpty  ? "EMPTY" : "OK",
      pumpLocked ? "YES"   : "NO");

  // publish to Adafruit io every 30s to avoid hitting rate limits
  if (now - lastTelemetryMs >= TELEMETRY_INTERVAL_MS)
  {
    publishTelemetry(moisture, lux);
    lastTelemetryMs = now;
  }

  delay(LOOP_DELAY_MS);
}