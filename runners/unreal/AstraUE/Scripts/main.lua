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
-- Phase 1: no traces yet - her height is the player's last grounded height (stairs and slopes come with the
-- follow brain's line traces).

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

---------------------------------------------------------------------------------------------------- follow brain

--- One step of her walking. s: her state (UE cm); p: the player {x, y, z (feet), yaw, vx, vy, falling}; dt seconds.
function M.follow(s, p, dt)
    local yr = math.rad(p.yaw)
    local fx, fy = math.cos(yr), math.sin(yr)
    local rx, ry = -fy, fx -- UE: +Y is to the right of +X
    local tx, ty = p.x - fx * M.BACK + rx * M.SIDE, p.y - fy * M.BACK + ry * M.SIDE
    if not p.falling then s.ground = p.z end
    local pspeed = math.sqrt(p.vx * p.vx + p.vy * p.vy)
    local to_spot = math.sqrt((tx - s.x) ^ 2 + (ty - s.y) ^ 2)
    if not s.placed or to_spot > M.TELEPORT or math.abs((s.ground or p.z) - s.z) > M.FLOOR_APART then
        s.x, s.y, s.z, s.yaw = tx, ty, s.ground or p.z, p.yaw
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
    M.follow(her, { x = at.X, y = at.Y, z = at.Z - (cache.half_height or 0), yaw = prot.Yaw, vx = vel.X, vy = vel.Y, falling = falling }, dt)
    -- her placement first, the camera last (the engine draws when the camera comes)
    if send("V 1\n" .. M.avatar_line(her) .. M.camera_line(frame, loc, rot, fov, cache.horizontal, cache.aspect, cache.near)) then
        stats.sent = stats.sent + 1
    end
    stats.frames = stats.frames + 1
    stats.cost = stats.cost + (os.clock() - t0)
    if now - stats.logged_at >= 10 then
        stats.logged_at = now
        log("frame %d  camera %.0f %.0f %.0f  pitch %.1f yaw %.1f roll %.1f  fov %.1f%s  player %.0f %.0f %.0f%s  her %.0f %.0f %.0f  %.3f ms/frame",
            frame, loc.X, loc.Y, loc.Z, rot.Pitch, rot.Yaw, rot.Roll, fov, cache.horizontal and "h" or "v", at.X, at.Y, at.Z,
            falling and " (falling)" or "", her.x, her.y, her.z, stats.frames > 0 and stats.cost * 1000 / stats.frames or 0)
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
