// Enough of the zip format for .love files: stored and deflated entries, no
// zip64 or encryption. Inflating is left to the browser.

const END_OF_DIRECTORY = 0x06054b50;
const DIRECTORY_ENTRY = 0x02014b50;
const LOCAL_HEADER = 0x04034b50;

async function inflate(bytes) {
  const stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream('deflate-raw'));
  return new Uint8Array(await new Response(stream).arrayBuffer());
}

/**
 * @param {ArrayBuffer} buffer
 * @returns {Promise<Map<string, Uint8Array>>} files by their path in the
 *   archive, directories left out
 */
export async function unzip(buffer) {
  const bytes = new Uint8Array(buffer);
  const view = new DataView(buffer);

  // The directory is found from the end, behind a comment of unknown length
  let end = bytes.length - 22;
  const lowest = Math.max(0, end - 0xffff);
  while (end >= lowest && view.getUint32(end, true) !== END_OF_DIRECTORY) {
    end--;
  }
  if (end < lowest) {
    throw new Error('Not a zip file');
  }

  const count = view.getUint16(end + 10, true);
  let at = view.getUint32(end + 16, true);
  if (at === 0xffffffff) {
    throw new Error('zip64 archives are not supported');
  }

  const decoder = new TextDecoder();
  const files = new Map();

  for (let i = 0; i < count; i++) {
    if (view.getUint32(at, true) !== DIRECTORY_ENTRY) {
      throw new Error('Corrupt zip directory');
    }
    const flags = view.getUint16(at + 8, true);
    const method = view.getUint16(at + 10, true);
    const compressedSize = view.getUint32(at + 20, true);
    const nameLength = view.getUint16(at + 28, true);
    const extraLength = view.getUint16(at + 30, true);
    const commentLength = view.getUint16(at + 32, true);
    const headerAt = view.getUint32(at + 42, true);
    const name = decoder.decode(bytes.subarray(at + 46, at + 46 + nameLength));
    at += 46 + nameLength + extraLength + commentLength;

    if (name.endsWith('/')) continue;
    if (flags & 1) {
      throw new Error(`${name} is encrypted`);
    }
    if (view.getUint32(headerAt, true) !== LOCAL_HEADER) {
      throw new Error(`Corrupt zip entry for ${name}`);
    }

    // The local header has its own extra field, sized apart from the
    // directory's
    const dataAt = headerAt + 30 + view.getUint16(headerAt + 26, true) + view.getUint16(headerAt + 28, true);
    const data = bytes.subarray(dataAt, dataAt + compressedSize);

    if (method === 0) {
      files.set(name, data);
    } else if (method === 8) {
      files.set(name, await inflate(data));
    } else {
      throw new Error(`${name} uses unsupported compression method ${method}`);
    }
  }

  return files;
}
