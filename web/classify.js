// Garment-type classifier: Marqo-FashionCLIP, run in the browser.
//
// The model is zero-shot: it turns the photo into a 512-number "embedding" and
// compares it with the embedding of a text description of each category. The
// category embeddings are computed once, ahead of time, by
// tools/build_category_embeddings.py and stored in category_embeddings.json,
// so only the image half of the model is downloaded here (about 87 MB, 8-bit).
// The browser caches it after the first visit. The photo never leaves the phone.
//
//   preloadClassifier()        start downloading early (optional)
//   classifyGarment(file, cb)  -> [{ name, prob }] best first, probs sum to 1

const TRANSFORMERS_URL = "https://cdn.jsdelivr.net/npm/@huggingface/transformers@4.3.2";
const MODEL_ID = "Marqo/marqo-fashionCLIP";
const LOGIT_SCALE = 100; // CLIP's temperature: turns similarities into confident probabilities

let loading = null;           // Promise<{ model, processor, RawImage, categories }>
let progressListener = null;  // set while classifyGarment is waiting for the download

function load() {
  loading ??= (async () => {
    // Imported on first use, so the library doesn't slow down opening the page.
    const { AutoProcessor, CLIPVisionModelWithProjection, RawImage } =
      await import(TRANSFORMERS_URL);
    const progress_callback = (p) => {
      if (p.status === "progress" && p.file.endsWith(".onnx")) progressListener?.(p.progress / 100);
    };
    const [model, processor, categories] = await Promise.all([
      CLIPVisionModelWithProjection.from_pretrained(MODEL_ID, { dtype: "q8", progress_callback }),
      AutoProcessor.from_pretrained(MODEL_ID),
      fetch(new URL("category_embeddings.json", import.meta.url))
        .then((r) => { if (!r.ok) throw new Error(`category_embeddings.json: ${r.status}`); return r.json(); })
        .then((j) => j.categories),
    ]);
    return { model, processor, RawImage, categories };
  })();
  // A failed download shouldn't block a retry on the next garment.
  loading.catch(() => { loading = null; });
  return loading;
}

/** Starts the model download in the background, e.g. once a photo is chosen. */
export function preloadClassifier() {
  load().catch((err) => console.warn("Classifier preload failed", err));
}

/**
 * Classifies a garment photo (a File or Blob).
 * `onDownload(fraction)` is called while the model is still downloading.
 */
export async function classifyGarment(file, onDownload) {
  progressListener = onDownload;
  try {
    const { model, processor, RawImage, categories } = await load();
    const image = await RawImage.fromBlob(file);
    const { image_embeds } = await model(await processor(image));
    const img = normalise(image_embeds.data);
    const logits = categories.map((c) => LOGIT_SCALE * dot(img, c.vec));
    const probs = softmax(logits);
    return categories
      .map((c, i) => ({ name: c.name, prob: probs[i] }))
      .sort((a, b) => b.prob - a.prob);
  } finally {
    progressListener = null;
  }
}

function normalise(v) {
  let n = 0;
  for (const x of v) n += x * x;
  n = Math.sqrt(n) || 1;
  return Array.from(v, (x) => x / n);
}

function dot(a, b) {
  let s = 0;
  for (let i = 0; i < a.length; i++) s += a[i] * b[i];
  return s;
}

function softmax(xs) {
  const max = Math.max(...xs);
  const e = xs.map((x) => Math.exp(x - max));
  const sum = e.reduce((a, b) => a + b, 0);
  return e.map((x) => x / sum);
}
