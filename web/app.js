// Smart Tag website: photo -> analysis -> review -> save -> wait for station.
//
// Flow
//   1. Garment photo  -> dominant colour (analysis.js)
//   2. Label photo    -> text recognition (Tesseract.js) -> fibres (analysis.js)
//   3. Review         -> user corrects category, colour, fibres
//   4. Save           -> clothing row (tag_status = 'pending') + garment_fibre rows
//   5. Wait           -> the ESP32 tagging station writes the tag and sets
//                        tag_status = 'written'; this page sees it via
//                        Supabase Realtime (with polling as a backup).

import { createClient } from "https://cdn.jsdelivr.net/npm/@supabase/supabase-js@2.45.4/+esm";
import { SUPABASE_URL, SUPABASE_KEY } from "./config.js";
import {
  parseFibres, dominantColour, COLOUR_GROUPS, buildTagRecord,
} from "./analysis.js";

const CONFIGURED = !SUPABASE_URL.includes("YOUR-") && !SUPABASE_KEY.includes("YOUR-");
const db = CONFIGURED ? createClient(SUPABASE_URL, SUPABASE_KEY) : null;

// Used in demo mode; matches the starter data in smart_tag_schema.sql.
const DEMO_CATEGORIES = [
  "T-shirt / top", "Shirt / blouse", "Jumper / sweatshirt", "Trousers", "Jeans",
  "Skirt", "Dress", "Jacket / coat", "Other",
].map((name, i) => ({ category_id: i + 1, name }));
const DEMO_FIBRES = [
  ["CO", "Cotton"], ["PES", "Polyester"], ["WO", "Wool"], ["PA", "Polyamide (nylon)"],
  ["EL", "Elastane (spandex, Lycra)"], ["VI", "Viscose"], ["LI", "Linen"], ["PAN", "Acrylic"],
].map(([code, name], i) => ({ fibre_id: i + 1, code, name }));

const state = {
  categories: [],
  fibres: [],
  garmentFile: null,
  labelFile: null,
  ocrText: "",
  colourRgb: null,
  clothingId: null,
  channel: null,
  pollTimer: null,
};

const $ = (id) => document.getElementById(id);

function show(step) {
  document.querySelectorAll("[data-step]").forEach((s) => { s.hidden = s.dataset.step !== step; });
  window.scrollTo(0, 0);
}

function fail(message, err) {
  console.error(message, err);
  $("error-text").textContent = err?.message ? `${message} (${err.message})` : message;
  show("error");
}

// ---------------------------------------------------------------
// Start: load garment types and fibres
// ---------------------------------------------------------------

async function loadReferenceData() {
  if (!CONFIGURED) {
    state.categories = DEMO_CATEGORIES;
    state.fibres = DEMO_FIBRES;
    return;
  }
  const [cats, fibs] = await Promise.all([
    db.from("garment_category").select("category_id, name").order("category_id"),
    db.from("fibre").select("fibre_id, code, name").order("fibre_id"),
  ]);
  if (cats.error) throw cats.error;
  if (fibs.error) throw fibs.error;
  state.categories = cats.data;
  state.fibres = fibs.data;
}

// ---------------------------------------------------------------
// Photos
// ---------------------------------------------------------------

function wirePhoto(inputId, previewId, nextId, key) {
  $(inputId).addEventListener("change", (e) => {
    const file = e.target.files?.[0];
    if (!file) return;
    state[key] = file;
    const img = $(previewId);
    img.src = URL.createObjectURL(file);
    img.hidden = false;
    $(nextId).disabled = false;
  });
}

async function colourFromFile(file) {
  const bitmap = await createImageBitmap(file);
  const scale = Math.min(1, 160 / Math.max(bitmap.width, bitmap.height));
  const w = Math.max(1, Math.round(bitmap.width * scale));
  const h = Math.max(1, Math.round(bitmap.height * scale));
  const canvas = document.createElement("canvas");
  canvas.width = w; canvas.height = h;
  const ctx = canvas.getContext("2d", { willReadFrequently: true });
  ctx.drawImage(bitmap, 0, 0, w, h);
  return dominantColour(ctx.getImageData(0, 0, w, h).data, w, h);
}

// ---------------------------------------------------------------
// Analysis
// ---------------------------------------------------------------

function setProgress(fraction, text) {
  $("progress-bar").style.width = `${Math.round(fraction * 100)}%`;
  $("progress-text").textContent = text;
}

async function analyse() {
  show("analysing");
  let colour = null;
  let parsed = { fibres: [], total: 0, unknownWords: [], confident: false };
  state.ocrText = "";

  try {
    setProgress(0.05, "Finding the main colour…");
    colour = await colourFromFile(state.garmentFile);
    state.colourRgb = colour.rgb;
  } catch (err) {
    console.warn("Colour detection failed", err);
  }

  if (state.labelFile) {
    try {
      setProgress(0.1, "Reading the care label… (the first time takes longer)");
      const result = await window.Tesseract.recognize(state.labelFile, "eng", {
        logger: (m) => {
          if (m.status === "recognizing text") {
            setProgress(0.1 + 0.9 * m.progress, "Reading the care label…");
          }
        },
      });
      state.ocrText = result.data.text || "";
      parsed = parseFibres(state.ocrText);
    } catch (err) {
      console.warn("Text recognition failed", err);
    }
  }

  setProgress(1, "Done");
  fillReview(colour, parsed);
  show("review");
}

