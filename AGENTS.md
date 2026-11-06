# Thief (2014) third-person camera mod

Standalone dinput8.dll proxy mod for Shipping-ThiefGame.exe (64-bit UE3, Eidos Montréal fork).
Runs under Wine/Proton on CachyOS (fish shell). Read NOTES.md before changing hooks or offsets.

## Environment
- Repo root and working directory: /home/jackedhawk117/reverse-engineering/Thief 2014 (path contains a space, always quote it)
- The original game exe copy lives in this same folder. NEVER modify or commit it: check .gitignore before any `git add`, and use specific filenames rather than `git add .`

## Build
- `make clean && make` cross-compiles the PE64 DLL (clang + lld + Wine headers)
- `make dist` builds the release zip
- Test: copy dinput8.dll + thief_camera.ini into Binaries/Win64/ with WINEDLLOVERRIDES="dinput8=n,b"

## Reverse engineering tools
- MCP servers: `ghidra` (needs Ghidra running with the program loaded, port 8080) and `rea`
- Do not modify or distribute game binaries. Work on the copy only.

## Key facts (see NOTES.md for the full list)
- Camera hook: call site in ULocalPlayer::CalcSceneView (0x1403F6D5E), offsets the render view only;
  gameplay queries still originate at Garrett's head
- Collision: UWorld::LineCheck (0x140397520)
- Meshes: hero->Mesh (cape), Mesh2 (body), Mesh3 (head/hood)
- Native ShowThirdPersonMesh hides Mesh2 sections, so use ShowAllMaterialSections + SetMaterialSectionHidden
- Always validate pointers with VirtualQuery; crashes happen on loads and menu transitions

## General rules
- Record every new offset/RVA in thief_structs.h AND NOTES.md with how it was found
- Verify decompiler findings with a second source (REA or live testing) before hooking
- Keep F5 (QuickSave) untouched; toggle is F4, shoulder swap F3
- Do not commit unless asked; stage specific source files only

## Current goal: swap/replace Garrett's animations with existing in-game animations
- Find what animation system the game uses (UE3 AnimSets, AnimTree/AnimNodes, slot nodes, PlayAnim-style calls) and how Garrett's body mesh (Mesh2) is driven in third person
- Build an inventory of existing animation sequences (names, AnimSet, which gameplay state uses them)
- Add a mechanism to override a gameplay state's animation with another existing one (config-driven via thief_camera.ini)

## Animation rules
- Only swap between animations that share Garrett's skeleton (same bone track names); verify before swapping
- Prefer hooking/redirecting the animation selection call over patching asset data
- Log-first: add a debug logging mode that prints sequence names as the game plays them, before any override
- Record every class layout, offset and RVA in thief_structs.h AND NOTES.md (new "Animation" section), noting how each was found
- Keep first-person arm animations intact; do not break lockpicking, takedowns or context interactions
- Treat all function names/offsets guessed from UE3 knowledge as hypotheses until confirmed in the binary (Eidos forked the engine)
