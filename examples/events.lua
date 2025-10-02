
local angle = 0
local speed = 100
local x, y = 400, 300

function love.load()
  love.graphics.setBackgroundColor(0.1, 0.1, 0.15)
  print("Keyboard Events Demo - check browser console")
  print("Press any key to see events...")
  print("Try holding down space or arrow keys!")
end

function love.keypressed(key)
  print("KEY PRESSED:  " .. key)
end

function love.keyreleased(key)
  print("KEY RELEASED: " .. key)
end

function love.update(dt)
  angle = angle + dt

  -- Demonstrate love.keyboard.isDown
  if love.keyboard.isDown("space") then
    angle = angle + dt * 2  -- Spin faster when space is held
  end

  if love.keyboard.isDown("up") then
    y = y - dt * speed
  end

  if love.keyboard.isDown("down") then
    y = y + dt * speed
  end

  if love.keyboard.isDown("left") then
    x = x - dt * speed
  end

  if love.keyboard.isDown("right") then
    x = x + dt * speed
  end
end

function love.draw()
  -- Change color based on whether space is held
  if love.keyboard.isDown("space") then
    love.graphics.setColor(0.3, 1, 0.3)  -- Green when space held
  else
    love.graphics.setColor(1, 0.75, 0.8)  -- Pink normally
  end

  love.graphics.push()
  love.graphics.translate(x, y)
  love.graphics.rotate(angle or 0)
  love.graphics.polygon('fill', -50, -50, 50, -50, 0, 50)
  love.graphics.pop()
end
