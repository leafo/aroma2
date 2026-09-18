import wasmAromaModule from './wasm-aroma.js';
import { createTextureStore } from './texture-store.js';
import { createAudioStore } from './audio-store.js';
import { unzip } from './unzip.js';

// Where a project's files live in emscripten's filesystem, PROJECT_ROOT in
// main.c
const PROJECT_ROOT = '/game';

/**
 * Initialize the Aroma WebAssembly runtime
 * @param {HTMLCanvasElement} canvas - The canvas element to render to
 * @param {Object} [options]
 * @param {boolean} [options.pixelPerfect=true] - Size the canvas's CSS box so
 *   its pixels land on whole screen pixels. Turn off to lay the canvas out
 *   with your own CSS, at the cost of the browser's smoothing when it scales
 * @returns {Promise<Object>} Promise that resolves with the initialized Aroma module
 */
export async function initAroma(canvas, options = {}) {
  if (!canvas) {
    throw new Error('Canvas element is required');
  }

  const moduleConfig = {
    canvas,
    locateFile: (path) => `./${path}`,
  };

  let textureStore = null;
  let projectMounted = false;

  // Paths given to newImage and the like. With a project mounted they name
  // its files and nothing else, without one they are urls
  async function readAsset(Module, path) {
    if (projectMounted) {
      const full = `${PROJECT_ROOT}/${path.replace(/^\/+/, '')}`;
      if (!Module.FS.analyzePath(full).exists) {
        throw new Error(`No file ${path} in the project`);
      }
      return new Blob([Module.FS.readFile(full)]);
    }
    const response = await fetch(path);
    if (!response.ok) {
      throw new Error(`Failed to fetch ${path}: ${response.status}`);
    }
    return response.blob();
  }

  function ensureTextureStore(Module) {
    if (!textureStore) {
      const gl = Module.ctx;
      if (!gl) {
        return null;
      }
      textureStore = createTextureStore(gl);
    }
    return textureStore;
  }

  moduleConfig.preRun = moduleConfig.preRun || [];
  moduleConfig.preRun.push((Module) => {
    async function waitForTextureStore() {
      let store = ensureTextureStore(Module);
      while (!store) {
        await new Promise((resolve) => setTimeout(resolve, 0));
        store = ensureTextureStore(Module);
      }
      return store;
    }

    // Completion callbacks must always fire, even on failure: the Lua
    // coroutine that requested the load is suspended until it is resumed.
    Module.requestTextureLoad = (generation, imagePtr, url) => {
      Promise.all([waitForTextureStore(), readAsset(Module, url)])
        .then(([store, blob]) => store.load(blob))
        .then(({ id, width, height }) => {
          Module._aroma_image_loaded(generation, imagePtr, id, width, height);
        })
        .catch((err) => {
          console.error('Failed to load texture', url, err);
          Module._aroma_image_loaded(generation, imagePtr, 0, 0, 0);
        });
    };

    Module.audio = createAudioStore();

    Module.requestAudioLoad = (generation, sourcePtr, url) => {
      readAsset(Module, url)
        .then((blob) => Module.audio.load(blob))
        .then(({ id, duration }) => {
          Module._aroma_source_loaded(generation, sourcePtr, id, duration);
        })
        .catch((err) => {
          console.error('Failed to load audio', url, err);
          Module._aroma_source_loaded(generation, sourcePtr, 0, 0);
        });
    };

    Module.requestFontLoad = async (generation, fontPtr, url, glyphs, _extraSpacing) => {
      try {
        const store = await waitForTextureStore();

        const bitmap = await createImageBitmap(await readAsset(Module, url));

        const canvas = document.createElement('canvas');
        canvas.width = bitmap.width;
        canvas.height = bitmap.height;
        const ctx = canvas.getContext('2d');
        if (!ctx) {
          throw new Error('Failed to acquire 2D context for font parsing');
        }

        ctx.drawImage(bitmap, 0, 0);
        if (typeof bitmap.close === 'function') {
          bitmap.close();
        }

        const imageData = ctx.getImageData(0, 0, canvas.width, canvas.height);
        const pixelData = imageData.data;
        const firstRow = pixelData.slice(0, canvas.width * 4);

        const parsedGlyphs = parseImageFont(firstRow, glyphs, canvas.width);

        if (firstRow.length >= 4) {
          const spacerR = firstRow[0];
          const spacerG = firstRow[1];
          const spacerB = firstRow[2];
          const spacerA = firstRow[3];

          for (let i = 0; i < pixelData.length; i += 4) {
            if (
              pixelData[i] === spacerR &&
              pixelData[i + 1] === spacerG &&
              pixelData[i + 2] === spacerB &&
              pixelData[i + 3] === spacerA
            ) {
              pixelData[i + 3] = 0;
            }
          }

          ctx.putImageData(imageData, 0, 0);
        }

        // Unflipped so texture row 0 is the image's top row, matching the
        // top-down V coordinates used by graphics.print. (Images go through
        // createImageBitmap, where UNPACK_FLIP_Y_WEBGL is ignored anyway.)
        const result = store.createFromSource(canvas, { flipY: false });

        const maxGlyphs = Module._aroma_font_max_glyphs();
        const glyphCount = Math.min(parsedGlyphs.length, maxGlyphs);

        if (parsedGlyphs.length > glyphCount) {
          console.warn(`Truncating image font glyphs for ${url} to ${glyphCount} entries`);
        }

        const glyphBufferPtr = Module._get_glyph_buffer();
        const glyphArray = new Int32Array(Module.HEAP32.buffer, glyphBufferPtr, glyphCount * 3);

        for (let i = 0; i < glyphCount; i++) {
          glyphArray[i * 3 + 0] = parsedGlyphs[i].x;
          glyphArray[i * 3 + 1] = parsedGlyphs[i].width;
          glyphArray[i * 3 + 2] = parsedGlyphs[i].codepoint;
        }

        Module._aroma_font_set_glyphs(generation, fontPtr, result.id, result.width, result.height, glyphCount);
      } catch (err) {
        console.error('Failed to load font', url, err);
        Module._aroma_font_set_glyphs(generation, fontPtr, 0, 0, 0, 0);
      }
    };

    // Parse image font glyphs in JavaScript
    function parseImageFont(pixelData, glyphString, width) {
      const glyphs = [];

      // First pixel is spacer color
      const spacer = (pixelData[0] << 24) | (pixelData[1] << 16) | (pixelData[2] << 8) | pixelData[3];

      let start = 0;
      let end = 0;
      let glyphIndex = 0;

      // Convert glyph string to array of codepoints
      const codepoints = Array.from(glyphString).map(char => char.codePointAt(0));

      while (glyphIndex < codepoints.length && end < width) {
        start = end;

        // Skip spacer pixels
        while (start < width) {
          const idx = start * 4;
          const pixel = (pixelData[idx] << 24) | (pixelData[idx + 1] << 16) |
                       (pixelData[idx + 2] << 8) | pixelData[idx + 3];
          if (pixel !== spacer) break;
          start++;
        }

        end = start;

        // Find end of glyph
        while (end < width) {
          const idx = end * 4;
          const pixel = (pixelData[idx] << 24) | (pixelData[idx + 1] << 16) |
                       (pixelData[idx + 2] << 8) | pixelData[idx + 3];
          if (pixel === spacer) break;
          end++;
        }

        if (start >= end) break;

        glyphs.push({
          x: start,
          width: end - start,
          codepoint: codepoints[glyphIndex]
        });

        glyphIndex++;
      }

      return glyphs;
    }

    Module.bindTexture = (id) => {
      const store = ensureTextureStore(Module);
      if (!store || !store.bind(id)) {
        console.warn('Attempted to bind missing texture', id);
      }
    };

    Module.createTextureFromPixels = (pixels, width, height) => {
      const store = ensureTextureStore(Module);
      return store ? store.createFromPixels(pixels, width, height) : 0;
    };

    Module.createCanvasTexture = (width, height) => {
      const store = ensureTextureStore(Module);
      return store ? store.createCanvas(width, height) : 0;
    };

    Module.attachCanvasDepth = (id) => {
      const store = ensureTextureStore(Module);
      return store && store.attachDepth(id) ? 1 : 0;
    };

    Module.bindFramebuffer = (id) => {
      const store = ensureTextureStore(Module);
      if (store) {
        store.bindFramebuffer(id);
      }
    };

    Module.setTextureParams = (id, minNearest, magNearest, wrapH, wrapV) => {
      const store = ensureTextureStore(Module);
      if (store) {
        store.setParams(id, minNearest, magNearest, wrapH, wrapV);
      }
    };

    Module.releaseTexture = (id) => {
      const store = ensureTextureStore(Module);
      if (store) {
        store.release(id);
      }
    };
  });

  const Module = await wasmAromaModule(moduleConfig);
  console.log('Aroma WASM module loaded successfully');

  if (options.pixelPerfect !== false) {
    keepPixelPerfect(canvas, Module);
  }

  const pressedKeys = new Set();

  Module.isKeyDown = (keyName) => {
    return pressedKeys.has(keyName);
  };

  // Map browser key names to love naming convention
  function mapKeyName(key, location) {
    const keyMap = {
      // Arrow keys
      'ArrowUp': 'up',
      'ArrowDown': 'down',
      'ArrowLeft': 'left',
      'ArrowRight': 'right',

      // Common keys
      'Enter': 'return',
      'Escape': 'escape',
      ' ': 'space',
      'Tab': 'tab',
      'Backspace': 'backspace',
      'Delete': 'delete',
      'Insert': 'insert',

      // Navigation
      'Home': 'home',
      'End': 'end',
      'PageUp': 'pageup',
      'PageDown': 'pagedown',

      // Lock keys
      'CapsLock': 'capslock',
      'NumLock': 'numlock',
      'ScrollLock': 'scrolllock',

      // Other
      'PrintScreen': 'printscreen',
      'Pause': 'pause',

      // Function keys
      'F1': 'f1', 'F2': 'f2', 'F3': 'f3', 'F4': 'f4',
      'F5': 'f5', 'F6': 'f6', 'F7': 'f7', 'F8': 'f8',
      'F9': 'f9', 'F10': 'f10', 'F11': 'f11', 'F12': 'f12',
      'F13': 'f13', 'F14': 'f14', 'F15': 'f15', 'F16': 'f16',
      'F17': 'f17', 'F18': 'f18', 'F19': 'f19', 'F20': 'f20',
      'F21': 'f21', 'F22': 'f22', 'F23': 'f23', 'F24': 'f24',

      // Numpad
      'Numpad0': 'kp0', 'Numpad1': 'kp1', 'Numpad2': 'kp2',
      'Numpad3': 'kp3', 'Numpad4': 'kp4', 'Numpad5': 'kp5',
      'Numpad6': 'kp6', 'Numpad7': 'kp7', 'Numpad8': 'kp8',
      'Numpad9': 'kp9',
      'NumpadDivide': 'kp/',
      'NumpadMultiply': 'kp*',
      'NumpadSubtract': 'kp-',
      'NumpadAdd': 'kp+',
      'NumpadDecimal': 'kp.',
      'NumpadEnter': 'kpenter',
      'NumpadEqual': 'kp=',
    };

    // Check static mappings
    const mapped = keyMap[key];
    if (mapped) return mapped;

    // Handle modifier keys with location
    // location: 1 = left, 2 = right, 0 = standard
    if (key === 'Control') {
      return location === 2 ? 'rctrl' : 'lctrl';
    }
    if (key === 'Shift') {
      return location === 2 ? 'rshift' : 'lshift';
    }
    if (key === 'Alt') {
      return location === 2 ? 'ralt' : 'lalt';
    }
    if (key === 'Meta') {
      return location === 2 ? 'rgui' : 'lgui';
    }

    // Return lowercase for single characters (letters, numbers, symbols)
    if (key.length === 1) {
      return key.toLowerCase();
    }

    // For any other keys, return lowercase
    return key.toLowerCase();
  }

  // Set up keyboard event handling
  canvas.setAttribute('tabindex', '0'); // Make canvas focusable
  // Keys the page would otherwise act on while the game has focus: scrolling
  // and moving focus off the canvas. Browser shortcuts are left alone
  const pageKeys = new Set([' ', 'ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight', 'Tab', 'Backspace', 'PageUp', 'PageDown', 'Home', 'End']);

  function claimKey(e) {
    if (pageKeys.has(e.key) && !e.ctrlKey && !e.metaKey && !e.altKey) {
      e.preventDefault();
    }
  }

  canvas.addEventListener('keydown', (e) => {
    Module.audio.unlock();
    claimKey(e);
    const key = mapKeyName(e.key, e.location);
    pressedKeys.add(key);
    if (Module._aroma_keypressed) {
      Module.ccall('aroma_keypressed', null, ['string', 'number'], [key, e.repeat ? 1 : 0]);
    }
  });

  canvas.addEventListener('keyup', (e) => {
    claimKey(e);
    const key = mapKeyName(e.key, e.location);
    pressedKeys.delete(key);
    if (Module._aroma_keyreleased) {
      Module.ccall('aroma_keyreleased', null, ['string'], [key]);
    }
  });

  // Mouse positions are in canvas pixels, which differ from CSS pixels when
  // the page scales the canvas
  function mousePosition(e) {
    const rect = canvas.getBoundingClientRect();
    return [
      Math.floor((e.clientX - rect.left) * canvas.width / (rect.width || 1)),
      Math.floor((e.clientY - rect.top) * canvas.height / (rect.height || 1)),
    ];
  }

  // DOM button index to love's: 1 left, 2 right, 3 middle
  const mouseButtons = [1, 3, 2];
  const heldButtons = new Set();

  canvas.addEventListener('mousedown', (e) => {
    Module.audio.unlock();
    const button = mouseButtons[e.button] || e.button + 1;
    heldButtons.add(button);
    const [x, y] = mousePosition(e);
    Module._aroma_mousebutton(1, x, y, button, e.detail || 1);
  });

  // On the window so a drag that leaves the canvas still ends, only buttons
  // that went down on the canvas are reported
  window.addEventListener('mouseup', (e) => {
    const button = mouseButtons[e.button] || e.button + 1;
    if (!heldButtons.delete(button)) return;
    const [x, y] = mousePosition(e);
    Module._aroma_mousebutton(0, x, y, button, e.detail || 1);
  });

  // Moves outside the canvas only count in the middle of a drag
  window.addEventListener('mousemove', (e) => {
    if (e.target !== canvas && heldButtons.size === 0) return;
    const [x, y] = mousePosition(e);
    Module._aroma_mousemoved(x, y);
  });

  // Right click belongs to the game
  canvas.addEventListener('contextmenu', (e) => e.preventDefault());

  canvas.addEventListener('focus', () => {
    if (Module._aroma_focus) {
      Module._aroma_focus(1);
    }
  });

  canvas.addEventListener('blur', () => {
    // keyups are lost while unfocused, so don't leave keys stuck down
    pressedKeys.clear();
    if (Module._aroma_focus) {
      Module._aroma_focus(0);
    }
  });

  function removeTree(path) {
    const FS = Module.FS;
    if (!FS.analyzePath(path).exists) return;
    for (const name of FS.readdir(path)) {
      if (name === '.' || name === '..') continue;
      const child = `${path}/${name}`;
      if (FS.isDir(FS.stat(child).mode)) {
        removeTree(child);
      } else {
        FS.unlink(child);
      }
    }
    FS.rmdir(path);
  }

  // files is a Map of path to Uint8Array
  function runProject(files) {
    const FS = Module.FS;
    removeTree(PROJECT_ROOT);
    FS.mkdir(PROJECT_ROOT);
    for (const [path, data] of projectFiles(files)) {
      const full = `${PROJECT_ROOT}/${path}`;
      FS.mkdirTree(full.slice(0, full.lastIndexOf('/')));
      FS.writeFile(full, data);
    }
    projectMounted = true;
    if (Module._run_project() !== 0) {
      throw new Error('Failed to start the project');
    }
  }

  return {
    module: Module,
    runProject,
    runLove: async (buffer) => runProject(await unzip(buffer)),
    runCode: (code) => {
      projectMounted = false;
      try {
        const result = Module.ccall(
          'run_lua_code',
          'number',
          ['string'],
          [code]
        );
        if (result !== 0) {
          throw new Error('Lua code execution failed');
        }
        return true;
      } catch (err) {
        console.error('Error running Lua code:', err);
        throw err;
      }
    }
  };
}

