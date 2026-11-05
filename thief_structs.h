#pragma once
#include <cstdint>

// Thief (2014) - Shipping-ThiefGame.exe (x86_64, modified Unreal Engine 3)
// Verified class and struct layouts for Single-Player Third-Person Camera Mod

#pragma pack(push, 4)

struct FVector
{
    float X;
    float Y;
    float Z;

    FVector() : X(0.0f), Y(0.0f), Z(0.0f) {}
    FVector(float inX, float inY, float inZ) : X(inX), Y(inY), Z(inZ) {}
};

struct FRotator
{
    int32_t Pitch; // 65536 units = 360 degrees
    int32_t Yaw;
    int32_t Roll;

    FRotator() : Pitch(0), Yaw(0), Roll(0) {}
    FRotator(int32_t p, int32_t y, int32_t r) : Pitch(p), Yaw(y), Roll(r) {}
};

struct FName
{
    int32_t Index;
    int32_t Number;
};

struct FNameEntry
{
    uint64_t Flags;
    int32_t Index;
    FNameEntry* HashNext;
    union
    {
        char AnsiName[1024];
        wchar_t WideName[1024];
    } Name;
};

template<typename T>
struct TArray
{
    T* Data;
    int32_t Num;
    int32_t Max;
};

class AActor;
class UPrimitiveComponent;

// Collision trace hit result (total size: 0x74 bytes)
struct FCheckResult
{
    FCheckResult* Next;             // +0x00 (Next hit in linked list)
    AActor* Actor;                  // +0x08 (Actor hit by trace, nullptr if no hit)
    FVector Location;               // +0x10 (World position of impact)
    FVector Normal;                 // +0x1C (Surface normal at impact)
    float Time;                     // +0x28 (Normalized distance [0.0 - 1.0], 1.0 = no hit)
    int32_t Item;                   // +0x2C (Hit primitive sub-element index)
    UPrimitiveComponent* Component; // +0x30 (Primitive component hit)
    void* PhysMaterial;             // +0x38 (Physical material hit)
    uint8_t Pad_40[0x34];           // +0x40 (Internal trace metadata)

    FCheckResult() : Next(nullptr), Actor(nullptr), Location(), Normal(), Time(1.0f), Item(-1), Component(nullptr), PhysMaterial(nullptr) {}
};

namespace CollisionFlags
{
    constexpr uint32_t TRACE_WorldGeometry = 0x2086;  // Level brushes, static meshes, blocking volumes
    constexpr uint32_t TRACE_CameraProbe   = 0x8020DF;// Complex camera collision query with dynamic actors
}

class UClass;

// Base UObject (0x60 bytes header)
class UObject
{
public:
    void** VTable;                  // +0x00
    UObject* HashNext;              // +0x08
    uint64_t ObjectFlags;           // +0x10
    UObject* HashOuterNext;         // +0x18
    uint8_t Pad_20[0x10];           // +0x20
    UObject* HashPrev;              // +0x30
    int32_t InternalIndex;          // +0x38
    uint8_t Pad_3C[0x04];           // +0x3C
    UObject* Outer;                 // +0x40
    FName Name;                     // +0x48
    UClass* Class;                  // +0x50
    UObject* ObjectArchetype;       // +0x58
};

class UClass : public UObject
{
};

class UActorComponent : public UObject
{
public:
    AActor* OwnerActor;             // +0x60
};

// Base AActor (inherits UObject)
class AActor : public UObject
{
public:
    TArray<UActorComponent*> Components; // +0x60
    uint8_t Pad_70[0x10];                // +0x70
    FVector Location;                    // +0x80
    FRotator Rotation;                   // +0x8C
    uint8_t Pad_98[0x21];                // +0x98
    uint8_t Physics;                     // +0xB9 (EPhysics)
    uint8_t Pad_BA[0x12];                // +0xBA
    AActor* Owner;                       // +0xCC
};

class AController;
class USkeletalMeshComponent;

// Base APawn (inherits AActor)
class APawn : public AActor
{
public:
    uint8_t Pad_D4[0xCC];                // +0xD4
    FVector Velocity;                    // +0x1A0
    uint8_t Pad_1AC[0xFC];               // +0x1AC
    AController* Controller;             // +0x2A8
    uint8_t Pad_2B0[0xC8];               // +0x2B0
    int32_t Health;                      // +0x378
    uint8_t Pad_37C[0xC8];               // +0x37C
    USkeletalMeshComponent* Mesh;        // +0x444 (4-byte alignment pack)
};

