#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

const int PULSE_PIN = 2;      // Pulse Sensor
const int LED_PIN = 8;        // 每次检测到心跳闪一下
const int BUTTON_PIN = 20;    // Rotary button SW

int threshold = 800;

// ===== BLE UUID =====
#define SERVICE_UUID        "ff87d8a1-889f-4068-8cc1-1a8efa971645"
#define CHARACTERISTIC_UUID "491fe531-6c89-4539-a0d4-bcbe2a419ee5"

BLECharacteristic* pCharacteristic;
bool deviceConnected = false;

// ===== 心率检测 =====
bool pulseDetected = false;
unsigned long lastBeatTime = 0;
int bpm = 0;

// ===== 40秒测量 =====
const unsigned long RECORD_DURATION = 40000;   // 40 s
const unsigned long SAMPLE_INTERVAL = 5;       // 5 ms
unsigned long startTime = 0;
unsigned long lastSampleTime = 0;

bool measuring = false;
bool finished = false;

// ===== 平均 HR 统计 =====
unsigned long rrSum = 0;     // 所有有效 RR interval 总和
int rrCount = 0;             // 有效 RR interval 个数
int finalHR = 0;

// ===== 按钮防抖 =====
bool lastButtonReading = HIGH;
bool buttonState = HIGH;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50;

class MyServerCallbacks: public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) override {
    deviceConnected = true;
    Serial.println("BLE client connected.");
  }

  void onDisconnect(BLEServer* pServer) override {
    deviceConnected = false;
    Serial.println("BLE client disconnected.");
    BLEDevice::startAdvertising();
  }
};

void startMeasurement() {
  pulseDetected = false;
  lastBeatTime = 0;
  bpm = 0;
  finalHR = 0;
  rrSum = 0;
  rrCount = 0;

  startTime = millis();
  lastSampleTime = 0;

  measuring = true;
  finished = false;

  Serial.println("Start measuring for 40 seconds...");
}

void sendHRByBLE(int hr) {
  String hrString = String(hr);
  pCharacteristic->setValue(hrString.c_str());
  pCharacteristic->notify();

  Serial.print("BLE sent HR: ");
  Serial.println(hr);
}

void setupBLE() {
  BLEDevice::init("SensingDevice");
  BLEServer* pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService* pService = pServer->createService(SERVICE_UUID);

  pCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ |
    BLECharacteristic::PROPERTY_NOTIFY
  );

  pCharacteristic->addDescriptor(new BLE2902());
  pCharacteristic->setValue("0");

  pService->start();

  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->start();

  Serial.println("BLE advertising started.");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== SENSING DEVICE ===");
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  setupBLE();

  Serial.println("Press rotary button to start measurement.");
}

void loop() {
  // ===== 按钮检测 =====
  bool reading = digitalRead(BUTTON_PIN);

  if (reading != lastButtonReading) {
    lastDebounceTime = millis();
  }

  if ((millis() - lastDebounceTime) > debounceDelay) {
    if (reading != buttonState) {
      buttonState = reading;

      // 按下：HIGH -> LOW
      if (buttonState == LOW) {
        if (!measuring) {
          startMeasurement();
        }
      }
    }
  }

  lastButtonReading = reading;

  if (!measuring) {
    return;
  }

  unsigned long currentTime = millis();

  if (currentTime - lastSampleTime < SAMPLE_INTERVAL) {
    return;
  }
  lastSampleTime = currentTime;

  // ===== 40秒结束 =====
  if (currentTime - startTime >= RECORD_DURATION) {
    measuring = false;
    finished = true;

    if (rrCount > 0) {
      unsigned long avgRR = rrSum / rrCount;
      finalHR = 60000 / avgRR;
    } else {
      finalHR = 0;
    }

    Serial.print("Measurement finished. Final HR = ");
    Serial.println(finalHR);

    if (deviceConnected) {
      sendHRByBLE(finalHR);
    } else {
      Serial.println("No BLE client connected, HR not sent.");
    }

    Serial.println("Press rotary button to start again.");
    return;
  }

  int signal = analogRead(PULSE_PIN);

  // 上升穿越阈值 = 检测到一次 beat
  if (signal > threshold && !pulseDetected) {
    pulseDetected = true;

    if (lastBeatTime > 0) {
      unsigned long interval = currentTime - lastBeatTime;

      if (interval > 300 && interval < 2000) {
        bpm = 60000 / interval;

        rrSum += interval;
        rrCount++;

        Serial.print("Instant BPM: ");
        Serial.println(bpm);

        // 每次心跳闪灯
        digitalWrite(LED_PIN, HIGH);
        delay(20);
        digitalWrite(LED_PIN, LOW);
      }
    }

    lastBeatTime = currentTime;
  }

  if (signal < threshold) {
    pulseDetected = false;
  }
}