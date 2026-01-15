#include <Arduino.h>

const int analogPin = A0;

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Starting ADC read...");
}

void loop() {
  int adcValue = analogRead(analogPin);
  float voltage = (adcValue / 4095.0) * 3.3;

  Serial.print("ADC: ");
  Serial.print(adcValue);
  Serial.print("   Voltage: ");
  Serial.print(voltage);
  Serial.println(" V");

  delay(1000);
}