// Garrett Hero Pawn (inherits APawn, total size 0x1640 bytes)
class AThiefPawn_Hero : public APawn
{
public:
    uint8_t Pad_44C[0xC8C];              // +0x44C
    uint32_t StateFlags;                 // +0x10D8 (Bit 1 / 0x2 = ThirdPersonMeshVisible)
    uint8_t Pad_10DC[0xF0];              // +0x10DC
    UObject* Component11cc;              // +0x11CC
    uint8_t Pad_11D4[0x2D4];             // +0x11D4
    USkeletalMeshComponent* Mesh2;       // +0x14A8 (Secondary mesh / first-person arms)
    UActorComponent* Mesh3;              // +0x14B0 (Attached quiver / bow gear)
    uint8_t Pad_14B8[0x80];              // +0x14B8
    FName SocketName;                    // +0x1538 (Socket name for Mesh3 attachment)
    uint8_t Pad_1540[0x68];              // +0x1540
    TArray<int32_t> HiddenMaterialSections; // +0x15A8 (Sections hidden in 1P, shown in 3P)
    TArray<int32_t> HiddenBoneIndices;      // +0x15B8 (Head/neck bones hidden in 1P, shown in 3P)
};

class UPlayer;
class UThiefCameraBehavior;

struct FTPOV
{
    FVector Location;                    // +0x00
    FRotator Rotation;                   // +0x0C
    float FOV;                           // +0x18
};

// Base Camera Behavior
class UThiefCameraBehavior : public UObject
{
public:
    uint8_t Pad_60[0x254];               // +0x60
    FVector ViewLocation;                // +0x2B4
    FRotator ViewRotation;               // +0x2C0
    float FOVAngle;                      // +0x2CC
    uint32_t Flags;                      // +0x2D0 (Bit 0: Active, Bit 1: FirstPerson, Bit 4: Valid)
};

// Main Thief Game Camera
class AThiefCamera : public AActor
{
public:
    uint8_t Pad_D4[0x6E4];               // +0xD4
    FTPOV POV;                           // +0x7B8 (Location +0x7B8, Rotation +0x7C4, FOV +0x7D0)
    uint8_t Pad_7D4[0x1A4];              // +0x7D4
    UObject* PawnHolder;                 // +0x978 (points to container with Garrett at +0x718)
    int32_t HeadBoneIndex;               // +0x988 (Cached index for "Bone-Head", -1 if uninit)
    uint8_t Pad_98C[0xB8];               // +0x98C
    FVector InternalLocation;            // +0xA44 (Camera location before copy to POV)
    FRotator InternalRotation;           // +0xA50 (Camera rotation before copy to POV)
    float InternalFOV;                   // +0xA5C (Camera FOV before copy to POV)
    UObject* CameraConfig;               // +0xA60 (Camera behavior config object, has threshold at +0x60 -> +0x70)
    uint8_t Pad_A68[0x100];              // +0xA68
    UThiefCameraBehavior* ActiveBehavior;// +0xB68
};

struct FTViewTarget
{
    AActor* Target;                      // +0x48C
    uint8_t Pad_08[0x1C];
};

// Base APlayerController (inherits AController)
class APlayerController : public AActor
{
public:
    uint8_t Pad_D4[0x374];               // +0xD4
    UPlayer* Player;                     // +0x448 (points to ULocalPlayer)
    AThiefCamera* PlayerCamera;          // +0x450 (points to AThiefCamera)
    uint8_t Pad_458[0x34];               // +0x458
    FTViewTarget ViewTarget;             // +0x48C
    float DefaultFOV;                    // +0x4AC
};

// Local Player representation
class ULocalPlayer : public UObject
{
public:
    uint8_t Pad_60[0x08];                // +0x60
    APlayerController* Actor;            // +0x68
    FVector LastViewLocation;            // +0x70
};

// Skeletal Mesh Component
class USkeletalMeshComponent : public UActorComponent
{
public:
    uint8_t Pad_00[0x13C];               // +0x00
    // RefBasesInvMatrix at +0x13C
    uint8_t Pad_13C[0x1C];               // +0x13C
    uint32_t BitFlags;                   // +0x158 (Bit 22 / 0x00400000 = bOwnerNoSee / bHidden)
    uint8_t Pad_15C[0x174];              // +0x15C
    UObject* SkeletalMesh;               // +0x2D0
    uint8_t Pad_2D8[0xDC];               // +0x2D8
    TArray<UObject*> AnimSets;           // +0x3B4
    uint8_t Pad_3C4[0x2B4];              // +0x3C4
    TArray<uint32_t> BoneVisibilityFlags;// +0x678
};

#pragma pack(pop)

