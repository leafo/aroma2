# TODO

## Runtime & Window Lifecycle
- Invoke `love.conf` (`ludum-dare-30/conf.lua`) before initializing graphics to honor the game’s 840×544 window, title, and scale settings.
- Mirror Love’s event loop semantics so `Dispatcher:bind(love)` receives callbacks for `love.update`, `love.draw`, and input handlers every frame.
- Route browser input events into the matching Love callbacks (`keypressed`, `mousepressed`, `mousereleased`, `mousemoved`, `joystickpressed`) while forwarding the correct arguments.
- Provide a way to quit by mapping `love.event.push("quit")` back to the hosting environment (used for ESC handling).
- Keep `love.timer.getTime()` and `love.timer.step()` in sync with the engine clock so `Sequence`, `Controller`, and timing code behave correctly.

## Graphics
- Implement the following `love.graphics` entry points with proper matrix stack integration: `push`, `pop`, `translate`, `scale`, `rotate`, `setColor`, `setBackgroundColor`, `setBlendMode`, `setScissor`, `setCanvas`, `getCanvas`, `clear`, `draw`, `print`, `printf`, `point`, `line`, `rectangle`, and return values for `getWidth`, `getHeight`, `getFont`, `setFont`, `getPointSize`, `setPointSize`.
- Support resource constructors: `newImage`, `newImageFont`, `newQuad`, `newSpriteBatch`, `newCanvas`, `newShader`. Ensure defaults such as `Image:setFilter('nearest','nearest')` and Canvas creation with optional width/height work.
- Expose object methods used by Lovekit:
  - `Image`/`Texture`: `getWidth`, `getHeight`, `setFilter`, `setWrap`, `draw` (including center helper).
  - `Canvas`: `:clear(r,g,b,a)`, `:setFilter`, `:setWrap`, `:getWidth`, `:getHeight`.
  - `Shader`: `:send` (numbers, vec2/vec3 arrays) and allow toggling via `love.graphics.setShader(nil)`.
  - `SpriteBatch`: constructor with capacity limit plus `:add(quad, x, y, r, sx, sy)` and `:clear` before redraw.
  - `Quad`: creation with texture dimensions for tile maps.
- Implement `love.image.newImageData` along with `ImageData:getWidth`, `getHeight`, and `getPixel` (used to build tile maps from color masks).
- Ensure `love.graphics.setBlendMode('premultiplied')` and restoration to `'alpha'` behave like desktop Love.
- Allow nested canvases and scissor rectangles so `Viewport`/`EffectViewport` rendering paths can draw to offscreen targets, flip, and composite back.
- Provide `love.graphics.getCanvas()` / `setCanvas()` parity when nil is passed to restore the default framebuffer.

## Shaders & Effects
- Compile GLSL ES shaders via `love.graphics.newShader` and support the uniform APIs relied on by `FullScreenShader`, `RedGlow`, and `ColorShader` (`:send` for numbers/vectors, rendering with fullscreen quad).
- Ensure canvases created for shader effects accept wrap/filter changes and can be drawn with premultiplied alpha blending.

## Fonts & Text
- Implement image-based fonts (`love.graphics.newImageFont`) so HUD text renders; include glyph remapping and `Font:setFilter` if needed.
- Make sure `love.graphics.print`/`printf` respect the active font and current color stack from `lovekit.color`.

## Audio
- Provide `love.audio.newSource(path, type)` for both `"static"` and `"stream"` sources used by `Audio:play`/`play_music`.
- Implement Source object controls: `:play`, `:stop`, `:setLooping`, `:setVolume`, `:getVolume`, and `:getVolumeLimits` (required for fades).

## Input & Controllers
- Mirror keyboard helpers: `love.keyboard.isDown`, `love.keyboard.setKeyRepeat`, `love.keyboard.hasKeyRepeat`.
- Expose mouse helpers: `love.mouse.getPosition`, `love.mouse.isDown`, `love.mouse.setVisible`.
- Support joysticks: `love.joystick.getJoysticks`, `love.joystick.setGamepadMapping`, and per-joystick methods `:getHat`, `:getGamepadAxis` for analog/dpad controls.
- Surface button/axis events so `Controller` can detect confirm/cancel inputs and multitap behavior.

## Timing & Math
- Back `love.timer.getTime()` / `love.timer.step()` with high-resolution timers to match Sequencer expectations.
- Provide `love.math.random` (and optional seeding) with parity to Love’s RNG for particle/effect timing.

## Asset Loading & Files
- Package `images/`, `sounds/`, `maps/`, and shader sources into the virtual filesystem and ensure `require`/`love.graphics.newImage`/`love.audio.newSource` can read them by path.
- Handle TMX-derived Lua map files and raw image masks accessed via `love.image.newImageData`.

## Miscellaneous Utilities
- Maintain a color stack compatible with `lovekit.color` by making `love.graphics.setColor` immediately affect subsequent draw calls.
- Ensure global helpers like `love.graphics.getWidth()` report the scaled viewport size used by `Viewport` calculations.
- Confirm that calling `love.graphics.setShader()`/`setCanvas()`/`setBlendMode()` with no arguments restores defaults exactly once (Lovekit relies on that behavior in its push/pop stacks).
