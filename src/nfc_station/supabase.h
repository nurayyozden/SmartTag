// Talking to Supabase (PostgREST over HTTPS) for the tagging station.
//
//   fetchNextJob()  oldest clothing row with tag_status = 'pending',
//                   turned into the compact tag record
//   markWritten()   sets tag_status = 'written' and tag_uid, which the
//                   website is watching for ("Tag written ✓")
#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "secrets.h"

namespace supa {

struct Job {
  String clothingId;   // full UUID
  String record;       // compact text to write to the tag
};

// One HTTPS request. Returns the HTTP status (or a negative error).
inline int request(const char* method, const String& path, const String& body,
                   String& response) {
  WiFiClientSecure client;
  client.setInsecure();  // skips certificate checks; fine for a class prototype
  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(client, String(SUPABASE_URL) + "/rest/v1/" + path)) return -1;
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Prefer", "return=representation");
  int status = http.sendRequest(method, body);
  response = (status > 0) ? http.getString() : http.errorToString(status);
  http.end();
  return status;
}

// Builds the same text as buildTagRecord() in web/analysis.js:
//   {"id":"a81f2c3d","cat":3,"col":"BLU","f":[["CO",60],["PES",40]]}
// "f" is left out when the composition is unknown.
inline String buildRecord(JsonObjectConst row) {
  String id = row["clothing_id"] | "";
  String rec = "{\"id\":\"" + id.substring(0, 8) + "\"";
  rec += ",\"cat\":" + String(row["category_id"] | 0);
  rec += ",\"col\":\"" + String(row["colour"] | "") + "\"";

  JsonArrayConst fibres = row["garment_fibre"].as<JsonArrayConst>();
  bool unknown = row["fibre_unknown"] | false;
  if (!unknown && fibres.size() > 0) {
    rec += ",\"f\":[";
    bool first = true;
    for (JsonObjectConst gf : fibres) {
      const char* code = gf["fibre"]["code"] | "";
      if (!*code) continue;
      if (!first) rec += ",";
      rec += "[\"" + String(code) + "\"," + String(gf["percentage"] | 0) + "]";
      first = false;
    }
    rec += "]";
  }
  rec += "}";
  return rec;
}

// Fetches the oldest pending garment. Returns true and fills `job` if there
// is one; false if the queue is empty or the request failed (`error` says
// which: empty string means simply nothing pending).
inline bool fetchNextJob(Job& job, String& error) {
  error = "";
  String resp;
  int status = request("GET",
      "clothing?select=clothing_id,category_id,colour,fibre_unknown,"
      "garment_fibre(percentage,fibre(code))"
      "&tag_status=eq.pending&order=created_at.asc&limit=1"
      "&garment_fibre.order=percentage.desc",
      "", resp);
  if (status != 200) {
    error = "HTTP " + String(status) + ": " + resp.substring(0, 120);
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, resp);
  if (err) {
    error = String("bad JSON: ") + err.c_str();
    return false;
  }
  JsonArrayConst rows = doc.as<JsonArrayConst>();
  if (rows.size() == 0) return false;

  JsonObjectConst row = rows[0];
  job.clothingId = row["clothing_id"] | "";
  job.record = buildRecord(row);
  return job.clothingId.length() > 0;
}

enum MarkResult { MARK_OK, MARK_NOT_PENDING, MARK_FAILED };

// Marks the garment as tagged. Only updates it if it is still pending.
inline MarkResult markWritten(const String& clothingId, const String& uid, String& error) {
  String resp;
  String body = "{\"tag_status\":\"written\",\"tag_uid\":\"" + uid + "\"}";
  int status = request("PATCH",
      "clothing?clothing_id=eq." + clothingId + "&tag_status=eq.pending",
      body, resp);
  if (status != 200) {
    error = "HTTP " + String(status) + ": " + resp.substring(0, 120);
    return MARK_FAILED;
  }
  // With row-level security, a blocked update returns [] rather than an
  // error, so check that a row actually changed.
  if (resp.indexOf("clothing_id") < 0) {
    error = "no row updated (no longer pending, or no UPDATE policy for the key - see README)";
    return MARK_NOT_PENDING;
  }
  return MARK_OK;
}

}  // namespace supa
