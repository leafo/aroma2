-- A project with more than one file: conf.lua sizes the window, modules come
-- in through require, and the font, image and sound are read from the .love
--
--   click or space    drop a bouncer, with a blip

local assets = require "assets"
local Bouncer = require "entities.bouncer"

local g = love.graphics
local bouncers = {}

local function spawn(x, y)
  table.insert(bouncers, Bouncer(x, y))
  assets.blip:clone():play()
end

function love.load()
  g.setBackgroundColor(0.1, 0.1, 0.14)
  g.setFont(assets.font)
  for i = 1, 3 do
    table.insert(bouncers, Bouncer(80 * i, 60))
  end
end

function love.update(dt)
  for _, bouncer in ipairs(bouncers) do
    bouncer:update(dt)
  end
end

function love.mousepressed(x, y)
  spawn(x, y)
end

function love.keypressed(key)
  if key == "space" then
    spawn(love.math.random(40, g.getWidth() - 40), 40)
  end
end

function love.draw()
  g.setColor(1, 1, 1)
  for _, bouncer in ipairs(bouncers) do
    bouncer:draw()
  end
  g.print(("%d bouncers, click or space for more"):format(#bouncers), 10, 10)

  -- read through love.filesystem
  local y = g.getHeight() - 18 * #assets.notes - 6
  g.setColor(1, 1, 1, 0.6)
  for i, line in ipairs(assets.notes) do
    g.print(line, 10, y + (i - 1) * 18)
  end
end
