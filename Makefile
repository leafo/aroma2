OUT_DIR := dist
TARGET_JS := js/wasm-aroma.js
WASM_OUT := $(TARGET_JS:.js=.wasm)
DATA_OUT := $(TARGET_JS:.js=.data)
HTML_OUT := $(OUT_DIR)/index.html
EMSCRIPTEN_ROOT := /usr/lib/emscripten
EMCC := $(EMSCRIPTEN_ROOT)/emcc
EM_CACHE := $(abspath $(OUT_DIR)/.emscripten_cache)
SHELL_FILE := shell.html
LUA_DIR := lua-5.2/src
LUA_CORE := \
	lapi.c lcode.c lctype.c ldebug.c ldo.c ldump.c lfunc.c lgc.c llex.c \
	lmem.c lobject.c lopcodes.c lparser.c lstate.c lstring.c ltable.c \
	ltm.c lundump.c lvm.c lzio.c
LUA_LIB := \
	lauxlib.c lbaselib.c lbitlib.c lcorolib.c ldblib.c liolib.c lmathlib.c \
	loslib.c lstrlib.c ltablib.c loadlib.c linit.c
SOURCES := main.c $(addprefix $(LUA_DIR)/,$(LUA_CORE) $(LUA_LIB))
ASSETS := hi.png

JS_SRCS := $(shell find js -type f -name '*.js' ! -name 'wasm-aroma.js')
BUNDLE := $(OUT_DIR)/app.js

CFLAGS := -O3 -s WASM=1 -s FULL_ES2=1 -s MIN_WEBGL_VERSION=1 -s MAX_WEBGL_VERSION=1 \
          -s ENVIRONMENT=web -s ALLOW_MEMORY_GROWTH=0 -s ASSERTIONS=0 \
          -s MODULARIZE=1 -s EXPORT_ES6=1 -s EXPORT_NAME=wasmAromaModule \
          -s EXPORTED_RUNTIME_METHODS=ccall \
          -I$(LUA_DIR)
LDFLAGS :=

.PHONY: all clean run

all: $(HTML_OUT) $(BUNDLE)

$(TARGET_JS): main.c | lua-5.2 js
	EM_CACHE=$(EM_CACHE) $(EMCC) $(SOURCES) $(CFLAGS) $(LDFLAGS) -o $@

$(BUNDLE): $(JS_SRCS) $(TARGET_JS) | $(OUT_DIR)
	esbuild js/shell.js --bundle --format=esm --platform=browser --outfile=$(BUNDLE)

$(HTML_OUT): $(SHELL_FILE) $(BUNDLE) $(TARGET_JS)
	cp $(SHELL_FILE) $(HTML_OUT)
	cp $(WASM_OUT) $(OUT_DIR)/
	@if [ -f $(DATA_OUT) ]; then cp $(DATA_OUT) $(OUT_DIR)/; fi
	cp $(ASSETS) $(OUT_DIR)/

js:
	mkdir -p js

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

clean:
	rm -rf $(OUT_DIR)
	rm -f js/wasm-aroma.js js/wasm-aroma.wasm js/wasm-aroma.data

lua-5.2:
	curl -O https://www.lua.org/ftp/lua-5.2.4.tar.gz
	tar -xzf lua-5.2.4.tar.gz
	mv lua-5.2.4 lua-5.2
