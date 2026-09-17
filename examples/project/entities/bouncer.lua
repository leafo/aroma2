local assets = require "assets"

local Bouncer = {}
Bouncer.__index = Bouncer

local SCALE = 0.2

local function new(_, x, y)
  return setmetatable({
    x = x, y = y,
    vx = love.math.random(-120, 120), vy = 0,
    spin = love.math.random() * 4 - 2,
    angle = 0,
  }, Bouncer)
end

function Bouncer:update(dt)
  local w, h = love.graphics.getDimensions()
  local radius = assets.image:getWidth() * SCALE / 2

  self.vy = self.vy + 600 * dt
  self.x = self.x + self.vx * dt
  self.y = self.y + self.vy * dt
  self.angle = self.angle + self.spin * dt

  if self.y > h - radius then
    self.y = h - radius
    self.vy = -self.vy * 0.85
  end
  if self.x < radius or self.x > w - radius then
    self.x = math.max(radius, math.min(w - radius, self.x))
    self.vx = -self.vx
  end
end

function Bouncer:draw()
  local image = assets.image
  love.graphics.draw(image, self.x, self.y, self.angle, SCALE, SCALE, image:getWidth() / 2, image:getHeight() / 2)
end

return setmetatable(Bouncer, { __call = new })
