// Initialises the PN532 over I2C, waits for a tag tap, reads the
//   text record stored on the tag, decides LEFT / RIGHT / LEVEL
//   from one sort rule, and tilts the MG996R platform accordingly.
//
// ---------------------------------------------------------------
// CHANGES (13 Nov 2026) — previous version saved in
//   backup/nfc_reader_main_before_2026-11-13.cpp
//
// 1. Reads the tag's DATA, not just its UID.
//    The old code only printed the tag's serial number. This version
//    reads the NDEF text record from an NTAG215 (the format NFC Tools
//    and the tagging station write), e.g.
//      {"id":"a81f","cat":3,"col":"BLU","f":[["CO",60],["PES",40]]}
//    Pages are read directly with mifareultralight_ReadPage(), so the
//    separate NDEF library (NfcAdapter) is not needed.
//
// 2. Sort rule instead of a random angle.
//    SORT_BY picks the parameter (fibre or colour), mirroring the
//    sort_rule table in Supabase:
//      fibre:  one fibre >= 95%  -> LEFT,  blend -> RIGHT
//      colour: dark colour code  -> LEFT,  light -> RIGHT
//    Missing/unreadable data -> platform stays LEVEL (manual check).
//
// 3. Fixed, safe servo angles.
//    The random 0-180 deg move could drive the platform into the frame.
//    Now it only goes LEVEL (90), LEFT (50) or RIGHT (130), then returns
//    to level. Adjust the three constants once the platform is built.
//    The servo is also attached with an explicit 50 Hz signal and the
//    MG996R's 500-2400 us pulse range.
//
// 4. No repeat triggering.
//    A tag left on the reader used to fire every ~0.5 s. The UID of the
//    last tag is remembered and ignored until that tag has been away
//    for TAG_GONE_MS.
//
// Unchanged: GPIO13 for the servo, I2C on GPIO21/22, the 3 s start-up
//   delay and the RSTPDN note (still worth wiring later).
// ---------------------------------------------------------------

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

// ---- Settings ---------------------------------------------------
enum SortBy { SORT_FIBRE, SORT_COLOUR };
const SortBy SORT_BY = SORT_FIBRE;     // change to SORT_COLOUR to test colour

const int ANGLE_LEVEL = 90;
const int ANGLE_LEFT  = 50;            // 40 deg one way
const int ANGLE_RIGHT = 130;           // 40 deg the other way
const unsigned long TILT_HOLD_MS = 1500;   // time tilted before returning
const unsigned long TAG_GONE_MS  = 1000;   // tag must be away this long before it counts again

const int SINGLE_FIBRE_MIN_PCT = 95;
const char* DARK_COLOURS[] = { "BLK", "NVY", "BRN", "GRY", "BLU" };  // edit to suit

// NTAG215: user memory starts at page 4, 4 bytes per page.
const uint8_t FIRST_USER_PAGE = 4;
const size_t  MAX_NDEF_BYTES  = 200;   // plenty for the compact record
// -----------------------------------------------------------------

PN532_I2C pn532i2c(Wire);
PN532 nfc(pn532i2c);
Servo servo;

enum Decision { GO_LEFT, GO_RIGHT, STAY_LEVEL };

uint8_t lastUid[7];
uint8_t lastUidLen = 0;
unsigned long lastSeenMs = 0;

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

// ---- Reading the tag ----------------------------------------------

// Reads the NDEF text record into `out`. Returns false if the tag
// holds no readable text record.
bool readTagText(String& out) {
  uint8_t mem[MAX_NDEF_BYTES + 8];
  size_t have = 0;

  // Read pages until we have the TLV header, then as many as the
  // message needs.
  size_t need = 8;  // enough to see the TLV and record header
  size_t msgStart = 0;  // where the NDEF message begins in mem
  uint8_t page = FIRST_USER_PAGE;
  while (have < need && have + 4 <= sizeof(mem)) {
    if (!nfc.mifareultralight_ReadPage(page, mem + have)) {
      Serial.printf("  read failed at page %u\n", page);
      return false;
    }
    have += 4;
    page++;

    if (have == 8) {
      // Skip NULL TLVs (0x00) and other TLVs until the NDEF message TLV (0x03).
      size_t i = 0;
      while (i < have && mem[i] == 0x00) i++;
      if (i >= have || mem[i] != 0x03) {
        Serial.println("  no NDEF message on tag (is it blank or not NTAG?)");
        return false;
      }
      size_t len = mem[i + 1];
      size_t start = i + 2;
      if (len == 0xFF) {  // 3-byte length form
        len = (mem[i + 2] << 8) | mem[i + 3];
        start = i + 4;
      }
      need = start + len;
      if (need > MAX_NDEF_BYTES) {
        Serial.println("  NDEF message too long for buffer");
        return false;
      }
      msgStart = start;
    }
  }

  const uint8_t* rec = mem + msgStart;

  // NDEF record header
  uint8_t header   = rec[0];
  bool shortRecord = header & 0x10;
  bool hasId       = header & 0x08;
  uint8_t tnf      = header & 0x07;
  uint8_t typeLen  = rec[1];
  size_t p = 2;
  uint32_t payloadLen;
  if (shortRecord) {
    payloadLen = rec[p++];
  } else {
    payloadLen = ((uint32_t)rec[p] << 24) | ((uint32_t)rec[p + 1] << 16) |
                 ((uint32_t)rec[p + 2] << 8) | rec[p + 3];
    p += 4;
  }
  uint8_t idLen = hasId ? rec[p++] : 0;

  // Expect a well-known (TNF 1) Text record, type "T".
  if (tnf != 0x01 || typeLen != 1 || rec[p] != 'T') {
    Serial.println("  first record is not a text record");
    return false;
  }
  p += typeLen + idLen;

  // Text payload: status byte (low 6 bits = language-code length), language code, text.
  uint8_t langLen = rec[p] & 0x3F;
  size_t textStart = p + 1 + langLen;
  size_t textLen = payloadLen - 1 - langLen;

  out = "";
  for (size_t k = 0; k < textLen; k++) out += (char)rec[textStart + k];
  return true;
}

