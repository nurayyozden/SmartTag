// Reading and writing the NDEF text record on an NTAG213/215/216.
//
// The tag holds one NDEF Text record ("T", language "en") whose text is
// the compact garment record, e.g.
//   {"id":"a81f2c3d","cat":3,"col":"BLU","f":[["CO",60],["PES",40]]}
// This is the same layout NFC Tools writes, so phones can read the tags
// too.
//
// Memory layout from page 4 (4 bytes per page):
//   03 <len>            NDEF message TLV
//   D1 01 <plen> 54     record header: MB|ME|SR, TNF 1, type "T"
//   02 65 6E            status (language length 2), "en"
//   <text>
//   FE                  terminator TLV
#pragma once

#include <Arduino.h>
#include <PN532.h>

namespace ntag {

const uint8_t CC_PAGE         = 3;
const uint8_t FIRST_USER_PAGE = 4;
const uint8_t USER_PAGES      = 36;    // NTAG213 (the smallest) has 36; NTAG215 has 126
const size_t  MAX_NDEF_BYTES  = 200;   // plenty for the compact record

// Reads the NDEF text record into `out`. Returns false if the tag holds
// no readable text record (blank, empty, or not an NTAG). With `verbose`
// the reason is printed.
inline bool readText(PN532& nfc, String& out, bool verbose = true) {
  uint8_t mem[MAX_NDEF_BYTES + 8];
  size_t have = 0;
  size_t need = 8;      // enough to see the TLV and record header
  size_t msgStart = 0;  // where the NDEF message begins in mem
  uint8_t page = FIRST_USER_PAGE;

  while (have < need && have + 4 <= sizeof(mem)) {
    if (!nfc.mifareultralight_ReadPage(page, mem + have)) {
      if (verbose) Serial.printf("  read failed at page %u (is it an NTAG?)\n", page);
      return false;
    }
    have += 4;
    page++;

    if (have == 8) {
      // Skip NULL TLVs (0x00) until the NDEF message TLV (0x03).
      size_t i = 0;
      while (i < have && mem[i] == 0x00) i++;
      if (i >= have || mem[i] != 0x03) {
        if (verbose) Serial.println("  no NDEF message on tag (blank?)");
        return false;
      }
      size_t len = mem[i + 1];
      size_t start = i + 2;
      if (len == 0xFF) {  // 3-byte length form
        len = (mem[i + 2] << 8) | mem[i + 3];
        start = i + 4;
      }
      if (len == 0) {
        if (verbose) Serial.println("  NDEF message is empty");
        return false;
      }
      need = start + len;
      if (need > MAX_NDEF_BYTES) {
        if (verbose) Serial.println("  NDEF message too long for buffer");
        return false;
      }
      msgStart = start;
    }
  }

  const uint8_t* rec = mem + msgStart;

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

  if (tnf != 0x01 || typeLen != 1 || rec[p] != 'T') {
    if (verbose) Serial.println("  first record is not a text record");
    return false;
  }
  p += typeLen + idLen;

  uint8_t langLen = rec[p] & 0x3F;
  size_t textStart = p + 1 + langLen;
  if (payloadLen < 1u + langLen || msgStart + textStart + (payloadLen - 1 - langLen) > have) {
    if (verbose) Serial.println("  text record is malformed");
    return false;
  }
  size_t textLen = payloadLen - 1 - langLen;

  out = "";
  out.reserve(textLen);
  for (size_t k = 0; k < textLen; k++) out += (char)rec[textStart + k];
  return true;
}

// Writes `text` as a single NDEF text record, then reads it back to check.
// Returns false (with the reason printed) if anything fails.
inline bool writeText(PN532& nfc, const String& text) {
  // Capability container: a factory-blank NTAG already has one (E1 ...).
  // If it is all zeros, write the NTAG215 default. Bits in the CC can only
  // be set, never cleared, so a CC that isn't NDEF is left alone.
  uint8_t cc[4];
  if (!nfc.mifareultralight_ReadPage(CC_PAGE, cc)) {
    Serial.println("  couldn't read the tag (is it an NTAG213/215/216?)");
    return false;
  }
  if (cc[0] == 0x00 && cc[1] == 0x00 && cc[2] == 0x00 && cc[3] == 0x00) {
    uint8_t ndefCc[4] = { 0xE1, 0x10, 0x3E, 0x00 };
    if (!nfc.mifareultralight_WritePage(CC_PAGE, ndefCc)) {
      Serial.println("  couldn't format the tag for NDEF");
      return false;
    }
  } else if (cc[0] != 0xE1) {
    Serial.printf("  tag isn't NDEF-formatted (CC %02X %02X %02X %02X)\n",
                  cc[0], cc[1], cc[2], cc[3]);
    return false;
  }

  const size_t textLen = text.length();
  const size_t payloadLen = 1 + 2 + textLen;   // status byte + "en" + text
  const size_t recordLen  = 4 + payloadLen;    // header, type len, payload len, "T"
  if (recordLen > 254) {                       // keeps both lengths in one byte
    Serial.println("  record too long for a short NDEF record");
    return false;
  }

  uint8_t buf[USER_PAGES * 4];
  size_t n = 0;
  buf[n++] = 0x03;
  buf[n++] = (uint8_t)recordLen;
  buf[n++] = 0xD1;
  buf[n++] = 0x01;
  buf[n++] = (uint8_t)payloadLen;
  buf[n++] = 'T';
  buf[n++] = 0x02;
  buf[n++] = 'e';
  buf[n++] = 'n';
  if (n + textLen + 1 > sizeof(buf)) {
    Serial.println("  record doesn't fit on the tag");
    return false;
  }
  memcpy(buf + n, text.c_str(), textLen);
  n += textLen;
  buf[n++] = 0xFE;
  while (n % 4) buf[n++] = 0x00;

  for (size_t i = 0; i < n / 4; i++) {
    if (!nfc.mifareultralight_WritePage(FIRST_USER_PAGE + i, buf + 4 * i)) {
      Serial.printf("  write failed at page %u - keep the tag still\n",
                    (unsigned)(FIRST_USER_PAGE + i));
      return false;
    }
  }

  // The library doesn't check the tag's ACK, so confirm by reading back.
  String check;
  if (!readText(nfc, check) || check != text) {
    Serial.println("  read-back didn't match - keep the tag still and try again");
    return false;
  }
  return true;
}

// Formats a UID as "04:A1:B2:C3:D4:E5:F6".
inline String uidToString(const uint8_t* uid, uint8_t len) {
  String s;
  char hex[4];
  for (uint8_t i = 0; i < len; i++) {
    snprintf(hex, sizeof(hex), i ? ":%02X" : "%02X", uid[i]);
    s += hex;
  }
  return s;
}

}  // namespace ntag
