#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include <GLES2/gl2.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STACK_MAX 32
#define MAX_GLYPHS 256
#define DEFAULT_WIDTH 800
#define DEFAULT_HEIGHT 600
/* requestAnimationFrame stops in a hidden tab, so the first frame back would
 * otherwise carry the whole absence as one step */
#define MAX_DT 0.1f

typedef struct {
    float m[9];
} Mat3;

typedef enum { FILTER_LINEAR, FILTER_NEAREST } FilterMode;
typedef enum { WRAP_CLAMP, WRAP_REPEAT, WRAP_MIRRORED_REPEAT } WrapMode;

static const char *const filter_names[] = {"linear", "nearest", NULL};
static const char *const wrap_names[] = {"clamp", "repeat", "mirroredrepeat", NULL};

/* Kept on the C side so the getters don't need the page, which only hears
 * about changes */
typedef struct {
    FilterMode min;
    FilterMode mag;
    WrapMode wrap_h;
    WrapMode wrap_v;
} TextureParams;

typedef struct {
    int texture_id;
    int width;
    int height;
    int loaded;
    TextureParams params;
    /* A Canvas shares this struct so that drawing and sampling treat the two
     * alike, only the metatable differs */
    int is_canvas;
} AromaImage;

/* RGBA bytes with the top row first */
typedef struct {
    int width;
    int height;
    unsigned char *pixels;
} AromaImageData;

/* The settings are mirrored here so the getters don't need the page */
typedef struct {
    int source_id;
    int loaded;
    double duration;
    float volume;
    float pitch;
    int looping;
} AromaSource;

#define MAX_JOYSTICKS 4
#define MAX_JOYSTICK_BUTTONS 32
#define MAX_JOYSTICK_AXES 8
/* One slot of what the page writes each frame: connected, mapped, button
 * count, axis count, then pressed flags, analog button values and axes */
#define JOYSTICK_STAGING_FLOATS (4 + MAX_JOYSTICK_BUTTONS * 2 + MAX_JOYSTICK_AXES)

#define GAMEPAD_BUTTONS 15
#define GAMEPAD_AXES 6
#define GAMEPAD_STICK_AXES 4
#define MAX_GAMEPAD_MAPPINGS 64

/* Where one of love's named gamepad inputs comes from in a pad's raw state */
typedef enum { BIND_NONE, BIND_BUTTON, BIND_AXIS, BIND_HAT } BindType;

typedef struct {
    BindType type;
    /* Raw button or axis from 0, or the hat */
    int index;
    /* BIND_HAT: the direction, 1 up 2 right 4 down 8 left as in SDL */
    int hat_mask;
    /* BIND_AXIS: the stretch of the raw axis that is used, from rest to full.
     * A whole axis is -1 to 1, SDL's +a2 is 0 to 1 and a2~ is 1 to -1 */
    float from;
    float to;
} Binding;

/* An SDL style mapping from raw inputs to the gamepad layout, for pads the
 * browser doesn't map itself. They're matched to pads by USB ids */
typedef struct {
    int vendor;
    int product;
    Binding buttons[GAMEPAD_BUTTONS];
    Binding axes[GAMEPAD_AXES];
} GamepadMapping;

/* A slot of navigator.getGamepads(). The state is diffed against what the
 * page reports each frame to send love's joystick callbacks */
typedef struct {
    int connected;
    /* What makes it a Gamepad: the browser's "standard" layout, or else a
     * mapping of ours for its USB ids. NULL for a plain joystick */
    const GamepadMapping *mapping;
    int vendor;
    int product;
    /* Set when the mapping reads a hat, which browsers deliver as the last
     * two axes. They're then reported as a hat and not as axes, like SDL */
    int has_hat;
    /* Axes that have moved. Browsers report 0 for an axis until its first
     * event, which for a trigger resting at -1 would read as half pulled */
    uint32_t axes_seen;
    uint32_t gamepad_pressed;
    float gamepad_axes[GAMEPAD_AXES];
    /* Tells apart pads that come and go in one slot, a Joystick object is
     * only live while its id matches */
    int instance_id;
    int object_ref;
    char name[128];
    int button_count;
    int axis_count;
    uint32_t pressed;
    float values[MAX_JOYSTICK_BUTTONS];
    float axes[MAX_JOYSTICK_AXES];
} JoystickSlot;

typedef struct {
    int slot;
    int instance_id;
} AromaJoystick;

#define MESH_VERTEX_FLOATS 8

/* Vertices are x, y, u, v, r, g, b, a. The copy in vertices is what getVertex
 * reads and setVertex uploads from. The texture is anchored by the
 * userdata's uservalue */
typedef struct {
    GLuint buffer;
    GLenum mode;
    int count;
    float *vertices;
    AromaImage *texture;
} AromaMesh;

/* sw, sh are the size of the texture that x, y, w, h were measured against */
typedef struct {
    float x, y, w, h;
    float sw, sh;
} AromaQuad;

typedef struct {
    int x;           // x position in texture
    int width;       // width of glyph
    uint32_t codepoint; // character code
} GlyphInfo;

typedef struct {
    int texture_id;
    int image_width;
    int image_height;
    GlyphInfo glyphs[MAX_GLYPHS];
    int glyph_count;
    float extra_spacing;
    /* Multiple of the font's height that a line advances by */
    float line_height;
    int loaded;
    TextureParams params;
} AromaFont;

typedef enum {
    BLEND_ALPHA, BLEND_ADD, BLEND_SUBTRACT, BLEND_MULTIPLY, BLEND_REPLACE, BLEND_SCREEN
} BlendMode;
typedef enum { BLEND_ALPHA_MULTIPLY, BLEND_PREMULTIPLIED } BlendAlphaMode;

static const char *const blend_mode_names[] = {"alpha", "add", "subtract", "multiply", "replace", "screen", NULL};
static const char *const blend_alpha_names[] = {"alphamultiply", "premultiplied", NULL};

/* What push("all") saves next to the transform. The font and canvas are
 * anchored in the registry by their refs for as long as they're in here */
typedef struct {
    float draw_color[4];
    float bg_color[4];
    float line_width;
    AromaFont *font;
    int font_ref;
    /* NULL draws to the window */
    AromaImage *canvas;
    int canvas_ref;
    BlendMode blend_mode;
    BlendAlphaMode blend_alpha;
    int scissor_enabled;
    /* In the pixels of the target with y down, not GL's */
    int scissor[4];
} GraphicsState;

typedef struct {
    Mat3 matrix;
    /* Set for push("all"), pop then puts saved back */
    int has_state;
    GraphicsState saved;
} StackEntry;

typedef enum {
    SCRIPT_ENTRY_NONE,
    SCRIPT_ENTRY_BOOTSTRAP,
    SCRIPT_ENTRY_UPDATE,
    SCRIPT_ENTRY_DRAW,
    SCRIPT_ENTRY_KEY_EVENT,
    SCRIPT_ENTRY_MOUSE_EVENT,
    SCRIPT_ENTRY_JOYSTICK_EVENT,
    SCRIPT_ENTRY_FOCUS,
    SCRIPT_ENTRY_QUIT
} ScriptEntryPoint;

typedef struct {
    lua_State *L;
    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE gl_context;
    GLuint program;
    GLuint vbo;
    GLint transform_loc;
    GLint projection_loc;
    GLint color_loc;
    GLint use_texture_loc;
    GLint sampler_loc;
    float projection[9];
    GraphicsState gfx;
    StackEntry stack[STACK_MAX];
    int stack_top;
    double last_time;
    double start_time;
    float dt;
    /* Frames per second, refreshed once a second like love's timer */
    int fps;
    int fps_frames;
    double fps_window_start;
    int window_width;
    int window_height;
    /* Guards async load completions against a Lua state that was closed and
     * recreated while the load was in flight. */
    int generation;
    /* Coroutine the current script entry point (chunk, load, update, draw,
     * key events) runs on; non-NULL while suspended on a resource load. */
    lua_State *script_thread;
    int script_thread_ref;
    /* Coroutine cached between entry points so regular frames reuse one
     * thread instead of allocating per callback. */
    lua_State *idle_thread;
    int idle_thread_ref;
    ScriptEntryPoint script_entry_point;
    /* A draw that resumes from an async load is only being drained so Lua can
     * reach the next load/completion. The next animation frame renders a fresh
     * pass after the coroutine finishes. */
    int discard_rendering;
    /* Set by resource constructors just before they yield. A yield without it
     * came from user code and has no completion to resume it. */
    int resource_wait;
    /* Mouse state is pushed in by the page's events so reads from Lua don't
     * cross into JS. Buttons are a bitmask, love's button n is bit n - 1 */
    FilterMode default_min;
    FilterMode default_mag;
    int mouse_x;
    int mouse_y;
    unsigned int mouse_buttons;
    int mouse_hidden;
    float master_volume;
    JoystickSlot joysticks[MAX_JOYSTICKS];
    /* Mappings the script added with setGamepadMapping or
     * loadGamepadMappings, looked up before the built in ones */
    GamepadMapping user_mappings[MAX_GAMEPAD_MAPPINGS];
    int user_mapping_count;
    int next_joystick_instance;
    /* Off by default like love: held keys send one keypressed */
    int key_repeat;
    /* Set from inside Lua, the state is torn down once the frame unwinds */
    int quit_requested;
} EngineState;

static EngineState g_state;

static void poll_joysticks(void);
static void apply_render_target(void);
static void apply_scissor(void);
static void apply_blend_mode(void);

static const char *vertex_shader_source =
    "attribute vec2 aPosition;\n"
    "attribute vec2 aTexCoord;\n"
    "attribute vec4 aColor;\n"
    "uniform mat3 uTransform;\n"
    "uniform mat3 uProjection;\n"
    "varying vec2 vTexCoord;\n"
    "varying vec4 vColor;\n"
    "void main() {\n"
    "  vColor = aColor;\n"
    "  vec3 world = uTransform * vec3(aPosition, 1.0);\n"
    "  vec3 clip = uProjection * world;\n"
    "  gl_Position = vec4(clip.xy, 0.0, 1.0);\n"
    "  vTexCoord = aTexCoord;\n"
    "}\n";

/* mediump can be 16 bits, too few to tell the texels of a wide texture apart:
 * glyphs from a long font strip come out mangled */
static const char *fragment_shader_source =
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
    "precision highp float;\n"
    "#else\n"
    "precision mediump float;\n"
    "#endif\n"
    "uniform vec4 uColor;\n"
    "uniform sampler2D uTexture;\n"
    "uniform int uUseTexture;\n"
    "varying vec2 vTexCoord;\n"
    "varying vec4 vColor;\n"
    "void main() {\n"
    "  vec4 tex = uUseTexture == 1 ? texture2D(uTexture, vTexCoord) : vec4(1.0);\n"
    "  gl_FragColor = tex * uColor * vColor;\n"
    "}\n";

static void mat3_identity(Mat3 *m) {
    memset(m->m, 0, sizeof(m->m));
    m->m[0] = 1.0f;
    m->m[4] = 1.0f;
    m->m[8] = 1.0f;
}

static void mat3_copy(Mat3 *dst, const Mat3 *src) {
    memcpy(dst->m, src->m, sizeof(dst->m));
}

static void mat3_multiply(Mat3 *out, const Mat3 *a, const Mat3 *b) {
    Mat3 result;
    for (int col = 0; col < 3; ++col) {
        for (int row = 0; row < 3; ++row) {
            result.m[col * 3 + row] =
                a->m[0 * 3 + row] * b->m[col * 3 + 0] +
                a->m[1 * 3 + row] * b->m[col * 3 + 1] +
                a->m[2 * 3 + row] * b->m[col * 3 + 2];
        }
    }
    mat3_copy(out, &result);
}

static void mat3_translate(Mat3 *m, float tx, float ty) {
    Mat3 t;
    mat3_identity(&t);
    t.m[6] = tx;
    t.m[7] = ty;
    mat3_multiply(m, m, &t);
}

static void mat3_rotate(Mat3 *m, float angle) {
    float c = cosf(angle);
    float s = sinf(angle);
    Mat3 r;
    mat3_identity(&r);
    r.m[0] = c;
    r.m[1] = s;
    r.m[3] = -s;
    r.m[4] = c;
    mat3_multiply(m, m, &r);
}

static void mat3_scale(Mat3 *m, float sx, float sy) {
    Mat3 s;
    mat3_identity(&s);
    s.m[0] = sx;
    s.m[4] = sy;
    mat3_multiply(m, m, &s);
}

/* The transform love's draw calls take, ox, oy being the point of the object
 * that lands on x, y */
static void mat3_local(Mat3 *out, const Mat3 *base, float x, float y, float r, float sx, float sy, float ox, float oy) {
    Mat3 local;
    mat3_identity(&local);
    mat3_translate(&local, x, y);
    if (r != 0.0f) {
        mat3_rotate(&local, r);
    }
    mat3_scale(&local, sx, sy);
    if (ox != 0.0f || oy != 0.0f) {
        mat3_translate(&local, -ox, -oy);
    }
    mat3_multiply(out, base, &local);
}

EM_JS(void, js_request_texture_load, (int generation, uintptr_t image_ptr, const char *path), {
  if (Module.requestTextureLoad) {
    const url = UTF8ToString(path);
    Module.requestTextureLoad(generation, image_ptr, url);
  } else {
    console.error('Module.requestTextureLoad is not defined');
    /* Deferred so the completion lands after the requester has yielded */
    setTimeout(() => Module._aroma_image_loaded(generation, image_ptr, 0, 0, 0), 0);
  }
});

EM_JS(void, js_bind_texture, (int texture_id), {
  Module.bindTexture(texture_id);
});

EM_JS(int, js_create_texture_from_pixels, (const unsigned char *pixels, int width, int height), {
  return Module.createTextureFromPixels(HEAPU8.subarray(pixels, pixels + width * height * 4), width, height);
});

/* Fills the staging buffer from navigator.getGamepads(). Chrome only updates
 * a pad's state when it's asked for, so this runs every frame */
EM_JS(void, js_poll_gamepads, (float *staging, int slots, int slot_floats, int max_buttons, int max_axes), {
  const pads = navigator.getGamepads ? navigator.getGamepads() : [];
  const out = HEAPF32.subarray(staging >> 2, (staging >> 2) + slots * slot_floats);
  out.fill(0);
  for (let slot = 0; slot < slots; slot++) {
    const pad = pads[slot];
    if (!pad || !pad.connected) continue;
    const base = slot * slot_floats;
    const buttons = Math.min(pad.buttons.length, max_buttons);
    const axes = Math.min(pad.axes.length, max_axes);
    out[base] = 1;
    out[base + 1] = pad.mapping === "standard" ? 1 : 0;
    out[base + 2] = buttons;
    out[base + 3] = axes;
    for (let i = 0; i < buttons; i++) {
      out[base + 4 + i] = pad.buttons[i].pressed ? 1 : 0;
      out[base + 4 + max_buttons + i] = pad.buttons[i].value;
    }
    for (let i = 0; i < axes; i++) {
      out[base + 4 + max_buttons * 2 + i] = pad.axes[i];
    }
  }
});

/* Raw button and axis numbers differ by operating system, so SDL's mappings
 * each name the one they're for. 0 Linux, 1 Windows, 2 Mac OS X, -1 other */
EM_JS(int, js_gamepad_platform, (void), {
  const ua = navigator.userAgent || "";
  if (/Android/.test(ua)) return -1;
  if (/Linux|CrOS/.test(ua)) return 0;
  if (/Windows/.test(ua)) return 1;
  if (/Mac OS X|Macintosh/.test(ua)) return 2;
  return -1;
});

EM_JS(void, js_gamepad_name, (int slot, char *buffer, int size), {
  const pad = navigator.getGamepads ? navigator.getGamepads()[slot] : null;
  stringToUTF8(pad ? pad.id : "", buffer, size);
});

EM_JS(void, js_request_audio_load, (int generation, uintptr_t source_ptr, const char *path), {
  Module.requestAudioLoad(generation, source_ptr, UTF8ToString(path));
});

EM_JS(int, js_audio_clone, (int source_id), {
  return Module.audio.clone(source_id);
});

EM_JS(int, js_audio_play, (int source_id), {
  return Module.audio.play(source_id) ? 1 : 0;
});

EM_JS(void, js_audio_pause, (int source_id), {
  Module.audio.pause(source_id);
});

EM_JS(void, js_audio_stop, (int source_id), {
  Module.audio.stop(source_id);
});

EM_JS(void, js_audio_stop_all, (void), {
  Module.audio.stopAll();
});

EM_JS(int, js_audio_is_playing, (int source_id), {
  return Module.audio.isPlaying(source_id) ? 1 : 0;
});

EM_JS(double, js_audio_tell, (int source_id), {
  return Module.audio.tell(source_id);
});

EM_JS(void, js_audio_seek, (int source_id, double seconds), {
  Module.audio.seek(source_id, seconds);
});

EM_JS(void, js_audio_set_volume, (int source_id, double volume), {
  Module.audio.setVolume(source_id, volume);
});

EM_JS(void, js_audio_set_pitch, (int source_id, double pitch), {
  Module.audio.setPitch(source_id, pitch);
});

EM_JS(void, js_audio_set_looping, (int source_id, int looping), {
  Module.audio.setLooping(source_id, !!looping);
});

EM_JS(void, js_audio_release, (int source_id), {
  Module.audio.release(source_id);
});

EM_JS(void, js_audio_set_master_volume, (double volume), {
  Module.audio.setMasterVolume(volume);
});

EM_JS(int, js_create_canvas, (int width, int height), {
  return Module.createCanvasTexture(width, height);
});

/* 0 is the window */
EM_JS(void, js_bind_framebuffer, (int texture_id), {
  Module.bindFramebuffer(texture_id);
});

EM_JS(void, js_set_texture_params, (int texture_id, int min_nearest, int mag_nearest, int wrap_h, int wrap_v), {
  Module.setTextureParams(texture_id, min_nearest, mag_nearest, wrap_h, wrap_v);
});

EM_JS(void, js_release_texture, (int texture_id), {
  Module.releaseTexture(texture_id);
});

EM_JS(int, js_is_key_down, (const char *key), {
  if (Module.isKeyDown) {
    const keyName = UTF8ToString(key);
    return Module.isKeyDown(keyName) ? 1 : 0;
  }
  return 0;
});

EM_JS(void, js_request_font_load, (int generation, uintptr_t font_ptr, const char *path, const char *glyphs_str, double extra_spacing), {
  if (Module.requestFontLoad) {
    const url = UTF8ToString(path);
    const glyphs = UTF8ToString(glyphs_str);
    Module.requestFontLoad(generation, font_ptr, url, glyphs, extra_spacing);
  } else {
    console.error('Module.requestFontLoad is not defined');
    setTimeout(() => Module._aroma_font_set_glyphs(generation, font_ptr, 0, 0, 0, 0), 0);
  }
});

EM_JS(int, js_desktop_width, (void), {
  return Module.getDesktopDimensions ? Module.getDesktopDimensions()[0] : window.screen.width;
});

EM_JS(int, js_desktop_height, (void), {
  return Module.getDesktopDimensions ? Module.getDesktopDimensions()[1] : window.screen.height;
});

EM_JS(void, js_set_title, (const char *title), {
  const text = UTF8ToString(title);
  if (Module.setTitle) {
    Module.setTitle(text);
  } else {
    document.title = text;
  }
});

EM_JS(void, js_canvas_resized, (void), {
  if (Module.onCanvasResized) {
    Module.onCanvasResized();
  }
});

EM_JS(void, js_set_cursor_visible, (int visible), {
  Module.canvas.style.cursor = visible ? "" : "none";
});

EM_JS(void, js_on_quit, (void), {
  if (Module.onQuit) {
    Module.onQuit();
  }
});