// ---- Deciding ------------------------------------------------------

// Pulls the value of a simple "key":"value" pair out of the text.
String jsonString(const String& s, const char* key) {
  String k = String("\"") + key + "\":\"";
  int a = s.indexOf(k);
  if (a < 0) return "";
  a += k.length();
  int b = s.indexOf('"', a);
  return (b < 0) ? "" : s.substring(a, b);
}

// Largest single-fibre percentage in "f":[["CO",60],["PES",40]], or -1 if absent.
int maxFibrePct(const String& s) {
  int a = s.indexOf("\"f\":[");
  if (a < 0) return -1;
  int end = s.indexOf("]]", a);
  if (end < 0) return -1;
  int best = -1;
  int i = a;
  while ((i = s.indexOf(',', i)) >= 0 && i < end) {
    i++;
    if (isDigit(s[i])) {
      int pct = s.substring(i).toInt();
      if (pct > best) best = pct;
    }
  }
  return best;
}

Decision decide(const String& text) {
  if (SORT_BY == SORT_FIBRE) {
    int pct = maxFibrePct(text);
    if (pct < 0) { Serial.println("  fibre: unknown"); return STAY_LEVEL; }
    Serial.printf("  fibre: largest share %d%%\n", pct);
    return (pct >= SINGLE_FIBRE_MIN_PCT) ? GO_LEFT : GO_RIGHT;
  } else {
    String col = jsonString(text, "col");
    if (col.length() == 0) { Serial.println("  colour: unknown"); return STAY_LEVEL; }
    Serial.printf("  colour: %s\n", col.c_str());
    for (const char* dark : DARK_COLOURS) {
      if (col.equalsIgnoreCase(dark)) return GO_LEFT;
    }
    return GO_RIGHT;
  }
}

// ---- Moving --------------------------------------------------------

void tilt(Decision d) {
  if (d == STAY_LEVEL) {
    Serial.println("  -> stay level (needs manual check)");
    return;
  }
  int angle = (d == GO_LEFT) ? ANGLE_LEFT : ANGLE_RIGHT;
  Serial.printf("  -> tilt %s (%d deg)\n", d == GO_LEFT ? "LEFT" : "RIGHT", angle);
  servo.write(angle);
  delay(TILT_HOLD_MS);
  servo.write(ANGLE_LEVEL);
}

// ---- Main ----------------------------------------------------------

void setup() {
  Serial.begin(115200);
  servo.setPeriodHertz(50);
  servo.attach(SERVO_PIN, 500, 2400);
  servo.write(ANGLE_LEVEL);
  initNFC();
  Serial.printf("Ready - sorting by %s. Tap a tag.\n",
                SORT_BY == SORT_FIBRE ? "fibre" : "colour");
}

void loop() {
  uint8_t uid[7];
  uint8_t uidLen;

  if (!nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 200)) {
    return;  // no tag this pass
  }

  // Same tag still on the reader? Ignore it until it has been away.
  bool sameTag = (uidLen == lastUidLen) && memcmp(uid, lastUid, uidLen) == 0;
  unsigned long now = millis();
  if (sameTag && now - lastSeenMs < TAG_GONE_MS) {
    lastSeenMs = now;
    return;
  }
  memcpy(lastUid, uid, uidLen);
  lastUidLen = uidLen;
  lastSeenMs = now;

  Serial.print("Tag UID: ");
  for (uint8_t i = 0; i < uidLen; i++) Serial.printf("%02X ", uid[i]);
  Serial.println();

  String text;
  if (!readTagText(text)) {
    tilt(STAY_LEVEL);
    return;
  }
  Serial.printf("  data: %s\n", text.c_str());

  tilt(decide(text));

  // Count the time spent tilting as "seen", so the same tag isn't re-sorted
  // straight away if it is still on the platform.
  lastSeenMs = millis();
}
