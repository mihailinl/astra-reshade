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
M.CAMERA_GO_START, M.CAMERA_GO_STOP = 60.0, 30.0 -- the same edges for a speed read off the CAMERA's motion (no
                                 -- Pawn): a 300 cm boom turning at 11 deg/s moves the camera 60 cm/s with the
                                 -- player standing still (0.2.4's single edge was 30)
M.FIRST_PERSON_DIST = 100.0     -- the camera this near the player's centre (cm, times his size) is his eyes: first
                                 -- person (Poppy Playtime's eye sits ~50 cm above its capsule's centre; a third-
                                 -- person boom is 2-4 m away)...
M.FIRST_PERSON_LEAVE = 140.0    -- ...and only past this is she out of first person again (a hysteresis gap: an aim
                                 -- camera hovering at the edge must not re-pick her distance every frame)
M.FIRST_PERSON_SCALE = 1.6      -- ...and in first person her distances are this much larger: the camera IS him, so
                                 -- her whole gap is her distance from the viewer, with no boom to add to it (the
                                 -- owner, 2026-10-20: "she runs too close behind me" - 166 cm read as at his heels)
M.SETTLE = 300.0                -- cm/s^2: how gently her pace comes down to WALK_PACE when he stops going while
                                 -- she is still catching up (review r3: the step from CALM_PACE was a 50 ms jolt)
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
M.STEP_UP = 50.0                -- ground higher than this above her feet is hers only with no ceiling between (a ledge,
                                 -- not a balcony) - and (fix #2, M.SWEEP_RADIUS below) the swept capsule's own step
                                 -- allowance: a stair/kerb no taller than this never blocks her sweep either, the
                                 -- same number Unreal's own CharacterMovement sweeps a MaxStepHeight allowance with
M.OBJECT_BAND = M.STEP_UP       -- ...and even then, only if the PLAYER's own feet are within this of it too - the
                                 -- discriminator between a ledge/balcony he actually climbed (his feet are up
                                 -- there) and an isolated prop near him he is not standing on or near the height
                                 -- of (a crate, a table, a railing): she must not end up standing ON the latter
                                 -- (the owner, 2026-10-20: "sometimes stays on them"). Checked on BOTH the
                                 -- walking path and a forced teleport - a teleport landing over furniture was
                                 -- accepted completely unconditionally before this (UNVERIFIED tuning: reusing
                                 -- STEP_UP's number for symmetry, not measured against real game furniture).
M.OBJECT_CONTINUITY = 25.0      -- a ray alone cannot tell a continuous slope (keep the surface it found) from
                                 -- an isolated object sitting over a void (a crate at a pit's edge, him down
                                 -- in the pit - the SAME "nothing lower" reading as a slope, for the opposite
                                 -- reason - review finding): the ground at the MIDPOINT between her and the
                                 -- player must read within this of the midpoint of their two heights, or it
                                 -- is an object, not a slope (M.update_ground's own continuity check)
M.GROUND_KEEP = 1.0             -- a trace that misses keeps the last ground this long, then the player's
M.TRACE_CHANNEL = 0             -- ETraceTypeQuery: 0 = TraceTypeQuery1, the Visibility channel in every UE game

-- her body against the world (UE centimetres, seconds, degrees) - collision-aware walking. ALL of this is
-- reached only through the optional `sweep` function M.follow's 6th argument: nil (today, or a game where the
-- capsule sweep call threw once and it was withdrawn for the session - see the glue's `sweep_broken` latch)
-- takes the exact original unconditional-move code path, byte for byte. Ported in spirit from the Death
-- Stranding follower's try_step/slide/stuck (astra-in-death-stranding/src/astra.cpp:111-149, 580-607, 494-516),
-- adapted to AstraUE's one-call-per-engine-frame model (no DS-style fixed substep loop is needed for tunnelling
-- - a SWEEP, not a point sample, cannot miss a thin wall at any speed - COLLIDE_SUBSTEP exists only to bound
-- how far she commits before re-aiming near a corner, i.e. cost, not correctness).
M.SWEEP_RADIUS = 34.0           -- her capsule radius (cm): close to UE's default ThirdPersonCharacter (34) so a
                                 -- sweep against the SAME collision the player's own capsule would hit reads as
                                 -- "her body", not a point
M.SWEEP_BODY_HEIGHT = 150.0     -- her body height (cm) for the sweep's own half-height (fix #2, below), scaled by
                                 -- dscale like every other distance in M.follow - NOT the fixed ThirdPersonCharacter
                                 -- half-height (her VRM body isn't that pawn's capsule): max(M.SWEEP_RADIUS,
                                 -- (M.SWEEP_BODY_HEIGHT * dscale - M.STEP_UP) / 2), so the capsule's bottom can sit
                                 -- M.STEP_UP above her feet without shrinking to nothing on a short/tall pawn
-- Her sweep reads the SAME trace channel as the ground ray (M.TRACE_CHANNEL, Visibility) - never an
-- object-type query (EObjectTypeQuery, a TArray<TEnumAsByte<EObjectTypeQuery>> UE4SS marshals as a Lua table):
-- that TArray can arrive silently empty under some UE4SS versions (RE-UE4SS#378, a dropped sweep that never
-- blocks), and an object-type query hits Trigger volumes - their object type is WorldDynamic, same as a solid
-- prop's, so she would bump into something invisible. A channel trace instead asks each primitive's own
-- per-channel response, and a Trigger's default response block list ignores Visibility the same way it
-- ignores the ground ray already - no extra filter code needed, and never hers to push through either.
M.COLLIDE_SUBSTEP = 120.0       -- longest move committed before re-checking (cm); not a tunnelling guard (see
                                 -- above) - only how often a blocked frame re-aims
M.COLLIDE_MAX_SUBSTEPS = 6      -- a hard cap on sweeps/frame: bounds worst-case cost (see notes/engine-rules
                                 -- budget note at the call site)
M.SLIDE_ANGLES = { 0.6, -0.6, 1.2, -1.2, 1.6, -1.6 } -- radians, ported verbatim from astra.cpp:590 (the 1.6 rad
                                 -- member is just past perpendicular on purpose: pressed flush against a wall,
                                 -- every angle under 90 degrees still drifts INTO it over any nonzero slide
                                 -- length - astra-in-death-stranding's own report on this port)
M.SLIDE_FACTOR = 0.7            -- ported from astra.cpp:594
M.COLLIDE_TURN_RATE = 400.0     -- deg/s the WALK direction chases the goal bearing while a sweep is wired (never
                                 -- the raw bearing recomputed fresh every frame - that oscillates against a wall,
                                 -- see M.walk_collide's doc); her cosmetic s.yaw turn (540 deg/s) is unaffected
M.STUCK_SECONDS, M.STUCK_DISTANCE = 1.6, 300.0 -- ported verbatim (m -> cm) from astra.cpp kStuckSeconds/
                                 -- kStuckDistance (astra.cpp:32), the CONSERVATIVE DS rule (round-2 review
                                 -- finding: a sole "any block this frame" gate fired on a slow creep sliding
                                 -- cleanly around a small obstacle): no progress toward her REAL spot for this
                                 -- long, while still this far from it (within STUCK_DISTANCE she is never
                                 -- teleported for being stuck - a documented limitation, the same one DS has),
                                 -- AND a sweep actually reported a block at some point during the SAME
                                 -- no-progress stretch (s.stuck_blocked_seen, below - never raw distance alone,
                                 -- and never collision-unaware: an open-field sprint at her speed cap plateaus
                                 -- the gap the SAME WAY a genuine wedge does, and sweep == nil must never read
                                 -- as stuck at all) folds into the EXISTING put-her-at-her-spot branch below
                                 -- (never a second "teleport" mechanism) - a wall that will never open must not
                                 -- wedge her against it forever, the same way raw distance already doesn't
                                 -- (M.TELEPORT)
M.STUCK_IMPROVE = 30.0          -- "progress" (astra.cpp:495): to_spot must shrink by more than this to count -
                                 -- a noise floor, not a literal zero (her own slow walk covers far more than
                                 -- this every tick, so only truly stuck - pressed against something - trips it)
M.STUCK_GIVEUP = 1.5            -- her optional stand-mode step-away/stroll aim (never her real spot) is just
                                 -- DROPPED after this long stuck, not teleported for - low stakes, recomputed
                                 -- next time she needs one (astra.cpp's own aim/stay distinction)
M.STUCK_AIM_DROPS = 3            -- ...but dropping an aim changes nothing about the geometry, so an unreachable
                                 -- one is set again identically next frame and would retry forever with no
                                 -- escape at all; this many consecutive giveups with no arrival between them
                                 -- escalates to the real teleport above (a genuinely sealed-off spot, the
                                 -- player himself never moving) - about STUCK_GIVEUP * (this + 1) seconds...
M.AIM_DROP_DIST, M.AIM_DROP_GAP = 100.0, 5.0 -- ...PROVIDED the drops are actually related (review finding,
                                 -- round 1): a new aim set more than AIM_DROP_DIST from HER OWN position at
                                 -- the LAST drop (round-2 correction - comparing the derived aim POINT instead
                                 -- read a small sway of the PLAYER as "a different spot", since the aim is her
                                 -- bearing from him projected ~166 cm out; she barely moves while genuinely
                                 -- stuck, so her own position is the stable reference), or more than
                                 -- AIM_DROP_GAP seconds after it, resets s.aim_drops to 0 first - two giveups
                                 -- at opposite sides of him minutes apart are unrelated stand-mode failures,
                                 -- never "a sealed-off spot" - only a RUN of drops while she herself stays put,
                                 -- close together in time, escalates
M.NAV_EVERY = 0.5               -- seconds between navmesh path queries (optional, best-effort - never per frame;
                                 -- a synchronous query is comparatively expensive and only needs to be roughly
                                 -- right). A level with no baked NavMesh (plausible for a scripted first-person
                                 -- game with no AI) answers invalid/empty every time - unchanged, see M.nav_goal
M.NAV_LOOKAHEAD = 150.0         -- cm: a path point this near her is skipped for the next one - she steers at the
                                 -- next useful bend, not every polyline vertex

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
    -- `not (a >= b)` rather than `a < b`: a NaN answer (a degenerate view, review r2) fails every comparison
    if not (cy >= 2 and dy >= 0.25) then return nil end
    local t = (M.PROBE_H / M.PROBE_D) * cy / dy
    if not (t >= 0.0087 and t <= 11.43) then return nil end -- 1..170 degrees
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
--- balcony the player stands on over her head is a ceiling to her, and she stays on her floor. Separately, a surface
--- more than OBJECT_BAND above the PLAYER's own feet is never hers either, ceiling or not - a ledge/balcony he
--- actually climbed has HIM up there too; an isolated prop (a crate, a table, a railing) near him does not, and a
--- single downward ray cannot otherwise tell the two apart (both read as "higher surface, nothing overhead"). The
--- retrace below (from z - 1 down) ONLY replaces z when it actually finds something lower (a crate sitting on a
--- floor); finding nothing there is not "no floor" - on a continuous slope or a hill, the surface already found
--- (z itself) IS the only floor there is, and the OLD retrace discarding it (review finding) sank her through
--- her own real floor the moment she stood more than OBJECT_BAND above the player's feet on one.
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
        if z ~= nil and z > p.z + M.OBJECT_BAND then
            local under = ground(s.x, s.y, z - 1, bottom) -- not a ledge he's on: is there a real floor under/around it?
            if under ~= nil then
                z = under -- yes - a crate (or similar) over a floor: the floor is hers, not the crate's top
                g.objects = (g.objects or 0) + 1
            else
                -- the retrace found nothing - but a single downward ray cannot tell a continuous slope (keep
                -- z) from an isolated object sitting over a void (a crate at a pit's edge, him down in the
                -- pit: the SAME "nothing lower" reading, for the opposite reason - review finding). A
                -- continuity check: a real slope's height varies smoothly, so the ground at the MIDPOINT
                -- between her and the player should read close to the midpoint of their two heights; an
                -- object over a void does not - there is nothing continuous to find there at all.
                local midx, midy = (s.x + p.x) / 2, (s.y + p.y) / 2
                local mid_z = ground(midx, midy, math.max(s.z, p.z) + M.TRACE_UP, bottom)
                if mid_z == nil or math.abs(mid_z - (p.z + z) / 2) > M.OBJECT_CONTINUITY then
                    z = nil -- an object over a void: no floor here (the pre-fe33078 behaviour)
                    g.objects = (g.objects or 0) + 1
                end -- else: a continuous slope after all - keep z, same as fe33078's own slope case
            end
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

--- His velocity (UE cm/s) as a raw fact: what the game reports (vx, vy - nil: no report), unless his own displacement
--- says he moves much faster - a pawn the game moves without a movement component reports ~0 while it walks (Mecha
--- Chameleon, 2026-10-20: she followed "at a slow walk however far, then teleported"), and the camera-only fallback has
--- no report at all (the camera's displacement). The displacement is measured over the REAL time since the last sample
--- (now: the tick clock, seconds - never the frame's clamped dt) and averaged over ~50 ms; a gap over GAP_MAX (a hitch,
--- frames without an answer) or a speed over JUMP_SPEED (a respawn, a cut) starts it over instead of reading as a sprint
--- (review of 0.2.6: a 1 s hitch read 10x his speed). st: this function's own state.
M.GAP_MAX, M.JUMP_SPEED = 0.25, 4000.0
function M.real_velocity(st, x, y, vx, vy, now)
    local gap = st.t ~= nil and now - st.t or nil
    if gap ~= nil and gap > 0 then
        local ux, uy = (x - st.x) / gap, (y - st.y) / gap
        if gap > M.GAP_MAX or ux * ux + uy * uy > M.JUMP_SPEED ^ 2 then
            st.vx, st.vy = nil, nil -- a hitch or a jump: no speed from it, a fresh start
        else
            local k = math.min(1, gap * 20)
            st.vx = st.vx == nil and ux or st.vx + (ux - st.vx) * k
            st.vy = st.vy == nil and uy or st.vy + (uy - st.vy) * k
        end
    end
    if gap == nil or gap > 0 then st.x, st.y, st.t = x, y, now end
    if st.vx == nil then return vx or 0, vy or 0 end
    if vx == nil then return st.vx, st.vy end
    local said, moved = math.sqrt(vx * vx + vy * vy), math.sqrt(st.vx * st.vx + st.vy * st.vy)
    if moved > said * 1.5 + M.GO_START then return st.vx, st.vy end
    return vx, vy
end

--- Is a fresh navmesh path query due (optional long-range steering hint)? `at`: when the last one was taken
--- (nil: never). Gates the GLUE's call, not a frame-rate thing M here decides on its own - never more than
--- once per NAV_EVERY seconds, so a comparatively expensive synchronous query never runs every frame.
function M.nav_due(at, now)
    return at == nil or now - at >= M.NAV_EVERY
end

--- Her next steering goal given a cached navmesh path (optional - nil/empty `points`: the plain goal,
--- unchanged, so a level with no baked NavMesh, or a game where the query call is missing or threw, behaves
--- exactly as without this). `points`: {x, y} pairs from the last successful, non-partial path query, nearest
--- first. Skips every point already within NAV_LOOKAHEAD of her (she steers at the next useful bend, not every
--- polyline vertex - a dense path would otherwise stall her re-aiming every few cm); the LAST point replaces
--- the plain goal only once she is past every point before it.
function M.nav_goal(x, y, goal_x, goal_y, points)
    if points == nil or #points == 0 then return goal_x, goal_y end
    for _, pt in ipairs(points) do
        local dx, dy = pt[1] - x, pt[2] - y
        if dx * dx + dy * dy > M.NAV_LOOKAHEAD * M.NAV_LOOKAHEAD then return pt[1], pt[2] end
    end
    return goal_x, goal_y
end

--- One try: from (x, y), by (dx, dy) - nil if it would land nearer the player than clear_r (she must never be
--- pushed into him by a SLIDE - the caller passes clear_r for a slide candidate only, nil/0 for the direct
--- move: the direct move already has its own push-away steering in M.follow, before M.walk_collide is ever
--- called, and vetoing it too would read a merely-nearby direct step as "blocked", halving her pace and
--- counting a blocked frame for no obstacle at all), or if `sweep` finds her body blocked there; else the
--- landing x, y. `sweep(x1, y1, x2, y2) -> true if blocked` is ALREADY fail-safe at the call site (the glue's
--- capsule-sweep closure, or the test stub) - a throw there is the glue's job to latch, not this pure
--- function's; `sweep == nil` always answers clear (the caller only reaches here once a sweep IS given - see
--- M.follow).
local function try_move(x, y, dx, dy, sweep, px, py, clear_r, stats)
    local nx, ny = x + dx, y + dy
    if clear_r and clear_r > 0 then
        local ddx, ddy = nx - px, ny - py
        if ddx * ddx + ddy * ddy < clear_r * clear_r then return nil end
    end
    if sweep == nil then return nx, ny end
    if stats then stats.sweeps = (stats.sweeps or 0) + 1 end
    if sweep(x, y, nx, ny) then return nil end
    return nx, ny
end

--- Fix #3: where she lands when she is PUT at a spot (first placement, TELEPORT, floor apart, stuck) - never
--- inside geometry, the way an unconditional teleport could before this. `sweep` already answers (blocked,
--- time) - see trace_capsule/capsule_sweep's own doc; `sweep == nil` always answers clear (the caller only
--- reaches here once a sweep IS given, same convention as try_move above). From the PLAYER's own position
--- (px, py) toward the candidate (cx, cy): if clear, she lands there exactly (today's behaviour). If blocked,
--- the free point along that SAME line - hit.Time short by about her own radius (M.SWEEP_RADIUS, a margin,
--- not literally her edge). Returns x, y (the landing point - ALWAYS a real point along the line, even when
--- not `ok`, so the caller can fall back to the best of several partial results - review finding, round 2:
--- the old "too little room" miss returned nil, nil, pushing the caller all the way to an unswept last
--- resort), ok (true once `free` reaches `near_player`, M.NEAR_PLAYER * scale - the same comfort radius her
--- walking already keeps), and free itself (the achieved distance - math.huge with no sweep, 0 if px,py and
--- cx,cy already coincide).
local function sweep_place(px, py, cx, cy, sweep, near_player)
    if sweep == nil then return cx, cy, true, math.huge end
    local dx, dy = cx - px, cy - py
    local dist = math.sqrt(dx * dx + dy * dy)
    if dist < 1e-3 then return px, py, true, 0 end
    local blocked, time = sweep(px, py, cx, cy)
    if not blocked then return cx, cy, true, dist end
    local free = math.max(0, (time or 0) * dist - M.SWEEP_RADIUS)
    local ux, uy = dx / dist, dy / dist
    return px + ux * free, py + uy * free, free >= near_player, free
end

--- The substepped walk, collision-aware: from (x, y), `total` cm along unit (ux, uy); `sweep` a capsule sweep
--- (see try_move) - ported from the Death Stranding follower's frame()-loop + try_step/slide (astra.cpp:580-
--- 607, in spirit: ONE capsule sweep per try here, where astra.cpp tests two spheres, since UE4SS gives one
--- call that already sweeps her whole body). `cross_for_side`: (her walk direction x the raw goal direction) -
--- only its SIGN matters, and only the FIRST time a slide is needed this stretch. `prev_side`/`prev_dir_x,y`:
--- what the LAST frame committed to (nil: nothing yet) - both are STICKY across frames (round-tripped through
--- the caller's persisted state), for two reasons astra-in-death-stranding's own port notes found by
--- reproducing the stalls directly: (1) with nothing committed yet, the walk direction and the raw goal
--- direction are the SAME vector, so their cross product is noise - re-picking a side fresh every frame
--- flip-flops between the two slide options and she never commits; (2) even with a side fixed, retrying the
--- fixed angle list smallest-first every frame oscillates in a stable 2-frame cycle right at a hard boundary
--- (pressed flush against a wall) - trying the exact direction that worked last FIRST, before the fixed list,
--- breaks the cycle ("keep doing what is working" beats "always prefer the smallest turn"). Both are returned
--- even on a frame that needed no slide at all - dropping them the moment a frame is briefly clear re-opens the
--- same stall over a wider, less predictable range; the caller drops them only on a real placement/teleport.
--- Returns final x, y, whether she ran out of substeps or every slide angle failed this frame, the side/
--- direction now committed (both nil: clear, nothing to stay committed to), and whether a sweep reported a
--- block AT ALL this frame (even one she slid around successfully) - the stuck detector's own input (see
--- M.follow), never just the "truly stuck" case below.
function M.walk_collide(x, y, ux, uy, total, sweep, substep, max_substeps, px, py, clear_r, cross_for_side, prev_side, prev_dir_x, prev_dir_y, stats)
    if sweep == nil or total <= 0 then return x + ux * total, y + uy * total, false, nil, nil, nil, false end
    local left, n, blocked_out, side, any_blocked = total, 0, false, prev_side, false
    local dir_x, dir_y = prev_dir_x, prev_dir_y
    while left > 1e-4 and n < max_substeps do
        n = n + 1
        local len = math.min(left, substep)
        local nx, ny = try_move(x, y, ux * len, uy * len, sweep, px, py, nil, stats) -- the direct move: no clear_r veto
        if nx ~= nil then
            x, y, left = nx, ny, left - len
        else
            any_blocked = true
            if side == nil then side = (cross_for_side >= 0) and 1 or -1 end
            local found, slen = false, len * M.SLIDE_FACTOR
            if dir_x ~= nil then
                local sxn, syn = try_move(x, y, dir_x * slen, dir_y * slen, sweep, px, py, clear_r, stats)
                if sxn ~= nil then x, y, found = sxn, syn, true end
            end
            if not found then
                for _, ang in ipairs(M.SLIDE_ANGLES) do
                    local a = ang * side
                    local c, sn = math.cos(a), math.sin(a)
                    local sx, sy = ux * c - uy * sn, ux * sn + uy * c
                    local sxn, syn = try_move(x, y, sx * slen, sy * slen, sweep, px, py, clear_r, stats)
                    if sxn ~= nil then x, y, found, dir_x, dir_y = sxn, syn, true, sx, sy break end
                end
            end
            left = left - len -- the full substep is spent whether she slid 0.7x of it or not at all (astra.cpp:605)
            if not found then blocked_out = true break end
        end
    end
    return x, y, blocked_out, side, dir_x, dir_y, any_blocked
end

--- One step of her walking. s: her state (UE cm); p: the player {x, y, z (feet), yaw, vx, vy, falling,
--- half_height (optional: the pawn's capsule half-height, for scale - F)}; dt seconds; now: a clock (seconds);
--- ground: a line trace down (optional: without it she stands on the player's ground); sweep(x1, y1, x2, y2) ->
--- blocked (optional: without it - missing, or withdrawn by the glue after it threw once - every line below it
--- touches is skipped and she moves exactly as before, one unclamped straight-line step); nav: a cached navmesh
--- path's points (optional, see M.nav_goal - a best-effort long-range hint only, never required).
function M.follow(s, p, dt, now, ground, sweep, nav)
    -- scale-aware distances (F): a smaller/taller capsule than the reference moves her spot and comfort
    -- radii proportionally; no half_height given (or 0): today's fixed numbers, unchanged
    local dscale = (p.half_height and p.half_height > 0) and (p.half_height / M.REF_HALF_HEIGHT) or 1.0
    -- first person (the camera at his head, p.camera_dist): every comfort distance larger (FIRST_PERSON_SCALE)
    if p.camera_dist == nil then
        s.first_person = false
    elseif p.camera_dist < M.FIRST_PERSON_DIST * dscale then
        s.first_person = true
    elseif p.camera_dist > M.FIRST_PERSON_LEAVE * dscale then
        s.first_person = false
    end
    if s.first_person then dscale = dscale * M.FIRST_PERSON_SCALE end
    local back, side, near_enough, stay_radius = M.BACK * dscale, M.SIDE * dscale, M.NEAR_ENOUGH * dscale, M.STAY_RADIUS * dscale
    local yr = math.rad(p.yaw)
    local fx, fy = math.cos(yr), math.sin(yr)
    local rx, ry = -fy, fx -- UE: +Y is to the right of +X
    local tx, ty = p.x - fx * back + rx * side, p.y - fy * back + ry * side
    if s.placed then M.update_ground(s, p, now, ground) end
    local pspeed = math.sqrt(p.vx * p.vx + p.vy * p.vy)
    local to_spot = math.sqrt((tx - s.x) ^ 2 + (ty - s.y) ^ 2)
    -- stuck (G): the Death Stranding follower's OWN, conservative rule (astra.cpp:494-516), ported literally
    -- this time (review finding, round 2 - the round-1 attempt used "any try_move failed this frame" as the
    -- sole gate, which fired on a slow creep sliding CLEANLY around a small obstacle - 2 teleports in 20 s
    -- past a 40x40 cm pillar at her trailing spot - because that gate doesn't care whether the stall is a
    -- genuine wedge or a few frames of ordinary slide negotiation on an otherwise-fine detour). stuck_time
    -- accumulates only while ALL THREE hold: to_spot > M.STUCK_DISTANCE (far enough to matter at all - WITHIN
    -- this she is never teleported for being stuck, a documented limitation, the same one DS itself has);
    -- NO PROGRESS (to_spot has not shrunk by more than M.STUCK_IMPROVE since the current stretch's best); and
    -- a sweep reported a block AT SOME POINT during this SAME no-progress stretch (s.stuck_blocked_seen,
    -- latched true the first time s.collide_blocked is true after the last progress reset, not required on
    -- EVERY frame of the stretch - a genuinely sealed wall reports a block on most frames but not literally
    -- all of them, and requiring every single one reopened the exact "sealed wall, player walking away" stall
    -- this ported from). Progress (to_spot shrinking) resets stuck_time AND the latch together - she is never
    -- cut off mid-progress, and a fresh stretch starts clean. With sweep == nil, s.collide_blocked is always
    -- false, so the latch never sets and this can never fire - BYTE-IDENTICAL to today; an open-field sprint
    -- at her speed cap plateauing the gap the SAME WAY a genuine wedge does must never read as stuck either,
    -- which the STUCK_DISTANCE gate alone already guards (a plateaued gap keeps growing past it, but with no
    -- sweep the latch never sets). Gated on s.going (last frame's: it is not recomputed yet this frame) - see
    -- the stand/go hysteresis below for why tx/ty is not her live goal at all while she is standing.
    if s.going then
        if to_spot < (s.stuck_best or 1e9) - M.STUCK_IMPROVE then
            s.stuck_best, s.stuck_time, s.stuck_blocked_seen = to_spot, 0, false
        else
            if s.collide_blocked then s.stuck_blocked_seen = true end
            if to_spot > M.STUCK_DISTANCE and s.stuck_blocked_seen then
                s.stuck_time = (s.stuck_time or 0) + dt
            end
        end
    else
        s.stuck_time, s.stuck_best, s.stuck_blocked_seen = 0, 1e9, false
    end
    -- a SECOND escape, for when she is never "going" at all (the player never moves - the quieter aim giveup
    -- below just drops an unreachable aim rather than teleporting for it, since on its own that is low stakes;
    -- but dropping it changes nothing about the geometry, so the SAME conditions set an identical new aim next
    -- frame and she would otherwise retry forever, in lockstep with STUCK_GIVEUP, with no escape at all - a
    -- genuinely sealed-off spot must still free her eventually). s.aim_drops counts consecutive giveups with no
    -- successful arrival between them (reset on arrival or on any placement/teleport, including this one).
    local stuck = s.placed and ((s.going and (s.stuck_time or 0) > M.STUCK_SECONDS) or (s.aim_drops or 0) > M.STUCK_AIM_DROPS)
    if not s.placed or to_spot > M.TELEPORT or stuck or math.abs((s.ground or p.z) - s.z) > M.FLOOR_APART then
        -- put at her spot (fix #3: swept from the player, never landing her inside geometry) - on the ground
        -- there (traced at once). Tried in order, every one of them SWEPT (review finding: the old last
        -- resort was unswept and landed her inside a wall in a corridor narrower than about her spot's own
        -- lateral reach, ~134 cm): her real spot; its mirror (the other side of him, same back/side
        -- magnitudes - "behind him on the other side"); his right side; his left side; directly behind him.
        -- The three new ones land at near_player + M.SWEEP_RADIUS, never exactly AT near_player - M.SIDE
        -- happens to equal M.NEAR_PLAYER (90 cm each), and an UNBLOCKED sweep_place lands a candidate exactly
        -- where it was aimed, so aiming "his side" at M.SIDE put her exactly on the comfort-radius boundary,
        -- which the very next frame's own walking dynamics could nudge inside of (the same coincidence fix #3
        -- itself already had to correct for its own last resort). If EVERY swept candidate falls short of
        -- near_player (review finding, round 2: the old code then fell back to his EXACT position - inside
        -- his own capsule, or inside the camera in a tight first-person room), keep the best PARTIAL result
        -- among the five instead - whichever reached the most free distance - provided that clears at least
        -- her own body (M.SWEEP_RADIUS); never his exact (p.x, p.y). If even the best partial result is
        -- tighter than that and she already had a position, stay there rather than move into something worse
        -- - only a first placement (nothing to stay at yet) takes the best point anyway.
        local near_player = M.NEAR_PLAYER * dscale
        local beside = near_player + M.SWEEP_RADIUS
        local was_placed = s.placed
        local candidates = {
            { tx, ty },                                             -- her real spot
            { p.x - fx * back - rx * side, p.y - fy * back - ry * side }, -- the mirror: the other side of him
            { p.x + rx * beside, p.y + ry * beside },               -- his right side
            { p.x - rx * beside, p.y - ry * beside },               -- his left side
            { p.x - fx * beside, p.y - fy * beside },               -- directly behind him
        }
        local px2, py2, ok = nil, nil, false
        local best_x, best_y, best_free = nil, nil, -1
        for _, c in ipairs(candidates) do
            local x, y, cok, free = sweep_place(p.x, p.y, c[1], c[2], sweep, near_player)
            if free > best_free then best_x, best_y, best_free = x, y, free end
            if cok then
                px2, py2, ok = x, y, true
                break
            end
        end
        if not ok then
            if best_free >= M.SWEEP_RADIUS then
                px2, py2 = best_x, best_y
            elseif was_placed then
                px2, py2 = s.x, s.y -- every candidate too tight, already somewhere: stay put
            else
                px2, py2 = best_x, best_y -- first placement: nothing to stay at, take the best anyway
            end
        end
        s.x, s.y, s.z, s.yaw = px2, py2, p.z, p.yaw
        if s.trace then s.trace.z = nil end
        M.update_ground(s, p, now, ground, true)
        s.z = s.ground or p.z
        s.v, s.vx, s.vy, s.speed, s.placed, s.teleports, s.going = 0, 0, 0, 0, true, (s.teleports or 0) + 1, false
        s.aim_x, s.aim_y, s.aim_time, s.aim_best, s.aim_drops = nil, nil, 0, 1e9, 0
        s.aim_drop_x, s.aim_drop_y, s.aim_drop_time = nil, nil, nil
        s.stuck_time, s.stuck_best, s.stuck_blocked_seen, s.slide_side, s.slide_dir_x, s.slide_dir_y = 0, 1e9, false, nil, nil, nil
        s.collide_blocked = false
        if stuck then s.stuck_teleports = (s.stuck_teleports or 0) + 1 end
        return s
    end
    -- stand/go (E): she tracks the player once he is really GOING; a hysteresis gap (GO_START to start,
    -- GO_STOP to stop), not one bare threshold, so a tiny speed jitter near it can't flip the stand/catch-up
    -- branch every frame.
    local go_start, go_stop = M.GO_START, M.GO_STOP
    if p.from_camera then go_start, go_stop = M.CAMERA_GO_START, M.CAMERA_GO_STOP end
    if pspeed >= go_start then
        s.going = true
    elseif pspeed < go_stop then
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
            -- review finding, round 2: compare HER OWN position (s.x, s.y), not the derived aim point - the
            -- aim is her bearing from him projected `r` (~166 cm) out, so a small sway of the PLAYER swings
            -- the aim point by more than M.AIM_DROP_DIST even though neither SHE nor the actual obstacle
            -- moved at all, wrongly resetting a genuinely sealed-off retry into an infinite loop. She barely
            -- moves while actually stuck, so her own position is the stable reference: a NEW aim set more
            -- than M.AIM_DROP_DIST from where she was at the LAST drop, or more than M.AIM_DROP_GAP seconds
            -- after it, is an unrelated stand-mode failure - never let it inherit an escalating count toward
            -- a teleport that belongs to a different spot entirely.
            if s.aim_drop_x ~= nil then
                local ddx, ddy = s.x - s.aim_drop_x, s.y - s.aim_drop_y
                if ddx * ddx + ddy * ddy > M.AIM_DROP_DIST * M.AIM_DROP_DIST or now - s.aim_drop_time > M.AIM_DROP_GAP then
                    s.aim_drops = 0
                end
            end
        end
        if s.aim_x ~= nil then goal_x, goal_y = s.aim_x, s.aim_y else goal_x, goal_y = s.x, s.y end
    else
        s.aim_x, s.aim_y = nil, nil
    end
    local dx, dy = goal_x - s.x, goal_y - s.y
    local d = math.sqrt(dx * dx + dy * dy)
    if s.aim_x ~= nil and d <= near_enough then s.aim_x, s.aim_y, s.aim_drops = nil, nil, 0 end -- there
    if s.aim_x ~= nil then
        -- a quieter stuck case (astra.cpp's own aim/stay distinction): only her OPTIONAL step-away/stroll aim
        -- is unreachable, never her real spot - drop it rather than teleport for it (low stakes; recomputed
        -- next time she needs one, from wherever she actually is) - UNLESS it keeps happening with no arrival
        -- in between (s.aim_drops, counted and escalated at the top of this function).
        if d < (s.aim_best or 1e9) - M.STUCK_IMPROVE then
            s.aim_best, s.aim_time = d, 0
        else
            s.aim_time = (s.aim_time or 0) + dt
            if s.aim_time > M.STUCK_GIVEUP then
                s.aim_drop_x, s.aim_drop_y, s.aim_drop_time = s.x, s.y, now -- HER position, not the aim point
                s.aim_x, s.aim_y, s.aim_time, s.aim_best = nil, nil, 0, 1e9
                s.aim_drops = (s.aim_drops or 0) + 1
                goal_x, goal_y, dx, dy, d = s.x, s.y, 0, 0, 0
            end
        end
    else
        s.aim_time, s.aim_best = 0, 1e9
    end
    -- the navmesh hint (optional - M.nav_goal is a no-op without a usable path): steer toward the next useful
    -- bend of a cached path instead of straight at the goal, ONLY while actually heading for her real spot
    -- (s.going; goal_x, goal_y == tx, ty in that branch) - never while standing still or walking her short,
    -- close-by stand-mode aim, where a cached path toward the far spot would be the wrong target entirely.
    -- Recomputing dx, dy, d keeps every line below (push-off, pace, stuck) working from the live goal.
    if nav ~= nil and s.going then
        goal_x, goal_y = M.nav_goal(s.x, s.y, goal_x, goal_y, nav)
        dx, dy = goal_x - s.x, goal_y - s.y
        d = math.sqrt(dx * dx + dy * dy)
    end
    local want
    if s.going and d <= near_enough then
        want = math.min(pspeed, M.RUN_MAX) -- at her spot while he walks: keep his pace, never stop-and-go
    elseif s.going then
        want = math.min(M.catchup_want(pspeed, d, near_enough), math.max(pspeed * 1.5, M.CALM_PACE))
    else
        -- he stands (or creeps slower than GO_START): she only ever walks, at her calm walk - never a rush at
        -- CALM_PACE to an aim while he barely moves (review r2: a 9-14 cm/s creep stood, then burst at 1.2 m/s)
        -- (from a faster catch-up he just stopped, her pace comes down to it at SETTLE, not with a jolt)
        want = math.min(M.catchup_want(pspeed, d, near_enough), math.max(M.WALK_PACE, (s.v or 0) - M.SETTLE * dt))
    end
    s.v = M.accel_brake(s.v or 0, want, dt)
    local step = math.min(s.v * dt, d)
    local mx, my = 0, 0 -- this frame's move
    s.collide_blocked = false -- the stuck detector's input (G, above - read as LAST frame's, the same
                               -- one-frame lag s.going already uses): a sweep actually reported a block THIS
                               -- frame (even one she slid around) - set true below only when one did
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
        if sweep == nil then
            -- today's exact behaviour: one unclamped straight-line step, byte for byte (the fail-safe test
            -- requires this, not just "close enough" - see the glue's sweep_broken latch).
            mx, my = ux * step, uy * step
            s.x, s.y = s.x + mx, s.y + my
            if step > 0.3 then s.yaw = M.turn(s.yaw, math.deg(math.atan(uy, ux)), 540 * dt) end
        else
            -- a STABLE heading to walk/slide along, not the raw bearing-to-goal recomputed fresh every frame -
            -- that bearing is perturbed by a slide's own small lateral moves, and feeding it straight back into
            -- "which slide angle is clear first" is exactly the 2-frame oscillation M.walk_collide's own doc
            -- describes (astra.cpp avoids this because ITS heading is the avatar's own slowly-turning facing,
            -- decoupled from the instantaneous goal bearing - ported fix: chase the goal bearing at a capped
            -- turn rate, same mechanism as her existing cosmetic yaw turn, and walk/slide along THAT).
            local target_deg = math.deg(math.atan(uy, ux))
            s.move_heading = s.move_heading and M.turn(s.move_heading, target_deg, M.COLLIDE_TURN_RATE * dt) or target_deg
            local mh = math.rad(s.move_heading)
            local fux, fuy = math.cos(mh), math.sin(mh)
            local cross_for_side = fux * dy - fuy * dx
            local nx, ny, blocked_this_frame, used_side, used_dx, used_dy, any_blocked = M.walk_collide(s.x, s.y, fux, fuy, step, sweep,
                M.COLLIDE_SUBSTEP * dscale, M.COLLIDE_MAX_SUBSTEPS, p.x, p.y, clear, cross_for_side, s.slide_side, s.slide_dir_x,
                s.slide_dir_y, s.collide_stats)
            s.slide_side, s.slide_dir_x, s.slide_dir_y = used_side, used_dx, used_dy
            s.collide_blocked = any_blocked
            mx, my = nx - s.x, ny - s.y
            s.x, s.y = nx, ny
            if blocked_this_frame then
                s.v = s.v * 0.5 -- astra.cpp:611: slow down instead of staying slammed at full pace against a wall
                s.blocked_frames = (s.blocked_frames or 0) + 1
            end
            local moved = math.sqrt(mx * mx + my * my)
            if moved > 0.3 then s.yaw = M.turn(s.yaw, math.deg(math.atan(my, mx)), 540 * dt) end
        end
    elseif not s.going then
        -- standing: turn slowly the way he looks - in first person, where his body turns with his view, toward HIM
        -- instead (the owner, Poppy Playtime, 2026-10-20: "when I turn my head she rotates in place, like I'm moving
        -- the camera around her"; the Death Stranding follower faces Sam too)
        local face = p.yaw
        if s.first_person then face = math.deg(math.atan(p.y - s.y, p.x - s.x)) end
        s.yaw = M.turn(s.yaw, face, 90 * dt)
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
local cache = { at = -1, paused_at = -1, project_misses = 0, project_next = -1 }
local her = { placed = false, x = 0, y = 0, z = 0, yaw = 0, v = 0, vx = 0, vy = 0, speed = 0 }
local last_frame, last_clock, own_frame, frame_counter = -1, nil, 0, nil
local stats = { frames = 0, sent = 0, cost = 0, logged_at = 0, errors = 0 }
local kismet = nil
local NO_COLOR = { R = 0, G = 0, B = 0, A = 0 }
local trace_broken = false
local sun = { comp = nil, find_at = -1e9, sent_at = -1e9, said = nil, visible = 1 }
local his_motion = {} -- M.real_velocity's state for the followed actor (a Pawn or a view target)
local cam_motion = {} -- ... and for the camera-only fallback (no actor at all: GTA DE's Gameface, see player_actor)

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
    cache.horizontal, cache.aspect, cache.near, cache.ortho = true, 0, 0, false
    pcall(function()
        local c = pc.Player.AspectRatioAxisConstraint -- 0 MaintainYFOV, 1 MaintainXFOV, 2 MajorAxisFOV
        cache.horizontal = c ~= 0
    end)
    pcall(function()
        local pov = pcm.CameraCachePrivate.POV
        if pov.bConstrainAspectRatio then cache.aspect = pov.AspectRatio end
        if pov.PerspectiveNearClipPlane and pov.PerspectiveNearClipPlane > 0 then cache.near = pov.PerspectiveNearClipPlane end
        cache.ortho = pov.ProjectionMode == 1 -- ECameraProjectionMode::Orthographic
    end)
    -- no per-camera near plane (the POV's is <= 0: "use the engine's"): the engine's own, UEngine::NearClipPlane - a
    -- project setting (Unreal's default 10 cm, which the add-on assumes when told nothing). The add-on turns the game's
    -- depth buffer into metres with it, so a game drawing with a 1 cm near plane read every wall ~10x further than it
    -- is and she showed through all of them (the owner, Mecha Chameleon, 2026-10-20: depth bands every few cm).
    if cache.near <= 0 then
        pcall(function()
            if not (cache.engine and cache.engine:IsValid()) then cache.engine = FindFirstOf("Engine") end
            local n = cache.engine.NearClipPlane
            if type(n) == "number" and n > 0 then cache.near = n end
        end)
    end
    if cache.near ~= cache.said_near then
        cache.said_near = cache.near
        -- (the engine's is its project setting: a game changing it at run time with r.SetNearClipPlane moves only the
        -- renderer's global, which nothing here can read - the add-on's DebugView 2 shows it: bands 1 m apart are right)
        log("the game's near plane: %s", cache.near > 0 and string.format("%.2f cm", cache.near) or "unknown (the add-on assumes 10 cm)")
    end
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

--- One capsule trace along the horizontal segment (x1,y1)-(x2,y2) at world Z `z` (her sweep height), on
--- M.TRACE_CHANNEL - the SAME channel the ground ray already uses, never an object-type query (see the
--- M.SWEEP_RADIUS comment above for why). `pawn` is the WorldContextObject, with bIgnoreSelf = true - the
--- player must never block her own sweep, and never only because he happens to be named in ActorsToIgnore
--- (passed empty here, {}). Returns: ok (the call did not throw), blocked (a clean blocking hit OR
--- bStartPenetrating - she must never read "clear" while already inside something), hit.Time (0..1, nil
--- without a hit or without ok). The one place AstraUE calls CapsuleTraceSingle; both the walking sweep below
--- and the placement sweep (M.follow's teleport branch) go through this, never a second call site.
local function trace_capsule(pawn, x1, y1, x2, y2, z, radius, half_height)
    local hit = {}
    local ok, did = pcall(function()
        return kismet:CapsuleTraceSingle(pawn, { X = x1, Y = y1, Z = z }, { X = x2, Y = y2, Z = z }, radius, half_height,
            M.TRACE_CHANNEL, false, {}, 0, hit, true, NO_COLOR, NO_COLOR, 0.0)
    end)
    if not ok then return false, false, nil, did end
    return true, did == true or hit.bStartPenetrating == true, hit.Time, nil
end

--- Her body against the world, horizontally: a capsule sweep between two XY points at her current feet
--- height (cm), through trace_capsule above. `half_height` (optional, cm - the followed pawn's own capsule
--- half-height, same as M.follow's own scale argument): fix #2's step allowance. Her swept capsule's BOTTOM
--- sits M.STEP_UP above her feet, not at them - centre Z = her feet + M.STEP_UP + sweep_half, sweep_half =
--- max(M.SWEEP_RADIUS, (M.SWEEP_BODY_HEIGHT * dscale - M.STEP_UP) / 2) - so a stair/kerb no taller than
--- M.STEP_UP sits entirely below the swept capsule and never blocks (Unreal's own CharacterMovement sweeps
--- with exactly this kind of MaxStepHeight allowance); a wall taller than that still fills the band and
--- blocks as before. The ground ray (M.update_ground, with its own, separate M.STEP_UP/M.OBJECT_BAND rule)
--- still decides her actual Z - this only keeps a step from reading as a wall to the HORIZONTAL sweep.
--- A game where the call fails disables collision-aware walking for the rest of the session (sweep_broken,
--- permanent - same shape as trace_broken/trace_down) and says so once; M.follow is then given `nil` for its
--- sweep argument, taking the exact unconditional-move code path, byte for byte - "today's behaviour", not a
--- softer, still-different fallback. UNVERIFIED: whether UE4SS's Lua reflection marshals CapsuleTraceSingle at
--- all (only LineTraceSingle is exercised anywhere in this codebase or its installed UE4SS examples) - this is
--- exactly what the latch is for.
local sweeper, sweeper_pawn, sweeper_half, sweep_broken = nil, nil, nil, false
local function capsule_sweep(pawn, half_height)
    if sweep_broken then return nil end
    local dscale = (half_height and half_height > 0) and (half_height / M.REF_HALF_HEIGHT) or 1.0
    local sweep_half = math.max(M.SWEEP_RADIUS, (M.SWEEP_BODY_HEIGHT * dscale - M.STEP_UP) / 2)
    local center = M.STEP_UP + sweep_half
    if sweeper_pawn == pawn and sweeper_half == sweep_half and sweeper ~= nil then return sweeper end
    sweeper_pawn, sweeper_half = pawn, sweep_half
    sweeper = function(x1, y1, x2, y2)
        local ok, blocked, time, err = trace_capsule(pawn, x1, y1, x2, y2, her.z + center, M.SWEEP_RADIUS, sweep_half)
        if not ok then
            sweep_broken = true
            log("cannot sweep for collision (she walks through walls as before): %s", tostring(err))
            return false
        end
        return blocked, time
    end
    return sweeper
end

--- A long-range steering hint from the game's navmesh (optional, best-effort - never required; see
--- M.nav_goal). Re-asked at most every NAV_EVERY seconds (M.nav_due), never per frame: a synchronous path
--- query is comparatively expensive and only needs to be roughly right. UNVERIFIED: whether
--- NavigationSystemV1's CDO is reachable (StaticFindObject on its default object) and its
--- FindPathToLocationSynchronously is callable through UE4SS the same general way KismetSystemLibrary/
--- GameplayStatics already are in this codebase - and whether either game even carries a baked NavMesh at all
--- (plausible for a scripted first-person game with no AI; a missing NavMesh answers invalid/empty every time,
--- handled the same as the call itself being missing - never a hard dependency). A throw disables it for the
--- session (nav_broken, permanent) and `nil` is given to M.follow from then on - unchanged from today.
local navsys, nav_broken, nav_at, nav_points = nil, false, nil, nil
local function nav_update(pawn, fx, fy, fz, tx, ty, tz, now)
    if nav_broken or not M.nav_due(nav_at, now) then return nav_points end
    nav_at = now
    if navsys == nil then
        local ok, found = pcall(function() return StaticFindObject("/Script/NavigationSystem.Default__NavigationSystemV1") end)
        if not ok or found == nil then
            nav_broken = true
            log("no navmesh query available (her route is the straight line to her spot, as before): %s", tostring(found))
            return nil
        end
        navsys = found
    end
    local ok, path = pcall(function()
        -- all FIVE parameters (UE4SS checks the arity): no pathfinding context actor, the default filter class
        return navsys:FindPathToLocationSynchronously(pawn, { X = fx, Y = fy, Z = fz }, { X = tx, Y = ty, Z = tz }, nil, nil)
    end)
    if not ok then
        nav_broken = true
        log("cannot query a path (her route is the straight line to her spot, as before): %s", tostring(path))
        return nil
    end
    if path == nil or path.PathPoints == nil then
        nav_points = nil
        return nil
    end
    local vok, is_valid = pcall(function() return path:IsValid() end)
    local pok, is_partial = pcall(function() return path:IsPartial() end)
    if not (vok and is_valid) or (pok and is_partial) then
        nav_points = nil
        return nil
    end
    local pts = {}
    for _, v in ipairs(path.PathPoints) do pts[#pts + 1] = { v.X, v.Y } end
    nav_points = pts
    return nav_points
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
--- ProjectWorldLocationToScreen (player-viewport-relative pixels), or nil - then she keeps the fov from its settings.
--- Two seconds without an answer (a game without the function, one that always says no, a long loading screen): it is
--- asked once a second from then on, until it answers again (said once per such stretch) - never given up for good,
--- since an alt-tab or a resolution change can make the call fail for a moment (review r2). An orthographic camera
--- (cache.ortho) is not asked at all: the measure is a pinhole's.
local function measured_tan_v(pc, loc, rot, now)
    if cache.ortho or now < cache.project_next then return nil end
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
    if t ~= nil then
        cache.project_misses, cache.said_no_projection = 0, false
        return t
    end
    cache.project_misses = cache.project_misses + 1
    if cache.project_misses >= 120 then
        cache.project_next = now + 1
        if not cache.said_no_projection then
            cache.said_no_projection = true
            log("the game does not say where it draws (%s): her fov comes from its settings, asked again once a second",
                failed and tostring(cache.project_error) or "no answer")
        end
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
    local tan_v = measured_tan_v(pc, loc, rot, now)
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
            if his_motion.actor ~= actor then his_motion = { actor = actor } end -- another actor: no speed from the switch
            local vx, vy = M.real_velocity(his_motion, at.X, at.Y, vel.X, vel.Y, now)
            p = { x = at.X, y = at.Y, z = at.Z - (cache.half_height or 0), yaw = prot.Yaw, vx = vx, vy = vy, falling = falling,
                half_height = cache.half_height,
                camera_dist = math.sqrt((loc.X - at.X) ^ 2 + (loc.Y - at.Y) ^ 2 + (loc.Z - at.Z) ^ 2) }
            source, tracer_actor = (actor == pawn and "pawn" or "view target"), actor
        else
            actor = nil -- stopped answering mid-frame (gone): fall through to the camera-only placement
        end
    end
    if actor == nil then
        -- no actor for the player at all: place her relative to the camera - her spot (M.follow's BACK/SIDE,
        -- the same fixed distance a Pawn gets) is behind-and-beside the camera, ground traced from it.
        -- His velocity: the camera's own frame-to-frame movement (there is no actor to read one from). A
        -- third-person camera moves whenever the view only turns, and camera motion alone cannot tell an orbit
        -- round a still player from a player stepping sideways while the view turns (review r3 of 0.2.5: a
        -- two-ray pivot estimate failed both slow pans and strafing) - so this speed only counts as going past
        -- the higher CAMERA_GO_START (from_camera). A pawnless game wants a provider of its own (astra-gta-trilogy).
        local vx, vy = M.real_velocity(cam_motion, loc.X, loc.Y, nil, nil, now)
        p = { x = loc.X, y = loc.Y, z = loc.Z, yaw = rot.Yaw, vx = vx, vy = vy, falling = false, from_camera = true }
        source, tracer_actor = "camera", pcm
    end
    -- her route to the player's vicinity (never the exact spot - a small approximation; M.nav_goal only uses
    -- a path point once it is further than NAV_LOOKAHEAD away, so the BACK/SIDE offset is lost in the noise)
    local nav = nav_update(tracer_actor, her.x, her.y, her.z, p.x, p.y, p.z, now)
    M.follow(her, p, dt, now, trace_down(tracer_actor), capsule_sweep(tracer_actor, p.half_height), nav)
    -- her placement first, the camera last (the engine draws when the camera comes)
    if send("V 1\n" .. sun_line(now, tracer_actor) .. M.avatar_line(her) .. M.camera_line(frame, loc, rot, send_fov, send_horizontal, cache.aspect, cache.near)) then
        stats.sent = stats.sent + 1
    end
    stats.frames = stats.frames + 1
    stats.cost = stats.cost + (os.clock() - t0)
    if now - stats.logged_at >= 10 then
        stats.logged_at = now
        local t = her.trace or {}
        log("frame %d  camera %.0f %.0f %.0f  pitch %.1f yaw %.1f roll %.1f  fov %.1f%s, drawn %s  %s %.0f %.0f %.0f%s  her %.0f %.0f %.0f  ground %s (%d traces, %d missed, %d objects)  collide %s (%d blocked, %d stuck)  %.3f ms/frame",
            frame, loc.X, loc.Y, loc.Z, rot.Pitch, rot.Yaw, rot.Roll, fov, cache.horizontal and "h" or "v",
            tan_v ~= nil and string.format("%.1fv", send_fov) or "not measured", source, p.x, p.y, p.z,
            p.falling and " (falling)" or "", her.x, her.y, her.z, t.z ~= nil and string.format("%.0f", t.z) or "the player's", t.traces or 0,
            t.misses or 0, t.objects or 0, sweep_broken and "off (threw)" or (capsule_sweep(tracer_actor, p.half_height) and "on" or "off"),
            her.blocked_frames or 0, her.stuck_teleports or 0, stats.frames > 0 and stats.cost * 1000 / stats.frames or 0)
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