// UTF-8 decoder for print function
static uint32_t utf8_decode(const char **str) {
    const unsigned char *s = (const unsigned char *)*str;
    uint32_t codepoint = 0;
    int len = 0;

    if (s[0] < 0x80) {
        codepoint = s[0];
        len = 1;
    } else if ((s[0] & 0xE0) == 0xC0) {
        codepoint = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);
        len = 2;
    } else if ((s[0] & 0xF0) == 0xE0) {
        codepoint = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        len = 3;
    } else if ((s[0] & 0xF8) == 0xF0) {
        codepoint = ((s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        len = 4;
    } else {
        codepoint = '?';
        len = 1;
    }

    *str += len;
    return codepoint;
}

static void parse_color(lua_State *L, int idx, float out[4]) {
    double r, g, b, a;
    int has_alpha;

    if (lua_istable(L, idx)) {
        int len = (int)lua_rawlen(L, idx);
        has_alpha = len >= 4;
        lua_rawgeti(L, idx, 1);
        lua_rawgeti(L, idx, 2);
        lua_rawgeti(L, idx, 3);
        r = luaL_checknumber(L, -3);
        g = luaL_checknumber(L, -2);
        b = luaL_checknumber(L, -1);
        lua_pop(L, 3);
        a = 1.0;
        if (has_alpha) {
            lua_rawgeti(L, idx, 4);
            a = luaL_checknumber(L, -1);
            lua_pop(L, 1);
        }
    } else {
        int count = lua_gettop(L) - idx + 1;
        has_alpha = count >= 4;
        r = luaL_checknumber(L, idx + 0);
        g = luaL_checknumber(L, idx + 1);
        b = luaL_checknumber(L, idx + 2);
        a = has_alpha ? luaL_checknumber(L, idx + 3) : 1.0;
    }

    if (r > 1.0 || g > 1.0 || b > 1.0 || a > 1.0) {
        r /= 255.0;
        g /= 255.0;
        b /= 255.0;
        /* A defaulted alpha is already opaque in 0..1, only rescale a given one */
        if (has_alpha) {
            a /= 255.0;
        }
    }

    out[0] = (float)r;
    out[1] = (float)g;
    out[2] = (float)b;
    out[3] = (float)a;
}

static Mat3 *current_matrix(void) {
    return &g_state.stack[g_state.stack_top].matrix;
}

static AromaImage *check_image(lua_State *L, int idx) {
    return (AromaImage *)luaL_checkudata(L, idx, "aroma.image");
}

static AromaImage *check_texture(lua_State *L, int idx) {
    AromaImage *img = (AromaImage *)luaL_testudata(L, idx, "aroma.image");
    if (!img) {
        img = (AromaImage *)luaL_testudata(L, idx, "aroma.canvas");
    }
    if (!img) {
        luaL_argerror(L, idx, "Image or Canvas expected");
    }
    return img;
}

static AromaQuad *check_quad(lua_State *L, int idx) {
    return (AromaQuad *)luaL_checkudata(L, idx, "aroma.quad");
}

static void apply_texture_params(int texture_id, const TextureParams *params) {
    if (texture_id) {
        js_set_texture_params(texture_id, params->min == FILTER_NEAREST, params->mag == FILTER_NEAREST,
                              params->wrap_h, params->wrap_v);
    }
}

static void default_texture_params(TextureParams *params) {
    params->min = g_state.default_min;
    params->mag = g_state.default_mag;
    params->wrap_h = WRAP_CLAMP;
    params->wrap_v = WRAP_CLAMP;
}

static int set_filter(lua_State *L, int texture_id, TextureParams *params) {
    params->min = (FilterMode)luaL_checkoption(L, 2, NULL, filter_names);
    params->mag = (FilterMode)luaL_checkoption(L, 3, filter_names[params->min], filter_names);
    apply_texture_params(texture_id, params);
    return 0;
}

static int get_filter(lua_State *L, const TextureParams *params) {
    lua_pushstring(L, filter_names[params->min]);
    lua_pushstring(L, filter_names[params->mag]);
    lua_pushinteger(L, 1); /* anisotropy, not supported */
    return 3;
}

static int set_wrap(lua_State *L, int texture_id, TextureParams *params) {
    params->wrap_h = (WrapMode)luaL_checkoption(L, 2, NULL, wrap_names);
    params->wrap_v = (WrapMode)luaL_checkoption(L, 3, wrap_names[params->wrap_h], wrap_names);
    apply_texture_params(texture_id, params);
    return 0;
}

static int get_wrap(lua_State *L, const TextureParams *params) {
    lua_pushstring(L, wrap_names[params->wrap_h]);
    lua_pushstring(L, wrap_names[params->wrap_v]);
    return 2;
}

/* The names an object answers to are the upvalues, its own type first */
static int l_object_type(lua_State *L) {
    lua_pushvalue(L, lua_upvalueindex(1));
    return 1;
}

static int l_object_typeOf(lua_State *L) {
    const char *name = luaL_checkstring(L, 2);
    for (int i = 1; !lua_isnone(L, lua_upvalueindex(i)); i++) {
        if (strcmp(name, lua_tostring(L, lua_upvalueindex(i))) == 0) {
            lua_pushboolean(L, 1);
            return 1;
        }
    }
    lua_pushboolean(L, 0);
    return 1;
}

/* Adds type and typeOf to the method table on top of the stack. names runs
 * from the object's own type up its parents and ends in NULL */
static void register_object_type(lua_State *L, const char *const *names) {
    int count = 0;
    for (; names[count]; count++) {
        lua_pushstring(L, names[count]);
    }
    lua_pushcclosure(L, l_object_typeOf, count);
    lua_setfield(L, -2, "typeOf");

    lua_pushstring(L, names[0]);
    lua_pushcclosure(L, l_object_type, 1);
    lua_setfield(L, -2, "type");
}

static AromaFont *check_font(lua_State *L, int idx) {
    return (AromaFont *)luaL_checkudata(L, idx, "aroma.font");
}

static int l_graphics_setBackgroundColor(lua_State *L) {
    parse_color(L, 1, g_state.gfx.bg_color);
    return 0;
}

static int l_graphics_getBackgroundColor(lua_State *L) {
    for (int i = 0; i < 4; i++) {
        lua_pushnumber(L, g_state.gfx.bg_color[i]);
    }
    return 4;
}

static int l_graphics_getColor(lua_State *L) {
    for (int i = 0; i < 4; i++) {
        lua_pushnumber(L, g_state.gfx.draw_color[i]);
    }
    return 4;
}

static int l_graphics_setColor(lua_State *L) {
    parse_color(L, 1, g_state.gfx.draw_color);
    return 0;
}

static int l_graphics_push(lua_State *L) {
    static const char *const kinds[] = {"transform", "all", NULL};
    int all = luaL_checkoption(L, 1, "transform", kinds);

    if (g_state.stack_top + 1 >= STACK_MAX) {
        return luaL_error(L, "love.graphics.push: stack overflow");
    }

    StackEntry *entry = &g_state.stack[g_state.stack_top + 1];
    mat3_copy(&entry->matrix, current_matrix());
    entry->has_state = all;
    if (all) {
        entry->saved = g_state.gfx;
        /* The saved font needs an anchor of its own, setFont releases the
         * active one */
        if (g_state.gfx.font_ref != LUA_NOREF) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, g_state.gfx.font_ref);
            entry->saved.font_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        }
        if (g_state.gfx.canvas_ref != LUA_NOREF) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, g_state.gfx.canvas_ref);
            entry->saved.canvas_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        }
    }
    g_state.stack_top++;
    return 0;
}

static void release_font_ref(lua_State *L) {
    if (g_state.gfx.font_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, g_state.gfx.font_ref);
        g_state.gfx.font_ref = LUA_NOREF;
    }
}

static void release_canvas_ref(lua_State *L) {
    if (g_state.gfx.canvas_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, g_state.gfx.canvas_ref);
        g_state.gfx.canvas_ref = LUA_NOREF;
    }
}

static void pop_stack_entry(lua_State *L) {
    StackEntry *entry = &g_state.stack[g_state.stack_top];
    if (entry->has_state) {
        release_font_ref(L);
        release_canvas_ref(L);
        g_state.gfx = entry->saved;
        entry->has_state = 0;
        apply_render_target();
        apply_blend_mode();
    }
    g_state.stack_top--;
}

static int l_graphics_pop(lua_State *L) {
    if (g_state.stack_top == 0) {
        return luaL_error(L, "love.graphics.pop: stack underflow");
    }
    pop_stack_entry(L);
    return 0;
}

static int l_graphics_origin(lua_State *L) {
    (void)L;
    mat3_identity(current_matrix());
    return 0;
}

static int l_graphics_scale(lua_State *L) {
    float sx = (float)luaL_checknumber(L, 1);
    float sy = (float)luaL_optnumber(L, 2, sx);
    mat3_scale(current_matrix(), sx, sy);
    return 0;
}

static int l_graphics_translate(lua_State *L) {
    float tx = (float)luaL_checknumber(L, 1);
    float ty = (float)luaL_checknumber(L, 2);
    mat3_translate(current_matrix(), tx, ty);
    return 0;
}

static int l_graphics_rotate(lua_State *L) {
    float angle = (float)luaL_checknumber(L, 1);
    mat3_rotate(current_matrix(), angle);
    return 0;
}

/* Sets up the shader for a draw with the current color. texture_id 0 draws
 * untextured */
static void begin_draw(const Mat3 *transform, int texture_id) {
    glUseProgram(g_state.program);
    glUniformMatrix3fv(g_state.transform_loc, 1, GL_FALSE, transform->m);
    glUniformMatrix3fv(g_state.projection_loc, 1, GL_FALSE, g_state.projection);
    glUniform4fv(g_state.color_loc, 1, g_state.gfx.draw_color);
    if (g_state.use_texture_loc >= 0) {
        glUniform1i(g_state.use_texture_loc, texture_id ? 1 : 0);
    }

    if (texture_id) {
        glActiveTexture(GL_TEXTURE0);
        js_bind_texture(texture_id);
        if (g_state.sampler_loc >= 0) {
            glUniform1i(g_state.sampler_loc, 0);
        }
    }
}

/* Vertex scratch space shared by the shape functions, grown on demand and
 * kept. Shapes are built and drawn within one call so nothing overlaps */
static float *g_scratch[2];
static int g_scratch_capacity[2];

static float *scratch_floats(lua_State *L, int slot, int count) {
    if (count > g_scratch_capacity[slot]) {
        float *grown = (float *)realloc(g_scratch[slot], sizeof(float) * count);
        if (!grown) {
            luaL_error(L, "out of memory");
        }
        g_scratch[slot] = grown;
        g_scratch_capacity[slot] = count;
    }
    return g_scratch[slot];
}

static void draw_solid(const float *coords, int points, GLenum mode) {
    if (g_state.discard_rendering || points <= 0) {
        return;
    }

    begin_draw(current_matrix(), 0);
    glDisableVertexAttribArray(1);
    glVertexAttrib2f(1, 0.0f, 0.0f);

    glBindBuffer(GL_ARRAY_BUFFER, g_state.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * points * 2, coords, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (const void *)0);

    glDrawArrays(mode, 0, points);
}

/* How far a miter may reach past the line's half width before it's cut
 * short, the tip of a very sharp corner would otherwise run off to infinity */
#define MITER_LIMIT 8.0f

/* WebGL only has 1 pixel lines, so lines are built as a triangle strip with
 * mitered corners. The width is in the units of the current transform, like
 * love's */
static void draw_polyline(lua_State *L, const float *coords, int points, int closed) {
    /* Closing point given twice, as love's polygon vertices allow */
    if (closed && points > 1 &&
        coords[0] == coords[(points - 1) * 2] && coords[1] == coords[(points - 1) * 2 + 1]) {
        points--;
    }

    if (points < 2) {
        return;
    }

    if (points == 2) {
        closed = 0;
    }

    float half = g_state.gfx.line_width * 0.5f;
    int pairs = closed ? points + 1 : points;
    float *strip = scratch_floats(L, 1, pairs * 4);

    for (int n = 0; n < pairs; n++) {
        int i = n % points;
        int has_prev = closed || i > 0;
        int has_next = closed || i < points - 1;
        const float *p = &coords[i * 2];
        const float *prev = &coords[((i + points - 1) % points) * 2];
        const float *next = &coords[((i + 1) % points) * 2];

        /* Unit normals of the segments into and out of this point */
        float in_x = 0.0f, in_y = 0.0f, out_x = 0.0f, out_y = 0.0f;
        if (has_prev) {
            float dx = p[0] - prev[0], dy = p[1] - prev[1];
            float len = sqrtf(dx * dx + dy * dy);
            if (len > 0.0f) { in_x = -dy / len; in_y = dx / len; } else { has_prev = 0; }
        }
        if (has_next) {
            float dx = next[0] - p[0], dy = next[1] - p[1];
            float len = sqrtf(dx * dx + dy * dy);
            if (len > 0.0f) { out_x = -dy / len; out_y = dx / len; } else { has_next = 0; }
        }
        if (!has_prev) { in_x = out_x; in_y = out_y; }
        if (!has_next) { out_x = in_x; out_y = in_y; }

        float mx = in_x + out_x, my = in_y + out_y;
        float mlen = sqrtf(mx * mx + my * my);
        float reach = half;
        if (mlen > 0.0001f) {
            mx /= mlen;
            my /= mlen;
            float cos_half = mx * in_x + my * in_y;
            reach = cos_half > 1.0f / MITER_LIMIT ? half / cos_half : half * MITER_LIMIT;
        } else {
            /* Doubles straight back on itself */
            mx = in_x;
            my = in_y;
        }

        strip[n * 4 + 0] = p[0] + mx * reach;
        strip[n * 4 + 1] = p[1] + my * reach;
        strip[n * 4 + 2] = p[0] - mx * reach;
        strip[n * 4 + 3] = p[1] - my * reach;
    }

    draw_solid(strip, pairs * 2, GL_TRIANGLE_STRIP);
}

static int check_draw_mode(lua_State *L, int idx) {
    static const char *const modes[] = {"fill", "line", NULL};
    return luaL_checkoption(L, idx, NULL, modes);
}

/* x, y pairs as arguments from idx on, or as one table there. The
 * coordinates land in scratch slot 0 */
static int read_points(lua_State *L, int idx, const char *what, float **out) {
    int from_table = lua_istable(L, idx);
    int count = from_table ? (int)lua_rawlen(L, idx) : lua_gettop(L) - idx + 1;

    if (count % 2 != 0) {
        luaL_error(L, "%s: coordinates must come in pairs", what);
    }

    float *coords = scratch_floats(L, 0, count > 0 ? count : 1);
    for (int i = 0; i < count; i++) {
        if (from_table) {
            lua_rawgeti(L, idx, i + 1);
            coords[i] = (float)luaL_checknumber(L, -1);
            lua_pop(L, 1);
        } else {
            coords[i] = (float)luaL_checknumber(L, idx + i);
        }
    }

    *out = coords;
    return count / 2;
}

/* Filled polygons are drawn as a fan and so have to be convex, as in love */
static void draw_outline(lua_State *L, int fill, const float *coords, int points) {
    if (fill) {
        draw_solid(coords, points, GL_TRIANGLE_FAN);
    } else {
        draw_polyline(L, coords, points, 1);
    }
}

static int l_graphics_polygon(lua_State *L) {
    int fill = check_draw_mode(L, 1) == 0;
    float *coords;
    int points = read_points(L, 2, "love.graphics.polygon", &coords);
    if (points < 3) {
        return luaL_error(L, "love.graphics.polygon: need at least 3 points");
    }
    draw_outline(L, fill, coords, points);
    return 0;
}

static int l_graphics_line(lua_State *L) {
    float *coords;
    int points = read_points(L, 1, "love.graphics.line", &coords);
    if (points < 2) {
        return luaL_error(L, "love.graphics.line: need at least 2 points");
    }
    draw_polyline(L, coords, points, 0);
    return 0;
}

/* Segments for a full turn of a curve, more as it gets bigger on screen */
static int curve_segments(float rx, float ry) {
    const float *m = current_matrix()->m;
    float scale = sqrtf(fabsf(m[0] * m[4] - m[1] * m[3]));
    int segments = (int)sqrtf((fabsf(rx) + fabsf(ry)) * 0.5f * 20.0f * scale);
    return segments < 8 ? 8 : segments;
}

static void draw_ellipse(lua_State *L, int fill, float x, float y, float rx, float ry, int segments) {
    if (segments < 3) {
        segments = 3;
    }

    float *coords = scratch_floats(L, 0, segments * 2);
    for (int i = 0; i < segments; i++) {
        float angle = (float)i / segments * 2.0f * (float)M_PI;
        coords[i * 2 + 0] = x + cosf(angle) * rx;
        coords[i * 2 + 1] = y + sinf(angle) * ry;
    }
    draw_outline(L, fill, coords, segments);
}

static int l_graphics_circle(lua_State *L) {
    int fill = check_draw_mode(L, 1) == 0;
    float x = (float)luaL_checknumber(L, 2);
    float y = (float)luaL_checknumber(L, 3);
    float radius = (float)luaL_checknumber(L, 4);
    int segments = (int)luaL_optinteger(L, 5, curve_segments(radius, radius));
    draw_ellipse(L, fill, x, y, radius, radius, segments);
    return 0;
}

static int l_graphics_ellipse(lua_State *L) {
    int fill = check_draw_mode(L, 1) == 0;
    float x = (float)luaL_checknumber(L, 2);
    float y = (float)luaL_checknumber(L, 3);
    float rx = (float)luaL_checknumber(L, 4);
    float ry = (float)luaL_checknumber(L, 5);
    int segments = (int)luaL_optinteger(L, 6, curve_segments(rx, ry));
    draw_ellipse(L, fill, x, y, rx, ry, segments);
    return 0;
}

/* arc(mode, [arctype], x, y, radius, angle1, angle2, [segments]). A pie
 * closes through the center, closed straight across, open not at all */
static int l_graphics_arc(lua_State *L) {
    static const char *const types[] = {"pie", "open", "closed", NULL};
    enum { ARC_PIE, ARC_OPEN, ARC_CLOSED };

    int fill = check_draw_mode(L, 1) == 0;
    int type = ARC_PIE;
    int idx = 2;
    if (lua_type(L, 2) == LUA_TSTRING) {
        type = luaL_checkoption(L, 2, NULL, types);
        idx = 3;
    }

    float x = (float)luaL_checknumber(L, idx);
    float y = (float)luaL_checknumber(L, idx + 1);
    float radius = (float)luaL_checknumber(L, idx + 2);
    float angle1 = (float)luaL_checknumber(L, idx + 3);
    float angle2 = (float)luaL_checknumber(L, idx + 4);

    float sweep = angle2 - angle1;
    if (sweep == 0.0f) {
        return 0;
    }

    int full_turn = curve_segments(radius, radius);
    if (fabsf(sweep) >= 2.0f * (float)M_PI) {
        /* All the way around, the ends would only overlap */
        draw_ellipse(L, fill, x, y, radius, radius, (int)luaL_optinteger(L, idx + 5, full_turn));
        return 0;
    }

    int default_segments = (int)ceilf(full_turn * fabsf(sweep) / (2.0f * (float)M_PI));
    int segments = (int)luaL_optinteger(L, idx + 5, default_segments);
    if (segments < 1) {
        segments = 1;
    }

    /* Room for the center point of a pie in front of the curve */
    float *coords = scratch_floats(L, 0, (segments + 2) * 2);
    int points = 0;
    if (type == ARC_PIE) {
        coords[0] = x;
        coords[1] = y;
        points = 1;
    }
    for (int i = 0; i <= segments; i++) {
        float angle = angle1 + sweep * i / segments;
        coords[points * 2 + 0] = x + cosf(angle) * radius;
        coords[points * 2 + 1] = y + sinf(angle) * radius;
        points++;
    }

    if (fill) {
        draw_solid(coords, points, GL_TRIANGLE_FAN);
    } else {
        draw_polyline(L, coords, points, type != ARC_OPEN);
    }
    return 0;
}

static int l_graphics_setLineWidth(lua_State *L) {
    float width = (float)luaL_checknumber(L, 1);
    luaL_argcheck(L, width > 0.0f, 1, "line width must be positive");
    g_state.gfx.line_width = width;
    return 0;
}

static int l_graphics_getLineWidth(lua_State *L) {
    lua_pushnumber(L, g_state.gfx.line_width);
    return 1;
}

static int l_graphics_newImage_cont(lua_State *L) {
    AromaImage *img = (AromaImage *)lua_touserdata(L, lua_gettop(L));
    if (!img || !img->loaded) {
        return luaL_error(L, "failed to load image: %s", luaL_checkstring(L, 1));
    }
    return 1;
}

/* Loads suspend the engine's entry-point coroutine. A yield from a user
 * coroutine would surface at its resumer instead, leaving the load with no
 * thread the completion callback could safely resume. */
static void check_load_context(lua_State *L, const char *what) {
    if (L != g_state.script_thread) {
        luaL_error(L, "%s: cannot load resources inside a user coroutine", what);
    }
}

static AromaImageData *check_image_data(lua_State *L, int idx) {
    AromaImageData *data = (AromaImageData *)luaL_checkudata(L, idx, "aroma.image_data");
    if (!data->pixels) {
        luaL_error(L, "ImageData has been released");
    }
    return data;
}

static int l_image_newImageData(lua_State *L) {
    if (lua_type(L, 1) == LUA_TSTRING) {
        return luaL_error(L, "love.image.newImageData: loading from a file isn't supported yet");
    }

    int width = (int)luaL_checkinteger(L, 1);
    int height = (int)luaL_checkinteger(L, 2);
    luaL_argcheck(L, width > 0, 1, "width must be positive");
    luaL_argcheck(L, height > 0, 2, "height must be positive");

    AromaImageData *data = (AromaImageData *)lua_newuserdata(L, sizeof(AromaImageData));
    data->width = width;
    data->height = height;
    /* Transparent black, like love */
    data->pixels = (unsigned char *)calloc((size_t)width * height, 4);
    if (!data->pixels) {
        return luaL_error(L, "love.image.newImageData: out of memory for %dx%d", width, height);
    }

    luaL_getmetatable(L, "aroma.image_data");
    lua_setmetatable(L, -2);
    return 1;
}

static unsigned char *check_pixel(lua_State *L, AromaImageData *data, int idx) {
    int x = (int)floor(luaL_checknumber(L, idx));
    int y = (int)floor(luaL_checknumber(L, idx + 1));
    if (x < 0 || y < 0 || x >= data->width || y >= data->height) {
        luaL_error(L, "pixel (%d, %d) is outside of the %dx%d ImageData", x, y, data->width, data->height);
    }
    return &data->pixels[((size_t)y * data->width + x) * 4];
}

/* Rounds the way love does so the same script gives the same bytes */
static unsigned char color_to_byte(double value) {
    if (value <= 0.0) return 0;
    if (value >= 1.0) return 255;
    return (unsigned char)(value * 255.0 + 0.5);
}

/* Unlike setColor there is no guessing at a 0..255 range */
static void read_pixel_color(lua_State *L, int idx, unsigned char *pixel) {
    if (lua_istable(L, idx)) {
        for (int i = 0; i < 4; i++) {
            lua_rawgeti(L, idx, i + 1);
            pixel[i] = color_to_byte(i == 3 ? luaL_optnumber(L, -1, 1.0) : luaL_checknumber(L, -1));
            lua_pop(L, 1);
        }
        return;
    }

    for (int i = 0; i < 3; i++) {
        pixel[i] = color_to_byte(luaL_checknumber(L, idx + i));
    }
    pixel[3] = color_to_byte(luaL_optnumber(L, idx + 3, 1.0));
}

static int l_imagedata_setPixel(lua_State *L) {
    AromaImageData *data = check_image_data(L, 1);
    read_pixel_color(L, 4, check_pixel(L, data, 2));
    return 0;
}

static int l_imagedata_getPixel(lua_State *L) {
    AromaImageData *data = check_image_data(L, 1);
    unsigned char *pixel = check_pixel(L, data, 2);
    for (int i = 0; i < 4; i++) {
        lua_pushnumber(L, pixel[i] / 255.0);
    }
    return 4;
}

