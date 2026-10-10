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

-------------------------------------------------------------------------------- a stub of UE4SS and a game
--- A fake game: a PlayerController with its camera manager and pawn, UE4SS's helpers and kismet library.
function make_world()
    local w = { frame = 1000 }
    local function object(t)
        t.IsValid = function() return true end
        t.GetFullName = function() return "BP_PlayerController_C /Game/Test" end
        return t
    end
    local kismet = object({
        GetFrameCount = function() return w.frame end,
        GetGameName = function() return { ToString = function() return "TestGame" end } end,
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
    local pc = object({ PlayerCameraManager = pcm, Pawn = pawn, Player = { AspectRatioAxisConstraint = 1 }, IsPaused = function() return false end })
    package.loaded["UEHelpers"] = nil
    package.preload["UEHelpers"] = function()
        return { GetKismetSystemLibrary = function() return kismet end, GetPlayerController = function() return pc end }
    end
    UnrealVersion = { GetMajor = function() return 5 end, GetMinor = function() return 6 end }
    EngineTickAvailable = true
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
    io.open, os.clock = real_open, real_clock

    local all = table.concat(written)
    check(all:find("I astra.unreal TestGame\n", 1, true) ~= nil, "it says who it is")
    local cams, avatars, last_c = 0, 0, nil
    for l in all:gmatch("[^\n]+") do
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
