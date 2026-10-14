-- AstraUE: Astra (the Astra companion's avatar) in any Unreal Engine game - the game's half.
--
-- Every frame, on the game thread (UE4SS's engine-tick loop), this reads what EVERY UE game has:
--   * the local PlayerController's PlayerCameraManager - its final view (GetCameraLocation/Rotation, GetFOVAngle:
--     the camera cache the game renders with, whatever camera logic the game's subclass runs);
--   * the possessed Pawn - where it stands, its velocity, whether it is falling;
-- walks Astra beside the player (a small follow brain: a spot beside and behind, catch up, keep still while the
-- player stands), and hands camera + her placement to the astra-reshade add-on (ReShade, same process) over its
-- named pipe, one text line each (include/astra_provider.h, PIPE PROTOCOL). The add-on does the rest: her engine,
-- her picture, compositing with the game's depth.
--
-- Coordinates sent: glTF (right-handed, +Y up, metres) = (x, z, y) * 0.01 of UE's (x forward, y right, z up) cm.
-- Her height is the ground under HER: a line trace straight down where she stands (KismetSystemLibrary's
-- LineTraceSingle, the Visibility channel), a few times a second and whenever she moved - so she stands on slopes and
-- stairs. A trace that misses (or a game where tracing fails) leaves her on the player's ground.
-- The game's sun: its main DirectionalLightComponent (found again every few seconds: the brightest one that lights
-- the world, the atmosphere's sun first), its direction, colour and temperature, and whether it reaches her (a trace
-- from her chest toward it) - sent as the provider API's light, so her engine lights her with it and casts her shadow.

local M = {}

M.PIPE = [[\\.\pipe\astra-provider]]
M.INTEGRATION = "astra.unreal"

-- the follow brain's numbers (UE centimetres, seconds)
M.BACK, M.SIDE = 140.0, 90.0    -- her spot: behind and to the right of the player
M.NEAR_ENOUGH = 35.0            -- she stops this close to her spot
M.STAY_RADIUS = 300.0           -- while the player stands, she stays put if she is this near him
M.TELEPORT = 2500.0             -- further than this from her spot (or a floor apart): she is put there
M.FLOOR_APART = 300.0
M.WALK_MIN = 150.0              -- her slowest catch-up pace (cm/s)
M.RUN_MAX = 950.0
M.STILL = 30.0                  -- the player "stands" below this speed (cm/s)

-- the ground under her (UE centimetres, seconds)
M.TRACE_MIN, M.TRACE_MAX = 0.05, 0.25 -- never more often than this; at least this often
M.TRACE_MOVE = 15.0             -- sooner than TRACE_MAX once she moved this far
M.TRACE_UP, M.TRACE_DOWN = 100.0, 300.0 -- from this far above the higher of her and the player's feet, to this far below the lower
M.GROUND_KEEP = 1.0             -- a trace that misses keeps the last ground this long, then the player's
M.TRACE_CHANNEL = 0             -- ETraceTypeQuery: 0 = TraceTypeQuery1, the Visibility channel in every UE game

-- the game's sun
M.SUN_FIND_EVERY = 5.0          -- seconds between searches for the sun (a level change, a time of day swapping lights)
M.SUN_EVERY = 0.2               -- seconds between light lines (the add-on sends ~10 a second to her engine)
M.SUN_LUX = nil                 -- a game's daylight in lux, if its lights use physical units: intensity = lux / SUN_LUX
                                -- (nil: every sun is a daylight sun, 1 - her engine bounds her level, the game's exposure ours)
M.SUN_CHEST = 120.0             -- her chest above her feet (cm): where the sun is traced from
M.SUN_REACH = 20000.0           -- how far toward the sun a blocker counts (cm)

---------------------------------------------------------------------------------------------------- maths

function M.to_gl(x, y, z)
    return x * 0.01, z * 0.01, y * 0.01
end

function M.dir_to_gl(x, y, z)
    return x, z, y
end

--- An FRotator's forward and up (UE axes), as FRotationMatrix's X and Z axes.
function M.rotator_axes(pitch, yaw, roll)
    local p, y, r = math.rad(pitch), math.rad(yaw), math.rad(roll)
    local SP, CP, SY, CY, SR, CR = math.sin(p), math.cos(p), math.sin(y), math.cos(y), math.sin(r), math.cos(r)
    return CP * CY, CP * SY, SP, -(CR * SP * CY + SR * SY), CY * SR - CR * SP * SY, CR * CP
end

local function fmt(v)
    return string.format("%.5f", v)
end

--- The camera line: C <frame> <pos gl m> <fwd gl> <up gl> <fov> <h|v> <aspect> <near m>
function M.camera_line(frame, loc, rot, fov, horizontal, aspect, near_cm)
    local px, py, pz = M.to_gl(loc.X, loc.Y, loc.Z)
    local fx, fy, fz, ux, uy, uz = M.rotator_axes(rot.Pitch, rot.Yaw, rot.Roll)
    fx, fy, fz = M.dir_to_gl(fx, fy, fz)
    ux, uy, uz = M.dir_to_gl(ux, uy, uz)
    return string.format("C %d %.4f %.4f %.4f %s %s %s %s %s %s %.4f %s %.5f %.4f\n", frame, px, py, pz, fmt(fx), fmt(fy), fmt(fz), fmt(ux),
        fmt(uy), fmt(uz), fov, horizontal and "h" or "v", aspect or 0, (near_cm or 0) * 0.01)
end

--- Her line: A <feet gl m> <fwd gl> <up gl> <vel gl m/s> <speed m/s> <airborne> <scale>
function M.avatar_line(s)
    local px, py, pz = M.to_gl(s.x, s.y, s.z)
    local yr = math.rad(s.yaw)
    local fx, fy, fz = M.dir_to_gl(math.cos(yr), math.sin(yr), 0)
    local vx, vy, vz = M.dir_to_gl(s.vx * 0.01, s.vy * 0.01, 0)
    return string.format("A %.4f %.4f %.4f %s %s %s 0 1 0 %.3f %.3f %.3f %.3f %d 1\n", px, py, pz, fmt(fx), fmt(fy), fmt(fz), vx, vy, vz,
        s.speed, s.airborne and 1 or 0)
end

--- Turns `from` toward `to` (degrees) by at most `step` degrees.
function M.turn(from, to, step)
    local d = (to - from + 540) % 360 - 180
    if d > step then d = step elseif d < -step then d = -step end
    return (from + d + 540) % 360 - 180
end

---------------------------------------------------------------------------------------------------- the sun

--- An sRGB 8-bit channel (an FColor's) to linear.
function M.srgb_to_linear(c)
    c = c / 255
    if c <= 0.04045 then return c / 12.92 end
    return ((c + 0.055) / 1.055) ^ 2.4
end

--- A black body's colour at `kelvin` (linear RGB, max 1): Tanner Helland's fit, the curve Unreal's light temperature follows.
function M.temperature_rgb(kelvin)
    local t = math.max(1000, math.min(kelvin, 40000)) / 100
    local r, g, b
    if t <= 66 then
        r = 255
        g = 99.4708025861 * math.log(t) - 161.1195681661
        b = t <= 19 and 0 or 138.5177312231 * math.log(t - 10) - 305.0447927307
    else
        r = 329.698727446 * (t - 60) ^ -0.1332047592
        g = 288.1221695283 * (t - 60) ^ -0.0755148492
        b = 255
    end
    local function c(v) return M.srgb_to_linear(math.max(0, math.min(255, v))) end
    r, g, b = c(r), c(g), c(b)
    local m = math.max(r, g, b, 1e-6)
    return r / m, g / m, b / m
end

--- Of the directional lights (each {intensity, affects, visible, atmosphere, name}), the sun: lights the world,
--- shown, the atmosphere's sun first, then the brightest. Returns its index or nil.
function M.pick_sun(lights)
    local best, best_score = nil, -1
    for i, l in ipairs(lights) do
        if l.affects ~= false and l.visible ~= false and (l.intensity or 0) > 0 and not (l.name or ""):find("Default__", 1, true) then
            local score = (l.atmosphere and 1e9 or 0) + l.intensity
            if score > best_score then best, best_score = i, score end
        end
    end
    return best
end

--- The light line: L <toward the sun, glTF> <linear colour> <intensity> <visible>. rot: the light component's rotation
--- (it shines along its forward axis); color: its FColor; temperature: kelvin or nil; lux: its Intensity.
function M.light_line(rot, color, temperature, lux, visible)
    local fx, fy, fz = M.rotator_axes(rot.Pitch, rot.Yaw, rot.Roll)
    local tx, ty, tz = M.dir_to_gl(-fx, -fy, -fz)
    local r, g, b = M.srgb_to_linear(color.R or 255), M.srgb_to_linear(color.G or 255), M.srgb_to_linear(color.B or 255)
    if temperature then
        local kr, kg, kb = M.temperature_rgb(temperature)
        r, g, b = r * kr, g * kg, b * kb
    end
    local m = math.max(r, g, b, 1e-6)
    local intensity = M.SUN_LUX and math.max(0, math.min((lux or 0) / M.SUN_LUX, 16)) or 1
    return string.format("L %s %s %s %.4f %.4f %.4f %.3f %.2f\n", fmt(tx), fmt(ty), fmt(tz), r / m, g / m, b / m, intensity, visible)
end

---------------------------------------------------------------------------------------------------- the ground

--- Should she trace the ground now? g: the last trace {at, x, y}; at most every TRACE_MIN, at least every TRACE_MAX,
--- and in between once she moved TRACE_MOVE.
function M.should_trace(g, now, x, y)
    if g.at == nil then return true end
    local since = now - g.at
    if since < M.TRACE_MIN then return false end
    if since >= M.TRACE_MAX then return true end
    local dx, dy = x - g.x, y - g.y
    return dx * dx + dy * dy >= M.TRACE_MOVE * M.TRACE_MOVE
end

--- Her ground: under her (`ground(x, y, top, bottom)` -> the height hit, or nil: a line trace down), rate-limited;
--- a miss keeps the last one GROUND_KEEP seconds; with no trace, the player's ground while he stands on it.
function M.update_ground(s, p, now, ground, force)
    s.trace = s.trace or {}
    local g = s.trace
    if ground ~= nil and now ~= nil and (force or M.should_trace(g, now, s.x, s.y)) then
        local z = ground(s.x, s.y, math.max(s.z, p.z) + M.TRACE_UP, math.min(s.z, p.z) - M.TRACE_DOWN)
        g.at, g.x, g.y, g.traces = now, s.x, s.y, (g.traces or 0) + 1
        if z ~= nil then
            g.z, g.hit_at = z, now
        else
            g.misses = (g.misses or 0) + 1
        end
    end
    if g.z ~= nil and now ~= nil and now - g.hit_at <= M.GROUND_KEEP then
        s.ground = g.z
    elseif not p.falling then
        s.ground = p.z
    end
end

---------------------------------------------------------------------------------------------------- follow brain

--- One step of her walking. s: her state (UE cm); p: the player {x, y, z (feet), yaw, vx, vy, falling}; dt seconds;
--- now: a clock (seconds) and ground: a line trace down (both optional: without them she stands on the player's ground).
function M.follow(s, p, dt, now, ground)
    local yr = math.rad(p.yaw)
    local fx, fy = math.cos(yr), math.sin(yr)
    local rx, ry = -fy, fx -- UE: +Y is to the right of +X
    local tx, ty = p.x - fx * M.BACK + rx * M.SIDE, p.y - fy * M.BACK + ry * M.SIDE
    if s.placed then M.update_ground(s, p, now, ground) end
    local pspeed = math.sqrt(p.vx * p.vx + p.vy * p.vy)
    local to_spot = math.sqrt((tx - s.x) ^ 2 + (ty - s.y) ^ 2)
    if not s.placed or to_spot > M.TELEPORT or math.abs((s.ground or p.z) - s.z) > M.FLOOR_APART then
        -- put at her spot, on the ground there (traced at once)
        s.x, s.y, s.z, s.yaw = tx, ty, p.z, p.yaw
        if s.trace then s.trace.z = nil end
        M.update_ground(s, p, now, ground, true)
        s.z = s.ground or p.z
        s.v, s.vx, s.vy, s.speed, s.placed, s.teleports = 0, 0, 0, 0, true, (s.teleports or 0) + 1
        return s
    end
    -- while the player stands (or turns on the spot), she stays where she is near him
    local to_player = math.sqrt((p.x - s.x) ^ 2 + (p.y - s.y) ^ 2)
    local goal_x, goal_y = tx, ty
    if pspeed < M.STILL and to_player < M.STAY_RADIUS and to_player > 60 then goal_x, goal_y = s.x, s.y end
    local dx, dy = goal_x - s.x, goal_y - s.y
    local d = math.sqrt(dx * dx + dy * dy)
    local want = 0
    if d > M.NEAR_ENOUGH then want = math.min(math.max(pspeed * 1.1, d * 1.5, M.WALK_MIN), M.RUN_MAX) end
    s.v = (s.v or 0) + (want - (s.v or 0)) * math.min(1, dt * 6)
    local step = math.min(s.v * dt, d)
    if d > 1e-3 and step > 0 then
        s.x, s.y = s.x + dx / d * step, s.y + dy / d * step
        if step > 0.3 then s.yaw = M.turn(s.yaw, math.deg(math.atan(dy, dx)), 540 * dt) end
    elseif pspeed < M.STILL then
        s.yaw = M.turn(s.yaw, p.yaw, 90 * dt) -- standing: turn slowly the way he looks
    end
    s.z = s.z + ((s.ground or p.z) - s.z) * math.min(1, dt * 8)
    local inv = dt > 0 and 1 / dt or 0
    s.vx, s.vy = (d > 1e-3 and dx / d * step or 0) * inv, (d > 1e-3 and dy / d * step or 0) * inv
    -- her pace in m/s, steady (the engine's animation set reads it)
    local pace = math.sqrt(s.vx * s.vx + s.vy * s.vy) * 0.01
    s.speed = (s.speed or 0) + (pace - (s.speed or 0)) * math.min(1, dt * 10)
    if s.speed < 0.05 then s.speed = 0 end
    s.airborne = false
    return s
end

---------------------------------------------------------------------------------------------------- the game

if ASTRAUE_TEST then return M end -- tests/astraue_tests.lua loads the maths only

local UEHelpers = require("UEHelpers")

local pipe, next_open = nil, 0
local pc, next_find = nil, 0
local cache = { at = -1, paused_at = -1 }
local her = { placed = false, x = 0, y = 0, z = 0, yaw = 0, v = 0, vx = 0, vy = 0, speed = 0 }
local last_frame, last_clock, own_frame, frame_counter = -1, nil, 0, nil
local stats = { frames = 0, sent = 0, cost = 0, logged_at = 0, errors = 0 }
local kismet = nil
local NO_COLOR = { R = 0, G = 0, B = 0, A = 0 }
local trace_broken = false
local sun = { comp = nil, find_at = -1e9, sent_at = -1e9, said = nil, visible = 1 }

local function log(fmtstr, ...)
    print(string.format("[AstraUE] " .. fmtstr .. "\n", ...))
end

local function send(text)
    if not pipe then return false end
    local ok = pipe:write(text)
    if not ok then
        log("the add-on's pipe closed")
        pipe:close()
        pipe = nil
        return false
    end
    return true
end

local function open_pipe(now)
    if pipe or now < next_open then return end
    next_open = now + 2
    local f = io.open(M.PIPE, "r+b")
    if not f then return end
    f:setvbuf("no")
    pipe = f
    local name = "Unreal game"
    pcall(function()
        local n = kismet:GetGameName():ToString()
        if n and #n > 0 then name = n end
    end)
    log("connected to the astra-reshade add-on (%s)", name)
    local version = "?"
    pcall(function() version = string.format("%d.%d", UnrealVersion.GetMajor(), UnrealVersion.GetMinor()) end)
    send(string.format("I %s %s\nM AstraUE connected (UE %s)\n", M.INTEGRATION, name, version))
end

local function valid(o)
    return o ~= nil and o:IsValid()
end

--- Values that change rarely, read once a second: the camera's near plane and fov axis, the pawn's capsule.
local function refresh_cache(now, pcm, pawn)
    if now - cache.at < 1 then return end
    cache.at = now
    cache.horizontal, cache.aspect, cache.near = true, 0, 0
    pcall(function()
        local c = pc.Player.AspectRatioAxisConstraint -- 0 MaintainYFOV, 1 MaintainXFOV, 2 MajorAxisFOV
        cache.horizontal = c ~= 0
    end)
    pcall(function()
        local pov = pcm.CameraCachePrivate.POV
        if pov.bConstrainAspectRatio then cache.aspect = pov.AspectRatio end
        if pov.PerspectiveNearClipPlane and pov.PerspectiveNearClipPlane > 0 then cache.near = pov.PerspectiveNearClipPlane end
    end)
    cache.half_height = 0
    pcall(function() cache.half_height = pawn.CapsuleComponent:GetScaledCapsuleHalfHeight() end)
end

--- Is the game paused (its pause menu)? Read ten times a second: she hides as the menu opens. GameplayStatics'
--- IsGamePaused, a UFunction in every UE4/UE5 - APlayerController's own IsPaused is plain C++, not callable from
--- here (UE 5.6 live: "attempt to call a TrivialObject value"). A read that fails says so once and counts as running.
local function paused(now)
    if now - cache.paused_at < 0.1 then return cache.paused end
    cache.paused_at = now
    local ok, value = pcall(function() return UEHelpers.GetGameplayStatics():IsGamePaused(pc) end)
    if not ok and not cache.pause_warned then
        cache.pause_warned = true
        log("cannot read the game's pause (she stays out in its pause menu): %s", tostring(value))
    end
    cache.paused = ok and value == true
    return cache.paused
end

--- A line trace straight down at (x, y) from top to bottom (cm): the height it hits, or nil. The player's pawn is the
--- world context and is ignored. A game where the call fails says so once and is not traced again.
local tracer, tracer_pawn = nil, nil
local function trace_down(pawn)
    if tracer_pawn == pawn and tracer ~= nil then return tracer end
    tracer_pawn = pawn
    tracer = function(x, y, top, bottom)
        if trace_broken then return nil end
        local hit = {}
        local ok, did = pcall(function()
            return kismet:LineTraceSingle(pawn, { X = x, Y = y, Z = top }, { X = x, Y = y, Z = bottom }, M.TRACE_CHANNEL, false, { pawn }, 0, hit,
                true, NO_COLOR, NO_COLOR, 0.0)
        end)
        if not ok then
            trace_broken = true
            log("cannot trace the ground (she stands on the player's ground): %s", tostring(did))
            return nil
        end
        if did ~= true then return nil end
        local point = hit.ImpactPoint or hit.Location
        local z = point ~= nil and point.Z or nil
        return type(z) == "number" and z or nil
    end
    return tracer
end

--- The game's sun, searched again every SUN_FIND_EVERY seconds (UE4SS's FindAllOf walks every object: not each frame).
local function find_sun(now)
    if now - sun.find_at < M.SUN_FIND_EVERY then return sun.comp end
    sun.find_at = now
    local lights, found = {}, {}
    pcall(function()
        for _, c in ipairs(FindAllOf("DirectionalLightComponent") or {}) do
            if c:IsValid() then
                local l = { comp = c }
                pcall(function() l.name = c:GetFullName() end)
                pcall(function() l.intensity = c.Intensity end)
                pcall(function() l.affects = c.bAffectsWorld end)
                pcall(function() l.visible = c.bVisible end)
                pcall(function() l.atmosphere = c.bAtmosphereSunLight == true or c.bUsedAsAtmosphereSunLight == true end)
                lights[#lights + 1] = l
                found[#found + 1] = l
            end
        end
    end)
    local i = M.pick_sun(lights)
    sun.comp = i and lights[i].comp or nil
    local said = sun.comp and lights[i].name or "none"
    if said ~= sun.said then
        sun.said = said
        log("the game's sun: %s (%d directional lights)", said, #found)
    end
    return sun.comp
end

--- The light line for this moment (rate-limited), or "".
local function sun_line(now, pawn)
    if now - sun.sent_at < M.SUN_EVERY then return "" end
    sun.sent_at = now
    local comp = find_sun(now)
    if comp == nil or not comp:IsValid() then
        sun.comp = nil
        return ""
    end
    local ok, line = pcall(function()
        local rot = comp:K2_GetComponentRotation()
        local color = comp.LightColor or { R = 255, G = 255, B = 255 }
        local temperature = comp.bUseTemperature == true and comp.Temperature or nil
        -- does it reach her? a trace from her chest toward it (the player ignored); a blocker within SUN_REACH: shade
        local fx, fy, fz = M.rotator_axes(rot.Pitch, rot.Yaw, rot.Roll)
        if not trace_broken then
            local from = { X = her.x, Y = her.y, Z = her.z + M.SUN_CHEST }
            local to = { X = from.X - fx * M.SUN_REACH, Y = from.Y - fy * M.SUN_REACH, Z = from.Z - fz * M.SUN_REACH }
            local hit = {}
            local tok, blocked = pcall(function()
                return kismet:LineTraceSingle(pawn, from, to, M.TRACE_CHANNEL, false, { pawn }, 0, hit, true, NO_COLOR, NO_COLOR, 0.0)
            end)
            if tok then sun.visible = blocked == true and 0 or 1 end
        end
        return M.light_line(rot, color, temperature, comp.Intensity, sun.visible)
    end)
    if not ok then
        if not sun.warned then
            sun.warned = true
            log("cannot read the game's sun: %s", tostring(line))
        end
        sun.comp = nil
        return ""
    end
    return line
end

local function tick()
    local t0 = os.clock()
    local now = t0
    if not kismet or not kismet:IsValid() then kismet = UEHelpers.GetKismetSystemLibrary() end
    open_pipe(now)
    -- one camera per engine frame (GFrameCounter; an engine without GetFrameCount: ours, one per tick)
    if frame_counter == nil then
        frame_counter = pcall(function() return kismet:GetFrameCount() end)
        log("frame counter: %s", frame_counter and "the engine's" or "AstraUE's own")
    end
    own_frame = own_frame + 1
    local frame = frame_counter and kismet:GetFrameCount() or own_frame
    if frame == last_frame then return end
    last_frame = frame
    -- no add-on (ReShade not there yet, or not installed): read and log the camera now and then, send nothing
    if not pipe and own_frame % 60 ~= 0 then return end
    if not valid(pc) and now >= next_find then
        next_find = now + 1
        pc = UEHelpers.GetPlayerController()
        if valid(pc) then log("player controller: %s", pc:GetFullName()) end
    end
    if not valid(pc) then send("V 0\n") return end
    local pcm, pawn = pc.PlayerCameraManager, pc.Pawn
    if not valid(pcm) or not valid(pawn) then send("V 0\n") return end
    refresh_cache(now, pcm, pawn)
    if paused(now) then send("V 0\n") return end

    local loc, rot, fov = pcm:GetCameraLocation(), pcm:GetCameraRotation(), pcm:GetFOVAngle()
    local at, prot, vel = pawn:K2_GetActorLocation(), pawn:K2_GetActorRotation(), pawn:GetVelocity()
    local falling = false
    pcall(function() falling = pawn.CharacterMovement:IsFalling() end)
    local dt = last_clock and math.min(math.max(now - last_clock, 0), 0.1) or 0
    last_clock = now
    M.follow(her, { x = at.X, y = at.Y, z = at.Z - (cache.half_height or 0), yaw = prot.Yaw, vx = vel.X, vy = vel.Y, falling = falling }, dt, now,
        trace_down(pawn))
    -- her placement first, the camera last (the engine draws when the camera comes)
    if send("V 1\n" .. sun_line(now, pawn) .. M.avatar_line(her) .. M.camera_line(frame, loc, rot, fov, cache.horizontal, cache.aspect, cache.near)) then
        stats.sent = stats.sent + 1
    end
    stats.frames = stats.frames + 1
    stats.cost = stats.cost + (os.clock() - t0)
    if now - stats.logged_at >= 10 then
        stats.logged_at = now
        local t = her.trace or {}
        log("frame %d  camera %.0f %.0f %.0f  pitch %.1f yaw %.1f roll %.1f  fov %.1f%s  player %.0f %.0f %.0f%s  her %.0f %.0f %.0f  ground %s (%d traces, %d missed)  %.3f ms/frame",
            frame, loc.X, loc.Y, loc.Z, rot.Pitch, rot.Yaw, rot.Roll, fov, cache.horizontal and "h" or "v", at.X, at.Y, at.Z,
            falling and " (falling)" or "", her.x, her.y, her.z, t.z ~= nil and string.format("%.0f", t.z) or "the player's", t.traces or 0,
            t.misses or 0, stats.frames > 0 and stats.cost * 1000 / stats.frames or 0)
        stats.frames, stats.cost = 0, 0
    end
end

local function safe_tick()
    local ok, err = pcall(tick)
    if not ok then
        stats.errors = stats.errors + 1
        if stats.errors <= 5 or stats.errors % 1000 == 0 then log("error (%d): %s", stats.errors, tostring(err)) end
        pc = nil -- found again next second (a level change destroys it)
    end
end

if EngineTickAvailable then
    LoopInGameThreadAfterFrames(1, safe_tick)
    log("started: every engine frame (EngineTick)")
else
    LoopInGameThreadWithDelay(4, safe_tick) -- deduplicated by the frame counter
    log("started: EngineTick hook unavailable, polling every 4 ms on the game thread (ProcessEvent)")
end