static int l_imagedata_mapPixel(lua_State *L) {
    AromaImageData *data = check_image_data(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    int x0 = (int)luaL_optinteger(L, 3, 0);
    int y0 = (int)luaL_optinteger(L, 4, 0);
    int w = (int)luaL_optinteger(L, 5, data->width);
    int h = (int)luaL_optinteger(L, 6, data->height);

    if (x0 < 0 || y0 < 0 || w < 0 || h < 0 || x0 + w > data->width || y0 + h > data->height) {
        return luaL_error(L, "mapPixel: region is outside of the %dx%d ImageData", data->width, data->height);
    }

    for (int y = y0; y < y0 + h; y++) {
        for (int x = x0; x < x0 + w; x++) {
            /* Looked up every time, the function may release the ImageData */
            if (!data->pixels) {
                return luaL_error(L, "ImageData has been released");
            }
            unsigned char *pixel = &data->pixels[((size_t)y * data->width + x) * 4];

            lua_pushvalue(L, 2);
            lua_pushinteger(L, x);
            lua_pushinteger(L, y);
            for (int i = 0; i < 4; i++) {
                lua_pushnumber(L, pixel[i] / 255.0);
            }
            lua_call(L, 6, 4);

            if (!data->pixels) {
                return luaL_error(L, "ImageData has been released");
            }
            read_pixel_color(L, lua_gettop(L) - 3, pixel);
            lua_pop(L, 4);
        }
    }
    return 0;
}

/* Parts of the region that fall outside of either ImageData are left out
 * rather than being an error */
static int l_imagedata_paste(lua_State *L) {
    AromaImageData *dst = check_image_data(L, 1);
    AromaImageData *src = check_image_data(L, 2);
    int dx = (int)luaL_checkinteger(L, 3);
    int dy = (int)luaL_checkinteger(L, 4);
    int sx = (int)luaL_optinteger(L, 5, 0);
    int sy = (int)luaL_optinteger(L, 6, 0);
    int sw = (int)luaL_optinteger(L, 7, src->width);
    int sh = (int)luaL_optinteger(L, 8, src->height);

    if (sx < 0) { sw += sx; dx -= sx; sx = 0; }
    if (sy < 0) { sh += sy; dy -= sy; sy = 0; }
    if (dx < 0) { sw += dx; sx -= dx; dx = 0; }
    if (dy < 0) { sh += dy; sy -= dy; dy = 0; }
    if (sx + sw > src->width) sw = src->width - sx;
    if (sy + sh > src->height) sh = src->height - sy;
    if (dx + sw > dst->width) sw = dst->width - dx;
    if (dy + sh > dst->height) sh = dst->height - dy;

    for (int row = 0; row < sh; row++) {
        /* memmove, the source may be the destination */
        memmove(&dst->pixels[((size_t)(dy + row) * dst->width + dx) * 4],
                &src->pixels[((size_t)(sy + row) * src->width + sx) * 4],
                (size_t)(sw > 0 ? sw : 0) * 4);
    }
    return 0;
}

static int l_imagedata_getWidth(lua_State *L) {
    lua_pushinteger(L, check_image_data(L, 1)->width);
    return 1;
}

static int l_imagedata_getHeight(lua_State *L) {
    lua_pushinteger(L, check_image_data(L, 1)->height);
    return 1;
}

static int l_imagedata_getDimensions(lua_State *L) {
    AromaImageData *data = check_image_data(L, 1);
    lua_pushinteger(L, data->width);
    lua_pushinteger(L, data->height);
    return 2;
}

/* Also the __gc, so it can't go through check_image_data */
static int l_imagedata_release(lua_State *L) {
    AromaImageData *data = (AromaImageData *)luaL_checkudata(L, 1, "aroma.image_data");
    int had_pixels = data->pixels != NULL;
    free(data->pixels);
    data->pixels = NULL;
    lua_pushboolean(L, had_pixels);
    return 1;
}

/* Copies the pixels as they are now. Doesn't yield, unlike a load from a path */
static int new_image_from_data(lua_State *L) {
    AromaImageData *data = check_image_data(L, 1);

    AromaImage *img = (AromaImage *)lua_newuserdata(L, sizeof(AromaImage));
    memset(img, 0, sizeof(AromaImage));
    default_texture_params(&img->params);
    luaL_getmetatable(L, "aroma.image");
    lua_setmetatable(L, -2);

    img->texture_id = js_create_texture_from_pixels(data->pixels, data->width, data->height);
    if (!img->texture_id) {
        return luaL_error(L, "love.graphics.newImage: failed to create a texture");
    }
    img->width = data->width;
    img->height = data->height;
    img->loaded = 1;
    apply_texture_params(img->texture_id, &img->params);
    return 1;
}

static int l_graphics_newImage(lua_State *L) {
    if (lua_type(L, 1) == LUA_TUSERDATA) {
        return new_image_from_data(L);
    }

    const char *path = luaL_checkstring(L, 1);
    check_load_context(L, "love.graphics.newImage");

    AromaImage *img = (AromaImage *)lua_newuserdata(L, sizeof(AromaImage));
    memset(img, 0, sizeof(AromaImage));
    default_texture_params(&img->params);

    luaL_getmetatable(L, "aroma.image");
    lua_setmetatable(L, -2);

    js_request_texture_load(g_state.generation, (uintptr_t)img, path);

    /* Suspend until the async load completes; aroma_image_loaded resumes the
     * thread and the continuation returns the ready image. */
    g_state.resource_wait = 1;
    return lua_yieldk(L, 0, 0, l_graphics_newImage_cont);
}

static const char *collect_imagefont_glyphs(lua_State *L, int idx, size_t *length_out, int *needs_pop) {
    int abs_idx = lua_absindex(L, idx);

    if (lua_type(L, abs_idx) == LUA_TSTRING) {
        return luaL_checklstring(L, abs_idx, length_out);
    }

    if (lua_type(L, abs_idx) == LUA_TTABLE) {
        luaL_Buffer buf;
        luaL_buffinit(L, &buf);
        lua_Integer len = luaL_len(L, abs_idx);

        for (lua_Integer i = 1; i <= len; ++i) {
            lua_rawgeti(L, abs_idx, i);
            size_t glyph_len = 0;
            const char *glyph = luaL_checklstring(L, -1, &glyph_len);
            if (glyph_len == 0) {
                lua_pop(L, 1);
                continue;
            }

            const char *scan = glyph;
            utf8_decode(&scan);
            if (*scan != '\0') {
                lua_pop(L, 1);
                luaL_error(L, "love.graphics.newImageFont: glyph table entries must be single UTF-8 characters");
                return NULL;
            }

            luaL_addlstring(&buf, glyph, glyph_len);
            lua_pop(L, 1);
        }

        luaL_pushresult(&buf);
        if (needs_pop) {
            *needs_pop = 1;
        }
        return lua_tolstring(L, -1, length_out);
    }

    luaL_argerror(L, idx, "string or table expected");
    return NULL;
}

static GlyphInfo *find_glyph(AromaFont *font, uint32_t codepoint) {
    if (!font) {
        return NULL;
    }

    for (int i = 0; i < font->glyph_count; ++i) {
        if (font->glyphs[i].codepoint == codepoint) {
            return &font->glyphs[i];
        }
    }

    return NULL;
}

static GlyphInfo *glyph_for(AromaFont *font, uint32_t codepoint) {
    GlyphInfo *glyph = find_glyph(font, codepoint);
    return glyph ? glyph : find_glyph(font, (uint32_t)'?');
}

/* Newlines are the caller's business */
static float glyph_advance(AromaFont *font, uint32_t codepoint) {
    if (codepoint == '\r' || codepoint == '\n') {
        return 0.0f;
    }

    if (codepoint == '\t') {
        GlyphInfo *space = find_glyph(font, (uint32_t)' ');
        float advance = space ? (float)space->width + font->extra_spacing : font->image_height * 0.5f;
        return advance * 4.0f;
    }

    GlyphInfo *glyph = glyph_for(font, codepoint);
    return glyph ? (float)glyph->width + font->extra_spacing : 0.0f;
}

static float font_line_advance(AromaFont *font) {
    return (float)font->image_height * font->line_height;
}

/* A line of laid out text, a span of the string it came from. The span keeps
 * the spaces it was wrapped at, as love's getWrap does, but the width and
 * space count leave them out so they don't push aligned text off center */
typedef struct {
    const char *start;
    const char *end;
    float width;
    int spaces;
    /* Ended by a newline or the end of the text rather than by wrapping,
     * justify leaves these alone */
    int hard_break;
} TextLine;

static TextLine *g_lines;
static int g_line_count;
static int g_line_capacity;

static void add_line(lua_State *L, const char *start, const char *end, int hard_break) {
    if (g_line_count >= g_line_capacity) {
        int capacity = g_line_capacity ? g_line_capacity * 2 : 16;
        TextLine *grown = (TextLine *)realloc(g_lines, sizeof(TextLine) * capacity);
        if (!grown) {
            luaL_error(L, "out of memory");
        }
        g_lines = grown;
        g_line_capacity = capacity;
    }

    TextLine *line = &g_lines[g_line_count++];
    line->start = start;
    line->end = end;
    line->hard_break = hard_break;
    line->width = 0.0f;
    line->spaces = 0;
}

/* A negative limit doesn't wrap. A word wider than the limit on its own is
 * broken where it runs out of room */
static void layout_text(lua_State *L, AromaFont *font, const char *text, float limit) {
    g_line_count = 0;

    const char *line_start = text;
    /* Where the line can be wrapped, the start of the word after the last
     * space */
    const char *wrap_resume = NULL;
    float width = 0.0f;

    const char *ptr = text;
    while (*ptr) {
        const char *at = ptr;
        uint32_t codepoint = utf8_decode(&ptr);

        if (codepoint == '\n') {
            add_line(L, line_start, at, 1);
            line_start = ptr;
            wrap_resume = NULL;
            width = 0.0f;
            continue;
        }

        float advance = glyph_advance(font, codepoint);

        if (codepoint == ' ') {
            wrap_resume = ptr;
        } else if (limit >= 0.0f && width + advance > limit && at > line_start) {
            if (wrap_resume) {
                add_line(L, line_start, wrap_resume, 0);
                line_start = wrap_resume;
            } else {
                add_line(L, line_start, at, 0);
                line_start = at;
            }
            wrap_resume = NULL;

            /* Measure again what was carried over to the new line */
            width = 0.0f;
            const char *carried = line_start;
            while (carried < at) {
                width += glyph_advance(font, utf8_decode(&carried));
            }
        }

        width += advance;
    }

    add_line(L, line_start, ptr, 1);

    for (int i = 0; i < g_line_count; i++) {
        TextLine *line = &g_lines[i];
        const char *visible_end = line->end;
        while (visible_end > line->start && visible_end[-1] == ' ') {
            visible_end--;
        }

        const char *scan = line->start;
        while (scan < visible_end) {
            uint32_t codepoint = utf8_decode(&scan);
            line->width += glyph_advance(font, codepoint);
            if (codepoint == ' ') {
                line->spaces++;
            }
        }
    }
}

typedef enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT, ALIGN_JUSTIFY } TextAlign;

