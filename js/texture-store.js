export function createTextureStore(gl) {
  const textures = new Map();
  let nextId = 1;

  function createTextureFromSource(source, opts = {}) {
    const tex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tex);

    const {
      flipY = true,
      mag = gl.NEAREST,
      min = gl.NEAREST,
      wrapS = gl.CLAMP_TO_EDGE,
      wrapT = gl.CLAMP_TO_EDGE,
    } = opts;

    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, flipY);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, mag);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, min);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, wrapS);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, wrapT);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, source);

    const width = source.width || source.videoWidth || source.naturalWidth || 0;
    const height = source.height || source.videoHeight || source.naturalHeight || 0;
    const id = nextId++;
    textures.set(id, { tex, width, height });
    return { id, width, height };
  }

  async function load(url, opts) {
    const response = await fetch(url);
    if (!response.ok) {
      throw new Error(`Failed to fetch ${url}: ${response.status}`);
    }
    const blob = await response.blob();
    const bitmap = await createImageBitmap(blob);
    return createTextureFromSource(bitmap, opts);
  }

  // RGBA bytes, top row first. Sampling is set afterwards through setParams
  function createFromPixels(pixels, width, height) {
    const tex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tex);
    // Unlike image bitmaps, typed arrays do get flipped and premultiplied
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
    gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, width, height, 0, gl.RGBA, gl.UNSIGNED_BYTE, pixels);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);

    const id = nextId++;
    textures.set(id, { tex, width, height });
    return id;
  }

  function createFromSource(source, opts) {
    return createTextureFromSource(source, opts);
  }

  const isPowerOfTwo = (n) => (n & (n - 1)) === 0;
  let warnedAboutWrap = false;

  // wrap modes are indexed clamp, repeat, mirroredrepeat
  function setParams(id, minNearest, magNearest, wrapH, wrapV) {
    const entry = textures.get(id);
    if (!entry) return;

    const wraps = [gl.CLAMP_TO_EDGE, gl.REPEAT, gl.MIRRORED_REPEAT];
    let wrapS = wraps[wrapH] ?? gl.CLAMP_TO_EDGE;
    let wrapT = wraps[wrapV] ?? gl.CLAMP_TO_EDGE;

    // WebGL 1 can only repeat textures with power of two sides, anything
    // else samples as black
    const canRepeat = isPowerOfTwo(entry.width) && isPowerOfTwo(entry.height);
    if (!canRepeat && (wrapS !== gl.CLAMP_TO_EDGE || wrapT !== gl.CLAMP_TO_EDGE)) {
      if (!warnedAboutWrap) {
        warnedAboutWrap = true;
        console.warn(`Texture wrap needs power of two sides, clamping a ${entry.width}x${entry.height} texture instead`);
      }
      wrapS = gl.CLAMP_TO_EDGE;
      wrapT = gl.CLAMP_TO_EDGE;
    }

    gl.bindTexture(gl.TEXTURE_2D, entry.tex);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, minNearest ? gl.NEAREST : gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, magNearest ? gl.NEAREST : gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, wrapS);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, wrapT);
  }

  function bind(id) {
    const entry = textures.get(id);
    if (!entry) return false;
    gl.bindTexture(gl.TEXTURE_2D, entry.tex);
    return true;
  }

  function release(id) {
    const entry = textures.get(id);
    if (!entry) return;
    gl.deleteTexture(entry.tex);
    textures.delete(id);
  }

  return {
    load,
    createFromSource,
    createFromPixels,
    setParams,
    bind,
    release,
  };
}
