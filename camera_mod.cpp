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

// Helper function to resolve wide string for an FName via native FName_ToString
static bool GetFNameWide(FName name, wchar_t* outBuf, size_t maxChars)
{
    if (name.Index <= 0 || !outBuf || maxChars == 0)
        return false;

    outBuf[0] = L'\0';
    auto pToString = (ThiefSignatures::tFName_ToString)RESOLVE_ADDR(ThiefFunctions::FName_ToString);
    auto pFree = (ThiefSignatures::tAppFree)RESOLVE_ADDR(ThiefFunctions::appFree);

    if (!pToString)
        return false;

    FString str = { nullptr, 0, 0 };
    uint64_t nameVal = *(uint64_t*)&name;
    pToString(nameVal, &str);

    bool bSuccess = false;
    if (str.Data && str.Num > 0)
    {
        wcsncpy(outBuf, str.Data, maxChars - 1);
        outBuf[maxChars - 1] = L'\0';
        bSuccess = true;
    }

    if (str.Data && pFree)
    {
        pFree(str.Data);
    }

    return bSuccess;
}

// Check if player is currently in a minigame, interaction, bow aiming, or cutscene
static bool IsMinigameOrCutsceneActive(APlayerController* Controller, AThiefPawn_Hero* hero)
{
    if (!Controller || !IsValidReadPtr(Controller, 0x480))
        return false;

    // Signal A: Garrett Action/Interaction state (native check used by Eidos camera collision at 0x140C072F0)
    if (hero && IsValidReadPtr(hero, 0x1100))
    {
        uintptr_t p10a8 = *(uintptr_t*)((uintptr_t)hero + 0x10A8);
        if (p10a8 && IsValidReadPtr((void*)p10a8, 0x70))
        {
            uintptr_t p68 = *(uintptr_t*)(p10a8 + 0x68);
            if (p68 && IsValidReadPtr((void*)p68, 0x110))
            {
                uintptr_t pAction = *(uintptr_t*)(p68 + 0x104);
                if (pAction && IsValidReadPtr((void*)pAction, 0x30))
                {
                    // Non-null action indicates Garrett is lockpicking, searching, cracking safe, in peephole, or takedown!
                    return true;
                }
            }
        }
    }

    // Signal B: ViewTarget is an external object / minigame target (not Garrett)
    if (Controller->ViewTarget.Target && (AActor*)Controller->ViewTarget.Target != (AActor*)hero)
    {
        return true;
    }

    // Signal C: Check Camera's ActiveBehavior and FOV
    uintptr_t cam = *(uintptr_t*)((uintptr_t)Controller + 0x450);
    if (cam && IsValidReadPtr((void*)cam, 0xC00))
    {
        AThiefCamera* pCam = (AThiefCamera*)cam;
        // If camera FOV is significantly zoomed in (e.g. lockpicking, bow aiming), keep in 1P!
        if (pCam->POV.FOV > 0.0f && pCam->POV.FOV < 60.0f)
        {
            return true;
        }

        uintptr_t behavior = *(uintptr_t*)(cam + 0xB68);
        if (behavior && IsValidReadPtr((void*)behavior, 0x300))
        {
            uintptr_t pClass = *(uintptr_t*)(behavior + 0x50);
            if (pClass && IsValidReadPtr((void*)pClass, 0x60))
            {
                FName className = *(FName*)(pClass + 0x48);
                wchar_t nameBuf[128];
                if (GetFNameWide(className, nameBuf, 128))
                {
                    if (wcsstr(nameBuf, L"Lockpick") ||
                        wcsstr(nameBuf, L"Marksman") ||
                        wcsstr(nameBuf, L"SmartObject") ||
                        wcsstr(nameBuf, L"Cinematic") ||
                        wcsstr(nameBuf, L"Cineser") ||
                        wcsstr(nameBuf, L"SecondPerson") ||
                        wcsstr(nameBuf, L"Safe") ||
                        wcsstr(nameBuf, L"Keypad") ||
                        wcsstr(nameBuf, L"Takedown") ||
                        wcsstr(nameBuf, L"Aim") ||
                        wcsstr(nameBuf, L"MiniGame") ||
                        wcsstr(nameBuf, L"Minigame") ||
                        wcsstr(nameBuf, L"Peephole") ||
                        wcsstr(nameBuf, L"Keyhole"))
                    {
                        return true;
                    }
                }
            }
        }
    }

    return false;
}