/* limit only matters to alignments other than left */
static void draw_text_lines(lua_State *L, AromaFont *font, const Mat3 *transform, TextAlign align, float limit) {
    if (g_state.discard_rendering || font->image_width <= 0) {
        return;
    }

    int max_glyphs = 0;
    for (int i = 0; i < g_line_count; i++) {
        max_glyphs += (int)(g_lines[i].end - g_lines[i].start);
    }
    if (max_glyphs == 0) {
        return;
    }

    /* Two triangles a glyph, x y u v a vertex */
    float *vertices = scratch_floats(L, 0, max_glyphs * 6 * 4);
    int count = 0;

    float inv_width = 1.0f / font->image_width;
    float height = (float)font->image_height;

    for (int i = 0; i < g_line_count; i++) {
        TextLine *line = &g_lines[i];
        float cursor_x = 0.0f;
        float cursor_y = i * font_line_advance(font);
        float space_padding = 0.0f;

        switch (align) {
            case ALIGN_CENTER:
                /* Whole pixels, half of one would blur a pixel font */
                cursor_x = floorf((limit - line->width) / 2.0f);
                break;
            case ALIGN_RIGHT:
                cursor_x = limit - line->width;
                break;
            case ALIGN_JUSTIFY:
                if (!line->hard_break && line->spaces > 0 && line->width < limit) {
                    space_padding = (limit - line->width) / line->spaces;
                }
                break;
            default:
                break;
        }

        const char *ptr = line->start;
        while (ptr < line->end) {
            uint32_t codepoint = utf8_decode(&ptr);
            GlyphInfo *glyph = codepoint == '\t' || codepoint == '\r' ? NULL : glyph_for(font, codepoint);

            if (glyph && glyph->width > 0) {
                float x1 = cursor_x, x2 = cursor_x + (float)glyph->width;
                float y1 = cursor_y, y2 = cursor_y + height;
                float u1 = glyph->x * inv_width, u2 = (glyph->x + glyph->width) * inv_width;

                const float quad[6][4] = {
                    {x1, y1, u1, 0.0f}, {x1, y2, u1, 1.0f}, {x2, y2, u2, 1.0f},
                    {x1, y1, u1, 0.0f}, {x2, y2, u2, 1.0f}, {x2, y1, u2, 0.0f}
                };
                memcpy(&vertices[count * 4], quad, sizeof(quad));
                count += 6;
            }

            cursor_x += glyph_advance(font, codepoint);
            if (codepoint == ' ') {
                cursor_x += space_padding;
            }
        }
    }

    if (count == 0) {
        return;
    }

    begin_draw(transform, font->texture_id);

    glBindBuffer(GL_ARRAY_BUFFER, g_state.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * count * 4, vertices, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (const void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (const void *)(2 * sizeof(float)));

    glDrawArrays(GL_TRIANGLES, 0, count);

    glDisableVertexAttribArray(1);
}

static int draw_mesh(lua_State *L);

/* draw(image, [quad], x, y, r, sx, sy, ox, oy), or a mesh for the image */
static int l_graphics_draw(lua_State *L) {
    if (luaL_testudata(L, 1, "aroma.mesh")) {
        return draw_mesh(L);
    }

    AromaImage *img = check_texture(L, 1);
    if (img == g_state.gfx.canvas) {
        return luaL_error(L, "love.graphics.draw: a canvas can't be drawn to itself");
    }

    AromaQuad *quad = NULL;
    int idx = 2;
    if (lua_type(L, 2) == LUA_TUSERDATA) {
        quad = check_quad(L, 2);
        idx = 3;
    }

    float x = (float)luaL_optnumber(L, idx, 0.0);
    float y = (float)luaL_optnumber(L, idx + 1, 0.0);
    float r = (float)luaL_optnumber(L, idx + 2, 0.0);
    float sx = (float)luaL_optnumber(L, idx + 3, 1.0);
    float sy = (float)luaL_optnumber(L, idx + 4, sx);
    float ox = (float)luaL_optnumber(L, idx + 5, 0.0);
    float oy = (float)luaL_optnumber(L, idx + 6, 0.0);

    if (!img->loaded || img->texture_id == 0) {
        return 0;
    }

    if (g_state.discard_rendering) {
        return 0;
    }

    Mat3 final;
    mat3_local(&final, current_matrix(), x, y, r, sx, sy, ox, oy);

    float w = (float)img->width;
    float h = (float)img->height;
    float u1 = 0.0f, v1 = 0.0f, u2 = 1.0f, v2 = 1.0f;
    if (quad) {
        w = quad->w;
        h = quad->h;
        u1 = quad->x / quad->sw;
        v1 = quad->y / quad->sh;
        u2 = (quad->x + quad->w) / quad->sw;
        v2 = (quad->y + quad->h) / quad->sh;
    }

    float vertices[] = {
        0.0f, 0.0f, u1, v1,
        w,    0.0f, u2, v1,
        w,    h,    u2, v2,
        0.0f, h,    u1, v2
    };

    begin_draw(&final, img->texture_id);

    glBindBuffer(GL_ARRAY_BUFFER, g_state.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4, (const void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4, (const void *)(sizeof(float) * 2));

    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

    glDisableVertexAttribArray(1);
    return 0;
}

static AromaMesh *check_mesh(lua_State *L, int idx) {
    AromaMesh *mesh = (AromaMesh *)luaL_checkudata(L, idx, "aroma.mesh");
    if (!mesh->vertices) {
        luaL_error(L, "Mesh has been released");
    }
    return mesh;
}

/* A vertex is {x, y, u, v, r, g, b, a}, everything after y optional */
static void read_mesh_vertex(lua_State *L, int idx, float *vertex) {
    static const float defaults[MESH_VERTEX_FLOATS] = {0, 0, 0, 0, 1, 1, 1, 1};
    luaL_checktype(L, idx, LUA_TTABLE);
    for (int i = 0; i < MESH_VERTEX_FLOATS; i++) {
        lua_rawgeti(L, idx, i + 1);
        vertex[i] = (float)luaL_optnumber(L, -1, defaults[i]);
        lua_pop(L, 1);
    }
}

/* newMesh(vertices, [mode], [usage]) or newMesh(count, [mode], [usage]). Only
 * love's standard vertex format, the usage hint is ignored */
static int l_graphics_newMesh(lua_State *L) {
    static const char *const modes[] = {"fan", "strip", "triangles", "points", NULL};
    static const GLenum gl_modes[] = {GL_TRIANGLE_FAN, GL_TRIANGLE_STRIP, GL_TRIANGLES, GL_POINTS};

    int from_table = lua_istable(L, 1);
    int count = from_table ? (int)lua_rawlen(L, 1) : (int)luaL_checkinteger(L, 1);
    luaL_argcheck(L, count > 0, 1, "a mesh needs at least 1 vertex");

    if (from_table) {
        /* A vertex format is a table of tables of strings, vertices of numbers */
        lua_rawgeti(L, 1, 1);
        if (lua_istable(L, -1)) {
            lua_rawgeti(L, -1, 1);
            if (lua_type(L, -1) == LUA_TSTRING) {
                return luaL_error(L, "love.graphics.newMesh: custom vertex formats aren't supported yet");
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }

    int mode = luaL_checkoption(L, 2, "fan", modes);

    AromaMesh *mesh = (AromaMesh *)lua_newuserdata(L, sizeof(AromaMesh));
    memset(mesh, 0, sizeof(AromaMesh));
    luaL_getmetatable(L, "aroma.mesh");
    lua_setmetatable(L, -2);

    mesh->mode = gl_modes[mode];
    mesh->count = count;
    mesh->vertices = (float *)calloc((size_t)count, sizeof(float) * MESH_VERTEX_FLOATS);
    if (!mesh->vertices) {
        return luaL_error(L, "love.graphics.newMesh: out of memory");
    }

    for (int i = 0; i < count; i++) {
        float *vertex = &mesh->vertices[i * MESH_VERTEX_FLOATS];
        if (from_table) {
            lua_rawgeti(L, 1, i + 1);
            read_mesh_vertex(L, lua_gettop(L), vertex);
            lua_pop(L, 1);
        } else {
            vertex[4] = vertex[5] = vertex[6] = vertex[7] = 1.0f;
        }
    }

    glGenBuffers(1, &mesh->buffer);
    glBindBuffer(GL_ARRAY_BUFFER, mesh->buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * MESH_VERTEX_FLOATS * count, mesh->vertices, GL_STATIC_DRAW);
    return 1;
}

static int draw_mesh(lua_State *L) {
    AromaMesh *mesh = check_mesh(L, 1);
    float x = (float)luaL_optnumber(L, 2, 0.0);
    float y = (float)luaL_optnumber(L, 3, 0.0);
    float r = (float)luaL_optnumber(L, 4, 0.0);
    float sx = (float)luaL_optnumber(L, 5, 1.0);
    float sy = (float)luaL_optnumber(L, 6, sx);
    float ox = (float)luaL_optnumber(L, 7, 0.0);
    float oy = (float)luaL_optnumber(L, 8, 0.0);

    AromaImage *texture = mesh->texture;
    if (texture && texture == g_state.gfx.canvas) {
        return luaL_error(L, "love.graphics.draw: a canvas can't be drawn to itself");
    }

    if (g_state.discard_rendering) {
        return 0;
    }

    Mat3 final;
    mat3_local(&final, current_matrix(), x, y, r, sx, sy, ox, oy);
    /* A texture that was released draws as if there was none */
    begin_draw(&final, texture && texture->loaded ? texture->texture_id : 0);

    GLsizei stride = sizeof(float) * MESH_VERTEX_FLOATS;
    glBindBuffer(GL_ARRAY_BUFFER, mesh->buffer);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (const void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (const void *)(sizeof(float) * 2));
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, (const void *)(sizeof(float) * 4));

    glDrawArrays(mesh->mode, 0, mesh->count);

    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
    return 0;
}

static int l_mesh_setTexture(lua_State *L) {
    AromaMesh *mesh = check_mesh(L, 1);
    AromaImage *texture = lua_isnoneornil(L, 2) ? NULL : check_texture(L, 2);

    lua_settop(L, 2);
    if (texture) {
        lua_createtable(L, 1, 0);
        lua_pushvalue(L, 2);
        lua_rawseti(L, -2, 1);
    } else {
        lua_pushnil(L);
    }
    lua_setuservalue(L, 1);
    mesh->texture = texture;
    return 0;
}

static int l_mesh_getTexture(lua_State *L) {
    check_mesh(L, 1);
    lua_getuservalue(L, 1);
    if (!lua_istable(L, -1)) {
        return 0;
    }
    lua_rawgeti(L, -1, 1);
    return 1;
}

static int l_mesh_getVertexCount(lua_State *L) {
    lua_pushinteger(L, check_mesh(L, 1)->count);
    return 1;
}

static float *check_mesh_vertex(lua_State *L, AromaMesh *mesh, int idx) {
    int index = (int)luaL_checkinteger(L, idx);
    luaL_argcheck(L, index >= 1 && index <= mesh->count, idx, "vertex index out of range");
    return &mesh->vertices[(index - 1) * MESH_VERTEX_FLOATS];
}

static int l_mesh_getVertex(lua_State *L) {
    AromaMesh *mesh = check_mesh(L, 1);
    float *vertex = check_mesh_vertex(L, mesh, 2);
    for (int i = 0; i < MESH_VERTEX_FLOATS; i++) {
        lua_pushnumber(L, vertex[i]);
    }
    return MESH_VERTEX_FLOATS;
}

/* setVertex(index, x, y, u, v, r, g, b, a) or setVertex(index, vertex) */
static int l_mesh_setVertex(lua_State *L) {
    AromaMesh *mesh = check_mesh(L, 1);
    float *vertex = check_mesh_vertex(L, mesh, 2);

    if (lua_istable(L, 3)) {
        read_mesh_vertex(L, 3, vertex);
    } else {
        /* Components that aren't given keep their value, as in love */
        for (int i = 0; i < MESH_VERTEX_FLOATS; i++) {
            vertex[i] = (float)luaL_optnumber(L, 3 + i, vertex[i]);
        }
    }

    glBindBuffer(GL_ARRAY_BUFFER, mesh->buffer);
    glBufferSubData(GL_ARRAY_BUFFER, (char *)vertex - (char *)mesh->vertices,
                    sizeof(float) * MESH_VERTEX_FLOATS, vertex);
    return 0;
}

/* Also the __gc */
static int l_mesh_release(lua_State *L) {
    AromaMesh *mesh = (AromaMesh *)luaL_checkudata(L, 1, "aroma.mesh");
    int had_vertices = mesh->vertices != NULL;
    if (had_vertices) {
        glDeleteBuffers(1, &mesh->buffer);
        free(mesh->vertices);
        mesh->vertices = NULL;
        mesh->buffer = 0;
        mesh->texture = NULL;
    }
    lua_pushboolean(L, had_vertices);
    return 1;
}

/* newQuad(x, y, w, h, sw, sh), or a texture in place of sw, sh */
static int l_graphics_newQuad(lua_State *L) {
    float x = (float)luaL_checknumber(L, 1);
    float y = (float)luaL_checknumber(L, 2);
    float w = (float)luaL_checknumber(L, 3);
    float h = (float)luaL_checknumber(L, 4);
    float sw, sh;

    if (lua_type(L, 5) == LUA_TUSERDATA) {
        AromaImage *img = check_texture(L, 5);
        sw = (float)img->width;
        sh = (float)img->height;
    } else {
        sw = (float)luaL_checknumber(L, 5);
        sh = (float)luaL_checknumber(L, 6);
    }
    luaL_argcheck(L, sw > 0.0f && sh > 0.0f, 5, "reference size must be positive");

    AromaQuad *quad = (AromaQuad *)lua_newuserdata(L, sizeof(AromaQuad));
    quad->x = x;
    quad->y = y;
    quad->w = w;
    quad->h = h;
    quad->sw = sw;
    quad->sh = sh;
    luaL_getmetatable(L, "aroma.quad");
    lua_setmetatable(L, -2);
    return 1;
}

static int l_quad_getViewport(lua_State *L) {
    AromaQuad *quad = check_quad(L, 1);
    lua_pushnumber(L, quad->x);
    lua_pushnumber(L, quad->y);
    lua_pushnumber(L, quad->w);
    lua_pushnumber(L, quad->h);
    return 4;
}

static int l_quad_setViewport(lua_State *L) {
    AromaQuad *quad = check_quad(L, 1);
    float x = (float)luaL_checknumber(L, 2);
    float y = (float)luaL_checknumber(L, 3);
    float w = (float)luaL_checknumber(L, 4);
    float h = (float)luaL_checknumber(L, 5);

    if (!lua_isnoneornil(L, 6)) {
        float sw = (float)luaL_checknumber(L, 6);
        float sh = (float)luaL_checknumber(L, 7);
        luaL_argcheck(L, sw > 0.0f && sh > 0.0f, 6, "reference size must be positive");
        quad->sw = sw;
        quad->sh = sh;
    }

    quad->x = x;
    quad->y = y;
    quad->w = w;
    quad->h = h;
    return 0;
}

static int l_quad_getTextureDimensions(lua_State *L) {
    AromaQuad *quad = check_quad(L, 1);
    lua_pushnumber(L, quad->sw);
    lua_pushnumber(L, quad->sh);
    return 2;
}

static int l_graphics_setDefaultFilter(lua_State *L) {
    FilterMode min = (FilterMode)luaL_checkoption(L, 1, NULL, filter_names);
    g_state.default_mag = (FilterMode)luaL_checkoption(L, 2, filter_names[min], filter_names);
    g_state.default_min = min;
    return 0;
}

static int l_graphics_getDefaultFilter(lua_State *L) {
    lua_pushstring(L, filter_names[g_state.default_min]);
    lua_pushstring(L, filter_names[g_state.default_mag]);
    lua_pushinteger(L, 1); /* anisotropy */
    return 3;
}

static int l_graphics_getWidth(lua_State *L) {
    lua_pushinteger(L, g_state.window_width);
    return 1;
}

static int l_graphics_getHeight(lua_State *L) {
    lua_pushinteger(L, g_state.window_height);
    return 1;
}

static int l_graphics_getDimensions(lua_State *L) {
    lua_pushinteger(L, g_state.window_width);
    lua_pushinteger(L, g_state.window_height);
    return 2;
}

static int l_graphics_rectangle(lua_State *L) {
    int fill = check_draw_mode(L, 1) == 0;
    float x = (float)luaL_checknumber(L, 2);
    float y = (float)luaL_checknumber(L, 3);
    float w = (float)luaL_checknumber(L, 4);
    float h = (float)luaL_checknumber(L, 5);

    float coords[8] = {
        x, y,
        x, y + h,
        x + w, y + h,
        x + w, y
    };

    draw_outline(L, fill, coords, 4);
    return 0;
}

static int l_image_getWidth(lua_State *L) {
    AromaImage *img = check_texture(L, 1);
    lua_pushinteger(L, img->width);
    return 1;
}

static int l_image_getHeight(lua_State *L) {
    AromaImage *img = check_texture(L, 1);
    lua_pushinteger(L, img->height);
    return 1;
}

static int l_image_getDimensions(lua_State *L) {
    AromaImage *img = check_texture(L, 1);
    lua_pushinteger(L, img->width);
    lua_pushinteger(L, img->height);
    return 2;
}

static int l_image_setFilter(lua_State *L) {
    AromaImage *img = check_texture(L, 1);
    return set_filter(L, img->texture_id, &img->params);
}

static int l_image_getFilter(lua_State *L) {
    return get_filter(L, &check_texture(L, 1)->params);
}

static int l_image_setWrap(lua_State *L) {
    AromaImage *img = check_texture(L, 1);
    return set_wrap(L, img->texture_id, &img->params);
}

static int l_image_getWrap(lua_State *L) {
    return get_wrap(L, &check_texture(L, 1)->params);
}

/* Also the __gc. A released image draws nothing */
static int l_image_release(lua_State *L) {
    AromaImage *img = check_texture(L, 1);
    /* Releasing the canvas being drawn to goes back to the window. Only by
     * hand, the collector can't reach a canvas while it's the target */
    if (img == g_state.gfx.canvas) {
        release_canvas_ref(L);
        g_state.gfx.canvas = NULL;
        apply_render_target();
    }
    int had_texture = img->texture_id != 0;
    if (had_texture) {
        js_release_texture(img->texture_id);
        img->texture_id = 0;
    }
    img->loaded = 0;
    lua_pushboolean(L, had_texture);
    return 1;
}

static int l_graphics_newImageFont_cont(lua_State *L) {
    AromaFont *font = (AromaFont *)lua_touserdata(L, lua_gettop(L));
    if (!font || !font->loaded) {
        return luaL_error(L, "failed to load image font: %s", luaL_checkstring(L, 1));
    }
    return 1;
}

static int l_graphics_newImageFont(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    check_load_context(L, "love.graphics.newImageFont");
    /* Read spacing before collect_imagefont_glyphs can push the collected
     * glyph string on top of the stack at index 3. */
    double spacing = luaL_optnumber(L, 3, 0.0);
    int needs_pop = 0;
    const char *glyphs = collect_imagefont_glyphs(L, 2, NULL, &needs_pop);

    AromaFont *font = (AromaFont *)lua_newuserdata(L, sizeof(AromaFont));
    memset(font, 0, sizeof(AromaFont));
    font->extra_spacing = (float)spacing;
    default_texture_params(&font->params);

    luaL_getmetatable(L, "aroma.font");
    lua_setmetatable(L, -2);

    /* glyphs may point into the collected string below the userdata; the JS
     * bridge copies it during this call, so it can be removed right after. */
    js_request_font_load(g_state.generation, (uintptr_t)font, path, glyphs, spacing);

    if (needs_pop) {
        lua_remove(L, -2);
    }

    g_state.resource_wait = 1;
    return lua_yieldk(L, 0, 0, l_graphics_newImageFont_cont);
}

static int l_graphics_setFont(lua_State *L) {
    /* The active font is anchored in the registry so it survives even when
     * the script drops its last reference to the userdata. */
    release_font_ref(L);

    if (lua_isnoneornil(L, 1)) {
        g_state.gfx.font = NULL;
        return 0;
    }

    AromaFont *font = check_font(L, 1);
    lua_pushvalue(L, 1);
    g_state.gfx.font_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    g_state.gfx.font = font;
    return 0;
}

static int l_graphics_getFont(lua_State *L) {
    if (g_state.gfx.font_ref == LUA_NOREF) {
        lua_pushnil(L);
    } else {
        lua_rawgeti(L, LUA_REGISTRYINDEX, g_state.gfx.font_ref);
    }
    return 1;
}

/* There is no built in font yet, text drawn before setFont doesn't show */
static AromaFont *drawable_font(void) {
    AromaFont *font = g_state.gfx.font;
    if (!font || !font->loaded || font->texture_id == 0 || font->glyph_count == 0) {
        return NULL;
    }
    return font;
}

/* The r, sx, sy, ox, oy that end print and printf, starting at idx */
static void check_text_transform(lua_State *L, int idx, float x, float y, Mat3 *out) {
    float r = (float)luaL_optnumber(L, idx, 0.0);
    float sx = (float)luaL_optnumber(L, idx + 1, 1.0);
    float sy = (float)luaL_optnumber(L, idx + 2, sx);
    float ox = (float)luaL_optnumber(L, idx + 3, 0.0);
    float oy = (float)luaL_optnumber(L, idx + 4, 0.0);
    mat3_local(out, current_matrix(), x, y, r, sx, sy, ox, oy);
}

static int l_graphics_print(lua_State *L) {
    const char *text = luaL_checkstring(L, 1);
    float x = (float)luaL_optnumber(L, 2, 0.0);
    float y = (float)luaL_optnumber(L, 3, 0.0);
    Mat3 transform;
    check_text_transform(L, 4, x, y, &transform);

    AromaFont *font = drawable_font();
    if (!font) {
        return 0;
    }

    layout_text(L, font, text, -1.0f);
    draw_text_lines(L, font, &transform, ALIGN_LEFT, 0.0f);
    return 0;
}

static int l_graphics_printf(lua_State *L) {
    static const char *const aligns[] = {"left", "center", "right", "justify", NULL};

    const char *text = luaL_checkstring(L, 1);
    float x = (float)luaL_checknumber(L, 2);
    float y = (float)luaL_checknumber(L, 3);
    float limit = (float)luaL_checknumber(L, 4);
    TextAlign align = (TextAlign)luaL_checkoption(L, 5, "left", aligns);
    Mat3 transform;
    check_text_transform(L, 6, x, y, &transform);

    AromaFont *font = drawable_font();
    if (!font) {
        return 0;
    }

    layout_text(L, font, text, limit < 0.0f ? 0.0f : limit);
    draw_text_lines(L, font, &transform, align, limit);
    return 0;
}

static int l_font_getWidth(lua_State *L) {
    AromaFont *font = check_font(L, 1);
    const char *text = luaL_checkstring(L, 2);

    /* Trailing spaces count here, unlike in a laid out line */
    float widest = 0.0f, width = 0.0f;
    while (*text) {
        uint32_t codepoint = utf8_decode(&text);
        if (codepoint == '\n') {
            width = 0.0f;
            continue;
        }
        width += glyph_advance(font, codepoint);
        if (width > widest) {
            widest = width;
        }
    }

    lua_pushnumber(L, widest);
    return 1;
}

static int l_font_getHeight(lua_State *L) {
    lua_pushinteger(L, check_font(L, 1)->image_height);
    return 1;
}

static int l_font_getLineHeight(lua_State *L) {
    lua_pushnumber(L, check_font(L, 1)->line_height);
    return 1;
}

static int l_font_setLineHeight(lua_State *L) {
    check_font(L, 1)->line_height = (float)luaL_checknumber(L, 2);
    return 0;
}

static int l_font_getWrap(lua_State *L) {
    AromaFont *font = check_font(L, 1);
    const char *text = luaL_checkstring(L, 2);
    float limit = (float)luaL_checknumber(L, 3);

    layout_text(L, font, text, limit < 0.0f ? 0.0f : limit);

    float widest = 0.0f;
    lua_createtable(L, g_line_count, 0);
    for (int i = 0; i < g_line_count; i++) {
        TextLine *line = &g_lines[i];
        if (line->width > widest) {
            widest = line->width;
        }
        lua_pushlstring(L, line->start, (size_t)(line->end - line->start));
        lua_rawseti(L, -2, i + 1);
    }

    lua_pushnumber(L, widest);
    lua_insert(L, -2);
    return 2;
}

static int l_font_setFilter(lua_State *L) {
    AromaFont *font = check_font(L, 1);
    return set_filter(L, font->texture_id, &font->params);
}

static int l_font_getFilter(lua_State *L) {
    return get_filter(L, &check_font(L, 1)->params);
}

static int l_font_hasGlyphs(lua_State *L) {
    AromaFont *font = check_font(L, 1);
    int nargs = lua_gettop(L);

    for (int i = 2; i <= nargs; i++) {
        const char *text = luaL_checkstring(L, i);
        while (*text) {
            if (!find_glyph(font, utf8_decode(&text))) {
                lua_pushboolean(L, 0);
                return 1;
            }
        }
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int l_font_gc(lua_State *L) {
    AromaFont *font = check_font(L, 1);
    if (font->texture_id) {
        js_release_texture(font->texture_id);
    }
    font->texture_id = 0;
    font->glyph_count = 0;
    font->image_width = 0;
    font->image_height = 0;
    font->line_height = 0.0f;
    font->loaded = 0;
    if (g_state.gfx.font == font) {
        g_state.gfx.font = NULL;
    }
    return 0;
}

static int l_keyboard_isDown(lua_State *L) {
    int numargs = lua_gettop(L);

    for (int i = 1; i <= numargs; i++) {
        const char *key = luaL_checkstring(L, i);
        if (js_is_key_down(key)) {
            lua_pushboolean(L, 1);
            return 1;
        }
    }

    lua_pushboolean(L, 0);
    return 1;
}


static AromaSource *check_source(lua_State *L, int idx) {
    AromaSource *source = (AromaSource *)luaL_checkudata(L, idx, "aroma.source");
    if (!source->source_id) {
        luaL_error(L, "Source has been released");
    }
    return source;
}

static AromaSource *push_source(lua_State *L) {
    AromaSource *source = (AromaSource *)lua_newuserdata(L, sizeof(AromaSource));
    memset(source, 0, sizeof(AromaSource));
    source->volume = 1.0f;
    source->pitch = 1.0f;
    luaL_getmetatable(L, "aroma.source");
    lua_setmetatable(L, -2);
    return source;
}

static int l_audio_newSource_cont(lua_State *L) {
    AromaSource *source = (AromaSource *)lua_touserdata(L, lua_gettop(L));
    if (!source || !source->loaded) {
        return luaL_error(L, "failed to load audio: %s", luaL_checkstring(L, 1));
    }
    return 1;
}

/* newSource(path, type). "stream" sources are decoded whole like "static"
 * ones, there is no streaming */
static int l_audio_newSource(lua_State *L) {
    static const char *const types[] = {"static", "stream", NULL};
    const char *path = luaL_checkstring(L, 1);
    luaL_checkoption(L, 2, "static", types);
    check_load_context(L, "love.audio.newSource");

    AromaSource *source = push_source(L);
    js_request_audio_load(g_state.generation, (uintptr_t)source, path);

    g_state.resource_wait = 1;
    return lua_yieldk(L, 0, 0, l_audio_newSource_cont);
}

static int l_source_clone(lua_State *L) {
    AromaSource *source = check_source(L, 1);
    AromaSource *copy = push_source(L);
    *copy = *source;
    copy->source_id = js_audio_clone(source->source_id);
    if (!copy->source_id) {
        return luaL_error(L, "Source:clone failed");
    }
    return 1;
}

static int l_source_play(lua_State *L) {
    lua_pushboolean(L, js_audio_play(check_source(L, 1)->source_id));
    return 1;
}

static int l_source_pause(lua_State *L) {
    js_audio_pause(check_source(L, 1)->source_id);
    return 0;
}

static int l_source_stop(lua_State *L) {
    js_audio_stop(check_source(L, 1)->source_id);
    return 0;
}

static int l_source_isPlaying(lua_State *L) {
    lua_pushboolean(L, js_audio_is_playing(check_source(L, 1)->source_id));
    return 1;
}

static int l_source_setVolume(lua_State *L) {
    AromaSource *source = check_source(L, 1);
    float volume = (float)luaL_checknumber(L, 2);
    source->volume = volume < 0.0f ? 0.0f : volume > 1.0f ? 1.0f : volume;
    js_audio_set_volume(source->source_id, source->volume);
    return 0;
}

static int l_source_getVolume(lua_State *L) {
    lua_pushnumber(L, check_source(L, 1)->volume);
    return 1;
}

static int l_source_getVolumeLimits(lua_State *L) {
    check_source(L, 1);
    lua_pushnumber(L, 0.0);
    lua_pushnumber(L, 1.0);
    return 2;
}

static int l_source_setPitch(lua_State *L) {
    AromaSource *source = check_source(L, 1);
    float pitch = (float)luaL_checknumber(L, 2);
    luaL_argcheck(L, pitch > 0.0f, 2, "pitch has to be greater than 0");
    source->pitch = pitch;
    js_audio_set_pitch(source->source_id, pitch);
    return 0;
}

static int l_source_getPitch(lua_State *L) {
    lua_pushnumber(L, check_source(L, 1)->pitch);
    return 1;
}

static int l_source_setLooping(lua_State *L) {
    AromaSource *source = check_source(L, 1);
    luaL_checktype(L, 2, LUA_TBOOLEAN);
    source->looping = lua_toboolean(L, 2);
    js_audio_set_looping(source->source_id, source->looping);
    return 0;
}

static int l_source_isLooping(lua_State *L) {
    lua_pushboolean(L, check_source(L, 1)->looping);
    return 1;
}

/* Positions are in seconds, love's "samples" unit isn't supported */
static int l_source_seek(lua_State *L) {
    AromaSource *source = check_source(L, 1);
    double seconds = luaL_checknumber(L, 2);
    luaL_argcheck(L, seconds >= 0.0, 2, "can't seek to a negative position");
    js_audio_seek(source->source_id, seconds);
    return 0;
}

static int l_source_tell(lua_State *L) {
    lua_pushnumber(L, js_audio_tell(check_source(L, 1)->source_id));
    return 1;
}

static int l_source_getDuration(lua_State *L) {
    lua_pushnumber(L, check_source(L, 1)->duration);
    return 1;
}

/* Also the __gc */
static int l_source_release(lua_State *L) {
    AromaSource *source = (AromaSource *)luaL_checkudata(L, 1, "aroma.source");
    int had_source = source->source_id != 0;
    if (had_source) {
        js_audio_release(source->source_id);
        source->source_id = 0;
    }
    lua_pushboolean(L, had_source);
    return 1;
}

static int l_audio_setVolume(lua_State *L) {
    float volume = (float)luaL_checknumber(L, 1);
    g_state.master_volume = volume < 0.0f ? 0.0f : volume;
    js_audio_set_master_volume(g_state.master_volume);
    return 0;
}

static int l_audio_getVolume(lua_State *L) {
    lua_pushnumber(L, g_state.master_volume);
    return 1;
}

/* stop() for every source, or stop(source, ...) */
static int l_audio_stop(lua_State *L) {
    int nargs = lua_gettop(L);
    if (nargs == 0) {
        js_audio_stop_all();
        return 0;
    }
    for (int i = 1; i <= nargs; i++) {
        js_audio_stop(check_source(L, i)->source_id);
    }
    return 0;
}

static const char *const gamepad_button_names[GAMEPAD_BUTTONS + 1] = {
    "a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick",
    "leftshoulder", "rightshoulder", "dpup", "dpdown", "dpleft", "dpright", NULL
};

static const char *const gamepad_axis_names[GAMEPAD_AXES + 1] = {
    "leftx", "lefty", "rightx", "righty", "triggerleft", "triggerright", NULL
};

/* SDL's mapping strings call the triggers something else */
static const char *const sdl_axis_names[GAMEPAD_AXES] = {
    "leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger"
};

#define B(n) {BIND_BUTTON, n, 0, 0.0f, 0.0f}
#define A(n) {BIND_AXIS, n, 0, -1.0f, 1.0f}

/* The Gamepad API's "standard" layout. Its triggers are buttons 6 and 7,
 * whose analog value is the axis */
static const GamepadMapping standard_mapping = {
    0, 0,
    {B(0), B(1), B(2), B(3), B(8), B(16), B(9), B(10), B(11), B(4), B(5), B(12), B(13), B(14), B(15)},
    {A(0), A(1), A(2), A(3), B(6), B(7)}
};

#undef B
#undef A

/* Mappings for pads that browsers are known to leave unmapped, in SDL's
 * format. Scripts add their own with love.joystick.loadGamepadMappings */
static const char *const builtin_mapping_strings[] = {
    "0300c27e373500002210000010010000,GameSir-G7 Pro,a:b0,b:b1,x:b3,y:b4,back:b10,guide:b12,start:b11,leftstick:b13,rightstick:b14,leftshoulder:b6,rightshoulder:b7,dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a5,righttrigger:a4,platform:Linux,",
    NULL
};

static GamepadMapping g_builtin_mappings[sizeof(builtin_mapping_strings) / sizeof(builtin_mapping_strings[0])];
static int g_builtin_mapping_count = -1;

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* A 16 bit little endian field of an SDL GUID, which is 32 hex digits. The
 * vendor is at digit 8 and the product at 16 */
static int guid_field(const char *guid, int at) {
    int digits[4];
    for (int i = 0; i < 4; i++) {
        digits[i] = hex_digit(guid[at + i]);
        if (digits[i] < 0) return -1;
    }
    return (digits[2] << 12) | (digits[3] << 8) | (digits[0] << 4) | digits[1];
}

static int parse_guid(const char *guid, size_t length, int *vendor, int *product) {
    if (length != 32) {
        return 0;
    }
    *vendor = guid_field(guid, 8);
    *product = guid_field(guid, 16);
    return *vendor > 0 && *product >= 0;
}

/* One value of a mapping string: b3, a2, +a2, -a2, a2~ or h0.4 */
static int parse_binding(const char *value, size_t length, Binding *out) {
    char text[16];
    if (length == 0 || length >= sizeof(text)) {
        return 0;
    }
    memcpy(text, value, length);
    text[length] = '\0';

    const char *ptr = text;
    float from = -1.0f, to = 1.0f;
    if (*ptr == '+') { from = 0.0f; ptr++; }
    else if (*ptr == '-') { from = 0.0f; to = -1.0f; ptr++; }

    char kind = *ptr++;
    char *end;
    long index = strtol(ptr, &end, 10);
    if (end == ptr || index < 0) {
        return 0;
    }

    memset(out, 0, sizeof(Binding));
    out->index = (int)index;

    if (kind == 'b' && index < MAX_JOYSTICK_BUTTONS) {
        out->type = BIND_BUTTON;
    } else if (kind == 'a' && index < MAX_JOYSTICK_AXES) {
        out->type = BIND_AXIS;
        if (*end == '~') {
            float swap = from; from = to; to = swap;
        }
        out->from = from;
        out->to = to;
    } else if (kind == 'h' && *end == '.') {
        out->type = BIND_HAT;
        out->hat_mask = (int)strtol(end + 1, NULL, 10);
    } else {
        return 0;
    }
    return 1;
}

/* Parses one line of SDL's gamecontrollerdb format. Returns 0 for lines that
 * aren't mappings or that are for another platform */
static int parse_mapping_line(const char *line, size_t length, GamepadMapping *out) {
    static const char *const platforms[] = {"Linux", "Windows", "Mac OS X"};

    const char *end = line + length;
    const char *comma = memchr(line, ',', length);
    if (!comma) {
        return 0;
    }

    memset(out, 0, sizeof(GamepadMapping));
    if (!parse_guid(line, (size_t)(comma - line), &out->vendor, &out->product)) {
        return 0;
    }

    /* Skip the name */
    const char *field = memchr(comma + 1, ',', (size_t)(end - comma - 1));
    while (field && field + 1 < end) {
        field++;
        const char *next = memchr(field, ',', (size_t)(end - field));
        const char *field_end = next ? next : end;
        const char *colon = memchr(field, ':', (size_t)(field_end - field));

        if (colon) {
            size_t key_len = (size_t)(colon - field);
            const char *value = colon + 1;
            size_t value_len = (size_t)(field_end - value);

            if (key_len == 8 && memcmp(field, "platform", 8) == 0) {
                int platform = js_gamepad_platform();
                if (platform < 0 || strlen(platforms[platform]) != value_len ||
                    memcmp(platforms[platform], value, value_len) != 0) {
                    return 0;
                }
            }

            for (int i = 0; i < GAMEPAD_BUTTONS; i++) {
                if (strlen(gamepad_button_names[i]) == key_len && memcmp(gamepad_button_names[i], field, key_len) == 0) {
                    parse_binding(value, value_len, &out->buttons[i]);
                }
            }
            for (int i = 0; i < GAMEPAD_AXES; i++) {
                if (strlen(sdl_axis_names[i]) == key_len && memcmp(sdl_axis_names[i], field, key_len) == 0) {
                    parse_binding(value, value_len, &out->axes[i]);
                }
            }
        }

        field = next;
    }
    return 1;
}

static const GamepadMapping *find_mapping(int vendor, int product) {
    if (vendor <= 0) {
        return NULL;
    }

    if (g_builtin_mapping_count < 0) {
        g_builtin_mapping_count = 0;
        for (int i = 0; builtin_mapping_strings[i]; i++) {
            const char *line = builtin_mapping_strings[i];
            if (parse_mapping_line(line, strlen(line), &g_builtin_mappings[g_builtin_mapping_count])) {
                g_builtin_mapping_count++;
            }
        }
    }

    for (int i = 0; i < g_state.user_mapping_count; i++) {
        const GamepadMapping *mapping = &g_state.user_mappings[i];
        if (mapping->vendor == vendor && mapping->product == product) return mapping;
    }
    for (int i = 0; i < g_builtin_mapping_count; i++) {
        const GamepadMapping *mapping = &g_builtin_mappings[i];
        if (mapping->vendor == vendor && mapping->product == product) return mapping;
    }
    return NULL;
}

/* The user mapping for these ids, added if there is room */
static GamepadMapping *user_mapping_for(int vendor, int product) {
    for (int i = 0; i < g_state.user_mapping_count; i++) {
        GamepadMapping *mapping = &g_state.user_mappings[i];
        if (mapping->vendor == vendor && mapping->product == product) return mapping;
    }
    if (g_state.user_mapping_count >= MAX_GAMEPAD_MAPPINGS) {
        return NULL;
    }
    GamepadMapping *mapping = &g_state.user_mappings[g_state.user_mapping_count++];
    memset(mapping, 0, sizeof(GamepadMapping));
    mapping->vendor = vendor;
    mapping->product = product;
    return mapping;
}

static int mapping_has_hat(const GamepadMapping *mapping) {
    for (int i = 0; i < GAMEPAD_BUTTONS; i++) {
        if (mapping->buttons[i].type == BIND_HAT) return 1;
    }
    return 0;
}

/* Picks the mapping for a pad, again whenever the script changes them */
static void assign_mapping(JoystickSlot *slot, int browser_mapped) {
    slot->mapping = browser_mapped ? &standard_mapping : find_mapping(slot->vendor, slot->product);
    slot->has_hat = slot->mapping && mapping_has_hat(slot->mapping);
}

/* SDL's hat bits from the two axes the hat arrives as */
static int joystick_hat(const JoystickSlot *slot) {
    if (!slot->has_hat || slot->axis_count < 2) {
        return 0;
    }
    float x = slot->axes[slot->axis_count - 2];
    float y = slot->axes[slot->axis_count - 1];
    return (y < -0.5f ? 1 : 0) | (x > 0.5f ? 2 : 0) | (y > 0.5f ? 4 : 0) | (x < -0.5f ? 8 : 0);
}

/* 0 to 1 along the stretch of the raw axis that the binding uses */
static float binding_axis_travel(const JoystickSlot *slot, const Binding *binding) {
    if (binding->index >= slot->axis_count || !(slot->axes_seen & (1u << binding->index))) {
        return 0.0f;
    }
    float travel = (slot->axes[binding->index] - binding->from) / (binding->to - binding->from);
    return travel < 0.0f ? 0.0f : travel > 1.0f ? 1.0f : travel;
}

static int gamepad_button_down(const JoystickSlot *slot, int button) {
    const Binding *binding = &slot->mapping->buttons[button];
    switch (binding->type) {
        case BIND_BUTTON:
            return binding->index < slot->button_count && ((slot->pressed >> binding->index) & 1);
        case BIND_AXIS:
            return binding_axis_travel(slot, binding) > 0.5f;
        case BIND_HAT:
            return (joystick_hat(slot) & binding->hat_mask) != 0;
        default:
            return 0;
    }
}

static float gamepad_axis_value(const JoystickSlot *slot, int axis) {
    const Binding *binding = &slot->mapping->axes[axis];
    int is_trigger = axis >= GAMEPAD_STICK_AXES;

    switch (binding->type) {
        case BIND_BUTTON:
            if (binding->index >= slot->button_count) return 0.0f;
            return slot->values[binding->index];
        case BIND_AXIS: {
            if (binding->index >= slot->axis_count) return 0.0f;
            int whole = fabsf(binding->to - binding->from) == 2.0f;
            if (!is_trigger && whole) {
                /* A stick on a whole axis passes through, flipped for a2~ */
                return binding->to > binding->from ? slot->axes[binding->index] : -slot->axes[binding->index];
            }
            return binding_axis_travel(slot, binding);
        }
        default:
            return 0.0f;
    }
}

static AromaJoystick *check_joystick(lua_State *L, int idx) {
    return (AromaJoystick *)luaL_checkudata(L, idx, "aroma.joystick");
}

/* The slot a Joystick reads from, NULL once the pad it stood for is gone.
 * Every getter answers with zeros then, as love does for a removed joystick */
static JoystickSlot *live_joystick(lua_State *L, int idx) {
    AromaJoystick *joystick = check_joystick(L, idx);
    JoystickSlot *slot = &g_state.joysticks[joystick->slot];
    return slot->connected && slot->instance_id == joystick->instance_id ? slot : NULL;
}

static int l_joystick_getJoysticks(lua_State *L) {
    lua_newtable(L);
    int count = 0;
    for (int i = 0; i < MAX_JOYSTICKS; i++) {
        JoystickSlot *slot = &g_state.joysticks[i];
        if (slot->connected && slot->object_ref != LUA_NOREF) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, slot->object_ref);
            lua_rawseti(L, -2, ++count);
        }
    }
    return 1;
}

static int l_joystick_getJoystickCount(lua_State *L) {
    int count = 0;
    for (int i = 0; i < MAX_JOYSTICKS; i++) {
        count += g_state.joysticks[i].connected;
    }
    lua_pushinteger(L, count);
    return 1;
}

static int l_joystick_isConnected(lua_State *L) {
    lua_pushboolean(L, live_joystick(L, 1) != NULL);
    return 1;
}

static int l_joystick_getName(lua_State *L) {
    JoystickSlot *slot = live_joystick(L, 1);
    lua_pushstring(L, slot ? slot->name : "");
    return 1;
}

static int l_joystick_getID(lua_State *L) {
    AromaJoystick *joystick = check_joystick(L, 1);
    lua_pushinteger(L, joystick->slot + 1);
    if (live_joystick(L, 1)) {
        lua_pushinteger(L, joystick->instance_id);
    } else {
        lua_pushnil(L);
    }
    return 2;
}

static int l_joystick_isGamepad(lua_State *L) {
    JoystickSlot *slot = live_joystick(L, 1);
    lua_pushboolean(L, slot && slot->mapping);
    return 1;
}

/* Without the two axes that are reported as the hat */
static int joystick_axis_count(const JoystickSlot *slot) {
    return slot->has_hat && slot->axis_count >= 2 ? slot->axis_count - 2 : slot->axis_count;
}

static int l_joystick_getAxisCount(lua_State *L) {
    JoystickSlot *slot = live_joystick(L, 1);
    lua_pushinteger(L, slot ? joystick_axis_count(slot) : 0);
    return 1;
}

static int l_joystick_getButtonCount(lua_State *L) {
    JoystickSlot *slot = live_joystick(L, 1);
    lua_pushinteger(L, slot ? slot->button_count : 0);
    return 1;
}

static int l_joystick_getHatCount(lua_State *L) {
    JoystickSlot *slot = live_joystick(L, 1);
    lua_pushinteger(L, slot && slot->has_hat ? 1 : 0);
    return 1;
}

static int l_joystick_getHat(lua_State *L) {
    static const char *const directions[16] = {
        "c", "u", "r", "ru", "d", "c", "rd", "c", "l", "lu", "c", "c", "ld", "c", "c", "c"
    };
    JoystickSlot *slot = live_joystick(L, 1);
    int hat = (int)luaL_checkinteger(L, 2);
    lua_pushstring(L, slot && hat == 1 ? directions[joystick_hat(slot)] : "c");
    return 1;
}

/* A made up SDL GUID carrying the pad's USB ids, which is the part that
 * mappings are matched by. Pads whose ids the browser hides get zeros */
static int l_joystick_getGUID(lua_State *L) {
    JoystickSlot *slot = live_joystick(L, 1);
    int vendor = slot ? slot->vendor : 0;
    int product = slot ? slot->product : 0;
    char guid[33];
    snprintf(guid, sizeof(guid), "03000000%02x%02x0000%02x%02x000000000000",
             vendor & 0xff, (vendor >> 8) & 0xff, product & 0xff, (product >> 8) & 0xff);
    lua_pushstring(L, guid);
    return 1;
}

static int l_joystick_getAxis(lua_State *L) {
    JoystickSlot *slot = live_joystick(L, 1);
    int axis = (int)luaL_checkinteger(L, 2);
    lua_pushnumber(L, slot && axis >= 1 && axis <= joystick_axis_count(slot) ? slot->axes[axis - 1] : 0.0);
    return 1;
}

static int l_joystick_getAxes(lua_State *L) {
    JoystickSlot *slot = live_joystick(L, 1);
    int count = slot ? joystick_axis_count(slot) : 0;
    luaL_checkstack(L, count, "too many axes");
    for (int i = 0; i < count; i++) {
        lua_pushnumber(L, slot->axes[i]);
    }
    return count;
}

/* isDown(button, ...) with raw button numbers from 1 */
static int l_joystick_isDown(lua_State *L) {
    JoystickSlot *slot = live_joystick(L, 1);
    int nargs = lua_gettop(L);
    int down = 0;
    for (int i = 2; i <= nargs; i++) {
        int button = (int)luaL_checkinteger(L, i);
        if (slot && button >= 1 && button <= slot->button_count && (slot->pressed & (1u << (button - 1)))) {
            down = 1;
        }
    }
    lua_pushboolean(L, down);
    return 1;
}

static int l_joystick_isGamepadDown(lua_State *L) {
    JoystickSlot *slot = live_joystick(L, 1);
    int nargs = lua_gettop(L);
    int down = 0;
    for (int i = 2; i <= nargs; i++) {
        int button = luaL_checkoption(L, i, NULL, gamepad_button_names);
        if (slot && slot->mapping && gamepad_button_down(slot, button)) {
            down = 1;
        }
    }
    lua_pushboolean(L, down);
    return 1;
}

static int l_joystick_getGamepadAxis(lua_State *L) {
    JoystickSlot *slot = live_joystick(L, 1);
    int axis = luaL_checkoption(L, 2, NULL, gamepad_axis_names);
    lua_pushnumber(L, slot && slot->mapping ? gamepad_axis_value(slot, axis) : 0.0);
    return 1;
}

static void reassign_mappings(void) {
    for (int i = 0; i < MAX_JOYSTICKS; i++) {
        JoystickSlot *slot = &g_state.joysticks[i];
        if (slot->connected && slot->mapping != &standard_mapping) {
            assign_mapping(slot, 0);
        }
    }
}

/* setGamepadMapping(guid, button or axis, inputtype, inputindex, [hatdir]).
 * Only reaches pads that the browser hasn't mapped itself */
static int l_joystick_setGamepadMapping(lua_State *L) {
    static const char *const input_types[] = {"button", "axis", "hat", NULL};
    static const char *const hat_dirs[] = {"u", "r", "d", "l", NULL};

    size_t guid_len;
    const char *guid = luaL_checklstring(L, 1, &guid_len);
    const char *target = luaL_checkstring(L, 2);
    int input_type = luaL_checkoption(L, 3, NULL, input_types);
    int index = (int)luaL_checkinteger(L, 4) - 1;
    luaL_argcheck(L, index >= 0, 4, "inputs count from 1");

    Binding binding;
    memset(&binding, 0, sizeof(binding));
    binding.index = index;
    if (input_type == 0) {
        luaL_argcheck(L, index < MAX_JOYSTICK_BUTTONS, 4, "button out of range");
        binding.type = BIND_BUTTON;
    } else if (input_type == 1) {
        luaL_argcheck(L, index < MAX_JOYSTICK_AXES, 4, "axis out of range");
        binding.type = BIND_AXIS;
        binding.from = -1.0f;
        binding.to = 1.0f;
    } else {
        binding.type = BIND_HAT;
        binding.hat_mask = 1 << luaL_checkoption(L, 5, NULL, hat_dirs);
    }

    int vendor, product;
    GamepadMapping *mapping = parse_guid(guid, guid_len, &vendor, &product) ? user_mapping_for(vendor, product) : NULL;
    if (!mapping) {
        lua_pushboolean(L, 0);
        return 1;
    }

    int found = 0;
    for (int i = 0; i < GAMEPAD_BUTTONS; i++) {
        if (strcmp(gamepad_button_names[i], target) == 0) { mapping->buttons[i] = binding; found = 1; }
    }
    for (int i = 0; i < GAMEPAD_AXES; i++) {
        if (strcmp(gamepad_axis_names[i], target) == 0) { mapping->axes[i] = binding; found = 1; }
    }
    if (!found) {
        return luaL_error(L, "Invalid gamepad axis/button: %s", target);
    }

    reassign_mappings();
    lua_pushboolean(L, 1);
    return 1;
}

/* loadGamepadMappings(text) in SDL's gamecontrollerdb format. Mappings for
 * other platforms are skipped, the raw numbering differs between them */
static int l_joystick_loadGamepadMappings(lua_State *L) {
    size_t length;
    const char *text = luaL_checklstring(L, 1, &length);
    if (!memchr(text, ',', length)) {
        return luaL_error(L, "love.joystick.loadGamepadMappings: loading from a file isn't supported yet, pass the mappings as a string");
    }

    const char *end = text + length;
    while (text < end) {
        const char *newline = memchr(text, '\n', (size_t)(end - text));
        const char *line_end = newline ? newline : end;
        size_t line_len = (size_t)(line_end - text);
        if (line_len > 0 && line_end[-1] == '\r') {
            line_len--;
        }

        GamepadMapping parsed;
        if (line_len > 0 && text[0] != '#' && parse_mapping_line(text, line_len, &parsed)) {
            GamepadMapping *mapping = user_mapping_for(parsed.vendor, parsed.product);
            if (mapping) {
                *mapping = parsed;
            }
        }
        text = line_end + 1;
    }

    reassign_mappings();
    return 0;
}

static int l_keyboard_setKeyRepeat(lua_State *L) {
    luaL_checktype(L, 1, LUA_TBOOLEAN);
    g_state.key_repeat = lua_toboolean(L, 1);
    return 0;
}

static int l_keyboard_hasKeyRepeat(lua_State *L) {
    lua_pushboolean(L, g_state.key_repeat);
    return 1;
}

static int l_mouse_getPosition(lua_State *L) {
    lua_pushinteger(L, g_state.mouse_x);
    lua_pushinteger(L, g_state.mouse_y);
    return 2;
}

static int l_mouse_getX(lua_State *L) {
    lua_pushinteger(L, g_state.mouse_x);
    return 1;
}

static int l_mouse_getY(lua_State *L) {
    lua_pushinteger(L, g_state.mouse_y);
    return 1;
}

static int l_mouse_isDown(lua_State *L) {
    int numargs = lua_gettop(L);
    int down = 0;

    for (int i = 1; i <= numargs; i++) {
        int button = (int)luaL_checkinteger(L, i);
        if (button >= 1 && button <= 32 && (g_state.mouse_buttons & (1u << (button - 1)))) {
            down = 1;
        }
    }

    lua_pushboolean(L, down);
    return 1;
}

static void set_mouse_visible(int visible) {
    g_state.mouse_hidden = !visible;
    js_set_cursor_visible(visible);
}

static int l_mouse_setVisible(lua_State *L) {
    luaL_checktype(L, 1, LUA_TBOOLEAN);
    set_mouse_visible(lua_toboolean(L, 1));
    return 0;
}

static int l_mouse_isVisible(lua_State *L) {
    lua_pushboolean(L, !g_state.mouse_hidden);
    return 1;
}

static void setup_projection(float width, float height, int flip_y);

/* A canvas is drawn into upside down so that its top row is the texture's
 * first, the same way up as a loaded image */
static void apply_render_target(void) {
    if (!g_state.gl_context) {
        return;
    }

    AromaImage *canvas = g_state.gfx.canvas;
    int width = canvas ? canvas->width : g_state.window_width;
    int height = canvas ? canvas->height : g_state.window_height;

    js_bind_framebuffer(canvas ? canvas->texture_id : 0);
    glViewport(0, 0, width, height);
    setup_projection((float)width, (float)height, canvas != NULL);
    apply_scissor();
}

static void apply_scissor(void) {
    if (!g_state.gl_context) {
        return;
    }

    if (!g_state.gfx.scissor_enabled) {
        glDisable(GL_SCISSOR_TEST);
        return;
    }

    const int *box = g_state.gfx.scissor;
    /* GL counts rows from the bottom of the window. A canvas is drawn into
     * upside down, which makes its rows already count from the top */
    int y = g_state.gfx.canvas ? box[1] : g_state.window_height - (box[1] + box[3]);
    glEnable(GL_SCISSOR_TEST);
    glScissor(box[0], y, box[2], box[3]);
}

static int set_scissor(lua_State *L, int intersect) {
    GraphicsState *gfx = &g_state.gfx;

    if (lua_gettop(L) == 0 && !intersect) {
        gfx->scissor_enabled = 0;
        apply_scissor();
        return 0;
    }

    int x = (int)luaL_checknumber(L, 1);
    int y = (int)luaL_checknumber(L, 2);
    int w = (int)luaL_checknumber(L, 3);
    int h = (int)luaL_checknumber(L, 4);
    if (w < 0 || h < 0) {
        return luaL_error(L, "Can't set scissor with negative width and/or height.");
    }

    if (intersect && gfx->scissor_enabled) {
        int x2 = x + w < gfx->scissor[0] + gfx->scissor[2] ? x + w : gfx->scissor[0] + gfx->scissor[2];
        int y2 = y + h < gfx->scissor[1] + gfx->scissor[3] ? y + h : gfx->scissor[1] + gfx->scissor[3];
        if (x < gfx->scissor[0]) x = gfx->scissor[0];
        if (y < gfx->scissor[1]) y = gfx->scissor[1];
        w = x2 > x ? x2 - x : 0;
        h = y2 > y ? y2 - y : 0;
    }

    gfx->scissor_enabled = 1;
    gfx->scissor[0] = x;
    gfx->scissor[1] = y;
    gfx->scissor[2] = w;
    gfx->scissor[3] = h;
    apply_scissor();
    return 0;
}

static int l_graphics_setScissor(lua_State *L) {
    return set_scissor(L, 0);
}

static int l_graphics_intersectScissor(lua_State *L) {
    return set_scissor(L, 1);
}

static int l_graphics_getScissor(lua_State *L) {
    if (!g_state.gfx.scissor_enabled) {
        return 0;
    }
    for (int i = 0; i < 4; i++) {
        lua_pushinteger(L, g_state.gfx.scissor[i]);
    }
    return 4;
}

/* The blend functions are love's, so that results match it */
static void apply_blend_mode(void) {
    if (!g_state.gl_context) {
        return;
    }

    GLenum src_rgb = GL_ONE, src_a = GL_ONE, dst_rgb = GL_ZERO, dst_a = GL_ZERO;
    GLenum equation = GL_FUNC_ADD;

    switch (g_state.gfx.blend_mode) {
        case BLEND_ALPHA:
            dst_rgb = dst_a = GL_ONE_MINUS_SRC_ALPHA;
            break;
        case BLEND_MULTIPLY:
            src_rgb = src_a = GL_DST_COLOR;
            break;
        case BLEND_REPLACE:
            break;
        case BLEND_SUBTRACT:
            equation = GL_FUNC_REVERSE_SUBTRACT;
            /* fall through */
        case BLEND_ADD:
            src_a = GL_ZERO;
            dst_rgb = dst_a = GL_ONE;
            break;
        case BLEND_SCREEN:
            dst_rgb = dst_a = GL_ONE_MINUS_SRC_COLOR;
            break;
    }

    if (src_rgb == GL_ONE && g_state.gfx.blend_alpha == BLEND_ALPHA_MULTIPLY) {
        src_rgb = GL_SRC_ALPHA;
    }

    glBlendEquation(equation);
    glBlendFuncSeparate(src_rgb, dst_rgb, src_a, dst_a);
}

static void set_canvas_size(int width, int height) {
    g_state.window_width = width;
    g_state.window_height = height;
    emscripten_set_canvas_element_size("#canvas", width, height);
    js_canvas_resized();
    apply_render_target();
}

static int l_graphics_setBlendMode(lua_State *L) {
    BlendMode mode = (BlendMode)luaL_checkoption(L, 1, NULL, blend_mode_names);
    BlendAlphaMode alpha = (BlendAlphaMode)luaL_checkoption(L, 2, "alphamultiply", blend_alpha_names);
    if (mode == BLEND_MULTIPLY && alpha != BLEND_PREMULTIPLIED) {
        return luaL_error(L, "love.graphics.setBlendMode: the multiply blend mode must be used with premultiplied alpha");
    }
    g_state.gfx.blend_mode = mode;
    g_state.gfx.blend_alpha = alpha;
    apply_blend_mode();
    return 0;
}

static int l_graphics_getBlendMode(lua_State *L) {
    lua_pushstring(L, blend_mode_names[g_state.gfx.blend_mode]);
    lua_pushstring(L, blend_alpha_names[g_state.gfx.blend_alpha]);
    return 2;
}

/* Starts out transparent black */
static int l_graphics_newCanvas(lua_State *L) {
    int width = (int)luaL_optinteger(L, 1, g_state.window_width);
    int height = (int)luaL_optinteger(L, 2, g_state.window_height);
    luaL_argcheck(L, width > 0, 1, "width must be positive");
    luaL_argcheck(L, height > 0, 2, "height must be positive");

    AromaImage *canvas = (AromaImage *)lua_newuserdata(L, sizeof(AromaImage));
    memset(canvas, 0, sizeof(AromaImage));
    default_texture_params(&canvas->params);
    canvas->is_canvas = 1;
    luaL_getmetatable(L, "aroma.canvas");
    lua_setmetatable(L, -2);

    canvas->texture_id = js_create_canvas(width, height);
    if (!canvas->texture_id) {
        return luaL_error(L, "love.graphics.newCanvas: failed to create a %dx%d canvas", width, height);
    }
    canvas->width = width;
    canvas->height = height;
    canvas->loaded = 1;
    apply_texture_params(canvas->texture_id, &canvas->params);

    /* Creating it bound its framebuffer */
    apply_render_target();
    return 1;
}

static void set_canvas(lua_State *L, int idx) {
    AromaImage *canvas = NULL;
    if (!lua_isnoneornil(L, idx)) {
        canvas = (AromaImage *)luaL_checkudata(L, idx, "aroma.canvas");
        if (!canvas->loaded) {
            luaL_error(L, "love.graphics.setCanvas: the canvas has been released");
        }
    }

    release_canvas_ref(L);
    g_state.gfx.canvas = canvas;
    if (canvas) {
        lua_pushvalue(L, idx);
        g_state.gfx.canvas_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    apply_render_target();
}

/* love's table form setCanvas({canvas, depth = ...}) is accepted, its
 * options are ignored for now */
static int l_graphics_setCanvas(lua_State *L) {
    if (lua_istable(L, 1)) {
        lua_rawgeti(L, 1, 1);
        set_canvas(L, lua_gettop(L));
        return 0;
    }
    set_canvas(L, 1);
    return 0;
}

static int l_graphics_getCanvas(lua_State *L) {
    if (g_state.gfx.canvas_ref == LUA_NOREF) {
        lua_pushnil(L);
    } else {
        lua_rawgeti(L, LUA_REGISTRYINDEX, g_state.gfx.canvas_ref);
    }
    return 1;
}

/* Without a color this is transparent black as of love 11, not the
 * background color */
static int l_graphics_clear(lua_State *L) {
    float color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (!lua_isnoneornil(L, 1)) {
        parse_color(L, 1, color);
    }

    if (g_state.discard_rendering) {
        return 0;
    }

    glClearColor(color[0], color[1], color[2], color[3]);
    glClear(GL_COLOR_BUFFER_BIT);
    return 0;
}

static int l_canvas_renderTo(lua_State *L) {
    luaL_checkudata(L, 1, "aroma.canvas");
    luaL_checktype(L, 2, LUA_TFUNCTION);

    l_graphics_getCanvas(L);              /* 3: previous target */
    set_canvas(L, 1);

    lua_pushvalue(L, 2);
    int status = lua_pcall(L, 0, 0, 0);

    set_canvas(L, 3);
    if (status != LUA_OK) {
        return lua_error(L);
    }
    return 0;
}

static int l_canvas_newImageData(lua_State *L) {
    AromaImage *canvas = (AromaImage *)luaL_checkudata(L, 1, "aroma.canvas");
    if (!canvas->loaded) {
        return luaL_error(L, "the canvas has been released");
    }

    lua_settop(L, 1);
    lua_pushcfunction(L, l_image_newImageData);
    lua_pushinteger(L, canvas->width);
    lua_pushinteger(L, canvas->height);
    lua_call(L, 2, 1);
    AromaImageData *data = check_image_data(L, 2);

    /* Row 0 of the framebuffer is the canvas's top row, see apply_render_target */
    js_bind_framebuffer(canvas->texture_id);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, canvas->width, canvas->height, GL_RGBA, GL_UNSIGNED_BYTE, data->pixels);
    apply_render_target();
    return 1;
}

/* The flags table is accepted and ignored. Fullscreen is asked for with a
 * size of 0, which takes the size of the desktop like it does in love */
static int l_window_setMode(lua_State *L) {
    int width = (int)luaL_checkinteger(L, 1);
    int height = (int)luaL_checkinteger(L, 2);
    luaL_argcheck(L, width >= 0, 1, "width must not be negative");
    luaL_argcheck(L, height >= 0, 2, "height must not be negative");

    if (width == 0) width = js_desktop_width();
    if (height == 0) height = js_desktop_height();

    set_canvas_size(width, height);
    lua_pushboolean(L, 1);
    return 1;
}

static int l_window_getMode(lua_State *L) {
    lua_pushinteger(L, g_state.window_width);
    lua_pushinteger(L, g_state.window_height);
    lua_newtable(L);
    return 3;
}

static int l_window_getDesktopDimensions(lua_State *L) {
    lua_pushinteger(L, js_desktop_width());
    lua_pushinteger(L, js_desktop_height());
    return 2;
}

/* Kept in the registry so getTitle works without a trip through the page */
static int l_window_setTitle(lua_State *L) {
    js_set_title(luaL_checkstring(L, 1));
    lua_settop(L, 1);
    lua_setfield(L, LUA_REGISTRYINDEX, "aroma.window_title");
    return 0;
}

static int l_window_getTitle(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "aroma.window_title");
    if (lua_isnil(L, -1)) {
        lua_pushliteral(L, "");
    }
    return 1;
}

static int l_event_quit(lua_State *L) {
    (void)L;
    g_state.quit_requested = 1;
    return 0;
}

/* There is no event queue, quit is the one event scripts push by hand */
static int l_event_push(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    if (strcmp(name, "quit") != 0) {
        return luaL_error(L, "love.event.push: only 'quit' is supported, got '%s'", name);
    }
    g_state.quit_requested = 1;
    return 0;
}

static int l_timer_getTime(lua_State *L) {
    lua_pushnumber(L, (emscripten_get_now() - g_state.start_time) * 0.001);
    return 1;
}

static int l_timer_getDelta(lua_State *L) {
    lua_pushnumber(L, g_state.dt);
    return 1;
}

static int l_timer_getFPS(lua_State *L) {
    lua_pushinteger(L, g_state.fps);
    return 1;
}

/* love.math. The generator and noise follow love 11 bit for bit so that
 * seeded content (procedural art, level layouts) matches the desktop. */

typedef struct {
    uint64_t seed;
    uint64_t state;
    /* Box-Muller makes normals in pairs, the spare is kept for the next call */
    double spare_normal;
    int has_spare_normal;
} AromaRandom;

static AromaRandom g_random;

/* Thomas Wang's 64 bit integer hash. Xorshift spreads similar seeds poorly */
static uint64_t wang_hash64(uint64_t key) {
    key = (~key) + (key << 21);
    key = key ^ (key >> 24);
    key = (key + (key << 3)) + (key << 8);
    key = key ^ (key >> 14);
    key = (key + (key << 2)) + (key << 4);
    key = key ^ (key >> 28);
    key = key + (key << 31);
    return key;
}

static void random_set_seed(AromaRandom *rng, uint64_t seed) {
    rng->seed = seed;
    do {
        seed = wang_hash64(seed);
    } while (seed == 0);
    rng->state = seed;
    rng->has_spare_normal = 0;
}

/* xorshift64*, uniform in [0, 1) */
static double random_next(AromaRandom *rng) {
    rng->state ^= rng->state >> 12;
    rng->state ^= rng->state << 25;
    rng->state ^= rng->state >> 27;
    uint64_t r = rng->state * 2685821657736338717ULL;

    union { uint64_t i; double d; } u;
    u.i = (0x3FFULL << 52) | (r >> 12);
    return u.d - 1.0;
}

static double random_next_normal(AromaRandom *rng, double stddev) {
    if (rng->has_spare_normal) {
        rng->has_spare_normal = 0;
        return rng->spare_normal * stddev;
    }

    double r = sqrt(-2.0 * log(1.0 - random_next(rng)));
    double phi = 2.0 * M_PI * (1.0 - random_next(rng));

    rng->spare_normal = r * cos(phi);
    rng->has_spare_normal = 1;
    return r * sin(phi) * stddev;
}

static AromaRandom *check_random(lua_State *L, int idx) {
    return (AromaRandom *)luaL_checkudata(L, idx, "aroma.random_generator");
}

/* A seed is one number, or the low and high 32 bits as two */
static uint64_t check_random_seed(lua_State *L, int idx) {
    if (lua_isnoneornil(L, idx + 1)) {
        double num = luaL_checknumber(L, idx);
        luaL_argcheck(L, num >= 0 && num < 18446744073709551616.0, idx, "seed out of range");
        return (uint64_t)num;
    }
    uint64_t low = (uint32_t)luaL_checknumber(L, idx);
    uint64_t high = (uint32_t)luaL_checknumber(L, idx + 1);
    return low | (high << 32);
}

/* random(), random(max) and random(min, max), the arguments start at idx */
static int push_random(lua_State *L, AromaRandom *rng, int idx) {
    double r = random_next(rng);
    double low = 1.0, high;

    switch (lua_gettop(L) - (idx - 1)) {
        case 0:
            lua_pushnumber(L, r);
            return 1;
        case 1:
            high = floor(luaL_checknumber(L, idx));
            break;
        default:
            low = floor(luaL_checknumber(L, idx));
            high = floor(luaL_checknumber(L, idx + 1));
            break;
    }

    luaL_argcheck(L, low <= high, idx, "interval is empty");
    lua_pushnumber(L, floor(r * (high - low + 1.0)) + low);
    return 1;
}

static int push_random_normal(lua_State *L, AromaRandom *rng, int idx) {
    double stddev = luaL_optnumber(L, idx, 1.0);
    double mean = luaL_optnumber(L, idx + 1, 0.0);
    lua_pushnumber(L, random_next_normal(rng, stddev) + mean);
    return 1;
}

static int push_random_seed(lua_State *L, AromaRandom *rng) {
    lua_pushnumber(L, (double)(uint32_t)(rng->seed & 0xFFFFFFFFu));
    lua_pushnumber(L, (double)(uint32_t)(rng->seed >> 32));
    return 2;
}

static int l_math_random(lua_State *L) {
    return push_random(L, &g_random, 1);
}

static int l_math_randomNormal(lua_State *L) {
    return push_random_normal(L, &g_random, 1);
}

static int l_math_setRandomSeed(lua_State *L) {
    random_set_seed(&g_random, check_random_seed(L, 1));
    return 0;
}

static int l_math_getRandomSeed(lua_State *L) {
    return push_random_seed(L, &g_random);
}

static int l_math_newRandomGenerator(lua_State *L) {
    /* love's default seed, every unseeded generator gives the same run */
    uint64_t seed = 0x0139408DCBBF7A44ULL;
    if (!lua_isnoneornil(L, 1)) {
        seed = check_random_seed(L, 1);
    }

    AromaRandom *rng = (AromaRandom *)lua_newuserdata(L, sizeof(AromaRandom));
    random_set_seed(rng, seed);
    luaL_getmetatable(L, "aroma.random_generator");
    lua_setmetatable(L, -2);
    return 1;
}

static int l_random_random(lua_State *L) {
    return push_random(L, check_random(L, 1), 2);
}

static int l_random_randomNormal(lua_State *L) {
    return push_random_normal(L, check_random(L, 1), 2);
}

static int l_random_setSeed(lua_State *L) {
    random_set_seed(check_random(L, 1), check_random_seed(L, 2));
    return 0;
}

static int l_random_getSeed(lua_State *L) {
    return push_random_seed(L, check_random(L, 1));
}

/* Simplex noise after Stefan Gustavson's simplexnoise1234, the one love uses
 * for 1 and 2 dimensions */
static const unsigned char noise_perm[256] = {
    151,160,137,91,90,15,131,13,201,95,96,53,194,233,7,225,140,36,103,30,69,
    142,8,99,37,240,21,10,23,190,6,148,247,120,234,75,0,26,197,62,94,252,219,
    203,117,35,11,32,57,177,33,88,237,149,56,87,174,20,125,136,171,168,68,175,
    74,165,71,134,139,48,27,166,77,146,158,231,83,111,229,122,60,211,133,230,
    220,105,92,41,55,46,245,40,244,102,143,54,65,25,63,161,1,216,80,73,209,76,
    132,187,208,89,18,169,200,196,135,130,116,188,159,86,164,100,109,198,173,
    186,3,64,52,217,226,250,124,123,5,202,38,147,118,126,255,82,85,212,207,
    206,59,227,47,16,58,17,182,189,28,42,223,183,170,213,119,248,152,2,44,154,
    163,70,221,153,101,155,167,43,172,9,129,22,39,253,19,98,108,110,79,113,
    224,232,178,185,112,104,218,246,97,228,251,34,242,193,238,210,144,12,191,
    179,162,241,81,51,145,235,249,14,239,107,49,192,214,31,181,199,106,157,
    184,84,204,176,115,121,50,45,127,4,150,254,138,236,205,93,222,114,67,29,
    24,72,243,141,128,195,78,66,215,61,156,180
};

static int noise_hash(int i) {
    return noise_perm[i & 0xff];
}

static int noise_floor(float x) {
    return x > 0 ? (int)x : (int)x - 1;
}

static float noise_grad1(int hash, float x) {
    int h = hash & 15;
    float grad = 1.0f + (h & 7);
    if (h & 8) grad = -grad;
    return grad * x;
}

static float noise_grad2(int hash, float x, float y) {
    int h = hash & 7;
    float u = h < 4 ? x : y;
    float v = h < 4 ? y : x;
    return ((h & 1) ? -u : u) + ((h & 2) ? -2.0f * v : 2.0f * v);
}

static float simplex_noise1(float x) {
    int i0 = noise_floor(x);
    int i1 = i0 + 1;
    float x0 = x - i0;
    float x1 = x0 - 1.0f;

    float t0 = 1.0f - x0 * x0;
    t0 *= t0;
    float n0 = t0 * t0 * noise_grad1(noise_hash(i0), x0);

    float t1 = 1.0f - x1 * x1;
    t1 *= t1;
    float n1 = t1 * t1 * noise_grad1(noise_hash(i1), x1);

    return 0.395f * (n0 + n1);
}

static float simplex_corner2(int hash, float x, float y) {
    float t = 0.5f - x * x - y * y;
    if (t < 0.0f) {
        return 0.0f;
    }
    t *= t;
    return t * t * noise_grad2(hash, x, y);
}

static float simplex_noise2(float x, float y) {
    /* Doubles, as in the original: the float math around them widens and
     * rounds differently with float constants, and the output drifts from
     * love's in the 6th place */
    const double F2 = 0.366025403; /* 0.5 * (sqrt(3) - 1) */
    const double G2 = 0.211324865; /* (3 - sqrt(3)) / 6 */

    /* Skew into the grid of squares to find which cell we're in */
    float s = (x + y) * F2;
    float xs = x + s;
    float ys = y + s;
    int i = noise_floor(xs);
    int j = noise_floor(ys);

    float t = (float)(i + j) * G2;
    float origin_x = i - t;
    float origin_y = j - t;
    float x0 = x - origin_x;
    float y0 = y - origin_y;

    /* Lower triangle goes through (1, 0), upper through (0, 1) */
    int i1 = x0 > y0 ? 1 : 0;
    int j1 = 1 - i1;

    float x1 = x0 - i1 + G2;
    float y1 = y0 - j1 + G2;
    float x2 = x0 - 1.0f + 2.0f * G2;
    float y2 = y0 - 1.0f + 2.0f * G2;

    float n0 = simplex_corner2(noise_hash(i + noise_hash(j)), x0, y0);
    float n1 = simplex_corner2(noise_hash(i + i1 + noise_hash(j + j1)), x1, y1);
    float n2 = simplex_corner2(noise_hash(i + 1 + noise_hash(j + 1)), x2, y2);

    return 45.23f * (n0 + n1 + n2);
}

static int l_math_noise(lua_State *L) {
    int nargs = lua_gettop(L);
    float n;

    if (nargs <= 1) {
        n = simplex_noise1((float)luaL_checknumber(L, 1));
    } else if (nargs == 2) {
        n = simplex_noise2((float)luaL_checknumber(L, 1), (float)luaL_checknumber(L, 2));
    } else {
        return luaL_error(L, "love.math.noise: only 1 and 2 dimensions are supported");
    }

    lua_pushnumber(L, n * 0.5f + 0.5f);
    return 1;
}

static void register_aroma_api(lua_State *L) {
    if (luaL_newmetatable(L, "aroma.image")) {
        lua_pushcfunction(L, l_image_release);
        lua_setfield(L, -2, "__gc");

        lua_newtable(L);
        lua_pushcfunction(L, l_image_getWidth);
        lua_setfield(L, -2, "getWidth");
        lua_pushcfunction(L, l_image_getHeight);
        lua_setfield(L, -2, "getHeight");
        lua_pushcfunction(L, l_image_getDimensions);
        lua_setfield(L, -2, "getDimensions");
        lua_pushcfunction(L, l_image_setFilter);
        lua_setfield(L, -2, "setFilter");
        lua_pushcfunction(L, l_image_getFilter);
        lua_setfield(L, -2, "getFilter");
        lua_pushcfunction(L, l_image_setWrap);
        lua_setfield(L, -2, "setWrap");
        lua_pushcfunction(L, l_image_getWrap);
        lua_setfield(L, -2, "getWrap");
        lua_pushcfunction(L, l_image_release);
        lua_setfield(L, -2, "release");
        register_object_type(L, (const char *const[]){"Image", "Texture", "Drawable", "Object", NULL});
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);

    if (luaL_newmetatable(L, "aroma.canvas")) {
        lua_pushcfunction(L, l_image_release);
        lua_setfield(L, -2, "__gc");

        lua_newtable(L);
        lua_pushcfunction(L, l_image_getWidth);
        lua_setfield(L, -2, "getWidth");
        lua_pushcfunction(L, l_image_getHeight);
        lua_setfield(L, -2, "getHeight");
        lua_pushcfunction(L, l_image_getDimensions);
        lua_setfield(L, -2, "getDimensions");
        lua_pushcfunction(L, l_image_setFilter);
        lua_setfield(L, -2, "setFilter");
        lua_pushcfunction(L, l_image_getFilter);
        lua_setfield(L, -2, "getFilter");
        lua_pushcfunction(L, l_image_setWrap);
        lua_setfield(L, -2, "setWrap");
        lua_pushcfunction(L, l_image_getWrap);
        lua_setfield(L, -2, "getWrap");
        lua_pushcfunction(L, l_image_release);
        lua_setfield(L, -2, "release");
        lua_pushcfunction(L, l_canvas_renderTo);
        lua_setfield(L, -2, "renderTo");
        lua_pushcfunction(L, l_canvas_newImageData);
        lua_setfield(L, -2, "newImageData");
        register_object_type(L, (const char *const[]){"Canvas", "Texture", "Drawable", "Object", NULL});
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);

    if (luaL_newmetatable(L, "aroma.joystick")) {
        static const luaL_Reg joystick_methods[] = {
            {"isConnected", l_joystick_isConnected},
            {"getName", l_joystick_getName},
            {"getID", l_joystick_getID},
            {"isGamepad", l_joystick_isGamepad},
            {"getAxisCount", l_joystick_getAxisCount},
            {"getButtonCount", l_joystick_getButtonCount},
            {"getHatCount", l_joystick_getHatCount},
            {"getHat", l_joystick_getHat},
            {"getGUID", l_joystick_getGUID},
            {"getAxis", l_joystick_getAxis},
            {"getAxes", l_joystick_getAxes},
            {"isDown", l_joystick_isDown},
            {"isGamepadDown", l_joystick_isGamepadDown},
            {"getGamepadAxis", l_joystick_getGamepadAxis},
            {NULL, NULL}
        };

        luaL_newlib(L, joystick_methods);
        register_object_type(L, (const char *const[]){"Joystick", "Object", NULL});
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);

    if (luaL_newmetatable(L, "aroma.source")) {
        static const luaL_Reg source_methods[] = {
            {"clone", l_source_clone},
            {"play", l_source_play},
            {"pause", l_source_pause},
            {"stop", l_source_stop},
            {"isPlaying", l_source_isPlaying},
            {"setVolume", l_source_setVolume},
            {"getVolume", l_source_getVolume},
            {"getVolumeLimits", l_source_getVolumeLimits},
            {"setPitch", l_source_setPitch},
            {"getPitch", l_source_getPitch},
            {"setLooping", l_source_setLooping},
            {"isLooping", l_source_isLooping},
            {"seek", l_source_seek},
            {"tell", l_source_tell},
            {"getDuration", l_source_getDuration},
            {"release", l_source_release},
            {NULL, NULL}
        };

        lua_pushcfunction(L, l_source_release);
        lua_setfield(L, -2, "__gc");

        luaL_newlib(L, source_methods);
        register_object_type(L, (const char *const[]){"Source", "Object", NULL});
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);

    if (luaL_newmetatable(L, "aroma.mesh")) {
        lua_pushcfunction(L, l_mesh_release);
        lua_setfield(L, -2, "__gc");

        lua_newtable(L);
        lua_pushcfunction(L, l_mesh_setTexture);
        lua_setfield(L, -2, "setTexture");
        lua_pushcfunction(L, l_mesh_getTexture);
        lua_setfield(L, -2, "getTexture");
        lua_pushcfunction(L, l_mesh_getVertexCount);
        lua_setfield(L, -2, "getVertexCount");
        lua_pushcfunction(L, l_mesh_getVertex);
        lua_setfield(L, -2, "getVertex");
        lua_pushcfunction(L, l_mesh_setVertex);
        lua_setfield(L, -2, "setVertex");
        lua_pushcfunction(L, l_mesh_release);
        lua_setfield(L, -2, "release");
        register_object_type(L, (const char *const[]){"Mesh", "Drawable", "Object", NULL});
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);

    if (luaL_newmetatable(L, "aroma.image_data")) {
        lua_pushcfunction(L, l_imagedata_release);
        lua_setfield(L, -2, "__gc");

        lua_newtable(L);
        lua_pushcfunction(L, l_imagedata_getWidth);
        lua_setfield(L, -2, "getWidth");
        lua_pushcfunction(L, l_imagedata_getHeight);
        lua_setfield(L, -2, "getHeight");
        lua_pushcfunction(L, l_imagedata_getDimensions);
        lua_setfield(L, -2, "getDimensions");
        lua_pushcfunction(L, l_imagedata_getPixel);
        lua_setfield(L, -2, "getPixel");
        lua_pushcfunction(L, l_imagedata_setPixel);
        lua_setfield(L, -2, "setPixel");
        lua_pushcfunction(L, l_imagedata_mapPixel);
        lua_setfield(L, -2, "mapPixel");
        lua_pushcfunction(L, l_imagedata_paste);
        lua_setfield(L, -2, "paste");
        lua_pushcfunction(L, l_imagedata_release);
        lua_setfield(L, -2, "release");
        register_object_type(L, (const char *const[]){"ImageData", "Data", "Object", NULL});
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);

    if (luaL_newmetatable(L, "aroma.quad")) {
        lua_newtable(L);
        lua_pushcfunction(L, l_quad_getViewport);
        lua_setfield(L, -2, "getViewport");
        lua_pushcfunction(L, l_quad_setViewport);
        lua_setfield(L, -2, "setViewport");
        lua_pushcfunction(L, l_quad_getTextureDimensions);
        lua_setfield(L, -2, "getTextureDimensions");
        register_object_type(L, (const char *const[]){"Quad", "Object", NULL});
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);

    if (luaL_newmetatable(L, "aroma.font")) {
        lua_pushcfunction(L, l_font_gc);
        lua_setfield(L, -2, "__gc");

        lua_newtable(L);
        lua_pushcfunction(L, l_font_getWidth);
        lua_setfield(L, -2, "getWidth");
        lua_pushcfunction(L, l_font_getHeight);
        lua_setfield(L, -2, "getHeight");
        lua_pushcfunction(L, l_font_getLineHeight);
        lua_setfield(L, -2, "getLineHeight");
        lua_pushcfunction(L, l_font_setLineHeight);
        lua_setfield(L, -2, "setLineHeight");
        lua_pushcfunction(L, l_font_getWrap);
        lua_setfield(L, -2, "getWrap");
        lua_pushcfunction(L, l_font_hasGlyphs);
        lua_setfield(L, -2, "hasGlyphs");
        lua_pushcfunction(L, l_font_setFilter);
        lua_setfield(L, -2, "setFilter");
        lua_pushcfunction(L, l_font_getFilter);
        lua_setfield(L, -2, "getFilter");
        register_object_type(L, (const char *const[]){"Font", "Object", NULL});
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);

    if (luaL_newmetatable(L, "aroma.random_generator")) {
        lua_newtable(L);
        lua_pushcfunction(L, l_random_random);
        lua_setfield(L, -2, "random");
        lua_pushcfunction(L, l_random_randomNormal);
        lua_setfield(L, -2, "randomNormal");
        lua_pushcfunction(L, l_random_setSeed);
        lua_setfield(L, -2, "setSeed");
        lua_pushcfunction(L, l_random_getSeed);
        lua_setfield(L, -2, "getSeed");
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);

    lua_newtable(L);                /* aroma */
    lua_newtable(L);                /* aroma.graphics */

    lua_pushcfunction(L, l_graphics_setBackgroundColor);
    lua_setfield(L, -2, "setBackgroundColor");

    lua_pushcfunction(L, l_graphics_setColor);
    lua_setfield(L, -2, "setColor");

    lua_pushcfunction(L, l_graphics_getColor);
    lua_setfield(L, -2, "getColor");

    lua_pushcfunction(L, l_graphics_push);
    lua_setfield(L, -2, "push");

    lua_pushcfunction(L, l_graphics_pop);
    lua_setfield(L, -2, "pop");

    lua_pushcfunction(L, l_graphics_translate);
    lua_setfield(L, -2, "translate");

    lua_pushcfunction(L, l_graphics_rotate);
    lua_setfield(L, -2, "rotate");

    lua_pushcfunction(L, l_graphics_polygon);
    lua_setfield(L, -2, "polygon");

    lua_pushcfunction(L, l_graphics_getBackgroundColor);
    lua_setfield(L, -2, "getBackgroundColor");

    lua_pushcfunction(L, l_graphics_origin);
    lua_setfield(L, -2, "origin");

    lua_pushcfunction(L, l_graphics_scale);
    lua_setfield(L, -2, "scale");

    lua_pushcfunction(L, l_graphics_line);
    lua_setfield(L, -2, "line");

    lua_pushcfunction(L, l_graphics_circle);
    lua_setfield(L, -2, "circle");

    lua_pushcfunction(L, l_graphics_ellipse);
    lua_setfield(L, -2, "ellipse");

    lua_pushcfunction(L, l_graphics_arc);
    lua_setfield(L, -2, "arc");

    lua_pushcfunction(L, l_graphics_setLineWidth);
    lua_setfield(L, -2, "setLineWidth");

    lua_pushcfunction(L, l_graphics_getLineWidth);
    lua_setfield(L, -2, "getLineWidth");

    lua_pushcfunction(L, l_graphics_newImage);
    lua_setfield(L, -2, "newImage");

    lua_pushcfunction(L, l_graphics_draw);
    lua_setfield(L, -2, "draw");

    lua_pushcfunction(L, l_graphics_newQuad);
    lua_setfield(L, -2, "newQuad");

    lua_pushcfunction(L, l_graphics_newMesh);
    lua_setfield(L, -2, "newMesh");

    lua_pushcfunction(L, l_graphics_newCanvas);
    lua_setfield(L, -2, "newCanvas");

    lua_pushcfunction(L, l_graphics_setCanvas);
    lua_setfield(L, -2, "setCanvas");

    lua_pushcfunction(L, l_graphics_getCanvas);
    lua_setfield(L, -2, "getCanvas");

    lua_pushcfunction(L, l_graphics_clear);
    lua_setfield(L, -2, "clear");

    lua_pushcfunction(L, l_graphics_setScissor);
    lua_setfield(L, -2, "setScissor");

    lua_pushcfunction(L, l_graphics_intersectScissor);
    lua_setfield(L, -2, "intersectScissor");

    lua_pushcfunction(L, l_graphics_getScissor);
    lua_setfield(L, -2, "getScissor");

    lua_pushcfunction(L, l_graphics_setBlendMode);
    lua_setfield(L, -2, "setBlendMode");

    lua_pushcfunction(L, l_graphics_getBlendMode);
    lua_setfield(L, -2, "getBlendMode");

    lua_pushcfunction(L, l_graphics_setDefaultFilter);
    lua_setfield(L, -2, "setDefaultFilter");

    lua_pushcfunction(L, l_graphics_getDefaultFilter);
    lua_setfield(L, -2, "getDefaultFilter");

    lua_pushcfunction(L, l_graphics_getWidth);
    lua_setfield(L, -2, "getWidth");

    lua_pushcfunction(L, l_graphics_getHeight);
    lua_setfield(L, -2, "getHeight");

    lua_pushcfunction(L, l_graphics_getDimensions);
    lua_setfield(L, -2, "getDimensions");

    lua_pushcfunction(L, l_graphics_rectangle);
    lua_setfield(L, -2, "rectangle");

    lua_pushcfunction(L, l_graphics_newImageFont);
    lua_setfield(L, -2, "newImageFont");

    lua_pushcfunction(L, l_graphics_setFont);
    lua_setfield(L, -2, "setFont");

    lua_pushcfunction(L, l_graphics_getFont);
    lua_setfield(L, -2, "getFont");

    lua_pushcfunction(L, l_graphics_print);
    lua_setfield(L, -2, "print");

    lua_pushcfunction(L, l_graphics_printf);
    lua_setfield(L, -2, "printf");

    lua_setfield(L, -2, "graphics"); /* aroma.graphics = table */

    lua_newtable(L);                /* aroma.keyboard */
    lua_pushcfunction(L, l_keyboard_isDown);
    lua_setfield(L, -2, "isDown");
    lua_pushcfunction(L, l_keyboard_setKeyRepeat);
    lua_setfield(L, -2, "setKeyRepeat");
    lua_pushcfunction(L, l_keyboard_hasKeyRepeat);
    lua_setfield(L, -2, "hasKeyRepeat");
    lua_setfield(L, -2, "keyboard"); /* aroma.keyboard = table */

    lua_newtable(L);                /* aroma.joystick */
    lua_pushcfunction(L, l_joystick_getJoysticks);
    lua_setfield(L, -2, "getJoysticks");
    lua_pushcfunction(L, l_joystick_getJoystickCount);
    lua_setfield(L, -2, "getJoystickCount");
    lua_pushcfunction(L, l_joystick_setGamepadMapping);
    lua_setfield(L, -2, "setGamepadMapping");
    lua_pushcfunction(L, l_joystick_loadGamepadMappings);
    lua_setfield(L, -2, "loadGamepadMappings");
    lua_setfield(L, -2, "joystick"); /* aroma.joystick = table */

    lua_newtable(L);                /* aroma.audio */
    lua_pushcfunction(L, l_audio_newSource);
    lua_setfield(L, -2, "newSource");
    lua_pushcfunction(L, l_audio_setVolume);
    lua_setfield(L, -2, "setVolume");
    lua_pushcfunction(L, l_audio_getVolume);
    lua_setfield(L, -2, "getVolume");
    lua_pushcfunction(L, l_audio_stop);
    lua_setfield(L, -2, "stop");
    lua_setfield(L, -2, "audio");    /* aroma.audio = table */

    lua_newtable(L);                /* aroma.image */
    lua_pushcfunction(L, l_image_newImageData);
    lua_setfield(L, -2, "newImageData");
    lua_setfield(L, -2, "image");    /* aroma.image = table */

    lua_newtable(L);                /* aroma.mouse */
    lua_pushcfunction(L, l_mouse_getPosition);
    lua_setfield(L, -2, "getPosition");
    lua_pushcfunction(L, l_mouse_getX);
    lua_setfield(L, -2, "getX");
    lua_pushcfunction(L, l_mouse_getY);
    lua_setfield(L, -2, "getY");
    lua_pushcfunction(L, l_mouse_isDown);
    lua_setfield(L, -2, "isDown");
    lua_pushcfunction(L, l_mouse_setVisible);
    lua_setfield(L, -2, "setVisible");
    lua_pushcfunction(L, l_mouse_isVisible);
    lua_setfield(L, -2, "isVisible");
    lua_setfield(L, -2, "mouse");    /* aroma.mouse = table */

    lua_newtable(L);                /* aroma.window */
    lua_pushcfunction(L, l_window_setMode);
    lua_setfield(L, -2, "setMode");
    lua_pushcfunction(L, l_window_getMode);
    lua_setfield(L, -2, "getMode");
    lua_pushcfunction(L, l_window_getDesktopDimensions);
    lua_setfield(L, -2, "getDesktopDimensions");
    lua_pushcfunction(L, l_window_setTitle);
    lua_setfield(L, -2, "setTitle");
    lua_pushcfunction(L, l_window_getTitle);
    lua_setfield(L, -2, "getTitle");
    lua_setfield(L, -2, "window");   /* aroma.window = table */

    lua_newtable(L);                /* aroma.event */
    lua_pushcfunction(L, l_event_quit);
    lua_setfield(L, -2, "quit");
    lua_pushcfunction(L, l_event_push);
    lua_setfield(L, -2, "push");
    lua_setfield(L, -2, "event");    /* aroma.event = table */

    lua_newtable(L);                /* aroma.timer */
    lua_pushcfunction(L, l_timer_getTime);
    lua_setfield(L, -2, "getTime");
    lua_pushcfunction(L, l_timer_getDelta);
    lua_setfield(L, -2, "getDelta");
    lua_pushcfunction(L, l_timer_getFPS);
    lua_setfield(L, -2, "getFPS");
    lua_setfield(L, -2, "timer");    /* aroma.timer = table */

    lua_newtable(L);                /* aroma.math */
    lua_pushcfunction(L, l_math_random);
    lua_setfield(L, -2, "random");
    lua_pushcfunction(L, l_math_randomNormal);
    lua_setfield(L, -2, "randomNormal");
    lua_pushcfunction(L, l_math_setRandomSeed);
    lua_setfield(L, -2, "setRandomSeed");
    lua_pushcfunction(L, l_math_getRandomSeed);
    lua_setfield(L, -2, "getRandomSeed");
    lua_pushcfunction(L, l_math_newRandomGenerator);
    lua_setfield(L, -2, "newRandomGenerator");
    lua_pushcfunction(L, l_math_noise);
    lua_setfield(L, -2, "noise");
    lua_setfield(L, -2, "math");     /* aroma.math = table */

    lua_setglobal(L, "aroma");

    /* Create "love" alias for compatibility */
    lua_getglobal(L, "aroma");
    lua_setglobal(L, "love");
}


static void report_lua_error(lua_State *L) {
    const char *msg = lua_tostring(L, -1);
    fprintf(stderr, "Lua error: %s\n", msg ? msg : "(unknown)");
    lua_pop(L, 1);
}

static void script_thread_finish(void) {
    lua_State *T = g_state.script_thread;
    if (g_state.L && T) {
        /* A thread that ended LUA_OK can host the next entry point; errored
         * and abandoned-suspended threads are dead in Lua 5.2 and must be
         * dropped. The idle ref keeps the cached thread anchored. */
        if (lua_status(T) == LUA_OK && !g_state.idle_thread) {
            lua_settop(T, 0);
            g_state.idle_thread = T;
            g_state.idle_thread_ref = g_state.script_thread_ref;
        } else if (g_state.script_thread_ref != LUA_NOREF) {
            luaL_unref(g_state.L, LUA_REGISTRYINDEX, g_state.script_thread_ref);
        }
    }
    g_state.script_thread_ref = LUA_NOREF;
    g_state.script_thread = NULL;
    g_state.script_entry_point = SCRIPT_ENTRY_NONE;
}

static int script_thread_run(int nargs) {
    lua_State *T = g_state.script_thread;
    g_state.resource_wait = 0;
    int status = lua_resume(T, NULL, nargs);
    if (status == LUA_YIELD) {
        if (g_state.resource_wait) {
            return status; /* suspended on a resource load; the completion callback resumes */
        }
        fprintf(stderr, "Lua error: script yielded outside of a resource load\n");
        status = LUA_ERRRUN;
    } else if (status != LUA_OK) {
        report_lua_error(T);
    }
    script_thread_finish();
    return status;
}

/* Returns a fresh coroutine to run a script entry point on, or NULL while the
 * previous entry point is still suspended on a resource load (callbacks fired
 * in that window are dropped). */
static lua_State *script_thread_begin(ScriptEntryPoint entry_point) {
    if (!g_state.L || g_state.script_thread) {
        return NULL;
    }
    lua_State *T;
    if (g_state.idle_thread) {
        T = g_state.idle_thread;
        g_state.script_thread_ref = g_state.idle_thread_ref;
        g_state.idle_thread = NULL;
        g_state.idle_thread_ref = LUA_NOREF;
    } else {
        T = lua_newthread(g_state.L);
        g_state.script_thread_ref = luaL_ref(g_state.L, LUA_REGISTRYINDEX);
    }
    g_state.script_thread = T;
    g_state.script_entry_point = entry_point;
    return T;
}

static void resume_resource_wait(void) {
    if (!g_state.script_thread) {
        return;
    }

    int discard_rendering = g_state.script_entry_point == SCRIPT_ENTRY_DRAW;
    g_state.discard_rendering = discard_rendering;
    script_thread_run(0);
    g_state.discard_rendering = 0;
}

/* Callbacks resolve through the global each call so scripts that reassign
 * aroma/love wholesale still get their handlers found. */
static int push_aroma_callback(lua_State *T, const char *name) {
    lua_getglobal(T, "aroma");
    if (!lua_istable(T, -1)) {
        lua_pop(T, 1);
        return 0;
    }
    lua_getfield(T, -1, name);
    lua_remove(T, -2);
    if (!lua_isfunction(T, -1)) {
        lua_pop(T, 1);
        return 0;
    }
    return 1;
}

static void call_aroma_update(float dt) {
    lua_State *T = script_thread_begin(SCRIPT_ENTRY_UPDATE);
    if (!T) return;
    if (!push_aroma_callback(T, "update")) {
        script_thread_finish();
        return;
    }
    lua_pushnumber(T, dt);
    script_thread_run(1);
}

static void call_aroma_draw(void) {
    lua_State *T = script_thread_begin(SCRIPT_ENTRY_DRAW);
    if (!T) return;
    if (!push_aroma_callback(T, "draw")) {
        script_thread_finish();
        return;
    }
    script_thread_run(0);
}

static int finish_quit(void);

/* Scancodes aren't tracked apart from keys, the key stands in for both.
 * is_repeat is negative for keyreleased, which doesn't take one */
static void call_aroma_key_event(const char *name, const char *key, int is_repeat) {
    lua_State *T = script_thread_begin(SCRIPT_ENTRY_KEY_EVENT);
    if (!T) return;
    if (!push_aroma_callback(T, name)) {
        script_thread_finish();
        return;
    }
    lua_pushstring(T, key);
    lua_pushstring(T, key);
    int nargs = 2;
    if (is_repeat >= 0) {
        lua_pushboolean(T, is_repeat);
        nargs++;
    }
    script_thread_run(nargs);
    finish_quit();
}

EMSCRIPTEN_KEEPALIVE void aroma_source_loaded(int generation, uintptr_t source_ptr, int source_id, double duration) {
    if (generation != g_state.generation || !g_state.L) {
        if (source_id) {
            js_audio_release(source_id);
        }
        return;
    }

    AromaSource *source = (AromaSource *)source_ptr;
    source->source_id = source_id;
    source->duration = duration;
    source->loaded = source_id != 0;

    if (g_state.script_thread) {
        resume_resource_wait();
    }
}

EMSCRIPTEN_KEEPALIVE void aroma_image_loaded(int generation, uintptr_t image_ptr, int texture_id, int width, int height) {
    if (generation != g_state.generation || !g_state.L) {
        /* Load finished after the requesting Lua state was torn down. */
        if (texture_id) {
            js_release_texture(texture_id);
        }
        return;
    }

    AromaImage *img = (AromaImage *)image_ptr;
    img->texture_id = texture_id;
    img->width = width;
    img->height = height;
    img->loaded = texture_id != 0;
    apply_texture_params(texture_id, &img->params);

    if (g_state.script_thread) {
        resume_resource_wait();
    }
}

static int32_t glyph_staging[MAX_GLYPHS * 3];

EMSCRIPTEN_KEEPALIVE int32_t *get_glyph_buffer(void) {
    return glyph_staging;
}

EMSCRIPTEN_KEEPALIVE int aroma_font_max_glyphs(void) {
    return MAX_GLYPHS;
}

EMSCRIPTEN_KEEPALIVE void aroma_font_set_glyphs(int generation, uintptr_t font_ptr, int texture_id, int width, int height, int glyph_count) {
    if (generation != g_state.generation || !g_state.L) {
        if (texture_id) {
            js_release_texture(texture_id);
        }
        return;
    }

    AromaFont *font = (AromaFont *)font_ptr;
    font->texture_id = texture_id;
    font->image_width = width;
    font->image_height = height;

    if (glyph_count > MAX_GLYPHS) {
        glyph_count = MAX_GLYPHS;
    }
    font->glyph_count = glyph_count;
    for (int i = 0; i < glyph_count; i++) {
        font->glyphs[i].x = glyph_staging[i * 3 + 0];
        font->glyphs[i].width = glyph_staging[i * 3 + 1];
        font->glyphs[i].codepoint = (uint32_t)glyph_staging[i * 3 + 2];
    }

    font->line_height = 1.0f;
    apply_texture_params(texture_id, &font->params);
    font->loaded = texture_id != 0 && glyph_count > 0;

    if (g_state.script_thread) {
        resume_resource_wait();
    }
}

static GLuint compile_shader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint status = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        char info_log[512];
        glGetShaderInfoLog(shader, sizeof(info_log), NULL, info_log);
        fprintf(stderr, "Shader compile error: %s\n", info_log);
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

static int init_program(void) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex_shader_source);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fragment_shader_source);

    if (!vs || !fs) {
        return 0;
    }

    g_state.program = glCreateProgram();
    glAttachShader(g_state.program, vs);
    glAttachShader(g_state.program, fs);
    glBindAttribLocation(g_state.program, 0, "aPosition");
    glBindAttribLocation(g_state.program, 1, "aTexCoord");
    glBindAttribLocation(g_state.program, 2, "aColor");
    glLinkProgram(g_state.program);

    GLint status = GL_FALSE;
    glGetProgramiv(g_state.program, GL_LINK_STATUS, &status);
    if (!status) {
        char info_log[512];
        glGetProgramInfoLog(g_state.program, sizeof(info_log), NULL, info_log);
        fprintf(stderr, "Program link error: %s\n", info_log);
        return 0;
    }

    g_state.transform_loc = glGetUniformLocation(g_state.program, "uTransform");
    g_state.projection_loc = glGetUniformLocation(g_state.program, "uProjection");
    g_state.color_loc = glGetUniformLocation(g_state.program, "uColor");
    g_state.use_texture_loc = glGetUniformLocation(g_state.program, "uUseTexture");
    g_state.sampler_loc = glGetUniformLocation(g_state.program, "uTexture");

    /* Only meshes have colors of their own. Everything else leaves the array
     * off and gets this constant */
    glVertexAttrib4f(2, 1.0f, 1.0f, 1.0f, 1.0f);

    glUseProgram(g_state.program);
    if (g_state.sampler_loc >= 0) {
        glUniform1i(g_state.sampler_loc, 0);
    }
    if (g_state.use_texture_loc >= 0) {
        glUniform1i(g_state.use_texture_loc, 0);
    }

    glDeleteShader(vs);
    glDeleteShader(fs);

    return 1;
}

