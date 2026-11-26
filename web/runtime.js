// One shared ONNX Runtime for both models.
//
// The garment classifier (transformers.js, classify.js) and the label reader
// (ppu-paddle-ocr, ocr.js) both run their models on ONNX Runtime. Each used
// to bring its own copy, and each copy reserved its own WebAssembly memory,
// which a browser never gives back. On an iPhone the two together ran out of
// memory after a few garments. Now the import map in index.html points both
// libraries at this single copy, so they share one engine and one memory.
//
// It is the plain WebAssembly build (no WebGPU): the smallest engine, and
// the same on every phone. Its .wasm file location is set here, before either
// library starts, because transformers.js would otherwise pick a different,
// larger variant of the engine.

import * as ort from "onnxruntime-web";

ort.env.wasm.wasmPaths = `https://cdn.jsdelivr.net/npm/onnxruntime-web@${ort.env.versions.web}/dist/`;

export { ort };
