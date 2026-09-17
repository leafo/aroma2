-- A tour of the timer, math, window, mouse, keyboard and shape APIs.
--
--   mouse      move to aim the arc, the crosshair follows in canvas pixels
--   click      left drops a dot, right clears them, a double click drops a big one
--   drag       hold a button and leave the canvas, the release still arrives
--   up / down  line width (hold to repeat once r has turned key repeat on)
--   r          toggle key repeat
--   h          hide / show the cursor
--   w          switch the window between 800x600 and 640x480
--   s          reseed the terrain
--   space      hold to spin faster
--   escape     quit, the first press is refused by love.quit

local g = love.graphics

local font
local seed = 1
local terrain = {}
local dots = {}
local spin = 0
local line_width = 2
local small = false
local quit_armed = false
local last_event = "none yet"

-- the same seed gives the same ridge here and on desktop love
local function build_terrain()
  local rng = love.math.newRandomGenerator(seed)
  local offset = rng:random() * 1000
  terrain = {}
  for i = 0, 40 do
    terrain[#terrain + 1] = i * 20
    terrain[#terrain + 1] = 420 + love.math.noise(offset + i * 0.15) * 120
  end
end

function love.load()
  g.setBackgroundColor(0.1, 0.1, 0.15)
  font = g.newImageFont("font1.png", [[ ABCDEFGHIJKLMNOPQRSTUVWXYZ[\]^_`abcdefghijklmnopqrstuvwxyz{|}~!"#$%&'()*+,-./0123456789:;<=>?]])
  g.setFont(font)
  love.window.setTitle("aroma playground")
  build_terrain()
end

function love.update(dt)
  spin = spin + dt * (love.keyboard.isDown("space") and 6 or 1)
end

function love.keypressed(key, scancode, isrepeat)
  last_event = "keypressed " .. key .. (isrepeat and " (repeat)" or "")

  if key == "up" then
    line_width = math.min(20, line_width + 1)
  elseif key == "down" then
    line_width = math.max(1, line_width - 1)
  elseif key == "r" and not isrepeat then
    love.keyboard.setKeyRepeat(not love.keyboard.hasKeyRepeat())
  elseif key == "h" and not isrepeat then
    love.mouse.setVisible(not love.mouse.isVisible())
  elseif key == "w" and not isrepeat then
    small = not small
    if small then
      love.window.setMode(640, 480)
    else
      love.window.setMode(800, 600)
    end
  elseif key == "s" then
    seed = seed + 1
    build_terrain()
  elseif key == "escape" then
    love.event.quit()
  end
end

function love.mousepressed(x, y, button, istouch, presses)
  last_event = ("mousepressed %d,%d button %d presses %d"):format(x, y, button, presses)
  if button == 1 then
    dots[#dots + 1] = {
      x = x, y = y,
      radius = presses > 1 and 24 or 8,
      color = {love.math.random(), love.math.random(), love.math.random()},
    }
  elseif button == 2 then
    dots = {}
  end
end

function love.mousereleased(x, y, button)
  last_event = ("mousereleased %d,%d button %d"):format(x, y, button)
end

-- refuses the first quit to show that love.quit can
function love.quit()
  if not quit_armed then
    quit_armed = true
    last_event = "love.quit refused, escape again to really quit"
    return true
  end
end

local function draw_spinner(x, y)
  g.push("all")
  g.translate(x, y)
  g.rotate(spin)
  g.setLineWidth(line_width)
  g.setColor(1, 0.8, 0.1)
  g.polygon("line", -40, -30, 40, -30, 0, 45)
  g.setColor(0.8, 0.3, 0.2)
  g.circle("fill", 0, 0, 12, 6)
  g.pop()
  -- the color and line width from inside the push are gone again
end

function love.draw()
  local w, h = g.getDimensions()
  local mx, my = love.mouse.getPosition()

  -- terrain, a polyline with mitered corners
  g.setColor(0.3, 0.7, 0.4)
  g.setLineWidth(line_width)
  g.line(terrain)
  g.setLineWidth(1)

  -- shapes
  g.setColor(0.25, 0.66, 0.88)
  g.circle("fill", 80, 130, 40)
  g.setColor(1, 1, 1)
  g.circle("line", 80, 130, 46)

  g.setColor(0.9, 0.4, 0.8)
  g.ellipse("fill", 210, 130, 60, 25)

  g.setColor(1, 1, 1)
  g.setLineWidth(line_width)
  g.rectangle("line", 300, 95, 90, 70)
  g.setLineWidth(1)

  draw_spinner(470, 130)

  -- an arc from straight up around to the mouse
  local cx, cy = 620, 130
  local angle = math.atan2(my - cy, mx - cx)
  if angle < -math.pi / 2 then angle = angle + math.pi * 2 end
  g.setColor(0.2, 0.8, 0.4)
  g.arc("fill", cx, cy, 40, -math.pi / 2, angle)
  g.setColor(1, 1, 1)
  g.arc("line", "open", cx, cy, 48, -math.pi / 2, angle)

  -- scaled drawing, the line width scales with it
  g.push()
  g.translate(700, 95)
  g.scale(3)
  g.rectangle("line", 0, 0, 20, 20)
  g.circle("line", 10, 10, 6)
  g.pop()

  for _, dot in ipairs(dots) do
    g.setColor(dot.color)
    g.circle("fill", dot.x, dot.y, dot.radius)
  end

  -- crosshair, brighter while a button is held
  local held = love.mouse.isDown(1, 2, 3)
  g.setColor(1, 1, 1, held and 1 or 0.5)
  g.line(mx - 10, my, mx + 10, my)
  g.line(mx, my - 10, mx, my + 10)
  if held then
    g.circle("line", mx, my, 14)
  end

  -- window corners, to check the size after w
  g.setColor(1, 0, 0)
  g.rectangle("fill", 0, 0, 6, 6)
  g.rectangle("fill", w - 6, 0, 6, 6)
  g.rectangle("fill", 0, h - 6, 6, 6)
  g.rectangle("fill", w - 6, h - 6, 6, 6)

  g.setColor(1, 1, 1)
  local dw, dh = love.window.getDesktopDimensions()
  g.print(("fps %d   time %.1f   dt %.4f"):format(love.timer.getFPS(), love.timer.getTime(), love.timer.getDelta()), 12, 12)
  g.print(("window %dx%d   desktop %dx%d   mouse %d,%d"):format(w, h, dw, dh, mx, my), 12, 32)
  g.print(("line width %d   key repeat %s   cursor %s   seed %d"):format(
    line_width,
    love.keyboard.hasKeyRepeat() and "on" or "off",
    love.mouse.isVisible() and "shown" or "hidden",
    seed), 12, 52)
  g.print("last event: " .. last_event, 12, h - 28)

  -- printf aligns inside a box, and getWidth says how wide text will be
  local label = "right aligned, wrapped by printf to a 160 pixel box"
  g.setColor(1, 1, 1, 0.15)
  g.rectangle("fill", w - 172, 12, 160, font:getHeight() * 3)
  g.setColor(1, 1, 1)
  g.printf(label, w - 172, 12, 160, "right")
  local tag = "pivot"
  g.print(tag, mx, my - 24, math.sin(spin) * 0.5, 1, 1, font:getWidth(tag) / 2, font:getHeight() / 2)
end
