# NFC station (write + sort)

One ESP32 + PN532 firmware with two modes:

| Mode | What it does |
| --- | --- |
| **WRITE** (start-up default) | Picks the oldest garment the website saved (`tag_status = 'pending'`), writes its compact record to a blank NTAG, reads it back, then sets `tag_status = 'written'` and `tag_uid`. The website then shows "Tag written ✓". |
| **SORT** | Same as `src/nfc_reader`: reads a tag and tilts the platform LEFT / RIGHT / LEVEL. |

Switch modes with the **BOOT** button on the board, or type `w` / `s` in the
serial monitor (115200 baud).

## Files

| File | What it does |
| --- | --- |
| `main.cpp` | Mode switching, write flow, sort flow |
| `ntag.h` | Reads and writes the NDEF text record on NTAG213/215/216 |
| `supabase.h` | Fetches the next pending garment, marks it written |
| `secrets.h` | Wi-Fi and Supabase settings (git-ignored; see `secrets.example.h`) |

## Set-up

1. Put your Wi-Fi name and password in `secrets.h`. It must be a 2.4 GHz
   network with a normal password (a phone hotspot works; eduroam doesn't).
2. Let the station update rows. The website's key can already insert and
   read `clothing`; the station also needs to **update** it. In the Supabase
   SQL Editor:

   ```sql
   create policy "station marks tags written"
   on clothing for update
   to anon
   using (tag_status = 'pending')
   with check (tag_status = 'written');
   ```

   (Skip this if row-level security is off for `clothing`. If the policy is
   missing, the serial monitor says "no row updated".)
3. `platformio.ini` now defaults to `nfc_station`. Upload and open the
   serial monitor.

## Using it

1. On the phone, photograph the garment and label, review, and press
   **Save and tag**.
2. Within about 3 s the serial monitor prints
   `Next garment a81f2c3d  record: {...}  -> tap a blank tag`.
3. Hold a blank NTAG on the reader until it says `written and checked`.
4. The website switches to "Tag written ✓".

The tag record matches `buildTagRecord()` in `web/analysis.js`, e.g.

```
{"id":"a81f2c3d","cat":3,"col":"BLU","f":[["CO",60],["PES",40]]}
```

## Safety checks

- Tags can be reused: a tag that already holds a different garment is
  overwritten, and the serial output says which garment it held. Set
  `ALLOW_OVERWRITE = false` in `main.cpp` to refuse such tags instead, so
  tapping an old tag by mistake can't take it from its garment.
- If the read-back doesn't match (tag moved during writing), nothing is
  marked written; tap again.
- If the tag is written but Supabase can't be reached, the station keeps
  retrying the update before taking the next garment.
- Garments are tagged oldest first, as the website README says.
