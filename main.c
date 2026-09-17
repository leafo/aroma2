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

typedef struct {
    int texture_id;
    int width;
    int height;
    int loaded;
} AromaImage;

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
    float line_height;
    int loaded;
} AromaFont;

typedef enum {
    SCRIPT_ENTRY_NONE,
    SCRIPT_ENTRY_BOOTSTRAP,
    SCRIPT_ENTRY_UPDATE,
    SCRIPT_ENTRY_DRAW,
    SCRIPT_ENTRY_KEY_EVENT,
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
    float bg_color[4];
    float draw_color[4];
    Mat3 matrix_stack[STACK_MAX];
    int stack_top;
    double last_time;
    double start_time;
    float dt;
    /* Frames per second, refreshed once a second like love's timer */
    int fps;
    int fps_frames;
    double fps_window_start;
    int canvas_width;
    int canvas_height;
    AromaFont *current_font;
    int current_font_ref;
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
    /* Off by default like love: held keys send one keypressed */
    int key_repeat;
    /* Set from inside Lua, the state is torn down once the frame unwinds */
    int quit_requested;
} EngineState;

static EngineState g_state;

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
    return &g_state.matrix_stack[g_state.stack_top];
}

static AromaImage *check_image(lua_State *L, int idx) {
    return (AromaImage *)luaL_checkudata(L, idx, "aroma.image");
}

static AromaFont *check_font(lua_State *L, int idx) {
    return (AromaFont *)luaL_checkudata(L, idx, "aroma.font");
}

static int l_graphics_setBackgroundColor(lua_State *L) {
    parse_color(L, 1, g_state.bg_color);
    return 0;
}

static int l_graphics_getColor(lua_State *L) {
    for (int i = 0; i < 4; i++) {
        lua_pushnumber(L, g_state.draw_color[i]);
    }
    return 4;
}

static int l_graphics_setColor(lua_State *L) {
    parse_color(L, 1, g_state.draw_color);
    return 0;
}

static int l_graphics_push(lua_State *L) {
    if (g_state.stack_top + 1 >= STACK_MAX) {
        return luaL_error(L, "love.graphics.push: stack overflow");
    }
    mat3_copy(&g_state.matrix_stack[g_state.stack_top + 1], current_matrix());
    g_state.stack_top++;
    return 0;
}

