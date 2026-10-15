-- AstraUETest: a live-test driver for the Unreal runner - NOT shipped (tools/ue-test-driver/, docs/LIVE-TEST.md).
-- It turns the camera, moves the player and pauses the game the way a player would, from commands written into a
-- file, so the live test's step 3 runs without hands on the keyboard - and in games whose own camera never turns.
-- Generic Unreal API only: the PlayerController (its view target, its control rotation), the Pawn, GameplayStatics.
--
--   printf "swing 147 -8 60 1 8\n" > "<exe dir>/ue4ss/Mods/AstraUETest/cmd.txt"     (read once, then removed)
--
--   look <yaw> <pitch>                    view from the player's pawn (its eyes, or its own camera component), fixed
--   swing <yaw> <pitch> <deg> <period> <s>  the same view, yaw swinging +-deg around <yaw> (peak deg*2pi/period deg/s)
--   spin <yaw> <pitch> <deg/s> <s>         the same view turning at a steady rate (one direction: a known speed)
--   walk <dx> <dy> <cm/s>                 the pawn moves by (dx, dy) cm at that speed (placed along the way)
--   pause <0|1>                           GameplayStatics.SetGamePaused
--   restore                               the game's own view target back, swing and walk stopped
--   state                                 a log line: view target, camera, pawn
--   quit                                  KismetSystemLibrary.QuitGame: the game quits as from its own menu

local UEHelpers = require("UEHelpers")
-- beside this mod's Scripts folder (the process's working directory is the game's, not UE4SS's)
local CMD = (debug.getinfo(1, "S").source:match("^@(.*)[/\\]Scripts[/\\]main%.lua$") or "Mods/AstraUETest") .. "/cmd.txt"

local own_view = nil -- the game's view target, given back by `restore`
local swing = nil    -- {yaw, pitch, deg, period, t0, t1}
local walk = nil     -- {x0, y0, z, dx, dy, t0, t1}
local frames, errors = 0, 0

local function log(fmt, ...)
    print(string.format("[AstraUETest] " .. fmt .. "\n", ...))
end

local function pc_and_pawn()
    local pc = UEHelpers.GetPlayerController()
    if pc == nil or not pc:IsValid() then return nil, nil end
    local pawn = pc.Pawn
    if pawn == nil or not pawn:IsValid() then return pc, nil end
    return pc, pawn
end

local function view_from_pawn(pc, pawn)
    local vt = pc:GetViewTarget()
    if vt ~= nil and vt:IsValid() and vt:GetFullName() ~= pawn:GetFullName() then
        own_view = vt
        pc:SetViewTargetWithBlend(pawn, 0.0, 0, 0.0, false)
        log("view target: the pawn (was %s)", vt:GetFullName())
    end
end

local function set_view(pc, yaw, pitch)
    pc:SetControlRotation({ Pitch = pitch, Yaw = yaw, Roll = 0.0 })
end

local function state(pc, pawn)
    local pcm = pc.PlayerCameraManager
    local loc, rot = pcm:GetCameraLocation(), pcm:GetCameraRotation()
    local at = pawn ~= nil and pawn:K2_GetActorLocation() or { X = 0, Y = 0, Z = 0 }
    local vt = pc:GetViewTarget()
    log("view %s  camera %.0f %.0f %.0f  yaw %.1f pitch %.1f  fov %.1f  pawn %.0f %.0f %.0f  paused %s",
        vt ~= nil and vt:IsValid() and vt:GetFullName() or "?", loc.X, loc.Y, loc.Z, rot.Yaw, rot.Pitch, pcm:GetFOVAngle(),
        at.X, at.Y, at.Z, tostring(UEHelpers.GetGameplayStatics():IsGamePaused(pc)))
end

local function command(line, pc, pawn)
    local words = {}
    for w in line:gmatch("%S+") do words[#words + 1] = w end
    local verb, n = words[1], function(i) return tonumber(words[i]) or 0 end
    local now = os.clock()
    if verb == "look" and pawn then
        swing = nil
        view_from_pawn(pc, pawn)
        set_view(pc, n(2), n(3))
    elseif verb == "swing" and pawn then
        view_from_pawn(pc, pawn)
        swing = { yaw = n(2), pitch = n(3), deg = n(4), period = math.max(n(5), 0.1), t0 = now, t1 = now + n(6) }
    elseif verb == "spin" and pawn then
        view_from_pawn(pc, pawn)
        swing = { yaw = n(2), pitch = n(3), rate = n(4), t0 = now, t1 = now + n(5) }
    elseif verb == "walk" and pawn then
        local at = pawn:K2_GetActorLocation()
        walk = { x0 = at.X, y0 = at.Y, z = at.Z, dx = n(2), dy = n(3), t0 = now,
            t1 = now + math.sqrt(n(2) ^ 2 + n(3) ^ 2) / math.max(n(4), 1) }
    elseif verb == "pause" then
        UEHelpers.GetGameplayStatics():SetGamePaused(pc, n(2) ~= 0)
    elseif verb == "restore" then
        swing, walk = nil, nil
        if own_view ~= nil and own_view:IsValid() then pc:SetViewTargetWithBlend(own_view, 0.0, 0, 0.0, false) end
        own_view = nil
    elseif verb == "quit" then
        log("did: quit")
        UEHelpers.GetKismetSystemLibrary():QuitGame(pc, pc, 0, false) -- EQuitPreference::Quit
        return
    elseif verb ~= "state" then
        log("unknown or impossible now: %s", line)
        return
    end
    log("did: %s", line)
    state(pc, pawn)
end

local function tick()
    frames = frames + 1
    if frames == 1 then log("ticking") end
    local pc, pawn = pc_and_pawn()
    if pc == nil then return end
    local now = os.clock()
    if frames % 15 == 0 then
        local f = io.open(CMD, "r")
        if f then
            local text = f:read("a") or ""
            f:close()
            os.remove(CMD)
            for line in text:gmatch("[^\r\n]+") do
                local ok, err = pcall(command, line, pc, pawn)
                if not ok then log("failed: %s: %s", line, tostring(err)) end
            end
        end
    end
    if swing and pawn then
        if now > swing.t1 then
            set_view(pc, swing.yaw, swing.pitch)
            swing = nil
            log("swing done")
        else
            local turned = swing.rate and swing.rate * (now - swing.t0)
                or swing.deg * math.sin((now - swing.t0) * 2 * math.pi / swing.period)
            set_view(pc, swing.yaw + turned, swing.pitch)
        end
    end
    if walk and pawn then
        local k = math.min((now - walk.t0) / math.max(walk.t1 - walk.t0, 1e-3), 1.0)
        pawn:K2_SetActorLocation({ X = walk.x0 + walk.dx * k, Y = walk.y0 + walk.dy * k, Z = walk.z }, false, {}, true)
        if k >= 1.0 then
            walk = nil
            log("walk done")
            state(pc, pawn)
        end
    end
end

LoopInGameThreadAfterFrames(1, function()
    local ok, err = pcall(tick)
    if not ok then
        errors = errors + 1
        if errors <= 5 or errors % 600 == 0 then log("error (%d): %s", errors, tostring(err)) end
    end
end)
log("ready: write commands into %s", CMD)
