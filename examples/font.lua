local font

function love.load()
  love.graphics.setBackgroundColor(0.1, 0.1, 0.15)
  font = love.graphics.newImageFont("font1.png", [[ ABCDEFGHIJKLMNOPQRSTUVWXYZ[\]^_`abcdefghijklmnopqrstuvwxyz{|}~!"#$%&'()*+,-./0123456789:;<=>?]])
  print("Font loaded!")
end

function love.draw()
  love.graphics.setFont(font)
  love.graphics.setColor(1, 1, 1)
  love.graphics.print("Hello world!", 100, 100)
end
