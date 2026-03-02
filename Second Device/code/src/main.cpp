
#include <Arduino.h>
#include <PulseSensorPlayground.h>

// ======================= USER CONFIG =======================

// -------- PPG input pin (XIAO ESP32-C3) --------
static const int PULSE_INPUT_PIN = A0;  // A0 == GPIO2 on XIAO ESP32-C3

// -------- Start mode selection --------
// Choose ONE primary mode by setting these flags.
// MODE_TOUCH_START: use capacitive touch to start/stop sensing
// MODE_AUTO_START: automatically start when PPG signal looks "present/stable"
static const bool MODE_TOUCH_START = false;  // set true if your touchRead() works on your setup
static const bool MODE_AUTO_START  = true;   // recommended if touch isn't available on ESP32-C3 core

// -------- Touch settings (only used if MODE_TOUCH_START==true) --------
static const int TOUCH_PIN = 4;  // try GPIO4 if your core supports touchRead on C3; otherwise disable MODE_TOUCH_START
static const int TOUCH_SAMPLES_FOR_BASELINE = 30;
static const int TOUCH_DROP_THRESHOLD = 15;          // lower-than-baseline by this amount => touched
static const unsigned long TOUCH_DEBOUNCE_MS = 200;

// -------- PulseSensor settings --------
static const int PULSE_THRESHOLD = 550;  // tune 520~650 depending on sensor + finger placement

// -------- Auto-start settings (only used if MODE_AUTO_START==true) --------
// We treat "PPG present" as: analog signal has enough variation over a short window
static const unsigned long AUTO_WINDOW_MS = 800;
static const int AUTO_SAMPLE_INTERVAL_MS = 10;
static const int AUTO_VARIATION_THRESHOLD = 18;   // tune: larger => harder to start; smaller => more false starts
static const int AUTO_START_HOLD_WINDOWS = 2;     // require N consecutive windows with variation
static const int AUTO_STOP_HOLD_WINDOWS  = 3;     // require N consecutive windows without variation to stop

// -------- Stress calculation (HRV proxy) --------
static const int IBI_BUF = 12;  // store last ~12 beats
// ============================================================

PulseSensorPlayground pulseSensor;

// -------- Device state --------
bool sensingActive = false;

// -------- Touch baseline --------
int touchBaseline = 0;
unsigned long lastTouchCheckMs = 0;

// -------- Auto-start stats --------
unsigned long autoWindowStartMs = 0;
int autoMinV = 4095;
int autoMaxV = 0;
int autoGoodWindows = 0;
int autoBadWindows  = 0;
unsigned long lastAutoSampleMs = 0;

// -------- IBI buffer for RMSSD --------
int ibiBuf[IBI_BUF];
int ibiCount = 0;
int ibiIndex = 0;

void resetIBIBuffer() {
  ibiCount = 0;
  ibiIndex = 0;
  for (int i = 0; i < IBI_BUF; i++) ibiBuf[i] = 0;
}

void addIBI(int ibiMs) {
  ibiBuf[ibiIndex] = ibiMs;
  ibiIndex = (ibiIndex + 1) % IBI_BUF;
  if (ibiCount < IBI_BUF) ibiCount++;
}

float computeRMSSDms() {
  // RMSSD = sqrt(mean(diff(IBI)^2)) over buffer
  if (ibiCount < 6) return -1.0f;

  int n = (ibiCount < IBI_BUF) ? ibiCount : IBI_BUF;
  float sumSq = 0.0f;
  int pairs = 0;

  // use newest values based on ibiIndex (points to next write position)
  for (int i = 1; i < n; i++) {
    int a = ibiBuf[(ibiIndex - i + IBI_BUF) % IBI_BUF];
    int b = ibiBuf[(ibiIndex - i - 1 + IBI_BUF) % IBI_BUF];
    int d = a - b;
    sumSq += (float)(d * d);
    pairs++;
  }

  if (pairs <= 0) return -1.0f;
  return sqrt(sumSq / (float)pairs);
}

int mapStressIndex(float rmssd_ms, int bpm) {
  // Heuristic mapping for prototyping:
  // Lower RMSSD => higher stress. Clamp RMSSD to [10, 60] ms.
  if (rmssd_ms < 0) return -1;

  float rmssdClamped = constrain(rmssd_ms, 10.0f, 60.0f);
  float stress = 90.0f - (rmssdClamped - 10.0f) * (70.0f / 50.0f);  // 10->90, 60->20

  // small BPM influence
  if (bpm > 100) stress += 8;
  else if (bpm > 90) stress += 4;
  else if (bpm < 55) stress += 2;

  stress = constrain(stress, 0.0f, 100.0f);
  return (int)lround(stress);
}

void setSensingActive(bool on) {
  if (on == sensingActive) return;
  sensingActive = on;

  if (sensingActive) {
    resetIBIBuffer();
    Serial.println("\n[STATE] START sensing.");
  } else {
    Serial.println("\n[STATE] STOP sensing.");
  }
}

