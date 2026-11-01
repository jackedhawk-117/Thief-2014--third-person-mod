# Thief (2014) — Standalone Third-Person Camera Mod

A high-performance, standalone third-person camera modification for **Thief (2014)** (`Shipping-ThiefGame.exe`, 64-bit Unreal Engine 3 / Eidos Montréal custom fork). 

Implemented as a lightweight DirectInput 8 proxy DLL (`dinput8.dll`), this mod provides a complete, modern third-person experience with full player character model rendering, smooth camera tracking, shoulder swapping, and real-time offset tuning—without requiring external injectors or Cheat Engine.

---

## Highlights & Features

- **Seamless Perspective Toggle**: Switch between first-person and third-person perspectives instantly with <kbd>F4</kbd>.
- **Complete Garrett Model Rendering (No Missing Head or Torso)**:
  - Fixes the infamous "headless Garrett" issue common in rudimentary third-person camera hacks by resolving Unreal Engine 3's bone-hiding and material-section culling systems.
  - Correctly manages the multi-component character hierarchy (`hero->Mesh` cape, `Mesh2` torso/limbs, `Mesh3` head/hood).
  - Preserves first-person arms and hands when switching back to first person.
  - Automatically re-evaluates and maintains mesh visibility across mantling, swooping, cutscenes, and level transitions.
- **Dynamic Camera Controls**:
  - Shoulder swapping (<kbd>F5</kbd>) for peeking around tight corners.
  - Hotkeys for adjusting camera distance, height, and lateral shoulder offset on the fly.
  - Camera reset shortcut (<kbd>NumPad 0</kbd>) to return instantly to default framing.
  - Smooth camera interpolation and lag compensation.
- **Gameplay & Interaction Decoupling**:
  - Injected at `ULocalPlayer::CalcSceneView` (`0x1403F6D5E`), modifying only the scene render view matrix.
  - Target raycasts, pickpocketing, lockpicking, context takedowns, swooping, and dialogue prompts remain anchored to Garrett's character coordinates without desyncing.
- **Crash-Proof & Safe**:
  - Employs deep Win32 memory validation (`VirtualQuery`) to guard against asynchronous asset streaming races during level loading and save-game resumes.
  - Self-contained proxy with no external background processes or hooked drivers.

---

## Controls & Keybindings

| Key | Action | Description |
| :--- | :--- | :--- |
| <kbd>F4</kbd> | **Toggle 3P Mode** | Switch between Third-Person and First-Person camera |
| <kbd>F5</kbd> | **Shoulder Swap** | Toggle between right-shoulder and left-shoulder view |
| <kbd>NumPad 0</kbd> | **Reset Camera** | Reset distance, height, and shoulder offsets to defaults |
| <kbd>Page Up</kbd> | **Zoom In** | Decrease camera distance (bring camera closer) |
| <kbd>Page Down</kbd> | **Zoom Out** | Increase camera distance (push camera further away) |
| <kbd>Home</kbd> | **Height Up** | Increase camera vertical height |
| <kbd>End</kbd> | **Height Down** | Decrease camera vertical height |
| <kbd>Insert</kbd> | **Shoulder Right** | Shift camera position further to the right |
| <kbd>Delete</kbd> | **Shoulder Left** | Shift camera position further to the left |

---

## Installation

### Windows
1. Download or compile `dinput8.dll`.
2. Locate your *Thief* installation directory.
3. Place `dinput8.dll` inside the **`Binaries/Win64/`** folder (the directory containing `Shipping-ThiefGame.exe`):
   ```text
   Thief/
   └── Binaries/
       └── Win64/
           ├── Shipping-ThiefGame.exe
           └── dinput8.dll  <-- [Place here]
   ```
4. Launch the game normally.

### Linux / Steam Deck (Proton / Wine / Heroic)
1. Copy `dinput8.dll` into `Binaries/Win64/`.
2. Configure a DLL override for `dinput8` so Wine loads the mod DLL instead of the built-in library:
   - **Steam Launch Options**:
     ```bash
     WINEDLLOVERRIDES="dinput8=n,b" %command%
     ```
   - **Heroic Games Launcher**:
     - Game Settings → Environment Variables → Add `WINEDLLOVERRIDES` with value `dinput8=n,b`.
     - Alternatively, open Wine Configuration (`winecfg`) for the prefix → **Libraries** tab → type `dinput8` → click **Add** → ensure it is set to `(native, builtin)`.
