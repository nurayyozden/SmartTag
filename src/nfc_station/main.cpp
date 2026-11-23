// Smart Tag NFC station: one ESP32 + PN532, two modes.
//
//   WRITE mode (tagging station)
//     Gets the oldest garment saved by the website (tag_status =
//     'pending') as a compact record, e.g.
//       {"id":"a81f2c3d","cat":3,"col":"BLU","f":[["CO",60],["PES",40]]}
//     and writes it to the next blank NTAG tapped on the reader. After
//     reading it back to check, the garment is set to tag_status =
//     'written' with its tag_uid, so the website shows "Tag written ✓".
//
//     Two ways to reach Supabase (USE_WIFI below):
//       false (default): USB. tools/usb_bridge.py on the laptop talks to
//              Supabase and sends jobs over the serial cable:
//                laptop -> ESP32   JOB <clothing_id> <record>   |  CLEAR
//                ESP32 -> laptop   @READY  |  @WROTE <clothing_id> <uid>
//       true:  Wi-Fi. The ESP32 talks to Supabase itself (secrets.h).
//
//   SORT mode (sorting machine)
//     Same behaviour as src/nfc_reader: reads the record from a tapped
//     tag, decides LEFT / RIGHT / LEVEL from the sort rule, and tilts the
//     MG996R platform.
//
// Switching mode: press the BOOT button on the ESP32 board, or type
//   w  (write)  /  s  (sort)  + Enter in the serial monitor / bridge.
// The blue on-board LED shows the mode: on = WRITE, off = SORT.
//
// Wiring is unchanged from nfc_reader: PN532 on I2C (GPIO21 SDA, GPIO22
// SCL), servo signal on GPIO13.

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <PN532_I2C.h>
#include <PN532.h>
#include <ESP32Servo.h>

#include "secrets.h"
#include "ntag.h"
#include "supabase.h"

#define SERVO_PIN 13
#define MODE_BUTTON_PIN 0  // the BOOT button on most ESP32 dev boards
#define MODE_LED_PIN 2     // the blue on-board LED: on = WRITE, off = SORT

// ---- Settings ---------------------------------------------------
enum Mode { MODE_WRITE, MODE_SORT };
const Mode START_MODE = MODE_WRITE;

// Write mode
const bool USE_WIFI = false;          // false = jobs come over USB from tools/usb_bridge.py
const unsigned long POLL_MS = 3000;   // how often to check Supabase for new garments
const bool ALLOW_OVERWRITE = true;    // false = refuse tags that already hold another garment

// Sort mode (same as nfc_reader)
enum SortBy { SORT_FIBRE, SORT_COLOUR };
const SortBy SORT_BY = SORT_FIBRE;

const int ANGLE_LEVEL = 90;
const int ANGLE_LEFT  = 50;
const int ANGLE_RIGHT = 130;
const unsigned long TILT_HOLD_MS = 1500;

const int SINGLE_FIBRE_MIN_PCT = 95;
const char* DARK_COLOURS[] = { "BLK", "NVY", "BRN", "GRY", "BLU" };

// Both modes
const unsigned long TAG_GONE_MS = 1000;  // tag must be away this long before it counts again
// -----------------------------------------------------------------

PN532_I2C pn532i2c(Wire);
PN532 nfc(pn532i2c);
Servo servo;

Mode mode = START_MODE;

// Tag debounce
uint8_t lastUid[7];
uint8_t lastUidLen = 0;
unsigned long lastSeenMs = 0;

// Write-mode state
supa::Job job;
bool haveJob = false;
bool needMark = false;         // tag written, Supabase not yet updated
String writtenUid;
unsigned long lastPollMs = 0;
bool pollNow = true;

// ---- Set-up ------------------------------------------------------

// If the ESP32 restarted in the middle of an I2C message, the PN532 can be
// left holding the data line low, waiting for clock pulses that never
// come. Sending nine clock pulses lets it finish and release the bus.
void recoverI2CBus() {
  const int SDA_PIN = 21, SCL_PIN = 22;
  pinMode(SDA_PIN, INPUT_PULLUP);
  pinMode(SCL_PIN, OUTPUT_OPEN_DRAIN);
  for (int i = 0; i < 9; i++) {
    digitalWrite(SCL_PIN, LOW);
    delayMicroseconds(10);
    digitalWrite(SCL_PIN, HIGH);
    delayMicroseconds(10);
  }
}

void initNFC() {
  recoverI2CBus();
  Wire.begin();
  Wire.setClock(100000);
  delay(3000);  // remove once RSTPDN is wired (see nfc_reader)

  // The PN532 sometimes misses the first attempt after a reset, so try a
  // few times before giving up.
  for (int attempt = 1; attempt <= 5; attempt++) {
    nfc.begin();
    if (nfc.getFirmwareVersion()) {
      nfc.SAMConfig();
      Serial.println("NFC ready");
      return;
    }
    Serial.printf("PN532 not answering (attempt %d of 5)\n", attempt);
    delay(1000);
  }

  // Still nothing: restart and try again rather than hanging forever.
  Serial.println("ERROR: PN532 not found. Check its wiring and I2C switches; "
                 "restarting in 5 s (unplug the USB cable to power-cycle it if this repeats).");
  delay(5000);
  ESP.restart();
}

void initWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("Connecting to Wi-Fi \"%s\"", WIFI_SSID);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf(" connected (%s)\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println(" not connected yet; will keep trying (sort mode works offline)");
  }
}

void announceMode() {
  digitalWrite(MODE_LED_PIN, mode == MODE_WRITE ? HIGH : LOW);
  if (mode == MODE_WRITE) {
    Serial.println("\n=== WRITE mode: writing tags for garments saved on the website ===");
    pollNow = true;
  } else {
    Serial.printf("\n=== SORT mode: sorting by %s. Tap a tag. ===\n",
                  SORT_BY == SORT_FIBRE ? "fibre" : "colour");
  }
}

// ---- Serial input and mode switching ------------------------------

void showJob() {
  Serial.printf("\nNext garment %s\n  record: %s (%u bytes)\n  -> tap a blank tag\n",
                job.clothingId.substring(0, 8).c_str(), job.record.c_str(),
                (unsigned)job.record.length());
}

// Handles one line from the serial port. Returns the mode it asks for.
Mode handleLine(String line, Mode wanted) {
  line.trim();
  if (line.equalsIgnoreCase("w")) return MODE_WRITE;
  if (line.equalsIgnoreCase("s")) return MODE_SORT;

  if (!USE_WIFI && line.startsWith("JOB ")) {
    int space = line.indexOf(' ', 4);
    if (space < 0) return wanted;
    String id = line.substring(4, space);
    String record = line.substring(space + 1);
    if (!haveJob || id != job.clothingId || record != job.record) {
      job.clothingId = id;
      job.record = record;
      haveJob = true;
      showJob();
    }
  } else if (!USE_WIFI && line == "CLEAR") {
    if (haveJob) Serial.println("\nQueue empty - save a garment on the website.");
    haveJob = false;
  }
  return wanted;
}

void checkModeInput() {
  static int lastButton = HIGH;
  static unsigned long lastChangeMs = 0;
  static String line;

  Mode wanted = mode;

  int b = digitalRead(MODE_BUTTON_PIN);
  if (b != lastButton && millis() - lastChangeMs > 50) {
    lastChangeMs = millis();
    lastButton = b;
    if (b == LOW) wanted = (mode == MODE_WRITE) ? MODE_SORT : MODE_WRITE;
  }

  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (line.length()) wanted = handleLine(line, wanted);
      line = "";
    } else if (line.length() < 400) {
      line += c;
    }
  }

  if (wanted != mode) {
    if (needMark) {
      Serial.println("(finishing the Supabase update before switching)");
      return;
    }
    mode = wanted;
    announceMode();
  }
}

// ---- Tag detection -------------------------------------------------

// Returns true when a tag that wasn't just handled is on the reader.
bool newTag(uint8_t* uid, uint8_t& uidLen) {
  if (!nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 200)) return false;

  bool sameTag = (uidLen == lastUidLen) && memcmp(uid, lastUid, uidLen) == 0;
  unsigned long now = millis();
  if (sameTag && now - lastSeenMs < TAG_GONE_MS) {
    lastSeenMs = now;
    return false;
  }
  memcpy(lastUid, uid, uidLen);
  lastUidLen = uidLen;
  lastSeenMs = now;
  return true;
}

// ---- WRITE mode ----------------------------------------------------

// Pulls "id":"..." out of a record, or "" if absent.
String recordId(const String& s) {
  int a = s.indexOf("\"id\":\"");
  if (a < 0) return "";
  a += 6;
  int b = s.indexOf('"', a);
  return (b < 0) ? "" : s.substring(a, b);
}

void tryMark() {
  String error;
  supa::MarkResult r = supa::markWritten(job.clothingId, writtenUid, error);
  if (r == supa::MARK_OK) {
    Serial.printf("  Supabase updated - the website now shows \"Tag written\" (%s)\n",
                  writtenUid.c_str());
  } else if (r == supa::MARK_NOT_PENDING) {
    Serial.printf("  WARNING: %s\n", error.c_str());
  } else {
    Serial.printf("  Supabase update failed, retrying: %s\n", error.c_str());
    return;  // keep needMark set and try again later
  }
  needMark = false;
  haveJob = false;
  pollNow = true;
}