static int l_graphics_pop(lua_State *L) {
    if (g_state.stack_top == 0) {
        return luaL_error(L, "love.graphics.pop: stack underflow");
    }
    g_state.stack_top--;
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

static int l_graphics_polygon(lua_State *L) {
    const char *mode = luaL_checkstring(L, 1);
    if (strcmp(mode, "fill") != 0) {
        return luaL_error(L, "love.graphics.polygon: only 'fill' supported");
    }

    int args = lua_gettop(L) - 1;
    if (args < 6 || (args % 2) != 0) {
        return luaL_error(L, "love.graphics.polygon: need pairs of coordinates");
    }

    int points = args / 2;
    float *coords = (float *)malloc(sizeof(float) * points * 2);
    if (!coords) {
        return luaL_error(L, "love.graphics.polygon: out of memory");
    }

    for (int i = 0; i < points; ++i) {
        coords[i * 2 + 0] = (float)luaL_checknumber(L, 2 + i * 2);
        coords[i * 2 + 1] = (float)luaL_checknumber(L, 3 + i * 2);
    }

    if (g_state.discard_rendering) {
        free(coords);
        return 0;
    }

    glUseProgram(g_state.program);
    glUniformMatrix3fv(g_state.transform_loc, 1, GL_FALSE, current_matrix()->m);
    glUniformMatrix3fv(g_state.projection_loc, 1, GL_FALSE, g_state.projection);
    glUniform4fv(g_state.color_loc, 1, g_state.draw_color);
    if (g_state.use_texture_loc >= 0) {
        glUniform1i(g_state.use_texture_loc, 0);
    }
    glDisableVertexAttribArray(1);
    glVertexAttrib2f(1, 0.0f, 0.0f);

    glBindBuffer(GL_ARRAY_BUFFER, g_state.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * points * 2, coords, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (const void *)0);

    glDrawArrays(GL_TRIANGLE_FAN, 0, points);

    free(coords);
    return 0;
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

static int l_graphics_newImage(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    check_load_context(L, "love.graphics.newImage");

    AromaImage *img = (AromaImage *)lua_newuserdata(L, sizeof(AromaImage));
    memset(img, 0, sizeof(AromaImage));

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

static int count_printable_glyphs(AromaFont *font, const char *text, int has_fallback) {
    int count = 0;
    const char *ptr = text;

    while (*ptr) {
        uint32_t codepoint = utf8_decode(&ptr);

        if (codepoint == '\n' || codepoint == '\r' || codepoint == '\t') {
            continue;
        }

        if (find_glyph(font, codepoint)) {
            count++;
            continue;
        }

        if (has_fallback) {
            count++;
        }
    }

    return count;
}

static int l_graphics_draw(lua_State *L) {
    AromaImage *img = check_image(L, 1);
    float x = (float)luaL_optnumber(L, 2, 0.0);
    float y = (float)luaL_optnumber(L, 3, 0.0);
    float r = (float)luaL_optnumber(L, 4, 0.0);
    float sx = (float)luaL_optnumber(L, 5, 1.0);
    float sy = (float)luaL_optnumber(L, 6, sx);
    float ox = (float)luaL_optnumber(L, 7, 0.0);
    float oy = (float)luaL_optnumber(L, 8, 0.0);

    if (!img->loaded || img->texture_id == 0) {
        return 0;
    }

    if (g_state.discard_rendering) {
        return 0;
    }



    Mat3 base;
    mat3_copy(&base, current_matrix());

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

    Mat3 final;
    mat3_multiply(&final, &base, &local);

    float w = (float)img->width;
    float h = (float)img->height;
    float vertices[] = {
        0.0f, 0.0f, 0.0f, 0.0f,
        w,    0.0f, 1.0f, 0.0f,
        w,    h,    1.0f, 1.0f,
        0.0f, h,    0.0f, 1.0f
    };

    glUseProgram(g_state.program);
    glUniformMatrix3fv(g_state.transform_loc, 1, GL_FALSE, final.m);
    glUniformMatrix3fv(g_state.projection_loc, 1, GL_FALSE, g_state.projection);
    glUniform4fv(g_state.color_loc, 1, g_state.draw_color);
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

static int l_graphics_getWidth(lua_State *L) {
    lua_pushinteger(L, g_state.canvas_width);
    return 1;
}

static int l_graphics_getHeight(lua_State *L) {
    lua_pushinteger(L, g_state.canvas_height);
    return 1;
}

static int l_graphics_getDimensions(lua_State *L) {
    lua_pushinteger(L, g_state.canvas_width);
    lua_pushinteger(L, g_state.canvas_height);
    return 2;
}

static int l_graphics_rectangle(lua_State *L) {
    const char *mode = luaL_checkstring(L, 1);

    float x = (float)luaL_checknumber(L, 2);
    float y = (float)luaL_checknumber(L, 3);
    float w = (float)luaL_checknumber(L, 4);
    float h = (float)luaL_checknumber(L, 5);

    if (strcmp(mode, "fill") != 0 && strcmp(mode, "line") != 0) {
        return luaL_error(L, "love.graphics.rectangle: mode must be 'fill' or 'line'");
    }

    if (g_state.discard_rendering) {
        return 0;
    }

    // Create 5 vertices for the rectangle (last point closes the loop)
    float coords[10] = {
        x, y,
        x, y + h,
        x + w, y + h,
        x + w, y,
        x, y
    };

    glUseProgram(g_state.program);
    glUniformMatrix3fv(g_state.transform_loc, 1, GL_FALSE, current_matrix()->m);
    glUniformMatrix3fv(g_state.projection_loc, 1, GL_FALSE, g_state.projection);
    glUniform4fv(g_state.color_loc, 1, g_state.draw_color);
    if (g_state.use_texture_loc >= 0) {
        glUniform1i(g_state.use_texture_loc, 0);
    }
    glDisableVertexAttribArray(1);
    glVertexAttrib2f(1, 0.0f, 0.0f);

    glBindBuffer(GL_ARRAY_BUFFER, g_state.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(coords), coords, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (const void *)0);

    if (strcmp(mode, "fill") == 0) {
        glDrawArrays(GL_TRIANGLE_FAN, 0, 5);
    } else if (strcmp(mode, "line") == 0) {
        glDrawArrays(GL_LINE_STRIP, 0, 5);
    }

    return 0;
}

static int l_image_getWidth(lua_State *L) {
    AromaImage *img = check_image(L, 1);
    lua_pushinteger(L, img->width);
    return 1;
}

static int l_image_getHeight(lua_State *L) {
    AromaImage *img = check_image(L, 1);
    lua_pushinteger(L, img->height);
    return 1;
}

static int l_image_gc(lua_State *L) {
    AromaImage *img = check_image(L, 1);
    if (img->texture_id) {
        js_release_texture(img->texture_id);
        img->texture_id = 0;
    }
    img->loaded = 0;
    return 0;
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
    if (g_state.current_font_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, g_state.current_font_ref);
        g_state.current_font_ref = LUA_NOREF;
    }

    if (lua_isnoneornil(L, 1)) {
        g_state.current_font = NULL;
        return 0;
    }

    AromaFont *font = check_font(L, 1);
    lua_pushvalue(L, 1);
    g_state.current_font_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    g_state.current_font = font;
    return 0;
}

static int l_graphics_print(lua_State *L) {
    const char *text = luaL_checkstring(L, 1);
    float x = (float)luaL_optnumber(L, 2, 0.0);
    float y = (float)luaL_optnumber(L, 3, 0.0);

    AromaFont *font = g_state.current_font;
    if (!font || !font->loaded || font->texture_id == 0 || font->glyph_count == 0) {
        return 0;
    }

    if (g_state.discard_rendering) {
        return 0;
    }

    GlyphInfo *fallback = find_glyph(font, (uint32_t)'?');
    int drawable_count = count_printable_glyphs(font, text, fallback != NULL);
    if (drawable_count <= 0) {
        return 0;
    }

    float *vertices = (float *)malloc(sizeof(float) * drawable_count * 4 * 4);
    if (!vertices) {
        return luaL_error(L, "love.graphics.print: out of memory");
    }

    float inv_width = font->image_width > 0 ? 1.0f / font->image_width : 0.0f;
    float glyph_height_px = font->image_height > 0 ? (float)font->image_height : 1.0f;
    float line_height = font->line_height > 0.0f ? font->line_height : glyph_height_px;

    float cursor_x = x;
    float cursor_y = y;
    const float base_x = x;

    const char *ptr = text;
    int vertex_count = 0;

    while (*ptr) {
        uint32_t codepoint = utf8_decode(&ptr);

        if (codepoint == '\n') {
            cursor_x = base_x;
            cursor_y += line_height;
            continue;
        }

        if (codepoint == '\r') {
            cursor_x = base_x;
            continue;
        }

        if (codepoint == '\t') {
            GlyphInfo *space = find_glyph(font, (uint32_t)' ');
            float tab_advance = space ? (float)space->width + font->extra_spacing : line_height * 0.5f;
            cursor_x += tab_advance * 4.0f;
            continue;
        }

        GlyphInfo *glyph = find_glyph(font, codepoint);
        if (!glyph) {
            glyph = fallback;
            if (!glyph) {
                continue;
            }
        }

        float advance = (float)glyph->width + font->extra_spacing;

        if (glyph->width > 0 && inv_width > 0.0f) {
            float x1 = cursor_x;
            float x2 = cursor_x + (float)glyph->width;
            float y1 = cursor_y;
            float y2 = cursor_y + glyph_height_px;

            float u1 = glyph->x * inv_width;
            float u2 = (glyph->x + glyph->width) * inv_width;
            float v1 = 0.0f;
            float v2 = 1.0f;

            vertices[vertex_count++] = x1;
            vertices[vertex_count++] = y1;
            vertices[vertex_count++] = u1;
            vertices[vertex_count++] = v1;

            vertices[vertex_count++] = x1;
            vertices[vertex_count++] = y2;
            vertices[vertex_count++] = u1;
            vertices[vertex_count++] = v2;

            vertices[vertex_count++] = x2;
            vertices[vertex_count++] = y2;
            vertices[vertex_count++] = u2;
            vertices[vertex_count++] = v2;

            vertices[vertex_count++] = x2;
            vertices[vertex_count++] = y1;
            vertices[vertex_count++] = u2;
            vertices[vertex_count++] = v1;
        }

        cursor_x += advance;
    }

    if (vertex_count > 0) {
        glUseProgram(g_state.program);
        glUniformMatrix3fv(g_state.transform_loc, 1, GL_FALSE, current_matrix()->m);
        glUniformMatrix3fv(g_state.projection_loc, 1, GL_FALSE, g_state.projection);
        glUniform4fv(g_state.color_loc, 1, g_state.draw_color);
        if (g_state.use_texture_loc >= 0) {
            glUniform1i(g_state.use_texture_loc, 1);
        }

        glActiveTexture(GL_TEXTURE0);
        js_bind_texture(font->texture_id);
        if (g_state.sampler_loc >= 0) {
            glUniform1i(g_state.sampler_loc, 0);
        }

        glBindBuffer(GL_ARRAY_BUFFER, g_state.vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(float) * vertex_count, vertices, GL_DYNAMIC_DRAW);

        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (const void *)0);

        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (const void *)(2 * sizeof(float)));

        int num_quads = vertex_count / 16;
        for (int i = 0; i < num_quads; i++) {
            glDrawArrays(GL_TRIANGLE_FAN, i * 4, 4);
        }

        glDisableVertexAttribArray(1);
    }

    free(vertices);
    return 0;
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
    if (g_state.current_font == font) {
        g_state.current_font = NULL;
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

static void setup_projection(float width, float height);

static void set_canvas_size(int width, int height) {
    g_state.canvas_width = width;
    g_state.canvas_height = height;
    emscripten_set_canvas_element_size("#canvas", width, height);
    glViewport(0, 0, width, height);
    setup_projection((float)width, (float)height);
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
    lua_pushinteger(L, g_state.canvas_width);
    lua_pushinteger(L, g_state.canvas_height);
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
        lua_pushcfunction(L, l_image_gc);
        lua_setfield(L, -2, "__gc");

        lua_newtable(L);
        lua_pushcfunction(L, l_image_getWidth);
        lua_setfield(L, -2, "getWidth");
        lua_pushcfunction(L, l_image_getHeight);
        lua_setfield(L, -2, "getHeight");
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);

    if (luaL_newmetatable(L, "aroma.font")) {
        lua_pushcfunction(L, l_font_gc);
        lua_setfield(L, -2, "__gc");

        lua_newtable(L);
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

    lua_pushcfunction(L, l_graphics_newImage);
    lua_setfield(L, -2, "newImage");

    lua_pushcfunction(L, l_graphics_draw);
    lua_setfield(L, -2, "draw");

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

    lua_pushcfunction(L, l_graphics_print);
    lua_setfield(L, -2, "print");

    lua_setfield(L, -2, "graphics"); /* aroma.graphics = table */

    lua_newtable(L);                /* aroma.keyboard */
    lua_pushcfunction(L, l_keyboard_isDown);
    lua_setfield(L, -2, "isDown");
    lua_pushcfunction(L, l_keyboard_setKeyRepeat);
    lua_setfield(L, -2, "setKeyRepeat");
    lua_pushcfunction(L, l_keyboard_hasKeyRepeat);
    lua_setfield(L, -2, "hasKeyRepeat");
    lua_setfield(L, -2, "keyboard"); /* aroma.keyboard = table */

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

    font->line_height = (float)height;
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

static void setup_projection(float width, float height) {
    float *p = g_state.projection;
    memset(p, 0, sizeof(float) * 9);
    p[0] = 2.0f / width;
    p[4] = -2.0f / height;
    p[6] = -1.0f;
    p[7] = 1.0f;
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

    // Enable alpha blending for transparent images
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    return 1;
}

static void reset_graphics_state(void) {
    g_state.stack_top = 0;
    mat3_identity(&g_state.matrix_stack[0]);
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

    glClearColor(g_state.bg_color[0], g_state.bg_color[1], g_state.bg_color[2], g_state.bg_color[3]);
    glClear(GL_COLOR_BUFFER_BIT);

    reset_graphics_state();
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

    lua_close(g_state.L);
    g_state.L = NULL;
    g_state.script_thread = NULL;
    g_state.script_thread_ref = LUA_NOREF;
    g_state.idle_thread = NULL;
    g_state.idle_thread_ref = LUA_NOREF;
    g_state.script_entry_point = SCRIPT_ENTRY_NONE;
    g_state.discard_rendering = 0;
    g_state.resource_wait = 0;
    g_state.current_font = NULL;
    g_state.current_font_ref = LUA_NOREF;
    g_state.key_repeat = 0;
    g_state.quit_requested = 0;
}

/* Runs love.quit and returns whether it asked to keep going */
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

/* Acts on a quit asked for during the entry point that just ran. A quit from
 * an entry point that is still suspended on a load waits for it to finish.
 * Returns whether the state was closed */
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
    g_state.bg_color[0] = g_state.bg_color[1] = g_state.bg_color[2] = 0.0f;
    g_state.bg_color[3] = 1.0f;
    g_state.draw_color[0] = g_state.draw_color[1] = g_state.draw_color[2] = g_state.draw_color[3] = 1.0f;

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
    g_state.bg_color[3] = 1.0f;
    g_state.draw_color[0] = 1.0f;
    g_state.draw_color[1] = 1.0f;
    g_state.draw_color[2] = 1.0f;
    g_state.draw_color[3] = 1.0f;
    g_state.script_thread_ref = LUA_NOREF;
    g_state.idle_thread_ref = LUA_NOREF;
    g_state.current_font_ref = LUA_NOREF;

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
