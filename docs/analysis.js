// Pure analysis functions (no browser or network needed), so they can be
// tested on their own:
//   parseFibres(text)       care-label text -> fibre codes and percentages
//   dominantColour(px,w,h)  garment photo pixels -> colour group code
//   buildTagRecord(...)     the compact record the station writes to the tag

// ---------------------------------------------------------------
// Fibres
// ---------------------------------------------------------------

// Standard textile codes (as in the Supabase `fibre` table) and the words
// that commonly appear on care labels for each. Words are upper case with
// accents removed, because the label text is normalised the same way.
const SYNONYMS = {
  CO:  ["CO", "COTTON", "COTON", "BAUMWOLLE", "ALGODON", "COTONE", "KATOEN"],
  PES: ["PES", "PL", "POLYESTER", "POLIESTER", "POLIESTERE"],
  WO:  ["WO", "WV", "WOOL", "LAINE", "WOLLE", "LANA", "WOL", "MERINO"],
  PA:  ["PA", "NYLON", "POLYAMIDE", "POLYAMID", "POLIAMIDA", "POLIAMMIDE"],
  EL:  ["EL", "EA", "ELASTANE", "ELASTHAN", "ELASTAN", "ELASTANO", "SPANDEX", "LYCRA"],
  VI:  ["VI", "CV", "VISCOSE", "VISKOSE", "VISCOSA", "RAYON"],
  LI:  ["LI", "LINEN", "LIN", "LEINEN", "LINO"],
  PAN: ["PAN", "ACRYLIC", "ACRYLIQUE", "ACRYL", "ACRILICO"],
};

const WORD_TO_CODE = new Map();
for (const [code, words] of Object.entries(SYNONYMS)) {
  for (const w of words) WORD_TO_CODE.set(w, code);
}

// Words that start the lining section; only the main fabric is used.
const LINING = /\b(LINING|LINER|FUTTER|DOUBLURE|FORRO|FODERA|FODERO|VOERING)\b/;

export function normalise(text) {
  return (text || "")
    .normalize("NFD").replace(/[̀-ͯ]/g, "")   // strip accents
    .toUpperCase()
    .replace(/(\d)O(?=\s*%)/g, "$10")                   // OCR: "6O%" -> "60%"
    .replace(/\s+/g, " ")
    .trim();
}

/**
 * Finds fibre/percentage pairs in care-label text.
 * Handles "60% Cotton 40% Polyester", "Cotton 60% Polyester 40%",
 * the short codes ("60% CO"), several languages, labels that repeat
 * the composition in each language, and a separate lining section.
 *
 * Returns { fibres: [{code, pct}], total, unknownWords, confident }.
 * `confident` is true when the percentages add up to exactly 100.
 */
export function parseFibres(rawText) {
  const text = normalise(rawText);

  // Keep only the main fabric: cut at "LINING" (etc.) if a percentage
  // appears before it.
  let section = text;
  const lining = text.match(LINING);
  if (lining && lining.index > 0 && /\d\s*%/.test(text.slice(0, lining.index))) {
    section = text.slice(0, lining.index);
  }

  // Labels put the number either before or after the fibre name. Try both
  // readings and keep the one that recognises more fibres (ties go to
  // number-first, the most common layout). Mixing them would pair a number
  // with the wrong fibre.
  const pctFirst = [...section.matchAll(/(\d{1,3})\s*%\s*([A-Z]+)/g)]
    .map((m) => ({ pct: Number(m[1]), word: m[2] }));
  const wordFirst = [...section.matchAll(/([A-Z]+)\s*(\d{1,3})\s*%/g)]
    .map((m) => ({ pct: Number(m[2]), word: m[1] }));
  const known = (list) => list.filter((e) => WORD_TO_CODE.has(e.word)).length;
  const pairs = known(pctFirst) >= known(wordFirst) ? pctFirst : wordFirst;

  const fibres = [];
  const unknown = new Set();
  let total = 0;
  for (const { pct, word } of pairs) {
    const code = WORD_TO_CODE.get(word);
    if (!code) { unknown.add(word); continue; }
    if (pct < 1 || pct > 100) continue;
    if (fibres.some((f) => f.code === code)) continue;  // same fibre in another language
    if (total + pct > 100) continue;                    // repeated block in another language
    fibres.push({ code, pct });
    total += pct;
  }

  return { fibres, total, unknownWords: [...unknown], confident: total === 100 };
}

// ---------------------------------------------------------------
// Colour
// ---------------------------------------------------------------

// Colour groups used across the system. The sorting machine's
// DARK_COLOURS list (BLK, NVY, BRN, GRY, BLU) uses these codes.
export const COLOUR_GROUPS = [
  { code: "BLK", name: "Black" },
  { code: "WHT", name: "White" },
  { code: "GRY", name: "Grey" },
  { code: "BGE", name: "Beige" },
  { code: "BRN", name: "Brown" },
  { code: "RED", name: "Red" },
  { code: "ORG", name: "Orange" },
  { code: "YEL", name: "Yellow" },
  { code: "GRN", name: "Green" },
  { code: "BLU", name: "Blue" },
  { code: "NVY", name: "Navy" },
  { code: "PUR", name: "Purple" },
  { code: "PNK", name: "Pink" },
  { code: "MLT", name: "Multicoloured" },
];

