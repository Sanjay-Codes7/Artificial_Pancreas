/*
 * Artificial Pancreas - BENCH PROTOTYPE firmware (ESP32)
 * Team: Moorthy & Gang | Dexathon 2026 | Team ID DEX26319
 *
 *  ENDOCRINE side : glucose sensing -> closed-loop insulin micro-dosing
 *                   (syringe pump driven by a stepper motor)
 *  EXOCRINE side  : meal-triggered digestive-enzyme dosing
 *                   (peristaltic pump driven through a MOSFET)
 *
 * !!! SAFETY !!!
 * This is an educational / hackathon prototype. Use it on the bench with
 * coloured water only. NEVER connect it to a human or use real insulin.
 *
 * Libraries (Arduino Library Manager):
 *   - Adafruit ADS1X15
 *   - AccelStepper
 * Board: "ESP32 Dev Module"  (Arduino-ESP32 core 2.x or 3.x)
 */

#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Adafruit_ADS1X15.h>
#include <AccelStepper.h>
#include <math.h>

// =====================  USER CONFIGURATION  =====================
const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

// true  = simulated glucose (no sensor needed, ideal for demo / judging)
// false = read the real sensor through the ADS1115
#define SIM_MODE true

// ---- Pins (ESP32 DevKit v1) ----
const int PIN_STEP     = 26;   // A4988 STEP
const int PIN_DIR      = 27;   // A4988 DIR
const int PIN_EN       = 25;   // A4988 EN (active LOW)
const int PIN_ENZ_PUMP = 14;   // MOSFET gate -> peristaltic pump
const int PIN_ESTOP    = 33;   // E-stop button to GND (INPUT_PULLUP)
const int PIN_BUZZER   = 32;   // active buzzer
const int PIN_LED      = 2;    // on-board LED
// I2C (ADS1115): SDA = 21, SCL = 22

// ---- Sensor calibration (real mode): glucose = SLOPE * volts + OFFSET ----
// Do a 2-point calibration with 100 and 300 mg/dL glucose solutions.
float CAL_SLOPE  = 250.0;   // mg/dL per volt
float CAL_OFFSET = 20.0;    // mg/dL

// ---- Pump calibration ----
const float STEPS_PER_UNIT   = 400.0;  // stepper steps per 1 "unit" of fluid
const float ENZ_ML_PER_SEC   = 0.8;    // measure with a syringe & stopwatch
const float ENZ_ML_PER_G_FAT = 0.10;   // demo ratio: ml of enzyme fluid per g fat

// ---- Hard safety limits ----
const float MAX_AUTO_DOSE_U   = 0.5;   // largest automatic correction
const float MAX_MEAL_BOLUS_U  = 5.0;   // largest single meal bolus
const float MAX_DAILY_UNITS   = 20.0;  // daily cap (resets at power-up)
const float MAX_ENZYME_ML     = 5.0;   // largest single enzyme dose
const float LOW_BG            = 70.0;  // mg/dL -> suspend insulin
const float PREDICT_LOW_BG    = 80.0;  // predicted-30min threshold

// ---- Therapy settings (editable from the web page / doctor) ----
float targetBG = 110.0;  // mg/dL
float isf      = 50.0;   // mg/dL dropped by 1 unit
float icr      = 10.0;   // grams of carbs covered by 1 unit

// ---- Loop timing ----
#if SIM_MODE
const unsigned long CONTROL_INTERVAL_MS = 10000UL;   // 10 s (demo speed)
#else
const unsigned long CONTROL_INTERVAL_MS = 300000UL;  // 5 min (like a real CGM)
#endif
// ================================================================

Adafruit_ADS1115 ads;
AccelStepper insulinPump(AccelStepper::DRIVER, PIN_STEP, PIN_DIR);
WebServer server(80);

float glucose = 110.0, lastGlucose = 110.0, trendPerMin = 0.0, predicted30 = 110.0;
float iob = 0.0, dailyUnits = 0.0, totalEnzymeMl = 0.0;
bool  suspended = false, estop = false, lowAlarm = false, lowSuspended = false;
String lastEvent = "System ready";

unsigned long lastControl = 0, lastIobTick = 0, enzymeStopAt = 0;

// simulator state
float simPendingDrop = 0.0, simPendingRise = 0.0;

// ---------- helpers ----------
void beep(int ms) {
  digitalWrite(PIN_BUZZER, HIGH);
  delay(ms);
  digitalWrite(PIN_BUZZER, LOW);
}

void setEvent(const String& e) {
  lastEvent = e;
  Serial.println("[EVENT] " + e);
}

