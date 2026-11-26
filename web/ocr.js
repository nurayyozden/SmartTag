// Care-label text recognition: PaddleOCR, run in the browser.
//
// Uses ppu-paddle-ocr (PaddleOCR models on ONNX Runtime Web). It finds the
// lines of text in the photo, then reads each one. The photo never leaves the
// phone. The model files (about 13 MB) download from Hugging Face the first
// time and the browser keeps them.
//
// The library imports "onnxruntime-web" and "ppu-ocv/canvas-web" by name; the
// import map in index.html says where to find them. It shares its ONNX
// Runtime with the garment classifier (runtime.js), to save memory.
//
//   preloadLabelReader()   start downloading early (optional)
//   labelReaderReady()     true once the models are loaded
//   readLabel(file)        -> the label's text, lines separated by "\n"

import "./runtime.js";

const PADDLE_URL = "https://cdn.jsdelivr.net/npm/ppu-paddle-ocr@6.7.1/web/index.js";
const MODELS = "https://huggingface.co/snowfluke/ppu-paddle-ocr-models/resolve/main";

// Latin-alphabet model: reads English, French, German, Spanish, Italian,
// Portuguese, Dutch, etc., including accents and "%". Care labels often repeat
// the composition in other scripts too (Chinese, Cyrillic, Arabic); this model
// reads those as nonsense, which the fibre parser simply ignores.
const MODEL = {
  detection: `${MODELS}/detection/PP-OCRv5_mobile_det_infer.onnx`,
  recognition: `${MODELS}/recognition/multi/latin/v5/latin_PP-OCRv5_mobile_rec_infer.onnx`,
  charactersDictionary: `${MODELS}/recognition/multi/latin/v5/ppocrv5_latin_dict.txt`,
};

// Saved with each fibre prediction (see supabase/prediction.sql). Change it
// when MODEL or MAX_SIDE changes.
export const LABEL_READER_VERSION = "ppocr-v5-latin-mobile@1280px";

// Big phone photos are scaled down first. 1280 px read the test labels as well
// as 2000 px or the full photo did (better, on one), using far less memory:
// iPhones ran out of memory with larger images.
const MAX_SIDE = 1280;

let loading = null; // Promise<PaddleOcrService>
let ready = false;

function load() {
  loading ??= (async () => {
    const { PaddleOcrService } = await import(PADDLE_URL);
    const service = new PaddleOcrService({
      model: MODEL,
      session: { executionProviders: ["wasm"] }, // the shared plain-WebAssembly engine
    });
    await service.initialize();
    ready = true;
    return service;
  })();
  // A failed download shouldn't block a retry on the next garment.
  loading.catch(() => { loading = null; });
  return loading;
}

/** Starts the model download in the background. */
export function preloadLabelReader() {
  return load().then(() => {}, (err) => console.warn("Label reader preload failed", err));
}

/** True once the models are downloaded and ready. */
export function labelReaderReady() {
  return ready;
}

/** Reads the text on a care-label photo (a File or Blob). */
export async function readLabel(file) {
  let service, canvas, result;
  try { service = await load(); } catch (e) { throw atStage(e, "loading the label reader"); }
  try { canvas = await toCanvas(file); } catch (e) { throw atStage(e, "preparing the photo"); }
  try { result = await service.recognize(canvas); } catch (e) {
    throw atStage(e, `reading the photo (${canvas.width}x${canvas.height})`);
  }
  return result.text || "";
}

// Marks which step an error came from (saved with failed predictions, see
// recordPredictions in app.js). Some libraries throw plain strings, so those
// are wrapped in an Error first.
function atStage(err, stage) {
  const e = err instanceof Error ? err : new Error(String(err));
  e.stage ??= stage;
  return e;
}

async function toCanvas(file) {
  const bitmap = await createImageBitmap(file);
  const scale = Math.min(1, MAX_SIDE / Math.max(bitmap.width, bitmap.height));
  const canvas = document.createElement("canvas");
  canvas.width = Math.max(1, Math.round(bitmap.width * scale));
  canvas.height = Math.max(1, Math.round(bitmap.height * scale));
  canvas.getContext("2d").drawImage(bitmap, 0, 0, canvas.width, canvas.height);
  bitmap.close();
  return canvas;
}
