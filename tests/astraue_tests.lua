-- Tests of the AstraUE UE4SS mod without a game: its maths, its follow brain, and a whole frame against a stub of
-- UE4SS's Lua API (a fake PlayerController, camera manager and pawn), checking the pipe lines it writes.
--   lua tests/astraue_tests.lua runners/unreal/AstraUE/Scripts/main.lua [lines-out.txt]
-- lines-out.txt (optional) receives every line written, for tests/core_tests to parse with the add-on's parser.
local path, lines_out = arg[1], arg[2]
local failed, checks = 0, 0
local function check(ok, what)
    checks = checks + 1
    if not ok then
        failed = failed + 1
        io.stdout:write("FAIL " .. what .. "\n")
    end
end
local function near(a, b, eps) return math.abs(a - b) <= (eps or 1e-4) end

-------------------------------------------------------------------------------- the maths
ASTRAUE_TEST = true
local M = dofile(path)
ASTRAUE_TEST = nil

do
    local x, y, z = M.to_gl(100, 200, 300)
    check(near(x, 1) and near(y, 3) and near(z, 2), "to_gl swaps y/z and scales cm -> m")
    local fx, fy, fz, ux, uy, uz = M.rotator_axes(0, 0, 0)
    check(near(fx, 1) and near(fy, 0) and near(fz, 0) and near(ux, 0) and near(uy, 0) and near(uz, 1), "identity rotator: forward +X, up +Z")
    fx, fy, fz = M.rotator_axes(0, 90, 0)
    check(near(fx, 0) and near(fy, 1) and near(fz, 0), "yaw 90 looks along +Y (to the right)")
    fx, fy, fz, ux, uy, uz = M.rotator_axes(90, 0, 0)
    check(near(fx, 0) and near(fz, 1) and near(ux, -1) and near(uz, 0), "pitch 90 looks up, up turns to -X")
    fx, fy, fz, ux, uy, uz = M.rotator_axes(0, 0, 90)
    check(near(ux, 0) and near(uy, 1) and near(uz, 0), "roll 90 tilts up to +Y")
    -- a right-handed glTF basis: right = forward x up must be UE's +Y (the camera's right) mapped to glTF
    fx, fy, fz, ux, uy, uz = M.rotator_axes(10, 30, 5)
    local gfx, gfy, gfz = M.dir_to_gl(fx, fy, fz)
    local gux, guy, guz = M.dir_to_gl(ux, uy, uz)
    local rx, ry, rz = gfy * guz - gfz * guy, gfz * gux - gfx * guz, gfx * guy - gfy * gux
    -- UE's right axis (FRotationMatrix Y) for the same rotator
    local p, yw, r = math.rad(10), math.rad(30), math.rad(5)
    local SP, CP, SY, CY, SR, CR = math.sin(p), math.cos(p), math.sin(yw), math.cos(yw), math.sin(r), math.cos(r)
    local ex, ey, ez = M.dir_to_gl(SR * SP * CY - CR * SY, SR * SP * SY + CR * CY, -SR * CP)
    check(near(rx, ex) and near(ry, ey) and near(rz, ez), "glTF forward x up = the camera's right: no mirror")
    check(near(M.turn(170, -170, 5), 175) and near(M.turn(10, 20, 100), 20) and near(M.turn(-170, 170, 5), -175), "turn goes the short way")
end