// ---------- glucose acquisition ----------
float readGlucose() {
#if SIM_MODE
  // Slow random walk + effect of insulin (drop) and carbs (rise)
  float step = (random(-30, 31)) / 10.0;       // -3.0 .. +3.0 mg/dL noise
  float applyDrop = simPendingDrop * 0.20;     // insulin acts over several cycles
  float applyRise = simPendingRise * 0.20;
  simPendingDrop -= applyDrop;
  simPendingRise -= applyRise;
  glucose = glucose + step - applyDrop + applyRise;
  glucose = constrain(glucose, 40.0, 400.0);
  return glucose;
#else
  float sum = 0;
  for (int i = 0; i < 5; i++) {               // 5-sample average
    int16_t raw = ads.readADC_SingleEnded(0);
    sum += ads.computeVolts(raw);
    delay(20);
  }
  float volts = sum / 5.0;
  return constrain(CAL_SLOPE * volts + CAL_OFFSET, 20.0, 500.0);
#endif
}

// ---------- insulin pump ----------
bool deliverInsulin(float units, const String& reason) {
  if (estop || suspended) return false;
  if (units <= 0.0) return false;
  if (dailyUnits + units > MAX_DAILY_UNITS) {
    setEvent("Daily insulin cap reached - dose refused");
    return false;
  }
  long steps = (long)(units * STEPS_PER_UNIT);
  digitalWrite(PIN_EN, LOW);                  // enable driver
  insulinPump.move(steps);
  iob += units;
  dailyUnits += units;
#if SIM_MODE
  simPendingDrop += units * isf;
#endif
  setEvent(String("Insulin ") + String(units, 2) + " U (" + reason + ")");
  return true;
}

// ---------- enzyme pump (exocrine function) ----------
bool deliverEnzyme(float fatGrams) {
  if (estop) return false;
  float ml = constrain(fatGrams * ENZ_ML_PER_G_FAT, 0.0, MAX_ENZYME_ML);
  if (ml <= 0.0) return false;
  unsigned long runMs = (unsigned long)((ml / ENZ_ML_PER_SEC) * 1000.0);
  digitalWrite(PIN_ENZ_PUMP, HIGH);
  enzymeStopAt = millis() + runMs;
  totalEnzymeMl += ml;
  setEvent(String("Enzyme ") + String(ml, 2) + " ml for " + String(fatGrams, 0) + " g fat");
  return true;
}

// ---------- safety ----------
void emergencyStop() {
  estop = true;
  insulinPump.setCurrentPosition(0);          // cancels any move
  digitalWrite(PIN_EN, HIGH);                 // disable stepper driver
  digitalWrite(PIN_ENZ_PUMP, LOW);
  setEvent("EMERGENCY STOP - reset the board to resume");
}

// ---------- control loop (runs every CONTROL_INTERVAL_MS) ----------
void controlStep() {
  lastGlucose = glucose;
  glucose = readGlucose();
  float minutes = CONTROL_INTERVAL_MS / 60000.0;
  trendPerMin = (glucose - lastGlucose) / minutes;
  predicted30 = glucose + trendPerMin * 30.0;

  lowAlarm = (glucose < LOW_BG) || (predicted30 < PREDICT_LOW_BG);
  if (lowAlarm) {
    suspended = true;
    lowSuspended = true;
    setEvent("LOW GLUCOSE SUSPEND - treat with 15 g fast carbs");
    beep(300);
    return;
  }
  if (lowSuspended && !lowAlarm && glucose > LOW_BG + 20.0) {
    suspended = false;                         // auto-resume after a low-glucose suspend
    lowSuspended = false;
    setEvent("Glucose recovered - insulin resumed");
  }
  if (suspended || estop) return;

  // Automatic correction only when high AND not already falling
  if (glucose > targetBG + 30.0 && trendPerMin >= 0.0) {
    float dose = (glucose - targetBG) / isf - iob;
    dose = constrain(dose, 0.0, MAX_AUTO_DOSE_U);
    if (dose >= 0.05) deliverInsulin(dose, "auto correction");
  }
}

// ---------- IOB decay (half-life ~75 min) ----------
void tickIob() {
  unsigned long now = millis();
  float dtSec = (now - lastIobTick) / 1000.0;
  lastIobTick = now;
#if SIM_MODE
  dtSec *= 30.0;   // run insulin physiology 30x faster in demo mode
#endif
  iob *= pow(0.5, dtSec / 4500.0);
  if (iob < 0.01) iob = 0.0;
}

// ---------- web API ----------
void sendJson(const String& body) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", body);
}

void handleStatus() {
  String j = "{";
  j += "\"glucose\":" + String(glucose, 1);
  j += ",\"trend\":" + String(trendPerMin, 2);
  j += ",\"predicted30\":" + String(predicted30, 1);
  j += ",\"iob\":" + String(iob, 2);
  j += ",\"dailyUnits\":" + String(dailyUnits, 2);
  j += ",\"enzymeMl\":" + String(totalEnzymeMl, 2);
  j += ",\"suspended\":" + String(suspended ? "true" : "false");
  j += ",\"estop\":" + String(estop ? "true" : "false");
  j += ",\"target\":" + String(targetBG, 0);
  j += ",\"isf\":" + String(isf, 0);
  j += ",\"icr\":" + String(icr, 1);
  j += ",\"sim\":" + String(SIM_MODE ? "true" : "false");
  j += ",\"event\":\"" + lastEvent + "\"}";
  sendJson(j);
}