// ---------------------------------------------------------------
// Review
// ---------------------------------------------------------------

function fillReview(colour, parsed) {
  const cat = $("sel-category");
  cat.innerHTML = `<option value="">Choose…</option>` +
    state.categories.map((c) => `<option value="${c.category_id}">${c.name}</option>`).join("");

  const col = $("sel-colour");
  col.innerHTML = `<option value="">Choose…</option>` +
    COLOUR_GROUPS.map((c) => `<option value="${c.code}">${c.name}</option>`).join("");
  col.value = colour?.code || "";
  $("swatch").style.background = colour ? `rgb(${colour.rgb.join(",")})` : "transparent";

  $("fibre-rows").innerHTML = "";
  const usable = parsed.fibres.filter((f) => state.fibres.some((x) => x.code === f.code));
  usable.forEach((f) => addFibreRow(f.code, f.pct));

  let source;
  if (!state.labelFile) source = "No label photo: enter the composition if you know it.";
  else if (usable.length === 0) source = "Couldn't read any fibres from the label. Enter them, or tick “unknown”.";
  else if (parsed.confident) source = "Read from the label. Check the numbers.";
  else source = `Read from the label, but it adds up to ${parsed.total}%. Check the numbers.`;
  if (parsed.unknownWords.length) source += ` Unrecognised: ${parsed.unknownWords.join(", ")}.`;
  $("fibre-source").textContent = source;

  $("chk-unknown").checked = !state.labelFile && usable.length === 0;
  if ($("fibre-rows").children.length === 0 && !$("chk-unknown").checked) addFibreRow("", "");

  $("ocr-text").textContent = state.ocrText.trim() || "(nothing)";
  $("ocr-details").hidden = !state.labelFile;
  $("review-error").textContent = "";
  updateUnknown();
  updateTotal();
}

function addFibreRow(code = "", pct = "") {
  const row = document.createElement("div");
  row.className = "fibre-row";
  row.innerHTML = `
    <select aria-label="Fibre">
      <option value="">Fibre…</option>
      ${state.fibres.map((f) => `<option value="${f.code}">${f.name} (${f.code})</option>`).join("")}
    </select>
    <input type="number" min="1" max="100" step="1" inputmode="numeric" placeholder="%" aria-label="Percentage">
    <button type="button" aria-label="Remove fibre">✕</button>`;
  row.querySelector("select").value = code;
  row.querySelector("input").value = pct;
  row.querySelector("button").addEventListener("click", () => { row.remove(); updateTotal(); clearReviewError(); });
  row.querySelector("select").addEventListener("change", clearReviewError);
  row.querySelector("input").addEventListener("input", () => { updateTotal(); clearReviewError(); });
  $("fibre-rows").appendChild(row);
  updateTotal();
}

function readFibreRows() {
  return [...$("fibre-rows").children].map((row) => ({
    code: row.querySelector("select").value,
    pct: Number(row.querySelector("input").value),
  }));
}

function updateTotal() {
  const total = readFibreRows().reduce((s, r) => s + (r.pct || 0), 0);
  $("fibre-total").textContent = $("chk-unknown").checked ? "" : `Total: ${total}%`;
}

function updateUnknown() {
  const unknown = $("chk-unknown").checked;
  $("fibre-rows").hidden = unknown;
  $("btn-add-fibre").hidden = unknown;
  updateTotal();
}

function clearReviewError() { $("review-error").textContent = ""; }

/** Returns the checked values, or an error message string. */
function validateReview() {
  const categoryId = Number($("sel-category").value);
  const colour = $("sel-colour").value;
  const unknown = $("chk-unknown").checked;
  if (!categoryId) return "Choose the garment type.";
  if (!colour) return "Choose the colour.";
  if (unknown) return { categoryId, colour, unknown, fibres: [] };

  const rows = readFibreRows();
  if (rows.length === 0) return "Add at least one fibre, or tick “Composition unknown”.";
  for (const r of rows) {
    if (!r.code) return "Choose a fibre in every row.";
    if (!Number.isInteger(r.pct) || r.pct < 1 || r.pct > 100) return "Each percentage must be a whole number from 1 to 100.";
  }
  if (new Set(rows.map((r) => r.code)).size !== rows.length) return "Each fibre can only appear once.";
  const total = rows.reduce((s, r) => s + r.pct, 0);
  if (total !== 100) return `The percentages add up to ${total}%. They need to total 100%.`;
  return { categoryId, colour, unknown, fibres: rows };
}

// ---------------------------------------------------------------
// Save and wait for the station
// ---------------------------------------------------------------

