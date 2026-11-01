#include <windows.h>
#include "camera_mod.h"

typedef HRESULT (WINAPI *tDirectInput8Create)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
typedef HRESULT (WINAPI *tDllCanUnloadNow)();
typedef HRESULT (WINAPI *tDllGetClassObject)(REFCLSID, REFIID, LPVOID*);
typedef HRESULT (WINAPI *tDllRegisterServer)();
typedef HRESULT (WINAPI *tDllUnregisterServer)();

static HMODULE s_hRealDInput8 = nullptr;
static tDirectInput8Create s_pfnRealDirectInput8Create = nullptr;
static tDllCanUnloadNow s_pfnRealDllCanUnloadNow = nullptr;
static tDllGetClassObject s_pfnRealDllGetClassObject = nullptr;
static tDllRegisterServer s_pfnRealDllRegisterServer = nullptr;
static tDllUnregisterServer s_pfnRealDllUnregisterServer = nullptr;

static bool LoadRealDInput8()
{
    if (s_hRealDInput8)
        return true;

    char systemDir[MAX_PATH];
    if (GetSystemDirectoryA(systemDir, MAX_PATH) == 0)
        return false;

    char realDllPath[MAX_PATH];
    wsprintfA(realDllPath, "%s\\dinput8.dll", systemDir);

    s_hRealDInput8 = LoadLibraryA(realDllPath);
    if (!s_hRealDInput8)
        return false;

    s_pfnRealDirectInput8Create = (tDirectInput8Create)GetProcAddress(s_hRealDInput8, "DirectInput8Create");
    s_pfnRealDllCanUnloadNow    = (tDllCanUnloadNow)GetProcAddress(s_hRealDInput8, "DllCanUnloadNow");
    s_pfnRealDllGetClassObject  = (tDllGetClassObject)GetProcAddress(s_hRealDInput8, "DllGetClassObject");
    s_pfnRealDllRegisterServer  = (tDllRegisterServer)GetProcAddress(s_hRealDInput8, "DllRegisterServer");
    s_pfnRealDllUnregisterServer= (tDllUnregisterServer)GetProcAddress(s_hRealDInput8, "DllUnregisterServer");

    return (s_pfnRealDirectInput8Create != nullptr);
}

extern "C" __declspec(dllexport) HRESULT WINAPI DirectInput8Create(
    HINSTANCE hinst, DWORD dwVersion, REFIID riidltf, LPVOID *ppvOut, LPUNKNOWN punkOuter)
{
    if (!LoadRealDInput8() || !s_pfnRealDirectInput8Create)
        return E_FAIL;
    return s_pfnRealDirectInput8Create(hinst, dwVersion, riidltf, ppvOut, punkOuter);
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllCanUnloadNow()
{
    if (!LoadRealDInput8() || !s_pfnRealDllCanUnloadNow)
        return S_OK;
    return s_pfnRealDllCanUnloadNow();
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv)
{
    if (!LoadRealDInput8() || !s_pfnRealDllGetClassObject)
        return CLASS_E_CLASSNOTAVAILABLE;
    return s_pfnRealDllGetClassObject(rclsid, riid, ppv);
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllRegisterServer()
{
    if (!LoadRealDInput8() || !s_pfnRealDllRegisterServer)
        return S_OK;
    return s_pfnRealDllRegisterServer();
}

extern "C" __declspec(dllexport) HRESULT WINAPI DllUnregisterServer()
{
    if (!LoadRealDInput8() || !s_pfnRealDllUnregisterServer)
        return S_OK;
    return s_pfnRealDllUnregisterServer();
}

extern "C" BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    switch (fdwReason)
    {
    case DLL_PROCESS_ATTACH:
        g_hModule = (HMODULE)hinstDLL;
        DisableThreadLibraryCalls(hinstDLL);
        InitCameraMod();
        break;

    case DLL_PROCESS_DETACH:
        ShutdownCameraMod();
        if (s_hRealDInput8)
        {
            FreeLibrary(s_hRealDInput8);
            s_hRealDInput8 = nullptr;
        }
        break;
    }
    return TRUE;
}
