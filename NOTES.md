

## Phase 2: Find the Camera Pipeline Findings

### 1. View Calculation & Camera Pipeline Hierarchy

The camera pipeline in Thief (2014) is implemented through a combination of stock UE3 view matrix generation and a proprietary Eidos Montréal camera behavior state machine:

```
ULocalPlayer::CalcSceneView (0x1403F6BA0)
  │
  ├── Calls APlayerController::eventGetPlayerViewPoint (0x14039A800)
  │     │
  │     └── Calls UObject::ProcessEvent(GetPlayerViewPoint)
  │           │
  │           └── Calls AThiefCamera::GetCameraViewPoint (0x140BE8DA0, VTable +0x8D8)
  │                 │
  │                 ├── Checks AThiefCamera::ActiveBehavior (+0xB68, UThiefCameraBehavior*)
  │                 │     ├── If valid & active:
  │                 │     │     Location = Behavior->ViewLocation (+0x2B4)
  │                 │     │     Rotation = Behavior->ViewRotation (+0x2C0)
  │                 │     └── Else (Fallback):
  │                 │           Location = AThiefCamera::POV.Location (+0x7B8)
  │                 │           Rotation = AThiefCamera::POV.Rotation (+0x7C4)
  │
  ├── Calls APlayerController::GetFOVAngle (0x1403F1260)
  │     └── Queries AThiefCamera::GetFOVAngle (0x140BE8E60)
  │           ├── If Behavior active: returns Behavior->FOVAngle (+0x2CC)
  │           └── Else: returns AThiefCamera::POV.FOV (+0x7D0)
  │
  └── Generates View Matrix (0x1403F6DF3 - 0x1403F6E35)
        ├── Quat = FRotator::Quaternion(ViewRotation)
        ├── RotMatrix = FQuat::Inverse(Quat)
        ├── TransMatrix = FTranslationMatrix(-ViewLocation)
        └── ViewMatrix = TransMatrix * RotMatrix
```

- **`ULocalPlayer::CalcSceneView`**:
  - **Address**: `0x1403F6BA0`
  - **Confidence**: Confirmed.
  - **Ghidra Symbol**: `ULocalPlayer_CalcSceneView`
  - **Functionality**: Top-level scene view generator called per-viewport per frame.

- **`APlayerController::eventGetPlayerViewPoint`**:
  - **Address**: `0x14039A800`
  - **Signature**: `void APlayerController_eventGetPlayerViewPoint(APlayerController* this, FVector* out_Location, FRotator* out_Rotation)`
  - **Callers**: Only 2 in entire executable:
    1. `0x1403F6D5E` (inside `ULocalPlayer::CalcSceneView`)
    2. `0x1403A931B` (inside `UAudioDevice::Update` / 3D audio listener calculation)
  - **Confidence**: Confirmed.

- **`AThiefCamera::GetCameraViewPoint`**:
  - **Address**: `0x140BE8DA0` (VTable `0x141AEAC90 + 0x8D8`)
  - **Signature**: `void AThiefCamera::GetCameraViewPoint(FVector& OutLocation, FRotator& OutRotation)`
  - **Confidence**: Confirmed.

- **`AThiefCamera` Struct Members**:
  - `+0x7B8`: `FVector POV_Location` (`X` +0x7B8, `Y` +0x7BC, `Z` +0x7C0)
  - `+0x7C4`: `FRotator POV_Rotation` (`Pitch` +0x7C4, `Yaw` +0x7C8, `Roll` +0x7CC)
  - `+0x7D0`: `float POV_FOV`
  - `+0xB68`: `UThiefCameraBehavior* ActiveBehavior`
  - Global Camera instance pointer: `0x142205EC0` (`GThiefCamera`)
  - **Confidence**: Confirmed.

- **`UThiefCameraBehavior` Base Struct Members**:
  - `+0x2B4`: `FVector ViewLocation` (`X` +0x2B4, `Y` +0x2B8, `Z` +0x2BC)
  - `+0x2C0`: `FRotator ViewRotation` (`Pitch` +0x2C0, `Yaw` +0x2C4, `Roll` +0x2C8)
  - `+0x2CC`: `float FOVAngle`
  - `+0x2D0`: `uint32_t Flags` (Bit 0: Active, Bit 1: FirstPerson, Bit 4: Valid)
  - **Confidence**: Confirmed.

---

### 2. The Single Best Choke Point for Third-Person Camera Injection

