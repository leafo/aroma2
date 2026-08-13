// MoonScript-to-Lua compilation via the wasm compiler hosted on
// moonscript.org. Workers must be same-origin, so a tiny blob stub imports
// the remote worker module; the module installs the message protocol on the
// stub's global and resolves moonscript.wasm against its own origin.
// The version param both pins the compiler build and sidesteps the CDN's
// long-lived cache of the bare URL; bump it to pick up a newer deploy.
const COMPILER_URL = 'https://moonscript.org/compiler/moonscript-worker.js?v=aroma-20260813';

let worker = null;
let requestId = 0;
const pending = new Map();

function getWorker() {
  if (!worker) {
    const stub = new Blob([`import ${JSON.stringify(COMPILER_URL)};`], {
      type: 'text/javascript',
    });
    worker = new Worker(URL.createObjectURL(stub), { type: 'module' });
    worker.onmessage = (e) => {
      const [id, result] = e.data;
      const callbacks = pending.get(id);
      if (callbacks) {
        pending.delete(id);
        callbacks(result);
      }
    };
    worker.onerror = (e) => {
      const err = new Error(`MoonScript compiler failed to load: ${e.message || 'worker error'}`);
      for (const callbacks of pending.values()) {
        callbacks({ error: err.message });
      }
      pending.clear();
      worker.terminate();
      worker = null;
    };
  }
  return worker;
}

// compile_moonscript returns compile errors in-band as the result string
// (the published API has no success flag), so recognize its failure shapes
const COMPILE_ERROR_PREFIXES = ['failed to parse', 'Failed to load code compiler'];

export function compileMoonScript(code, options = {}) {
  return new Promise((resolve, reject) => {
    const id = ++requestId;
    pending.set(id, (result) => {
      if (result && typeof result === 'object' && result.error) {
        reject(new Error(result.error));
      } else if (typeof result === 'string' && COMPILE_ERROR_PREFIXES.some((p) => result.startsWith(p))) {
        reject(new Error(`MoonScript: ${result}`));
      } else {
        resolve(result);
      }
    });
    getWorker().postMessage([id, 'compile', code, options]);
  });
}