export function rgbToHsv(r, g, b) {
  r /= 255; g /= 255; b /= 255;
  const max = Math.max(r, g, b), min = Math.min(r, g, b), d = max - min;
  let h = 0;
  if (d !== 0) {
    if (max === r) h = ((g - b) / d) % 6;
    else if (max === g) h = (b - r) / d + 2;
    else h = (r - g) / d + 4;
    h *= 60;
    if (h < 0) h += 360;
  }
  return { h, s: max === 0 ? 0 : d / max, v: max };
}

/** Maps an RGB colour to one of the COLOUR_GROUPS codes. */
export function nameColour(r, g, b) {
  const { h, s, v } = rgbToHsv(r, g, b);
  if (v < 0.2) return "BLK";
  if (s < 0.15) return v > 0.82 ? "WHT" : "GRY";
  if (h >= 20 && h < 50 && s < 0.4 && v > 0.65) return "BGE";
  if (h >= 10 && h < 50 && v < 0.6) return "BRN";
  if (h < 12 || h >= 345) return (s < 0.45 && v > 0.7) ? "PNK" : "RED";
  if (h < 40) return "ORG";
  if (h < 70) return "YEL";
  if (h < 170) return "GRN";
  if (h < 255) return v < 0.45 ? "NVY" : "BLU";
  if (h < 290) return "PUR";
  return "PNK";
}

/**
 * Finds the main colour in an image.
 * `pixels` is RGBA data (as from canvas getImageData), `w` x `h` in size.
 * Only the central 60% of the image is used, which is usually garment
 * rather than background, then the pixels are grouped (k-means, 4 groups)
 * and the largest group's average colour is named.
 * Returns { code, rgb: [r, g, b], share } where share is the fraction of
 * pixels in that group.
 */
export function dominantColour(pixels, w, h, k = 4) {
  const pts = [];
  const x0 = Math.floor(w * 0.2), x1 = Math.ceil(w * 0.8);
  const y0 = Math.floor(h * 0.2), y1 = Math.ceil(h * 0.8);
  for (let y = y0; y < y1; y++) {
    for (let x = x0; x < x1; x++) {
      const i = (y * w + x) * 4;
      if (pixels[i + 3] < 128) continue;  // skip transparent pixels
      pts.push([pixels[i], pixels[i + 1], pixels[i + 2]]);
    }
  }
  if (pts.length === 0) return { code: "GRY", rgb: [128, 128, 128], share: 0 };

  // Deterministic start: centres spread across the brightness range.
  const sorted = [...pts].sort((a, b) => (a[0] + a[1] + a[2]) - (b[0] + b[1] + b[2]));
  k = Math.min(k, pts.length);
  let centres = Array.from({ length: k }, (_, j) =>
    [...sorted[Math.floor((j + 0.5) * sorted.length / k)]]);

  const assign = new Array(pts.length).fill(0);
  for (let iter = 0; iter < 10; iter++) {
    for (let p = 0; p < pts.length; p++) {
      let best = 0, bestD = Infinity;
      for (let c = 0; c < k; c++) {
        const dr = pts[p][0] - centres[c][0], dg = pts[p][1] - centres[c][1], db = pts[p][2] - centres[c][2];
        const d = dr * dr + dg * dg + db * db;
        if (d < bestD) { bestD = d; best = c; }
      }
      assign[p] = best;
    }
    const sums = Array.from({ length: k }, () => [0, 0, 0, 0]);
    for (let p = 0; p < pts.length; p++) {
      const s = sums[assign[p]];
      s[0] += pts[p][0]; s[1] += pts[p][1]; s[2] += pts[p][2]; s[3]++;
    }
    centres = sums.map((s, c) => (s[3] ? [s[0] / s[3], s[1] / s[3], s[2] / s[3]] : centres[c]));
  }

  const counts = new Array(k).fill(0);
  for (const a of assign) counts[a]++;
  const top = counts.indexOf(Math.max(...counts));
  const rgb = centres[top].map(Math.round);
  return { code: nameColour(...rgb), rgb, share: counts[top] / pts.length };
}

// ---------------------------------------------------------------
// Tag record
// ---------------------------------------------------------------

/**
 * The compact text the station writes to the tag, e.g.
 *   {"id":"a81f2c3d","cat":3,"col":"BLU","f":[["CO",60],["PES",40]]}
 * "f" is left out when the composition is unknown, which makes the
 * sorting machine keep the platform level for a manual check.
 */
export function buildTagRecord(clothingId, categoryId, colour, fibres, fibreUnknown) {
  const rec = { id: String(clothingId).slice(0, 8), cat: categoryId, col: colour };
  if (!fibreUnknown && fibres.length) rec.f = fibres.map((f) => [f.code, f.pct]);
  return JSON.stringify(rec);
}