// Safely resolve Garrett pawn and update head/hood mesh visibility
static void UpdateHeroMeshVisibility(APlayerController* Controller, bool bThirdPerson)
{
    AThiefPawn_Hero* hero = GetHeroPawn(Controller);
    if (!hero || !IsValidReadPtr(hero, 0x1600))
        return;

    USkeletalMeshComponent* mesh = *(USkeletalMeshComponent**)((uintptr_t)hero + 0x444);
    if (!mesh || !IsValidReadPtr(mesh, 0x700))
        return;

    auto pShowHero = (ThiefSignatures::tShowThirdPersonMesh)RESOLVE_ADDR(ThiefFunctions::AThiefPawn_Hero_ShowThirdPersonMesh);
    auto pHideHero = (ThiefSignatures::tHideThirdPersonMesh)RESOLVE_ADDR(ThiefFunctions::AThiefPawn_Hero_HideThirdPersonMesh);
    auto pToggleSections = (ThiefSignatures::tToggleMaterialSections)RESOLVE_ADDR(ThiefFunctions::AThiefPawn_Hero_ToggleMaterialSections);
    auto pSetHideBone = (ThiefSignatures::tHideBone)RESOLVE_ADDR(ThiefFunctions::USkeletalMeshComponent_HideBone);

    uintptr_t mesh2 = *(uintptr_t*)((uintptr_t)hero + 0x14A8);
    uintptr_t mesh3 = *(uintptr_t*)((uintptr_t)hero + 0x14B0);

    // Get total bone count on primary mesh skeleton
    int32_t totalBones = 0;
    uintptr_t skel = *(uintptr_t*)((uintptr_t)mesh + 0x32C);
    if (skel && IsValidReadPtr((void*)skel, 0x50))
    {
        totalBones = *(int32_t*)(skel + 0x44);
    }

    if (bThirdPerson)
    {
        // 1. Force global GIsFirstPersonView to 0
        *(uint32_t*)RESOLVE_ADDR(ThiefGlobals::GIsFirstPersonView) = 0;
        *(uint32_t*)((uintptr_t)hero + 0x10D8) |= 0x2; // StateFlags: ThirdPersonMeshVisible

        // 2. Clear bHidden bit on primary mesh (+0x158)
        *(uint32_t*)((uintptr_t)mesh + 0x158) &= ~0x00400000;

        // 3. Engine master unhide
        if (pShowHero)
        {
            pShowHero(hero);
        }

        // 4. UNHIDE ALL BONES on primary mesh so head, neck, face, and hood are 100% visible!
        if (pSetHideBone && totalBones > 0)
        {
            for (int32_t b = 0; b < totalBones && b < 256; ++b)
            {
                pSetHideBone(mesh, b, 0, 1, 1); // bHide = 0
            }
        }

        // 5. Unhide hood/face material sections
        if (pToggleSections)
        {
            pToggleSections(hero, (void*)((uintptr_t)hero + 0x15A8), 0); // bHide = 0
        }

        // 6. Unhide and show all attached gear/accessory components on Garrett
        int32_t attachCount = *(int32_t*)((uintptr_t)mesh + 0x454);
        uintptr_t pAttachments = *(uintptr_t*)((uintptr_t)mesh + 0x44C);
        if (pAttachments && attachCount > 0 && attachCount < 64 && IsValidReadPtr((void*)pAttachments, attachCount * 0x34))
        {
            for (int32_t a = 0; a < attachCount; ++a)
            {
                uintptr_t comp = *(uintptr_t*)(pAttachments + a * 0x34);
                if (comp && IsValidReadPtr((void*)comp, 0x160))
                {
                    *(uint32_t*)(comp + 0x158) &= ~0x00400000; // clear bHidden
                    void** vtbl = *(void***)comp;
                    if (vtbl && IsValidReadPtr(vtbl, 0x510))
                    {
                        typedef void(*tSetHidden)(void*, uint32_t);
                        tSetHidden setHidden = (tSetHidden)vtbl[0x508 / 8];
                        if (setHidden) setHidden((void*)comp, 0); // show
                    }
                }
            }
        }

        // Also ensure components +0x1498 to +0x14F8 are visible (except 1P arms)
        for (int c = 1; c < 14; ++c)
        {
            uintptr_t comp = *(uintptr_t*)((uintptr_t)hero + 0x1490 + c * 8);
            if (comp && comp != mesh2 && IsValidReadPtr((void*)comp, 0x160))
            {
                *(uint32_t*)(comp + 0x158) &= ~0x00400000;
                void** vtbl = *(void***)comp;
                if (vtbl && IsValidReadPtr(vtbl, 0x510))
                {
                    typedef void(*tSetHidden)(void*, uint32_t);
                    tSetHidden setHidden = (tSetHidden)vtbl[0x508 / 8];
                    if (setHidden) setHidden((void*)comp, 0);
                }
            }
        }

        ModLog("[CameraMod] Garrett full third-person body, torso, hood, eyes, and head unhidden");
    }
    else
    {
        // First Person Mode:
        // Set GIsFirstPersonView to 0 before calling pHideHero so that its internal check succeeds
        *(uint32_t*)RESOLVE_ADDR(ThiefGlobals::GIsFirstPersonView) = 0;
        *(uint32_t*)((uintptr_t)hero + 0x10D8) &= ~0x2; // Clear ThirdPersonMeshVisible

        if (pHideHero)
        {
            pHideHero(hero);
        }
        *(uint32_t*)RESOLVE_ADDR(ThiefGlobals::GIsFirstPersonView) = 1;

        // 1. Hide head/neck bones so they do not obstruct 1st-person camera
        int32_t boneCount = hero->HiddenBoneIndices.Num;
        int32_t* pBones = hero->HiddenBoneIndices.Data;
        if (pSetHideBone && pBones && boneCount > 0 && boneCount < 128 && IsValidReadPtr(pBones, boneCount * sizeof(int32_t)))
        {
            for (int i = 0; i < boneCount; ++i)
            {
                pSetHideBone(mesh, pBones[i], 1, 1, 1); // bHide = 1
            }
        }
        
        // Always hide cached HeadBoneIndex (and descendants)
        if (Controller->PlayerCamera && pSetHideBone)
        {
            int32_t headIdx = Controller->PlayerCamera->HeadBoneIndex;
            if (headIdx >= 0)
            {
                pSetHideBone(mesh, headIdx, 1, 1, 1);
            }
        }

        // 2. Hide hood/face material sections
        if (pToggleSections)
        {
            pToggleSections(hero, (void*)((uintptr_t)hero + 0x15A8), 1); // bHide = 1
        }

        // 3. Hide ALL head-attached gear and accessories (eyes, mask, hood accessory, quiver)
        int32_t attachCount = *(int32_t*)((uintptr_t)mesh + 0x454);
        uintptr_t pAttachments = *(uintptr_t*)((uintptr_t)mesh + 0x44C);
        if (pAttachments && attachCount > 0 && attachCount < 64 && IsValidReadPtr((void*)pAttachments, attachCount * 0x34))
        {
            for (int32_t a = 0; a < attachCount; ++a)
            {
                uintptr_t comp = *(uintptr_t*)(pAttachments + a * 0x34);
                FName socket = *(FName*)(pAttachments + a * 0x34 + 0x08);
                if (comp && IsValidReadPtr((void*)comp, 0x160))
                {
                    wchar_t sockName[64];
                    bool bIsHeadAttachment = false;
                    if (GetFNameWide(socket, sockName, 64))
                    {
                        if (wcsstr(sockName, L"Head") || wcsstr(sockName, L"head") ||
                            wcsstr(sockName, L"Neck") || wcsstr(sockName, L"Face") ||
                            wcsstr(sockName, L"Mask") || wcsstr(sockName, L"Eyes"))
                        {
                            bIsHeadAttachment = true;
                        }
                    }
                    if (socket.Index == hero->SocketName.Index || comp == mesh3)
                    {
                        bIsHeadAttachment = true;
                    }

                    if (bIsHeadAttachment)
                    {
                        *(uint32_t*)(comp + 0x158) |= 0x00400000; // set bHidden
                        void** vtbl = *(void***)comp;
                        if (vtbl && IsValidReadPtr(vtbl, 0x510))
                        {
                            typedef void(*tSetHidden)(void*, uint32_t);
                            tSetHidden setHidden = (tSetHidden)vtbl[0x508 / 8];
                            if (setHidden) setHidden((void*)comp, 1); // hide
                        }
                    }
                }
            }
        }

        // Explicitly ensure Mesh3 is hidden
        if (mesh3 && IsValidReadPtr((void*)mesh3, 0x160))
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

        // 4. Ensure primary mesh (body/legs) and Mesh2 (hands/arms) remain visible for 1st-person gameplay
        *(uint32_t*)((uintptr_t)mesh + 0x158) &= ~0x00400000;
        if (mesh2 && IsValidReadPtr((void*)mesh2, 0x300))
        {
            *(uint32_t*)(mesh2 + 0x158) &= ~0x00400000;
            void** vtbl = *(void***)mesh2;
            if (vtbl && IsValidReadPtr(vtbl, 0x510))
            {
                typedef void(*tSetHidden)(void*, uint32_t);
                tSetHidden setHidden = (tSetHidden)vtbl[0x508 / 8];
                if (setHidden) setHidden((void*)mesh2, 0);
            }
        }

        ModLog("[CameraMod] Restored 1st-person: Head and mask hidden, hands and body visible");
    }
}

