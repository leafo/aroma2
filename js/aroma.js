import wasmAromaModule from './wasm-aroma.js';
import { createTextureStore } from './texture-store.js';

/**
 * Initialize the Aroma WebAssembly runtime
 * @param {HTMLCanvasElement} canvas - The canvas element to render to
 * @returns {Promise<Object>} Promise that resolves with the initialized Aroma module
 */
export async function initAroma(canvas) {
  if (!canvas) {
    throw new Error('Canvas element is required');
  }

  const moduleConfig = {
    canvas,
    locateFile: (path) => `./${path}`,
  };

  let textureStore = null;

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
    Module.requestTextureLoad = (imagePtr, url) => {
      const attempt = () => {
        const store = ensureTextureStore(Module);
        if (!store) {
          setTimeout(attempt, 0);
          return;
        }
        store
          .load(url)
          .then(({ id, width, height }) => {
            Module._aroma_image_loaded(imagePtr, id, width, height);
          })
          .catch((err) => {
            console.error('Failed to load texture', url, err);
            Module._aroma_image_loaded(imagePtr, 0, 0, 0);
          });
      };

      attempt();
    };

    Module.bindTexture = (id) => {
      const store = ensureTextureStore(Module);
      if (!store || !store.bind(id)) {
        console.warn('Attempted to bind missing texture', id);
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
  canvas.addEventListener('keydown', (e) => {
    const key = mapKeyName(e.key, e.location);
    pressedKeys.add(key);
    if (Module._aroma_keypressed) {
      Module.ccall('aroma_keypressed', null, ['string'], [key]);
    }
  });

  // TODO: handle keyup separately to ensure key up isn't lost during focus change
  canvas.addEventListener('keyup', (e) => {
    const key = mapKeyName(e.key, e.location);
    pressedKeys.delete(key);
    if (Module._aroma_keyreleased) {
      Module.ccall('aroma_keyreleased', null, ['string'], [key]);
    }
  });

  return {
    module: Module,
    runCode: (code) => {
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