async function save() {
  const checked = validateReview();
  if (typeof checked === "string") { $("review-error").textContent = checked; return; }

  const btn = $("btn-confirm");
  btn.disabled = true;
  btn.textContent = "Saving…";
  try {
    let clothingId;
    if (!CONFIGURED) {
      clothingId = crypto.randomUUID();
    } else {
      const { data, error } = await db.from("clothing").insert({
        category_id: checked.categoryId,
        colour: checked.colour,
        fibre_unknown: checked.unknown,
      }).select("clothing_id").single();
      if (error) throw error;
      clothingId = data.clothing_id;

      if (checked.fibres.length) {
        const rows = checked.fibres.map((f) => ({
          clothing_id: clothingId,
          fibre_id: state.fibres.find((x) => x.code === f.code).fibre_id,
          percentage: f.pct,
        }));
        const res = await db.from("garment_fibre").insert(rows);
        if (res.error) {
          // Keep the record consistent: without its fibres it must not be
          // sorted by fibre, so mark the composition as unknown.
          await db.from("clothing").update({ fibre_unknown: true }).eq("clothing_id", clothingId);
          checked.unknown = true;
          alert("The fibres couldn't be saved, so this garment is marked “composition unknown”.");
        }
      }
    }

    state.clothingId = clothingId;
    $("rec-id").textContent = clothingId.slice(0, 8);
    $("tag-preview").textContent = buildTagRecord(
      clothingId, checked.categoryId, checked.colour, checked.fibres, checked.unknown);
    $("btn-simulate").hidden = CONFIGURED;
    show("tagging");
    if (CONFIGURED) waitForStation(clothingId);
  } catch (err) {
    fail("The garment couldn't be saved.", err);
  } finally {
    btn.disabled = false;
    btn.textContent = "Save and tag";
  }
}

function waitForStation(clothingId) {
  stopWaiting();
  state.channel = db.channel(`tag-${clothingId}`)
    .on("postgres_changes",
      { event: "UPDATE", schema: "public", table: "clothing", filter: `clothing_id=eq.${clothingId}` },
      (payload) => { if (payload.new.tag_status === "written") tagWritten(payload.new.tag_uid); })
    .subscribe();

  // Backup in case Realtime isn't enabled or the connection drops.
  state.pollTimer = setInterval(async () => {
    const { data } = await db.from("clothing")
      .select("tag_status, tag_uid").eq("clothing_id", clothingId).single();
    if (data?.tag_status === "written") tagWritten(data.tag_uid);
  }, 3000);
}

function stopWaiting() {
  if (state.pollTimer) clearInterval(state.pollTimer);
  state.pollTimer = null;
  if (state.channel) db.removeChannel(state.channel);
  state.channel = null;
}

function tagWritten(uid) {
  if (state.clothingId === null) return;  // already handled
  stopWaiting();
  state.clothingId = null;
  $("done-uid").textContent = uid || "(not reported)";
  show("done");
}

// ---------------------------------------------------------------
// Wiring
// ---------------------------------------------------------------

function resetPhotos() {
  for (const [input, preview, next, key] of [
    ["in-garment", "prev-garment", "btn-garment-next", "garmentFile"],
    ["in-label", "prev-label", "btn-label-next", "labelFile"],
  ]) {
    $(input).value = "";
    $(preview).hidden = true;
    $(preview).removeAttribute("src");
    $(next).disabled = true;
    state[key] = null;
  }
}

function newGarment() {
  resetPhotos();
  show("garment");
}

async function init() {
  $("demo-banner").hidden = CONFIGURED;
  wirePhoto("in-garment", "prev-garment", "btn-garment-next", "garmentFile");
  wirePhoto("in-label", "prev-label", "btn-label-next", "labelFile");

  document.querySelectorAll("[data-go]").forEach((b) =>
    b.addEventListener("click", () => show(b.dataset.go)));
  $("btn-garment-next").addEventListener("click", () => show("label"));
  $("btn-label-next").addEventListener("click", analyse);
  $("btn-no-label").addEventListener("click", () => { state.labelFile = null; analyse(); });
  $("btn-add-fibre").addEventListener("click", () => { addFibreRow(); clearReviewError(); });
  $("chk-unknown").addEventListener("change", () => { updateUnknown(); clearReviewError(); });
  $("sel-category").addEventListener("change", clearReviewError);
  $("sel-colour").addEventListener("change", () => {
    clearReviewError();
    $("swatch").style.background = "transparent";
  });
  $("btn-confirm").addEventListener("click", save);
  $("btn-simulate").addEventListener("click", () => tagWritten("04:A1:B2:C3:D4:E5:F6 (demo)"));
  $("btn-again").addEventListener("click", newGarment);
  $("btn-error-back").addEventListener("click", () => show("start"));

  const btnStart = $("btn-start");
  btnStart.disabled = true;
  $("start-status").textContent = "Loading…";
  try {
    await loadReferenceData();
    $("start-status").textContent = "";
    btnStart.disabled = false;
    btnStart.addEventListener("click", newGarment);
  } catch (err) {
    fail("Couldn't load garment types and fibres from Supabase. Check config.js and that the tables exist.", err);
  }
}

init();
