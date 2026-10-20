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

-- the follow brain's numbers (UE centimetres, seconds). BACK/SIDE/NEAR_ENOUGH/STAY_RADIUS are tuned for
-- M.REF_HALF_HEIGHT's capsule; M.follow scales them by the real player's own half_height when given one
-- (a smaller/shorter pawn, e.g. Poppy Playtime's ~138 cm eye height vs a standard ~160-165 cm Mannequin,
-- gets a proportionally closer spot - today's numbers are unchanged for a reference-sized capsule).
M.BACK, M.SIDE = 140.0, 90.0    -- her spot: behind and to the right of the player
M.NEAR_ENOUGH = 35.0            -- she stops this close to her spot
M.STAY_RADIUS = 300.0           -- while the player stands, she stays put if she is this near him (further:
                                 -- she comes back along her own bearing to her spot's distance from him)
M.NEAR_PLAYER = 90.0            -- while he stands, nearer him than this she steps away, along her own bearing,
                                 -- to her spot's distance (the Death Stranding follower's kStayNear, 0.9 m)
M.TELEPORT = 2500.0             -- further than this from her spot (or a floor apart): she is put there
M.FLOOR_APART = 300.0
M.REF_HALF_HEIGHT = 88.0        -- a standard UE ThirdPersonCharacter capsule half-height (cm): BACK/SIDE/
                                 -- NEAR_ENOUGH/STAY_RADIUS above are tuned for this
M.WALK_PACE = 70.0              -- her slowest catch-up pace once she is actually moving (cm/s) - matched to
                                 -- the built-in pack's measured no-slide walk speed (nayu-anims.toml's walk
                                 -- blend anchor 0.7 m/s): keep the two in step if either changes. Only
                                 -- applied once a catch-up is already wanted (d > NEAR_ENOUGH), never as an
                                 -- unconditional floor - that read as "runs at the slightest movement" (the
                                 -- old M.WALK_MIN=150 fired on ANY >35 cm gap, 2-3x the owner's own measured
                                 -- 51-70 cm/s slow walk in Poppy Playtime, UE4SS.log 2026-10-20).
M.RUN_MAX = 950.0
M.GO_START = 15.0               -- she starts tracking (stops "staying put") once the player is going at
                                 -- least this fast (cm/s): any real movement, a crouch or a half-pushed stick
                                 -- included - a creep below this edge would never count as going, and she
                                 -- would stand, then rush to a latched aim once he is STAY_RADIUS away (review
                                 -- of 0.2.5: a 30 cm/s creep froze-and-burst at an edge of 40). 15 cm/s is
                                 -- also the engine pack's own walk entry (nayu-anims.toml `speed > 0.15`).
M.GO_STOP = 8.0                 -- ...and only goes back to "staying put" once he is slower than this - a
                                 -- hysteresis gap, not one bare threshold (M.STILL used to be both edges at
                                 -- once: a jitter near it could flip the stand/catch-up branch every frame);
                                 -- the pack's walk exit is `speed < 0.08`.
M.CALM_PACE = 120.0             -- her catch-up never runs ahead of a slow player: at most this, or 1.5x his
                                 -- pace once he is faster (a slow first step after looking around - her spot
                                 -- swung to his other side - must not send her running there)
M.ACCEL, M.BRAKE = 700.0, 1000.0 -- her catch-up ramp (cm/s^2, linear - not an exponential filter): the
                                 -- Death Stranding follower's own already-live values
                                 -- (astra-in-death-stranding/src/astra.cpp:20, kAccel/kBrake)

