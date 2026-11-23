# Smart Tag website

The phone side of the system. It photographs a garment and its care label,
works out the colour and fibre composition, lets you correct them, and saves
the garment to Supabase. The ESP32 **tagging station** then writes the record
to a blank NFC tag, and this page shows "Tag written ✓".

iPhones don't let websites use NFC (Web NFC is Chrome-on-Android only), which
is why the station writes the tags rather than the phone.

## Files

| File | What it does |
| --- | --- |
| `index.html` | The page and its six steps |
| `style.css` | Layout and colours (light and dark mode) |
| `config.js` | Your Supabase URL and publishable key |
| `analysis.js` | Fibre parsing from label text, colour detection, the tag record |
| `app.js` | The flow: photos, text recognition, review, saving, waiting for the station |

## 1. Connect it to Supabase

Open `config.js` and paste in your **Project URL** and **publishable (anon)
key** from Supabase → Project Settings → API. Never use the secret /
service_role key here: this file is public.

Until you do, the site runs in **demo mode**: everything works on screen,
nothing is saved, and a "Simulate the station" button stands in for the ESP32.

## 2. Run it on your Mac

The page uses JavaScript modules, so it must be served, not opened as a file:

```
cd web
python3 -m http.server 8000
```

Then open http://localhost:8000 in your browser.

## 3. Put it online (to use it on your iPhone)

Any static hosting works, because Supabase is the backend. With GitHub Pages:

1. Create a GitHub repository and upload the contents of this `web` folder.
2. Settings → Pages → deploy from the `main` branch, root folder.
3. Open the `https://<your-username>.github.io/<repo>/` link on your iPhone.

## 4. Test the "tag written" step before the station exists

Save a garment, then in the Supabase SQL Editor run:

```sql
update clothing set tag_status = 'written', tag_uid = 'TEST'
where tag_status = 'pending';
```

The page should switch to "Tag written ✓" within a few seconds.

## How it works

1. **Colour:** the central 60% of the garment photo is grouped into four
   colours (k-means); the biggest group is named, e.g. BLU.
2. **Fibres:** Tesseract.js reads the label text in the browser; the parser
   finds pairs like "60% Cotton" or "CO 60%" in several languages, ignores the
   lining section and repeated languages, and maps them to fibre codes.
3. **Review:** you correct anything; percentages must total 100%, or you tick
   "Composition unknown" (the machine then keeps the platform level).
4. **Save:** a `clothing` row (`tag_status = 'pending'`) plus one
   `garment_fibre` row per fibre.
5. **Wait:** Supabase Realtime (with polling every 3 s as a backup) watches
   for the station to set `tag_status = 'written'`.

The tag record the station should write looks like:

```
{"id":"a81f2c3d","cat":3,"col":"BLU","f":[["CO",60],["PES",40]]}
```

## Notes and limits

- Photos stay on the phone; only the garment data is saved.
- The station writes pending garments **oldest first**. A garment saved and
  then abandoned stays first in the queue until it is tagged.
- Text recognition in the browser is less accurate than Apple's own, so the
  review step matters. Close, flat, well-lit label photos help most.
- Only the eight fibres in the `fibre` table can be saved. To add more, insert
  rows there (e.g. `insert into fibre (code, name) values ('SE', 'Silk');`)
  and add their label words to `SYNONYMS` in `analysis.js`.
- The garment type is chosen by hand for now; a classifier can fill it in later.
