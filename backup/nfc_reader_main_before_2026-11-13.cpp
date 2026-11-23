// Initialises the PN532 over I2C,
//    waits for a tag tap, prints   
//   its UID, and moves an MG996R   
//   servo to a random angle. 


#include <Arduino.h>
#include <Wire.h>
#include <PN532_I2C.h>
#include <PN532.h>
#include <ESP32Servo.h>

#define SERVO_PIN 13 

// #define RSTPDN_PIN X  // connect PN532 RSTPDN to a free GPIO, then:
//   pinMode(RSTPDN_PIN, OUTPUT);
//   digitalWrite(RSTPDN_PIN, LOW);  delay(100);
//   digitalWrite(RSTPDN_PIN, HIGH); delay(500);
// before nfc.begin() to replace the 3s delay

PN532_I2C pn532i2c(Wire);
PN532 nfc(pn532i2c);
Servo servo;

void initNFC() {
  Wire.begin();
  Wire.setClock(100000);
  delay(3000);  // remove once RSTPDN is wired

  nfc.begin();

  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    Serial.println("ERROR: PN532 not found. Power-cycle the PN532 then reset.");
    while (1) delay(10);
  }

  nfc.SAMConfig();
  Serial.println("NFC ready");
}

void moveServoRandom() {
  int angle = random(0, 181);
  servo.write(angle);
  Serial.printf("Servo -> %d deg\n", angle);
}

void setup() {
  Serial.begin(115200);
  servo.attach(SERVO_PIN);
  initNFC();
  Serial.println("Ready — tap a tag");
}

void loop() {
  uint8_t uid[7];
  uint8_t uidLen;

  if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 500)) {
    Serial.print("Tag UID: ");
    for (uint8_t i = 0; i < uidLen; i++) {
      Serial.printf("%02X ", uid[i]);
    }
    Serial.println();
    moveServoRandom();
  }
}