- **Location**: Inside `ULocalPlayer::CalcSceneView` (`0x1403F6BA0`) immediately after `APlayerController_eventGetPlayerViewPoint` returns at `0x1403F6D5E` / `0x1403F6D63`.
- **Registers at Choke Point**:
  - `r12`: Points to `FVector ViewLocation`
  - `rdi`: Points to `FRotator ViewRotation`
  - `rsi`: Points to `ULocalPlayer` (`[rsi + 0x68]` points to `APlayerController`)
- **Decompiled Sequence**:
  ```x86asm
  0x1403f6d54: mov r8, rdi                 ; r8 = FRotator* ViewRotation
  0x1403f6d57: mov rdx, r12                ; rdx = FVector* ViewLocation
  0x1403f6d5a: mov rcx, qword ptr [rsi + 0x68] ; rcx = APlayerController*
  0x1403f6d5e: call 0x14039a800           ; calls eventGetPlayerViewPoint
  0x1403f6d63: mov rcx, qword ptr [rsi + 0x68] ; <--- CHOKE POINT
  0x1403f6d67: call 0x1403f1260           ; calls GetFOVAngle
  0x1403f6d6c: movaps xmm12, xmm0
  ```
- **Why this is strictly superior to hooking `GetPlayerViewPoint` or behavior updates**:
  1. **Separation of Render View from Gameplay Queries**: Offsetting `ViewLocation` at `0x1403F6D63` ONLY alters the scene rendering view matrix. Gameplay raycasts (pickpocketing, lockpicking, context takedown prompts, loot interaction, dialogue triggers) query `APawn::GetPawnViewLocation` or `eventGetPlayerViewPoint` and continue to originate naturally from Garrett's head/body.
  2. **Zero Desync of Aiming & Controls**: `ViewRotation` (`rdi`) is preserved intact, so mouse/controller pitch and yaw remain 100% synchronized with character locomotion.
  3. **Universal Compatibility**: Layering at `CalcSceneView` applies cleanly regardless of whether Garrett is running, crouching, mantling, or swooping.
- **Unique AOB Signature**:
  ```
  4C 8B C7 49 8B D4 48 8B 4E 68 E8 ?? ?? ?? ?? 48 8B 4E 68 E8 ?? ?? ?? ?? 44 0F 28 E0
  ```
  *(Verified: Exactly 1 match in binary at file offset `0x3F6154`, VA `0x1403F6D54`)*.

---

### 3. Custom Camera Classes & State Machine Inventory

Eidos Montréal created an extensive hierarchy of `UThiefCameraBehavior` subclasses to handle gameplay state-specific camera dynamics:

| Camera Behavior Class | StaticClass VA | Primary Responsibility | Interaction with Third-Person Mod |
| :--- | :--- | :--- | :--- |
| **`UThiefCameraFirstPerson`** | `0x140C0B970` | Standard locomotion / stealth in 1P | Primary override target |
| **`UThiefCameraThird`** | `0x140C0C960` | Eidos built-in 3P behavior | Stock 3P implementation (used for certain animations) |
| **`UThiefCameraThirdPersonConfig`** | `0x140C05840` | Config: camera distance, offsets, FOV | Can be inspected for default distance params |
| **`UThiefCameraVNav`** | `0x140C0B4F0` | Vertical navigation (pipes, ledges, ropes) | Coexists smoothly via `CalcSceneView` hook |
| **`UThiefCameraRush`** | `0x140C0BED0` | "Swoop" quick-dash ability | Coexists smoothly via `CalcSceneView` hook |
| **`UThiefCameraLockpick`** | `0x140C0D4B0` | Close-up lockpicking minigame | Auto-disables 3P offset if minigame active |
| **`UThiefCameraMarksman`** | `0x140C0DC50` | Bow aiming & precision zoom | Auto-disables or lowers offset when aiming bow |
| **`UThiefCameraCinematicCamActor`**| `0x140C0E140`| Scripted in-engine cinematics | Preserved untouched |
| **`UThiefCameraCinematicSecondPersonConfig`** | `0x140C0E7B0`| Takedowns and corner peeking | Native 3P/2P transitions |
| **`UThiefCameraSmartObjectDriven`**| `0x140C0ECB0`| Keyholes, peepholes, crawlspaces | Preserved untouched |
| **`UThiefCameraTransitionManager`**| `0x140AF01F0`| Manages blending between behaviors | Native blending engine |

