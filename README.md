# Aroma

Aroma is a Lua-based WebAssembly graphics runtime with a simple API for
creating interactive 2D graphics. Code is executed by a Lua 5.2 interpreter
compiled to WebAssembly. The runtime is implemented in C and renders through
WebGL via Emscripten. The goal of this project is to offload as much work to
built in browser APIs as possible to keep the bundled wasm and js files as
small as possible.

This project is a continuation of Aroma from 2012 which is no longer functional
due to removed APIs: https://github.com/leafo/aroma

Aroma provides a `aroma.*` API and includes a `love.*` alias for compatibility with Love2D-style code.

## Instructions to build locally

### Prerequisites

- [Emscripten SDK](https://emscripten.org) with `emcc`
- [Node.js](https://nodejs.org/) with `esbuild` installed (`npm install --save-dev esbuild`).

### Build

```sh
make
```

The `Makefile` performs these steps:

1. Compiles Lua + the engine to modular ES-module output (`js/wasm-aroma.js`, `js/wasm-aroma.wasm`).
2. Bundles the browser entry point (`js/shell.js`) and helper modules with esbuild into `dist/app.js`.
3. Copies `shell.html` to `dist/index.html`, along with assets and examples.

## Instructions to build using Docker

### Prerequisites

- [Docker](https://www.docker.com/get-started/)
- [Node.js & NPM](https://nodejs.org/) optional\*

### Build

```sh
docker compose up --build 'aroma-builder'
```

alternatively if you have Node.js installed locally you can run:

```sh
npm run build:docker
```

This will create the `dist/` directory with the built files.

### Run

```sh
docker compose up --build 'aroma-server'
```

alternatively if you have Node.js installed locally you can run:

```sh
npm run start:docker
```

This will run a web server at `http://localhost:8080` .

## Project Layout

- `main.c` — engine glue: embeds Lua 5.2, exposes the `aroma.graphics` API (with `love.*` compatibility alias), and drives the render loop.
- `lua-5.2/` — vanilla Lua 5.2 source used to build the interpreter.
- `js/aroma.js` — programmatic API for initializing and interacting with the Aroma runtime.
- `js/shell.js` — shell UI code (code editor, run button, example selector).
- `js/texture-store.js` — WebGL texture management helper.
- `examples/` — Lua example files that can be loaded in the shell.
- `Makefile` — orchestrates both the wasm build and the JS bundle.
- `shell.html` — HTML wrapper with code editor and canvas, copied to `dist/index.html`.
