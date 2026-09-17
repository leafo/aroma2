local assets = {
  image = love.graphics.newImage("assets/hi.png"),
  blip = love.audio.newSource("assets/blip.wav", "static"),
  font = love.graphics.newImageFont("assets/font1.png", [[ ABCDEFGHIJKLMNOPQRSTUVWXYZ[\]^_`abcdefghijklmnopqrstuvwxyz{|}~!"#$%&'()*+,-./0123456789:;<=>?]]),
  notes = {},
}

for line in love.filesystem.lines("assets/notes.txt") do
  table.insert(assets.notes, line)
end

return assets