-- the ground under her (UE centimetres, seconds)
M.TRACE_MIN, M.TRACE_MAX = 0.05, 0.25 -- never more often than this; at least this often
M.TRACE_MOVE = 15.0             -- sooner than TRACE_MAX once she moved this far
M.TRACE_UP, M.TRACE_DOWN = 100.0, 300.0 -- from this far above the higher of her and the player's feet, to this far below the lower
M.STEP_UP = 50.0                -- ground higher than this above her feet is hers only with no ceiling between (a ledge, not a balcony)
M.GROUND_KEEP = 1.0             -- a trace that misses keeps the last ground this long, then the player's
M.TRACE_CHANNEL = 0             -- ETraceTypeQuery: 0 = TraceTypeQuery1, the Visibility channel in every UE game

-- the game's sun
M.SUN_FIND_EVERY = 5.0          -- seconds before searching again when no sun was found, doubling while none is (a level
M.SUN_FIND_MAX = 60.0           -- with no directional light), up to this; a new level starts over. A sun found is kept
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

--- The projection the game really draws with, measured instead of read from its settings: GetFOVAngle's number is
--- vertical or horizontal depending on an aspect-axis setting that Unreal versions keep moving (the local player's,
--- a camera's own override, a project default), and a misread draws her ~0.56x too small or 1.8x too big at 16:9
--- (the owner, 2026-10-20: "in Mecha Chameleon she is so small" - UE 5.6, logged "fov 90.0v"). So: where does the
--- game put a point PROBE_D ahead on the view axis, and one PROBE_H above it? The first lands on the view's centre
--- (cx, cy: half its size in pixels), the second dy pixels higher, and tan(half the vertical fov) = (H/D) * cy / dy -
--- exact for a pinhole, whatever the pixel scale, roll or aspect. project(x, y, z) -> screen x, y (pixels, the
--- player's own view) or nil. Returns tan(half vfov), or nil when the answer is no use (an off-screen or degenerate
--- projection, a fov outside 1..170 degrees).
M.PROBE_D, M.PROBE_H = 1000.0, 100.0 -- UE cm: 10 m ahead, 1 m above the axis (about 6 degrees: on screen in any game)
function M.projected_tan_v(project, x, y, z, fx, fy, fz, ux, uy, uz)
    local ax, ay, az = x + fx * M.PROBE_D, y + fy * M.PROBE_D, z + fz * M.PROBE_D
    local cx, cy = project(ax, ay, az)
    if cx == nil or cy == nil then return nil end
    local _, ty = project(ax + ux * M.PROBE_H, ay + uy * M.PROBE_H, az + uz * M.PROBE_H)
    if ty == nil then return nil end
    local dy = cy - ty -- screen y grows downward: the point above the axis is drawn higher
    if cy < 2 or dy < 0.25 then return nil end
    local t = (M.PROBE_H / M.PROBE_D) * cy / dy
    if t < 0.0087 or t > 11.43 then return nil end -- 1..170 degrees
    return t
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

--- Her ground: under her (`ground(x, y, from, to)` -> the height of the first surface a line trace from `from` to `to`
--- hits, down or up, or nil), rate-limited; a miss keeps the last one GROUND_KEEP seconds; with no trace, the player's
--- ground while he stands on it. The trace starts above the higher of her and the player's feet (she steps up onto the
--- ledge he climbed); a surface more than STEP_UP above her own feet is hers only if nothing is between her and it - a
--- balcony the player stands on over her head is a ceiling to her, and she stays on her floor.
function M.update_ground(s, p, now, ground, force)
    s.trace = s.trace or {}
    local g = s.trace
    if ground ~= nil and now ~= nil and (force or M.should_trace(g, now, s.x, s.y)) then
        local bottom = math.min(s.z, p.z) - M.TRACE_DOWN
        local z = ground(s.x, s.y, math.max(s.z, p.z) + M.TRACE_UP, bottom)
        if z ~= nil and not force and z > s.z + M.STEP_UP and ground(s.x, s.y, s.z + M.STEP_UP * 0.5, z - 1) ~= nil then
            z = ground(s.x, s.y, s.z + M.STEP_UP, bottom) -- under a ceiling: her own floor
            g.ceilings = (g.ceilings or 0) + 1
        end
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