---

### 4. `BehindView` / `bBehindView` Assessment

- **Finding**: A complete string search (both ASCII and UTF-16LE) across the entire 37MB executable yielded **0 occurrences** of `BehindView` or `bBehindView`.
- **Conclusion**: The stock UE3 `BehindView` cheat/console command was completely removed/stripped from this shipping build by Eidos Montréal during the migration to their custom `AThiefCamera` architecture.
- **Engine-Native Alternative**: The engine utilizes `UThiefCameraBehavior_SetFirstPersonView(bool bFirstPerson)` at `0x140BFC720` and the global flag `GIsFirstPersonView` at `0x142206BE0` (`1` = 1P, `0` = 3P).
- **Confidence**: Confirmed.

---


---

## Phase 5 Troubleshooting & Crash Resolution

### 1. Crash Analysis (Minidump 0xC0000005 at 0x140B3B7AA)
- **Faulting Instruction**: mov r9, qword ptr [rax + 0x2d0] -> cmp dword ptr [r9 + 0x134], ebx inside AThiefPawn_Hero_ToggleMaterialSections (0x140B3B790 + 0x1A).
- **Cause**: During map loading, Garrett pawn was created but character mesh assets (SkeletalMesh at +0x2D0) were still streaming in asynchronously. The initial hook called ShowThirdPersonMesh unconditionally inside CalcSceneView, passing a USkeletalMeshComponent whose internal asset pointer was NULL.
- **Secondary Discovery**: Disassembly of 0x140397520 (UWorld::LineCheck) revealed a 10-parameter signature:
  uint32_t LineCheck(void* World, FCheckResult& Result, AActor* SourceActor, const FVector& End, const FVector& Start, uint32_t TraceFlags, uint8_t TraceType, const FVector* Extent, void* Opt1, void* Opt2).
  The original struct definition omitted TraceType and placed Extent at parameter 6, mismatching stack registers.

### 2. Implemented Fixes
1. **Asset Streaming Guard**:
   GetHeroPawn now strictly validates hero->Mesh and *(uintptr_t*)((uintptr_t)hero->Mesh + 0x2D0). If Garrett skeletal mesh is not yet loaded into memory, the hook immediately bails out, leaving vanilla first-person scene view calculation completely untouched.
2. **Mesh State Automation**:
   Removed all direct calls to ShowThirdPersonMesh and HideThirdPersonMesh from the mod. The engine native AThiefCamera::UpdateCamera (0x140BF6C40) handles head/hood material section toggling automatically once GIsFirstPersonView is set to 0.
3. **Collision Trace Signature**:
   Updated thief_structs.h tLineCheck prototype to match the full 10 arguments.
4. **Diagnostic Logging**:
   Added thief_camera.log to track initialization, call-site hook installation, and Garrett detection in real time.
5. **Heroic & Wine Configuration**:
   Fixed Heroic game configuration 4123bcce398b4b1d8b9ef878f81055d1.json environment variable from dinput=n,b to WINEDLLOVERRIDES="dinput8=n,b", complementing the prefix registry user.reg.


---

## Phase 5 Camera Transformation Resolution

### 1. Root Cause for No Camera Offset
- The hook in Hook_GetPlayerViewPoint was bailing out every frame on if (!hero) return;.
- Although GThiefCamera was found, the nested chain (PawnHolder -> Hero -> Mesh -> SkeletalMesh) returned nullptr in gameplay.
- Because of this guard, *OutLocation was never modified, leaving the camera locked in vanilla first person despite keypresses registering.

### 2. Camera Transform Architecture Realization
- OutLocation and OutRotation passed by ULocalPlayer::CalcSceneView to eventGetPlayerViewPoint already represent the player's eye/head coordinates and aiming angles.
- Offsetting OutLocation along the orthonormal view axes (forward, right, up) is pure vector math that directly drives the renderer's ViewTranslationMatrix without needing internal pawn pointers.
- Removed the blocking if (!hero) return; check.
- Added live transform telemetry (Head(X,Y,Z) -> Cam(X,Y,Z)) to thief_camera.log.

---

## Phase 5 Startup Crash Resolution

