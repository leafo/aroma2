// A buffer source node can only be started once, so every play makes a new
// node. Clones share their decoded buffer.
export function createAudioStore() {
  const sources = new Map();
  let nextId = 1;
  let context = null;
  let master = null;
  let masterVolume = 1;
  let unlocked = false;

  function ensureContext() {
    if (!context) {
      const AudioContext = window.AudioContext || window.webkitAudioContext;
      if (!AudioContext) {
        throw new Error('WebAudio is not available');
      }
      context = new AudioContext();
      master = context.createGain();
      master.gain.value = masterVolume;
      master.connect(context.destination);
    }
    return context;
  }

  // Browsers keep a context suspended until the page has been interacted
  // with. Called from input events
  function unlock() {
    unlocked = true;
    if (context && context.state === 'suspended') {
      context.resume();
    }
  }

  function add(buffer) {
    const id = nextId++;
    sources.set(id, {
      buffer,
      volume: 1,
      pitch: 1,
      looping: false,
      node: null,
      gain: null,
      // Seconds into the buffer at startedAt, or where it's paused
      offset: 0,
      startedAt: 0,
    });
    return id;
  }

  async function load(blob) {
    const ctx = ensureContext();
    const buffer = await ctx.decodeAudioData(await blob.arrayBuffer());
    return { id: add(buffer), duration: buffer.duration };
  }

  function clone(id) {
    const source = sources.get(id);
    if (!source) return 0;
    const copy = add(source.buffer);
    Object.assign(sources.get(copy), {
      volume: source.volume,
      pitch: source.pitch,
      looping: source.looping,
    });
    return copy;
  }

  function position(source) {
    if (!source.node) return source.offset;
    const played = source.offset + (context.currentTime - source.startedAt) * source.pitch;
    const duration = source.buffer.duration;
    return source.looping ? played % duration : Math.min(played, duration);
  }

  function halt(source) {
    if (!source.node) return;
    source.node.onended = null;
    source.node.stop();
    source.node.disconnect();
    source.gain.disconnect();
    source.node = null;
    source.gain = null;
  }

  function play(id) {
    const source = sources.get(id);
    if (!source || source.node) return !!source;

    // Time doesn't pass in a suspended context, so everything started before
    // the first input would sound at once when it resumes. Loops are let
    // through since they're still wanted by then. After an input the resume
    // is only moments away and nothing is dropped
    if (context.state !== 'running' && !unlocked && !source.looping) {
      return false;
    }

    const node = context.createBufferSource();
    node.buffer = source.buffer;
    node.loop = source.looping;
    node.playbackRate.value = source.pitch;

    const gain = context.createGain();
    gain.gain.value = source.volume;
    node.connect(gain);
    gain.connect(master);

    node.onended = () => {
      if (source.node === node) {
        halt(source);
        source.offset = 0;
      }
    };

    source.node = node;
    source.gain = gain;
    source.startedAt = context.currentTime;
    node.start(0, source.offset % source.buffer.duration);
    return true;
  }

  function pause(id) {
    const source = sources.get(id);
    if (!source || !source.node) return;
    source.offset = position(source);
    halt(source);
  }

  function stop(id) {
    const source = sources.get(id);
    if (!source) return;
    halt(source);
    source.offset = 0;
  }

  function seek(id, seconds) {
    const source = sources.get(id);
    if (!source) return;
    const playing = !!source.node;
    halt(source);
    source.offset = Math.max(0, Math.min(seconds, source.buffer.duration));
    if (playing) play(id);
  }

  function setVolume(id, volume) {
    const source = sources.get(id);
    if (!source) return;
    source.volume = volume;
    if (source.gain) source.gain.gain.value = volume;
  }

  function setPitch(id, pitch) {
    const source = sources.get(id);
    if (!source) return;
    if (source.node) {
      // Settle the position at the old rate before the rate changes
      source.offset = position(source);
      source.startedAt = context.currentTime;
      source.node.playbackRate.value = pitch;
    }
    source.pitch = pitch;
  }

  function setLooping(id, looping) {
    const source = sources.get(id);
    if (!source) return;
    source.looping = looping;
    if (source.node) source.node.loop = looping;
  }

  function release(id) {
    const source = sources.get(id);
    if (!source) return;
    halt(source);
    sources.delete(id);
  }

  return {
    load,
    clone,
    play,
    pause,
    stop,
    seek,
    release,
    setVolume,
    setPitch,
    setLooping,
    unlock,
    isPlaying: (id) => !!(sources.get(id) && sources.get(id).node),
    tell: (id) => (sources.get(id) ? position(sources.get(id)) : 0),
    stopAll: () => sources.forEach((_, id) => stop(id)),
    setMasterVolume: (volume) => {
      masterVolume = volume;
      if (master) master.gain.value = volume;
    },
    // For tests and embedders that want to route or inspect the output
    getOutput: () => (ensureContext(), { context, master }),
  };
}
