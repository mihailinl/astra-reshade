-- GTA III - The Definitive Edition (Steam appid 1546970, Gameface/Binaries/Win64/LibertyCity.exe, Rockstar's
-- modified UE 4.26 "Gameface"). RE-UE4SS experimental 3.0.1-1164-g5e627997's own pattern scan finds TWO
-- candidates and cannot tell which is right (UE4SS.log): "Failed to find StaticConstructObject_Internal:
-- found 2 unique values [140C89270, 141A2B330]".
--
-- 0x140C89270 is the OLD multi-argument overload - StaticConstructObject_Internal(UClass*, UObject*, FName,
-- EObjectFlags, EInternalObjectFlags, UObject*, bool, FObjectInstancingGraph*, bool): its body zero-fills an
-- FStaticConstructObjectParameters on the stack, copies the arguments into it, does one ensure()-style check,
-- then tail-calls the one-argument overload at 0x141A2B330 with &Params in rcx.
--
-- 0x141A2B330 IS StaticConstructObject_Internal(const FStaticConstructObjectParameters& Params) - the
-- canonical implementation UE4SS wants (its hook installs with this single-struct-pointer calling
-- convention; UObjectGlobals::SetupStaticConstructObjectInternalAddress expects the function that does the
-- actual construction work, not a thin forwarder). Confirmed by reading its body (rea/objdump on a scratch
-- copy of the exe, never the installed one):
--   mov rsi, [rcx]        ; Params.Class  (offset 0 of FStaticConstructObjectParameters)
--   mov r15, [rcx+8]      ; Params.Outer
--   mov r14d, [rcx+0x18]  ; Params.SetFlags
--   mov rbp, [rcx+0x28]   ; Params.Template
--   test dword [rsi+0xcc], 0x10000080   ; Class->ClassFlags & (two class-flag bits)
--   ... (the ensure()/IsInGameThread() dance 4.26's real implementation does) ...
--   call [rax+0xb0]       ; through the class vtable: StaticAllocateObject-style dispatch
-- then returns the constructed UObject*.
--
-- Uniqueness proof (tools/, 2026-10-17): this exact 79-byte AOB was searched across the WHOLE exe
-- (.text/.rdata/.data/.pdata/.msvcjmc/_RDATA/.rsrc/.reloc/.rkstr - every section objdump -h lists) and
-- matched EXACTLY ONCE, at 0x141A2B330 - the address this file names. The 4 wildcarded bytes are the
-- RIP-relative displacement of the __security_cookie load (stable across runs of this same build; wildcarded
-- only so a future patch that moves the cookie global does not silently point this file at the wrong byte).
--
-- Vice City / San Andreas DE are NOT installed on this machine (their ViceCity.exe / SanAndreas.exe were not
-- available to scan) - this file is NOT claimed to work for them. It is scoped to GTA III DE's exe only; see
-- ../../README.md for why it ships keyed to this one game, not in the shared ue4ss-overlay/.
function Register()
    return "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 56 41 57 48 81 EC A0 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 98 01 00 00 48 8B 31 48 8B D9 4C 8B 79 08 44 8B 71 18 48 8B 69 28 F7 86 CC 00 00 00 80 00 00 10 0F 84 AF 00 00 00"
end

function OnMatchFound(MatchAddress)
    return MatchAddress
end