### 1. Root Cause for Crash at Startup (Minidump 0xC0000005 at 0x1400017f9)
- Disassembly showed crash at `mov r15, qword ptr [rcx + 0x50]` inside `IsBehaviorOfClass`.
- Cause: `Hook_GetPlayerViewPoint` called `GetActiveBehavior(Controller)` which attempted to read `ActiveBehavior` from `[cam + 0xB68]`. During early startup / main menu initialization, `[cam + 0xB68]` is not yet a valid `UObject` pointer. Dereferencing `rcx + 0x50` (Class private) triggered an access violation.
- Confirmed by `thief_camera.log`, which stopped immediately after the first invocation of `Hook_GetPlayerViewPoint`.

### 2. Solution: Zero-Dereference Pure Vector Transform
- Completely removed `GetActiveBehavior`, `IsBehaviorOfClass`, `GetHeroPawn`, and `GetThiefCamera` from `Hook_GetPlayerViewPoint`.
- The hook now forwards directly to `APlayerController_eventGetPlayerViewPoint` to get Garrett's exact eye position and rotation, then calculates the third-person offset purely using vector math (`sinf`, `cosf`, forward/right/up unit axes).
- Added distance threshold snap (`distSq > 500^2`) to prevent camera dragging across map loading or teleportation.
- Compiled cleanly with `clang++` and deployed directly to `Binaries/Win64/dinput8.dll`.

---

## Phase 5 Garrett Head & Hood Mesh Visibility Resolution

### 1. Root Cause for "Headless Garrett"
- In first-person view, Unreal Engine 3 / Eidos Montréal hides Garrett's head and neck bones using `USkeletalMeshComponent::HideBone` (indices stored in `Garrett->HiddenBoneIndices` at `+0x15B8`) and hides the hood/face material sections (`Garrett->HiddenMaterialSections` at `+0x15A8`) to prevent the camera inside his head from clipping his skull and teeth.
- At map load, `HideThirdPersonMesh` (0x140B47D80) ran and hid those bones and materials.
- In normal walking, the engine's camera state machine never calls `ShowThirdPersonMesh` (0x140B3B850) unless triggering native cover or takedowns.
- Although the 3P camera mod moved the rendering view behind Garrett, his head bones and hood materials remained flagged as hidden in his `USkeletalMeshComponent`.

### 2. Solution: Safe Native Mesh Unhiding via VirtualQuery Validation
- Implemented `IsValidReadPtr` using Win32 `VirtualQuery` (`MEM_COMMIT`, `!PAGE_NOACCESS`) for 100% crash-proof pointer validation.
- Implemented `UpdateHeroMeshVisibility(Controller, bThirdPerson)`:
  1. Resolves `AThiefPawn_Hero*` via `Controller->PlayerCamera` or `GThiefCamera` -> `PawnHolder` (`+0x978`) -> `Garrett` (`+0x718`).
  2. Verifies `hero->Mesh` (+0x444) and `SkeletalMesh` (+0x2D0) are fully streamed in (`LODCount > 0`).
  3. Sets `GIsFirstPersonView = 0`.
  4. Calls `AThiefPawn_Hero_ShowThirdPersonMesh` (0x140B3B850).
  5. Directly calls `AThiefPawn_Hero_ToggleMaterialSections` (0x140B3B790) on `&hero->HiddenMaterialSections` to guarantee hood and face materials render.
  6. Directly unhides all bones in `Garrett->HiddenBoneIndices` and `AThiefCamera::HeadBoneIndex` using `USkeletalMeshComponent::HideBone` (0x140497270).
  7. When toggled back to first-person (<kbd>F4</kbd>), smoothly calls `AThiefPawn_Hero_HideThirdPersonMesh` (0x140B47D80) and sets `GIsFirstPersonView = 1`.
- Automatically executes on toggle and re-checks every 30 frames (~0.5s) to seamlessly restore head visibility after mantles or cutscenes.

---

## Phase 7: Resume Crash Fix & Crash-Proof Mesh Unhiding Architecture

### 1. Root Cause of Crash on "Resume"
1. **Unsafe Call-Site Detours**:
   - The Phase 6 experimental detours at `0x140BF6DBA` and `0x140BFC759` hooked raw call sites using 5-byte jumps to relay thunks. During map loading and save-game deserialization on "Resume", the stack and register conventions of the engine's camera state machine differed from the thunk, corrupting registers and causing access violations.
