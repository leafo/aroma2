
-- Bubble Spinner: shoot bubbles at the spinning cluster, match 3 or more of
-- a color to pop them, knock loose anything disconnected. Arrows aim, space
-- shoots. All rendering is polygon/rectangle primitives plus the image font.

import rectangle, polygon, setColor from aroma.graphics
import insert, remove from table

_print = aroma.graphics.print

TAU = math.pi * 2
W, H = aroma.graphics.getWidth!, aroma.graphics.getHeight!
CX, CY = W / 2, 250
SX, SY = W / 2, H - 55
R = 18
SPACING = R * 2
ROW_H = SPACING * math.sqrt(3) / 2
LIMIT = 235
SHOT_SPEED = 620

COLORS = {
  {231, 76, 60}
  {46, 204, 113}
  {52, 152, 219}
  {241, 196, 15}
  {155, 89, 182}
}

DIRS = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, -1}, {-1, 1}}

circle = (x, y, r, segs=14) ->
  pts = {}
  for i = 0, segs - 1
    a = i / segs * TAU
    insert pts, x + math.cos(a) * r
    insert pts, y + math.sin(a) * r
  polygon "fill", unpack pts

ring = (x, y, r, dots=48) ->
  for i = 1, dots
    a = i / dots * TAU
    circle x + math.cos(a) * r, y + math.sin(a) * r, 2, 6

cell_key = (q, r) -> q .. "," .. r

cell_xy = (q, r) -> (q + r / 2) * SPACING, r * ROW_H

-- fractional axial coords rounded to the nearest hex cell
nearest_cell = (x, y) ->
  rf = y / ROW_H
  qf = x / SPACING - rf / 2
  sf = -qf - rf
  q = math.floor(qf + 0.5)
  r = math.floor(rf + 0.5)
  s = math.floor(sf + 0.5)
  dq = math.abs(q - qf)
  dr = math.abs(r - rf)
  ds = math.abs(s - sf)
  if dq > dr and dq > ds
    q = -r - s
  elseif dr > ds
    r = -q - s
  q, r

bind_state = (state) ->
  for ev in *{"update", "draw", "keypressed"}
    aroma[ev] = state[ev] and (...) -> state[ev] state, ...