// /meal?carbs=45&fat=10
void handleMeal() {
  float carbs = server.arg("carbs").toFloat();
  float fat   = server.arg("fat").toFloat();
  if (estop || suspended) {
    sendJson("{\"ok\":false,\"msg\":\"Delivery blocked (suspended or e-stop)\"}");
    return;
  }
  float mealDose = carbs / icr;
  float corr = (glucose > targetBG) ? (glucose - targetBG) / isf : 0.0;
  float dose = constrain(mealDose + corr - iob, 0.0, MAX_MEAL_BOLUS_U);
  bool okI = (dose > 0.0) ? deliverInsulin(dose, "meal bolus") : false;
  bool okE = (fat > 0.0) ? deliverEnzyme(fat) : false;
#if SIM_MODE
  simPendingRise += carbs * (isf / icr);       // carbs raise glucose
#endif
  sendJson(String("{\"ok\":true,\"insulinU\":") + String(okI ? dose : 0.0, 2) +
           ",\"enzymeDone\":" + String(okE ? "true" : "false") + "}");
}

// /suspend?on=1   or   /suspend?on=0
void handleSuspend() {
  suspended = server.arg("on") == "1";
  lowSuspended = false;                        // manual choice overrides auto-resume
  setEvent(suspended ? "Insulin suspended by user" : "Insulin resumed by user");
  sendJson("{\"ok\":true}");
}

// /settings?target=110&isf=50&icr=10
void handleSettings() {
  if (server.hasArg("target")) targetBG = constrain(server.arg("target").toFloat(), 80, 180);
  if (server.hasArg("isf"))    isf      = constrain(server.arg("isf").toFloat(), 10, 200);
  if (server.hasArg("icr"))    icr      = constrain(server.arg("icr").toFloat(), 3, 50);
  setEvent("Settings updated by doctor/patient");
  sendJson("{\"ok\":true}");
}

// ---------- setup / loop ----------
void setup() {
  Serial.begin(115200);
  pinMode(PIN_EN, OUTPUT);       digitalWrite(PIN_EN, HIGH);   // driver off
  pinMode(PIN_ENZ_PUMP, OUTPUT); digitalWrite(PIN_ENZ_PUMP, LOW);
  pinMode(PIN_BUZZER, OUTPUT);   digitalWrite(PIN_BUZZER, LOW);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_ESTOP, INPUT_PULLUP);

  insulinPump.setMaxSpeed(600);
  insulinPump.setAcceleration(300);

#if !SIM_MODE
  Wire.begin(21, 22);
  if (!ads.begin()) {
    Serial.println("ADS1115 not found - check wiring!");
    while (true) { beep(100); delay(400); }
  }
  ads.setGain(GAIN_ONE);         // +/-4.096 V range
#endif

  randomSeed(esp_random());
  glucose = lastGlucose = predicted30 = 110.0;

  // Wi-Fi: try your network, otherwise open a hotspot
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) { delay(300); Serial.print("."); }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected. Open the web page and enter IP: " + WiFi.localIP().toString());
  } else {
    WiFi.mode(WIFI_AP);
    WiFi.softAP("ArtificialPancreas", "12345678");
    Serial.println("\nHotspot 'ArtificialPancreas' (pw 12345678). IP: " + WiFi.softAPIP().toString());
  }

  server.on("/status",   handleStatus);
  server.on("/meal",     handleMeal);
  server.on("/suspend",  handleSuspend);
  server.on("/settings", handleSettings);
  server.begin();

  lastIobTick = millis();
  lastControl = millis() - CONTROL_INTERVAL_MS;   // run first cycle immediately
  setEvent(SIM_MODE ? "Started in SIMULATION mode" : "Started with real sensor");
}

void loop() {
  server.handleClient();

  if (!estop && digitalRead(PIN_ESTOP) == LOW) emergencyStop();

  if (!estop) {
    insulinPump.run();                                   // non-blocking stepping
    if (insulinPump.distanceToGo() == 0) digitalWrite(PIN_EN, HIGH);  // idle: save power
    if (enzymeStopAt && millis() >= enzymeStopAt) {
      digitalWrite(PIN_ENZ_PUMP, LOW);
      enzymeStopAt = 0;
    }
    if (millis() - lastControl >= CONTROL_INTERVAL_MS) {
      lastControl = millis();
      controlStep();
    }
    tickIob();
  }

  digitalWrite(PIN_LED, estop ? (millis() / 150) % 2 : (suspended ? (millis() / 600) % 2 : HIGH));
}