do
    local line = M.camera_line(77, { X = 12345.6, Y = -200, Z = 150 }, { Pitch = -10, Yaw = 45, Roll = 0 }, 90, true, 0, 10)
    local f = {}
    for w in line:gmatch("%S+") do f[#f + 1] = w end
    check(#f == 15 and f[1] == "C" and f[2] == "77", "camera line: 15 fields, the frame")
    check(near(tonumber(f[3]), 123.456) and near(tonumber(f[4]), 1.5) and near(tonumber(f[5]), -2), "camera line: position in glTF metres")
    check(f[13] == "h" and near(tonumber(f[15]), 0.1), "camera line: horizontal fov, near 10 cm = 0.1 m")
    check(line:sub(-1) == "\n", "camera line ends the line")
end

-------------------------------------------------------------------------------- the follow brain
do
    local her = { placed = false, x = 0, y = 0, z = 0, yaw = 0 }
    local p = { x = 0, y = 0, z = 0, yaw = 0, vx = 0, vy = 0, falling = false }
    M.follow(her, p, 1 / 60)
    check(her.placed and near(her.x, -M.BACK) and near(her.y, M.SIDE), "she starts at her spot: behind and to the right")
    -- the player runs along +X at 4 m/s for 3 s
    local max_lag, speeds = 0, 0
    for i = 1, 180 do
        p.x, p.vx = p.x + 400 / 60, 400
        M.follow(her, p, 1 / 60)
        if i > 60 then
            local lag = math.sqrt((her.x - (p.x - M.BACK)) ^ 2 + (her.y - M.SIDE) ^ 2)
            if lag > max_lag then max_lag = lag end
            speeds = speeds + her.speed
        end
    end
    check(max_lag < 200, "running: she keeps within 2 m of her spot (" .. string.format("%.0f", max_lag) .. " cm)")
    check(near(speeds / 120, 4.0, 0.5), "running: her pace is the player's (" .. string.format("%.2f", speeds / 120) .. " m/s)")
    check(near(her.yaw, 0, 5), "running: she faces the way she runs")
    -- he stops: she settles, her pace goes to 0
    p.vx = 0
    for _ = 1, 120 do M.follow(her, p, 1 / 60) end
    check(her.speed == 0, "stopped: her pace is 0")
    -- he turns on the spot: she stays where she is
    local hx, hy = her.x, her.y
    for i = 1, 60 do
        p.yaw = i * 3
        M.follow(her, p, 1 / 60)
    end
    check(near(her.x, hx, 1) and near(her.y, hy, 1), "turning on the spot: she stays put")
    -- he falls off a ledge and lands 10 m below: she follows (put there, not floating)
    p.falling = true
    p.z = -500
    M.follow(her, p, 1 / 60)
    check(near(her.z, 0, 1), "while he falls she keeps her ground")
    p.falling = false
    p.z = -1000
    local before = her.teleports
    M.follow(her, p, 1 / 60)
    check(her.teleports == before + 1 and near(her.z, -1000), "a floor apart: she is put at her spot")
    -- far away (a fast travel): put there
    p.x = p.x + 100000
    M.follow(her, p, 1 / 60)
    check(near(her.x, p.x - M.BACK * math.cos(math.rad(p.yaw)) - M.SIDE * math.sin(math.rad(p.yaw)), 1), "fast travel: she is put at her spot")
end

-------------------------------------------------------------------------------- 0.2.5: a linear ramp, not a fixed-tau filter (B)
do
    -- the old exponential filter's settle time was ~3*tau (~500 ms) REGARDLESS of the target; a linear
    -- accel/brake ramp's settle time scales with how far `want` actually changed.
    local v, frames_to_95 = 0, nil
    for i = 1, 60 do
        v = M.accel_brake(v, M.WALK_PACE, 1 / 60)
        if frames_to_95 == nil and v >= M.WALK_PACE * 0.95 then frames_to_95 = i end
    end
    check(frames_to_95 ~= nil and frames_to_95 <= 9,
        string.format("accel ramp: reaches 95%% of her walk pace within 150 ms (took %s frames)", tostring(frames_to_95)))
    -- a bigger target settles in proportionally MORE time, not the same ~500 ms either way
    local v2, frames_big = 0, nil
    for i = 1, 60 do
        v2 = M.accel_brake(v2, 300, 1 / 60)
        if frames_big == nil and v2 >= 300 * 0.95 then frames_big = i end
    end
    check(frames_big ~= nil and frames_big > frames_to_95,
        "accel ramp: a bigger target takes longer to settle (scales with distance, not a fixed time)")
end

-------------------------------------------------------------------------------- 0.2.5: one speed, no second filter (C)
do
    -- the speed reported to the engine must be the SAME quantity that moves her: recompute her real
    -- per-frame displacement independently (from her.x/y before and after) and compare against her.speed,
    -- through an accel-then-abrupt-stop run - the scenario behind symptom 1 ("slides with no walk
    -- animation": the old code's separately filtered + 0.05-snapped `s.speed` could disagree with a
    -- position that was still moving, because it decayed at a different rate than `s.v`).
    local her = { placed = false, x = 0, y = 0, z = 0, yaw = 0 }
    local p = { x = 0, y = 0, z = 0, yaw = 0, vx = 0, vy = 0, falling = false }
    M.follow(her, p, 1 / 60)
    local worst = 0
    for i = 1, 120 do
        p.vx = i <= 60 and 90 or 0 -- 1 s walking at 90 cm/s, then the player stops dead
        p.x = p.x + p.vx / 60
        local px, py = her.x, her.y
        M.follow(her, p, 1 / 60)
        local real_pace = math.sqrt((her.x - px) ^ 2 + (her.y - py) ^ 2) * 60 * 0.01 -- m/s, from the actual displacement
        worst = math.max(worst, math.abs(her.speed - real_pace))
    end
    check(worst < 1e-4, string.format("one speed: her reported speed always equals her real displacement (worst diff %.4f m/s)", worst))
end

-------------------------------------------------------------------------------- 0.2.5: slow walk, no glide (B, C)
do
    -- the owner's own measured slow walk in Poppy Playtime, UE4SS.log 2026-10-20: 51.2 and 69.75 cm/s
    for _, pspeed in ipairs({ 50, 70 }) do
        local her = { placed = false, x = 0, y = 0, z = 0, yaw = 0 }
        local p = { x = 0, y = 0, z = 0, yaw = 0, vx = 0, vy = 0, falling = false }
        M.follow(her, p, 1 / 60) -- placed at her spot
        p.vx = pspeed
        local walked, low_run, max_low_run = false, 0, 0
        for _ = 1, 300 do -- 5 s: long enough to settle into steady pursuit
            p.x = p.x + pspeed / 60
            M.follow(her, p, 1 / 60)
            if her.speed > 0.15 then walked = true end
            if her.speed > 0 and her.speed < 0.15 then
                low_run = low_run + 1
                max_low_run = math.max(max_low_run, low_run)
            else
                low_run = 0
            end
        end
        check(walked, string.format("slow walk %d cm/s: she walks (speed > 0.15 m/s at some point)", pspeed))
        check(max_low_run <= 2,
            string.format("slow walk %d cm/s: never glides below walk pace for more than 2 frames (worst run %d)", pspeed, max_low_run))
    end
end

-------------------------------------------------------------------------------- 0.2.5: stop, then a slow walk (E)
do
    -- skeptic 1: a GO_START/STOP picked without checking it against the owner's own measured slow walk
    -- (51-70 cm/s) can FREEZE her behind a player who starts walking that slowly, since the stand/go gate
    -- never flips to "going" below GO_START - she would then only resume once to_player forces it past
    -- M.STAY_RADIUS. This must not happen: she tracks a 60 cm/s walk-off closely, never drifting far.
    local her = { placed = false, x = 0, y = 0, z = 0, yaw = 0 }
    local p = { x = 0, y = 0, z = 0, yaw = 0, vx = 0, vy = 0, falling = false }
    M.follow(her, p, 1 / 60)
    -- he runs a bit, then stands for 3 s: she catches up and settles, latching "not going"
    for _ = 1, 180 do
        p.x, p.vx = p.x + 200 / 60, 200
        M.follow(her, p, 1 / 60)
    end
    p.vx = 0
    for _ = 1, 180 do M.follow(her, p, 1 / 60) end
    check(her.going == false, "stop-then-slow-walk: standing settles her 'going' latch to false")
    -- now he walks off at 60 cm/s (within the owner's measured 51-70 cm/s range)
    p.vx = 60
    local max_to_player, went_going = 0, false
    for _ = 1, 180 do -- 3 s
        p.x = p.x + 60 / 60
        M.follow(her, p, 1 / 60)
        if her.going then went_going = true end
        local to_player = math.sqrt((p.x - her.x) ^ 2 + (p.y - her.y) ^ 2)
        max_to_player = math.max(max_to_player, to_player)
    end
    check(went_going, "stop-then-slow-walk: a 60 cm/s walk-off is recognized as 'going' (GO_START sits below it)")
    check(max_to_player < 250,
        "stop-then-slow-walk: she tracks a 60 cm/s walk-off, never drifting near STAY_RADIUS (" .. string.format("%.0f", max_to_player) .. " cm)")
end

-------------------------------------------------------------------------------- 0.2.5: high speed, no RUN_MAX saturation (D)
do
    -- skeptic 2: a SUM of the two pace-law terms (the design plan's rejected TRACK=0.9/CLOSE=1.4 form)
    -- saturates M.RUN_MAX at ordinary sprint+lag combinations; M.catchup_want's max() form must not.
    local pspeed, d = 600, 500
    local want = M.catchup_want(pspeed, d, M.NEAR_ENOUGH)
    check(want < M.RUN_MAX, string.format("pace law: pspeed %d d %d does not saturate RUN_MAX (want %.0f)", pspeed, d, want))
    for _, ps in ipairs({ 400, 450, 500, 550, 600 }) do
        for _, dd in ipairs({ 200, 275, 350, 425, 500 }) do
            check(M.catchup_want(ps, dd, M.NEAR_ENOUGH) < M.RUN_MAX,
                string.format("pace law: pspeed %d d %d stays under RUN_MAX", ps, dd))
        end
    end
end

-------------------------------------------------------------------------------- 0.2.5: what the owner saw in first-person games
-- Poppy Playtime / Mecha Chameleon, 2026-10-20: she ran at his slightest step, walked right up against him, and
-- trailed a camera turn by ~500 ms. In a first-person game the pawn turns with the camera, so her spot (behind
-- and to his right) swings round him whenever he looks around.
local function fresh_her() return { placed = false, x = 0, y = 0, z = 0, yaw = 0 } end
do
    -- a steady slow walk: she keeps his pace, no stop-and-go
    for _, pspeed in ipairs({ 51, 70 }) do
        local her, p = fresh_her(), { x = 0, y = 0, z = 0, yaw = 0, vx = pspeed, vy = 0, falling = false }
        M.follow(her, p, 1 / 60)
        local lo, hi = math.huge, 0
        for i = 1, 600 do
            p.x = p.x + pspeed / 60
            M.follow(her, p, 1 / 60)
            if i > 60 then lo, hi = math.min(lo, her.speed), math.max(hi, her.speed) end
        end
        check(lo > pspeed * 0.01 * 0.95 and hi < pspeed * 0.01 * 1.05,
            string.format("steady %d cm/s walk: she keeps his pace, no stop-and-go (%.2f..%.2f m/s)", pspeed, lo, hi))
    end
end
do
    -- he stands, turns round to look at her, then walks TOWARD her slowly (a jittery 36-66 cm/s): her spot is now
    -- behind him, so she has to pass him - at a walk, and round him, not through him
    local her, p = fresh_her(), { x = 0, y = 0, z = 0, yaw = 0, vx = 0, vy = 0, falling = false }
    M.follow(her, p, 1 / 60)
    for _ = 1, 60 do M.follow(her, p, 1 / 60) end
    local bx, by = her.x, her.y
    for _ = 1, 60 do
        p.yaw = p.yaw + 3
        M.follow(her, p, 1 / 60)
    end
    for _ = 1, 60 do M.follow(her, p, 1 / 60) end
    check(near(her.x, bx, 0.5) and near(her.y, by, 0.5), "he turns round: she does not chase her spot as it swings")
    local fastest, closest = 0, math.huge
    for i = 1, 480 do
        local v = 51 + 15 * math.sin(i * 0.7)
        p.vx, p.x = -v, p.x - v / 60
        M.follow(her, p, 1 / 60)
        fastest = math.max(fastest, her.speed)
        closest = math.min(closest, math.sqrt((p.x - her.x) ^ 2 + (p.y - her.y) ^ 2))
    end
    check(fastest <= M.CALM_PACE * 0.01 + 1e-6,
        string.format("a slow walk toward her: she walks to her spot, never runs (fastest %.2f m/s)", fastest))
    check(closest > 45, string.format("a slow walk toward her: she goes round him, not into him (closest %.0f cm)", closest))
end
do
    -- standing too near him (in front of his face): she steps away once, and a full turn of his view then moves her
    -- not at all
    local her, p = fresh_her(), { x = 0, y = 0, z = 0, yaw = 0, vx = 0, vy = 0, falling = false }
    M.follow(her, p, 1 / 60)
    her.x, her.y = 25, 0
    for _ = 1, 180 do M.follow(her, p, 1 / 60) end
    local d0 = math.sqrt(her.x ^ 2 + her.y ^ 2)
    check(d0 > M.NEAR_PLAYER and her.y < 1 and her.x > 0,
        string.format("too near him: she steps away along her own bearing (%.0f cm, at %.0f/%.0f)", d0, her.x, her.y))
    local cx, cy = her.x, her.y
    for _ = 1, 120 do
        p.yaw = p.yaw + 3
        M.follow(her, p, 1 / 60)
    end
    check(near(her.x, cx, 0.5) and near(her.y, cy, 0.5), "too near him, then a full camera turn: she stays where she stepped to")
    -- too far while he stands: she comes back along her bearing, and a turn then leaves her be
    her.x, her.y = 0, -400
    for _ = 1, 360 do M.follow(her, p, 1 / 60) end
    local d1 = math.sqrt(her.x ^ 2 + her.y ^ 2)
    check(d1 < M.STAY_RADIUS and near(her.x, 0, 1) and her.y < 0,
        string.format("too far while he stands: she comes back along her own bearing (%.0f cm)", d1))
    cx, cy = her.x, her.y
    for _ = 1, 120 do
        p.yaw = p.yaw - 3
        M.follow(her, p, 1 / 60)
    end
    check(near(her.x, cx, 0.5) and near(her.y, cy, 0.5), "back near him, then a camera turn: she stays")
end

do
    -- a creep (crouching, a half-pushed stick) is still going: she walks at his pace from the first steps, never
    -- standing and then rushing to catch up (review of 0.2.5: a 30 cm/s creep froze-and-burst at GO_START 40)
    for _, pspeed in ipairs({ 18, 25, 30, 38 }) do
        local her, p = fresh_her(), { x = 0, y = 0, z = 0, yaw = 0, vx = 0, vy = 0, falling = false }
        M.follow(her, p, 1 / 60)
        for _ = 1, 60 do M.follow(her, p, 1 / 60) end
        p.vx = pspeed
        local stood, fastest = 0, 0
        for i = 1, 1800 do
            p.x = p.x + pspeed / 60
            M.follow(her, p, 1 / 60)
            if i > 30 then
                if her.speed == 0 then stood = stood + 1 end
                fastest = math.max(fastest, her.speed)
            end
        end
        check(stood == 0 and fastest <= pspeed * 0.01 * 1.05,
            string.format("a %d cm/s creep: she walks at his pace throughout (stood %d frames, fastest %.2f m/s)", pspeed, stood, fastest))
    end
end

-------------------------------------------------------------------------------- 0.2.5: scale-aware distances (F)
do
    local ref_dist = math.sqrt(M.BACK ^ 2 + M.SIDE ^ 2)
    -- a capsule half the reference height: her spot is half the distance away
    local her = { placed = false, x = 0, y = 0, z = 0, yaw = 0 }
    local p = { x = 0, y = 0, z = 0, yaw = 0, vx = 0, vy = 0, falling = false, half_height = M.REF_HALF_HEIGHT * 0.5 }
    M.follow(her, p, 1 / 60)
    check(near(math.sqrt(her.x ^ 2 + her.y ^ 2), ref_dist * 0.5, 1),
        string.format("scale: a half-height capsule gets a half-distance spot (%.1f vs %.1f)", math.sqrt(her.x ^ 2 + her.y ^ 2), ref_dist * 0.5))
    -- a reference-sized capsule (88 cm, the stub pawn's own): today's exact distances, unchanged
    local her2 = { placed = false, x = 0, y = 0, z = 0, yaw = 0 }
    local p2 = { x = 0, y = 0, z = 0, yaw = 0, vx = 0, vy = 0, falling = false, half_height = M.REF_HALF_HEIGHT }
    M.follow(her2, p2, 1 / 60)
    check(near(her2.x, -M.BACK, 1) and near(her2.y, M.SIDE, 1), "scale: a reference 88 cm capsule keeps today's exact distances")
    -- no half_height at all (a game whose capsule read fails): also unchanged
    local her3 = { placed = false, x = 0, y = 0, z = 0, yaw = 0 }
    local p3 = { x = 0, y = 0, z = 0, yaw = 0, vx = 0, vy = 0, falling = false }
    M.follow(her3, p3, 1 / 60)
    check(near(her3.x, -M.BACK, 1) and near(her3.y, M.SIDE, 1), "scale: no half_height read: today's exact distances (a degrading game)")
end

-------------------------------------------------------------------------------- the ground under her
do
    -- the rate limit: at once, then not before TRACE_MIN, at TRACE_MAX at the latest, in between when she moved
    local g = {}
    check(M.should_trace(g, 10, 0, 0), "the first trace is at once")
    g.at, g.x, g.y = 10, 0, 0
    check(not M.should_trace(g, 10 + M.TRACE_MIN / 2, 500, 0), "never twice within TRACE_MIN, however far she went")
    check(not M.should_trace(g, 10 + M.TRACE_MIN * 1.5, M.TRACE_MOVE / 2, 0), "standing nearly still: not yet")
    check(M.should_trace(g, 10 + M.TRACE_MIN * 1.5, M.TRACE_MOVE * 1.5, 0), "moved TRACE_MOVE: again")
    check(M.should_trace(g, 10 + M.TRACE_MAX, 0, 0), "standing still: again after TRACE_MAX")

    -- a slope (10 % along +X) and stairs (20 cm every 30 cm along +Y): she walks on them, not at the player's height
    local traces = 0
    local function slope(x, y, top, bottom)
        traces = traces + 1
        local z = 0.1 * x
        if y > 3000 then z = math.floor((y - 3000) / 30) * 20 end
        if z > top or z < bottom then return nil end
        return z
    end
    local her = { placed = false, x = 0, y = 0, z = 0, yaw = 0 }
    local p = { x = 0, y = 0, z = 0, yaw = 0, vx = 0, vy = 0, falling = false }
    local now = 0
    M.follow(her, p, 1 / 60, now, slope)
    check(near(her.z, 0.1 * her.x, 0.5), "put at her spot, she stands on the slope there (" .. string.format("%.1f vs %.1f", her.z, 0.1 * her.x) .. ")")
    -- the player walks up the slope at 3 m/s for 4 s (his feet on it too)
    local worst = 0
    traces = 0
    for i = 1, 240 do
        now = now + 1 / 60
        p.x, p.vx = p.x + 300 / 60, 300
        p.z = 0.1 * p.x
        M.follow(her, p, 1 / 60, now, slope)
        if i > 60 then worst = math.max(worst, math.abs(her.z - 0.1 * her.x)) end
    end
    check(worst < 6, "walking up the slope her feet stay on it (worst " .. string.format("%.1f", worst) .. " cm)")
    check(traces <= 4 * 20 + 1 and traces >= 4 * 4, "walking: traced at most 20 and at least 4 times a second (" .. traces .. " in 4 s)")
    -- standing still: at most 4 traces a second
    traces = 0
    p.vx = 0
    for _ = 1, 120 do
        now = now + 1 / 60
        M.follow(her, p, 1 / 60, now, slope)
    end
    check(traces <= 2 * 4 + 1, "standing: at most 4 traces a second (" .. traces .. " in 2 s)")

    -- stairs: the player climbs along +Y; she settles on the step she stands on
    her = { placed = false, x = 0, y = 0, z = 0, yaw = 0 }
    p = { x = 0, y = 3300, z = 200, yaw = 90, vx = 0, vy = 0, falling = false }
    for _ = 1, 120 do
        now = now + 1 / 60
        M.follow(her, p, 1 / 60, now, slope)
    end
    local step = math.floor((her.y - 3000) / 30) * 20
    check(near(her.z, step, 1), "on stairs she stands on her step (" .. string.format("%.1f vs %d", her.z, step) .. ")")

    -- a trace that misses keeps the last ground GROUND_KEEP seconds, then the player's
    her = { placed = false, x = 0, y = 0, z = 0, yaw = 0 }
    p = { x = 0, y = 0, z = 50, yaw = 0, vx = 0, vy = 0, falling = false }
    local function at(z) return function() return z end end
    M.follow(her, p, 1 / 60, now, at(80))
    check(near(her.ground, 80), "a hit is her ground (80, the player at 50)")
    for _ = 1, math.floor(M.GROUND_KEEP * 60 * 0.5) do
        now = now + 1 / 60
        M.follow(her, p, 1 / 60, now, at(nil))
    end
    check(near(her.ground, 80), "misses for half of GROUND_KEEP: the last ground holds")
    for _ = 1, math.floor(M.GROUND_KEEP * 60) do
        now = now + 1 / 60
        M.follow(her, p, 1 / 60, now, at(nil))
    end
    check(near(her.ground, 50), "misses for longer: the player's ground")
    check(her.trace.misses > 0 and her.trace.traces > her.trace.misses, "traces and misses are counted")

    -- a floor at 0 everywhere and solids over it: { x from, x to, top face, bottom face (nil: it stands on the floor) }.
    -- A trace down hits the highest top face at or below its start; a trace up the lowest bottom face above its start;
    -- a trace starting inside a solid does not hit that solid (as Unreal's line traces).
    local function world_with(solids)
        return function(x, _, from, to)
            local best
            if from >= to then
                if from >= 0 and to <= 0 then best = 0 end
                for _, s in ipairs(solids) do
                    if x >= s[1] and x <= s[2] and s[3] <= from and s[3] >= to and (best == nil or s[3] > best) then best = s[3] end
                end
            else
                for _, s in ipairs(solids) do
                    if s[4] and x >= s[1] and x <= s[2] and s[4] >= from and s[4] <= to and (best == nil or s[4] < best) then best = s[4] end
                end
            end
            return best
        end
    end
    -- the player stands on a balcony (its floor 2.5 m up, 20 cm thick) right over her: she stays on her floor
    local balcony = world_with({ { -500, 500, 250, 230 } })
    her = { placed = false, x = 0, y = 0, z = 0, yaw = 0 }
    p = { x = -20, y = 0, z = 0, yaw = 0, vx = 0, vy = 0, falling = false }
    M.follow(her, p, 1 / 60, now, balcony)
    p.x, p.z = 100, 250 -- he went up (stairs elsewhere) and stands over her
    local highest = 0
    for _ = 1, 180 do
        now = now + 1 / 60
        M.follow(her, p, 1 / 60, now, balcony)
        highest = math.max(highest, her.z)
    end
    check(highest < 1 and (her.trace.ceilings or 0) > 0, "a balcony over her head is a ceiling: she stays on her floor (highest " .. string.format("%.1f", highest) .. ")")
    -- the player climbed a ledge (solid to the floor, 2 m up): walking to her spot on it, she steps up onto it
    local ledge = world_with({ { 200, 5000, 200, nil } })
    her = { placed = false, x = 0, y = 0, z = 0, yaw = 0 }
    p = { x = 100, y = -90, z = 0, yaw = 0, vx = 0, vy = 0, falling = false }
    M.follow(her, p, 1 / 60, now, ledge)
    p.x, p.z = 800, 200
    for _ = 1, 300 do
        now = now + 1 / 60
        M.follow(her, p, 1 / 60, now, ledge)
    end
    check(her.x > 200 and near(her.z, 200, 2), "a ledge he climbed: she stands on it (" .. string.format("x %.0f z %.1f", her.x, her.z) .. ")")
end

-------------------------------------------------------------------------------- the sun
do
    -- colour temperature: a black body - warm at 2000 K, about white at 6500 K, blue above
    local r, g, b = M.temperature_rgb(2000)
    check(near(r, 1) and g < 0.4 and b < 0.1, string.format("2000 K is orange (%.2f %.2f %.2f)", r, g, b))
    r, g, b = M.temperature_rgb(6500)
    check(r > 0.9 and g > 0.9 and b > 0.85, string.format("6500 K is about white (%.2f %.2f %.2f)", r, g, b))
    local _, _, b9 = M.temperature_rgb(9000)
    r = M.temperature_rgb(9000)
    check(near(b9, 1) and r < 1, "9000 K is blue")
    check(near(M.srgb_to_linear(255), 1) and near(M.srgb_to_linear(0), 0) and near(M.srgb_to_linear(188), 0.5, 0.01), "sRGB 8-bit to linear")

    -- the sun among the directional lights: the atmosphere's sun first, then the brightest that lights the world
    check(M.pick_sun({ { intensity = 2, name = "Fill" }, { intensity = 10, name = "Sun" } }) == 2, "the brightest")
    check(M.pick_sun({ { intensity = 50, name = "Fill" }, { intensity = 3, atmosphere = true, name = "Sun" } }) == 2, "the atmosphere's sun first")
    check(M.pick_sun({ { intensity = 50, affects = false }, { intensity = 50, visible = false }, { intensity = 50, name = "Default__DirectionalLightComponent" },
        { intensity = 0 } }) == nil, "none: off, hidden, a class default, dark")

    -- the light line: a sun 45 degrees up, shining along +X and down: toward it is up and back (-X)
    local line = M.light_line({ Pitch = -45, Yaw = 0, Roll = 0 }, { R = 255, G = 128, B = 0 }, nil, 10, 1)
    local f = {}
    for w in line:gmatch("%S+") do f[#f + 1] = w end
    check(#f == 9 and f[1] == "L", "the light line: 9 fields")
    check(near(tonumber(f[2]), -0.7071, 1e-3) and near(tonumber(f[3]), 0.7071, 1e-3) and near(tonumber(f[4]), 0, 1e-3), "toward the sun, glTF: up and back")
    check(near(tonumber(f[5]), 1) and near(tonumber(f[6]), M.srgb_to_linear(128), 1e-3) and near(tonumber(f[7]), 0), "its colour, linear, max 1")
    check(near(tonumber(f[8]), 1) and near(tonumber(f[9]), 1), "a daylight sun (no lux scale): intensity 1, visible")
    M.SUN_LUX = 20
    line = M.light_line({ Pitch = -45, Yaw = 0, Roll = 0 }, { R = 255, G = 255, B = 255 }, 6500, 10, 0)
    M.SUN_LUX = nil
    f = {}
    for w in line:gmatch("%S+") do f[#f + 1] = w end
    check(near(tonumber(f[8]), 0.5) and near(tonumber(f[9]), 0) and tonumber(f[7]) < 1, "a lux scale, the temperature, in shade")
end

-------------------------------------------------------------------------------- a stub of UE4SS and a game
--- A fake game: a PlayerController with its camera manager and pawn, UE4SS's helpers and kismet library.
--- opts.no_pawn: pc.Pawn is nil (GTA DE's Gameface - no Pawn/Possess at all); w.view_target (settable after
--- the call, read live by pc:GetViewTarget()) stands in for the controller's view target, nil meaning none.
--- opts.no_pcm: pc.PlayerCameraManager is nil too (no camera at all).
function make_world(opts)
    opts = opts or {}
    local w = { frame = 1000, view_target = opts.view_target }
    local function object(t)
        t.IsValid = function() return true end
        t.GetFullName = function() return "BP_PlayerController_C /Game/Test" end
        return t
    end
    w.object = object -- exposed so a test can build its own actors (a view target) the same way
    w.traces = 0
    local kismet = object({
        GetFrameCount = function() return w.frame end,
        GetGameName = function() return { ToString = function() return "TestGame" end } end,
        -- UE4SS's call: (world context, start, end, channel, complex, ignore, draw, out hit, ignore self, colours, time)
        LineTraceSingle = function(_, context, from, to, channel, complex, ignore, draw, hit, ignore_self)
            w.traces = w.traces + 1
            if w.trace_error then error("LineTraceSingle is not a function in this game") end
            w.trace_args = { context = context, from = from, to = to, channel = channel, ignore = ignore, ignore_self = ignore_self }
            if to.Z > from.Z then return w.sun_blocked == true end -- a trace up toward the sun
            if w.ground == nil or w.ground > from.Z or w.ground < to.Z then return false end
            hit.ImpactPoint = { X = from.X, Y = from.Y, Z = w.ground }
            hit.bBlockingHit = true
            return true
        end,
    })
    local pawn = object({
        K2_GetActorLocation = function() return { X = 1000, Y = 2000, Z = 188 } end,
        K2_GetActorRotation = function() return { Pitch = 0, Yaw = 90, Roll = 0 } end,
        GetVelocity = function() return { X = 0, Y = 0, Z = 0 } end,
        CapsuleComponent = object({ GetScaledCapsuleHalfHeight = function() return 88 end }),
        CharacterMovement = object({ IsFalling = function() return false end }),
    })
    local pcm = object({
        GetCameraLocation = function() return { X = 700, Y = 1800, Z = 260 } end,
        GetCameraRotation = function() return { Pitch = -15, Yaw = 80, Roll = 0 } end,
        GetFOVAngle = function() return 90 end,
        CameraCachePrivate = { POV = { bConstrainAspectRatio = false, AspectRatio = 1.7778, PerspectiveNearClipPlane = 10 } },
    })
    -- what a real UE 5.6 offers: no IsPaused on the PlayerController (plain C++), GameplayStatics' IsGamePaused
    -- w.level: bumped by a test, a new level - the old PlayerController is gone, a new one is found
    w.level = 1
    local pc
    local function make_pc()
        local level = w.level
        pc = object({ Player = { AspectRatioAxisConstraint = 1 } })
        if not opts.no_pcm then pc.PlayerCameraManager = pcm end
        if not opts.no_pawn then pc.Pawn = pawn end
        pc.IsValid = function() return w.level == level end
        pc.GetViewTarget = function() return w.view_target end
        return pc
    end
    make_pc()
    local statics = object({ IsGamePaused = function(_, context) return context == pc and w.paused == true end })
    package.loaded["UEHelpers"] = nil
    package.preload["UEHelpers"] = function()
        return { GetKismetSystemLibrary = function() return kismet end,
            GetPlayerController = function() if not pc:IsValid() then make_pc() end return pc end,
            GetGameplayStatics = function() return statics end }
    end
    UnrealVersion = { GetMajor = function() return 5 end, GetMinor = function() return 6 end }
    EngineTickAvailable = true
    -- two directional lights: a fill and the sun (40 degrees up, warm). w.lights_valid = false: they are gone (a level
    -- change). w.ue4: Unreal 4's properties - no bAtmosphereSunLight (reading it throws), bUsedAsAtmosphereSunLight on the
    -- sun, which is the dimmer light there (picked only if that flag is read)
    w.finds = 0
    local function light(name, intensity, pitch, atmosphere)
        local l = object({ Intensity = intensity, bAffectsWorld = true, bVisible = true, LightColor = { R = 255, G = 240, B = 220, A = 255 },
            bUseTemperature = false, K2_GetComponentRotation = function() return { Pitch = pitch, Yaw = 30, Roll = 0 } end })
        l.GetFullName = function() return name end
        l.IsValid = function() return w.lights_valid ~= false end
        if w.ue4 then
            l.bUsedAsAtmosphereSunLight = atmosphere == true
            setmetatable(l, { __index = function(_, k)
                if k == "bAtmosphereSunLight" then error("property bAtmosphereSunLight not found in DirectionalLightComponent") end
            end })
        end
        return l
    end
    FindAllOf = function(class)
        w.finds = w.finds + 1
        if class ~= "DirectionalLightComponent" or w.no_lights then return nil end
        return { light("DirectionalLightComponent /Game/Maps/Test.Test:PersistentLevel.Fill.Light", 2, -70),
            light("DirectionalLightComponent /Game/Maps/Test.Test:PersistentLevel.Sun.Light", w.ue4 and 1 or 8, -40, true) }
    end
    return w
end

-------------------------------------------------------------------------------- one frame, UE4SS stubbed
do
    local written = {}
    local fake_pipe = {
        write = function(self, s) written[#written + 1] = s return self end,
        setvbuf = function() end,
        close = function() end,
    }
    local real_open = io.open
    io.open = function(name, mode)
        if name == M.PIPE then
            check(mode == "r+b", "the pipe is opened for writing without truncation (r+b)")
            return fake_pipe
        end
        return real_open(name, mode)
    end
    local clock = 100.0
    local real_clock = os.clock
    os.clock = function() return clock end
    local logs = {}
    print = function(s) logs[#logs + 1] = s end

    local world = make_world()
    local function set_frame(n) world.frame = n end
    UnrealVersion = { GetMajor = function() return 5 end, GetMinor = function() return 6 end }
    EngineTickAvailable = true
    local loop
    LoopInGameThreadAfterFrames = function(n, fn)
        check(n == 1, "it runs every engine frame")
        loop = fn
    end
    dofile(path)
    check(loop ~= nil, "it registered its frame loop")
    for i = 1, 5 do
        set_frame(1000 + i)
        clock = clock + 1 / 60
        loop()
        loop() -- a second call in the same engine frame sends nothing
    end
    local first_five = #written

    -- the game pauses (its pause menu): she hides within a tenth of a second, and comes back with the game
    local sent = #written
    world.paused = true
    for i = 1, 10 do
        set_frame(2000 + i)
        clock = clock + 1 / 60
        loop()
    end
    local during = table.concat(written, "", sent + 1)
    check(during:find("V 0\n", 1, true) ~= nil and select(2, during:gsub("\nC ", "")) <= 6,
        "paused: she is hidden (V 0) within 0.1 s (" .. during:gsub("\n", " | "):sub(1, 120) .. ")")
    world.paused = false
    for i = 1, 10 do
        set_frame(3000 + i)
        clock = clock + 1 / 60
        loop()
    end
    check(table.concat(written, "", #written - 1):find("V 1\n", 1, true) ~= nil, "unpaused: she is back")
    io.open, os.clock = real_open, real_clock

    local all = table.concat(written)
    check(all:find("I astra.unreal TestGame\n", 1, true) ~= nil, "it says who it is")
    check(all:find("\nE unreal\n", 1, true) ~= nil, "it says the game is an Unreal game (the add-on draws her before its tonemapper)")
    local cams, avatars, last_c = 0, 0, nil
    for l in table.concat(written, "", 1, first_five):gmatch("[^\n]+") do
        if l:sub(1, 2) == "C " then cams, last_c = cams + 1, l end
        if l:sub(1, 2) == "A " then avatars = avatars + 1 end
    end
    check(cams == 5 and avatars == 5, "one avatar and one camera per engine frame (" .. cams .. ", " .. avatars .. ")")
    check(last_c and last_c:find("^C 1005 7%.0000 2%.6000 18%.0000 ") ~= nil, "the camera: frame, glTF metres (" .. tostring(last_c) .. ")")
    check(last_c and last_c:find(" 90%.0000 h 0%.00000 0%.1000$") ~= nil, "the camera: fov 90 horizontal, near 0.1 m")
    -- her feet: at the capsule's bottom (188 - 88 cm = 1 m up)
    local a = all:match("A [^\n]+")
    local f = {}
    for w in a:gmatch("%S+") do f[#f + 1] = w end
    check(#f == 16 and near(tonumber(f[3]), 1.0, 0.05), "her feet are on the player's ground (" .. a .. ")")
    local seen_log = false
    for _, s in ipairs(logs) do if s:find("[AstraUE] connected", 1, true) then seen_log = true end end
    check(seen_log, "it logs the connection")
    if lines_out then
        local out = assert(real_open(lines_out, "wb"))
        out:write(all)
        out:close()
    end
end

-------------------------------------------------------------------------------- the ground, traced through UE4SS's stub
do
    local written = {}
    local fake_pipe = { write = function(self, s) written[#written + 1] = s return self end, setvbuf = function() end, close = function() end }
    local real_open, real_clock, real_print = io.open, os.clock, print
    io.open = function(name, mode)
        if name == M.PIPE then return fake_pipe end
        return real_open(name, mode)
    end
    local clock = 200.0
    os.clock = function() return clock end
    local logs = {}
    print = function(s) logs[#logs + 1] = s end
    local world = make_world()
    world.ground = 130 -- the ground under her spot is 30 cm above the player's feet (100): a slope
    local loop
    LoopInGameThreadAfterFrames = function(_, fn) loop = fn end
    dofile(path)
    for i = 1, 120 do
        world.frame = 7000 + i
        clock = clock + 1 / 60
        loop()
    end
    local a = table.concat(written):match("A [^\n]+\n$") or table.concat(written):match(".*(A [^\n]+)")
    local f = {}
    for w in a:gmatch("%S+") do f[#f + 1] = w end
    check(near(tonumber(f[3]), 1.3, 0.02), "her feet on the traced ground, 1.3 m (" .. a .. ")")
    check(world.traces >= 2 * 4 and world.traces <= 2 * 20 + 1, "rate-limited: " .. world.traces .. " traces in 2 s")
    check(world.trace_args.ignore_self == true and world.trace_args.channel == M.TRACE_CHANNEL, "traces on the Visibility channel, ignoring the player")
    -- the sun: found once in 2 s (not each frame), a light line ~5 times a second, the brighter light, in sun
    local text = table.concat(written)
    local lines = {}
    for l in text:gmatch("L [^\n]+") do lines[#lines + 1] = l end
    check(world.finds == 1, "the sun is searched for once in 2 s (" .. world.finds .. ")")
    check(#lines >= 8 and #lines <= 12, "a light line about 5 times a second (" .. #lines .. " in 2 s)")
    local lf = {}
    for w in (lines[#lines] or ""):gmatch("%S+") do lf[#lf + 1] = w end
    check(#lf == 9 and tonumber(lf[3]) > 0.6 and near(tonumber(lf[9]), 1), "the sun's line: 40 degrees up, she is in it (" .. tostring(lines[#lines]) .. ")")
    local said = false
    for _, s in ipairs(logs) do if s:find("the game's sun: DirectionalLightComponent /Game/Maps/Test.Test:PersistentLevel.Sun.Light", 1, true) then said = true end end
    check(said, "it says which light is the sun")
    -- something between her and the sun: in shade
    world.sun_blocked = true
    for i = 1, 30 do
        world.frame = 7500 + i
        clock = clock + 1 / 60
        loop()
    end
    lines = {}
    for l in table.concat(written):gmatch("L [^\n]+") do lines[#lines + 1] = l end
    lf = {}
    for w in lines[#lines]:gmatch("%S+") do lf[#lf + 1] = w end
    check(near(tonumber(lf[9]), 0), "a roof between her and the sun: visible 0 (" .. lines[#lines] .. ")")
    world.sun_blocked = false
    -- the sun found stays found: no search while it is valid (FindAllOf walks every object - a hitch every 5 s before)
    for i = 1, 12 * 60 do
        world.frame = 9000 + i
        clock = clock + 1 / 60
        loop()
    end
    check(world.finds == 1, "a valid sun is not searched for again (" .. world.finds .. " searches in 14 s)")
    -- ...until it is gone (a level change): searched for at once
    world.lights_valid = false
    for i = 1, 30 do
        world.frame = 11000 + i
        clock = clock + 1 / 60
        loop()
    end
    check(world.finds == 2, "the sun gone: searched for again (" .. world.finds .. ")")
    world.lights_valid = true
    -- a game where the call fails: logged once, never called again, she stands on the player's ground
    world.trace_error = true
    local before = world.traces
    for i = 1, 90 do
        world.frame = 8000 + i
        clock = clock + 1 / 60
        loop()
    end
    local said = 0
    for _, s in ipairs(logs) do if s:find("cannot trace the ground", 1, true) then said = said + 1 end end
    check(said == 1 and world.traces == before + 1, "a failing trace: said once, not called again (" .. said .. ", " .. (world.traces - before) .. ")")
    a = table.concat(written):match(".*(A [^\n]+)")
    f = {}
    for w in a:gmatch("%S+") do f[#f + 1] = w end
    check(near(tonumber(f[3]), 1.0, 0.05), "after that: the player's ground, 1.0 m (" .. a .. ")")
    io.open, os.clock, print = real_open, real_clock, real_print
end

-------------------------------------------------------------------------------- a level with no directional light
do
    local fake_pipe = { write = function(self) return self end, setvbuf = function() end, close = function() end }
    local real_open, real_clock, real_print = io.open, os.clock, print
    io.open = function(name, mode)
        if name == M.PIPE then return fake_pipe end
        return real_open(name, mode)
    end
    local clock = 900.0
    os.clock = function() return clock end
    print = function() end
    local world = make_world()
    world.no_lights = true
    local loop
    LoopInGameThreadAfterFrames = function(_, fn) loop = fn end
    dofile(path)
    local function run(seconds)
        for _ = 1, math.floor(seconds * 30) do
            world.frame = world.frame + 1
            clock = clock + 1 / 30
            loop()
        end
    end
    run(130)
    local in_130 = world.finds
    check(in_130 <= 6, "no sun in the level: searched for less and less often (" .. in_130 .. " searches in 130 s, every 5 s would be 26)")
    -- a new level: searched for at once, the wait starts over
    world.level = world.level + 1
    run(3)
    check(world.finds == in_130 + 1, "a new level: the sun searched for again at once (" .. (world.finds - in_130) .. ")")
    run(6)
    check(world.finds == in_130 + 2, "...and again 5 s later, the wait started over (" .. (world.finds - in_130) .. ")")
    io.open, os.clock, print = real_open, real_clock, real_print
end

-------------------------------------------------------------------------------- Unreal 4: the sun's atmosphere flag under its old name
do
    local fake_pipe = { write = function(self) return self end, setvbuf = function() end, close = function() end }
    local real_open, real_clock, real_print = io.open, os.clock, print
    io.open = function(name, mode)
        if name == M.PIPE then return fake_pipe end
        return real_open(name, mode)
    end
    local clock = 700.0
    os.clock = function() return clock end
    local logs = {}
    print = function(s) logs[#logs + 1] = s end
    local world = make_world()
    world.ue4 = true
    local loop
    LoopInGameThreadAfterFrames = function(_, fn) loop = fn end
    dofile(path)
    for i = 1, 60 do
        world.frame = 12000 + i
        clock = clock + 1 / 60
        loop()
    end
    io.open, os.clock, print = real_open, real_clock, real_print
    local sun = false
    for _, s in ipairs(logs) do if s:find("the game's sun: DirectionalLightComponent /Game/Maps/Test.Test:PersistentLevel.Sun.Light", 1, true) then sun = true end end
    check(sun, "Unreal 4: the atmosphere's sun (bUsedAsAtmosphereSunLight) is the sun, though bAtmosphereSunLight does not exist")
end

-------------------------------------------------------------------------------- no add-on: it still reads and logs the camera
do
    local real_open, real_clock, real_print = io.open, os.clock, print
    local clock = 500.0
    io.open = function(name, mode)
        if name == M.PIPE then return nil, "no such pipe" end
        return real_open(name, mode)
    end
    os.clock = function() return clock end
    local logs = {}
    print = function(s) logs[#logs + 1] = s end
    local world = make_world()
    local loop
    LoopInGameThreadAfterFrames = function(_, fn) loop = fn end
    dofile(path)
    for i = 1, 130 do
        world.frame = 5000 + i
        clock = clock + 1 / 60
        loop()
    end
    io.open, os.clock, print = real_open, real_clock, real_print
    local logged = false
    for _, s in ipairs(logs) do if s:find("[AstraUE] frame", 1, true) and s:find("camera 700 1800 260", 1, true) then logged = true end end
    check(logged, "without the add-on it still logs the camera it reads")
    for _, s in ipairs(logs) do if s:find("error", 1, true) then check(false, "no errors without the add-on: " .. s) end end
end

-------------------------------------------------------------------------------- no Pawn (GTA DE's Gameface): the view target
do
    local written, logs = {}, {}
    local fake_pipe = { write = function(self, s) written[#written + 1] = s return self end, setvbuf = function() end, close = function() end }
    local real_open, real_clock, real_print = io.open, os.clock, print
    io.open = function(name, mode)
        if name == M.PIPE then return fake_pipe end
        return real_open(name, mode)
    end
    local clock = 300.0
    os.clock = function() return clock end
    print = function(s) logs[#logs + 1] = s end
    local world = make_world({ no_pawn = true })
    local loop
    LoopInGameThreadAfterFrames = function(_, fn) loop = fn end
    dofile(path)
    world.view_target = world.object({
        K2_GetActorLocation = function() return { X = 500, Y = -200, Z = 90 } end,
        K2_GetActorRotation = function() return { Pitch = 0, Yaw = 180, Roll = 0 } end,
        GetVelocity = function() return { X = 0, Y = 0, Z = 0 } end,
    })
    world.view_target.GetFullName = function() return "BP_GTAViewTarget_C /Game/GTA3" end -- object() sets a default; override after
    for i = 1, 30 do
        world.frame = 6000 + i
        clock = clock + 1 / 60
        loop()
    end
    local cams, avatars = 0, 0
    for l in table.concat(written):gmatch("[^\n]+") do
        if l:sub(1, 2) == "C " then cams = cams + 1 end
        if l:sub(1, 2) == "A " then avatars = avatars + 1 end
    end
    check(cams == 30 and avatars == 30, "no Pawn, a view target: a camera and her placement every frame regardless (" .. cams .. ", " .. avatars .. ")")
    local c = table.concat(written):match(".*(C [^\n]+)")
    local cf = {}
    for w2 in c:gmatch("%S+") do cf[#cf + 1] = w2 end
    check(near(tonumber(cf[3]), 7.0) and near(tonumber(cf[4]), 2.6) and near(tonumber(cf[5]), 18.0),
        "no Pawn: the camera itself (pcm) is unaffected, still sent (" .. c .. ")")
    local a = table.concat(written):match(".*(A [^\n]+)")
    local f = {}
    for w2 in a:gmatch("%S+") do f[#f + 1] = w2 end
    check(near(tonumber(f[2]), 6.40, 0.02) and near(tonumber(f[3]), 0.90, 0.02) and near(tonumber(f[4]), -2.90, 0.02),
        "no Pawn, a view target: she follows IT (behind-and-beside, her usual spot), not the camera (" .. a .. ")")
    local said_pcm = 0
    for _, s in ipairs(logs) do if s:find("camera manager:", 1, true) then said_pcm = said_pcm + 1 end end
    check(said_pcm == 1, "the camera manager's class is said once at start (" .. said_pcm .. ")")

    -- the game pauses: no camera (V 0) once paused() notices (its own 0.1 s cache), and the WHY diagnostic
    -- names the view target's class
    world.paused = true
    for i = 1, 30 do
        world.frame = 6100 + i
        clock = clock + 1 / 60
        loop()
    end
    check(written[#written] == "V 0\n", "paused: no camera, even with a view target (" .. tostring(written[#written]) .. ")")
    local said_why = false
    for _, s in ipairs(logs) do
        if s:find("no camera sent (paused)", 1, true) and s:find("pc.Pawn none", 1, true) and s:find("BP_GTAViewTarget_C", 1, true) then said_why = true end
    end
    check(said_why, "paused: WHY, with pc.Pawn's and the view target's class")
    io.open, os.clock, print = real_open, real_clock, real_print
end

-------------------------------------------------------------------------------- no Pawn and no view target: the camera alone
do
    local written, logs = {}, {}
    local fake_pipe = { write = function(self, s) written[#written + 1] = s return self end, setvbuf = function() end, close = function() end }
    local real_open, real_clock, real_print = io.open, os.clock, print
    io.open = function(name, mode)
        if name == M.PIPE then return fake_pipe end
        return real_open(name, mode)
    end
    local clock = 400.0
    os.clock = function() return clock end
    print = function(s) logs[#logs + 1] = s end
    local world = make_world({ no_pawn = true }) -- view_target left nil: no actor for the player at all
    local loop
    LoopInGameThreadAfterFrames = function(_, fn) loop = fn end
    dofile(path)
    for i = 1, 30 do
        world.frame = 7000 + i
        clock = clock + 1 / 60
        loop()
    end
    io.open, os.clock, print = real_open, real_clock, real_print
    local cams, avatars = 0, 0
    for l in table.concat(written):gmatch("[^\n]+") do
        if l:sub(1, 2) == "C " then cams = cams + 1 end
        if l:sub(1, 2) == "A " then avatars = avatars + 1 end
    end
    check(cams == 30 and avatars == 30, "no Pawn, no view target: a camera and her placement every frame all the same (" .. cams .. ", " .. avatars .. ")")
    -- her spot: behind-and-beside the CAMERA (pos 700,1800,260 yaw 80), the same BACK/SIDE a Pawn would get
    local a = table.concat(written):match(".*(A [^\n]+)")
    local f = {}
    for w2 in a:gmatch("%S+") do f[#f + 1] = w2 end
    check(near(tonumber(f[2]), 5.8706, 0.01) and near(tonumber(f[3]), 2.60, 0.01) and near(tonumber(f[4]), 16.7776, 0.01),
        "no Pawn, no view target: she is placed relative to the camera alone (" .. a .. ")")
end

-------------------------------------------------------------------------------- no camera manager at all: WHY, rate-limited
do
    local written, logs = {}, {}
    local fake_pipe = { write = function(self, s) written[#written + 1] = s return self end, setvbuf = function() end, close = function() end }
    local real_open, real_clock, real_print = io.open, os.clock, print
    io.open = function(name, mode)
        if name == M.PIPE then return fake_pipe end
        return real_open(name, mode)
    end
    local clock = 600.0
    os.clock = function() return clock end
    print = function(s) logs[#logs + 1] = s end
    local world = make_world({ no_pcm = true })
    local loop
    LoopInGameThreadAfterFrames = function(_, fn) loop = fn end
    dofile(path)
    for i = 1, 25 * 60 do
        world.frame = 9000 + i
        clock = clock + 1 / 60
        loop()
    end
    io.open, os.clock, print = real_open, real_clock, real_print
    local cams = 0
    for l in table.concat(written):gmatch("[^\n]+") do if l:sub(1, 2) == "C " then cams = cams + 1 end end
    check(cams == 0, "no camera manager: never a camera (" .. cams .. ")")
    local said = 0
    for _, s in ipairs(logs) do if s:find("no camera sent (no pcm)", 1, true) and s:find("pc.Pawn BP_PlayerController_C", 1, true) then said = said + 1 end end
    check(said >= 2 and said <= 3, "no camera manager: the WHY diagnostic, at most once every 10 s over 25 s (" .. said .. ")")
end

io.stdout:write(string.format("AstraUE: %d checks, %d failed\n", checks, failed))
os.exit(failed == 0 and 0 or 1)
