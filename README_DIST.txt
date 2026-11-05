================================================================================
  THIEF (2014) — STANDALONE THIRD-PERSON CAMERA MOD
  Version: 1.0 (64-bit DirectInput 8 Proxy)
================================================================================

A high-performance, standalone third-person camera mod for Thief (2014)
(Shipping-ThiefGame.exe, 64-bit Unreal Engine 3 / Eidos Montreal custom fork).

Implemented as a lightweight DirectInput 8 proxy DLL (dinput8.dll).
No external injectors, memory trainers, or Cheat Engine required!

--------------------------------------------------------------------------------
1. FEATURES
--------------------------------------------------------------------------------
- Full Garrett Model Rendering:
  Garrett's primary skeleton, cape, torso, limbs, and head/cowl are 100% visible
  in third person. No missing head, torso, or hands!
- Dynamic Wall Collision Sweeping (LineCheck):
  Camera sweeps against walls, pillars, and low ceilings. Pulls in instantly
  to eliminate clipping, and smoothly glides back out into open space.
- Exact Crosshair Parallax Convergence:
  Center-screen targeting reticle converges onto the true line-of-sight target,
  ensuring arrows, bow aiming, and interaction markers land exactly on target.
- Third-Person Stealth & Combat Takedowns:
  Stay immersed in third person during takedowns without snapping to first person.
- Proximity Cowl Hiding:
  If backed up against a wall (<38 units), Garrett's cowl hides dynamically so
  the camera never clips inside his hood, while keeping his body fully visible.
- Preserved QuickSave:
  Mod toggle is on F4 (Alt+F4 protected) and shoulder swap is on F3, leaving
  F5 100% free for Thief's native QuickSave!
- INI Configuration:
  Easily customize default distance, height, shoulder offset, smoothing, and hotkeys.

--------------------------------------------------------------------------------
2. CONTROLS & KEYBINDINGS
--------------------------------------------------------------------------------
  F4          - Toggle Third-Person / First-Person mode (Alt+F4 safe)
  F3          - Shoulder Swap (Toggle Left / Right shoulder)
  F5          - Native QuickSave (Unmodified, works as usual)
  NumPad 0    - Reset camera distance, height, and shoulder to defaults
  Page Up     - Zoom In (Decrease camera distance)
  Page Down   - Zoom Out (Increase camera distance)
  Home        - Raise camera height
  End         - Lower camera height
  Insert      - Shift shoulder offset right
  Delete      - Shift shoulder offset left

--------------------------------------------------------------------------------
3. INSTALLATION
--------------------------------------------------------------------------------
=== WINDOWS ===
1. Locate your Thief installation directory.
   Example:
     C:\Program Files (x86)\Steam\steamapps\common\Thief\
2. Place both 'dinput8.dll' and 'thief_camera.ini' inside the 'Binaries\Win64\'
   folder alongside 'Shipping-ThiefGame.exe':
     Thief\Binaries\Win64\dinput8.dll
     Thief\Binaries\Win64\thief_camera.ini
3. Launch the game normally. Press F4 in-game to switch to 3rd person!

=== LINUX / STEAM DECK (Proton / Wine / Heroic) ===
1. Copy 'dinput8.dll' and 'thief_camera.ini' into the game's 'Binaries/Win64/' folder.
2. Enable the native DLL override so Proton loads the mod:
   - Steam Launch Options:
       WINEDLLOVERRIDES="dinput8=n,b" %command%
   - Heroic Games Launcher:
       Settings -> Environment Variables -> Add WINEDLLOVERRIDES with value dinput8=n,b
       (or in winecfg -> Libraries tab -> add 'dinput8' set to 'native, builtin')
   - Lutris:
       Runner options -> DLL overrides -> Key: dinput8, Value: n,b
3. Launch the game!

--------------------------------------------------------------------------------
4. CONFIGURATION (thief_camera.ini)
--------------------------------------------------------------------------------
You can adjust default camera values by editing 'thief_camera.ini':
  [Camera]
  ToggleKey=0x73      ; Virtual key code (0x73 = F4, 0x72 = F3, 0x75 = F6)
  Distance=180.0      ; Camera boom distance behind Garrett (Unreal units)
  HeightOffset=25.0   ; Vertical height above Garrett's head
  ShoulderOffset=30.0 ; Lateral offset (positive = right, negative = left)
  SmoothFactor=0.25   ; Camera tracking responsiveness (0.0 to 1.0)

--------------------------------------------------------------------------------
5. UNINSTALLATION
--------------------------------------------------------------------------------
Simply delete 'dinput8.dll' and 'thief_camera.ini' from 'Binaries\Win64\'.
No original game files are ever replaced or modified.

--------------------------------------------------------------------------------
6. CREDITS & DISCLAIMER
--------------------------------------------------------------------------------
Created for educational and research purposes.
Thief is a registered trademark of Square Enix / Eidos Montreal.
No proprietary game assets or copyrighted files are distributed with this mod.
================================================================================
