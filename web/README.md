# Smart Tag website

The phone side of the system. It photographs a garment and its care label,
works out the garment type, colour and fibre composition, lets you correct them, and saves
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
| `ocr.js` | Reads the care-label text (PaddleOCR, runs in the browser) |
| `classify.js` | Garment type from the photo (Marqo-FashionCLIP, runs in the browser) |
| `category_embeddings.json` | The garment types as the classifier sees them; made by `tools/build_category_embeddings.py` |
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

1. **Garment type:** Marqo-FashionCLIP compares the photo with a text
   description of each garment type and picks the closest. If it's at least
   50% sure, the type is pre-selected; otherwise it's only suggested. The
   model (about 87 MB) downloads from Hugging Face once, starting as soon as
   the garment photo is chosen, and the browser keeps it.
2. **Colour:** the central 60% of the garment photo is grouped into four
   colours (k-means); the biggest group is named, e.g. BLU.
3. **Fibres:** PaddleOCR (via the ppu-paddle-ocr library) reads the label
   text in the browser. It uses the Latin-alphabet model (about 13 MB,
   downloaded once and kept by the browser), so it reads English, French,
   German, Spanish, Italian and similar languages, accents included. The
   parser then finds pairs like "60% Cotton" or "CO 60%", ignores the lining
   section and repeated languages, and maps them to fibre codes.
4. **Review:** you correct anything; percentages must total 100%, or you tick
   "Composition unknown" (the machine then keeps the platform level).
5. **Save:** a `clothing` row (`tag_status = 'pending'`) plus one
   `garment_fibre` row per fibre.
6. **Wait:** Supabase Realtime (with polling every 3 s as a backup) watches
   for the station to set `tag_status = 'written'`.

The tag record the station should write looks like:

```
{"id":"a81f2c3d","cat":3,"col":"BLU","f":[["CO",60],["PES",40]]}
```

## Reviewing the models' mistakes

Each saved garment also records what the models guessed (garment type, colour
and fibres) next to what you finally chose, in the `prediction` table. Set it
up once: run [`supabase/prediction.sql`](../supabase/prediction.sql) in the
Supabase SQL Editor. Until then the site works as before and only logs a
warning in the browser console.

Then, in the Supabase Table Editor:

- **`prediction_mistakes`**: every wrong guess, newest first, e.g.
  garment_type: Jeans → Jacket / coat (61%). For fibres, `details` holds the
  text read from the label.
- **`prediction_accuracy`**: % correct per model and model version. The
  version changes when the prompts, models or parsing rules change, so you
  can see whether a change helped.

For the garment type the photo's embedding (512 numbers, not the photo) is
saved as well. With the type you chose, each one is a labelled example a
future version of the site can learn from. No photos are stored.

## Notes and limits

- Photos stay on the phone; only the garment data is saved.
- The station writes pending garments **oldest first**. A garment saved and
  then abandoned stays first in the queue until it is tagged.
- The label reader can still misread small, curved or shiny labels, so the
  review step matters. Close, flat, well-lit label photos help most.
- Only the eight fibres in the `fibre` table can be saved. To add more, insert
  rows there (e.g. `insert into fibre (code, name) values ('SE', 'Silk');`)
  and add their label words to `SYNONYMS` in `analysis.js`.
- The classifier only knows the garment types listed in
  `tools/build_category_embeddings.py`, matched to the `garment_category`
  table **by name**. After adding or renaming a type in Supabase, update that
  list, run the script (instructions at its top) and commit the new
  `category_embeddings.json`. "Other" is deliberately not in the list.
- If the model can't load (offline, very old browser), the garment type is
  simply left for you to choose.