static float g_CurrentCameraDistance = 180.0f;
static bool g_bWasInMinigame = false;

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

    AThiefPawn_Hero* hero = GetHeroPawn(Controller);

    // 2. Detect minigame / special interaction states (lockpicking, marksman, cutscenes)
    bool bInMinigame = IsMinigameOrCutsceneActive(Controller, hero);
    bool bEffective3P = g_Config.bEnabled && !bInMinigame;

    // 3. Update character model visibility ONLY on actual state change!
    if (s_LastModState != bEffective3P)
    {
        UpdateHeroMeshVisibility(Controller, bEffective3P);
        s_LastModState = bEffective3P;
    }

    // 4. If minigame or 1P mode is active, do not offset camera
    if (!bEffective3P)
    {
        g_CurrentCameraDistance = g_Config.Distance;
        g_bWasInMinigame = bInMinigame;
        g_bFirstFrameAfterToggle = true;
        return;
    }

    // 5. Compute direction vectors from head rotation
    FVector forward, right, up;
    GetRotatorAxes(*OutRotation, forward, right, up);

    FVector headPos = *OutLocation;

    // Pivot over Garrett's shoulder
    FVector pivot;
    pivot.X = headPos.X + (right.X * g_Config.ShoulderOffset) + (up.X * g_Config.HeightOffset);
    pivot.Y = headPos.Y + (right.Y * g_Config.ShoulderOffset) + (up.Y * g_Config.HeightOffset);
    pivot.Z = headPos.Z + (right.Z * g_Config.ShoulderOffset) + (up.Z * g_Config.HeightOffset);

    // Issue 1: Collision check using engine LineCheck
    float targetDist = g_Config.Distance;

    uintptr_t pGWorldPtr = (uintptr_t)RESOLVE_ADDR(ThiefGlobals::GWorld);
    void* pWorld = pGWorldPtr ? *(void**)pGWorldPtr : nullptr;
    auto pLineCheck = (ThiefSignatures::tLineCheck)RESOLVE_ADDR(ThiefFunctions::UWorld_LineCheck);

    if (pWorld && IsValidReadPtr(pWorld, 0x100) && pLineCheck)
    {
        FVector sweepExtent(12.0f, 12.0f, 12.0f); // 12 uu box sweep prevents near-clip poke through

        // Check A: trace from head to pivot (avoids camera getting stuck inside a wall to Garrett's right)
        FCheckResult shoulderHit;
        shoulderHit.Time = 1.0f;
        uint32_t shoulderClear = pLineCheck(pWorld, &shoulderHit, (AActor*)hero, &pivot, &headPos, CollisionFlags::TRACE_CameraProbe, 1, &sweepExtent, nullptr, nullptr);
        if (shoulderClear == 0 && shoulderHit.Time < 1.0f)
        {
            float safeFrac = shoulderHit.Time - 0.1f;
            if (safeFrac < 0.0f) safeFrac = 0.0f;
            pivot.X = headPos.X + (pivot.X - headPos.X) * safeFrac;
            pivot.Y = headPos.Y + (pivot.Y - headPos.Y) * safeFrac;
            pivot.Z = headPos.Z + (pivot.Z - headPos.Z) * safeFrac;
        }

        // Check B: trace from pivot backwards to desired camera position
        FVector desiredCamPos;
        desiredCamPos.X = pivot.X - (forward.X * g_Config.Distance);
        desiredCamPos.Y = pivot.Y - (forward.Y * g_Config.Distance);
        desiredCamPos.Z = pivot.Z - (forward.Z * g_Config.Distance);

        FCheckResult camHit;
        camHit.Time = 1.0f;
        uint32_t camClear = pLineCheck(pWorld, &camHit, (AActor*)hero, &desiredCamPos, &pivot, CollisionFlags::TRACE_CameraProbe, 1, &sweepExtent, nullptr, nullptr);
        if (camClear == 0 && camHit.Time < 1.0f)
        {
            float hitDist = (g_Config.Distance * camHit.Time) - 14.0f;
            if (hitDist < 0.0f) hitDist = 0.0f;
            targetDist = hitDist;
        }
    }

    // 6. Asymmetric distance smoothing: instant pull-in when blocked, ease-out when clear
    if (g_bFirstFrameAfterToggle)
    {
        g_CurrentCameraDistance = targetDist;
        g_bFirstFrameAfterToggle = false;
    }
    else if (targetDist < g_CurrentCameraDistance)
    {
        g_CurrentCameraDistance = targetDist; // Instant pull-in
    }
    else
    {
        g_CurrentCameraDistance += (targetDist - g_CurrentCameraDistance) * 0.15f; // Smooth ease-out
    }

    // 7. Calculate position along camera ray
    FVector finalCamPos;
    finalCamPos.X = pivot.X - (forward.X * g_CurrentCameraDistance);
    finalCamPos.Y = pivot.Y - (forward.Y * g_CurrentCameraDistance);
    finalCamPos.Z = pivot.Z - (forward.Z * g_CurrentCameraDistance);

    // 8. Near-proximity occlusion: fade/hide meshes when camera pulls too close to head
    // Uses hysteresis to prevent boundary flickering
    static bool s_bProximityHidden = false;
    if (hero)
    {
        USkeletalMeshComponent* primaryMesh = *(USkeletalMeshComponent**)((uintptr_t)hero + 0x444);
        if (g_CurrentCameraDistance < 15.0f && !s_bProximityHidden)
        {
            if (primaryMesh && IsValidReadPtr(primaryMesh, 0x160))
                *(uint32_t*)((uintptr_t)primaryMesh + 0x158) |= 0x00400000;
            s_bProximityHidden = true;
        }
        else if (g_CurrentCameraDistance > 25.0f && s_bProximityHidden)
        {
            if (primaryMesh && IsValidReadPtr(primaryMesh, 0x160))
                *(uint32_t*)((uintptr_t)primaryMesh + 0x158) &= ~0x00400000;
            s_bProximityHidden = false;
        }
    }

    // 9. Smooth interpolation (exponential decay filter)
    float dx = finalCamPos.X - g_CurrentCameraPos.X;
    float dy = finalCamPos.Y - g_CurrentCameraPos.Y;
    float dz = finalCamPos.Z - g_CurrentCameraPos.Z;
    float distSq = dx * dx + dy * dy + dz * dz;

    if (distSq > (500.0f * 500.0f))
    {
        g_CurrentCameraPos = finalCamPos;
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

    // 10. Write modified location into renderer's view info
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
