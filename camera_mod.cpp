#include "camera_mod.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

// Global configuration & state
ModConfig g_Config = {
    true,      // bEnabled (starts enabled)
    180.0f,    // Distance behind player
    25.0f,     // HeightOffset upward
    30.0f,     // ShoulderOffset rightward
    35.0f,     // MinDistance (minimum clearance to avoid skull clipping)
    VK_F4,     // ToggleKey (F4)
    0.25f      // SmoothFactor
};

bool g_bRunning = true;
HMODULE g_hModule = nullptr;
uintptr_t g_Base = 0;

static HANDLE g_hInputThread = nullptr;
static uint8_t* g_pCallSite = nullptr;
static uint8_t g_OriginalCallBytes[5] = { 0 };
static void* g_pRelayMemory = nullptr;

static bool g_bFirstFrameAfterToggle = true;
static FVector g_CurrentCameraPos;

#pragma pack(push, 1)
struct RelayThunk
{
    uint8_t MovRax[2];   // 0x48, 0xB8
    uint64_t TargetAddr; // 64-bit absolute target
    uint8_t JmpRax[2];   // 0xFF, 0xE0
};
#pragma pack(pop)

// File logger for troubleshooting
static void ModLog(const char* msg)
{
    OutputDebugStringA(msg);
    char logPath[MAX_PATH];
    if (GetModuleFileNameA(g_hModule, logPath, MAX_PATH) == 0)
        return;
    char* slash = strrchr(logPath, '\\');
    if (!slash) slash = strrchr(logPath, '/');
    if (slash) *(slash + 1) = '\0';
    strcat(logPath, "thief_camera.log");

    HANDLE hFile = CreateFileA(logPath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        SetFilePointer(hFile, 0, nullptr, FILE_END);
        DWORD written;
        WriteFile(hFile, msg, (DWORD)strlen(msg), &written, nullptr);
        WriteFile(hFile, "\r\n", 2, &written, nullptr);
        CloseHandle(hFile);
    }
}

// Pattern matching within module
static uint8_t* FindPattern(uint8_t* start, size_t size, const uint8_t* pattern, const char* mask)
{
    size_t patternLen = strlen(mask);
    for (size_t i = 0; i <= size - patternLen; ++i)
    {
        bool found = true;
        for (size_t j = 0; j < patternLen; ++j)
        {
            if (mask[j] != '?' && start[i + j] != pattern[j])
            {
                found = false;
                break;
            }
        }
        if (found)
            return &start[i];
    }
    return nullptr;
}

