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
  -- Draw some filled rectangles
  love.graphics.setColor(0.2, 0.4, 0.8)
  love.graphics.rectangle('fill', 50, 50, 100, 80)

  love.graphics.setColor(0.8, 0.3, 0.3)
  love.graphics.rectangle('fill', 650, 50, 120, 60)

  -- Draw some outlined rectangles
  love.graphics.setColor(0.3, 0.8, 0.4)
  love.graphics.rectangle('line', 50, 450, 150, 100)

  love.graphics.setColor(0.9, 0.7, 0.2)
  love.graphics.rectangle('line', 600, 400, 180, 150)

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