void pollQueue() {
  if (!pollNow && millis() - lastPollMs < POLL_MS) return;
  lastPollMs = millis();
  pollNow = false;

  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastWarnMs = 0;
    if (millis() - lastWarnMs > 10000) {
      Serial.println("  waiting for Wi-Fi...");
      lastWarnMs = millis();
    }
    return;
  }

  supa::Job next;
  String error;
  if (supa::fetchNextJob(next, error)) {
    if (!haveJob || next.clothingId != job.clothingId || next.record != job.record) {
      job = next;
      haveJob = true;
      showJob();
    }
  } else if (error.length()) {
    Serial.printf("  couldn't check Supabase: %s\n", error.c_str());
  } else if (haveJob) {
    haveJob = false;
    Serial.println("\nQueue empty - save a garment on the website.");
  }
}

void loopWrite() {
  if (USE_WIFI && needMark) {
    static unsigned long lastTryMs = 0;
    if (millis() - lastTryMs > POLL_MS) {
      lastTryMs = millis();
      tryMark();
    }
    return;
  }

  if (USE_WIFI) pollQueue();

  uint8_t uid[7];
  uint8_t uidLen;
  if (!newTag(uid, uidLen)) return;

  String uidStr = ntag::uidToString(uid, uidLen);
  Serial.printf("Tag %s\n", uidStr.c_str());

  if (!haveJob) {
    Serial.println(USE_WIFI
        ? "  nothing to write - save a garment on the website first"
        : "  nothing to write - save a garment on the website, and check usb_bridge.py is running");
    return;
  }

  // A tag that already belongs to another garment is refused, or (with
  // ALLOW_OVERWRITE) overwritten with a note saying which garment it held.
  String existing;
  if (ntag::readText(nfc, existing, false)) {
    String oldId = recordId(existing);
    String newId = job.clothingId.substring(0, 8);
    if (oldId.length() && oldId != newId) {
      if (!ALLOW_OVERWRITE) {
        Serial.printf("  this tag already holds garment %s - use a blank tag\n"
                      "  (or set ALLOW_OVERWRITE = true)\n", oldId.c_str());
        return;
      }
      Serial.printf("  overwriting garment %s on this tag\n", oldId.c_str());
    }
  }

  Serial.println("  writing...");
  if (!ntag::writeText(nfc, job.record)) {
    lastUidLen = 0;  // let the same tag be retried straight away
    return;
  }
  Serial.printf("  written and checked: %s\n", job.record.c_str());
  lastSeenMs = millis();

  if (!USE_WIFI) {
    // The laptop bridge updates Supabase and then sends the next job.
    Serial.printf("@WROTE %s %s\n", job.clothingId.c_str(), uidStr.c_str());
    haveJob = false;
    return;
  }

  writtenUid = uidStr;
  needMark = true;
  tryMark();
  lastSeenMs = millis();
}

// ---- SORT mode -----------------------------------------------------

enum Decision { GO_LEFT, GO_RIGHT, STAY_LEVEL };

String jsonString(const String& s, const char* key) {
  String k = String("\"") + key + "\":\"";
  int a = s.indexOf(k);
  if (a < 0) return "";
  a += k.length();
  int b = s.indexOf('"', a);
  return (b < 0) ? "" : s.substring(a, b);
}

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
  }
  String col = jsonString(text, "col");
  if (col.length() == 0) { Serial.println("  colour: unknown"); return STAY_LEVEL; }
  Serial.printf("  colour: %s\n", col.c_str());
  for (const char* dark : DARK_COLOURS) {
    if (col.equalsIgnoreCase(dark)) return GO_LEFT;
  }
  return GO_RIGHT;
}

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

void loopSort() {
  uint8_t uid[7];
  uint8_t uidLen;
  if (!newTag(uid, uidLen)) return;

  Serial.printf("Tag %s\n", ntag::uidToString(uid, uidLen).c_str());

  String text;
  if (!ntag::readText(nfc, text)) {
    tilt(STAY_LEVEL);
    return;
  }
  Serial.printf("  data: %s\n", text.c_str());
  tilt(decide(text));
  lastSeenMs = millis();  // don't re-sort straight away if it's still on the platform
}

// ---- Main ----------------------------------------------------------

void setup() {
  Serial.begin(115200);
  pinMode(MODE_BUTTON_PIN, INPUT_PULLUP);
  pinMode(MODE_LED_PIN, OUTPUT);
  digitalWrite(MODE_LED_PIN, LOW);

  servo.setPeriodHertz(50);
  servo.attach(SERVO_PIN, 500, 2400);
  servo.write(ANGLE_LEVEL);

  initNFC();
  if (USE_WIFI) initWiFi();
  else Serial.println("USB mode: jobs come from tools/usb_bridge.py");

  Serial.println("Press BOOT or type w / s to switch between WRITE and SORT mode.");
  announceMode();
  Serial.println("@READY");  // tells the bridge it can send the current job
}

void loop() {
  checkModeInput();
  if (mode == MODE_WRITE) loopWrite();
  else loopSort();
}
