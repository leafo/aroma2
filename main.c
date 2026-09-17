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
    /* Off by default like love: held keys send one keypressed */
    int key_repeat;
    /* Set from inside Lua, the state is torn down once the frame unwinds */
    int quit_requested;
} EngineState;

static EngineState g_state;

static void apply_render_target(void);
static void apply_blend_mode(void);

static const char *vertex_shader_source =
    "attribute vec2 aPosition;\n"
    "attribute vec2 aTexCoord;\n"
    "uniform mat3 uTransform;\n"
    "uniform mat3 uProjection;\n"
    "varying vec2 vTexCoord;\n"
    "void main() {\n"
    "  vec3 world = uTransform * vec3(aPosition, 1.0);\n"
    "  vec3 clip = uProjection * world;\n"
    "  gl_Position = vec4(clip.xy, 0.0, 1.0);\n"
    "  vTexCoord = aTexCoord;\n"
    "}\n";

static const char *fragment_shader_source =
    "precision mediump float;\n"
    "uniform vec4 uColor;\n"
    "uniform sampler2D uTexture;\n"
    "uniform int uUseTexture;\n"
    "varying vec2 vTexCoord;\n"
    "void main() {\n"
    "  vec4 tex = uUseTexture == 1 ? texture2D(uTexture, vTexCoord) : vec4(1.0);\n"
    "  gl_FragColor = tex * uColor;\n"
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

    glUseProgram(g_state.program);
    glUniformMatrix3fv(g_state.transform_loc, 1, GL_FALSE, current_matrix()->m);
    glUniformMatrix3fv(g_state.projection_loc, 1, GL_FALSE, g_state.projection);
    glUniform4fv(g_state.color_loc, 1, g_state.gfx.draw_color);
    if (g_state.use_texture_loc >= 0) {
        glUniform1i(g_state.use_texture_loc, 0);
    }
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

    glUseProgram(g_state.program);
    glUniformMatrix3fv(g_state.transform_loc, 1, GL_FALSE, transform->m);
    glUniformMatrix3fv(g_state.projection_loc, 1, GL_FALSE, g_state.projection);
    glUniform4fv(g_state.color_loc, 1, g_state.gfx.draw_color);
    if (g_state.use_texture_loc >= 0) {
        glUniform1i(g_state.use_texture_loc, 1);
    }

    glActiveTexture(GL_TEXTURE0);
    js_bind_texture(font->texture_id);
    if (g_state.sampler_loc >= 0) {
        glUniform1i(g_state.sampler_loc, 0);
    }

    glBindBuffer(GL_ARRAY_BUFFER, g_state.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * count * 4, vertices, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (const void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (const void *)(2 * sizeof(float)));

    glDrawArrays(GL_TRIANGLES, 0, count);

    glDisableVertexAttribArray(1);
}

/* draw(image, [quad], x, y, r, sx, sy, ox, oy) */
static int l_graphics_draw(lua_State *L) {
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

    glUseProgram(g_state.program);
    glUniformMatrix3fv(g_state.transform_loc, 1, GL_FALSE, final.m);
    glUniformMatrix3fv(g_state.projection_loc, 1, GL_FALSE, g_state.projection);
    glUniform4fv(g_state.color_loc, 1, g_state.gfx.draw_color);
    if (g_state.use_texture_loc >= 0) {
        glUniform1i(g_state.use_texture_loc, 1);
    }

    glActiveTexture(GL_TEXTURE0);
    js_bind_texture(img->texture_id);
    if (g_state.sampler_loc >= 0) {
        glUniform1i(g_state.sampler_loc, 0);
    }

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

    lua_pushcfunction(L, l_graphics_newCanvas);
    lua_setfield(L, -2, "newCanvas");

    lua_pushcfunction(L, l_graphics_setCanvas);
    lua_setfield(L, -2, "setCanvas");

    lua_pushcfunction(L, l_graphics_getCanvas);
    lua_setfield(L, -2, "getCanvas");

    lua_pushcfunction(L, l_graphics_clear);
    lua_setfield(L, -2, "clear");

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
