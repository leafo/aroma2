local image
local imageX, imageY = 180, 140
local moveSpeed = 10
local angle = 0

function love.load()
  love.graphics.setBackgroundColor(1, 1, 1)
  image = love.graphics.newImage("hi.png")

  print("Canvas width: " .. love.graphics.getWidth())
  print("Canvas height: " .. love.graphics.getHeight())

  local w, h = love.graphics.getDimensions()
  print("Canvas dimensions: " .. w .. "x" .. h)
end

function love.update(dt)
  angle = angle + dt
end

function love.keypressed(key)
  if key == "up" then
    imageY = imageY - moveSpeed
  elseif key == "down" then
    imageY = imageY + moveSpeed
  elseif key == "left" then
    imageX = imageX - moveSpeed
  elseif key == "right" then
    imageX = imageX + moveSpeed
  end
end

function love.draw()
  love.graphics.push()
  love.graphics.setColor(1, 0.75, 0.8)
  love.graphics.translate(400, 300)
  love.graphics.rotate(angle or 0)
  love.graphics.polygon('fill', -50, -50, 50, -50, 0, 50)
  love.graphics.pop()

  love.graphics.setColor(1, 1, 1)
  if image then
    love.graphics.draw(image, imageX, imageY, 0, 1, 1, image:getWidth() / 2, image:getHeight() / 2)
  end
end
