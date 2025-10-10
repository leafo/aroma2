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

  function createFromSource(source, opts) {
    return createTextureFromSource(source, opts);
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
    bind,
    release,
  };
}
