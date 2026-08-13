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
    SCRIPT_ENTRY_KEY_EVENT
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
    int canvas_width;
    int canvas_height;
    AromaFont *current_font;
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
    int count = lua_gettop(L) - idx + 1;
    double r = luaL_checknumber(L, idx + 0);
    double g = luaL_checknumber(L, idx + 1);
    double b = luaL_checknumber(L, idx + 2);
    double a = (count >= 4) ? luaL_checknumber(L, idx + 3) : 1.0;

    if (r > 1.0 || g > 1.0 || b > 1.0 || a > 1.0) {
        r /= 255.0;
        g /= 255.0;
        b /= 255.0;
        a /= 255.0;
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
    if (lua_isnoneornil(L, 1)) {
        g_state.current_font = NULL;
        return 0;
    }

    AromaFont *font = check_font(L, 1);
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

    lua_newtable(L);                /* aroma */
    lua_newtable(L);                /* aroma.graphics */

    lua_pushcfunction(L, l_graphics_setBackgroundColor);
    lua_setfield(L, -2, "setBackgroundColor");

    lua_pushcfunction(L, l_graphics_setColor);
    lua_setfield(L, -2, "setColor");

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
    lua_setfield(L, -2, "keyboard"); /* aroma.keyboard = table */

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

static void call_aroma_key_event(const char *name, const char *key) {
    lua_State *T = script_thread_begin(SCRIPT_ENTRY_KEY_EVENT);
    if (!T) return;
    if (!push_aroma_callback(T, name)) {
        script_thread_finish();
        return;
    }
    lua_pushstring(T, key);
    script_thread_run(1);
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

    g_state.canvas_width = 800;
    g_state.canvas_height = 600;
    emscripten_set_canvas_element_size("#canvas", g_state.canvas_width, g_state.canvas_height);
    glViewport(0, 0, g_state.canvas_width, g_state.canvas_height);
    setup_projection((float)g_state.canvas_width, (float)g_state.canvas_height);

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
    g_state.last_time = now;

    /* Leave a suspended entry point and its graphics stack untouched.
     * Resource completions drain independently of requestAnimationFrame. */
    if (g_state.script_thread) {
        return;
    }

    call_aroma_update(dt);

    if (g_state.script_thread) {
        return;
    }

    glClearColor(g_state.bg_color[0], g_state.bg_color[1], g_state.bg_color[2], g_state.bg_color[3]);
    glClear(GL_COLOR_BUFFER_BIT);

    reset_graphics_state();
    call_aroma_draw();
}

static const char *bootstrap_source =
    "local chunk = ...\n"
    "chunk()\n"
    "if type(aroma.load) == 'function' then aroma.load() end\n";

EMSCRIPTEN_KEEPALIVE int run_lua_code(const char *code) {
    /* Invalidate completions of any loads still in flight on the old state */
    g_state.generation++;

    if (g_state.L) {
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
    }

    g_state.L = luaL_newstate();
    if (!g_state.L) {
        fprintf(stderr, "Failed to create Lua state\n");
        return 1;
    }

    luaL_openlibs(g_state.L);
    register_aroma_api(g_state.L);

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

EMSCRIPTEN_KEEPALIVE void aroma_keypressed(const char *key) {
    call_aroma_key_event("keypressed", key);
}

EMSCRIPTEN_KEEPALIVE void aroma_keyreleased(const char *key) {
    call_aroma_key_event("keyreleased", key);
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

    if (!init_webgl()) {
        return 1;
    }

    if (!init_program()) {
        return 1;
    }

    glGenBuffers(1, &g_state.vbo);

    g_state.last_time = emscripten_get_now();
    emscripten_set_main_loop_arg(main_loop, NULL, 0, 1);
    return 0;
}