/* Pixels with y down to clip space. flip_y leaves y pointing down in clip
 * space too, which is up in a framebuffer's texture */
static void setup_projection(float width, float height, int flip_y) {
    float *p = g_state.projection;
    memset(p, 0, sizeof(float) * 9);
    p[0] = 2.0f / width;
    p[4] = flip_y ? 2.0f / height : -2.0f / height;
    p[6] = -1.0f;
    p[7] = flip_y ? -1.0f : 1.0f;
    p[8] = 1.0f;
}

static int init_webgl(void) {
    EmscriptenWebGLContextAttributes attrs;
    emscripten_webgl_init_context_attributes(&attrs);
    attrs.majorVersion = 1;
    attrs.minorVersion = 0;
    attrs.alpha = EM_TRUE;
    attrs.depth = EM_FALSE;
    attrs.stencil = EM_FALSE;

    g_state.gl_context = emscripten_webgl_create_context("#canvas", &attrs);
    if (!g_state.gl_context) {
        fprintf(stderr, "Failed to create WebGL context\n");
        return 0;
    }

    if (emscripten_webgl_make_context_current(g_state.gl_context) != EMSCRIPTEN_RESULT_SUCCESS) {
        fprintf(stderr, "Failed to set current WebGL context\n");
        return 0;
    }

    set_canvas_size(DEFAULT_WIDTH, DEFAULT_HEIGHT);

    glEnable(GL_BLEND);
    apply_blend_mode();

    return 1;
}

