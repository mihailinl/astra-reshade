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
function make_world()
    local w = { frame = 1000 }
    local function object(t)
        t.IsValid = function() return true end
        t.GetFullName = function() return "BP_PlayerController_C /Game/Test" end
        return t
    end
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
        pc = object({ PlayerCameraManager = pcm, Pawn = pawn, Player = { AspectRatioAxisConstraint = 1 } })
        pc.IsValid = function() return w.level == level end
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

io.stdout:write(string.format("AstraUE: %d checks, %d failed\n", checks, failed))
os.exit(failed == 0 and 0 or 1)
