#include <Arduino.h>
#include <NimBLEDevice.h>

// ====== BLE UUIDs ======
static const char* SERVICE_UUID = "67702f32-0192-4fd7-8711-8df368d1274b";
static const char* CHAR_UUID    = "ca419fce-b785-4809-80ad-bdce5b41b7fd";
static const char* DEVICE_NAME  = "Fiona";

// ====== HC-SR04 Pins ======
static const int TRIG_PIN = 7;
static const int ECHO_PIN = 6;

// ====== DSP: Moving Average ======
static const int WIN = 5;       
float buf[WIN] = {0};
int idx = 0;
int filled = 0;

static NimBLECharacteristic* pChar = nullptr;
static bool g_connected = false;

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo&) override {
    g_connected = true;
    Serial.println("[Server] Client connected!");
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int reason) override {
    g_connected = false;
    Serial.print("[Server] Client disconnected, reason=");
    Serial.println(reason);
    NimBLEDevice::startAdvertising();
  }
};

float readDistanceCM() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  unsigned long dur = pulseIn(ECHO_PIN, HIGH, 30000);
  if (dur == 0) return NAN;
  return dur / 58.0f;
}

float movingAverage(float x) {
  buf[idx] = x;
  idx = (idx + 1) % WIN;
  if (filled < WIN) filled++;

  float sum = 0;
  for (int i = 0; i < filled; i++) sum += buf[i];
  return sum / filled;
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("[Server] Boot");

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  // BLE init
  NimBLEDevice::init(DEVICE_NAME);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  NimBLEService* service = server->createService(SERVICE_UUID);
  pChar = service->createCharacteristic(
    CHAR_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
  );
  pChar->setValue("ready");
  service->start();

  // NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  // adv->addServiceUUID(SERVICE_UUID);
  // adv->start();
  // ====== BLE Advertising ======
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();

  NimBLEAdvertisementData ad;
  ad.setName(DEVICE_NAME);              
  ad.addServiceUUID(SERVICE_UUID);      

  adv->setAdvertisementData(ad);
  adv->start();


  Serial.print("[Server] Advertising as: ");
  Serial.println(DEVICE_NAME);
}

void loop() {
  float raw = readDistanceCM();
  // if (isnan(raw)) { // || raw <= 0 || raw > 500
  //   Serial.println("Raw: --  | Denoised: --  | (invalid)");
  //   delay(100);
  //   return;
  // }

  float den = movingAverage(raw);

  // raw + denoised
  Serial.print("Raw: ");
  Serial.print(raw, 2);
  Serial.print(" cm | Denoised(MA");
  Serial.print(WIN);
  Serial.print("): ");
  Serial.print(den, 2);
  Serial.print(" cm");

  if (g_connected && den < 30.0f) {
    char msg[16];
    snprintf(msg, sizeof(msg), "%.2f", den);
    pChar->setValue((uint8_t*)msg, strlen(msg));
    pChar->notify();
    Serial.print("  --> BLE notify SENT");
  } else {
    Serial.print("  --> (no send)");
  }

  Serial.println();
  delay(100); // 10Hz
}
