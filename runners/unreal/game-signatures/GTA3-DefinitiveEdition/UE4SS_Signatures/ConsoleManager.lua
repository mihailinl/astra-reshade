-- GTA III - The Definitive Edition (Steam appid 1546970, Gameface/Binaries/Win64/LibertyCity.exe, Rockstar's
-- modified UE 4.26 "Gameface"). RE-UE4SS experimental 3.0.1-1164-g5e627997's own pattern scan finds TWO
-- candidates and cannot tell which is right (UE4SS.log): "Failed to find ConsoleManagerSingleton: found 2
-- unique values [1416EB630, 1426853B0]".
--
-- 0x1416EB630 IS ConsoleManagerSingleton (IConsoleManager::Singleton's lazy initializer) - confirmed by
-- reading its body (rea/objdump on a scratch copy of the exe, never the installed one):
--   cmp qword [rip+..], 0      ; the IConsoleManager::Singleton global (0x1450FFC38) - null-checked first
--   jne <already built>
--   mov rcx, [rip+..]          ; GMalloc - at 0x14540F8D0, the SAME address UE4SS.log's own line already
--                               ; reports ("GMalloc 0x14540f8d0"): this function allocates through it
--   mov edx, 0xf8               ; sizeof(FConsoleManager) (0xF8 bytes)
--   call [rax+0x10]              ; GMalloc->Malloc(0xF8, 0) through its vtable
--   ... placement-constructs FConsoleManager at the new block (its vtable stored at offset 0) ...
--   mov [rip+..], rdi           ; stores the pointer into IConsoleManager::Singleton (0x1450FFC38)
--   ... registers the engine's default console variables ...
--
-- 0x1426853B0 is a DIFFERENT function - a lazy per-cvar accessor. Its own body CALLS 0x1416EB630 first ("if
-- the singleton pointer at 0x1450FFC38 is null, call 0x1416EB630") before doing its own cvar lookup through
-- the manager's vtable - proof it is a CONSUMER of the real singleton, not the singleton itself.
--
-- Uniqueness proof (tools/, 2026-10-17): this exact 61-byte AOB was searched across the WHOLE exe
-- (every section objdump -h lists) and matched EXACTLY ONCE, at 0x1416EB630 - the address this file names.
-- The 4 wildcarded bytes are the RIP-relative displacement of the IConsoleManager::Singleton global load
-- (stable across runs of this same build; wildcarded only so a future patch that moves that global does not
-- silently point this file at the wrong byte).
--
-- Vice City / San Andreas DE are NOT installed on this machine - this file is NOT claimed to work for them.
-- See ../../README.md for why it ships keyed to this one game, not in the shared ue4ss-overlay/.
function Register()
    return "48 83 EC 38 48 83 3D ?? ?? ?? ?? 00 0F 85 5F 01 00 00 48 8B 0D 87 42 D2 03 48 89 7C 24 30 48 85 C9 75 0C E8 28 86 01 00 48 8B 0D 71 42 D2 03 48 8B 01 45 33 C0 BA F8 00 00 00 FF 50 10"
end

function OnMatchFound(MatchAddress)
    return MatchAddress
end