// A .love zipped from outside the game's folder has everything one level
// down. love runs those too, so the folder holding main.lua is taken as the
// root and what lies outside of it is dropped
function projectFiles(files) {
  let root = '';
  if (!files.has('main.lua')) {
    const mains = [...files.keys()].filter((path) => path.endsWith('/main.lua'));
    if (mains.length === 0) {
      throw new Error('No main.lua in the project');
    }
    mains.sort((a, b) => a.length - b.length);
    root = mains[0].slice(0, -'main.lua'.length);
  }

  const out = new Map();
  for (const [path, data] of files) {
    if (!path.startsWith(root) || path.split('/').includes('..')) continue;
    out.set(path.slice(root.length), data);
  }
  return out;
}

// A canvas is laid out in CSS pixels, which on a display scaled to say 1.25
// aren't screen pixels: left alone, an 800 pixel canvas is stretched over
// 1000 of them and smoothed. The CSS size is set so that every canvas pixel
// covers a whole number of screen pixels instead, 1 up to a ratio of 1.5, 2
// from there. The canvas looks smaller than its size in CSS pixels as a
// result, which is what a desktop window of that size looks like too.
//
// The size isn't enough. Centering and the like put the canvas at a fraction
// of a screen pixel, and then it's resampled all the same, so it is also
// nudged onto the screen's pixel grid.
function keepPixelPerfect(canvas, Module) {
  let watched = null;

  function apply() {
    const ratio = window.devicePixelRatio || 1;
    const scale = Math.max(1, Math.round(ratio)) / ratio;
    canvas.style.width = `${canvas.width * scale}px`;
    canvas.style.height = `${canvas.height * scale}px`;
    // Only matters when a canvas pixel covers more than one screen pixel
    canvas.style.imageRendering = 'pixelated';

    canvas.style.transform = 'none';
    const rect = canvas.getBoundingClientRect();
    const nudge = (at) => (Math.round(at * ratio) - at * ratio) / ratio;
    canvas.style.transform = `translate(${nudge(rect.left)}px, ${nudge(rect.top)}px)`;

    // The ratio changes with browser zoom and when the window moves to
    // another display. A resolution query only fires on leaving the ratio it
    // names, so it's made again each time
    if (watched) watched.removeEventListener('change', apply);
    watched = window.matchMedia(`(resolution: ${ratio}dppx)`);
    watched.addEventListener('change', apply);
  }

  Module.onCanvasResized = apply;
  // Whatever moves the canvas on the page can take it off the grid again
  window.addEventListener('resize', apply);
  if (window.ResizeObserver && canvas.parentElement) {
    new ResizeObserver(apply).observe(canvas.parentElement);
  }
  apply();
}