// Allocate relay memory within 2GB of target address
static void* AllocateRelayNear(uintptr_t target_addr, size_t size)
{
    uintptr_t min_addr = (target_addr > 0x70000000ULL) ? (target_addr - 0x70000000ULL) : 0x10000ULL;
    uintptr_t max_addr = target_addr + 0x70000000ULL;

    for (uintptr_t addr = (target_addr + 0x10000ULL) & ~0xFFFFULL; addr < max_addr; addr += 0x10000ULL)
    {
        void* mem = VirtualAlloc((void*)addr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (mem) return mem;
    }
    for (uintptr_t addr = (target_addr - 0x10000ULL) & ~0xFFFFULL; addr > min_addr; addr -= 0x10000ULL)
    {
        void* mem = VirtualAlloc((void*)addr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (mem) return mem;
    }
    return nullptr;
}

// Convert FRotator (65536 = 360 deg) to orthonormal direction axes
// Unreal Engine 3 coordinate system: +X = Forward, +Y = Right, +Z = Up
static void GetRotatorAxes(const FRotator& rot, FVector& outForward, FVector& outRight, FVector& outUp)
{
    constexpr float UROT_TO_RAD = 3.14159265358979323846f / 32768.0f;

    float sp = sinf((float)rot.Pitch * UROT_TO_RAD);
    float cp = cosf((float)rot.Pitch * UROT_TO_RAD);
    float sy = sinf((float)rot.Yaw   * UROT_TO_RAD);
    float cy = cosf((float)rot.Yaw   * UROT_TO_RAD);
    float sr = sinf((float)rot.Roll  * UROT_TO_RAD);
    float cr = cosf((float)rot.Roll  * UROT_TO_RAD);

    // Forward (X Axis)
    outForward.X = cp * cy;
    outForward.Y = cp * sy;
    outForward.Z = sp;

    // Right (Y Axis)
    outRight.X = (sr * sp * cy) - (cr * sy);
    outRight.Y = (sr * sp * sy) + (cr * cy);
    outRight.Z = -sr * cp;

    // Up (Z Axis)
    outUp.X = -(cr * sp * cy) - (sr * sy);
    outUp.Y = -(cr * sp * sy) + (sr * cy);
    outUp.Z = cr * cp;
}

// Validate pointer and memory accessibility using Win32 VirtualQuery
static bool IsValidReadPtr(const void* p, size_t size)
{
    if (!p || (uintptr_t)p < 0x10000 || (uintptr_t)p > 0x7FFFFFFFFFFF)
        return false;

    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0)
        return false;

    if (mbi.State != MEM_COMMIT)
        return false;

    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
        return false;

    return true;
}

// Safely resolve Garrett pawn with 3 independent avenues
static AThiefPawn_Hero* GetHeroPawn(APlayerController* Controller)
{
    // Avenue A: Direct Controller->Pawn (+0x438)
    if (IsValidReadPtr(Controller, 0x480))
    {
        AThiefPawn_Hero* pPawn = *(AThiefPawn_Hero**)((uintptr_t)Controller + 0x438);
        if (pPawn && IsValidReadPtr(pPawn, 0x1600))
            return pPawn;
    }

    // Avenue B: Native Engine GetLocalPlayerCameraManager() -> Manager + 0x718
    auto pGetCamMgr = (ThiefSignatures::tGetLocalPlayerCameraManager)RESOLVE_ADDR(ThiefFunctions::GetLocalPlayerCameraManager);
    if (pGetCamMgr)
    {
        uintptr_t mgr = pGetCamMgr();
        if (mgr && IsValidReadPtr((void*)mgr, 0x750))
        {
            AThiefPawn_Hero* pPawn = *(AThiefPawn_Hero**)(mgr + 0x718);
            if (pPawn && IsValidReadPtr(pPawn, 0x1600))
                return pPawn;
        }
    }

    // Avenue C: Controller->PlayerCamera (+0x450) -> cam + 0x978 -> holder + 0x718
    if (IsValidReadPtr(Controller, 0x480))
    {
        uintptr_t cam = *(uintptr_t*)((uintptr_t)Controller + 0x450);
        if (cam && IsValidReadPtr((void*)cam, 0xA00))
        {
            uintptr_t holder = *(uintptr_t*)(cam + 0x978);
            if (holder && IsValidReadPtr((void*)holder, 0x750))
            {
                AThiefPawn_Hero* pPawn = *(AThiefPawn_Hero**)(holder + 0x718);
                if (pPawn && IsValidReadPtr(pPawn, 0x1600))
                    return pPawn;
            }
        }
    }

    return nullptr;
}

// Safely resolve Garrett pawn and update head/hood mesh visibility
static void UpdateHeroMeshVisibility(APlayerController* Controller, bool bThirdPerson)
{
    AThiefPawn_Hero* hero = GetHeroPawn(Controller);
    if (!hero)
        return;

    // Verify hero struct size
    if (!IsValidReadPtr(hero, 0x1600))
        return;

    // Verify hero->Mesh (+0x444)
    USkeletalMeshComponent* mesh = *(USkeletalMeshComponent**)((uintptr_t)hero + 0x444);
    if (!mesh || !IsValidReadPtr(mesh, 0x700))
        return;

    // Verify mesh->SkeletalMesh asset (+0x2D0) is loaded
    uintptr_t skelMesh = *(uintptr_t*)((uintptr_t)mesh + 0x2D0);
    if (!skelMesh || !IsValidReadPtr((void*)skelMesh, 0x90))
        return;

    static bool s_bLoggedUnhide = false;

    if (bThirdPerson)
    {
        // 1. Force global GIsFirstPersonView to 0
        *(uint32_t*)RESOLVE_ADDR(ThiefGlobals::GIsFirstPersonView) = 0;

        // 2. Clear bHidden bit on primary mesh (+0x158)
        *(uint32_t*)((uintptr_t)mesh + 0x158) &= ~0x00400000;

        // 3. Native engine reset: show all material sections on primary mesh and unhide all bones
        auto pShowAll = (ThiefSignatures::tShowAllMaterialSections)RESOLVE_ADDR(ThiefFunctions::AThiefPawn_Hero_ShowAllMaterialSections);
        if (pShowAll)
        {
            pShowAll(hero);
        }

        auto pShowSection = (ThiefSignatures::tShowMaterialSection)RESOLVE_ADDR(ThiefFunctions::USkeletalMeshComponent_ShowMaterialSection);
        auto pSetSectionHidden = (ThiefSignatures::tSetMaterialSectionHidden)RESOLVE_ADDR(ThiefFunctions::USkeletalMeshComponent_SetMaterialSectionHidden);

        // 4. Forcibly ensure all sections on hero->Mesh are shown (clearing bit 0 and bit 3)
        int32_t secCount1 = *(int32_t*)(skelMesh + 0x84);
        for (int s = 0; s < secCount1 && s < 32; ++s)
        {
            if (pShowSection) pShowSection(mesh, 0, s, 1, 1, 1);
            if (pSetSectionHidden) pSetSectionHidden(mesh, 0, s, 0);
        }

        // 5. Unhide and show all sections on Mesh2 (+0x14A8, torso/body)
        uintptr_t mesh2 = *(uintptr_t*)((uintptr_t)hero + 0x14A8);
        int32_t secCount2 = 0;
        if (mesh2 && IsValidReadPtr((void*)mesh2, 0x300))
        {
            *(uint32_t*)(mesh2 + 0x158) &= ~0x00400000;
            uintptr_t skel2 = *(uintptr_t*)(mesh2 + 0x2D0);
            if (skel2 && IsValidReadPtr((void*)skel2, 0x90))
            {
                secCount2 = *(int32_t*)(skel2 + 0x84);
                for (int s = 0; s < secCount2 && s < 32; ++s)
                {
                    if (pShowSection) pShowSection((USkeletalMeshComponent*)mesh2, 0, s, 1, 1, 1);
                    if (pSetSectionHidden) pSetSectionHidden((USkeletalMeshComponent*)mesh2, 0, s, 0);
                }
            }
        }

        // 6. Unhide and show all sections on Mesh3 (+0x14B0, head/hood)
        uintptr_t mesh3 = *(uintptr_t*)((uintptr_t)hero + 0x14B0);
        int32_t secCount3 = 0;
        if (mesh3 && IsValidReadPtr((void*)mesh3, 0x300))
        {
            *(uint32_t*)(mesh3 + 0x158) &= ~0x00400000;
            uintptr_t skel3 = *(uintptr_t*)(mesh3 + 0x2D0);
            if (skel3 && IsValidReadPtr((void*)skel3, 0x90))
            {
                secCount3 = *(int32_t*)(skel3 + 0x84);
                for (int s = 0; s < secCount3 && s < 32; ++s)
                {
                    if (pShowSection) pShowSection((USkeletalMeshComponent*)mesh3, 0, s, 1, 1, 1);
                    if (pSetSectionHidden) pSetSectionHidden((USkeletalMeshComponent*)mesh3, 0, s, 0);
                }
            }

            // Call SetHidden(0) via vtable +0x508 on Mesh3
            void** vtbl = *(void***)mesh3;
            if (vtbl && IsValidReadPtr(vtbl, 0x510))
            {
                typedef void(*tSetHidden)(void*, uint32_t);
                tSetHidden setHidden = (tSetHidden)vtbl[0x508 / 8];
                if (setHidden) setHidden((void*)mesh3, 0);
            }
        }

        if (!s_bLoggedUnhide)
        {
            char diag[256];
            uint32_t stateFlags = *(uint32_t*)((uintptr_t)hero + 0x10D8);
            wsprintfA(diag, "[MeshDiag] Hero=%p Flags=0x%x Mesh=%p(Sec=%d) Mesh2=%p(Sec=%d) Mesh3=%p(Sec=%d)",
                hero, stateFlags, mesh, secCount1, (void*)mesh2, secCount2, (void*)mesh3, secCount3);
            ModLog(diag);

            ModLog("[CameraMod] Garrett full third-person body, torso, and head unhidden");
            s_bLoggedUnhide = true;
        }
    }
    else
    {
        // First Person:
        *(uint32_t*)RESOLVE_ADDR(ThiefGlobals::GIsFirstPersonView) = 1;

        // Primary mesh and Mesh2 (hands/body) remain visible in first person
        *(uint32_t*)((uintptr_t)mesh + 0x158) &= ~0x00400000;
        uintptr_t mesh2 = *(uintptr_t*)((uintptr_t)hero + 0x14A8);
        if (mesh2 && IsValidReadPtr((void*)mesh2, 0x300))
        {
            *(uint32_t*)(mesh2 + 0x158) &= ~0x00400000;
        }

        // Hide Mesh3 (head) so it doesn't clip the 1st person camera
        uintptr_t mesh3 = *(uintptr_t*)((uintptr_t)hero + 0x14B0);
        if (mesh3 && IsValidReadPtr((void*)mesh3, 0x300))
        {
            *(uint32_t*)(mesh3 + 0x158) |= 0x00400000;
            void** vtbl = *(void***)mesh3;
            if (vtbl && IsValidReadPtr(vtbl, 0x510))
            {
                typedef void(*tSetHidden)(void*, uint32_t);
                tSetHidden setHidden = (tSetHidden)vtbl[0x508 / 8];
                if (setHidden) setHidden((void*)mesh3, 1);
            }
        }

        s_bLoggedUnhide = false;
    }
}

// Hook function: replaces the call to APlayerController::eventGetPlayerViewPoint inside ULocalPlayer::CalcSceneView
extern "C" void Hook_GetPlayerViewPoint(APlayerController* Controller, FVector* OutLocation, FRotator* OutRotation)
{
    // 1. Invoke original function (the original function entry at 0x14039A800 was never modified)
    auto pOriginal = (ThiefSignatures::tGetPlayerViewPoint)RESOLVE_ADDR(ThiefFunctions::APlayerController_eventGetPlayerViewPoint);
    pOriginal(Controller, OutLocation, OutRotation);

    static bool s_bFirstCallLogged = false;
    static int s_FrameLogCount = 0;
    static bool s_LastModState = false;
    static int s_MeshFrameCounter = 0;

    if (!s_bFirstCallLogged)
    {
        ModLog("[CameraMod] Hook_GetPlayerViewPoint active and transforming camera");
        s_bFirstCallLogged = true;
    }

    // 2. Manage Garrett's head and mesh visibility
    if (s_LastModState != g_Config.bEnabled || (++s_MeshFrameCounter % 15 == 0))
    {
        UpdateHeroMeshVisibility(Controller, g_Config.bEnabled);
        s_LastModState = g_Config.bEnabled;
    }

    // 3. Check if third person is enabled
    if (!g_Config.bEnabled)
    {
        *(uint32_t*)RESOLVE_ADDR(ThiefGlobals::GIsFirstPersonView) = 1;
        return;
    }

    *(uint32_t*)RESOLVE_ADDR(ThiefGlobals::GIsFirstPersonView) = 0;

    // 4. Compute direction vectors from head rotation
    FVector forward, right, up;
    GetRotatorAxes(*OutRotation, forward, right, up);

    FVector headPos = *OutLocation;
    FVector desiredPos;
    desiredPos.X = headPos.X - (forward.X * g_Config.Distance) + (right.X * g_Config.ShoulderOffset) + (up.X * g_Config.HeightOffset);
    desiredPos.Y = headPos.Y - (forward.Y * g_Config.Distance) + (right.Y * g_Config.ShoulderOffset) + (up.Y * g_Config.HeightOffset);
    desiredPos.Z = headPos.Z - (forward.Z * g_Config.Distance) + (right.Z * g_Config.ShoulderOffset) + (up.Z * g_Config.HeightOffset);

    // 5. Smooth interpolation (exponential decay filter)
    float dx = desiredPos.X - g_CurrentCameraPos.X;
    float dy = desiredPos.Y - g_CurrentCameraPos.Y;
    float dz = desiredPos.Z - g_CurrentCameraPos.Z;
    float distSq = dx * dx + dy * dy + dz * dz;

    if (g_bFirstFrameAfterToggle || distSq > (500.0f * 500.0f))
    {
        g_CurrentCameraPos = desiredPos;
        g_bFirstFrameAfterToggle = false;
    }
    else
    {
        float alpha = g_Config.SmoothFactor;
        g_CurrentCameraPos.X += dx * alpha;
        g_CurrentCameraPos.Y += dy * alpha;
        g_CurrentCameraPos.Z += dz * alpha;
    }

    if (s_FrameLogCount < 5)
    {
        s_FrameLogCount++;
        char buf[128];
        int hx = (int)headPos.X, hy = (int)headPos.Y, hz = (int)headPos.Z;
        int cx = (int)g_CurrentCameraPos.X, cy = (int)g_CurrentCameraPos.Y, cz = (int)g_CurrentCameraPos.Z;
        wsprintfA(buf, "[CameraMod] View transformed: Head(%d,%d,%d) -> Cam(%d,%d,%d)", hx, hy, hz, cx, cy, cz);
        ModLog(buf);
    }

    // 6. Write modified location into renderer's view info
    *OutLocation = g_CurrentCameraPos;
}

// Background thread for asynchronous hotkey handling
static DWORD WINAPI InputThreadProc(LPVOID lpParam)
{
    ModLog("[CameraMod] Input listener thread started");

    bool bLastToggleState = false;
    bool bLastF5State = false;
    bool bLastNumPad0State = false;
    bool bLastPageUpState = false;
    bool bLastPageDownState = false;
    bool bLastHomeState = false;
    bool bLastEndState = false;
    bool bLastInsertState = false;
    bool bLastDeleteState = false;

    while (g_bRunning)
    {
        Sleep(20);

        // F4: Toggle 1st / 3rd person perspective
        bool bToggleDown = (GetAsyncKeyState(g_Config.ToggleKey) & 0x8000) != 0;
        if (bToggleDown && !bLastToggleState)
        {
            g_Config.bEnabled = !g_Config.bEnabled;
            g_bFirstFrameAfterToggle = true;
            ModLog(g_Config.bEnabled ? "[CameraMod] Switched to 3rd Person View" : "[CameraMod] Switched to 1st Person View");
        }
        bLastToggleState = bToggleDown;

        // F5: Switch shoulder (Left / Center / Right)
        bool bF5Down = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
        if (bF5Down && !bLastF5State)
        {
            if (g_Config.ShoulderOffset > 10.0f)
                g_Config.ShoulderOffset = -30.0f; // Switch to left shoulder
            else if (g_Config.ShoulderOffset < -10.0f)
                g_Config.ShoulderOffset = 0.0f;   // Switch to center
            else
                g_Config.ShoulderOffset = 30.0f;  // Switch to right shoulder
            ModLog("[CameraMod] Shoulder toggled");
        }
        bLastF5State = bF5Down;

        // NumPad 0: Reset camera to defaults
        bool bNumPad0Down = (GetAsyncKeyState(VK_NUMPAD0) & 0x8000) != 0;
        if (bNumPad0Down && !bLastNumPad0State)
        {
            g_Config.Distance = 180.0f;
            g_Config.HeightOffset = 25.0f;
            g_Config.ShoulderOffset = 30.0f;
            g_Config.SmoothFactor = 0.25f;
            g_bFirstFrameAfterToggle = true;
            ModLog("[CameraMod] Camera reset to defaults");
        }
        bLastNumPad0State = bNumPad0Down;

        // Page Up / Page Down: Adjust distance
        bool bPageUpDown = (GetAsyncKeyState(VK_PRIOR) & 0x8000) != 0;
        if (bPageUpDown)
        {
            g_Config.Distance += 2.0f;
            if (g_Config.Distance > 600.0f) g_Config.Distance = 600.0f;
        }
        bool bPageDownDown = (GetAsyncKeyState(VK_NEXT) & 0x8000) != 0;
        if (bPageDownDown)
        {
            g_Config.Distance -= 2.0f;
            if (g_Config.Distance < 50.0f) g_Config.Distance = 50.0f;
        }

        // Home / End: Adjust height
        bool bHomeDown = (GetAsyncKeyState(VK_HOME) & 0x8000) != 0;
        if (bHomeDown)
        {
            g_Config.HeightOffset += 1.0f;
            if (g_Config.HeightOffset > 150.0f) g_Config.HeightOffset = 150.0f;
        }
        bool bEndDown = (GetAsyncKeyState(VK_END) & 0x8000) != 0;
        if (bEndDown)
        {
            g_Config.HeightOffset -= 1.0f;
            if (g_Config.HeightOffset < -50.0f) g_Config.HeightOffset = -50.0f;
        }

        // Insert / Delete: Adjust shoulder offset smoothly
        bool bInsertDown = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
        if (bInsertDown)
        {
            g_Config.ShoulderOffset += 1.0f;
            if (g_Config.ShoulderOffset > 100.0f) g_Config.ShoulderOffset = 100.0f;
        }
        bool bDeleteDown = (GetAsyncKeyState(VK_DELETE) & 0x8000) != 0;
        if (bDeleteDown)
        {
            g_Config.ShoulderOffset -= 1.0f;
            if (g_Config.ShoulderOffset < -100.0f) g_Config.ShoulderOffset = -100.0f;
        }
    }

    ModLog("[CameraMod] Input listener thread exiting");
    return 0;
}

// Load settings from thief_camera.ini if present
static void LoadConfigFromIni()
{
    char iniPath[MAX_PATH];
    if (GetModuleFileNameA(g_hModule, iniPath, MAX_PATH) != 0)
    {
        char* slash = strrchr(iniPath, '\\');
        if (!slash) slash = strrchr(iniPath, '/');
        if (slash) *(slash + 1) = '\0';
        strcat(iniPath, "thief_camera.ini");

        char valBuf[32];
        if (GetPrivateProfileStringA("Camera", "Distance", "", valBuf, sizeof(valBuf), iniPath) > 0)
            g_Config.Distance = (float)atof(valBuf);
        if (GetPrivateProfileStringA("Camera", "HeightOffset", "", valBuf, sizeof(valBuf), iniPath) > 0)
            g_Config.HeightOffset = (float)atof(valBuf);
        if (GetPrivateProfileStringA("Camera", "ShoulderOffset", "", valBuf, sizeof(valBuf), iniPath) > 0)
            g_Config.ShoulderOffset = (float)atof(valBuf);
        if (GetPrivateProfileStringA("Camera", "SmoothFactor", "", valBuf, sizeof(valBuf), iniPath) > 0)
            g_Config.SmoothFactor = (float)atof(valBuf);
        int toggleKey = GetPrivateProfileIntA("Camera", "ToggleKey", 0, iniPath);
        if (toggleKey > 0)
            g_Config.ToggleKey = toggleKey;
    }
}

// Initialize camera mod hooks
void InitCameraMod()
{
    ModLog("=== Initializing Thief 3rd Person Camera Mod ===");
    LoadConfigFromIni();

    g_Base = (uintptr_t)GetModuleHandleA(nullptr);
    if (!g_Base)
    {
        ModLog("[CameraMod] ERROR: Failed to obtain main module base address");
        return;
    }

    PIMAGE_DOS_HEADER dosHeader = (PIMAGE_DOS_HEADER)g_Base;
    PIMAGE_NT_HEADERS ntHeaders = (PIMAGE_NT_HEADERS)(g_Base + dosHeader->e_lfanew);
    PIMAGE_SECTION_HEADER section = IMAGE_FIRST_SECTION(ntHeaders);

    uint8_t* textStart = nullptr;
    size_t textSize = 0;

    for (WORD i = 0; i < ntHeaders->FileHeader.NumberOfSections; ++i)
    {
        if (strncmp((const char*)section[i].Name, ".text", 5) == 0)
        {
            textStart = (uint8_t*)(g_Base + section[i].VirtualAddress);
            textSize = section[i].Misc.VirtualSize;
            break;
        }
    }

    if (!textStart || textSize == 0)
    {
        textStart = (uint8_t*)(g_Base + 0x1000);
        textSize = 0x15F5AAC;
    }

    // Pattern for call to APlayerController::eventGetPlayerViewPoint inside ULocalPlayer::CalcSceneView
    const uint8_t pattern[] = {
        0x48, 0x8B, 0xCE,             // mov rcx, rsi
        0x48, 0x8D, 0x95, 0x90, 0x00, 0x00, 0x00, // lea rdx, [rbp+90h]
        0xE8                          // call rel32 (APlayerController::eventGetPlayerViewPoint)
    };
    const char mask[] = "xxxxxxxxxx?";

    uint8_t* match = FindPattern(textStart, textSize, pattern, mask);
    if (match)
    {
        g_pCallSite = match + 10; // Point to E8 rel32 instruction
        ModLog("[CameraMod] Pattern match found for call site at CalcSceneView + 0xBE");
    }
    else
    {
        // Fallback to verified RVA (0x3F6D5E)
        g_pCallSite = (uint8_t*)(g_Base + 0x3F6D5E);
        ModLog("[CameraMod] Using fallback RVA 0x3F6D5E for call site");
    }

    // Allocate relay near call site
    g_pRelayMemory = AllocateRelayNear((uintptr_t)g_pCallSite, sizeof(RelayThunk));
    if (!g_pRelayMemory)
    {
        ModLog("[CameraMod] ERROR: Failed to allocate relay memory");
        return;
    }

    // Fill relay thunk: mov rax, Hook_GetPlayerViewPoint; jmp rax
    RelayThunk* thunk = (RelayThunk*)g_pRelayMemory;
    thunk->MovRax[0] = 0x48;
    thunk->MovRax[1] = 0xB8;
    thunk->TargetAddr = (uint64_t)&Hook_GetPlayerViewPoint;
    thunk->JmpRax[0] = 0xFF;
    thunk->JmpRax[1] = 0xE0;

    // Patch 5-byte call instruction
    DWORD oldProtect;
    if (VirtualProtect(g_pCallSite, 5, PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        memcpy(g_OriginalCallBytes, g_pCallSite, 5);
        int32_t relOffset = (int32_t)((uintptr_t)g_pRelayMemory - ((uintptr_t)g_pCallSite + 5));
        g_pCallSite[0] = 0xE8;
        *(int32_t*)(g_pCallSite + 1) = relOffset;
        VirtualProtect(g_pCallSite, 5, oldProtect, &oldProtect);
        ModLog("[CameraMod] CalcSceneView hook installed successfully");
    }

    // Start background input listener
    g_hInputThread = CreateThread(nullptr, 0, InputThreadProc, nullptr, 0, nullptr);
    ModLog("[CameraMod] Input thread created");
}

// Clean shutdown and unhooking
void ShutdownCameraMod()
{
    g_bRunning = false;
    if (g_hInputThread)
    {
        WaitForSingleObject(g_hInputThread, 500);
        CloseHandle(g_hInputThread);
        g_hInputThread = nullptr;
    }

    // Restore original bytes at call site
    if (g_pCallSite && g_OriginalCallBytes[0] == 0xE8)
    {
        DWORD oldProtect;
        if (VirtualProtect(g_pCallSite, 5, PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            memcpy(g_pCallSite, g_OriginalCallBytes, 5);
            VirtualProtect(g_pCallSite, 5, oldProtect, &oldProtect);
        }
    }

    // Free relay memory
    if (g_pRelayMemory)
    {
        VirtualFree(g_pRelayMemory, 0, MEM_RELEASE);
        g_pRelayMemory = nullptr;
    }
}
