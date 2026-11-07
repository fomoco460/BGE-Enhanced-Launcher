// bgehook - d3d9.dll proxy for Beyond Good & Evil (PC, Direct3D 9)
//
// Features: 60 fps frame limiter, vsync / resolution / window-mode override,
// Hor+ widescreen + FOV, 4:3 HUD option, forced anisotropic filtering and MSAA,
// per-axis camera inversion (controller + mouse), and chain-loading of
// ReShade or DXVK.
//
// Config: bgehook.ini next to BGE.exe (written by the launcher). Log: bgehook.log.
// MIT licence - see LICENSE.
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <mmsystem.h>

typedef IDirect3D9 *(WINAPI *PFN_Create9)(UINT);
typedef HRESULT(WINAPI *PFN_Create9Ex)(UINT, IDirect3D9Ex **);

static HMODULE g_next;
static PFN_Create9 o_Create9;
static PFN_Create9Ex o_Create9Ex;
static char g_dir[MAX_PATH];
static FILE *g_log;
static unsigned g_frames;
static UINT g_gameW, g_gameH, g_bbW, g_bbH;
static int g_ws, g_hud;
static float g_fov = 1.0f, g_aspect = 16.0f / 9.0f;

static struct {
    int fps;           // 0 = unlimited
    int vsync;         // -1 = game, 0 off, 1 on
    int width, height; // 0 = game
    int windowed;      // -1 game, 0 fullscreen, 1 windowed, 2 borderless
    int aniso;         // 0 off, 2..16
    int msaa;          // 0 off, 2/4/8
    int refresh;       // 0 = game
    char chain[MAX_PATH];
    int log;
} cfg;

static void hlog(const char *fmt, ...) {
    if (!g_log) return;
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fputc('\n', g_log); fflush(g_log);
}

static int ini_int(const char *k, int def) {
    char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\bgehook.ini", g_dir);
    return GetPrivateProfileIntA("bgehook", k, def, p);
}

static int contains_ci(const char *h, const char *n) {
    size_t ln = strlen(n);
    for (; *h; h++) if (_strnicmp(h, n, ln) == 0) return 1;
    return 0;
}

static void load_cfg(void) {
    char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\bgehook.ini", g_dir);
    cfg.fps = ini_int("FPSLimit", 60);
    cfg.vsync = ini_int("VSync", -1);
    cfg.width = ini_int("Width", 0);
    cfg.height = ini_int("Height", 0);
    cfg.windowed = ini_int("WindowMode", -1);
    cfg.aniso = ini_int("Anisotropy", 16);
    cfg.msaa = ini_int("MSAA", 0);
    cfg.refresh = ini_int("RefreshRate", 0);
    cfg.log = ini_int("Log", 1);
    g_ws = ini_int("WidescreenFix", 1);
    g_hud = ini_int("HUDMode", 0);
    g_fov = ini_int("FOVPercent", 100) / 100.0f; if (g_fov < 0.5f || g_fov > 2.0f) g_fov = 1.0f;
    // ReShade runs as a Vulkan layer on top of DXVK; its layer manifest is only enabled
    // when this variable is set, so no other Vulkan program picks it up.
    if (ini_int("ReShadeVulkan", 0)) SetEnvironmentVariableA("BGE_RESHADE_VK", "1");
    if (ini_int("AudioDebug", 0)) { // DSOAL / OpenAL Soft logging (set in-process: Ubisoft Connect relaunches the game)
        char lp[MAX_PATH];
        SetEnvironmentVariableA("DSOAL_LOGLEVEL", "4"); // DSOAL adds 1: 4 = Debug, which is where EAX Set calls are logged
        snprintf(lp, sizeof lp, "%s\\dsoal.log", g_dir); SetEnvironmentVariableA("DSOAL_LOGFILE", lp);
        SetEnvironmentVariableA("ALSOFT_LOGLEVEL", "3");
        snprintf(lp, sizeof lp, "%s\\alsoft.log", g_dir); SetEnvironmentVariableA("ALSOFT_LOGFILE", lp);
    }
    GetPrivateProfileStringA("bgehook", "Chain", "", cfg.chain, sizeof cfg.chain, p);
}