// ----------------- Touch start logic -----------------
void initTouchBaselineIfNeeded() {
#if defined(ESP32)
  // touchRead exists on ESP32 Arduino core, but on ESP32-C3 it may or may not be supported depending on version.
  // We'll attempt baseline only if MODE_TOUCH_START.
  if (!MODE_TOUCH_START) return;

  long sum = 0;
  for (int i = 0; i < TOUCH_SAMPLES_FOR_BASELINE; i++) {
    sum += touchRead(TOUCH_PIN);
    delay(10);
  }
  touchBaseline = (int)(sum / TOUCH_SAMPLES_FOR_BASELINE);
  Serial.print("[INIT] touchBaseline=");
  Serial.println(touchBaseline);
#endif
}

void updateTouchStart() {
  if (!MODE_TOUCH_START) return;

#if defined(ESP32)
  unsigned long now = millis();
  if (now - lastTouchCheckMs < TOUCH_DEBOUNCE_MS) return;
  lastTouchCheckMs = now;

  int t = touchRead(TOUCH_PIN);
  bool touched = (t < (touchBaseline - TOUCH_DROP_THRESHOLD));

  if (touched && !sensingActive) setSensingActive(true);
  if (!touched && sensingActive) setSensingActive(false);
#else
  // If not ESP32, do nothing
#endif
}

// ----------------- Auto start logic -----------------
void resetAutoWindow(unsigned long now) {
  autoWindowStartMs = now;
  autoMinV = 4095;
  autoMaxV = 0;
}

void updateAutoStart() {
  if (!MODE_AUTO_START) return;

  unsigned long now = millis();

  // sample analog every AUTO_SAMPLE_INTERVAL_MS
  if (now - lastAutoSampleMs >= AUTO_SAMPLE_INTERVAL_MS) {
    lastAutoSampleMs = now;
    int v = analogRead(PULSE_INPUT_PIN);
    if (v < autoMinV) autoMinV = v;
    if (v > autoMaxV) autoMaxV = v;
  }

  // window evaluation
  if (now - autoWindowStartMs >= AUTO_WINDOW_MS) {
    int variation = autoMaxV - autoMinV;

    bool good = (variation >= AUTO_VARIATION_THRESHOLD);
    if (good) {
      autoGoodWindows++;
      autoBadWindows = 0;
    } else {
      autoBadWindows++;
      autoGoodWindows = 0;
    }

    // start/stop decision
    if (!sensingActive && autoGoodWindows >= AUTO_START_HOLD_WINDOWS) {
      Serial.print("[AUTO] variation=");
      Serial.print(variation);
      Serial.println(" -> signal present, starting.");
      setSensingActive(true);
      autoGoodWindows = 0;
    }

    if (sensingActive && autoBadWindows >= AUTO_STOP_HOLD_WINDOWS) {
      Serial.print("[AUTO] variation=");
      Serial.print(variation);
      Serial.println(" -> signal absent, stopping.");
      setSensingActive(false);
      autoBadWindows = 0;
    }

    resetAutoWindow(now);
  }
}

// ----------------- Setup & Loop -----------------
void setup() {
  Serial.begin(115200);
  delay(300);

  // ADC setup note: ESP32 uses 12-bit by default. analogRead returns 0..4095.
  analogReadResolution(12);

  // Pulse Sensor setup
  pulseSensor.analogInput(PULSE_INPUT_PIN);
  pulseSensor.setSerial(Serial);
  pulseSensor.setThreshold(PULSE_THRESHOLD);

  if (pulseSensor.begin()) {
    Serial.println("[INIT] PulseSensor begin() OK");
  } else {
    Serial.println("[INIT] PulseSensor begin() FAILED (check wiring/pin/library)");
  }

  // init start logic
  if (MODE_TOUCH_START) {
    Serial.println("[MODE] Touch-to-start enabled.");
    initTouchBaselineIfNeeded();
  }
  if (MODE_AUTO_START) {
    Serial.println("[MODE] Auto-start enabled (based on PPG variation).");
    resetAutoWindow(millis());
  }

  Serial.println("\nWiring (XIAO ESP32-C3):");
  Serial.println("- Pulse VCC->3V3, GND->GND, SIG->A0(GPIO2)");
  Serial.println("\nNotes:");
  Serial.println("- If touch mode doesn't work on your ESP32-C3 core, set MODE_TOUCH_START=false and keep MODE_AUTO_START=true.");
  Serial.println("- StressIndex is a prototyping heuristic (non-medical).");
}

void loop() {
  // Update start/stop conditions
  updateTouchStart();
  updateAutoStart();

  if (!sensingActive) {
    delay(10);
    return;
  }

  // PulseSensor sampling
  int bpm = pulseSensor.getBeatsPerMinute();

  if (pulseSensor.sawStartOfBeat()) {
    int ibi = pulseSensor.getInterBeatIntervalMs(); // ms
    addIBI(ibi);

    float rmssd = computeRMSSDms();
    int stress = mapStressIndex(rmssd, bpm);

    Serial.print("[BEAT] BPM=");
    Serial.print(bpm);
    Serial.print("  IBI=");
    Serial.print(ibi);
    Serial.print("ms  RMSSD~");
    Serial.print(rmssd, 1);
    Serial.print("ms  StressIndex=");
    Serial.println(stress);

    // TODO: send stress to Focus Drift Dial (BLE notify / Serial / ESP-NOW)
    // sendStress(stress);
  }

  delay(5);
}