--- Her desired catch-up pace (D) for this frame, cm/s: track the player's speed and close the distance to
--- her spot, floored at her real walk pace (C, D) only once a catch-up is actually wanted (d > near_enough)
--- - never unconditionally (the old M.WALK_MIN fired on ANY >35 cm gap). The max() shape (not a SUM of the
--- two terms) keeps today's RUN_MAX saturation point unchanged at speed: a sum saturates sooner (skeptic 2,
--- confirmed: pspeed 400-600 cm/s with d 200-500 cm saturates RUN_MAX with TRACK=0.9/CLOSE=1.4; this form
--- does not).
function M.catchup_want(pspeed, d, near_enough)
    if d <= near_enough then return 0 end
    local want = math.min(math.max(pspeed * 1.1, d * 1.5), M.RUN_MAX)
    return math.max(want, M.WALK_PACE)
end

--- A linear accel/brake ramp (B) from `v` toward `want` (cm/s) - not an exponential filter: its settle time
--- scales with how far `want` actually changed, unlike a fixed-time-constant filter's roughly constant
--- ~3*tau settle time regardless of the target (the owner's measured ~500 ms camera-turn-adjacent lag).
function M.accel_brake(v, want, dt)
    if v < want then return math.min(want, v + M.ACCEL * dt) end
    return math.max(want, v - M.BRAKE * dt)
end

--- One step of her walking. s: her state (UE cm); p: the player {x, y, z (feet), yaw, vx, vy, falling,
--- half_height (optional: the pawn's capsule half-height, for scale - F)}; dt seconds; now: a clock
--- (seconds) and ground: a line trace down (both optional: without them she stands on the player's ground).
function M.follow(s, p, dt, now, ground)
    -- scale-aware distances (F): a smaller/taller capsule than the reference moves her spot and comfort
    -- radii proportionally; no half_height given (or 0): today's fixed numbers, unchanged
    local dscale = (p.half_height and p.half_height > 0) and (p.half_height / M.REF_HALF_HEIGHT) or 1.0
    local back, side, near_enough, stay_radius = M.BACK * dscale, M.SIDE * dscale, M.NEAR_ENOUGH * dscale, M.STAY_RADIUS * dscale
    local yr = math.rad(p.yaw)
    local fx, fy = math.cos(yr), math.sin(yr)
    local rx, ry = -fy, fx -- UE: +Y is to the right of +X
    local tx, ty = p.x - fx * back + rx * side, p.y - fy * back + ry * side
    if s.placed then M.update_ground(s, p, now, ground) end
    local pspeed = math.sqrt(p.vx * p.vx + p.vy * p.vy)
    local to_spot = math.sqrt((tx - s.x) ^ 2 + (ty - s.y) ^ 2)
    if not s.placed or to_spot > M.TELEPORT or math.abs((s.ground or p.z) - s.z) > M.FLOOR_APART then
        -- put at her spot, on the ground there (traced at once)
        s.x, s.y, s.z, s.yaw = tx, ty, p.z, p.yaw
        if s.trace then s.trace.z = nil end
        M.update_ground(s, p, now, ground, true)
        s.z = s.ground or p.z
        s.v, s.vx, s.vy, s.speed, s.placed, s.teleports, s.going = 0, 0, 0, 0, true, (s.teleports or 0) + 1, false
        s.aim_x, s.aim_y = nil, nil
        return s
    end
    -- stand/go (E): she tracks the player once he is really GOING; a hysteresis gap (GO_START to start,
    -- GO_STOP to stop), not one bare threshold, so a tiny speed jitter near it can't flip the stand/catch-up
    -- branch every frame.
    if pspeed >= M.GO_START then
        s.going = true
    elseif pspeed < M.GO_STOP then
        s.going = false
    end
    local to_player = math.sqrt((p.x - s.x) ^ 2 + (p.y - s.y) ^ 2)
    local goal_x, goal_y = tx, ty
    if not s.going then
        -- he stands, or only turns his view: her spot turns with his yaw, so she never chases it (a first-person
        -- game turns the pawn with the camera). She stays where she is, and only when she is too near him or
        -- too far from him does she walk, along her OWN bearing from him, to her spot's distance - so turning
        -- the camera never moves her. That walk is latched (s.aim) until she gets there, so she does not stop
        -- the moment she crosses the edge and start again at his next sway.
        if s.aim_x == nil and (to_player < M.NEAR_PLAYER * dscale or to_player > stay_radius) then
            local r = math.sqrt(back * back + side * side)
            local ux, uy = -fx, -fy -- right on top of him: behind him
            if to_player > 1e-3 then ux, uy = (s.x - p.x) / to_player, (s.y - p.y) / to_player end
            s.aim_x, s.aim_y = p.x + ux * r, p.y + uy * r
        end
        if s.aim_x ~= nil then goal_x, goal_y = s.aim_x, s.aim_y else goal_x, goal_y = s.x, s.y end
    else
        s.aim_x, s.aim_y = nil, nil
    end
    local dx, dy = goal_x - s.x, goal_y - s.y
    local d = math.sqrt(dx * dx + dy * dy)
    if s.aim_x ~= nil and d <= near_enough then s.aim_x, s.aim_y = nil, nil end -- there
    local want
    if s.going and d <= near_enough then
        want = math.min(pspeed, M.RUN_MAX) -- at her spot while he walks: keep his pace, never stop-and-go
    else
        want = math.min(M.catchup_want(pspeed, d, near_enough), math.max(pspeed * 1.5, M.CALM_PACE))
    end
    s.v = M.accel_brake(s.v or 0, want, dt)
    local step = math.min(s.v * dt, d)
    local mx, my = 0, 0 -- this frame's move
    if d > 1e-3 and step > 0 then
        -- on her way she keeps clear of him (the Death Stranding follower's push, twice as strong: a
        -- first-person player walks INTO her) - going from in front of him to her spot behind him she walks
        -- round him, not through him
        local ux, uy = dx / d, dy / d
        local clear = M.NEAR_PLAYER * dscale
        if to_player < clear and to_player > 1e-3 then
            local push = (1 - to_player / clear) * 3.0
            ux, uy = ux - (p.x - s.x) / to_player * push, uy - (p.y - s.y) / to_player * push
            local l = math.sqrt(ux * ux + uy * uy)
            if l > 1e-3 then ux, uy = ux / l, uy / l else ux, uy = dx / d, dy / d end
        end
        mx, my = ux * step, uy * step
        s.x, s.y = s.x + mx, s.y + my
        if step > 0.3 then s.yaw = M.turn(s.yaw, math.deg(math.atan(uy, ux)), 540 * dt) end
    elseif not s.going then
        s.yaw = M.turn(s.yaw, p.yaw, 90 * dt) -- standing: turn slowly the way he looks
    end
    s.z = s.z + ((s.ground or p.z) - s.z) * math.min(1, dt * 8)
    local inv = dt > 0 and 1 / dt or 0
    s.vx, s.vy = mx * inv, my * inv
    -- her pace in m/s (C): the SAME quantity that just moved her (the realized step/dt, already clamped by
    -- d) - never a second, independently-filtered copy. The old code low-passed a separate `pace` signal at
    -- its own tau and snapped it to 0 below 0.05, which (decaying faster than s.v/her position) could leave
    -- her still translating a frame after the engine had already read her as idle ("slides with no walk
    -- animation").
    s.speed = math.sqrt(s.vx * s.vx + s.vy * s.vy) * 0.01
    s.airborne = false
    return s
end

---------------------------------------------------------------------------------------------------- the game

if ASTRAUE_TEST then return M end -- tests/astraue_tests.lua loads the maths only

local UEHelpers = require("UEHelpers")

local pipe, next_open = nil, 0
local pc, next_find = nil, 0
local cache = { at = -1, paused_at = -1, project_fails = 0 }
local her = { placed = false, x = 0, y = 0, z = 0, yaw = 0, v = 0, vx = 0, vy = 0, speed = 0 }
local last_frame, last_clock, own_frame, frame_counter = -1, nil, 0, nil
local stats = { frames = 0, sent = 0, cost = 0, logged_at = 0, errors = 0 }
local kismet = nil
local NO_COLOR = { R = 0, G = 0, B = 0, A = 0 }
local trace_broken = false
local sun = { comp = nil, find_at = -1e9, sent_at = -1e9, said = nil, visible = 1 }
local last_cam_pos = nil -- her camera-only fallback's velocity (no actor at all: GTA DE's Gameface, see player_actor)

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
    -- the game's engine, a raw fact: the add-on draws her before an Unreal game's tonemapper (its grade and UI over her)
    send(string.format("I %s %s\nE unreal\nM AstraUE connected (UE %s)\n", M.INTEGRATION, name, version))
end

local function valid(o)
    return o ~= nil and o:IsValid()
end

--- An object's class + path (UE4SS's GetFullName), defensively - for the diagnostics below. "none" for
--- nil/invalid, "?" when the read itself fails (an engine too old/new for GetFullName on this class).
local function class_name(o)
    if not valid(o) then return "none" end
    local ok, name = pcall(function() return o:GetFullName() end)
    return ok and name or "?"
end

--- Logs WHY no camera is being sent, at most once per 10 s: what the owner's next run needs to show us.
local no_cam = { logged_at = -1e9 }
local function log_no_camera(now, reason, pawn, vt)
    if now - no_cam.logged_at < 10 then return end
    no_cam.logged_at = now
    log("no camera sent (%s): pc.Pawn %s  view target %s", reason, class_name(pawn), class_name(vt))
end

--- The actor to read her follow-target from: the Pawn if valid; else the controller's view target (GTA DE's
--- Gameface, and any game whose player logic never possesses a UE Pawn, still points the camera somewhere -
--- its target is the closest thing to "where the player is"), when that is a valid actor other than the
--- camera manager itself. nil: no actor at all - she is then placed relative to the camera alone (below).
--- The second return is the view target found (or nil), for the diagnostics above - read only when there is
--- no Pawn, so a game that does possess one never pays for the extra calls.
local function player_actor(pc, pcm, pawn)
    if valid(pawn) then return pawn, nil end
    local ok, vt = pcall(function() return pc:GetViewTarget() end)
    if not (ok and valid(vt)) then ok, vt = pcall(function() return pcm.ViewTarget.Target end) end
    vt = (ok and valid(vt)) and vt or nil
    if vt ~= nil and vt ~= pcm then return vt, vt end
    return nil, vt
end

--- An actor's location, rotation, velocity: K2_GetActorLocation/K2_GetActorRotation/GetVelocity are
--- blueprint-callable on every AActor, but defensively - a view target standing in for a missing Pawn may be
--- a class that does not answer one of them the way a Character does. nil: the actor stopped answering even
--- its location (gone mid-frame); the caller falls through to the camera-only placement.
local function actor_state(actor)
    local ok, at = pcall(function() return actor:K2_GetActorLocation() end)
    if not (ok and at) then return nil end
    local rok, prot = pcall(function() return actor:K2_GetActorRotation() end)
    local vok, vel = pcall(function() return actor:GetVelocity() end)
    local falling = false
    pcall(function() falling = actor.CharacterMovement:IsFalling() end)
    return at, (rok and prot) or { Pitch = 0, Yaw = 0, Roll = 0 }, (vok and vel) or { X = 0, Y = 0, Z = 0 }, falling
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

--- The game's sun. UE4SS's FindAllOf walks every object (a hitch), so it is searched for only when there is none (then
--- after SUN_FIND_EVERY seconds, doubling up to SUN_FIND_MAX while there still is none), when the one found is gone (a
--- level change destroys it) or the player controller changed (a new level: the wait starts over) - never while the
--- one found is valid.
local function find_sun(now)
    local alive = false
    if sun.comp ~= nil then
        local ok, v = pcall(function() return sun.comp:IsValid() end)
        alive = ok and v == true
    end
    if alive and sun.world == pc then return sun.comp end
    if sun.world ~= pc then sun.wait = nil end
    if not alive and sun.comp == nil and sun.world == pc and now - sun.find_at < (sun.wait or M.SUN_FIND_EVERY) then return nil end
    sun.find_at, sun.world = now, pc
    local lights, found = {}, {}
    pcall(function()
        for _, c in ipairs(FindAllOf("DirectionalLightComponent") or {}) do
            if c:IsValid() then
                local l = { comp = c }
                pcall(function() l.name = c:GetFullName() end)
                pcall(function() l.intensity = c.Intensity end)
                pcall(function() l.affects = c.bAffectsWorld end)
                pcall(function() l.visible = c.bVisible end)
                -- UE5's name, then UE4's: a version lacking one throws on it, so each is read on its own
                pcall(function() l.atmosphere = c.bAtmosphereSunLight == true end)
                if not l.atmosphere then pcall(function() l.atmosphere = c.bUsedAsAtmosphereSunLight == true end) end
                lights[#lights + 1] = l
                found[#found + 1] = l
            end
        end
    end)
    local i = M.pick_sun(lights)
    sun.comp = i and lights[i].comp or nil
    sun.wait = sun.comp == nil and math.min((sun.wait or M.SUN_FIND_EVERY / 2) * 2, M.SUN_FIND_MAX) or nil
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

--- The game's own tan(half vertical fov) this frame: M.projected_tan_v through APlayerController::
--- ProjectWorldLocationToScreen (player-viewport-relative pixels), or nil. A nil answer (a loading screen, no
--- viewport yet) is not a failure; a call that throws 30 frames running (a game without the function) is not
--- made again, said once - she keeps the fov from its settings there.
local function measured_tan_v(pc, loc, rot)
    if cache.project_off then return nil end
    local fx, fy, fz, ux, uy, uz = M.rotator_axes(rot.Pitch, rot.Yaw, rot.Roll)
    local failed = false
    local function project(x, y, z)
        local screen = {}
        local ok, on = pcall(function() return pc:ProjectWorldLocationToScreen({ X = x, Y = y, Z = z }, screen, true) end)
        if not ok then
            failed, cache.project_error = true, on
            return nil
        end
        if on ~= true or type(screen.X) ~= "number" or type(screen.Y) ~= "number" then return nil end
        return screen.X, screen.Y
    end
    local t = M.projected_tan_v(project, loc.X, loc.Y, loc.Z, fx, fy, fz, ux, uy, uz)
    if not failed then
        cache.project_fails = 0
        return t
    end
    cache.project_fails = cache.project_fails + 1
    if cache.project_fails >= 30 then
        cache.project_off = true
        log("cannot ask the game where it draws (the fov comes from its settings): %s", tostring(cache.project_error))
    end
    return nil
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
    if not valid(pc) then
        log_no_camera(now, "no pc", nil, nil)
        send("V 0\n")
        return
    end
    local pcm = pc.PlayerCameraManager
    if not valid(pcm) then
        log_no_camera(now, "no pcm", pc.Pawn, nil)
        send("V 0\n")
        return
    end
    if not cache.said_pcm then
        cache.said_pcm = true
        log("camera manager: %s", class_name(pcm))
    end
    local pawn = pc.Pawn
    -- her follow-target: the Pawn, else the view target (GTA DE's Gameface never possesses one) - never a
    -- reason to withhold the camera: a camera manager alone is enough to keep her on screen
    local actor, vt = player_actor(pc, pcm, pawn)
    refresh_cache(now, pcm, pawn)
    if paused(now) then
        log_no_camera(now, "paused", pawn, vt)
        send("V 0\n")
        return
    end

    local loc, rot, fov = pcm:GetCameraLocation(), pcm:GetCameraRotation(), pcm:GetFOVAngle()
    -- the fov sent: the game's own projection, measured each frame (M.projected_tan_v); its settings only when it
    -- cannot answer (then GetFOVAngle on the axis its aspect-axis setting names, as before)
    local send_fov, send_horizontal = fov, cache.horizontal
    local tan_v = measured_tan_v(pc, loc, rot)
    if tan_v ~= nil then
        send_fov, send_horizontal = math.deg(2 * math.atan(tan_v)), false
        if not cache.said_projection then
            cache.said_projection = true
            log("the game draws a %.1f degree vertical fov (GetFOVAngle %.1f, its aspect-axis setting reads %s)", send_fov, fov,
                cache.horizontal and "horizontal" or "vertical")
        end
    end
    local dt = last_clock and math.min(math.max(now - last_clock, 0), 0.1) or 0
    last_clock = now

    local source, p, tracer_actor
    if actor ~= nil then
        local at, prot, vel, falling = actor_state(actor)
        if at ~= nil then
            p = { x = at.X, y = at.Y, z = at.Z - (cache.half_height or 0), yaw = prot.Yaw, vx = vel.X, vy = vel.Y, falling = falling,
                half_height = cache.half_height }
            source, tracer_actor = (actor == pawn and "pawn" or "view target"), actor
        else
            actor = nil -- stopped answering mid-frame (gone): fall through to the camera-only placement
        end
    end
    if actor == nil then
        -- no actor for the player at all: place her relative to the camera - her spot (M.follow's BACK/SIDE,
        -- the same fixed distance a Pawn gets) is now behind-and-beside the camera, ground traced from it.
        -- Her velocity: the camera's own frame-to-frame movement (there is no actor to read one from).
        local vx, vy = 0, 0
        if last_cam_pos and dt > 0 then vx, vy = (loc.X - last_cam_pos.X) / dt, (loc.Y - last_cam_pos.Y) / dt end
        last_cam_pos = { X = loc.X, Y = loc.Y }
        p = { x = loc.X, y = loc.Y, z = loc.Z, yaw = rot.Yaw, vx = vx, vy = vy, falling = false }
        source, tracer_actor = "camera", pcm
    end
    M.follow(her, p, dt, now, trace_down(tracer_actor))
    -- her placement first, the camera last (the engine draws when the camera comes)
    if send("V 1\n" .. sun_line(now, tracer_actor) .. M.avatar_line(her) .. M.camera_line(frame, loc, rot, send_fov, send_horizontal, cache.aspect, cache.near)) then
        stats.sent = stats.sent + 1
    end
    stats.frames = stats.frames + 1
    stats.cost = stats.cost + (os.clock() - t0)
    if now - stats.logged_at >= 10 then
        stats.logged_at = now
        local t = her.trace or {}
        log("frame %d  camera %.0f %.0f %.0f  pitch %.1f yaw %.1f roll %.1f  fov %.1f%s, drawn %s  %s %.0f %.0f %.0f%s  her %.0f %.0f %.0f  ground %s (%d traces, %d missed)  %.3f ms/frame",
            frame, loc.X, loc.Y, loc.Z, rot.Pitch, rot.Yaw, rot.Roll, fov, cache.horizontal and "h" or "v",
            tan_v ~= nil and string.format("%.1fv", send_fov) or "not measured", source, p.x, p.y, p.z,
            p.falling and " (falling)" or "", her.x, her.y, her.z, t.z ~= nil and string.format("%.0f", t.z) or "the player's", t.traces or 0,
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
