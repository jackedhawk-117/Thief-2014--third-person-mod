#pragma once
#include <windows.h>
#include <cstdint>
#include "thief_structs.h"

// Configuration settings for Third-Person Camera
struct ModConfig
{
    bool bEnabled;              // Whether third-person camera is currently active
    float Distance;             // Distance behind Garrett (units)
    float HeightOffset;         // Upward camera height offset (units)
    float ShoulderOffset;       // Rightward shoulder offset (units)
    float MinDistance;          // Minimum collision pull-in distance (prevents head clipping)
    int ToggleKey;              // Virtual key code for toggling 3P (default VK_F4)
    float SmoothFactor;         // Lerp smoothing factor per frame (0.0 - 1.0)
};

extern ModConfig g_Config;
extern bool g_bRunning;
extern HMODULE g_hModule;
extern uintptr_t g_Base;

// Core functions
void InitCameraMod();
void ShutdownCameraMod();
void LoadConfig();
void SaveConfig();

// Helper macro for relative address calculation with ASLR support
#define RESOLVE_ADDR(va) ((void*)(g_Base + ((uintptr_t)(va) - 0x140000000ULL)))
