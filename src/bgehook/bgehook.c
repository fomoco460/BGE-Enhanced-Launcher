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
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include <mmsystem.h>

static void parse_hotkey(const char *s);
static int iat_patch(const char *dll, const char *fn, WORD ord, void *hook, void **orig);

typedef IDirect3D9 *(WINAPI *PFN_Create9)(UINT);
typedef HRESULT(WINAPI *PFN_Create9Ex)(UINT, IDirect3D9Ex **);

static HMODULE g_next;
static PFN_Create9 o_Create9;
static PFN_Create9Ex o_Create9Ex;
static char g_dir[MAX_PATH];
static FILE *g_log;
static unsigned g_frames;
static UINT g_gameW, g_gameH, g_bbW, g_bbH;
static int g_ws, g_hud, g_engineWs;
static float g_fov = 1.0f, g_aspect = 16.0f / 9.0f;
static volatile float g_fovK = 1.0f;   // engine widescreen: FOV factor
// 1/tan(h'/2) and the game's own angle for recently widened cameras (menus such as the inventory use several per frame)
#define WIDE_SLOTS 16
static volatile float g_wideCot[WIDE_SLOTS], g_wideFov[WIDE_SLOTS]; static volatile LONG g_wideNext;
static volatile float g_viewShift;   // ViewHeight: vertical lens shift of the main camera, fraction of half the screen
static volatile int g_inTitle;       // title menu showing: its 3D logo uses a gameplay-like camera, so no view shift

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
    if (cfg.width <= 0 || cfg.height <= 0) {
        // 0 = the main screen's own resolution, read when the game starts (real pixels: see SetProcessDPIAware)
        cfg.width = GetSystemMetrics(SM_CXSCREEN); cfg.height = GetSystemMetrics(SM_CYSCREEN);
    }
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
    { char hk[64]; GetPrivateProfileStringA("bgehook", "ToggleKey", "Alt+Enter", hk, sizeof hk, p); parse_hotkey(hk); }
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

// High water quality draws the scene a second time into a small screen copy, then maps that copy onto the water
// with a texture matrix built from the game's own 4:3 projection (0.5 * its scale). The copy is drawn with our
// adjusted projection, so the matrix must get the same adjustment, or the reflections land in the wrong place
// (worse the wider the screen). Remember how each adjusted projection differs from the game's.
#define TM_SLOTS 8
static struct { float g11, sx, sy, dx, dy; } g_tm[TM_SLOTS];
static LONG g_tmNext;
static int g_tmLogged;
static void tm_remember(const D3DMATRIX *game, const D3DMATRIX *mod) {
    if (game->_11 == 0 || game->_22 == 0) return;
    float g11 = game->_11, sx = mod->_11 / game->_11, sy = mod->_22 / game->_22;
    float dx = mod->_31 - game->_31, dy = mod->_32 - game->_32;
    for (int i = 0; i < TM_SLOTS; i++)
        if (g_tm[i].g11 == g11) { g_tm[i].sx = sx; g_tm[i].sy = sy; g_tm[i].dx = dx; g_tm[i].dy = dy; return; }
    int i = (int)(InterlockedIncrement(&g_tmNext) % TM_SLOTS);
    g_tm[i].g11 = g11; g_tm[i].sx = sx; g_tm[i].sy = sy; g_tm[i].dx = dx; g_tm[i].dy = dy;
}
static int tm_fix(D3DMATRIX *n) {
    // a projective screen mapping: u = 0.5*x_ndc + 0.5, v = 0.5 - 0.5*y_ndc
    if (n->_14 != 0 || n->_24 != 0 || n->_34 != 1.0f || n->_11 <= 0 || n->_12 != 0 || n->_21 != 0) return 0;
    for (int i = 0; i < TM_SLOTS; i++) {
        float g = g_tm[i].g11;
        if (g > 0 && fabsf(2.0f * n->_11 - g) < 2e-3f * g) {
            if (g_tm[i].sx == 1.0f && g_tm[i].sy == 1.0f && g_tm[i].dx == 0 && g_tm[i].dy == 0) return 0;
            n->_11 *= g_tm[i].sx; n->_22 *= g_tm[i].sy;
            n->_31 += 0.5f * g_tm[i].dx; n->_32 -= 0.5f * g_tm[i].dy;
            return 1;
        }
    }
    return 0;
}

