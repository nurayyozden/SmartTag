// the basic serial/hardware
//   sanity check. Runs an I2C bus  
//   scan on boot and reports any
//   devices found. Used to verify
//   the ESP32's serial link and
//   confirm the PN532 was visible
//   on the I2C bus before tackling
//   the NFC library.

#include <Arduino.h>
#include <Wire.h>

void setup() {
  Serial.begin(115200);
  Wire.begin();
  Serial.println("Scanning I2C bus...");

  int found = 0;
  for (byte addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  Device found at 0x%02X\n", addr);
      found++;
    }
  }

  if (found == 0)
    Serial.println("  No devices found.");
  else
    Serial.printf("  %d device(s) found.\n", found);
}

void loop() {}