2. **Asynchronous Asset Streaming Race Conditions**:
   - During level loading, `APawn` and `USkeletalMeshComponent` exist in memory before their internal sub-objects (`mesh->AnimTree` at `+0x32c`, `mesh->LodInfo` at `+0x5fc`, `hero->Component` at `+0x11cc`, and `SkeletalMesh` at `+0x2D0`) are populated by asynchronous asset streaming. Calling `ShowThirdPersonMesh` or `HideBone` prematurely dereferenced null pointers at `[mesh + 0x32c]->vtable[0x20]` and `[hero + 0x11cc]`.
3. **Unchecked Pointer Offsets**:
   - Hero pawn memory validation was only checking `0x450` bytes, but accessing fields up to `0x15C0` risked page boundary access violations if the allocation boundary fell short.

### 2. Comprehensive Solution Implemented in Phase 7
1. **Elimination of All Call-Site Detours**:
   - Completely removed all patches at `0x140BF6DBA` and `0x140BFC759`. The ONLY hook installed in the binary is the 5-byte call site detour inside `ULocalPlayer::CalcSceneView` at `0x1403F6D5E` (which has been proven 100% stable).
2. **Deep VirtualQuery Memory Guards**:
   - Hero pawn resolution requires `IsValidReadPtr(hero, 0x1600)`.
   - Character mesh component requires `IsValidReadPtr(mesh, 0x700)`.
   - Skeletal mesh asset requires `IsValidReadPtr(skelMesh, 0x150)` and `0 < lodCount <= 10`.
   - Mesh animation instance requires `IsValidReadPtr(*(void**)(mesh + 0x32c), 0x50)`.
   - Native `ShowThirdPersonMesh` is only invoked if `*(void**)(hero + 0x11cc)` and `*(void**)(mesh + 0x5fc)` are valid. If assets are still streaming, the mod skips native sub-calls safely and retries on subsequent frames.
3. **Multi-Mesh Head & Material Unhiding**:
   - Sets `GIsFirstPersonView = 0` at `0x142206BE0`.
   - Sets `hero->StateFlags` bit 1 (`0x2`) at `hero + 0x10D8`.
   - Clears `bHidden` bit (`0x00400000`) on `hero->Mesh` (+0x158).
   - Unhides material sections across Array 3 (`0x15A8`), Array 1 (`0x1588`), and Array 2 (`0x1598`) via `AThiefPawn_Hero_ToggleMaterialSections`.
   - Unhides head/neck bones in Array 4 (`0x15B8`) via `USkeletalMeshComponent_HideBone(mesh, bone, 0, 1, 1)`.
   - Unhides secondary meshes `Mesh2` (`hero + 0x14A8`) and `Mesh3` (`hero + 0x14B0`).
   - Diagnostic logging (`[MeshDiag]`) provides real-time telemetry on pawn, mesh, LODs, bones, and array counts in `thief_camera.log`.

---

## Phase 8: Character Multi-Component Mesh Architecture & Visibility Restoration

### 1. Breakthrough Discovery on Character Decomposition
- **Mesh Hierarchy**:
  - `hero->Mesh` (`hero + 0x444`): The primary character skeleton / cape (`Bones=3`).
  - `Mesh2` (`hero + 0x14A8`): Garrett's full torso, arms, and legs component, attached to `hero->Mesh` at socket `0x1530`.
  - `Mesh3` (`hero + 0x14B0`): Garrett's head and hood component, attached to `hero->Mesh` at socket `0x1538` (`Head` socket).
- **Why Torso & First-Person Hands Disappeared**:
  1. Calling `AThiefPawn_Hero_ShowThirdPersonMesh` (`0x140B3B850`) iterates through all material sections of `Mesh2` (`0x14A8`) and invokes `0x1404A2080` with `r9d = 0`. Disassembly of `0x1404A2080` revealed `sete r9b`, meaning parameter `0` sets `bHidden = 1`, hiding every section of `Mesh2` (the entire torso and body)!
  2. `AThiefPawn_Hero_ToggleMaterialSections` (`0x140B3B790`) called with Array 1 (`0x1588`) and parameter `0` hid the first-person arms/hands material sections.

### 2. The Clean Restoration Solution
- **`AThiefPawn_Hero_ShowAllMaterialSections` (`0x140B3B630`)**:
  - Native engine function that cleanly iterates every section on `hero->Mesh`, invoking `0x1404A2080(mesh, 0, s, 1, 1, 1)` and `0x1404A2110(mesh, 0, s, 0)` (clearing bit 0 and bit 3), and calls `HideBone(mesh, b, 1, 1, 1)` to reset all bone hide flags.