/* Every frame starts from an empty stack, a push left open by the last one
 * is popped so what push("all") saved isn't lost with it */
static void reset_graphics_state(void) {
    while (g_state.stack_top > 0) {
        pop_stack_entry(g_state.L);
    }
    mat3_identity(&g_state.stack[0].matrix);

    /* Likewise a canvas that was left set, the frame is drawn to the window */
    if (g_state.gfx.canvas) {
        release_canvas_ref(g_state.L);
        g_state.gfx.canvas = NULL;
        apply_render_target();
    }
}

static void default_graphics_state(void) {
    GraphicsState *gfx = &g_state.gfx;
    gfx->bg_color[0] = gfx->bg_color[1] = gfx->bg_color[2] = 0.0f;
    gfx->bg_color[3] = 1.0f;
    gfx->draw_color[0] = gfx->draw_color[1] = gfx->draw_color[2] = gfx->draw_color[3] = 1.0f;
    gfx->line_width = 1.0f;
    gfx->font = NULL;
    gfx->font_ref = LUA_NOREF;
    gfx->canvas = NULL;
    gfx->canvas_ref = LUA_NOREF;
    gfx->blend_mode = BLEND_ALPHA;
    gfx->blend_alpha = BLEND_ALPHA_MULTIPLY;
    gfx->scissor_enabled = 0;
    g_state.default_min = FILTER_LINEAR;
    g_state.default_mag = FILTER_LINEAR;

    /* Anything the stack saved was anchored in a lua state that is gone */
    memset(g_state.stack, 0, sizeof(g_state.stack));
    g_state.stack_top = 0;
    mat3_identity(&g_state.stack[0].matrix);

    apply_render_target();
    apply_blend_mode();
}

