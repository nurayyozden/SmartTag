// Copy this file to secrets.h (same folder) and fill it in.
// secrets.h is git-ignored so your Wi-Fi password isn't committed.
#pragma once

// 2.4 GHz network (the ESP32 can't join 5 GHz). Imperial's eduroam uses
// WPA2-Enterprise, which this simple login doesn't support: use a phone
// hotspot or a home network.
#define WIFI_SSID     "YOUR-WIFI-NAME"
#define WIFI_PASSWORD "YOUR-WIFI-PASSWORD"

// Same values as web/config.js. Use the PUBLISHABLE key, never the
// secret / service_role key.
#define SUPABASE_URL  "https://YOUR-PROJECT.supabase.co"
#define SUPABASE_KEY  "sb_publishable_..."
