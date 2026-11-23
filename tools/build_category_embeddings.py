#!/usr/bin/env python3
"""Builds web/category_embeddings.json for the garment-type classifier.

The website classifies garment photos with Marqo-FashionCLIP, a zero-shot
model: it compares the photo with a text description of each category and
picks the closest. The category descriptions never change, so their text
embeddings are computed once here and shipped as a small JSON file. The phone
then only downloads the image half of the model.

Run this again whenever you add, rename or remove a garment category, or
change the prompts below. Then commit the new JSON.

Setup (once):
    python3 -m venv .venv
    .venv/bin/pip install onnxruntime tokenizers numpy
Run:
    .venv/bin/python tools/build_category_embeddings.py

The model files (about 250 MB) are downloaded to ~/.cache/smarttag the first
time.
"""

import json
import urllib.request
from pathlib import Path

import numpy as np
import onnxruntime as ort
from tokenizers import Tokenizer

MODEL_ID = "Marqo/marqo-fashionCLIP"
BASE_URL = f"https://huggingface.co/{MODEL_ID}/resolve/main"
CACHE = Path.home() / ".cache" / "smarttag" / MODEL_ID.replace("/", "--")
OUT = Path(__file__).resolve().parent.parent / "web" / "category_embeddings.json"

CONTEXT_LENGTH = 77          # CLIP's fixed text length
PAD_ID = 0                   # "!" -- the same padding open_clip uses

# One entry per row of the garment_category table. `name` must match the
# table's name column exactly: the website matches categories by name.
# "Other" is left out on purpose -- it is what the site falls back to when no
# category is a confident match, because CLIP handles vague labels badly.
#
# Each category has a few ways of naming it; they are combined with the
# templates below and averaged, which is steadier than a single prompt.
CATEGORIES = [
    ("T-shirt / top",       ["t-shirt", "tank top", "short-sleeved top", "cami top"]),
    ("Shirt / blouse",      ["button-up shirt", "blouse", "collared shirt", "dress shirt"]),
    ("Jumper / sweatshirt", ["jumper", "sweater", "sweatshirt", "hoodie", "knit cardigan"]),
    ("Trousers",            ["trousers", "chinos", "tailored pants", "joggers"]),
    ("Jeans",               ["jeans", "pair of jeans", "jeans trousers", "skinny jeans"]),
    ("Skirt",               ["skirt", "mini skirt", "midi skirt", "pleated skirt", "denim skirt"]),
    ("Dress",               ["dress", "summer dress", "maxi dress", "shirt dress", "pinafore dress"]),
    ("Jacket / coat",       ["jacket", "coat", "puffer jacket", "blazer", "trench coat"]),
]

TEMPLATES = [
    "a photo of a {}",
    "a {}",
    "a photo of a {}, a type of clothing",
]


def download(filename):
    path = CACHE / filename
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        print(f"Downloading {filename}…")
        tmp = path.with_suffix(path.suffix + ".part")
        urllib.request.urlretrieve(f"{BASE_URL}/{filename}", tmp)
        tmp.rename(path)
    return path


def encode(session, tokenizer, texts):
    ids = np.full((len(texts), CONTEXT_LENGTH), PAD_ID, dtype=np.int64)
    for row, enc in enumerate(tokenizer.encode_batch(texts)):
        tokens = enc.ids[:CONTEXT_LENGTH]
        ids[row, :len(tokens)] = tokens
    emb = session.run(["text_embeds"], {"input_ids": ids})[0]
    return emb / np.linalg.norm(emb, axis=1, keepdims=True)


def main():
    tokenizer = Tokenizer.from_file(str(download("tokenizer.json")))
    session = ort.InferenceSession(str(download("onnx/text_model.onnx")))

    out = []
    for name, words in CATEGORIES:
        prompts = [t.format(w) for w in words for t in TEMPLATES]
        vec = encode(session, tokenizer, prompts).mean(axis=0)
        vec /= np.linalg.norm(vec)
        out.append({"name": name, "vec": [round(float(v), 5) for v in vec]})
        print(f"  {name}: {len(prompts)} prompts")

    OUT.write_text(json.dumps({
        "model": MODEL_ID,
        "note": "Made by tools/build_category_embeddings.py -- do not edit by hand.",
        "categories": out,
    }, separators=(",", ":")) + "\n")
    print(f"Wrote {OUT} ({OUT.stat().st_size // 1024} KB)")


if __name__ == "__main__":
    main()