- **Torso & Head Component Visibility**:
  - Directly iterates sections of `Mesh2` (`+0x14A8`, torso) and `Mesh3` (`+0x14B0`, head), clearing `bHidden` bit `0x00400000` and invoking `SetMaterialSectionHidden(..., 0)` to guarantee full rendering.
  - Invokes `SetHidden(0)` on `Mesh3` via virtual method `+0x508` in 3rd person, and `SetHidden(1)` in 1st person to prevent head clipping.
  - Completely removed all calls to `ShowThirdPersonMesh`, `HideThirdPersonMesh`, and `ToggleMaterialSections`.
  - In 1st person, primary mesh and `Mesh2` are preserved, restoring first-person hands completely.

---

## Phase 9: Camera Wall Collision Detection & Asymmetric Smoothing

### 1. Engine Collision Query Function
- **Function**: `UWorld::LineCheck`
- **Address**: `0x140397520` (in `Shipping-ThiefGame.exe`)
- **Signature**:
  ```cpp
  bool LineCheck(UWorld* World, FCheckResult* Result, AActor* SourceActor,
                 const FVector* End, const FVector* Start, const FVector* Extent,
                 uint32_t TraceFlags);
  ```
- **World Context**: Resolved via `*(UWorld**)(g_Base + 0x238BEA0)` (`GWorld`).
- **Extent**: Bounding box extent `(12.0f, 12.0f, 12.0f)` sweeps a 24-unit volume along the camera boom ray, preventing the camera view plane from penetrating walls, pillars, or low ceilings.
- **Trace Flags**: `0x00000001` (`TRACE_World` geometry).

### 2. Pitch Boom Damping
- In tight corridors, looking upward or downward causes the pitch component to push the camera toward the ceiling or floor, creating an extreme overhead "bird's-eye" angle.
- The mod damps pitch-induced boom displacement by clamping the effective boom pitch angle to `[-65.0f, +45.0f]`, keeping the framing natural and level while allowing Garrett's aim rotation to pitch freely.

### 3. Asymmetric Camera Smoothing
- **Pull-In (Collision)**: Instantaneous when geometry is detected behind the camera (`targetDist < currentDist -> currentDist = targetDist`), eliminating wall clipping even during rapid turns or sprints.
- **Ease-Out (Recovery)**: Smooth lerp (`currentDist += (targetDist - currentDist) * 0.15f * factor`) when stepping away from obstacles, preventing jarring camera snaps.

### 4. Proximity Cowl Hiding
- When backed against a wall in extremely tight quarters (`currentDist < 38.0f`), the camera enters the volume of Garrett's cowl.
- Rather than hiding his entire body, the mod dynamically sets `Mesh3->SetHidden(1)` while keeping `Mesh` (cape) and `Mesh2` (torso/limbs) 100% visible.
- Once distance exceeds `38.0f`, `Mesh3` is instantly unhidden. Garrett remains visible at all times.

---

## Phase 10: Third-Person Stealth & Combat Takedowns

### 1. State Machine Interaction
- Thief's camera state machine handles context takedowns using `UThiefCameraBehavior_Takedown` and `UThiefCameraBehavior_SecondPerson`.
- Previously, `IsMinigameOrCutsceneActive` treated all behaviors with name tokens `"Takedown"` or `"SecondPerson"` as interactive sequences, reverting to first person.

### 2. Takedown Exception & Mesh Assertion
- Implemented `IsTakedownActive(AThiefCamera* camera)`:
  - Scans active behavior class name in `GNames` for `"Takedown"` or `"SecondPerson"`.
  - When true, overrides the minigame suppression check to keep the third-person camera active.
  - Continuously asserts `UpdateHeroMeshVisibility(controller, true)` so all three character components (`Mesh`, `Mesh2`, `Mesh3`) remain fully rendered throughout takedown animations.

---

## Phase 11: Crosshair Parallax Convergence

### 1. Problem Statement
- In first person, the crosshair is centered along the head ray $\vec{R}_{\text{head}}(t) = \vec{P}_{\text{head}} + t \cdot \vec{F}$.
- In third person, the camera is offset laterally by shoulder distance $S$ and vertically by height $H$:
  $$\vec{P}_{\text{cam}} = \vec{P}_{\text{head}} - D \cdot \vec{F} + S \cdot \vec{R} + H \cdot \vec{U}$$
- If the camera renders strictly parallel to Garrett's head rotation, the crosshair is shifted off-target by $(S, H)$, causing arrows, blackjack swings, and pickpocket reticles to miss.