3. Launch the game.

---

## Building from Source

### Linux (Clang Cross-Compilation)
The included `Makefile` cross-compiles a clean PE64 DLL using Clang and Wine headers:

```bash
# Prerequisites (Debian/Ubuntu/Arch/Fedora with Wine & Clang installed)
make clean
make
```

### Windows (MSVC / Visual Studio)
Compile `dllmain.cpp` and `camera_mod.cpp` as an x64 dynamic link library (`.dll`), linking against `kernel32.lib` and `user32.lib`.

---

## Technical Architecture & Reverse Engineering

A detailed technical breakdown of the reverse-engineering analysis is documented in [`NOTES.md`](NOTES.md). Key findings include:

### 1. View Calculation Choke Point
In *Thief (2014)*, the classic Unreal Engine 3 console command `BehindView` was stripped by Eidos Montréal in favor of a specialized `UThiefCameraBehavior` state machine.

Rather than fighting internal camera behavior classes, the mod hooks the top-level view generator inside `ULocalPlayer::CalcSceneView` at `0x1403F6D5E`:
```text
ULocalPlayer::CalcSceneView (0x1403F6BA0)
  │
  ├── Calls APlayerController::eventGetPlayerViewPoint (0x14039A800)
  │     └── [Call-Site Hook Installed Here]
  │           ├── Calculates orthonormal basis (Forward, Right, Up vectors)
  │           ├── Applies smooth offset & collision thresholding
  │           └── Updates Garrett mesh visibility states
  │
  └── Builds Scene ViewTranslationMatrix & ViewRotationMatrix
```
Because the offset is applied directly to the rendering viewpoint, gameplay queries (`eventGetPlayerViewPoint`, `GetPawnViewLocation`) continue to originate from Garrett's head, ensuring that context takedowns, pickpocketing, lockpicking, and shooting mechanics function without misalignment.

### 2. Multi-Component Character Hierarchy
Garrett's character model is decomposed into three distinct skeletal mesh components attached via sockets:
- **`hero->Mesh` (`hero + 0x444`)**: Primary skeleton and cloth cape.
- **`Mesh2` (`hero + 0x14A8`)**: Full body, torso, arms, and legs (attached at socket `0x1530`).
- **`Mesh3` (`hero + 0x14B0`)**: Head and hood mesh (attached at socket `0x1538`).

### 3. Mesh & Head Visibility Restoration
In first-person view, the engine hides Garrett's head/hood to prevent the camera inside his head from clipping skull geometry. Calling the engine's built-in `ShowThirdPersonMesh` (`0x140B3B850`) naively causes Garrett's torso and first-person hands to vanish because it invokes `USkeletalMeshComponent_ShowMaterialSection` with parameters that hide all sections on `Mesh2`.

The mod cleanly resolves character visibility by:
1. Invoking native `AThiefPawn_Hero_ShowAllMaterialSections` (`0x140B3B630`) to reset all bone hide and section hide flags.
2. Explicitly clearing `bHidden` bit (`0x00400000`) and invoking `SetMaterialSectionHidden(..., 0)` on `Mesh2` (torso) and `Mesh3` (head).
3. Toggling `Mesh3->SetHidden(0)` in 3rd person and `Mesh3->SetHidden(1)` in 1st person, rendering Garrett's complete model in 3P while preventing first-person camera clipping and preserving Garrett's hands.

---

## Project Structure

```text
.
├── camera_mod.cpp     # Third-person camera calculations, input handling & mesh visibility
├── camera_mod.h       # Mod configuration and exported interfaces
├── dllmain.cpp        # DirectInput 8 proxy exports and DLL lifecycle
├── thief_structs.h    # Unreal Engine 3 & Eidos Montréal class definitions, RVAs & prototypes
├── Makefile           # Cross-compilation recipe (Clang x86_64 PE)
├── dinput8.dll        # Compiled 64-bit proxy DLL ready for release
├── NOTES.md           # In-depth reverse-engineering findings (Phases 1–8)
└── rea_triage.yaml    # Binary triage metadata
```

---

## Disclaimer

This project is an unofficial fan modification created for educational and reverse-engineering research purposes. *Thief* is a registered trademark of Square Enix / Eidos Montréal. No proprietary assets or copyrighted game binaries are distributed with this project.