static HRESULT STDMETHODCALLTYPE h_SetTransform(IDirect3DDevice9 *d, D3DTRANSFORMSTATETYPE t, const D3DMATRIX *m) {
    if (t == D3DTS_PROJECTION && m && (g_ws || g_hud || g_fov != 1.0f)) {
        D3DMATRIX n = *m;
        float r = n._11 != 0 ? -n._22 / n._11 : 0;
        if (n._34 == 1.0f && n._44 == 0.0f && r > 1.32f && r < 1.345f) {
            // the game hard-codes a 4:3 perspective: make it Hor+ for the real aspect, then apply FOV
            if (g_ws) {
                // the main camera, already widened by the engine patch: put the height back to the 4:3 view
                int wi = -1;
                if (g_fovK > 1.0f)
                    for (int i = 0; i < WIDE_SLOTS; i++)
                        if (g_wideCot[i] > 0 && fabsf(n._11 - g_wideCot[i]) < 1e-3f * g_wideCot[i]) { wi = i; break; }
                if (wi >= 0) {
                    n._22 *= g_fovK;
                    // lens shift: moves the picture without turning the camera. The engine culls with the
                    // wider (pre-correction) frustum, so a shift of up to ~75% of half the screen stays covered.
                    // gameplay cameras only (angle 1.2); the inventory/menu 3D scenes use 1.0 and must stay put
                    if (g_wideFov[wi] >= 1.1f && !g_inTitle) n._32 += (n._22 < 0 ? g_viewShift : -g_viewShift);
                }
                n._11 = -n._22 / g_aspect;
            }
            n._11 /= g_fov; n._22 /= g_fov;
            tm_remember(m, &n);
            return o_SetTransform(d, t, &n);
        }
        if (g_hud == 1 && n._34 == 0.0f && n._44 == 1.0f && n._11 > 0 && n._11 < 0.01f) {
            float k = (4.0f / 3.0f) / g_aspect; // pillarbox the 2D layer to 4:3
            if (k < 1.0f) { n._11 *= k; n._41 *= k; return o_SetTransform(d, t, &n); }
        }
    }
    if (t >= D3DTS_TEXTURE0 && t <= D3DTS_TEXTURE7 && m && (g_ws || g_fov != 1.0f)) {
        D3DMATRIX n = *m;
        if (tm_fix(&n)) {
            if (!g_tmLogged) { g_tmLogged = 1; hlog("water: screen-copy texture matrix adjusted (%.4f -> %.4f)", m->_22, n._22); }
            return o_SetTransform(d, t, &n);
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

static int g_winMode = -1; // current window mode (1 windowed, 2 borderless); can be toggled by hotkey

static void set_window_mode(HWND h, int mode) {
    if (!h || mode < 1) return;
    int scrW = GetSystemMetrics(SM_CXSCREEN), scrH = GetSystemMetrics(SM_CYSCREEN);
    if (mode == 2) {
        SetWindowLongA(h, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(h, HWND_TOP, 0, 0, scrW, scrH, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        int sw = cfg.width ? cfg.width : scrW, sh = cfg.height ? cfg.height : scrH;
        RECT wa; SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
        int aw = wa.right - wa.left, ah = wa.bottom - wa.top;
        if (sw > aw * 9 / 10 || sh > ah * 9 / 10) {
            // the game resolution fills the screen: use a window at 75% of the work area, same shape
            float k = 0.75f * ((float)aw / sw < (float)ah / sh ? (float)aw / sw : (float)ah / sh);
            sw = (int)(sw * k); sh = (int)(sh * k);
        }
        RECT r = {0, 0, sw, sh};
        LONG st = WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
        SetWindowLongA(h, GWL_STYLE, st | WS_VISIBLE);
        AdjustWindowRect(&r, st, FALSE);
        int ww = r.right - r.left, wh = r.bottom - r.top;
        SetWindowPos(h, HWND_NOTOPMOST, wa.left + (aw - ww) / 2, wa.top + (ah - wh) / 2, ww, wh, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
    g_winMode = mode;
}
static void apply_window_mode(HWND h) {
    if (cfg.windowed < 1) return;
    set_window_mode(h, g_winMode >= 1 ? g_winMode : cfg.windowed);
}

// ---------------- window-mode hotkey ----------------
// ToggleKey in bgehook.ini, e.g. "Alt+Enter", "F11", "Ctrl+F". Empty or "None" = off.
static int g_hkVk, g_hkAlt, g_hkCtrl, g_hkShift, g_hkWasDown;
static void parse_hotkey(const char *s) {
    char buf[64]; int i = 0;
    g_hkVk = 0;
    if (!s || !*s || !_stricmp(s, "none")) return;
    while (*s && i < 63) {
        const char *e = strchr(s, '+'); size_t n = e ? (size_t)(e - s) : strlen(s);
        if (n > 31) n = 31;
        memcpy(buf, s, n); buf[n] = 0;
        if (!_stricmp(buf, "alt")) g_hkAlt = 1;
        else if (!_stricmp(buf, "ctrl")) g_hkCtrl = 1;
        else if (!_stricmp(buf, "shift")) g_hkShift = 1;
        else if (!_stricmp(buf, "enter") || !_stricmp(buf, "return")) g_hkVk = VK_RETURN;
        else if ((buf[0] == 'F' || buf[0] == 'f') && atoi(buf + 1) >= 1 && atoi(buf + 1) <= 12) g_hkVk = VK_F1 + atoi(buf + 1) - 1;
        else if (strlen(buf) == 1 && isalnum((unsigned char)buf[0])) g_hkVk = toupper((unsigned char)buf[0]);
        if (!e) break;
        s = e + 1; i++;
    }
}
static int key_down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
static void toggle_window_mode(void) {
    int mode = (g_winMode >= 1 ? g_winMode : cfg.windowed) == 2 ? 1 : 2;
    set_window_mode(g_hwnd, mode);
    hlog("hotkey: window mode -> %s", mode == 2 ? "borderless" : "windowed");
}
static WNDPROC g_origWndProc;
// The game reads the keyboard through DirectInput, so its window never sees our hotkey as a message:
// poll it once per frame instead. "Held now or pressed since the last poll" (0x8001) also catches
// very short presses that fall between two frames.
static int key_hit(int vk) { return (GetAsyncKeyState(vk) & 0x8001) != 0; }
static void poll_hotkey(void) {
    if (!g_hkVk || cfg.windowed < 1 || !g_hwnd || GetForegroundWindow() != g_hwnd) { g_hkWasDown = 0; return; }
    int down = key_hit(g_hkVk) && key_hit(VK_MENU) == g_hkAlt && key_hit(VK_CONTROL) == g_hkCtrl && key_hit(VK_SHIFT) == g_hkShift;
    if (down && !g_hkWasDown) toggle_window_mode();
    g_hkWasDown = down && key_down(g_hkVk); // re-arm as soon as the key is actually released
}
// Window subclass: stop a lone Alt press from opening the window's system menu in windowed mode.
// That enters a modal menu loop which freezes the game until the next key press.
static LRESULT CALLBACK hook_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_SYSCOMMAND && (w & 0xFFF0) == SC_KEYMENU) return 0;
    return CallWindowProcA(g_origWndProc, h, m, w, l);
}
static void subclass_window(HWND h) {
    if (!h || g_origWndProc || cfg.windowed < 1) return;
    g_origWndProc = (WNDPROC)SetWindowLongPtrA(h, GWLP_WNDPROC, (LONG_PTR)hook_wndproc);
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

// Cheats: keep units / pearls topped up.
// The Jade engine puts its game data in one 512 MB block it allocates at start-up. The block's address
// changes every run, but the inventory counters always sit at the same offset inside it (checked across
// restarts and different saves). Before every write we check the memory around each counter still has its
// usual shape, so we never touch memory that doesn't look like the counter.
// Units: an isolated int32 with zeros on both sides.
// Pearls: the count field of an item record {0, count, 70, 4, 255}. (The pearl number shown on screen is a
// separate copy at +0x20CA44; changing that one only changes the display, so shops still see the real count.)
#define POOL_MIN_SIZE   0x1F000000u
#define OFF_UNITS       0x20C230u
#define OFF_PEARLS      0x2104ACu
static int g_keepUnits, g_keepPearls, g_cheatOk;
static unsigned char *g_pool;
static unsigned char *find_pool(void) {
    MEMORY_BASIC_INFORMATION m; unsigned char *a = (unsigned char *)0x10000;
    while ((ULONG_PTR)a < 0x7FFF0000 && VirtualQuery(a, &m, sizeof m)) {
        if (m.State == MEM_COMMIT && m.Type == MEM_PRIVATE && m.Protect == PAGE_READWRITE && m.RegionSize >= POOL_MIN_SIZE)
            return (unsigned char *)m.BaseAddress;
        a = (unsigned char *)m.BaseAddress + m.RegionSize;
    }
    return NULL;
}
static volatile int *counter_at(unsigned char *base, unsigned off, int maxv) {
    MEMORY_BASIC_INFORMATION m;
    if (!VirtualQuery(base + off, &m, sizeof m) || m.State != MEM_COMMIT || m.Protect != PAGE_READWRITE) return NULL;
    volatile int *p = (volatile int *)(base + off);
    return p[0] >= 0 && p[0] <= maxv && p[-1] == 0 ? p : NULL;
}
static int units_valid(void)  { volatile int *p = counter_at(g_pool, OFF_UNITS, 1000000); return p && p[1] == 0 && p[2] == 0; }
static int pearls_valid(void) { volatile int *p = counter_at(g_pool, OFF_PEARLS, 100000);    return p && p[1] == 70 && p[2] == 4 && p[3] == 255; }
// Main menu: the menu's state is a struct in the same block, {..., 10, 11, -1, menuId, cursor}. menuId 2 is
// the title menu (New Game, Load, Options, Bonus, Quit); the cursor is the highlighted item.
#define OFF_MENU_CURSOR 0xAD9AE4u
static int g_startOnLoad, g_fastIntro, g_menuSeen;
static int title_menu_cursor(void) {   // returns the cursor, or -1 if the title menu isn't showing
    if (!g_pool) return -1;
    MEMORY_BASIC_INFORMATION m;
    if (!VirtualQuery(g_pool + OFF_MENU_CURSOR, &m, sizeof m) || m.State != MEM_COMMIT || m.Protect != PAGE_READWRITE) return -1;
    volatile int *p = (volatile int *)(g_pool + OFF_MENU_CURSOR);
    if (p[-1] != 2 || p[-2] != -1 || p[-3] != 11 || p[-4] != 10 || p[-5] != 9 || p[0] < 0 || p[0] > 4) return -1;
    return p[0];
}
// Fast intro: the start-up intro is timed by the clock, so while it plays the game's own clock calls
// (QueryPerformanceCounter and GetTickCount in BGE.exe's import table; nobody else's) run INTRO_SPEED times
// faster. When the title menu appears the clock goes back to normal speed, carrying on from where it got to,
// so it never jumps backwards.
#define INTRO_SPEED 8
typedef BOOL (WINAPI *PFN_QPC)(LARGE_INTEGER *);
typedef DWORD (WINAPI *PFN_GTC)(void);
static PFN_QPC o_QPC; static PFN_GTC o_GTC;
static volatile LONG g_speedOn;
static LONGLONG g_qpc0, g_qpcExtra; static DWORD g_tick0, g_tickExtra;
static BOOL WINAPI h_QPC(LARGE_INTEGER *p) {
    BOOL r = o_QPC(p);
    if (r && p) p->QuadPart += g_speedOn ? (p->QuadPart - g_qpc0) * (INTRO_SPEED - 1) : g_qpcExtra;
    return r;
}
static DWORD WINAPI h_GTC(void) {
    DWORD t = o_GTC();
    return t + (g_speedOn ? (t - g_tick0) * (INTRO_SPEED - 1) : g_tickExtra);
}
static void intro_speed_init(void) {
    if (!g_fastIntro) return;
    int a = iat_patch("KERNEL32.dll", "QueryPerformanceCounter", 0, (void *)h_QPC, (void **)&o_QPC);
    int b = iat_patch("KERNEL32.dll", "GetTickCount", 0, (void *)h_GTC, (void **)&o_GTC);
    if (a) { LARGE_INTEGER t; o_QPC(&t); g_qpc0 = t.QuadPart; }
    if (b) g_tick0 = o_GTC();
    g_speedOn = a || b;
    hlog("intro: clock x%d %s (QPC %d, GetTickCount %d)", INTRO_SPEED, g_speedOn ? "on" : "NOT HOOKED", a, b);
}
static void intro_speed_off(void) {
    if (!g_speedOn) return;
    if (o_QPC) { LARGE_INTEGER t; o_QPC(&t); g_qpcExtra = (t.QuadPart - g_qpc0) * (INTRO_SPEED - 1); }
    if (o_GTC) g_tickExtra = (o_GTC() - g_tick0) * (INTRO_SPEED - 1);
    InterlockedExchange(&g_speedOn, 0);
    hlog("intro: clock back to normal at frame %u", g_frames);
}
static void watch_title_menu(void) {
    // Runs every frame until the title menu has been seen once, then stops.
    if (g_menuSeen || (!g_startOnLoad && !g_fastIntro)) return;
    if (!g_pool) { if (g_frames % 30) return; g_pool = find_pool(); if (!g_pool) return; hlog("menu: game data block at %p", g_pool); }
    int c = title_menu_cursor();
    if (c < 0) return;
    g_menuSeen = g_frames;
    intro_speed_off();
    hlog("menu: title menu up at frame %u", g_frames);
    if (g_startOnLoad && c == 0) { *(volatile int *)(g_pool + OFF_MENU_CURSOR) = 1; hlog("menu: cursor -> Load"); }
}

static void apply_cheats(void) {
    if ((!g_keepUnits && !g_keepPearls) || g_frames % 30) return;  // twice a second is plenty
    if (!g_pool) { g_pool = find_pool(); if (!g_pool) return; hlog("cheats: game data block at %p", g_pool); }
    int ok = (!g_keepUnits || units_valid()) && (!g_keepPearls || pearls_valid());
    // Wait for ~5 s of sane values before the first write, so nothing is written while the game is still loading.
    if (!ok) { g_cheatOk = 0; return; }
    if (g_cheatOk < 10) {
        if (++g_cheatOk == 10)
            hlog("cheats: counters found (units %d, pearls %d)", *(int *)(g_pool + OFF_UNITS), *(int *)(g_pool + OFF_PEARLS));
        return;
    }
    int *u = (int *)(g_pool + OFF_UNITS), *pe = (int *)(g_pool + OFF_PEARLS);
    if (g_keepUnits && units_valid() && *u < g_keepUnits) *u = g_keepUnits;
    if (g_keepPearls && pearls_valid() && *pe < g_keepPearls) *pe = g_keepPearls;
}

// ---- Engine-side widescreen -------------------------------------------------------------------------
// The SetTransform fix alone only widens the final picture: the engine still sizes its camera-relative
// water patch and its visibility checks from the camera's own 4:3 view, so the extra width showed the
// edge of the water and empty space. So the camera angle the game copies into its display block every
// frame (0xB611F4) is widened for the real aspect: tan(h'/2) = tan(h/2) * aspect / (4/3). The engine
// then draws and culls the full width. Its screen ratio stays 4:3 (changing it rescales the HUD), so
// h_SetTransform restores the original vertical view for that camera's projection.
// BGE.exe always loads at 0x400000; the patch is only applied if the original bytes are found.
#define FOV_COPY_SITE 0x00B611F4u
static const BYTE FOV_COPY_ORIG[12] = { 0x8B, 0x91, 0xAC, 0x00, 0x00, 0x00, 0x89, 0x90, 0xDC, 0x00, 0x00, 0x00 };

float __cdecl bge_wide_fov(float f) {
    if (g_fovK <= 1.0f || !(f > 0.05f && f < 3.0f)) return f;
    // Menu scenes (the inventory uses exactly 1.0) place their 3D icons with the camera angle itself, so widening it
    // pulls them off their slots. They have no water/culling problem: leave them to the projection-only fix.
    if (f <= 1.001f) return f;
    float w = 2.0f * atanf(tanf(f * 0.5f) * g_fovK);
    if (w > 3.0f) w = 3.0f;
    float c = 1.0f / tanf(w * 0.5f);
    for (int i = 0; i < WIDE_SLOTS; i++) if (g_wideCot[i] == c) { g_wideFov[i] = f; return w; }
    LONG i = InterlockedIncrement(&g_wideNext) % WIDE_SLOTS;
    g_wideCot[i] = c; g_wideFov[i] = f;
    return w;
}
// Replaces "mov edx,[ecx+0xAC]; mov [eax+0xDC],edx": eax = display block, ecx = active camera.
__asm__(
    ".text\n"
    ".globl _bge_fov_stub\n"
    "_bge_fov_stub:\n"
    "    pushl %eax\n"
    "    pushl %ecx\n"
    "    pushl %edx\n"
    "    pushl 0xAC(%ecx)\n"
    "    call _bge_wide_fov\n"
    "    addl $4, %esp\n"
    "    popl %edx\n"
    "    popl %ecx\n"
    "    popl %eax\n"
    "    fstps 0xDC(%eax)\n"
    "    ret\n");
void bge_fov_stub(void);

static void engine_ws_init(void) {
    if (!g_engineWs) return;
    BYTE *site = (BYTE *)FOV_COPY_SITE;
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(site, &mi, sizeof mi) || mi.State != MEM_COMMIT || memcmp(site, FOV_COPY_ORIG, sizeof FOV_COPY_ORIG)) {
        g_engineWs = 0; hlog("widescreen: camera code not recognised, using the projection-only fix"); return;
    }
    DWORD old;
    if (!VirtualProtect(site, sizeof FOV_COPY_ORIG, PAGE_EXECUTE_READWRITE, &old)) { g_engineWs = 0; return; }
    BYTE code[12]; memset(code, 0x90, sizeof code);
    code[0] = 0xE8; *(INT32 *)(code + 1) = (INT32)((BYTE *)bge_fov_stub - (site + 5));
    memcpy(site, code, sizeof code);
    VirtualProtect(site, sizeof FOV_COPY_ORIG, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof FOV_COPY_ORIG);
    hlog("widescreen: engine camera patched");
}

static void view_shift_load(void) {
    // positive = look higher (horizon moves down the screen)
    int v = ini_int("ViewHeight", 0); if (v < -50) v = -50; if (v > 50) v = 50;
    g_viewShift = -v / 100.0f;
}
static void engine_ws_frame(void) {
    g_fovK = (g_engineWs && g_aspect > 1.34f) ? g_aspect / (4.0f / 3.0f) : 1.0f;
    if (g_viewShift != 0.0f) {
        if (!g_pool && g_frames % 30 == 0) g_pool = find_pool();
        int t = title_menu_cursor() >= 0;
        if (t != g_inTitle) { g_inTitle = t; hlog("view: title menu %s at frame %u", t ? "showing" : "gone", g_frames); }
    }
    // re-read ViewHeight when bgehook.ini changes, so it can be tuned while playing
    if (g_frames % 30 == 0) {
        static FILETIME last; WIN32_FILE_ATTRIBUTE_DATA fa; char p[MAX_PATH];
        snprintf(p, sizeof p, "%s\\bgehook.ini", g_dir);
        if (GetFileAttributesExA(p, GetFileExInfoStandard, &fa) && CompareFileTime(&fa.ftLastWriteTime, &last)) {
            int first = !last.dwLowDateTime && !last.dwHighDateTime;
            last = fa.ftLastWriteTime; view_shift_load();
            if (!first) hlog("view: height %d", ini_int("ViewHeight", 0));
        }
    }
}

static HRESULT STDMETHODCALLTYPE h_Present(IDirect3DDevice9 *d, const RECT *a, const RECT *b, HWND w, const RGNDATA *r) {
    poll_hotkey();
    apply_cheats();
    watch_title_menu();
    engine_ws_frame();
    if (g_speedOn && g_frames >= 20000) intro_speed_off();   // safety net if the title menu is never recognised
    if (!g_speedOn) limit_frame();
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
    subclass_window(g_hwnd);
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
        // BGE predates display scaling. Without this, at 125% or 150% Windows reports a smaller "virtual" screen
        // (2048x1152 for 2560x1440 at 125%) and stretches the game window up, blurring it.
        SetProcessDPIAware();
        load_cfg();
        if (cfg.log) { char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\bgehook.log", g_dir); g_log = fopen(p, "w"); }
        QueryPerformanceFrequency(&g_freq);
        g_timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /*HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
        if (!g_timer) g_timer = CreateWaitableTimerA(NULL, FALSE, NULL);
        timeBeginPeriod(1);
        hlog("bgehook loaded: fps=%d vsync=%d %dx%d win=%d aniso=%d msaa=%d chain='%s'",
             cfg.fps, cfg.vsync, cfg.width, cfg.height, cfg.windowed, cfg.aniso, cfg.msaa, cfg.chain);
        input_init();
        g_startOnLoad = ini_int("StartOnLoad", 0); g_fastIntro = ini_int("FastIntro", 0);
        intro_speed_init();
        g_engineWs = g_ws && ini_int("EngineWidescreen", 1);
        engine_ws_init();
        g_keepUnits = ini_int("KeepUnits", 0); g_keepPearls = ini_int("KeepPearls", 0);
        if (g_keepUnits || g_keepPearls) hlog("cheats: keep units >= %d, pearls >= %d", g_keepUnits, g_keepPearls);
    } else if (r == DLL_PROCESS_DETACH) {
        if (g_log) fclose(g_log);
    }
    return TRUE;
}