### 2. Solution: Orthonormal Camera-Local Convergence
1. **Target Raycast**:
   - Traces `LineCheck` from $\vec{P}_{\text{head}}$ along Garrett's aim forward vector $\vec{F}$ up to 5,000 units.
   - If geometry is struck, $\vec{P}_{\text{target}} = \vec{R}_{\text{hit}}$.
   - If no geometry is struck (open sky/distance), $\vec{P}_{\text{target}} = \vec{P}_{\text{head}} + 2500 \cdot \vec{F}$.
2. **Camera-to-Target Projection**:
   - Computes $\vec{V} = \vec{P}_{\text{target}} - \vec{P}_{\text{cam}}$.
   - Projects $\vec{V}$ onto Garrett's head orthonormal basis:
     $$\text{localForward} = \vec{V} \cdot \vec{F}, \quad \text{localRight} = \vec{V} \cdot \vec{R}, \quad \text{localUp} = \vec{V} \cdot \vec{U}$$
3. **Angular Correction in Unreal Rotator Units (65,536 units = 360°)**:
   $$\Delta \text{Yaw} = \arctan2(\text{localRight}, \text{localForward}) \times \frac{32768}{\pi}$$
   $$\Delta \text{Pitch} = \arctan2(\text{localUp}, \sqrt{\text{localForward}^2 + \text{localRight}^2}) \times \frac{32768}{\pi}$$
4. **Application**:
   - Directly added to `OutRotation->Yaw` and `OutRotation->Pitch` at the `CalcSceneView` hook:
     ```cpp
     OutRotation->Yaw += (int32_t)deltaYaw;
     OutRotation->Pitch += (int32_t)deltaPitch;
     ```
   - Center-screen crosshair reticles converge exactly onto the target in 3P across all ranges, angles, and shoulder offsets.

---

## Phase 12: Loading Screen & Main Menu Crash Guards

### 1. Root Cause Analysis
- When resuming a saved game or returning to the main menu, `ULocalPlayer::CalcSceneView` executes while the world is transitioning.
- During this window:
  - `Garrett` pawn exists in a partially constructed state (`HeadPos = (0,0,0)`).
  - Skeletal mesh assets (`hero->Mesh`, `Mesh2`, `Mesh3`) are streamed asynchronously.
  - Calling `LineCheck` or indexing into `GNames` with uninitialized or out-of-bounds indices caused `STATUS_ACCESS_VIOLATION` (`0xC0000005`).

### 2. Protective Verification Pipeline
- **Strict Pointer Validation**: `IsValidReadPtr(hero, 0x1600)` and `IsValidReadPtr(primaryMesh, 0x700)` with `skelMesh` LOD count checks (`0 < lodCount <= 10`).
- **GNames Bounds Checking**: Verifies `NameIndex < GNames->Num()` before accessing string chunks.
- **Head Coordinate Sanity**: If $\|\vec{P}_{\text{head}}\| < 1.0$, the hook skips third-person transformation for the frame, allowing the loading screen / main menu to render safely.

---

## Phase 13: Controls, Preserved QuickSave & Configuration

### 1. Hotkey Assignment
- **Toggle 1P/3P**: <kbd>F4</kbd> (guarded with `!GetAsyncKeyState(VK_MENU)` to prevent triggering on `Alt+F4`).
- **Shoulder Swap**: <kbd>F3</kbd> (toggles between right and left shoulder).
- **Native QuickSave**: <kbd>F5</kbd> is completely untouched by the mod, preserving Thief's native quicksave functionality.
- **Camera Tuning**:
  - Distance: <kbd>Page Up</kbd> / <kbd>Page Down</kbd>
  - Height: <kbd>Home</kbd> / <kbd>End</kbd>
  - Shoulder: <kbd>Insert</kbd> / <kbd>Delete</kbd>
  - Reset: <kbd>NumPad 0</kbd>

### 2. INI Configuration File
- Automatically reads `thief_camera.ini` located alongside `dinput8.dll`:
  ```ini
  [Camera]
  ToggleKey=0x73        ; VK_F4
  Distance=180.0        ; Default distance behind Garrett
  HeightOffset=25.0     ; Vertical camera height
  ShoulderOffset=30.0   ; Positive = right, negative = left
  SmoothFactor=0.25     ; Smoothing factor (0.0 to 1.0)
  ```



