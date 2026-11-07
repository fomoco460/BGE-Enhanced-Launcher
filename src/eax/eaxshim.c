// eax.dll - open-source stand-in for Creative's EAX helper DLL, for Beyond Good & Evil.
//
// When its EAX setting is on (registry "Sound FX" != 1 and "Audio board FX Capable" == 2),
// BGE.exe does LoadLibrary("eax.dll") + GetProcAddress("EAXDirectSoundCreate8") and calls the
// result without checking it, so the DLL must exist. Creative's DLL just creates the
// DirectSound device; the EAX work itself goes through property sets on DirectSound buffers,
// which DSOAL (the game's dsound.dll) implements with OpenAL Soft. So this shim only has to
// hand the call to DSOAL.
//
// MIT licence - part of BGE Enhanced Launcher.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

typedef HRESULT(WINAPI *PFN_DSCreate)(const GUID *, void **, void *);

static FILE *g_log;
static char g_dir[MAX_PATH];

static HRESULT create(const char *fn, const GUID *g, void **ds, void *u) {
    // Use the dsound.dll the game already loaded (DSOAL, from the game folder).
    HMODULE m = GetModuleHandleA("dsound.dll");
    if (!m) { char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\dsound.dll", g_dir); m = LoadLibraryA(p); }
    PFN_DSCreate f = m ? (PFN_DSCreate)GetProcAddress(m, fn) : NULL;
    HRESULT hr = f ? f(g, ds, u) : E_FAIL;
    if (g_log) { fprintf(g_log, "eax shim: %s -> 0x%08lx\n", fn, (unsigned long)hr); fflush(g_log); }
    return hr;
}

__declspec(dllexport) HRESULT WINAPI EAXDirectSoundCreate8(const GUID *g, void **ds, void *u) { return create("DirectSoundCreate8", g, ds, u); }
__declspec(dllexport) HRESULT WINAPI EAXDirectSoundCreate(const GUID *g, void **ds, void *u) { return create("DirectSoundCreate", g, ds, u); }

BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID x) {
    (void)x;
    if (r == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        GetModuleFileNameA(h, g_dir, sizeof g_dir);
        char *s = strrchr(g_dir, '\\'); if (s) *s = 0;
        char ini[MAX_PATH]; snprintf(ini, sizeof ini, "%s\\bgehook.ini", g_dir);
        if (GetPrivateProfileIntA("bgehook", "Log", 1, ini)) {
            char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\eax.log", g_dir);
            g_log = fopen(p, "w");
            if (g_log) { fputs("eax shim loaded\n", g_log); fflush(g_log); }
        }
    } else if (r == DLL_PROCESS_DETACH && g_log) fclose(g_log);
    return TRUE;
}