static void main_loop(void *userdata) {
    (void)userdata;
    double now = emscripten_get_now();
    float dt = (float)((now - g_state.last_time) * 0.001);
    if (dt > MAX_DT) {
        dt = MAX_DT;
    }
    g_state.last_time = now;
    g_state.dt = dt;

    g_state.fps_frames++;
    if (now - g_state.fps_window_start >= 1000.0) {
        g_state.fps = (int)(g_state.fps_frames * 1000.0 / (now - g_state.fps_window_start) + 0.5);
        g_state.fps_frames = 0;
        g_state.fps_window_start = now;
    }

    /* Leave a suspended entry point and its graphics stack untouched.
     * Resource completions drain independently of requestAnimationFrame. */
    if (g_state.script_thread) {
        return;
    }

    poll_joysticks();
    if (g_state.script_thread) {
        return;
    }

    call_aroma_update(dt);

    if (finish_quit() || g_state.script_thread) {
        return;
    }

    reset_graphics_state();

    glClearColor(g_state.gfx.bg_color[0], g_state.gfx.bg_color[1], g_state.gfx.bg_color[2], g_state.gfx.bg_color[3]);
    glClear(GL_COLOR_BUFFER_BIT);

    call_aroma_draw();
    finish_quit();
}

static const char *bootstrap_source =
    "local chunk = ...\n"
    "chunk()\n"
    "if type(aroma.load) == 'function' then aroma.load() end\n";

