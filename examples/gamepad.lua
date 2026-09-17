-- Gamepad input tester. Shows every connected pad, the one with the latest
-- input in detail: love's named gamepad buttons and axes on a diagram, the raw
-- button and axis numbers underneath, and a log of the callbacks as they fire.
--
-- Browsers only reveal a pad to the page after a button on it is pressed.
--
--   tab        pin the next pad instead of following the latest input
--   c          clear the log

local g = love.graphics

local font
local selected        -- Joystick being shown
local pinned = false  -- true once tab has picked a pad by hand
local log = {}
local LOG_LINES = 14

-- how long a released button keeps a fading highlight, so taps are visible
local FADE = 0.4
local released_at = {}

local function add_log(text)
  table.insert(log, 1, text)
  log[LOG_LINES + 1] = nil
end

local function follow(joystick)
  if not pinned then selected = joystick end
end

local function pad_label(joystick)
  local index = joystick:getID()
  return "#" .. index
end

function love.load()
  love.window.setTitle("gamepad tester")
  g.setBackgroundColor(0.09, 0.09, 0.12)
  g.setDefaultFilter("nearest")
  font = g.newImageFont("font1.png", [[ ABCDEFGHIJKLMNOPQRSTUVWXYZ[\]^_`abcdefghijklmnopqrstuvwxyz{|}~!"#$%&'()*+,-./0123456789:;<=>?]])
  g.setFont(font)
  selected = love.joystick.getJoysticks()[1]
end

function love.joystickadded(joystick)
  add_log(pad_label(joystick) .. " added: " .. joystick:getName())
  if not selected or not selected:isConnected() then
    selected = joystick
    pinned = false
  end
end

function love.joystickremoved(joystick)
  add_log(pad_label(joystick) .. " removed")
  if joystick == selected then
    selected = love.joystick.getJoysticks()[1]
    pinned = false
  end
end

function love.gamepadpressed(joystick, button)
  follow(joystick)
  add_log(pad_label(joystick) .. " gamepadpressed " .. button)
end

function love.gamepadreleased(joystick, button)
  released_at[button] = love.timer.getTime()
  add_log(pad_label(joystick) .. " gamepadreleased " .. button)
end

function love.joystickpressed(joystick, button)
  follow(joystick)
  add_log(pad_label(joystick) .. " joystickpressed " .. button)
end

function love.joystickreleased(joystick, button)
  released_at[button] = love.timer.getTime()
  add_log(pad_label(joystick) .. " joystickreleased " .. button)
end

-- axis callbacks fire on every change, only the big moves are worth a line
function love.gamepadaxis(joystick, axis, value)
  if math.abs(value) > 0.9 then follow(joystick) end
end

function love.keypressed(key)
  if key == "tab" then
    local pads = love.joystick.getJoysticks()
    if #pads > 0 then
      local at = 0
      for i, pad in ipairs(pads) do
        if pad == selected then at = i end
      end
      selected = pads[at % #pads + 1]
      pinned = true
    end
  elseif key == "c" then
    log = {}
  end
end

-- 1 while held, fading to 0 after the release
local function glow(down, key)
  if down then return 1 end
  local t = released_at[key]
  if not t then return 0 end
  return math.max(0, 1 - (love.timer.getTime() - t) / FADE)
end

local function set_glow(amount)
  g.setColor(0.25 + 0.75 * amount, 0.25 + 0.55 * amount, 0.3 - 0.2 * amount)
end

local function round_button(pad, name, x, y, radius, label)
  set_glow(glow(pad:isGamepadDown(name), name))
  g.circle("fill", x, y, radius)
  g.setColor(1, 1, 1, 0.6)
  g.circle("line", x, y, radius)
  g.setColor(0, 0, 0)
  local text = label or name
  g.print(text, x - font:getWidth(text) / 2, y - font:getHeight() / 2)
end

local function box_button(pad, name, x, y, w, h, label)
  set_glow(glow(pad:isGamepadDown(name), name))
  g.rectangle("fill", x, y, w, h)
  g.setColor(1, 1, 1, 0.6)
  g.rectangle("line", x, y, w, h)
  g.setColor(1, 1, 1)
  local text = label or name
  g.print(text, x + w / 2 - font:getWidth(text) / 2, y + h / 2 - font:getHeight() / 2)
end

local function stick(pad, side, x, y)
  local ax, ay = pad:getGamepadAxis(side .. "x"), pad:getGamepadAxis(side .. "y")
  local radius = 32

  -- the well lights up with the stick click
  set_glow(glow(pad:isGamepadDown(side .. "stick"), side .. "stick"))
  g.circle("fill", x, y, radius)
  g.setColor(1, 1, 1, 0.6)
  g.circle("line", x, y, radius)
  -- a common deadzone, to judge drift against
  g.setColor(1, 1, 1, 0.25)
  g.circle("line", x, y, radius * 0.2)

  -- the cap travels inside the well. blue so it still shows while clicked
  local cx, cy = x + ax * (radius - 12), y + ay * (radius - 12)
  g.setColor(0.25, 0.66, 0.88)
  g.circle("fill", cx, cy, 16)
  g.setColor(0, 0, 0, 0.6)
  g.circle("line", cx, cy, 16)
  g.circle("line", cx, cy, 9)
end

local function trigger(pad, side, x, y)
  local pull = pad:getGamepadAxis("trigger" .. side)
  g.setColor(0.25, 0.25, 0.3)
  g.rectangle("fill", x, y, 80, 16)
  g.setColor(0.2, 0.8, 0.4)
  g.rectangle("fill", x, y, 80 * pull, 16)
  g.setColor(1, 1, 1, 0.6)
  g.rectangle("line", x, y, 80, 16)
  g.setColor(1, 1, 1)
  g.printf(("%.2f"):format(pull), x, y, 80, "center")
end

-- filled shapes have to be convex, so the body is put together from a slab,
-- two shoulders and two grips
local function draw_body(x, y)
  g.setColor(0.17, 0.17, 0.22)
  g.rectangle("fill", x + 90, y + 44, 260, 150)
  g.circle("fill", x + 90, y + 114, 70)
  g.circle("fill", x + 350, y + 114, 70)
  g.ellipse("fill", x + 72, y + 196, 56, 84)
  g.ellipse("fill", x + 368, y + 196, 56, 84)
end

-- laid out like an xbox pad, which is also where love's button names come
-- from: left stick above the dpad, face buttons above the right stick
local function draw_diagram(pad, x, y)
  trigger(pad, "left", x + 50, y)
  trigger(pad, "right", x + 310, y)
  box_button(pad, "leftshoulder", x + 40, y + 22, 100, 20, "lshoulder")
  box_button(pad, "rightshoulder", x + 300, y + 22, 100, 20, "rshoulder")

  draw_body(x, y)

  stick(pad, "left", x + 96, y + 112)
  stick(pad, "right", x + 284, y + 186)

  local dx, dy = x + 160, y + 186
  box_button(pad, "dpup", dx - 12, dy - 36, 24, 24, "^")
  box_button(pad, "dpdown", dx - 12, dy + 12, 24, 24, "v")
  box_button(pad, "dpleft", dx - 36, dy - 12, 24, 24, "<")
  box_button(pad, "dpright", dx + 12, dy - 12, 24, 24, ">")

  local fx, fy = x + 346, y + 112
  round_button(pad, "y", fx, fy - 28, 14)
  round_button(pad, "a", fx, fy + 28, 14)
  round_button(pad, "x", fx - 28, fy, 14)
  round_button(pad, "b", fx + 28, fy, 14)

  box_button(pad, "back", x + 158, y + 100, 44, 20)
  round_button(pad, "guide", x + 220, y + 78, 16, "G")
  box_button(pad, "start", x + 238, y + 100, 44, 20)

  g.setColor(1, 1, 1)
  g.printf(("left %+.2f %+.2f   right %+.2f %+.2f"):format(
    pad:getGamepadAxis("leftx"), pad:getGamepadAxis("lefty"),
    pad:getGamepadAxis("rightx"), pad:getGamepadAxis("righty")), x, y + 284, 440, "center")
end

-- every button and axis by number, which is all an unmapped pad has
local function draw_raw(pad, x, y)
  g.setColor(1, 1, 1)
  g.print(("raw: %d buttons, %d axes, %d hats"):format(pad:getButtonCount(), pad:getAxisCount(), pad:getHatCount()), x, y)

  for i = 1, pad:getButtonCount() do
    local col, row = (i - 1) % 12, math.floor((i - 1) / 12)
    local bx, by = x + col * 36, y + 24 + row * 28
    set_glow(glow(pad:isDown(i), i))
    g.rectangle("fill", bx, by, 32, 24)
    g.setColor(1, 1, 1)
    g.print(tostring(i), bx + 16 - font:getWidth(tostring(i)) / 2, by + 4)
  end

  local rows = math.ceil(pad:getButtonCount() / 12)
  local ay = y + 32 + rows * 28
  for i, value in ipairs({pad:getAxes()}) do
    local by = ay + (i - 1) * 20
    g.setColor(1, 1, 1)
    g.print(("axis %d"):format(i), x, by)
    g.setColor(0.25, 0.25, 0.3)
    g.rectangle("fill", x + 70, by + 2, 200, 12)
    g.setColor(1, 0.8, 0.1)
    g.rectangle("fill", x + 170, by + 2, value * 100, 12)
    g.setColor(1, 1, 1, 0.5)
    g.line(x + 170, by, x + 170, by + 16)
    g.setColor(1, 1, 1)
    g.print(("%+.3f"):format(value), x + 280, by)
  end
end

function love.draw()
  local w, h = g.getDimensions()
  local pads = love.joystick.getJoysticks()

  g.setColor(1, 1, 1)
  g.print("gamepad tester", 16, 12)
  g.setColor(1, 1, 1, 0.6)
  g.printf("tab: next pad   c: clear log", 0, 12, w - 16, "right")

  if #pads == 0 then
    g.setColor(1, 1, 1)
    g.printf("no gamepad yet\n\npress a button on it, browsers keep a pad hidden from the page until then", 0, h / 2 - 40, w, "center")
    return
  end

  -- every pad, the one being shown marked
  for i, pad in ipairs(pads) do
    local y = 36 + (i - 1) * 18
    if pad == selected then
      g.setColor(1, 0.8, 0.1)
      g.print(pinned and "*" or ">", 16, y)
    else
      g.setColor(1, 1, 1, 0.6)
    end
    g.print(("%s %s%s"):format(pad_label(pad), pad:getName(), pad:isGamepad() and "" or "  (no standard mapping, raw only)"), 32, y)
  end

  local top = 36 + #pads * 18 + 24
  if selected and selected:isConnected() then
    draw_diagram(selected, 16, top)
    draw_raw(selected, 16, top + 316)

    -- love's gamepad functions only answer for pads the browser maps to its
    -- standard layout. For any other pad the diagram stays dark and only
    -- the raw numbers underneath move
    if not selected:isGamepad() then
      g.setColor(0.09, 0.09, 0.12, 0.75)
      g.rectangle("fill", 16, top - 4, 440, 310)
      g.setColor(1, 0.8, 0.1)
      g.printf("the browser has no standard mapping for this pad, so isGamepad is false and the named buttons and axes report nothing.\n\nthe raw buttons and axes below still work", 46, top + 100, 380, "center")
    end
  end

  -- callback log, newest on top and fading with age
  g.setColor(1, 1, 1)
  g.print("callbacks", 470, top - 8)
  for i, line in ipairs(log) do
    g.setColor(1, 1, 1, 1 - (i - 1) / LOG_LINES)
    g.print(line, 470, top + 12 + (i - 1) * 18)
  end
end
