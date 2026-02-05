#define ENABLE_USER_AUTH
#define ENABLE_DATABASE

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <FirebaseClient.h>
#include "secrets.h"
#include "esp_sleep.h"

static const int TRIG_PIN = 3;
static const int ECHO_PIN = 4;

static const float MOTION_THRESHOLD_CM = 10.0f;

static const uint32_t WORK_MS = 5000;
static const uint32_t SLEEP_LONG_S = 55;
static const uint32_t SLEEP_SHORT_S = 10;

static const uint32_t ULTRA_SAMPLE_COUNT = 5;
static const uint32_t ULTRA_SAMPLE_DELAY_MS = 60;

RTC_DATA_ATTR float last_cm = -1.0f;
RTC_DATA_ATTR uint32_t no_motion_cycles = 0;

UserAuth user_auth(FIREBASE_API_KEY, FIREBASE_USER_EMAIL, FIREBASE_USER_PASSWORD);
FirebaseApp app;

WiFiClientSecure ssl;
using AsyncClient = AsyncClientClass;
AsyncClient async_client(ssl);
RealtimeDatabase Database;

static void wifiOff() {
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
}

static bool wifiConnect(uint32_t timeoutMs = 15000) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
    delay(250);
  }
  return (WiFi.status() == WL_CONNECTED);
}

static void firebaseInit() {
  ssl.setInsecure();
  initializeApp(async_client, app, getAuth(user_auth));
  app.getApp<RealtimeDatabase>(Database);
  Database.url(FIREBASE_RTDB_URL);
}

static float readUltrasonicOnceCM() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  unsigned long duration = pulseIn(ECHO_PIN, HIGH, 30000UL);
  if (duration == 0) return 0.0f;
  return (float)duration / 58.0f;
}

static float readUltrasonicFilteredCM() {
  float vals[ULTRA_SAMPLE_COUNT];
  for (uint32_t i = 0; i < ULTRA_SAMPLE_COUNT; i++) {
    vals[i] = readUltrasonicOnceCM();
    delay(ULTRA_SAMPLE_DELAY_MS);
  }
  for (uint32_t i = 0; i < ULTRA_SAMPLE_COUNT; i++) {
    for (uint32_t j = i + 1; j < ULTRA_SAMPLE_COUNT; j++) {
      if (vals[j] < vals[i]) {
        float tmp = vals[i];
        vals[i] = vals[j];
        vals[j] = tmp;
      }
    }
  }
  return vals[ULTRA_SAMPLE_COUNT / 2];
}

static void goDeepSleepSeconds(uint32_t seconds) {
  wifiOff();
  esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
  delay(50);
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  uint32_t t0 = millis();
  float cm = readUltrasonicFilteredCM();
  uint32_t workElapsed = millis() - t0;

  bool motion = false;
  if (last_cm < 0.0f) {
    motion = true;
  } else {
    motion = fabsf(cm - last_cm) >= MOTION_THRESHOLD_CM;
  }
  last_cm = cm;

  if (motion) {
    no_motion_cycles = 0;

    bool ok = wifiConnect();
    if (ok) {
      firebaseInit();
      uint32_t ts = (uint32_t)(esp_timer_get_time() / 1000000ULL);

      app.loop();
      Database.set<float>(async_client, "/lab8/distance_cm", cm);
      app.loop();
      Database.set<uint32_t>(async_client, "/lab8/timestamp_s", ts);
      app.loop();
      Database.set<bool>(async_client, "/lab8/motion", true);

      for (int i = 0; i < 20; i++) {
        app.loop();
        delay(50);
      }
    }
  } else {
    no_motion_cycles++;
  }

  if (workElapsed < WORK_MS) {
    delay(WORK_MS - workElapsed);
  }

  uint32_t nextSleep = SLEEP_LONG_S;
  if (motion) {
    nextSleep = SLEEP_SHORT_S;
  }

  goDeepSleepSeconds(nextSleep);
}

void loop() {
}