class Game
  new: =>
    @cluster = {}
    @theta = 0
    @omega = 0.5
    @flying = nil
    @falling = {}
    @score = 0
    @state = "title"
    @aim = -TAU / 4

    for q = -3, 3
      for r = -3, 3
        d = (math.abs(q) + math.abs(r) + math.abs(q + r)) / 2
        if d > 0 and d <= 3
          @cluster[cell_key q, r] = {:q, :r, color: math.random #COLORS}

    @current = @pick_color!
    @next = @pick_color!

  colors_left: =>
    seen = {}
    seen[b.color] = true for _, b in pairs @cluster
    [c for c in pairs seen]

  pick_color: =>
    left = @colors_left!
    return 1 if #left == 0
    left[math.random #left]

  free_cell: (q, r) =>
    return false if q == 0 and r == 0
    not @cluster[cell_key q, r]

  shoot: =>
    return if @flying
    @flying = {
      x: SX
      y: SY
      vx: math.cos(@aim) * SHOT_SPEED
      vy: math.sin(@aim) * SHOT_SPEED
      color: @current
    }

  update: (dt) =>
    @omega *= math.exp(-dt * 0.5)
    @theta += @omega * dt

    if @state == "play"
      turn = 2.6 * dt
      @aim -= turn if aroma.keyboard.isDown "left"
      @aim += turn if aroma.keyboard.isDown "right"
      @aim = math.max -TAU / 4 - 1.25, math.min(-TAU / 4 + 1.25, @aim)

    if fly = @flying
      fly.x += fly.vx * dt
      fly.y += fly.vy * dt
      if fly.x < R and fly.vx < 0
        fly.vx = -fly.vx
      elseif fly.x > W - R and fly.vx > 0
        fly.vx = -fly.vx
      fly.vy = -fly.vy if fly.y < R and fly.vy < 0
      if fly.y > H + R
        @flying = nil
      else
        @check_hit fly

    alive = {}
    for b in *@falling
      b.vy += 900 * dt
      b.x += b.vx * dt
      b.y += b.vy * dt
      insert alive, b if b.y < H + R * 2
    @falling = alive

  check_hit: (fly) =>
    thresh = (SPACING * 0.9) ^ 2
    dxc, dyc = fly.x - CX, fly.y - CY
    if dxc * dxc + dyc * dyc < thresh
      return @attach fly

    c, s = math.cos(@theta), math.sin(@theta)
    for _, b in pairs @cluster
      lx, ly = cell_xy b.q, b.r
      wx = CX + lx * c - ly * s
      wy = CY + lx * s + ly * c
      dx, dy = fly.x - wx, fly.y - wy
      if dx * dx + dy * dy < thresh
        return @attach fly

  attach: (fly) =>
    @flying = nil
    c, s = math.cos(-@theta), math.sin(-@theta)
    dx, dy = fly.x - CX, fly.y - CY
    lx = dx * c - dy * s
    ly = dx * s + dy * c

    q, r = nearest_cell lx, ly
    unless @free_cell q, r
      best, best_d = nil, nil
      for d in *DIRS
        nq, nr = q + d[1], r + d[2]
        continue unless @free_cell nq, nr
        x, y = cell_xy nq, nr
        dist = (x - lx) ^ 2 + (y - ly) ^ 2
        if not best_d or dist < best_d
          best, best_d = {nq, nr}, dist
      return unless best
      q, r = best[1], best[2]

    @cluster[cell_key q, r] = {:q, :r, color: fly.color}

    -- impact spins the wheel: tangential velocity over distance
    d2 = dx * dx + dy * dy
    @omega += (dx * fly.vy - dy * fly.vx) / d2 * 0.6 if d2 > 1
    @omega = math.max -5, math.min(5, @omega)

    @resolve q, r
    @current = @next
    @next = @pick_color!

  flood: (q, r, color) =>
    seen = {}
    out = {}
    stack = {{q, r}}
    while #stack > 0
      cur = remove stack
      k = cell_key cur[1], cur[2]
      continue if seen[k]
      seen[k] = true
      b = @cluster[k]
      continue unless b and b.color == color
      insert out, b
      for d in *DIRS
        insert stack, {cur[1] + d[1], cur[2] + d[2]}
    out

  drop_disconnected: =>
    seen = {}
    stack = {}
    for d in *DIRS
      k = cell_key d[1], d[2]
      if @cluster[k]
        seen[k] = true
        insert stack, @cluster[k]

    while #stack > 0
      b = remove stack
      for d in *DIRS
        k = cell_key b.q + d[1], b.r + d[2]
        nb = @cluster[k]
        if nb and not seen[k]
          seen[k] = true
          insert stack, nb

    c, s = math.cos(@theta), math.sin(@theta)
    for k, b in pairs @cluster
      unless seen[k]
        lx, ly = cell_xy b.q, b.r
        wx = CX + lx * c - ly * s
        wy = CY + lx * s + ly * c
        insert @falling, {
          x: wx
          y: wy
          vx: (wx - CX) * 1.5 + math.random(-40, 40)
          vy: (wy - CY) * 1.5 - 120
          color: b.color
        }
        @cluster[k] = nil
        @score += 20

  resolve: (q, r) =>
    placed = @cluster[cell_key q, r]
    match = @flood q, r, placed.color
    if #match >= 3
      for b in *match
        @cluster[cell_key b.q, b.r] = nil
      @score += #match * 10
      @drop_disconnected!

    for _, b in pairs @cluster
      x, y = cell_xy b.q, b.r
      if x * x + y * y > (LIMIT - R) ^ 2
        @state = "over"
        return

    @state = "win" unless next @cluster

  keypressed: (key) =>
    switch @state
      when "title"
        @state = "play"
      when "play"
        @shoot! if key == " " or key == "up"
      when "over", "win"
        if key == "return"
          game = Game!
          game.state = "play"
          bind_state game

  draw_bubble: (x, y, color, r=R) =>
    c = COLORS[color]
    setColor c[1] * 0.45, c[2] * 0.45, c[3] * 0.45
    circle x, y, r
    setColor c
    circle x, y, r - 2.5

  draw: =>
    setColor 45, 45, 62
    ring CX, CY, LIMIT

    aroma.graphics.push!
    aroma.graphics.translate CX, CY
    aroma.graphics.rotate @theta
    setColor 85, 85, 100
    circle 0, 0, R
    setColor 150, 150, 165
    circle 0, 0, R * 0.45, 8
    for _, b in pairs @cluster
      x, y = cell_xy b.q, b.r
      @draw_bubble x, y, b.color
    aroma.graphics.pop!

    for b in *@falling
      @draw_bubble b.x, b.y, b.color

    if fly = @flying
      @draw_bubble fly.x, fly.y, fly.color

    -- shooter: aim dots, loaded bubble, next preview
    if @state == "play" or @state == "title"
      setColor 110, 110, 135
      for i = 1, 6
        d = i * 36
        circle SX + math.cos(@aim) * d, SY + math.sin(@aim) * d, 3, 8
      @draw_bubble SX, SY, @current
      @draw_bubble 70, SY, @next, R * 0.7
      setColor 150, 150, 165
      _print "next", 48, SY + 20

    setColor 220, 220, 230
    _print "score: " .. @score, 20, 16

    switch @state
      when "title"
        setColor 0, 0, 0, 140
        rectangle "fill", 0, 0, W, H
        setColor 240, 240, 245
        _print "BUBBLE SPINNER", CX - 60, CY - 30
        _print "arrows aim, space shoots", CX - 100, CY
        _print "match 3 or more to pop - press any key", CX - 150, CY + 24
      when "over"
        setColor 0, 0, 0, 140
        rectangle "fill", 0, 0, W, H
        setColor 240, 240, 245
        _print "game over! score: " .. @score, CX - 90, CY - 12
        _print "enter to play again", CX - 80, CY + 12
      when "win"
        setColor 0, 0, 0, 140
        rectangle "fill", 0, 0, W, H
        setColor 240, 240, 245
        _print "cleared! score: " .. @score, CX - 80, CY - 12
        _print "enter to play again", CX - 80, CY + 12

aroma.load = ->
  font = aroma.graphics.newImageFont "font1.png",
    [[ ABCDEFGHIJKLMNOPQRSTUVWXYZ[\]^_`abcdefghijklmnopqrstuvwxyz{|}~!"#$%&'()*+,-./0123456789:;<=>?]]
  aroma.graphics.setFont font
  aroma.graphics.setBackgroundColor 18, 18, 26
  bind_state Game!