// ---------------- frame limiter ----------------
static LARGE_INTEGER g_freq, g_next_t;
static HANDLE g_timer;

static void limit_frame(void) {
    if (cfg.fps <= 0) return;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    LONGLONG period = g_freq.QuadPart / cfg.fps;
    if (g_next_t.QuadPart == 0 || now.QuadPart - g_next_t.QuadPart > period * 4) {
        g_next_t.QuadPart = now.QuadPart + period;
        return;
    }
    LONGLONG remain = g_next_t.QuadPart - now.QuadPart;
    if (remain > 0) {
        // coarse sleep on a high-resolution timer, then spin the last ~1.5 ms
        LONGLONG us = remain * 1000000 / g_freq.QuadPart;
        if (us > 2000 && g_timer) {
            LARGE_INTEGER due; due.QuadPart = -(LONGLONG)(us - 1500) * 10;
            SetWaitableTimer(g_timer, &due, 0, NULL, NULL, FALSE);
            WaitForSingleObject(g_timer, INFINITE);
        }
        do { YieldProcessor(); QueryPerformanceCounter(&now); } while (now.QuadPart < g_next_t.QuadPart);
    }
    g_next_t.QuadPart += period;
}

// ---------------- vtable hooks ----------------
static void patch_vt(void *obj, int idx, void *fn, void **orig) {
    void **vt = *(void ***)obj;
    if (vt[idx] == fn) return;
    DWORD old;
    VirtualProtect(&vt[idx], sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
    if (orig && !*orig) *orig = vt[idx];
    vt[idx] = fn;
    VirtualProtect(&vt[idx], sizeof(void *), old, &old);
}

typedef HRESULT(STDMETHODCALLTYPE *PFN_CreateDevice)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
typedef UINT(STDMETHODCALLTYPE *PFN_GetModeCount)(IDirect3D9 *, UINT, D3DFORMAT);
typedef HRESULT(STDMETHODCALLTYPE *PFN_EnumModes)(IDirect3D9 *, UINT, D3DFORMAT, UINT, D3DDISPLAYMODE *);
typedef HRESULT(STDMETHODCALLTYPE *PFN_Present)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
typedef HRESULT(STDMETHODCALLTYPE *PFN_Reset)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
typedef HRESULT(STDMETHODCALLTYPE *PFN_SetSamplerState)(IDirect3DDevice9 *, DWORD, D3DSAMPLERSTATETYPE, DWORD);
typedef HRESULT(STDMETHODCALLTYPE *PFN_SetTransform)(IDirect3DDevice9 *, D3DTRANSFORMSTATETYPE, const D3DMATRIX *);
typedef HRESULT(STDMETHODCALLTYPE *PFN_SetViewport)(IDirect3DDevice9 *, const D3DVIEWPORT9 *);

static PFN_CreateDevice o_CreateDevice;
static PFN_GetModeCount o_GetModeCount;
static PFN_EnumModes o_EnumModes;
static PFN_Present o_Present;
static PFN_Reset o_Reset;
static PFN_SetSamplerState o_SetSamplerState;
static PFN_SetTransform o_SetTransform;
static PFN_SetViewport o_SetViewport;
static DWORD g_maxAniso = 16;
static HWND g_hwnd;

// The game picks a display mode by index from its registry setting. Report the launcher's
// resolution for every index so it always asks for the right one (else it may pick 640x480).
static UINT STDMETHODCALLTYPE h_GetModeCount(IDirect3D9 *d, UINT a, D3DFORMAT f) {
    UINT n = o_GetModeCount(d, a, f);
    return (cfg.width > 0 && n < 64) ? 64 : n;
}
static HRESULT STDMETHODCALLTYPE h_EnumModes(IDirect3D9 *d, UINT a, D3DFORMAT f, UINT i, D3DDISPLAYMODE *m) {
    if (cfg.width > 0 && cfg.height > 0 && m) {
        D3DDISPLAYMODE cur; UINT hz = 60;
        if (SUCCEEDED(IDirect3D9_GetAdapterDisplayMode(d, a, &cur))) hz = cur.RefreshRate;
        m->Width = cfg.width; m->Height = cfg.height; m->Format = f;
        m->RefreshRate = cfg.refresh > 0 ? (UINT)cfg.refresh : hz;
        return D3D_OK;
    }
    return o_EnumModes(d, a, f, i, m);
}

static HRESULT STDMETHODCALLTYPE h_SetTransform(IDirect3DDevice9 *d, D3DTRANSFORMSTATETYPE t, const D3DMATRIX *m) {
    if (t == D3DTS_PROJECTION && m && (g_ws || g_hud || g_fov != 1.0f)) {
        D3DMATRIX n = *m;
        float r = n._11 != 0 ? -n._22 / n._11 : 0;
        if (n._34 == 1.0f && n._44 == 0.0f && r > 1.32f && r < 1.345f) {
            // the game hard-codes a 4:3 perspective: make it Hor+ for the real aspect, then apply FOV
            if (g_ws) n._11 = -n._22 / g_aspect;
            n._11 /= g_fov; n._22 /= g_fov;
            return o_SetTransform(d, t, &n);
        }
        if (g_hud == 1 && n._34 == 0.0f && n._44 == 1.0f && n._11 > 0 && n._11 < 0.01f) {
            float k = (4.0f / 3.0f) / g_aspect; // pillarbox the 2D layer to 4:3
            if (k < 1.0f) { n._11 *= k; n._41 *= k; return o_SetTransform(d, t, &n); }
        }
    }
    return o_SetTransform(d, t, m);
}

// The game sizes full-screen viewports for the resolution it asked for; rescale them to the
// real backbuffer (only when drawing to the backbuffer, not to its own render targets).
static HRESULT STDMETHODCALLTYPE h_SetViewport(IDirect3DDevice9 *d, const D3DVIEWPORT9 *v) {
    if (v && g_gameW && g_gameH && g_bbW && g_bbH && (g_gameW != g_bbW || g_gameH != g_bbH)
        && v->X + v->Width <= g_gameW && v->Y + v->Height <= g_gameH) {
        IDirect3DSurface9 *rt = NULL;
        if (SUCCEEDED(IDirect3DDevice9_GetRenderTarget(d, 0, &rt)) && rt) {
            IDirect3DSurface9 *bb = NULL; int isbb = 0;
            if (SUCCEEDED(IDirect3DDevice9_GetBackBuffer(d, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) { isbb = (bb == rt); IDirect3DSurface9_Release(bb); }
            IDirect3DSurface9_Release(rt);
            if (isbb) {
                D3DVIEWPORT9 n = *v;
                n.X = v->X * g_bbW / g_gameW; n.Width = v->Width * g_bbW / g_gameW;
                n.Y = v->Y * g_bbH / g_gameH; n.Height = v->Height * g_bbH / g_gameH;
                return o_SetViewport(d, &n);
            }
        }
    }
    return o_SetViewport(d, v);
}

static void log_pp(const char *tag, D3DPRESENT_PARAMETERS *pp) {
    hlog("%s: %ux%u fmt=%u bb=%u ms=%u swap=%u win=%d hz=%u interval=0x%x",
         tag, pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat, pp->BackBufferCount,
         pp->MultiSampleType, pp->SwapEffect, pp->Windowed, pp->FullScreen_RefreshRateInHz, pp->PresentationInterval);
}

static void apply_window_mode(HWND h) {
    if (!h || cfg.windowed < 1) return;
    int sw = cfg.width ? cfg.width : GetSystemMetrics(SM_CXSCREEN);
    int sh = cfg.height ? cfg.height : GetSystemMetrics(SM_CYSCREEN);
    if (cfg.windowed == 2) {
        SetWindowLongA(h, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(h, HWND_TOP, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        RECT r = {0, 0, sw, sh};
        LONG st = WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
        SetWindowLongA(h, GWL_STYLE, st | WS_VISIBLE);
        AdjustWindowRect(&r, st, FALSE);
        SetWindowPos(h, HWND_NOTOPMOST, 40, 40, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
}

static void fix_pp(IDirect3D9 *d3d, UINT adapter, D3DPRESENT_PARAMETERS *pp) {
    log_pp("game pp", pp);
    g_gameW = pp->BackBufferWidth; g_gameH = pp->BackBufferHeight;
    if (cfg.width > 0 && cfg.height > 0) { pp->BackBufferWidth = cfg.width; pp->BackBufferHeight = cfg.height; }
    if (cfg.windowed >= 0) {
        pp->Windowed = cfg.windowed ? TRUE : FALSE;
        if (pp->Windowed) {
            pp->FullScreen_RefreshRateInHz = 0;
            if (cfg.windowed == 2) { pp->BackBufferWidth = cfg.width ? cfg.width : GetSystemMetrics(SM_CXSCREEN);
                                     pp->BackBufferHeight = cfg.height ? cfg.height : GetSystemMetrics(SM_CYSCREEN); }
        }
    }
    if (!pp->Windowed && cfg.refresh > 0) pp->FullScreen_RefreshRateInHz = cfg.refresh;
    if (cfg.vsync == 0) pp->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    else if (cfg.vsync == 1) pp->PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    if (cfg.msaa >= 2 && d3d) {
        D3DMULTISAMPLE_TYPE t = (D3DMULTISAMPLE_TYPE)cfg.msaa;
        DWORD q = 0;
        D3DFORMAT f = pp->BackBufferFormat == D3DFMT_UNKNOWN ? D3DFMT_X8R8G8B8 : pp->BackBufferFormat;
        if (SUCCEEDED(IDirect3D9_CheckDeviceMultiSampleType(d3d, adapter, D3DDEVTYPE_HAL, f, pp->Windowed, t, &q))) {
            pp->MultiSampleType = t; pp->MultiSampleQuality = 0;
            pp->SwapEffect = D3DSWAPEFFECT_DISCARD;
            pp->Flags &= ~D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
        } else hlog("MSAA x%d not supported", cfg.msaa);
    }
    if (pp->BackBufferWidth && pp->BackBufferHeight) g_aspect = (float)pp->BackBufferWidth / pp->BackBufferHeight;
    else if (pp->Windowed) { RECT rc; if (GetClientRect(pp->hDeviceWindow, &rc) && rc.bottom) g_aspect = (float)rc.right / rc.bottom; }
    g_bbW = pp->BackBufferWidth; g_bbH = pp->BackBufferHeight;
    if (!g_bbW || !g_bbH) { RECT rc; if (GetClientRect(pp->hDeviceWindow ? pp->hDeviceWindow : g_hwnd, &rc)) { g_bbW = rc.right; g_bbH = rc.bottom; } }
    log_pp("new  pp", pp);
}

static HRESULT STDMETHODCALLTYPE h_Present(IDirect3DDevice9 *d, const RECT *a, const RECT *b, HWND w, const RGNDATA *r) {
    limit_frame();
    static LARGE_INTEGER t0;
    ++g_frames;
    if (g_frames == 300) QueryPerformanceCounter(&t0);
    if (g_frames == 900) { LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
        hlog("measured %.1f fps over 600 frames (cap %d)", 600.0 * g_freq.QuadPart / (double)(t1.QuadPart - t0.QuadPart), cfg.fps); }
    return o_Present(d, a, b, w, r);
}

static HRESULT STDMETHODCALLTYPE h_Reset(IDirect3DDevice9 *d, D3DPRESENT_PARAMETERS *pp) {
    IDirect3D9 *d3d = NULL; IDirect3DDevice9_GetDirect3D(d, &d3d);
    fix_pp(d3d, D3DADAPTER_DEFAULT, pp);
    if (d3d) IDirect3D9_Release(d3d);
    HRESULT hr = o_Reset(d, pp);
    hlog("Reset -> 0x%08lx", hr);
    apply_window_mode(g_hwnd);
    return hr;
}

static HRESULT STDMETHODCALLTYPE h_SetSamplerState(IDirect3DDevice9 *d, DWORD s, D3DSAMPLERSTATETYPE t, DWORD v) {
    if (cfg.aniso >= 2) {
        if ((t == D3DSAMP_MINFILTER || t == D3DSAMP_MAGFILTER) && v == D3DTEXF_LINEAR) {
            o_SetSamplerState(d, s, D3DSAMP_MAXANISOTROPY, g_maxAniso);
            v = D3DTEXF_ANISOTROPIC;
        } else if (t == D3DSAMP_MAXANISOTROPY) v = g_maxAniso;
    }
    return o_SetSamplerState(d, s, t, v);
}

static HRESULT STDMETHODCALLTYPE h_CreateDevice(IDirect3D9 *d3d, UINT ad, D3DDEVTYPE t, HWND hw, DWORD flags,
                                                D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out) {
    hlog("CreateDevice adapter=%u type=%d flags=0x%lx", ad, t, flags);
    fix_pp(d3d, ad, pp);
    g_hwnd = pp->hDeviceWindow ? pp->hDeviceWindow : hw;
    if (pp->Windowed) apply_window_mode(g_hwnd);
    HRESULT hr = o_CreateDevice(d3d, ad, t, hw, flags, pp, out);
    hlog("CreateDevice -> 0x%08lx", hr);
    if (SUCCEEDED(hr) && out && *out) {
        D3DCAPS9 caps;
        if (SUCCEEDED(IDirect3DDevice9_GetDeviceCaps(*out, &caps)))
            g_maxAniso = caps.MaxAnisotropy < (DWORD)cfg.aniso ? caps.MaxAnisotropy : (DWORD)cfg.aniso;
        patch_vt(*out, 17, h_Present, (void **)&o_Present);
        patch_vt(*out, 16, h_Reset, (void **)&o_Reset);
        patch_vt(*out, 69, h_SetSamplerState, (void **)&o_SetSamplerState);
        patch_vt(*out, 44, h_SetTransform, (void **)&o_SetTransform);
        patch_vt(*out, 47, h_SetViewport, (void **)&o_SetViewport);
    }
    return hr;
}

// ---------------- chain loading ----------------
static void load_next(void) {
    if (g_next) return;
    char p[MAX_PATH];
    if (cfg.chain[0]) {
        if (cfg.chain[1] == ':') snprintf(p, sizeof p, "%s", cfg.chain);
        else snprintf(p, sizeof p, "%s\\%s", g_dir, cfg.chain);
        if (contains_ci(cfg.chain, "reshade")) { // ReShade hooks the system d3d9: make sure it's loaded first
            char sp[MAX_PATH]; GetSystemDirectoryA(sp, sizeof sp); strcat(sp, "\\d3d9.dll"); LoadLibraryA(sp);
        }
        g_next = LoadLibraryA(p);
        hlog("chain %s -> %p (err %lu)", p, (void *)g_next, g_next ? 0 : GetLastError());
    }
    if (!g_next) {
        GetSystemDirectoryA(p, sizeof p); strcat(p, "\\d3d9.dll");
        g_next = LoadLibraryA(p);
        hlog("system d3d9 %s -> %p", p, (void *)g_next);
    }
    o_Create9 = (PFN_Create9)GetProcAddress(g_next, "Direct3DCreate9");
    o_Create9Ex = (PFN_Create9Ex)GetProcAddress(g_next, "Direct3DCreate9Ex");
}

IDirect3D9 *WINAPI Direct3DCreate9(UINT sdk) {
    load_next();
    IDirect3D9 *d = o_Create9 ? o_Create9(sdk) : NULL;
    hlog("Direct3DCreate9(%u) -> %p", sdk, (void *)d);
    if (d) {
        patch_vt(d, 16, h_CreateDevice, (void **)&o_CreateDevice);
        patch_vt(d, 6, h_GetModeCount, (void **)&o_GetModeCount);
        patch_vt(d, 7, h_EnumModes, (void **)&o_EnumModes);
    }
    return d;
}

HRESULT WINAPI Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex **out) {
    load_next();
    return o_Create9Ex ? o_Create9Ex(sdk, out) : E_NOTIMPL;
}

// D3DPERF stubs (some tools/games probe these)
int WINAPI D3DPERF_BeginEvent(DWORD c, LPCWSTR n) { (void)c; (void)n; return 0; }
int WINAPI D3DPERF_EndEvent(void) { return 0; }
DWORD WINAPI D3DPERF_GetStatus(void) { return 0; }
BOOL WINAPI D3DPERF_QueryRepeatFrame(void) { return FALSE; }
void WINAPI D3DPERF_SetMarker(DWORD c, LPCWSTR n) { (void)c; (void)n; }
void WINAPI D3DPERF_SetOptions(DWORD o) { (void)o; }
void WINAPI D3DPERF_SetRegion(DWORD c, LPCWSTR n) { (void)c; (void)n; }

// ---------------- camera axis flips (controller + mouse) ----------------
// The game's own "invert camera" option flips both axes together. These flip one axis
// at the input level, on top of whatever the game does.
static int g_flipPadX, g_flipPadY, g_flipMouseX, g_flipMouseY;
typedef struct { WORD wButtons; BYTE bLT, bRT; SHORT sLX, sLY, sRX, sRY; } XPAD_;
typedef struct { DWORD dwPacketNumber; XPAD_ Gamepad; } XSTATE_;
typedef DWORD (WINAPI *PFN_XGetState)(DWORD, XSTATE_ *);
static PFN_XGetState o_XGetState;
static SHORT neg16(SHORT v) { return v == -32768 ? 32767 : (SHORT)-v; }
static DWORD WINAPI h_XGetState(DWORD i, XSTATE_ *s) {
    DWORD r = o_XGetState(i, s);
    if (r == ERROR_SUCCESS && s) {
        if (g_flipPadX) s->Gamepad.sRX = neg16(s->Gamepad.sRX);
        if (g_flipPadY) s->Gamepad.sRY = neg16(s->Gamepad.sRY);
    }
    return r;
}
// DirectInput 8 (mouse)
static const GUID GUID_SysMouse_ = {0x6F1D2B60,0xD5A0,0x11CF,{0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00}};
typedef HRESULT (WINAPI *PFN_DI8Create)(HINSTANCE, DWORD, REFIID, void **, void *);
typedef HRESULT (STDMETHODCALLTYPE *PFN_DICreateDev)(void *, REFGUID, void **, void *);
typedef HRESULT (STDMETHODCALLTYPE *PFN_DIGetState)(void *, DWORD, void *);
typedef HRESULT (STDMETHODCALLTYPE *PFN_DIGetData)(void *, DWORD, void *, DWORD *, DWORD);
static PFN_DI8Create o_DI8Create; static PFN_DICreateDev o_DICreateDev;
static PFN_DIGetState o_DIGetState; static PFN_DIGetData o_DIGetData;
static void *g_mouseDev;
static HRESULT STDMETHODCALLTYPE h_DIGetState(void *d, DWORD cb, void *p) {
    HRESULT r = o_DIGetState(d, cb, p);
    if (SUCCEEDED(r) && d == g_mouseDev && p && cb >= 8) {
        LONG *l = (LONG *)p;
        if (g_flipMouseX) l[0] = -l[0];
        if (g_flipMouseY) l[1] = -l[1];
    }
    return r;
}
static HRESULT STDMETHODCALLTYPE h_DIGetData(void *d, DWORD cb, void *p, DWORD *n, DWORD fl) {
    HRESULT r = o_DIGetData(d, cb, p, n, fl);
    if (SUCCEEDED(r) && d == g_mouseDev && p && n && cb >= 8) {
        DWORD i;
        for (i = 0; i < *n; i++) {
            DWORD *e = (DWORD *)((BYTE *)p + i * cb); // dwOfs, dwData, ...
            if ((e[0] == 0 && g_flipMouseX) || (e[0] == 4 && g_flipMouseY)) e[1] = (DWORD)-(LONG)e[1];
        }
    }
    return r;
}
static HRESULT STDMETHODCALLTYPE h_DICreateDev(void *di, REFGUID g, void **dev, void *u) {
    HRESULT r = o_DICreateDev(di, g, dev, u);
    if (SUCCEEDED(r) && dev && *dev && IsEqualGUID(g, &GUID_SysMouse_)) {
        g_mouseDev = *dev;
        if (!o_DIGetState) patch_vt(*dev, 9, (void *)h_DIGetState, (void **)&o_DIGetState);
        if (!o_DIGetData) patch_vt(*dev, 10, (void *)h_DIGetData, (void **)&o_DIGetData);
        hlog("input: mouse device hooked");
    }
    return r;
}
static HRESULT WINAPI h_DI8Create(HINSTANCE h, DWORD v, REFIID id, void **out, void *u) {
    HRESULT r = o_DI8Create(h, v, id, out, u);
    if (SUCCEEDED(r) && out && *out && !o_DICreateDev)
        patch_vt(*out, 3, (void *)h_DICreateDev, (void **)&o_DICreateDev);
    return r;
}
// Patch an entry in BGE.exe's import table, by name or (if ord != 0) by ordinal.
static int iat_patch(const char *dll, const char *fn, WORD ord, void *hook, void **orig) {
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR *d;
    if (!dd.VirtualAddress) return 0;
    for (d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dd.VirtualAddress); d->Name; d++) {
        if (lstrcmpiA((char *)(base + d->Name), dll)) continue;
        IMAGE_THUNK_DATA *nm = (IMAGE_THUNK_DATA *)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA *ad = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
        for (; nm->u1.AddressOfData; nm++, ad++) {
            if (IMAGE_SNAP_BY_ORDINAL(nm->u1.Ordinal)) { if (!ord || IMAGE_ORDINAL(nm->u1.Ordinal) != ord) continue; }
            else if (strcmp((char *)((IMAGE_IMPORT_BY_NAME *)(base + nm->u1.AddressOfData))->Name, fn)) continue;
            if ((void *)ad->u1.Function == hook) return 1;
            DWORD old; VirtualProtect(&ad->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
            *orig = (void *)ad->u1.Function; ad->u1.Function = (DWORD_PTR)hook;
            VirtualProtect(&ad->u1.Function, sizeof(void *), old, &old);
            return 1;
        }
    }
    return 0;
}
static void input_init(void) {
    g_flipPadX = ini_int("FlipStickX", 0);   g_flipPadY = ini_int("FlipStickY", 0);
    g_flipMouseX = ini_int("FlipMouseX", 0); g_flipMouseY = ini_int("FlipMouseY", 0);
    if (g_flipPadX || g_flipPadY) {
        // BGE imports XINPUT1_3 by ordinal; ordinal 2 is XInputGetState
        int ok = iat_patch("XINPUT1_3.dll", "XInputGetState", 2, (void *)h_XGetState, (void **)&o_XGetState);
        hlog("input: XInputGetState hook %s (flip X=%d Y=%d)", ok ? "ok" : "NOT FOUND", g_flipPadX, g_flipPadY);
    }
    if (g_flipMouseX || g_flipMouseY) {
        int ok = iat_patch("DINPUT8.dll", "DirectInput8Create", 0, (void *)h_DI8Create, (void **)&o_DI8Create);
        hlog("input: DirectInput8Create hook %s (flip X=%d Y=%d)", ok ? "ok" : "NOT FOUND", g_flipMouseX, g_flipMouseY);
    }
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID x) {
    (void)x;
    if (r == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        GetModuleFileNameA(h, g_dir, sizeof g_dir);
        char *s = strrchr(g_dir, '\\'); if (s) *s = 0;
        load_cfg();
        if (cfg.log) { char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\bgehook.log", g_dir); g_log = fopen(p, "w"); }
        QueryPerformanceFrequency(&g_freq);
        g_timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /*HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
        if (!g_timer) g_timer = CreateWaitableTimerA(NULL, FALSE, NULL);
        timeBeginPeriod(1);
        hlog("bgehook loaded: fps=%d vsync=%d %dx%d win=%d aniso=%d msaa=%d chain='%s'",
             cfg.fps, cfg.vsync, cfg.width, cfg.height, cfg.windowed, cfg.aniso, cfg.msaa, cfg.chain);
        input_init();
    } else if (r == DLL_PROCESS_DETACH) {
        if (g_log) fclose(g_log);
    }
    return TRUE;
}