// Verified Core Global Addresses (ImageBase = 0x140000000)
namespace ThiefGlobals
{
    constexpr uintptr_t GNames = 0x1420F2918;
    constexpr uintptr_t GObjects = 0x1420F2960;
    constexpr uintptr_t GObjHash = 0x142052440;
    constexpr uintptr_t GObjNameHash = 0x142062440;
    constexpr uintptr_t GWorld = 0x1421065A0;
    constexpr uintptr_t GNatives = 0x141FB1000;
    constexpr uintptr_t GThiefCamera = 0x142205EC0;
    constexpr uintptr_t GIsFirstPersonView = 0x142206BE0;
}

// Verified Function Addresses
namespace ThiefFunctions
{
    // Camera & View Pipeline
    constexpr uintptr_t ULocalPlayer_CalcSceneView = 0x1403F6BA0;
    constexpr uintptr_t APlayerController_eventGetPlayerViewPoint = 0x14039A800;
    constexpr uintptr_t APlayerController_GetFOVAngle = 0x1403F1260;
    constexpr uintptr_t AThiefCamera_GetCameraViewPoint = 0x140BE8DA0;
    constexpr uintptr_t AThiefCamera_GetFOVAngle = 0x140BE8E60;
    constexpr uintptr_t UWorld_LineCheck = 0x140397520;
    constexpr uintptr_t UThiefCamera_TraceCameraDistance = 0x140C104B0;

    // Mesh Visibility & Character Pipeline
    constexpr uintptr_t AThiefPawn_Hero_ShowThirdPersonMesh = 0x140B3B850;
    constexpr uintptr_t AThiefPawn_Hero_HideThirdPersonMesh = 0x140B47D80;
    constexpr uintptr_t AThiefPawn_Hero_ShowAllMaterialSections = 0x140B3B630;
    constexpr uintptr_t AThiefPawn_Hero_ToggleMaterialSections = 0x140B3B790;
    constexpr uintptr_t AThiefPawn_Hero_RefreshMeshVisibility = 0x140B4A050;
    constexpr uintptr_t USkeletalMeshComponent_HideBone = 0x140497270;
    constexpr uintptr_t USkeletalMeshComponent_ShowMaterialSection = 0x1404A2080;
    constexpr uintptr_t USkeletalMeshComponent_SetMaterialSectionHidden = 0x1404A2110;
    constexpr uintptr_t USkeletalMeshComponent_AttachComponent = 0x1404A96C0;
    constexpr uintptr_t GetLocalPlayerCameraManager = 0x140AF9360;
    constexpr uintptr_t UThiefCameraBehavior_SetFirstPersonView = 0x140BFC720;
}

// Function pointer signatures for runtime hooks and detours
namespace ThiefSignatures
{
    using tCalcSceneView = void(*)(ULocalPlayer* thisPlayer, void* SceneViewFamily, FVector* OutViewLocation, FRotator* OutViewRotation, void* Viewport, void* ViewFamily, void* AdditionalViewInitParams);
    using tGetPlayerViewPoint = void(*)(APlayerController* thisController, FVector* OutLocation, FRotator* OutRotation);
    using tGetCameraViewPoint = void(*)(AThiefCamera* thisCamera, FVector& OutLocation, FRotator& OutRotation);
    using tShowThirdPersonMesh = void(*)(AThiefPawn_Hero* thisHero);
    using tHideThirdPersonMesh = void(*)(AThiefPawn_Hero* thisHero);
    using tShowAllMaterialSections = void(*)(AThiefPawn_Hero* thisHero);
    using tSetFirstPersonView = void(*)(UThiefCameraBehavior* thisBehavior, uint32_t bFirstPerson);
    using tHideBone = void(*)(USkeletalMeshComponent* thisComp, int32_t BoneIndex, uint32_t bHide, uint32_t bPhysOnly, uint32_t bIncludeDescendants);
    using tShowMaterialSection = void(*)(USkeletalMeshComponent* thisComp, uint8_t LODIndex, int32_t SectionIndex, uint32_t bShow, uint32_t bPhysOnly, uint32_t bIncludeDescendants);
    using tSetMaterialSectionHidden = void(*)(USkeletalMeshComponent* thisComp, uint8_t LODIndex, int32_t SectionIndex, uint32_t bHide);
    using tToggleMaterialSections = void(*)(AThiefPawn_Hero* thisHero, void* Sections, uint32_t bHide);
    using tGetLocalPlayerCameraManager = uintptr_t(*)();
    using tAttachComponent = void(*)(USkeletalMeshComponent* mesh, void* comp);
    using tLineCheck = uint32_t(*)(void* World, FCheckResult* Result, AActor* SourceActor, const FVector* End, const FVector* Start, uint32_t TraceFlags, uint8_t TraceType, const FVector* Extent, void* Opt1, void* Opt2);
    using tTraceCameraDistance = float(*)(UThiefCameraBehavior* thisBehavior, const FVector& TargetLocation, const FVector& Direction, float DesiredDistance);
}
