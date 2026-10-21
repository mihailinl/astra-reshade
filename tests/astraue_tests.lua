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
--- opts.axis: the local player's AspectRatioAxisConstraint (default 1, MaintainXFOV). opts.draws = { tan_v, w, h }: the
--- PlayerController answers ProjectWorldLocationToScreen with that projection (no answer at all without it).
--- opts.ortho: the camera's POV is orthographic. w.cam_loc / w.cam_rot (settable): the camera, else a fixed one.
--- w.pawn_loc / w.pawn_vel (settable): the pawn's location and what GetVelocity says, else fixed / zero.
--- opts.pov_near: the camera POV's PerspectiveNearClipPlane (default 10; <= 0: "the engine's"); opts.engine_near: the
--- Engine object's NearClipPlane (none without it).
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
        -- UE4SS's call for the capsule-on-a-channel family (fix #1 - NEVER *ForObjects: see main.lua's own
        -- comment on M.SWEEP_RADIUS): (world context, start, end, radius, half height, trace channel, complex,
        -- ignore, draw, out hit, ignore self, colours, time). w.capsule_error: the call throws (a UE4SS build
        -- without this binding). w.boxes: axis-aligned {x1, y1, x2, y2[, top[, bottom]]} obstacles in the XY
        -- plane - top/bottom (world Z, cm) optional: absent, the box blocks at any height (today's wall); given,
        -- it blocks only a capsule whose swept Z range [z-half_height, z+half_height] overlaps [bottom or 0,
        -- top] - good enough to exercise the slide/stuck logic AND fix #2's step allowance without a real
        -- capsule-vs-mesh test. w.capsule_calls: how many times this was actually invoked; w.capsule_args: the
        -- last call's context/channel/ignore/ignore_self, for the contract checks below.
        CapsuleTraceSingle = function(_, context, from, to, radius, half_height, channel, complex, ignore, draw, hit, ignore_self)
            w.capsule_calls = (w.capsule_calls or 0) + 1
            if w.capsule_error then error("CapsuleTraceSingle is not a function in this game") end
            w.capsule_args = { context = context, channel = channel, ignore = ignore, ignore_self = ignore_self }
            local lo, hi = from.Z - half_height, from.Z + half_height
            local best_t
            for _, b in ipairs(w.boxes or {}) do
                if b[5] == nil or (lo <= b[5] and hi >= (b[6] or 0)) then
                    local bx1, by1, bx2, by2 = b[1] - radius, b[2] - radius, b[3] + radius, b[4] + radius
                    -- segment (from.X,from.Y)-(to.X,to.Y) vs the inflated box, Liang-Barsky slab test
                    local dx, dy = to.X - from.X, to.Y - from.Y
                    local tmin, tmax = 0, 1
                    local function clip(p, q)
                        if p == 0 then
                            if q < 0 then return false end
                        elseif p < 0 then
                            local r = q / p
                            if r > tmax then return false end
                            if r > tmin then tmin = r end
                        else
                            local r = q / p
                            if r < tmin then return false end
                            if r < tmax then tmax = r end
                        end
                        return true
                    end
                    if clip(-dx, from.X - bx1) and clip(dx, bx2 - from.X) and clip(-dy, from.Y - by1) and clip(dy, by2 - from.Y) and tmin <= tmax then
                        if best_t == nil or tmin < best_t then best_t = tmin end
                    end
                end
            end
            if best_t ~= nil then
                local dx, dy = to.X - from.X, to.Y - from.Y
                hit.ImpactPoint = { X = from.X + dx * best_t, Y = from.Y + dy * best_t, Z = from.Z }
                hit.bBlockingHit = true
                hit.Time = best_t
                hit.bStartPenetrating = best_t <= 0
                return true
            end
            hit.Time = 1
            return false
        end,
    })
    local pawn = object({
        K2_GetActorLocation = function() return w.pawn_loc or { X = 1000, Y = 2000, Z = 188 } end,
        K2_GetActorRotation = function() return { Pitch = 0, Yaw = 90, Roll = 0 } end,
        GetVelocity = function() return w.pawn_vel or { X = 0, Y = 0, Z = 0 } end,
        CapsuleComponent = object({ GetScaledCapsuleHalfHeight = function() return 88 end }),
        CharacterMovement = object({ IsFalling = function() return false end }),
    })
    local pcm = object({
        GetCameraLocation = function() return w.cam_loc or { X = 700, Y = 1800, Z = 260 } end,
        GetCameraRotation = function() return w.cam_rot or { Pitch = -15, Yaw = 80, Roll = 0 } end,
        GetFOVAngle = function() return 90 end,
        CameraCachePrivate = { POV = { bConstrainAspectRatio = false, AspectRatio = 1.7778, PerspectiveNearClipPlane = opts.pov_near or 10,
            ProjectionMode = opts.ortho and 1 or 0 } },
    })
    -- what a real UE 5.6 offers: no IsPaused on the PlayerController (plain C++), GameplayStatics' IsGamePaused
    -- w.level: bumped by a test, a new level - the old PlayerController is gone, a new one is found
    w.level = 1
    local pc
    local function make_pc()
        local level = w.level
        pc = object({ Player = { AspectRatioAxisConstraint = opts.axis or 1 } })
        if not opts.no_pcm then pc.PlayerCameraManager = pcm end
        if opts.draws then
            -- the game's real projection (opts.draws: tan of its half vertical fov, its view in pixels); w.draws_off:
            -- it answers false (a loading screen). The pinhole is spelled here, not borrowed from main.lua.
            pc.ProjectWorldLocationToScreen = function(_, at, screen, relative)
                w.projections, w.relative = (w.projections or 0) + 1, relative
                if w.draws_off then return false end
                local d, cl, cr = opts.draws, pcm.GetCameraLocation(), pcm.GetCameraRotation()
                local P, Y, R = math.rad(cr.Pitch), math.rad(cr.Yaw), math.rad(cr.Roll)
                local SP, CP, SY, CY, SR, CR = math.sin(P), math.cos(P), math.sin(Y), math.cos(Y), math.sin(R), math.cos(R)
                local fx, fy, fz = CP * CY, CP * SY, SP                                       -- FRotationMatrix X
                local ux, uy, uz = -(CR * SP * CY + SR * SY), CY * SR - CR * SP * SY, CR * CP -- FRotationMatrix Z
                local dx, dy, dz = at.X - cl.X, at.Y - cl.Y, at.Z - cl.Z
                local depth = dx * fx + dy * fy + dz * fz
                if depth <= 0 then return false end
                screen.X = d.w / 2
                screen.Y = d.h / 2 - ((dx * ux + dy * uy + dz * uz) / depth) / d.tan_v * (d.h / 2)
                return true
            end
        end
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
    -- a navmesh path stub, reached through StaticFindObject the same way GameplayStatics/KismetSystemLibrary's
    -- CDOs already are. w.nav_error: the lookup itself throws (no NavigationSystem module). w.nav_points: the
    -- path's PathPoints, an array of {X=,Y=,Z=} (nil/absent: IsValid() answers false, as a level with no baked
    -- NavMesh would). w.nav_partial: IsPartial() answers true (a path that doesn't actually reach the goal).
    -- w.nav_calls: how many times FindPathToLocationSynchronously was actually invoked.
    local navsys = object({
        FindPathToLocationSynchronously = function(_, context, from, to)
            w.nav_calls = (w.nav_calls or 0) + 1
            return { PathPoints = w.nav_points,
                IsValid = function() return w.nav_points ~= nil end,
                IsPartial = function() return w.nav_partial == true end }
        end,
    })
    StaticFindObject = function(name)
        if w.nav_error then error("StaticFindObject is not a function in this game") end
        if name == "/Script/NavigationSystem.Default__NavigationSystemV1" then return navsys end
        return nil
    end
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
    FindFirstOf = function(class)
        if class == "Engine" and opts.engine_near then return object({ NearClipPlane = opts.engine_near }) end
        return nil
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

-------------------------------------------------------------------------------- the fov she is drawn with: the game's own projection
-- The owner, 2026-10-20: "in Mecha Chameleon she is so small" (UE 5.6, its settings read "fov 90.0v"). A wrong axis
-- draws her ~0.56x (or 1.8x) her size; so the fov is measured from where the game itself projects two points.
do
    -- the pure measure, against an independent pinhole: any pitch/yaw/roll, any fov, any pixel size or scale
    local worst = 0
    for _, tv in ipairs({ math.tan(math.rad(17.5)), math.tan(math.rad(29.36)), 1.0, math.tan(math.rad(60)) }) do
        for _, size in ipairs({ { 2560, 1440 }, { 1280, 720 }, { 3440, 1440 }, { 1080, 1920 } }) do
            for _, r in ipairs({ { 0, 0, 0 }, { -15, 80, 0 }, { 60, -120, 0 }, { -40, 33, 90 }, { 10, 200, -35 } }) do
                local fx, fy, fz, ux, uy, uz = M.rotator_axes(r[1], r[2], r[3])
                local cam = { 500, -300, 120 }
                local function project(x, y, z)
                    local dx, dy, dz = x - cam[1], y - cam[2], z - cam[3]
                    local depth = dx * fx + dy * fy + dz * fz
                    if depth <= 0 then return nil end
                    return size[1] / 2, size[2] / 2 - ((dx * ux + dy * uy + dz * uz) / depth) / tv * (size[2] / 2)
                end
                local t = M.projected_tan_v(project, cam[1], cam[2], cam[3], fx, fy, fz, ux, uy, uz)
                worst = math.max(worst, t and math.abs(t - tv) / tv or 1)
            end
        end
    end
    check(worst < 1e-6, string.format("measured fov: exact for every pose, fov and view size (worst relative error %.2g)", worst))
    check(M.projected_tan_v(function() return nil end, 0, 0, 0, 1, 0, 0, 0, 0, 1) == nil, "measured fov: no answer, no measure")
    check(M.projected_tan_v(function() return 640, 360 end, 0, 0, 0, 1, 0, 0, 0, 0, 1) == nil, "measured fov: a degenerate answer is refused")
end

local function run_frames(opts, n, before, hers)
    local written = {}
    local fake_pipe = { write = function(self, str) written[#written + 1] = str return self end, setvbuf = function() end, close = function() end }
    local real_open, real_clock, real_print = io.open, os.clock, print
    io.open = function(name, mode)
        if name == M.PIPE then return fake_pipe end
        return real_open(name, mode)
    end
    local clock = 300.0
    os.clock = function() return clock end
    local logs = {}
    print = function(str) logs[#logs + 1] = str end
    local world = make_world(opts)
    if before then before(world) end
    local loop
    LoopInGameThreadAfterFrames = function(_, fn) loop = fn end
    dofile(path)
    for i = 1, n do
        world.frame = 9000 + i
        clock = clock + 1 / 60
        if world.on_frame then world.on_frame(i) end
        local before_n = #written
        loop()
        if hers then
            for l in table.concat(written, "", before_n + 1):gmatch("A [^\n]+") do
                local f = {}
                for x in l:gmatch("%S+") do f[#f + 1] = tonumber(x) end
                hers[#hers + 1] = { f[1], f[3] } -- glTF x, z: UE x, y (metres; "A" itself is no number)
            end
        end
    end
    io.open, os.clock, print = real_open, real_clock, real_print
    local last_c
    for l in table.concat(written):gmatch("[^\n]+") do if l:sub(1, 2) == "C " then last_c = l end end
    local f = {}
    for x in (last_c or ""):gmatch("%S+") do f[#f + 1] = x end
    return f, world, logs
end
do
    -- Mecha Chameleon's case: its settings say MaintainYFOV ("90.0v"), but the game draws 90 degrees ACROSS a 16:9
    -- view (vertical 58.72): she is sent the drawn fov, not the settings' reading
    local tv = math.tan(math.rad(45)) * 1440 / 2560
    local f, world = run_frames({ axis = 0, draws = { tan_v = tv, w = 2560, h = 1440 } }, 10)
    check(f[13] == "v" and near(tonumber(f[12]), math.deg(2 * math.atan(tv)), 0.01),
        string.format("drawn fov: the game's own projection is sent (%s%s, want %.2fv)", tostring(f[12]), tostring(f[13]), math.deg(2 * math.atan(tv))))
    check(world.relative == true, "drawn fov: asked in the player's own view (letterboxing and split screens measure their view)")
    -- a game whose settings are right: the same fov as before, now measured
    local f2 = run_frames({ axis = 1, draws = { tan_v = tv, w = 1280, h = 720 } }, 10)
    check(near(tonumber(f2[12]), 58.72, 0.01) and f2[13] == "v", "drawn fov: a game read right before is drawn the same (" .. tostring(f2[12]) .. f2[13] .. ")")
    -- no answer this frame (a loading screen): the settings' fov, and it keeps asking
    local f3, world3 = run_frames({ axis = 0, draws = { tan_v = tv, w = 2560, h = 1440 } }, 40, function(w) w.draws_off = true end)
    check(f3[13] == "v" and near(tonumber(f3[12]), 90, 0.001) and (world3.projections or 0) >= 40,
        "drawn fov: no answer - the settings' fov, still asking every frame (" .. tostring(f3[12]) .. tostring(f3[13]) .. ", " .. tostring(world3.projections) .. ")")
    -- a game without the function: the settings' fov; after 2 s it is asked once a second, said once
    local f4, world4, logs4 = run_frames({ axis = 1 }, 600) -- the stub PlayerController has no ProjectWorldLocationToScreen
    local said = 0
    for _, l in ipairs(logs4) do if l:find("does not say where it draws", 1, true) then said = said + 1 end end
    check(f4[13] == "h" and near(tonumber(f4[12]), 90, 0.001) and said == 1, "drawn fov: no such function - the settings' fov, said once (" .. said .. ")")
    -- a long stretch without an answer, then answers again (an alt-tab, a long load): asked about once a second
    -- meanwhile, and measured again within a second of the game answering - never given up for good
    local asked_while_off
    local f5, world5 = run_frames({ axis = 0, draws = { tan_v = tv, w = 2560, h = 1440 } }, 900, function(w)
        w.draws_off = true
        w.on_frame = function(i)
            if i == 600 then asked_while_off, w.draws_off = w.projections, false end
        end
    end)
    check(asked_while_off ~= nil and asked_while_off < 120 + 12, "drawn fov: no answer for 10 s - asked about once a second after the first 2 s (" .. tostring(asked_while_off) .. " asks)")
    check(near(tonumber(f5[12]), math.deg(2 * math.atan(tv)), 0.01) and f5[13] == "v", "drawn fov: answers again - measured again (" .. tostring(f5[12]) .. tostring(f5[13]) .. ")")
    -- an orthographic camera: not measured (the measure is a pinhole's), the settings' fov
    local f6, world6 = run_frames({ axis = 1, ortho = true, draws = { tan_v = tv, w = 2560, h = 1440 } }, 120)
    check(f6[13] == "h" and (world6.projections or 0) == 0, "drawn fov: an orthographic camera is not measured (" .. tostring(world6.projections) .. " asks)")
    -- a NaN answer (a degenerate view) is no answer
    check(M.projected_tan_v(function() return 0 / 0, 0 / 0 end, 0, 0, 0, 1, 0, 0, 0, 0, 1) == nil, "drawn fov: a NaN answer is refused")
end

-------------------------------------------------------------------------------- the camera alone: its view turning is not him going
do
    -- no Pawn and no view target: his speed is the camera's own motion, which a third-person boom has whenever the view
    -- turns. An ordinary look round (300 cm boom, 10 deg/s: the camera moves ~52 cm/s) is not him going; she stays.
    -- (A long orbit still carries the CAMERA - which is him here - away from her: then she walks back, calmly.)
    local hers = {}
    run_frames({ no_pawn = true }, 480, function(w)
        w.on_frame = function(i)
            local yaw = i <= 120 and 0 or i <= 240 and 10 * (i - 120) / 60 or 20 -- a 20 degree look round, then still
            local yr = math.rad(yaw)
            w.cam_loc = { X = 1000 - math.cos(yr) * 300, Y = 2000 - math.sin(yr) * 300, Z = 260 }
            w.cam_rot = { Pitch = -15, Yaw = yaw, Roll = 0 }
        end
    end, hers)
    local moved = 0
    for i = 120, #hers do moved = math.max(moved, 100 * math.sqrt((hers[i][1] - hers[120][1]) ^ 2 + (hers[i][2] - hers[120][2]) ^ 2)) end
    check(#hers >= 480 and moved < 1, string.format("camera alone: an ordinary view turn is not him going - she stays (moved %.1f cm)", moved))
    -- the camera carried forward (him walking, 150 cm/s): she follows
    local hers2 = {}
    run_frames({ no_pawn = true }, 300, function(w)
        w.on_frame = function(i)
            w.cam_loc = { X = 700 + 150 * i / 60, Y = 1800, Z = 260 }
            w.cam_rot = { Pitch = -15, Yaw = 0, Roll = 0 }
        end
    end, hers2)
    local went = 100 * (hers2[#hers2][1] - hers2[60][1])
    check(went > 300, string.format("camera alone: a camera carried forward is him going - she follows (%.0f cm)", went))
end

-------------------------------------------------------------------------------- standing: she only walks
do
    -- a creep inside the go/stop gap (9-14 cm/s, from standing) and a stand far away: she only ever walks, at her calm walk
    for _, pspeed in ipairs({ 0, 9, 14 }) do
        local her, p = { placed = false, x = 0, y = 0, z = 0, yaw = 0 }, { x = 0, y = 0, z = 0, yaw = 0, vx = 0, vy = 0, falling = false }
        M.follow(her, p, 1 / 60)
        her.x, her.y = -400, 0
        local fastest = 0
        p.vx = pspeed
        for _ = 1, 1800 do
            p.x = p.x + pspeed / 60
            M.follow(her, p, 1 / 60)
            fastest = math.max(fastest, her.speed)
        end
        check(fastest <= M.WALK_PACE * 0.01 + 1e-6, string.format("standing or creeping %d cm/s: she only walks, never rushes (fastest %.2f m/s)", pspeed, fastest))
    end
    -- he stops dead while she is still catching up at CALM_PACE: her pace comes down to her walk gently, not in a jolt
    local her, p = { placed = false, x = 0, y = 0, z = 0, yaw = 0 }, { x = 0, y = 0, z = 0, yaw = 0, vx = 0, vy = 0, falling = false }
    M.follow(her, p, 1 / 60)
    her.x = -1200
    p.vx = 20
    for _ = 1, 300 do
        p.x = p.x + 20 / 60
        M.follow(her, p, 1 / 60)
    end
    local before, worst_drop = her.v, 0
    p.vx = 0
    for _ = 1, 60 do
        local v0 = her.v
        M.follow(her, p, 1 / 60)
        worst_drop = math.max(worst_drop, (v0 - her.v) * 60)
    end
    check(before > M.WALK_PACE + 20 and worst_drop <= M.SETTLE + 1 and near(her.v, M.WALK_PACE, 1),
        string.format("he stops while she catches up at %.0f cm/s: she eases to her walk (worst %.0f cm/s^2, now %.0f)", before, worst_drop, her.v))
end

-------------------------------------------------------------------------------- 0.2.6: his speed is what he really does; first person keeps her further
do
    -- the pure measure: a report that agrees is taken; a report of ~0 while he moves is not; no report: the displacement;
    -- a teleport is no speed
    local st = {}
    local vx, vy
    for i = 0, 30 do vx, vy = M.real_velocity(st, 300 * i / 60, 0, 300, 0, i / 60) end
    check(near(vx, 300, 0.01) and near(vy, 0, 0.01), "his speed: a report that agrees is his speed")
    st = {}
    for i = 0, 30 do vx, vy = M.real_velocity(st, 300 * i / 60, 0, 0, 0, i / 60) end
    check(near(vx, 300, 3), string.format("his speed: a report of 0 while he walks 300 cm/s is not taken (%.1f)", vx))
    st = {}
    for i = 0, 30 do vx, vy = M.real_velocity(st, 0, 120 * i / 60, nil, nil, i / 60) end
    check(near(vy, 120, 2), string.format("his speed: no report - his displacement (%.1f)", vy))
    vx, vy = M.real_velocity(st, 100000, 0, 0, 0, 31 / 60)
    check(math.abs(vx) < 1 and math.abs(vy) < 200, string.format("his speed: a teleport is no speed (%.0f, %.0f)", vx, vy))
    -- standing still with a jittery position (a few mm a frame): no speed to speak of
    st = {}
    for i = 0, 120 do vx, vy = M.real_velocity(st, (i % 2) * 0.3, 0, 0, 0, i / 60) end
    check(math.sqrt(vx * vx + vy * vy) < M.GO_STOP, string.format("his speed: millimetre jitter is standing (%.1f cm/s)", math.sqrt(vx * vx + vy * vy)))
    -- review of 0.2.6: a 1 s hitch while he walks 300 cm/s (reported right) is no 3000 cm/s spike; a 15 m respawn while
    -- he stands (reported 0) is no sprint; a stretch with no sample (the actor did not answer) starts over
    local worst = 0
    st = {}
    local t, x = 0, 0
    for i = 1, 120 do
        local step = (i == 60) and 1.0 or 1 / 60
        t, x = t + step, x + 300 * step
        vx = M.real_velocity(st, x, 0, 300, 0, t)
        worst = math.max(worst, vx)
    end
    check(worst <= 300 * 1.5 + M.GO_START, string.format("his speed: a 1 s hitch is no spike (worst %.0f cm/s)", worst))
    st, worst = {}, 0
    for i = 1, 120 do
        vx = M.real_velocity(st, i == 60 and 1500 or (i > 60 and 1500 or 0), 0, 0, 0, i / 60)
        worst = math.max(worst, math.abs(vx))
    end
    check(worst < M.GO_STOP, string.format("his speed: a 15 m respawn is no speed (worst %.0f cm/s)", worst))
end
do
    -- review of 0.2.6: a camera hovering at the first-person edge (99/101 cm, every other frame) must not flip her
    -- distance every frame - once in first person she stays there until FIRST_PERSON_LEAVE
    local her, p = { placed = false, x = 0, y = 0, z = 0, yaw = 0 }, { x = 0, y = 0, z = 0, yaw = 0, vx = 300, vy = 0, falling = false, camera_dist = 99 }
    M.follow(her, p, 1 / 60)
    local flips, last = 0, her.first_person
    for i = 1, 240 do
        p.x, p.camera_dist = p.x + 5, (i % 2 == 0) and 99 or 101
        M.follow(her, p, 1 / 60)
        if her.first_person ~= last then flips, last = flips + 1, her.first_person end
    end
    check(flips == 0 and her.first_person == true, string.format("first person: a camera hovering at the edge does not flip her distance (%d flips)", flips))
    p.camera_dist = 300
    M.follow(her, p, 1 / 60)
    check(her.first_person == false, "first person: a camera pulled back past FIRST_PERSON_LEAVE is third person again")
end
do
    -- the whole frame, Mecha Chameleon's pawn: GetVelocity says 0 while he walks 300 cm/s for 6 s - she keeps up, at
    -- his pace, never teleported
    local hers, speeds = {}, {}
    local _, world = run_frames({}, 420, function(w)
        w.on_frame = function(i)
            local x = 1000 + (i > 60 and 300 * (i - 60) / 60 or 0)
            w.pawn_loc = { X = x, Y = 2000, Z = 188 }
            w.pawn_vel = { X = 0, Y = 0, Z = 0 }
            w.cam_loc = { X = x - 300, Y = 1800, Z = 260 }
        end
    end, hers)
    local last = hers[#hers]
    local gap = math.sqrt((last[1] - 28) ^ 2 + (last[2] - 20) ^ 2) * 100 -- him: 1000 + 300 * 6 = 2800 cm, 2000 cm
    check(#hers >= 420 and gap < 400, string.format("a pawn whose GetVelocity says 0: she keeps up with him (%.0f cm away)", gap))
end
do
    -- first person (the camera at his head) - her spot is FIRST_PERSON_SCALE further than in third person
    local spot = math.sqrt(M.BACK ^ 2 + M.SIDE ^ 2)
    local function placed_at(cam)
        local hers = {}
        run_frames({}, 3, function(w)
            w.cam_loc = cam
            w.cam_rot = { Pitch = 0, Yaw = 90, Roll = 0 }
        end, hers)
        return math.sqrt((hers[#hers][1] - 10) ^ 2 + (hers[#hers][2] - 20) ^ 2) * 100
    end
    local fp, tp = placed_at({ X = 1000, Y = 2000, Z = 238 }), placed_at({ X = 1000, Y = 1700, Z = 300 })
    check(near(fp, spot * M.FIRST_PERSON_SCALE, 2), string.format("first person: her spot is further (%.0f cm, want %.0f)", fp, spot * M.FIRST_PERSON_SCALE))
    check(near(tp, spot, 2), string.format("third person: her spot is unchanged (%.0f cm, want %.0f)", tp, spot))
end

-------------------------------------------------------------------------------- the near plane: the camera's, else the engine's
do
    -- Mecha Chameleon's case: no per-camera near plane (POV -1), the engine's is 1 cm - the camera line carries 0.01 m
    local f = run_frames({ pov_near = -1, engine_near = 1 }, 5)
    check(f[15] ~= nil and near(tonumber(f[15]), 0.01, 1e-6), "near plane: no camera's own - the engine's NearClipPlane (" .. tostring(f[15]) .. " m)")
    -- a camera with its own near plane keeps it
    local f2 = run_frames({ pov_near = 25, engine_near = 1 }, 5)
    check(near(tonumber(f2[15]), 0.25, 1e-6), "near plane: a camera's own wins (" .. tostring(f2[15]) .. " m)")
    -- neither: 0 (the add-on's 10 cm)
    local f3 = run_frames({ pov_near = -1 }, 5)
    check(near(tonumber(f3[15]), 0, 1e-9), "near plane: none known - 0, the add-on assumes 10 cm (" .. tostring(f3[15]) .. ")")
end

-------------------------------------------------------------------------------- standing: in first person she faces him, not his view
do
    -- first person (camera at his head): he stands and looks round 180 degrees - she does not spin with his view; she
    -- faces him. Third person: she still turns the way he looks (his body does not turn with a third-person camera).
    local function stand_and_look(camera_dist)
        local her, p = { placed = false, x = 0, y = 0, z = 0, yaw = 0 }, { x = 0, y = 0, z = 0, yaw = 0, vx = 0, vy = 0, falling = false, camera_dist = camera_dist }
        M.follow(her, p, 1 / 60)
        for _ = 1, 180 do M.follow(her, p, 1 / 60) end
        local before = her.yaw
        for i = 1, 120 do
            p.yaw = 1.5 * i
            M.follow(her, p, 1 / 60)
        end
        for _ = 1, 120 do M.follow(her, p, 1 / 60) end
        local to_him = math.deg(math.atan(p.y - her.y, p.x - her.x))
        return before, her.yaw, to_him
    end
    local before, after, to_him = stand_and_look(30)
    local function diff(a, b) return math.abs((a - b + 540) % 360 - 180) end
    check(diff(after, to_him) < 2 and diff(after, before) < 2, string.format("first person: looking round, she faces him and does not spin (yaw %.0f -> %.0f, him at %.0f)", before, after, to_him))
    local _, after3 = stand_and_look(300)
    check(diff(after3, 180) < 2, string.format("third person: standing, she still turns the way he looks (yaw %.0f)", after3))
end

-------------------------------------------------------------------------------- collision-aware walking + the ground fix
-- A pure-Lua stand-in for a capsule sweep against axis-aligned boxes in the XY plane (Z ignored - exercises
-- M.walk_collide/M.follow's logic without a game; the integration tests further down use the REAL stub's
-- CapsuleTraceSingle, which does the identical segment-vs-box test). boxes: {x1, y1, x2, y2}. Returns
-- (blocked, time) - time (0..1, the nearest hit) is read by fix #3's placement sweep (sweep_place); every
-- existing caller only reads the first value, so this is unchanged for them.
local function box_sweep(boxes, radius)
    radius = radius or 0
    return function(x1, y1, x2, y2)
        local best_t
        for _, b in ipairs(boxes) do
            local bx1, by1, bx2, by2 = b[1] - radius, b[2] - radius, b[3] + radius, b[4] + radius
            local dx, dy = x2 - x1, y2 - y1
            local tmin, tmax = 0, 1
            local function clip(p, q)
                if p == 0 then
                    if q < 0 then return false end
                elseif p < 0 then
                    local r = q / p
                    if r > tmax then return false end
                    if r > tmin then tmin = r end
                else
                    local r = q / p
                    if r < tmin then return false end
                    if r < tmax then tmax = r end
                end
                return true
            end
            if clip(-dx, x1 - bx1) and clip(dx, bx2 - x1) and clip(-dy, y1 - by1) and clip(dy, by2 - y1) and tmin <= tmax then
                if best_t == nil or tmin < best_t then best_t = tmin end
            end
        end
        if best_t ~= nil then return true, best_t end
        return false
    end
end

do
    -- fail-safe (A): `sweep == nil` and `nav == nil` (the call is missing, or was withdrawn after it threw) is
    -- BYTE FOR BYTE today's behaviour, not just "close enough" - every M.follow line the new parameters touch
    -- is skipped entirely. Driven with a player who moves, turns, stops and starts over 400 random frames.
    math.randomseed(20261020)
    for trial = 1, 5 do
        local her1, her2 = fresh_her(), fresh_her()
        local p = { x = 0, y = 0, z = 0, yaw = 0, vx = 0, vy = 0, falling = false }
        local now, diverged = 0, false
        for i = 1, 400 do
            p.x, p.y = p.x + (math.random() - 0.5) * 20, p.y + (math.random() - 0.5) * 20
            p.yaw = p.yaw + (math.random() - 0.5) * 10
            p.vx, p.vy = (math.random() - 0.5) * 300, (math.random() - 0.5) * 300
            now = now + 1 / 60
            M.follow(her1, p, 1 / 60, now)
            M.follow(her2, p, 1 / 60, now, nil, nil, nil)
            if her1.x ~= her2.x or her1.y ~= her2.y or her1.z ~= her2.z or her1.yaw ~= her2.yaw then diverged = true end
        end
        check(not diverged, string.format("fail-safe: sweep/nav omitted vs explicitly nil are byte-identical (trial %d)", trial))
    end
end

do
    -- the ground fix (B): an isolated prop (a crate/table/railing) near the player, who stays on the real
    -- floor - she must not end up standing ON it (the owner, 2026-10-20: "sometimes stays on them"). Modelled
    -- as a solid with a top face and NO bottom face registered (world_with's "s[4] = nil": nothing reads back
    -- as a ceiling above it, exactly the isolated-prop case M.OBJECT_BAND targets - see update_ground's doc).
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
    local crate = world_with({ { 150, 250, 75, nil } }) -- a 75 cm crate spanning x in [150, 250]
    local her, p, now, climbed = fresh_her(), { x = 0, y = 0, z = 0, yaw = 0, vx = 120, vy = 0, falling = false }, 0, false
    M.follow(her, p, 1 / 60, now, crate)
    for _ = 1, 600 do
        p.x = p.x + 2
        now = now + 1 / 60
        M.follow(her, p, 1 / 60, now, crate)
        if her.x >= 150 and her.x <= 250 and her.z > 10 then climbed = true end
    end
    check(not climbed and near(her.z, 0, 0.5), string.format("the ground fix: she never stands on a crate near the player's own floor (z %.1f)", her.z))
    -- the SAME fix on the teleport path (force = true): a teleport destination landing over the crate's
    -- footprint is accepted completely unconditionally before this fix - it must land on the real floor instead.
    local her2 = fresh_her()
    local p2 = { x = 320, y = 0, z = 0, yaw = 0, vx = 0, vy = 0, falling = false } -- her spot (p.x - BACK = 180) lands inside [150,250]
    M.follow(her2, p2, 1 / 60, 0, crate)
    check(near(her2.z, 0, 0.5) and (her2.trace.objects or 0) > 0,
        string.format("the ground fix: a teleport landing over the crate lands on the floor, not on it (z %.1f, objects %s)",
            her2.z, tostring(her2.trace.objects)))
end

do
    -- collision (C): a wall with a gap, offset from her natural walking line - she must slide to find it. Ported
    -- in spirit from the Death Stranding follower's try_step/slide (astra.cpp:580-607): never straight through
    -- the wall, never stuck forever either (the gap IS reachable).
    local sweep = box_sweep({ { -20, -1000, 20, 150 }, { -20, 250, 20, 1000 } }, M.SWEEP_RADIUS) -- gap in y: [150, 250]
    local her, p, now, entered_wall = fresh_her(), { x = -400, y = 0, z = 0, yaw = 0, vx = 300, vy = 0, falling = false }, 0, false
    M.follow(her, p, 1 / 60, now, nil, sweep) -- her natural offset (y ~= 90) sits inside the FIRST wall segment
    for _ = 1, 900 do
        p.x = math.min(p.x + 4, 400)
        p.vx = (p.x < 400) and 300 or 0
        now = now + 1 / 60
        M.follow(her, p, 1 / 60, now, nil, sweep)
        if her.x > -20 and her.x < 20 and (her.y < 150 or her.y > 250) then entered_wall = true end
    end
    check(not entered_wall, "collision: a wall with a gap - she never enters the solid part of it")
    check(her.stuck_teleports == nil, string.format("collision: a wall with a gap - she reaches it without needing the stuck escape (her %.0f %.0f)", her.x, her.y))
end

do
    -- collision + stuck (D): a SEALED wall (no gap) while the player keeps walking away - the TELEPORT distance
    -- threshold (2500 cm) would eventually rescue her anyway, but the stuck detector (astra.cpp:494-516, ported)
    -- must do it much sooner, and she must never have entered the wall first.
    local sweep = box_sweep({ { -20, -1000, 20, 1000 } }, M.SWEEP_RADIUS)
    local her, p, now, entered_wall = fresh_her(), { x = -500, y = 0, z = 0, yaw = 0, vx = 300, vy = 0, falling = false }, 0, false
    M.follow(her, p, 1 / 60, now, nil, sweep)
    local rescued_frame
    for i = 1, 600 do
        p.x = p.x + 5
        now = now + 1 / 60
        M.follow(her, p, 1 / 60, now, nil, sweep)
        if her.x > -20 and her.x < 20 then entered_wall = true end
        if (her.stuck_teleports or 0) > 0 and rescued_frame == nil then rescued_frame = i end
    end
    check(not entered_wall, "collision + stuck: a sealed wall, player walking away - she never enters it")
    check(rescued_frame ~= nil and rescued_frame / 60 < M.TELEPORT / 300,
        string.format("collision + stuck: rescued well before the raw TELEPORT distance would have caught it (%s frames)", tostring(rescued_frame)))
end

do
    -- collision + stuck (E): a SEALED wall with the PLAYER STANDING STILL the whole time - raw distance
    -- (TELEPORT) never grows at all here, so only the quieter aim-giveup-escalation path (M.STUCK_AIM_DROPS)
    -- can rescue her; she must still never enter the wall, and must eventually be freed, not loop forever.
    local sweep = box_sweep({ { -20, -1000, 20, 1000 } }, M.SWEEP_RADIUS)
    local her, p, now, entered_wall = fresh_her(), { x = -500, y = 0, z = 0, yaw = 0, vx = 0, vy = 0, falling = false }, 0, false
    M.follow(her, p, 1 / 60, now, nil, sweep)
    for i = 1, 160 do -- he walks to the far side of the wall (x = 300, well past the [-20, 20] band), then stops
        p.x = math.min(p.x + 5, 300)
        p.vx = (p.x < 300) and 300 or 0
        now = now + 1 / 60
        M.follow(her, p, 1 / 60, now, nil, sweep)
    end
    local rescued_frame
    for i = 1, 900 do -- up to 15 s
        now = now + 1 / 60
        M.follow(her, p, 1 / 60, now, nil, sweep)
        if her.x > -20 and her.x < 20 then entered_wall = true end
        if (her.stuck_teleports or 0) > 0 and rescued_frame == nil then rescued_frame = i end
    end
    check(not entered_wall, "collision + stuck: a sealed wall, player standing still - she never enters it")
    check(rescued_frame ~= nil and rescued_frame < 900, string.format("collision + stuck: a stationary player still frees her, not an infinite loop (%s frames)", tostring(rescued_frame)))
end

do
    -- collision (F): never pushed into the player while sliding - a wall channels her toward him.
    local sweep = box_sweep({ { -20, -1000, 20, 1000 }, { 20, -1000, 300, -50 } }, M.SWEEP_RADIUS)
    local her, p, now, closest = fresh_her(), { x = -400, y = 0, z = 0, yaw = 0, vx = 300, vy = 0, falling = false }, 0, 1e9
    M.follow(her, p, 1 / 60, now, nil, sweep)
    for _ = 1, 600 do
        p.x = math.min(p.x + 4, 100)
        now = now + 1 / 60
        M.follow(her, p, 1 / 60, now, nil, sweep)
        closest = math.min(closest, math.sqrt((her.x - p.x) ^ 2 + (her.y - p.y) ^ 2))
    end
    check(closest > M.NEAR_PLAYER, string.format("collision: sliding along a channelling wall never pushes her into the player (closest %.0f cm)", closest))
end

do
    -- integration (G): the REAL stub's CapsuleTraceSingle against w.boxes, through the FULL game
    -- glue (tick()'s own kismet call shape, not a hand-rolled sweep) - a wall box sits across her usual
    -- walking offset the whole way; run_frames drives the whole tick(), not just M.follow.
    local _, world = run_frames({}, 480, function(w)
        w.ground = 0
        w.boxes = { { -1000, 50, 1000, 300 } } -- walls off her usual offset band (y in [50, 300]) the whole corridor
        w.pawn_loc = { X = -400, Y = 0, Z = 88 }
        w.on_frame = function(i)
            w.pawn_loc.X = math.min(w.pawn_loc.X + 3, 400)
            w.pawn_vel = { X = (w.pawn_loc.X < 400) and 180 or 0, Y = 0, Z = 0 }
        end
    end)
    check((world.capsule_calls or 0) > 0, string.format("integration: CapsuleTraceSingle is actually called through the real glue (%s calls)", tostring(world.capsule_calls)))
end

do
    -- integration (H), fail-safe: the stub's capsule sweep THROWS (a UE4SS build without this binding) - she
    -- must end up exactly where the SAME run with no wall at all would put her (today's behaviour resumes).
    local f1, world1 = run_frames({}, 200, function(w)
        w.ground = 0
        w.capsule_error = true
        w.boxes = { { -1000, 50, 1000, 300 } }
        w.pawn_loc = { X = -400, Y = 0, Z = 88 }
        w.on_frame = function(i) w.pawn_loc.X = w.pawn_loc.X + 3 w.pawn_vel = { X = 180, Y = 0, Z = 0 } end
    end)
    local f2, world2 = run_frames({}, 200, function(w)
        w.ground = 0 -- no boxes at all: today, unmodified
        w.pawn_loc = { X = -400, Y = 0, Z = 88 }
        w.on_frame = function(i) w.pawn_loc.X = w.pawn_loc.X + 3 w.pawn_vel = { X = 180, Y = 0, Z = 0 } end
    end)
    check((world1.capsule_calls or 0) > 0, "integration fail-safe: the throwing call is actually attempted (not skipped)")
    local said = false
    -- (logs are not captured here without `hers`/a logs sink in this call form; the throw's own effect - matching
    -- the no-wall run exactly - is the behavioural proof that today's behaviour resumed, see check below)
    check(f1[1] == f2[1] and f1[2] == f2[2], "integration fail-safe: a throwing capsule sweep ends up exactly where no sweep at all would (camera line matches)")
end

do
    -- integration (I), the nav stub: FindPathToLocationSynchronously is asked no more than once per NAV_EVERY
    -- (0.5 s) - never per frame - and a missing/invalid path changes nothing (today's straight-line goal).
    local _, world = run_frames({}, 120, function(w) -- 2 s at 60 fps: at most 4-5 queries, never ~120
        w.ground = 0
        w.nav_points = { { X = 100, Y = 0, Z = 0 }, { X = 200, Y = 0, Z = 0 } }
        w.pawn_loc = { X = -400, Y = 0, Z = 88 }
        w.on_frame = function(i) w.pawn_loc.X = w.pawn_loc.X + 3 w.pawn_vel = { X = 180, Y = 0, Z = 0 } end
    end)
    check((world.nav_calls or 0) >= 1 and (world.nav_calls or 0) <= 6,
        string.format("integration: the navmesh query is rate-limited to about every %.1f s, never per frame (%s calls in 2 s)", M.NAV_EVERY, tostring(world.nav_calls)))
    local _, world2 = run_frames({}, 60, function(w)
        w.ground = 0
        w.nav_error = true
        w.pawn_loc = { X = -400, Y = 0, Z = 88 }
        w.on_frame = function(i) w.pawn_loc.X = w.pawn_loc.X + 3 w.pawn_vel = { X = 180, Y = 0, Z = 0 } end
    end)
    check((world2.nav_calls or 0) == 0, "integration: a throwing navmesh lookup is asked once, then withdrawn for the session")
end

do
    -- fix #1 (J): the sweep is a TRACE CHANNEL query (CapsuleTraceSingle, M.TRACE_CHANNEL), never an object-type
    -- one (CapsuleTraceSingleForObjects) - RE-UE4SS#378's silently-empty ObjectTypes array, and an object-type
    -- query hitting Trigger volumes a channel trace does not (both named in main.lua's own comment). The player
    -- himself is the WorldContextObject with bIgnoreSelf = true, and ActorsToIgnore is empty - never a reliance
    -- on naming him there instead.
    local _, world = run_frames({}, 60, function(w)
        w.ground = 0
        w.boxes = { { -1000, 50, 1000, 300 } }
        w.pawn_loc = { X = -400, Y = 0, Z = 88 }
        w.on_frame = function(i) w.pawn_loc.X = w.pawn_loc.X + 3 w.pawn_vel = { X = 180, Y = 0, Z = 0 } end
    end)
    check((world.capsule_calls or 0) > 0, "fix #1: the capsule sweep is actually exercised")
    local a = world.capsule_args or {}
    check(a.context ~= nil and a.context.IsValid ~= nil, "fix #1: the sweep's WorldContextObject is the followed actor (the pawn/view target), not nil")
    check(a.channel == M.TRACE_CHANNEL, string.format("fix #1: the sweep is a TRACE CHANNEL query, M.TRACE_CHANNEL (got %s)", tostring(a.channel)))
    check(type(a.ignore) == "table" and #a.ignore == 0, string.format("fix #1: ActorsToIgnore is empty - bIgnoreSelf keeps the player out, not this array (got %s entries)", tostring(a.ignore and #a.ignore)))
    check(a.ignore_self == true, "fix #1: bIgnoreSelf is true")
end

do
    -- fix #2 (K): step allowance - a riser no taller than M.STEP_UP (20 cm, three of them: "stairs") never
    -- blocks her sweep; a 1 m box in the same spot still does. w.boxes' 5th field is the box's world-Z top
    -- (the stub's Z-aware CapsuleTraceSingle, see make_world's own comment); her feet stay flat at 0 throughout
    -- (w.ground = 0) - this is purely the horizontal sweep's own tolerance, independent of the ground ray.
    local function entered(hers, bx1, by1, bx2, by2)
        for _, h in ipairs(hers) do
            local x, y = h[1] * 100, h[2] * 100 -- glTF metres -> UE cm
            if x >= bx1 and x <= bx2 and y >= by1 and y <= by2 then return true end
        end
        return false
    end
    local steps = { { 0, -250, 40, -30, 20 }, { 200, -250, 240, -30, 20 }, { 400, -250, 440, -30, 20 } }
    local hers = {}
    local _, world = run_frames({}, 400, function(w)
        w.ground = 0
        w.boxes = steps
        w.pawn_loc = { X = -400, Y = 0, Z = 88 }
        w.on_frame = function(i)
            w.pawn_loc.X = math.min(w.pawn_loc.X + 4, 900)
            w.pawn_vel = { X = (w.pawn_loc.X < 900) and 240 or 0, Y = 0, Z = 0 }
        end
    end, hers)
    check((world.capsule_calls or 0) > 0, "fix #2: the capsule sweep is actually exercised")
    for i, b in ipairs(steps) do
        check(entered(hers, b[1], b[2], b[3], b[4]), string.format("fix #2: a %d cm riser (step %d) never blocks her sweep", b[5], i))
    end
    local hers2 = {}
    run_frames({}, 400, function(w)
        w.ground = 0
        w.boxes = { { 0, -250, 40, -30, 100 } } -- same footprint, 1 m tall
        w.pawn_loc = { X = -400, Y = 0, Z = 88 }
        w.on_frame = function(i)
            w.pawn_loc.X = math.min(w.pawn_loc.X + 4, 900)
            w.pawn_vel = { X = (w.pawn_loc.X < 900) and 240 or 0, Y = 0, Z = 0 }
        end
    end, hers2)
    check(not entered(hers2, 0, -250, 40, -30), "fix #2: the same footprint, 1 m tall, still blocks her sweep")
end

do
    -- fix #3 (L): placement never lands her inside a wall. A wall sits squarely between the player and her
    -- spot (both her spot's line AND its mirror cross it, by construction) - she must end up on HIS side of
    -- it, never inside or past it, on first placement (s.placed == false, exactly the branch this fix changes).
    local sweep = box_sweep({ { -90, -1000, -70, 1000 } }, M.SWEEP_RADIUS)
    local her, p = fresh_her(), { x = 0, y = 0, z = 0, yaw = 0, vx = 0, vy = 0, falling = false }
    M.follow(her, p, 1 / 60, 0, nil, sweep)
    check(her.x > -70, string.format("fix #3: a wall between him and her spot - she lands on his side of it (x %.1f)", her.x))
    check(not (her.x >= -90 and her.x <= -70), string.format("fix #3: never placed inside the wall itself (x %.1f)", her.x))
    check(math.sqrt((her.x - p.x) ^ 2 + (her.y - p.y) ^ 2) >= M.NEAR_PLAYER, string.format("fix #3: still never closer to him than NEAR_PLAYER (dist %.1f)", math.sqrt((her.x - p.x) ^ 2 + (her.y - p.y) ^ 2)))

    -- without a sweep, today's behaviour: she still lands exactly at her spot, wall or not (no regression).
    local her2 = fresh_her()
    M.follow(her2, p, 1 / 60, 0, nil, nil)
    check(near(her2.x, -140) and near(her2.y, 90), string.format("fix #3: without a sweep, today's behaviour - she lands exactly at her spot (x %.1f, y %.1f)", her2.x, her2.y))
end

do
    -- review fix (M): try_move's near-player veto must reject only SLIDE candidates, never the plain direct
    -- move - with no obstacle at all, walking near him (45 cm, well inside NEAR_PLAYER=90) while he moves away
    -- must never count as "blocked" or halve her pace, just because the veto's own clear_r check fired on a
    -- step that had nothing to do with an obstacle.
    local her = fresh_her()
    her.placed, her.x, her.y, her.z, her.yaw, her.v = true, 45, 0, 0, 180, 0
    local p = { x = 0, y = 0, z = 0, yaw = 0, vx = 300, vy = 0, falling = false }
    local sweep = function() return false end -- never blocks: there is no obstacle in this test
    local now = 0
    for _ = 1, 90 do
        p.x = p.x + 5
        now = now + 1 / 60
        M.follow(her, p, 1 / 60, now, nil, sweep)
    end
    check((her.blocked_frames or 0) == 0, string.format("review fix: no obstacle - a direct move near him is never read as blocked (blocked_frames %s)", tostring(her.blocked_frames)))
    check(near(her.v, 300, 1), string.format("review fix: no obstacle - her pace is never halved just for being near him (v %.1f)", her.v))
end

do
    -- review fix (N): the stuck detector accumulates only on frames a sweep actually reported a block, never
    -- on raw distance from the spot alone - (a) an open-field sprint, sweep == nil, must be BYTE-IDENTICAL to
    -- today (never stuck: there is nothing to block her) even though she plateaus at her own speed cap and the
    -- gap to a 900 cm/s player never closes.
    local her = fresh_her()
    local p = { x = 0, y = 0, z = 0, yaw = 0, vx = 900, vy = 0, falling = false }
    local now = 0
    M.follow(her, p, 1 / 60, now, nil, nil)
    for _ = 1, 600 do -- 10 s
        p.x = p.x + 900 / 60
        now = now + 1 / 60
        M.follow(her, p, 1 / 60, now, nil, nil)
    end
    check((her.stuck_teleports or 0) == 0, string.format("review fix: a 10 s open-field sprint with no sweep is never read as stuck (stuck_teleports %s)", tostring(her.stuck_teleports)))
    -- the existing fail-safe test (sweep/nav omitted vs explicitly nil are byte-identical) already covers the
    -- "byte-identical to today" half of this at the top of the file; this one adds the SUSTAINED-sprint shape
    -- the review flagged specifically (a plateaued catch-up gap, not just 400 random frames).

    -- (b) wedged against a sealed wall, her spot ~200 cm past it, while he paces nearby (oscillating but always
    -- "going") - she must never enter the wall, and must be rescued within STUCK_SECONDS + 1 s of the pacing
    -- (i.e. the steady-state wedge) starting, not left stuck indefinitely by a reported-block flicker right at
    -- a slide's own corner case resetting her progress baseline.
    local sweep = box_sweep({ { -20, -1000, 20, 1000 } }, M.SWEEP_RADIUS)
    local her2, p2, now2, entered_wall = fresh_her(), { x = -500, y = 0, z = 0, yaw = 0, vx = 300, vy = 0, falling = false }, 0, false
    M.follow(her2, p2, 1 / 60, now2, nil, sweep)
    for _ = 1, 150 do -- he walks up to ~200 cm past the wall (her spot ends up there, her wedged on this side)
        p2.x = math.min(p2.x + 5, 286)
        now2 = now2 + 1 / 60
        M.follow(her2, p2, 1 / 60, now2, nil, sweep)
        if her2.x > -20 and her2.x < 20 then entered_wall = true end
    end
    local rescued_frame
    for i = 1, 300 do -- he paces near there, still registering as "going" throughout
        p2.x = 286 + 10 * math.sin(i * 0.3)
        p2.vx = 300 * ((i % 2 == 0) and 1 or -1)
        now2 = now2 + 1 / 60
        M.follow(her2, p2, 1 / 60, now2, nil, sweep)
        if her2.x > -20 and her2.x < 20 then entered_wall = true end
        if (her2.stuck_teleports or 0) > 0 and rescued_frame == nil then rescued_frame = i end
    end
    check(not entered_wall, "review fix: wedged near her spot while he paces - she never enters the wall")
    check(rescued_frame ~= nil and rescued_frame / 60 < M.STUCK_SECONDS + 1, string.format("review fix: wedged near her spot while he paces - rescued within STUCK_SECONDS + 1 s (%s frames)", tostring(rescued_frame)))
end

do
    -- review fix (O): placement's last-resort "beside him" fallback was unswept and could land her inside a
    -- wall in a corridor narrower than about her spot's own lateral reach (M.SIDE + M.SWEEP_RADIUS, ~134 cm).
    -- The player is centred in a 120 cm corridor (walls at |y| >= 60); her real spot and its mirror both reach
    -- for |y| = 90 cm (past the walls) and fail, and so do the new right-/left-side candidates (same lateral
    -- reach) - every swept candidate that reaches sideways fails, same as the old single fallback did; she
    -- must still never land inside either wall box.
    local walls = { { -100000, 60, 100000, 100000 }, { -100000, -100000, 100000, -60 } }
    local sweep = box_sweep(walls, M.SWEEP_RADIUS)
    local her = fresh_her()
    local p = { x = 0, y = 0, z = 0, yaw = 0, vx = 0, vy = 0, falling = false }
    M.follow(her, p, 1 / 60, 0, nil, sweep)
    local inside = her.y >= 60 - M.SWEEP_RADIUS or her.y <= -60 + M.SWEEP_RADIUS
    check(not inside, string.format("review fix: a 120 cm corridor, both swept side spots fail - she never lands inside a wall box (y %.1f)", her.y))
end

do
    -- review fix (P): s.aim_drops must not accumulate across UNRELATED stand-mode aim failures - a new aim
    -- more than M.AIM_DROP_DIST from the last dropped one, or more than M.AIM_DROP_GAP seconds after it,
    -- resets the count first. Here the player never moves (so every dropped aim is at the SAME spot - only
    -- the 20 s real-time gap between episodes makes each one "unrelated"); 5 such drops, 20 s apart, must
    -- never escalate to a teleport (more than enough to cross M.STUCK_AIM_DROPS if they wrongly accumulated).
    local sweep = function() return true, 0 end -- always blocked: her stand-mode aim can never be reached
    local her, p, now = fresh_her(), { x = 0, y = 0, z = 0, yaw = 0, vx = 0, vy = 0, falling = false }, 0
    M.follow(her, p, 1 / 60, now, nil, sweep) -- placed (last resort: his own feet, since every sweep call blocks)
    local drops_seen = 0
    for _ = 1, 5 do
        for _ = 1, 200 do
            now = now + 1 / 60
            local before = her.aim_drops or 0
            M.follow(her, p, 1 / 60, now, nil, sweep)
            if (her.aim_drops or 0) > before then
                drops_seen = drops_seen + 1
                break
            end
        end
        now = now + 20 -- a 20 s idle gap before the next, unrelated episode
    end
    check(drops_seen == 5, string.format("review fix: five unrelated drops actually happened (saw %d)", drops_seen))
    check((her.stuck_teleports or 0) == 0, string.format("review fix: unrelated drops, 20 s apart, never escalate to a teleport (stuck_teleports %s)", tostring(her.stuck_teleports)))
end

do
    -- review fix (Q): M.OBJECT_BAND's retrace (from z - 1 down) must only REPLACE z when it actually finds
    -- something lower (a crate over a floor) - finding NOTHING there is not "no floor": on a continuous
    -- slope, the surface already found IS the only floor there is, and discarding it (the old code) sank her
    -- through her own real floor the moment she stood more than OBJECT_BAND above the player's own feet on
    -- one. A plain slope, height 0.6 * y: her natural spot's y is 90 (M.SIDE, p.yaw = 0, p.y = 0) - 54 cm up,
    -- comfortably past M.OBJECT_BAND (50) above the player's floor (0).
    local function slope(x, y, from, to)
        local h = 0.6 * y
        if from >= to then
            if h <= from and h >= to then return h end
        else
            if h >= from and h <= to then return h end
        end
        return nil
    end
    local her = fresh_her()
    local p = { x = 0, y = 0, z = 0, yaw = 0, vx = 120, vy = 0, falling = false }
    M.follow(her, p, 1 / 60, 0, slope)
    check(near(her.z, 54, 1), string.format("OBJECT_BAND fix: placed on a slope, she stands on it, not on the player's floor (z %.1f)", her.z))
    local now = 0
    for _ = 1, 300 do
        p.x = p.x + 2
        now = now + 1 / 60
        M.follow(her, p, 1 / 60, now, slope)
    end
    check(near(her.z, 54, 1), string.format("OBJECT_BAND fix: walking on a slope, she stays on it, not on the player's floor (z %.1f)", her.z))
    -- the crate case (a solid WITH a bottom face registered separately, so the retrace DOES find the floor
    -- under it) must still give 0 - the "the ground fix (B)" tests above already cover this unchanged; this
    -- is the same assertion, inline, as the two-sided confirmation this fix's own test asks for.
    local function world_with(solids)
        return function(x, _, from, to)
            local best
            if from >= to then
                if from >= 0 and to <= 0 then best = 0 end
                for _, s in ipairs(solids) do
                    if x >= s[1] and x <= s[2] and s[3] <= from and s[3] >= to and (best == nil or s[3] > best) then best = s[3] end
                end
            end
            return best
        end
    end
    local crate = world_with({ { 150, 250, 75 } })
    local her2 = fresh_her()
    local p2 = { x = 0, y = 0, z = 0, yaw = 0, vx = 120, vy = 0, falling = false }
    M.follow(her2, p2, 1 / 60, 0, crate)
    for _ = 1, 300 do
        p2.x = p2.x + 2
        M.follow(her2, p2, 1 / 60, _ / 60, crate)
    end
    check(near(her2.z, 0, 0.5), string.format("OBJECT_BAND fix: the crate case still gives 0 (z %.1f)", her2.z))
end

io.stdout:write(string.format("AstraUE: %d checks, %d failed\n", checks, failed))
os.exit(failed == 0 and 0 or 1)