static void close_lua_state(void) {
    /* Invalidate completions of any loads still in flight on the old state */
    g_state.generation++;

    if (!g_state.L) {
        return;
    }

    /* Closing the state collects every canvas, none may count as in use */
    g_state.gfx.canvas = NULL;
    lua_close(g_state.L);
    g_state.L = NULL;
    g_state.script_thread = NULL;
    g_state.script_thread_ref = LUA_NOREF;
    g_state.idle_thread = NULL;
    g_state.idle_thread_ref = LUA_NOREF;
    g_state.script_entry_point = SCRIPT_ENTRY_NONE;
    g_state.discard_rendering = 0;
    g_state.resource_wait = 0;
    default_graphics_state();
    g_state.key_repeat = 0;
    g_state.quit_requested = 0;
    set_mouse_visible(1);
    g_state.master_volume = 1.0f;
    js_audio_set_master_volume(1.0);
    /* The next run finds the pads again and gets a joystickadded for each */
    memset(g_state.joysticks, 0, sizeof(g_state.joysticks));
    g_state.user_mapping_count = 0;
}

static int quit_aborted(void) {
    lua_State *T = script_thread_begin(SCRIPT_ENTRY_QUIT);
    if (!T) return 0;
    if (!push_aroma_callback(T, "quit")) {
        script_thread_finish();
        return 0;
    }

    int aborted = 0;
    int status = lua_resume(T, NULL, 0);
    if (status == LUA_OK) {
        aborted = lua_gettop(T) > 0 && lua_toboolean(T, 1);
    } else if (status == LUA_YIELD) {
        fprintf(stderr, "Lua error: love.quit cannot wait on a resource load\n");
    } else {
        report_lua_error(T);
    }
    script_thread_finish();
    return aborted;
}

/* A quit from an entry point that is still suspended on a load waits for it
 * to finish. Returns whether the state was closed */
static int finish_quit(void) {
    if (!g_state.quit_requested || !g_state.L || g_state.script_thread) {
        return 0;
    }
    g_state.quit_requested = 0;

    if (quit_aborted()) {
        return 0;
    }

    close_lua_state();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    js_on_quit();
    return 1;
}

EMSCRIPTEN_KEEPALIVE int run_lua_code(const char *code) {
    close_lua_state();

    /* A run starts from the same window and colors whatever the last one did */
    set_canvas_size(DEFAULT_WIDTH, DEFAULT_HEIGHT);

    g_state.L = luaL_newstate();
    if (!g_state.L) {
        fprintf(stderr, "Failed to create Lua state\n");
        return 1;
    }

    luaL_openlibs(g_state.L);
    register_aroma_api(g_state.L);

    /* Every run starts somewhere new, scripts that want a fixed run seed it */
    random_set_seed(&g_random, (uint64_t)emscripten_get_now() ^ ((uint64_t)(emscripten_random() * 4294967296.0) << 32));

    lua_State *T = script_thread_begin(SCRIPT_ENTRY_BOOTSTRAP);
    if (luaL_loadstring(T, bootstrap_source) != LUA_OK ||
        luaL_loadstring(T, code) != LUA_OK) {
        report_lua_error(T);
        script_thread_finish();
        return 1;
    }

    /* May suspend on resource loads; runs to completion asynchronously */
    int status = script_thread_run(1);
    return status == LUA_OK || status == LUA_YIELD ? 0 : 1;
}

EMSCRIPTEN_KEEPALIVE void aroma_focus(int focused) {
    lua_State *T = script_thread_begin(SCRIPT_ENTRY_FOCUS);
    if (!T) return;
    if (!push_aroma_callback(T, "focus")) {
        script_thread_finish();
        return;
    }
    lua_pushboolean(T, focused);
    script_thread_run(1);
}

/* Calls a joystick callback with the slot's Joystick first. The rest of the
 * arguments are one string or one number, and for an axis also its value */
static void call_joystick_event(const char *name, JoystickSlot *slot, const char *string_arg, int number_arg, int has_value, float value) {
    lua_State *T = script_thread_begin(SCRIPT_ENTRY_JOYSTICK_EVENT);
    if (!T) return;
    if (!push_aroma_callback(T, name)) {
        script_thread_finish();
        return;
    }

    int nargs = 1;
    lua_rawgeti(T, LUA_REGISTRYINDEX, slot->object_ref);
    if (string_arg) {
        lua_pushstring(T, string_arg);
        nargs++;
    } else if (number_arg > 0) {
        lua_pushinteger(T, number_arg);
        nargs++;
    }
    if (has_value) {
        lua_pushnumber(T, value);
        nargs++;
    }
    script_thread_run(nargs);
    finish_quit();
}

/* Browsers put the USB ids in the pad's name: Chrome and Safari append
 * "(... Vendor: 3537 Product: 1022)", which is taken off the name again, and
 * Firefox leads with "3537-1022-" */
static void read_usb_ids(JoystickSlot *slot) {
    unsigned int vendor = 0, product = 0;

    char *tag = strstr(slot->name, "Vendor: ");
    if (tag && sscanf(tag, "Vendor: %4x Product: %4x", &vendor, &product) == 2) {
        char *paren = tag;
        while (paren > slot->name && *paren != '(') paren--;
        if (*paren == '(') {
            while (paren > slot->name && paren[-1] == ' ') paren--;
            *paren = '\0';
        }
    } else {
        char rest;
        if (sscanf(slot->name, "%4x-%4x-%c", &vendor, &product, &rest) == 3) {
            char *name = strchr(strchr(slot->name, '-') + 1, '-') + 1;
            memmove(slot->name, name, strlen(name) + 1);
        } else {
            vendor = product = 0;
        }
    }

    slot->vendor = (int)vendor;
    slot->product = (int)product;
}

static float g_joystick_staging[MAX_JOYSTICKS * JOYSTICK_STAGING_FLOATS];

/* Takes in the state of the pads for this frame and sends the callbacks for
 * what changed. The state is kept current even when a callback has to be
 * dropped behind a suspended entry point */
static void poll_joysticks(void) {
    lua_State *L = g_state.L;
    if (!L) {
        return;
    }

    js_poll_gamepads(g_joystick_staging, MAX_JOYSTICKS, JOYSTICK_STAGING_FLOATS, MAX_JOYSTICK_BUTTONS, MAX_JOYSTICK_AXES);

    for (int i = 0; i < MAX_JOYSTICKS && g_state.L == L; i++) {
        JoystickSlot *slot = &g_state.joysticks[i];
        const float *in = &g_joystick_staging[i * JOYSTICK_STAGING_FLOATS];
        const float *in_pressed = in + 4;
        const float *in_values = in_pressed + MAX_JOYSTICK_BUTTONS;
        const float *in_axes = in_values + MAX_JOYSTICK_BUTTONS;
        int connected = in[0] != 0.0f;

        if (slot->connected && !connected) {
            slot->connected = 0;
            call_joystick_event("joystickremoved", slot, NULL, 0, 0, 0.0f);
            if (g_state.L != L) return;
            luaL_unref(L, LUA_REGISTRYINDEX, slot->object_ref);
            slot->object_ref = LUA_NOREF;
            continue;
        }

        if (!connected) {
            continue;
        }

        if (!slot->connected) {
            memset(slot, 0, sizeof(JoystickSlot));
            slot->connected = 1;
            slot->instance_id = ++g_state.next_joystick_instance;
            slot->button_count = (int)in[2];
            slot->axis_count = (int)in[3];
            js_gamepad_name(i, slot->name, sizeof(slot->name));
            read_usb_ids(slot);
            assign_mapping(slot, in[1] != 0.0f);

            AromaJoystick *joystick = (AromaJoystick *)lua_newuserdata(L, sizeof(AromaJoystick));
            joystick->slot = i;
            joystick->instance_id = slot->instance_id;
            luaL_getmetatable(L, "aroma.joystick");
            lua_setmetatable(L, -2);
            slot->object_ref = luaL_ref(L, LUA_REGISTRYINDEX);

            call_joystick_event("joystickadded", slot, NULL, 0, 0, 0.0f);
            if (g_state.L != L) return;
        }

        slot->button_count = (int)in[2];
        slot->axis_count = (int)in[3];

        /* Take in the whole raw state first so that the callbacks below all
         * see the same frame */
        uint32_t was_pressed = slot->pressed;
        float old_axes[MAX_JOYSTICK_AXES];
        memcpy(old_axes, slot->axes, sizeof(old_axes));

        slot->pressed = 0;
        for (int b = 0; b < slot->button_count; b++) {
            slot->values[b] = in_values[b];
            if (in_pressed[b] != 0.0f) {
                slot->pressed |= 1u << b;
            }
        }
        for (int a = 0; a < slot->axis_count; a++) {
            slot->axes[a] = in_axes[a];
            if (in_axes[a] != 0.0f) {
                slot->axes_seen |= 1u << a;
            }
        }

        int raw_axes = joystick_axis_count(slot);
        for (int a = 0; a < raw_axes; a++) {
            if (old_axes[a] == slot->axes[a]) continue;
            call_joystick_event("joystickaxis", slot, NULL, a + 1, 1, slot->axes[a]);
            if (g_state.L != L) return;
        }

        for (int b = 0; b < slot->button_count; b++) {
            int pressed = (slot->pressed >> b) & 1;
            if (pressed == (int)((was_pressed >> b) & 1)) continue;
            call_joystick_event(pressed ? "joystickpressed" : "joystickreleased", slot, NULL, b + 1, 0, 0.0f);
            if (g_state.L != L) return;
        }

        if (!slot->mapping) {
            continue;
        }

        for (int a = 0; a < GAMEPAD_AXES; a++) {
            float value = gamepad_axis_value(slot, a);
            if (value == slot->gamepad_axes[a]) continue;
            slot->gamepad_axes[a] = value;
            call_joystick_event("gamepadaxis", slot, gamepad_axis_names[a], 0, 1, value);
            if (g_state.L != L) return;
        }

        for (int b = 0; b < GAMEPAD_BUTTONS; b++) {
            int pressed = gamepad_button_down(slot, b);
            if (pressed == (int)((slot->gamepad_pressed >> b) & 1)) continue;
            slot->gamepad_pressed ^= 1u << b;
            call_joystick_event(pressed ? "gamepadpressed" : "gamepadreleased", slot, gamepad_button_names[b], 0, 0, 0.0f);
            if (g_state.L != L) return;
        }
    }
}

/* x and y are in canvas pixels. The state is kept current even when the
 * callback has to be dropped behind a suspended entry point */
EMSCRIPTEN_KEEPALIVE void aroma_mousemoved(int x, int y) {
    int dx = x - g_state.mouse_x;
    int dy = y - g_state.mouse_y;
    g_state.mouse_x = x;
    g_state.mouse_y = y;

    if (dx == 0 && dy == 0) {
        return;
    }

    lua_State *T = script_thread_begin(SCRIPT_ENTRY_MOUSE_EVENT);
    if (!T) return;
    if (!push_aroma_callback(T, "mousemoved")) {
        script_thread_finish();
        return;
    }
    lua_pushinteger(T, x);
    lua_pushinteger(T, y);
    lua_pushinteger(T, dx);
    lua_pushinteger(T, dy);
    lua_pushboolean(T, 0); /* istouch */
    script_thread_run(5);
    finish_quit();
}

/* button is love's numbering: 1 left, 2 right, 3 middle. presses counts the
 * clicks of a double or triple click */
EMSCRIPTEN_KEEPALIVE void aroma_mousebutton(int pressed, int x, int y, int button, int presses) {
    g_state.mouse_x = x;
    g_state.mouse_y = y;

    if (button < 1 || button > 32) {
        return;
    }

    if (pressed) {
        g_state.mouse_buttons |= 1u << (button - 1);
    } else {
        g_state.mouse_buttons &= ~(1u << (button - 1));
    }

    lua_State *T = script_thread_begin(SCRIPT_ENTRY_MOUSE_EVENT);
    if (!T) return;
    if (!push_aroma_callback(T, pressed ? "mousepressed" : "mousereleased")) {
        script_thread_finish();
        return;
    }
    lua_pushinteger(T, x);
    lua_pushinteger(T, y);
    lua_pushinteger(T, button);
    lua_pushboolean(T, 0); /* istouch */
    lua_pushinteger(T, presses);
    script_thread_run(5);
    finish_quit();
}

EMSCRIPTEN_KEEPALIVE void aroma_keypressed(const char *key, int is_repeat) {
    if (is_repeat && !g_state.key_repeat) {
        return;
    }
    call_aroma_key_event("keypressed", key, is_repeat);
}

EMSCRIPTEN_KEEPALIVE void aroma_keyreleased(const char *key) {
    call_aroma_key_event("keyreleased", key, -1);
}

int main(void) {
    memset(&g_state, 0, sizeof(g_state));
    default_graphics_state();
    g_state.master_volume = 1.0f;
    g_state.script_thread_ref = LUA_NOREF;
    g_state.idle_thread_ref = LUA_NOREF;

    if (!init_webgl()) {
        return 1;
    }

    if (!init_program()) {
        return 1;
    }

    glGenBuffers(1, &g_state.vbo);

    g_state.last_time = emscripten_get_now();
    g_state.start_time = g_state.last_time;
    g_state.fps_window_start = g_state.last_time;
    emscripten_set_main_loop_arg(main_loop, NULL, 0, 1);
    return 0;
}
