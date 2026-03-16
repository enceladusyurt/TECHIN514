#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLERemoteCharacteristic.h>

// ===== 电机引脚 =====
#define AIN1 D0
#define AIN2 D1
#define BIN1 D2
#define BIN2 D3

// ===== 显示灯 =====
#define LED_PIN D7   // GPIO20

// ===== BLE UUID =====
static BLEUUID serviceUUID("ff87d8a1-889f-4068-8cc1-1a8efa971645");
static BLEUUID charUUID("491fe531-6c89-4539-a0d4-bcbe2a419ee5");

// ===== X27 参数 =====
const float STEPS_PER_DEGREE = 2.1;   // 可按实测调整
const float HOME_ANGLE = -180.0;      // 机械初始/home位置（你手动放到这里）
int currentStep = 0;
float currentAngle = HOME_ANGLE;      // 程序默认当前就在 -180°

// 全步驱动序列（双极）
int stepSequence[4][4] = {
  {1, 0, 1, 0},
  {0, 1, 1, 0},
  {0, 1, 0, 1},
  {1, 0, 0, 1}
};

// ===== BLE 相关 =====
static BLEAddress* pServerAddress = nullptr;
static BLERemoteCharacteristic* pRemoteCharacteristic = nullptr;
static bool doConnect = false;
static bool connected = false;
static bool doScan = false;

void setStep(int stepIndex) {
  digitalWrite(AIN1, stepSequence[stepIndex][0]);
  digitalWrite(AIN2, stepSequence[stepIndex][1]);
  digitalWrite(BIN1, stepSequence[stepIndex][2]);
  digitalWrite(BIN2, stepSequence[stepIndex][3]);
}

void releaseMotor() {
  digitalWrite(AIN1, LOW);
  digitalWrite(AIN2, LOW);
  digitalWrite(BIN1, LOW);
  digitalWrite(BIN2, LOW);
}

void stepMotor(int steps, int direction) {
  for (int i = 0; i < steps; i++) {
    currentStep += direction;

    if (currentStep > 3) currentStep = 0;
    if (currentStep < 0) currentStep = 3;

    setStep(currentStep);
    delayMicroseconds(4000);   // 带指针时稳一点
  }
}

void rotateToAngle(float targetAngle) {
  float delta = targetAngle - currentAngle;

  if (delta == 0) return;

  int direction = (delta > 0) ? -1 : 1;
  int steps = (int)(abs(delta) * STEPS_PER_DEGREE + 0.5);

  stepMotor(steps, direction);
  currentAngle = targetAngle;

  releaseMotor();
}

// 返回表盘显示角度：-60° ~ +60°
float hrToAngle(int hr) {
  if (hr <= 45) return -60.0;
  if (hr >= 120) return 60.0;

  if (hr < 70) {
    // 45 -> -60, 70 -> 0
    return -60.0 + (hr - 45) * (60.0 / 25.0);
  } else {
    // 70 -> 0, 120 -> 60
    return (hr - 70) * (60.0 / 50.0);
  }
}

void handleHR(int hr) {
  Serial.print("Received HR = ");
  Serial.println(hr);

  float targetAngle = hrToAngle(hr);   // 显示区间仍然是 -60° ~ +60°

  Serial.print("Target angle = ");
  Serial.println(targetAngle);

  digitalWrite(LED_PIN, HIGH);

  // 从当前的 -180° home 转到目标显示角度
  rotateToAngle(targetAngle);

  // 停留 5 秒显示
  delay(5000);

  digitalWrite(LED_PIN, LOW);

  // 回到 -180° home
  rotateToAngle(HOME_ANGLE);
}

static void notifyCallback(
  BLERemoteCharacteristic* pBLERemoteCharacteristic,
  uint8_t* pData,
  size_t length,
  bool isNotify) {

  String msg = "";
  for (size_t i = 0; i < length; i++) {
    msg += (char)pData[i];
  }

  int hr = msg.toInt();
  handleHR(hr);
}

class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) override {
    Serial.println("Connected to sensing device.");
  }

  void onDisconnect(BLEClient* pclient) override {
    connected = false;
    Serial.println("Disconnected from sensing device.");
    doScan = true;
  }
};

bool connectToServer() {
  BLEClient* pClient = BLEDevice::createClient();
  pClient->setClientCallbacks(new MyClientCallback());

  if (!pClient->connect(*pServerAddress)) {
    Serial.println("Failed to connect.");
    return false;
  }

  BLERemoteService* pRemoteService = pClient->getService(serviceUUID);
  if (pRemoteService == nullptr) {
    Serial.println("Failed to find service UUID.");
    pClient->disconnect();
    return false;
  }

  pRemoteCharacteristic = pRemoteService->getCharacteristic(charUUID);
  if (pRemoteCharacteristic == nullptr) {
    Serial.println("Failed to find characteristic UUID.");
    pClient->disconnect();
    return false;
  }

  if (pRemoteCharacteristic->canNotify()) {
    pRemoteCharacteristic->registerForNotify(notifyCallback);
  }

  connected = true;
  Serial.println("BLE ready, waiting for HR notify...");
  return true;
}

class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    if (advertisedDevice.haveServiceUUID() &&
        advertisedDevice.isAdvertisingService(serviceUUID)) {
      Serial.println("Found sensing device.");
      BLEDevice::getScan()->stop();
      pServerAddress = new BLEAddress(advertisedDevice.getAddress());
      doConnect = true;
      doScan = false;
    }
  }
};

void setupBLE() {
  BLEDevice::init("DisplayDevice");
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);
  pBLEScan->start(5, false);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== DISPLAY DEVICE ===");

  pinMode(AIN1, OUTPUT);
  pinMode(AIN2, OUTPUT);
  pinMode(BIN1, OUTPUT);
  pinMode(BIN2, OUTPUT);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  releaseMotor();

  setupBLE();

  Serial.println("Display device started. Scanning for sensing device...");
  Serial.print("Home angle = ");
  Serial.println(HOME_ANGLE);
}

void loop() {
  if (doConnect) {
    if (connectToServer()) {
      Serial.println("Connected and subscribed.");
    } else {
      Serial.println("Connect failed, scanning again...");
      doScan = true;
    }
    doConnect = false;
  }

  if (doScan) {
    BLEDevice::getScan()->start(5, false);
  }

  delay(100);
}