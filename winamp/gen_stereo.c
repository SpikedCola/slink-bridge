/*
 * gen_stereo.dll: Winamp 5.x general purpose plugin.
 *
 * Adds an equalizer-sized skinned window with two buttons that switch the
 * Sony receiver's input through the slink-bridge ESP32:
 *   "Computer"  POST http://<host>/api/send?hex=c05004  (MD/DAT)
 *   "TV"        POST http://<host>/api/send?hex=c05016  (TV)
 * While the window is visible it polls GET /api/state every few seconds and
 * shows which input the receiver is really on.
 *
 * Toggle the window from Winamp's main menu ("Stereo Switch"). The bridge
 * address is set in Preferences > Plug-ins > General Purpose > Configure.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wininet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "resource.h"

/* ---- Winamp SDK bits (from gen.h / wa_ipc.h) ---- */

#define GPPHDR_VER 0x10

typedef struct {
  int version;
  char *description;
  int (*init)(void);
  void (*config)(void);
  void (*quit)(void);
  HWND hwndParent;
  HINSTANCE hDllInstance;
} winampGeneralPurposePlugin;

#define WM_WA_IPC WM_USER
#define IPC_GET_HMENU 281
#define IPC_GET_EMBEDIF 505
#define IPC_GET_GENSKINBITMAP 503
#define IPC_GETINIFILEW 1334
#define IPC_ADJUST_FFWINDOWSMENUPOS 2003

#define EMBED_FLAGS_NORESIZE 0x1
#define EMBED_FLAGS_GUID 0x8

typedef struct {
  HWND me;
  int flags;
  RECT r;
  void *user_ptr;
  int extra_data[64];
} embedWindowState;

typedef HWND (*embedWindowFunc)(embedWindowState *);

#define WINAMP_OPTIONS_EQ 40036 /* "Equalizer" item in the main menu */

/* ---- receiver codes (see ../firmware/README.md) ---- */

#define CODE_MDDAT 0x04
#define CODE_TV 0x16
static const char *const BTN_HEX[2] = {"c05004", "c05016"};
static const char *const BTN_TEXT[2] = {"Computer", "TV"};
static const int BTN_CODE[2] = {CODE_MDDAT, CODE_TV};

/* ---- plugin state ---- */

#define PLUGIN_NAME "Stereo Switch v1.0 (gen_stereo.dll)"
#define INI_SECTION L"gen_stereo"
#define MENU_ID 46317
#define POLL_MS 5000
/* Outer frame size: the classic equalizer size is the maximum, half of it the minimum. */
#define MAX_W 275
#define MAX_H 116
#define MIN_W 138
#define MIN_H 78
/* Classic skin frame borders (gen.bmp): content sits inside these. */
#define INSET_L 11
#define INSET_T 20
#define INSET_R 8
#define INSET_B 14
#define GRIP 20 /* size of the frame's bottom-right resize corner */
#define WM_STEREO_CREATE (WM_APP + 1)  /* posted to Winamp: create the window */
#define WM_STEREO_SENT (WM_APP + 2)    /* worker -> view: wParam = ok */
#define WM_STEREO_STATE (WM_APP + 3)   /* worker -> view: new state in g_state */

/* {7B3E5D21-9C4A-4F0E-B6D2-5A1C8E3F9D47} */
static const GUID STEREO_GUID = {0x7b3e5d21, 0x9c4a, 0x4f0e, {0xb6, 0xd2, 0x5a, 0x1c, 0x8e, 0x3f, 0x9d, 0x47}};

static int init(void);
static void config(void);
static void quit(void);

static winampGeneralPurposePlugin plugin = {GPPHDR_VER, PLUGIN_NAME, init, config, quit, 0, 0};

static HWND hwndWinamp, hFrame, hView;
static HMENU hMainMenu;
static WNDPROC oldWinampProc;
static UINT createMsg;
static embedWindowState ews;
static wchar_t iniPath[MAX_PATH];
static char host[256] = "slink.local";
#define MAX_FONT_PX 24
static HFONT fonts[2][MAX_FONT_PX]; /* [bold][pixel height], created on demand */
static WNDPROC oldFrameProc;

/* resize drag on the frame's corner (classic skin) */
static int sizing, frameDrag, forceW, forceH;
static POINT sizeStartPt;
static SIZE sizeStartWnd;

/* view state */
static int hotBtn = -1, downBtn = -1, pendingBtn = -1;

/* receiver state, written by the worker under cs */
typedef struct {
  int reachable, known, power, audio;
  char inputName[64];
} RecvState;
static RecvState g_state;
static int haveState;

/* worker thread */
static CRITICAL_SECTION cs;
static HANDLE hWorker, hWake, hQuit;
static HINTERNET hInet;
static char jobHex[16];
static int jobPoll;

/* ---- settings ---- */

static void loadSettings(void) {
  wchar_t *p = (wchar_t *)SendMessageW(hwndWinamp, WM_WA_IPC, 0, IPC_GETINIFILEW);
  wchar_t w[256];
  if (p) lstrcpynW(iniPath, p, MAX_PATH);
  GetPrivateProfileStringW(INI_SECTION, L"host", L"slink.local", w, 256, iniPath);
  WideCharToMultiByte(CP_ACP, 0, w, -1, host, sizeof host, NULL, NULL);
}

static void saveHost(void) {
  wchar_t w[256];
  MultiByteToWideChar(CP_ACP, 0, host, -1, w, 256);
  WritePrivateProfileStringW(INI_SECTION, L"host", w, iniPath);
}

static void saveInt(const wchar_t *key, int v) {
  wchar_t buf[16];
  wsprintfW(buf, L"%d", v);
  WritePrivateProfileStringW(INI_SECTION, key, buf, iniPath);
}

static int loadInt(const wchar_t *key, int def) {
  return (int)GetPrivateProfileIntW(INI_SECTION, key, def, iniPath);
}

/* ---- HTTP ---- */

/* "http://slink.local:8080/foo" -> name "slink.local", port 8080 */
static void parseHost(const char *in, char *name, int nameLen, INTERNET_PORT *port) {
  char tmp[256], *s, *e, *colon;
  lstrcpynA(tmp, in, sizeof tmp);
  s = tmp;
  while (*s == ' ' || *s == '\t') s++;
  if (!_strnicmp(s, "http://", 7)) s += 7;
  for (e = s; *e && *e != '/' && *e != ' ' && *e != '\t'; e++) {}
  *e = 0;
  *port = INTERNET_DEFAULT_HTTP_PORT;
  colon = strchr(s, ':');
  if (colon) {
    *colon = 0;
    if (atoi(colon + 1) > 0) *port = (INTERNET_PORT)atoi(colon + 1);
  }
  lstrcpynA(name, s, nameLen);
}

static HINTERNET openInternet(void) {
  DWORD t = 3000;
  HINTERNET h = InternetOpenA("gen_stereo/1.0", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
  if (h) {
    InternetSetOptionA(h, INTERNET_OPTION_CONNECT_TIMEOUT, &t, sizeof t);
    InternetSetOptionA(h, INTERNET_OPTION_SEND_TIMEOUT, &t, sizeof t);
    InternetSetOptionA(h, INTERNET_OPTION_RECEIVE_TIMEOUT, &t, sizeof t);
  }
  return h;
}

/* Returns the HTTP status code, or 0 if the bridge couldn't be reached. */
static int httpRequest(HINTERNET inet, const char *hostSpec, const char *verb, const char *path,
                       char *body, int bodyLen) {
  char name[256];
  INTERNET_PORT port;
  HINTERNET conn, req;
  DWORD status = 0, len = sizeof status, got;
  int total = 0;

  if (body && bodyLen) body[0] = 0;
  if (!inet) return 0;
  parseHost(hostSpec, name, sizeof name, &port);
  if (!name[0]) return 0;

  conn = InternetConnectA(inet, name, port, NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
  if (!conn) return 0;
  req = HttpOpenRequestA(conn, verb, path, NULL, NULL, NULL,
                         INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI |
                             INTERNET_FLAG_NO_COOKIES | INTERNET_FLAG_PRAGMA_NOCACHE,
                         0);
  if (req) {
    if (HttpSendRequestA(req, NULL, 0, NULL, 0)) {
      HttpQueryInfoA(req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &len, NULL);
      if (body && bodyLen > 1) {
        while (total < bodyLen - 1 && InternetReadFile(req, body + total, bodyLen - 1 - total, &got) && got)
          total += got;
        body[total] = 0;
      }
    }
    InternetCloseHandle(req);
  }
  InternetCloseHandle(conn);
  return (int)status;
}

/* Minimal lookups in the bridge's /api/state JSON. */
static int jsonInt(const char *j, const char *key, int def) {
  char pat[64];
  const char *p;
  wsprintfA(pat, "\"%s\":", key);
  p = strstr(j, pat);
  return p ? atoi(p + lstrlenA(pat)) : def;
}

static int jsonBool(const char *j, const char *key) {
  char pat[64];
  const char *p;
  wsprintfA(pat, "\"%s\":", key);
  p = strstr(j, pat);
  return p && !strncmp(p + lstrlenA(pat), "true", 4);
}

static void jsonInputName(const char *j, int code, char *out, int outLen) {
  char pat[32];
  const char *p = strstr(j, "\"names\":{"), *e;
  int n;
  out[0] = 0;
  if (!p) return;
  wsprintfA(pat, "\"%d\":\"", code);
  p = strstr(p, pat);
  if (!p) return;
  p += lstrlenA(pat);
  e = strchr(p, '"');
  if (!e) return;
  n = (int)(e - p);
  if (n > outLen - 1) n = outLen - 1;
  memcpy(out, p, n);
  out[n] = 0;
  while (n > 0 && out[n - 1] == ' ') out[--n] = 0;
}

static void parseState(const char *j, int reachable, RecvState *st) {
  memset(st, 0, sizeof *st);
  st->reachable = reachable;
  st->audio = -1;
  if (!reachable) return;
  st->known = jsonBool(j, "known");
  st->power = jsonBool(j, "power");
  st->audio = jsonInt(j, "audio", -1);
  if (st->audio >= 0) jsonInputName(j, st->audio, st->inputName, sizeof st->inputName);
}

static DWORD WINAPI workerProc(void *arg) {
  HANDLE waits[2] = {hQuit, hWake};
  char hex[16], h[256], path[64];
  static char body[4096];
  int poll;
  (void)arg;

  for (;;) {
    if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0) break;
    for (;;) {
      EnterCriticalSection(&cs);
      lstrcpyA(hex, jobHex);
      jobHex[0] = 0;
      poll = jobPoll;
      jobPoll = 0;
      lstrcpyA(h, host);
      LeaveCriticalSection(&cs);
      if (!hex[0] && !poll) break;

      if (hex[0]) {
        int code;
        wsprintfA(path, "/api/send?hex=%s", hex);
        code = httpRequest(hInet, h, "POST", path, NULL, 0);
        PostMessageW(hView, WM_STEREO_SENT, code == 200, 0);
        /* Give the bridge time to send it and hear the receiver's reply. */
        if (WaitForSingleObject(hQuit, 700) == WAIT_OBJECT_0) return 0;
        poll = 1;
      }
      if (poll) {
        RecvState st;
        int code = httpRequest(hInet, h, "GET", "/api/state", body, sizeof body);
        parseState(body, code == 200, &st);
        EnterCriticalSection(&cs);
        g_state = st;
        LeaveCriticalSection(&cs);
        PostMessageW(hView, WM_STEREO_STATE, 0, 0);
      }
      if (WaitForSingleObject(hQuit, 0) == WAIT_OBJECT_0) return 0;
    }
  }
  return 0;
}

static void queueSend(const char *hex) {
  EnterCriticalSection(&cs);
  lstrcpynA(jobHex, hex, sizeof jobHex);
  LeaveCriticalSection(&cs);
  SetEvent(hWake);
}

static void queuePoll(void) {
  EnterCriticalSection(&cs);
  jobPoll = 1;
  LeaveCriticalSection(&cs);
  SetEvent(hWake);
}

/* ---- drawing ---- */

/* Media-library style colours, read from the skin's genex.bmp like wa_dlg.h does. */
enum { C_ITEMBG, C_ITEMFG, C_WNDBG, C_BUTTONFG, C_WNDFG, C_HILITE, C_SELCOLOR, C_COUNT };

/* Winamp loads a new copy of genex.bmp for every IPC_GET_GENSKINBITMAP and the
   caller owns it. We keep one copy per skin and delete it ourselves; asking on
   every paint would use up a GDI handle each time until Winamp runs out. */
static HBITMAP skinBmp;
static COLORREF skinCol[C_COUNT];

static void freeSkin(void) {
  if (skinBmp) DeleteObject(skinBmp);
  skinBmp = NULL;
}

/* Fetches genex.bmp and its colours unless we already have them. Without a
   bitmap the colours are the defaults, and the next call tries again. */
static void loadSkin(void) {
  static const COLORREF defaults[C_COUNT] = {RGB(0, 0, 0),     RGB(0, 255, 0),   RGB(36, 36, 60), RGB(255, 255, 255),
                                             RGB(198, 198, 222), RGB(0, 0, 198), RGB(0, 128, 255)};
  HDC dc;
  HGDIOBJ old;
  int i;
  if (skinBmp) return;
  memcpy(skinCol, defaults, sizeof defaults);
  skinBmp = (HBITMAP)SendMessageW(hwndWinamp, WM_WA_IPC, 0, IPC_GET_GENSKINBITMAP);
  if (!skinBmp) return;
  dc = CreateCompatibleDC(NULL);
  old = dc ? SelectObject(dc, skinBmp) : NULL;
  if (old) {
    for (i = 0; i < C_COUNT; i++) {
      COLORREF c = GetPixel(dc, 48 + i * 2, 0);
      if (c != CLR_INVALID) skinCol[i] = c;
    }
    SelectObject(dc, old);
  } else {
    freeSkin(); /* can't draw from it */
  }
  if (dc) DeleteDC(dc);
}

/* Draws a button face from genex.bmp (47x15 at y=0 normal, y=15 pressed) as a 9-slice. */
static void drawFace(HDC hdc, HDC bmdc, const RECT *r, int pressed) {
  int y = pressed ? 15 : 0, w = r->right - r->left, h = r->bottom - r->top;
  int L = r->left, T = r->top, R = r->right, B = r->bottom;
  SetStretchBltMode(hdc, COLORONCOLOR);
  BitBlt(hdc, L, T, 4, 4, bmdc, 0, y, SRCCOPY);
  StretchBlt(hdc, L + 4, T, w - 8, 4, bmdc, 4, y, 39, 4, SRCCOPY);
  BitBlt(hdc, R - 4, T, 4, 4, bmdc, 43, y, SRCCOPY);
  StretchBlt(hdc, L, T + 4, 4, h - 8, bmdc, 0, y + 4, 4, 7, SRCCOPY);
  StretchBlt(hdc, L + 4, T + 4, w - 8, h - 8, bmdc, 4, y + 4, 39, 7, SRCCOPY);
  StretchBlt(hdc, R - 4, T + 4, 4, h - 8, bmdc, 43, y + 4, 4, 7, SRCCOPY);
  BitBlt(hdc, L, B - 4, 4, 4, bmdc, 0, y + 11, SRCCOPY);
  StretchBlt(hdc, L + 4, B - 4, w - 8, 4, bmdc, 4, y + 11, 39, 4, SRCCOPY);
  BitBlt(hdc, R - 4, B - 4, 4, 4, bmdc, 43, y + 11, SRCCOPY);
}

static void fillRect(HDC hdc, const RECT *r, COLORREF c) {
  SetBkColor(hdc, c);
  ExtTextOutA(hdc, 0, 0, ETO_OPAQUE, r, NULL, 0, NULL);
}

static HFONT getFont(int px, int bold) {
  if (px < 8) px = 8;
  if (px > MAX_FONT_PX - 1) px = MAX_FONT_PX - 1;
  if (!fonts[bold][px])
    fonts[bold][px] = CreateFontW(-px, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                  CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Tahoma");
  return fonts[bold][px];
}

static int isClassicFrame(void) {
  return hFrame && IsWindow(hFrame) && GetAncestor(hFrame, GA_PARENT) == GetDesktopWindow();
}

typedef struct {
  RECT btn[2], status;
  int showStatus, ledH, fontPx, btnFontPx;
} Layout;

/* Full size content area is 256x82; things drop out as it shrinks toward 119x24. */
/* hasStatus: there is status text; otherwise the buttons take the full height. */
static void layout(const RECT *client, int hasStatus, Layout *L) {
  int w = client->right, h = client->bottom;
  int m = h >= 60 ? 5 : 2, gap = w >= 200 ? 5 : 3;
  int statusH = hasStatus && h >= 56 ? 16 : 0, bh;

  memset(L, 0, sizeof *L);
  L->showStatus = statusH > 0;
  L->fontPx = 11;    /* status text */
  L->btnFontPx = 17; /* button labels, 50% larger; fixed at every window size */

  bh = h - 2 * m - statusH;
  if (bh < 8) bh = 8;
  SetRect(&L->btn[0], m, m, m + (w - 2 * m - gap) / 2, m + bh);
  SetRect(&L->btn[1], L->btn[0].right + gap, m, w - m, m + bh);
  L->ledH = bh >= 36 ? 4 : (bh >= 14 ? 2 : 0);
  SetRect(&L->status, m, m + bh, w - m, h);
  if (!L->showStatus) SetRectEmpty(&L->status);
}

static int activeButton(const RecvState *st) {
  int i;
  if (pendingBtn >= 0) return pendingBtn;
  if (!haveState || !st->reachable || !st->known || !st->power) return -1;
  for (i = 0; i < 2; i++)
    if (st->audio == BTN_CODE[i]) return i;
  return -1;
}

static void statusText(const RecvState *s, char *out, int len) {
  RecvState st = *s;
  char name[128];
  if (!haveState) {
    _snprintf(out, len, "Connecting to %s...", host);
  } else if (!st.reachable) {
    _snprintf(out, len, "Can't reach %s", host);
  } else if (!st.known) {
    _snprintf(out, len, "Waiting for the receiver...");
  } else if (!st.power) {
    _snprintf(out, len, "Receiver is off");
  } else {
    /* Computer/TV are shown by the button lights; name any other input. */
    if (st.audio == CODE_MDDAT || st.audio == CODE_TV) name[0] = 0;
    else if (st.inputName[0]) lstrcpynA(name, st.inputName, sizeof name);
    else wsprintfA(name, "Input 0x%02x", st.audio);
    _snprintf(out, len, "%s", name);
  }
  out[len - 1] = 0;
}

/* Snapshot of the worker's state plus the status text for it; returns 1 if there is text. */
static int currentStatus(RecvState *st, char *out, int len) {
  EnterCriticalSection(&cs);
  *st = g_state;
  LeaveCriticalSection(&cs);
  statusText(st, out, len);
  return out[0] != 0;
}

static void paintView(HWND hwnd, HDC hdc) {
  RECT rc;
  Layout L;
  HDC mem = CreateCompatibleDC(hdc), skin = CreateCompatibleDC(hdc);
  HBITMAP bmp, oldBmp, genex;
  HGDIOBJ oldSkin;
  const COLORREF *col = skinCol;
  char text[160];
  RecvState st;
  int i, active, hasStatus;

  hasStatus = currentStatus(&st, text, sizeof text);
  active = activeButton(&st);

  GetClientRect(hwnd, &rc);
  bmp = CreateCompatibleBitmap(hdc, rc.right > 0 ? rc.right : 1, rc.bottom > 0 ? rc.bottom : 1);
  oldBmp = SelectObject(mem, bmp);
  loadSkin();
  genex = skinBmp;
  oldSkin = genex ? SelectObject(skin, genex) : NULL;
  if (!oldSkin) genex = NULL; /* no skin bitmap: flat buttons in the default colours */
  layout(&rc, hasStatus, &L);

  fillRect(mem, &rc, col[C_WNDBG]);
  SetBkMode(mem, TRANSPARENT);

  /* Until the bridge has answered once, show only "Connecting to ...". */
  if (!haveState) {
    RECT tr = rc;
    InflateRect(&tr, -4, 0);
    SelectObject(mem, getFont(L.fontPx, 0));
    SetTextColor(mem, col[C_WNDFG]);
    DrawTextA(mem, text, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  }

  for (i = 0; haveState && i < 2; i++) {
    RECT r = L.btn[i], led, tr;
    const char *label = BTN_TEXT[i];
    int pressed = (i == active) || (i == downBtn && i == hotBtn);
    if (genex) {
      drawFace(mem, skin, &r, pressed);
    } else {
      fillRect(mem, &r, pressed ? col[C_SELCOLOR] : col[C_ITEMBG]);
      FrameRect(mem, &r, (HBRUSH)GetStockObject(GRAY_BRUSH));
    }
    tr = r;
    /* status light above the label: lit on the receiver's current input */
    if (L.ledH) {
      int half = L.ledH >= 4 ? 9 : 6, top = r.top + (L.ledH >= 4 ? 8 : 3);
      SetRect(&led, (r.left + r.right) / 2 - half, top, (r.left + r.right) / 2 + half, top + L.ledH);
      if (pressed) OffsetRect(&led, 1, 1);
      fillRect(mem, &led, i == active ? col[C_ITEMFG] : col[C_ITEMBG]);
      if (L.ledH >= 4) {
        HBRUSH b = CreateSolidBrush(col[C_BUTTONFG]);
        FrameRect(mem, &led, b);
        DeleteObject(b);
      }
      tr.top = led.bottom;
    }
    if (pressed) OffsetRect(&tr, 1, 1);
    SelectObject(mem, getFont(L.btnFontPx, 1));
    SetTextColor(mem, col[C_BUTTONFG]);
    InflateRect(&tr, -3, 0);
    DrawTextA(mem, label, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  }

  if (haveState && L.showStatus) {
    SelectObject(mem, getFont(L.fontPx, 0));
    SetTextColor(mem, col[C_WNDFG]);
    DrawTextA(mem, text, -1, &L.status, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  }

  BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
  if (oldSkin) SelectObject(skin, oldSkin); /* genex.bmp stays ours, for the next paint */
  SelectObject(mem, oldBmp);
  DeleteObject(bmp);
  DeleteDC(skin);
  DeleteDC(mem);
}

/* Returns 0/1 for a button, -1 for nothing. */
static int hitTest(HWND hwnd, LPARAM lp) {
  RECT rc;
  Layout L;
  POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
  RecvState st;
  char text[160];
  int i;
  GetClientRect(hwnd, &rc);
  layout(&rc, currentStatus(&st, text, sizeof text), &L);
  for (i = 0; haveState && i < 2; i++)
    if (PtInRect(&L.btn[i], pt)) return i;
  return -1;
}

static int clampInt(int v, int lo, int hi) {
  return v < lo ? lo : v > hi ? hi : v;
}

static void resizeFrame(int w, int h) {
  forceW = clampInt(w, MIN_W, MAX_W);
  forceH = clampInt(h, MIN_H, MAX_H);
  sizing = 1;
  SetWindowPos(hFrame, NULL, 0, 0, forceW, forceH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  sizing = 0;
}

/* Classic skin only: keep the content filling the frame, whatever the frame does itself. */
static void fitViewToFrame(void) {
  RECT fr;
  if (!hView || !isClassicFrame()) return;
  GetClientRect(hFrame, &fr);
  SetWindowPos(hView, NULL, INSET_L, INSET_T, fr.right - INSET_L - INSET_R, fr.bottom - INSET_T - INSET_B,
               SWP_NOZORDER | SWP_NOACTIVATE);
}

/* Classic skin: the frame is a top-level window, remember where it is and how
   big. Modern skins remember this themselves (by GUID). */
static void saveGeometry(void) {
  RECT r;
  if (!isClassicFrame() || !GetWindowRect(hFrame, &r)) return;
  saveInt(L"x", r.left);
  saveInt(L"y", r.top);
  saveInt(L"w", r.right - r.left);
  saveInt(L"h", r.bottom - r.top);
}

/* Point in frame client coordinates is on the skin's bottom-right resize corner. */
static int inResizeCorner(HWND hwnd, POINT pt) {
  RECT rc;
  GetClientRect(hwnd, &rc);
  return pt.x >= rc.right - GRIP && pt.y >= rc.bottom - GRIP && pt.x < rc.right && pt.y < rc.bottom;
}

/* Subclass of the embed frame (classic skin). Classic frames resize in 25x29
   steps and never below 275x116, which is our maximum, so the frame's own
   resize corner can't change the size at all. We take over drags on that
   corner, resize freely between MIN and MAX, and override the frame's own
   size choices to stay in that range. */
static LRESULT CALLBACK frameProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  LRESULT r;
  if (msg == WM_DISPLAYCHANGE) { /* Winamp posts this to every embed frame after a skin change */
    freeSkin();
    if (hView) InvalidateRect(hView, NULL, FALSE);
  }
  if (!isClassicFrame()) return CallWindowProcW(oldFrameProc, hwnd, msg, wp, lp);
  switch (msg) {
    case WM_NCHITTEST: {
      POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
      ScreenToClient(hwnd, &pt);
      if (inResizeCorner(hwnd, pt)) return HTCLIENT; /* so the click comes to us below */
      break;
    }
    case WM_SETCURSOR:
      if ((HWND)wp == hwnd && LOWORD(lp) == HTCLIENT) {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        if (frameDrag || inResizeCorner(hwnd, pt)) {
          SetCursor(LoadCursor(NULL, IDC_SIZENWSE));
          return TRUE;
        }
      }
      break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
      POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
      if (inResizeCorner(hwnd, pt)) {
        RECT fr;
        GetWindowRect(hwnd, &fr);
        GetCursorPos(&sizeStartPt);
        sizeStartWnd.cx = fr.right - fr.left;
        sizeStartWnd.cy = fr.bottom - fr.top;
        frameDrag = 1;
        SetCapture(hwnd);
        SetCursor(LoadCursor(NULL, IDC_SIZENWSE));
        return 0;
      }
      break;
    }
    case WM_MOUSEMOVE:
      if (frameDrag) {
        POINT pt;
        GetCursorPos(&pt);
        resizeFrame(sizeStartWnd.cx + pt.x - sizeStartPt.x, sizeStartWnd.cy + pt.y - sizeStartPt.y);
        return 0;
      }
      break;
    case WM_LBUTTONUP:
      if (frameDrag) {
        frameDrag = 0;
        ReleaseCapture();
        return 0;
      }
      break;
    case WM_CAPTURECHANGED:
      if (frameDrag) {
        frameDrag = 0;
        return 0;
      }
      break;
    case WM_GETMINMAXINFO: {
      MINMAXINFO *mi = (MINMAXINFO *)lp;
      r = CallWindowProcW(oldFrameProc, hwnd, msg, wp, lp);
      mi->ptMinTrackSize.x = MIN_W;
      mi->ptMinTrackSize.y = MIN_H;
      mi->ptMaxTrackSize.x = MAX_W;
      mi->ptMaxTrackSize.y = MAX_H;
      return r;
    }
    case WM_WINDOWPOSCHANGING: {
      WINDOWPOS *pos = (WINDOWPOS *)lp;
      r = CallWindowProcW(oldFrameProc, hwnd, msg, wp, lp);
      if (!(pos->flags & SWP_NOSIZE)) {
        if (sizing) {
          pos->cx = forceW;
          pos->cy = forceH;
        }
        pos->cx = clampInt(pos->cx, MIN_W, MAX_W);
        pos->cy = clampInt(pos->cy, MIN_H, MAX_H);
      }
      return r;
    }
    case WM_SIZE:
      r = CallWindowProcW(oldFrameProc, hwnd, msg, wp, lp);
      fitViewToFrame();
      InvalidateRect(hwnd, NULL, TRUE); /* frame skin pieces move with the size */
      return r;
    case WM_DESTROY:
      /* On a normal exit Winamp destroys its windows, this one included, before
         it calls quit(), so this is the last chance to see the size. */
      saveGeometry();
      break;
  }
  return CallWindowProcW(oldFrameProc, hwnd, msg, wp, lp);
}

static LRESULT CALLBACK viewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE:
      SetTimer(hwnd, 1, POLL_MS, NULL);
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hdc = BeginPaint(hwnd, &ps);
      paintView(hwnd, hdc);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_DISPLAYCHANGE: /* Winamp sends this on skin change: reload genex.bmp */
      freeSkin();
      InvalidateRect(hwnd, NULL, FALSE);
      return 0;
    case WM_SIZE:
      InvalidateRect(hwnd, NULL, FALSE);
      return 0;
    case WM_TIMER:
      if (IsWindowVisible(hwnd)) queuePoll();
      return 0;
    case WM_MOUSEMOVE: {
      int h = hitTest(hwnd, lp);
      if (h != hotBtn) {
        hotBtn = h;
        InvalidateRect(hwnd, NULL, FALSE);
      }
      return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
      downBtn = hotBtn = hitTest(hwnd, lp);
      if (downBtn >= 0) SetCapture(hwnd);
      InvalidateRect(hwnd, NULL, FALSE);
      return 0;
    case WM_LBUTTONUP: {
      int h = hitTest(hwnd, lp), down = downBtn;
      downBtn = -1;
      if (down >= 0) {
        ReleaseCapture();
        if (h == down) {
          pendingBtn = h;
          queueSend(BTN_HEX[h]);
        }
      }
      InvalidateRect(hwnd, NULL, FALSE);
      return 0;
    }
    case WM_CAPTURECHANGED:
      downBtn = -1;
      InvalidateRect(hwnd, NULL, FALSE);
      return 0;
    case WM_STEREO_SENT:
      if (!wp) { /* request failed: drop the optimistic highlight now */
        pendingBtn = -1;
        EnterCriticalSection(&cs);
        g_state.reachable = 0;
        LeaveCriticalSection(&cs);
        haveState = 1;
      }
      InvalidateRect(hwnd, NULL, FALSE);
      return 0;
    case WM_STEREO_STATE:
      pendingBtn = -1;
      haveState = 1;
      InvalidateRect(hwnd, NULL, FALSE);
      return 0;
    case WM_DESTROY:
      KillTimer(hwnd, 1);
      hView = NULL;
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- window / menu management ---- */

static int isShown(void) {
  return hFrame && IsWindow(hFrame) && IsWindowVisible(hFrame);
}

static void showWindow(int show) {
  if (!hFrame || !IsWindow(hFrame)) return;
  ShowWindow(hFrame, show ? SW_SHOWNA : SW_HIDE);
  if (show) queuePoll();
  if (hMainMenu) CheckMenuItem(hMainMenu, MENU_ID, MF_BYCOMMAND | (show ? MF_CHECKED : MF_UNCHECKED));
  saveInt(L"visible", show);
}

static void createWindow(void) {
  WNDCLASSW wc = {0};
  embedWindowFunc embed = (embedWindowFunc)SendMessageW(hwndWinamp, WM_WA_IPC, 0, IPC_GET_EMBEDIF);
  RECT m;
  int w, h;
  if (!embed || hFrame) return;

  wc.style = CS_DBLCLKS;
  wc.lpfnWndProc = viewProc;
  wc.hInstance = plugin.hDllInstance;
  wc.hCursor = LoadCursor(NULL, IDC_ARROW);
  wc.lpszClassName = L"gen_stereo_view";
  RegisterClassW(&wc);

  /* Equalizer size by default (also the maximum); default spot is below the main window. */
  GetWindowRect(hwndWinamp, &m);
  memset(&ews, 0, sizeof ews);
  ews.flags = EMBED_FLAGS_GUID;
  memcpy(&ews.extra_data[4], &STEREO_GUID, sizeof STEREO_GUID); /* SET_EMBED_GUID */
  ews.r.left = loadInt(L"x", m.left);
  ews.r.top = loadInt(L"y", m.bottom);
  w = clampInt(loadInt(L"w", MAX_W), MIN_W, MAX_W);
  h = clampInt(loadInt(L"h", MAX_H), MIN_H, MAX_H);
  ews.r.right = ews.r.left + w;
  ews.r.bottom = ews.r.top + h;

  hFrame = embed(&ews);
  if (!hFrame) return;
  SetWindowTextW(hFrame, L"Stereo Switch");

  hView = CreateWindowExW(0, L"gen_stereo_view", L"", WS_CHILD | WS_VISIBLE, INSET_L, INSET_T,
                          MAX_W - INSET_L - INSET_R, MAX_H - INSET_T - INSET_B, hFrame, NULL, plugin.hDllInstance, NULL);
  oldFrameProc = (WNDPROC)SetWindowLongPtrW(hFrame, GWLP_WNDPROC, (LONG_PTR)frameProc);
  if (isClassicFrame()) {
    /* Winamp creates classic frames no smaller than 275x116 whatever we asked
       for, so put the saved size back now that frameProc can enforce it. */
    resizeFrame(w, h);
    fitViewToFrame();
  }
  if (loadInt(L"visible", 1)) showWindow(1);
}

static void addMenuItem(void) {
  MENUITEMINFOW mii;
  int i, n, pos = -1;
  memset(&mii, 0, sizeof mii);
  mii.cbSize = sizeof mii;
  hMainMenu = (HMENU)SendMessageW(hwndWinamp, WM_WA_IPC, 0, IPC_GET_HMENU);
  if (!hMainMenu) return;
  n = GetMenuItemCount(hMainMenu);
  for (i = 0; i < n; i++)
    if (GetMenuItemID(hMainMenu, i) == WINAMP_OPTIONS_EQ) pos = i + 1;
  if (pos < 0) pos = n > 2 ? n - 2 : n;
  mii.fMask = MIIM_ID | MIIM_STRING | MIIM_STATE;
  mii.wID = MENU_ID;
  mii.dwTypeData = L"Stereo Switch";
  mii.fState = MFS_UNCHECKED;
  InsertMenuItemW(hMainMenu, pos, TRUE, &mii);
  SendMessageW(hwndWinamp, WM_WA_IPC, 1, IPC_ADJUST_FFWINDOWSMENUPOS);
}

static LRESULT CALLBACK winampProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == createMsg) {
    createWindow();
    return 0;
  }
  if ((msg == WM_COMMAND || msg == WM_SYSCOMMAND) && LOWORD(wp) == MENU_ID) {
    showWindow(!isShown());
    return 0;
  }
  if (msg == WM_INITMENUPOPUP && hMainMenu)
    CheckMenuItem(hMainMenu, MENU_ID, MF_BYCOMMAND | (isShown() ? MF_CHECKED : MF_UNCHECKED));
  return CallWindowProcW(oldWinampProc, hwnd, msg, wp, lp);
}

/* ---- config dialog ---- */

static INT_PTR CALLBACK configProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  (void)lp;
  switch (msg) {
    case WM_INITDIALOG:
      SetDlgItemTextA(dlg, IDC_HOST, host);
      SendDlgItemMessageA(dlg, IDC_HOST, EM_LIMITTEXT, sizeof host - 1, 0);
      return TRUE;
    case WM_COMMAND:
      switch (LOWORD(wp)) {
        case IDC_TEST: {
          char h[256], body[4096], msgText[128];
          HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
          HINTERNET inet = openInternet();
          int code;
          GetDlgItemTextA(dlg, IDC_HOST, h, sizeof h);
          SetDlgItemTextA(dlg, IDC_RESULT, "Testing...");
          UpdateWindow(GetDlgItem(dlg, IDC_RESULT));
          code = httpRequest(inet, h, "GET", "/api/state", body, sizeof body);
          if (inet) InternetCloseHandle(inet);
          SetCursor(old);
          if (code == 200 && strstr(body, "\"audio\":")) lstrcpyA(msgText, "OK, bridge found");
          else if (code) wsprintfA(msgText, "HTTP %d: not a bridge?", code);
          else lstrcpyA(msgText, "No response");
          SetDlgItemTextA(dlg, IDC_RESULT, msgText);
          return TRUE;
        }
        case IDOK: {
          char h[256];
          GetDlgItemTextA(dlg, IDC_HOST, h, sizeof h);
          EnterCriticalSection(&cs);
          lstrcpyA(host, h[0] ? h : "slink.local");
          LeaveCriticalSection(&cs);
          saveHost();
          haveState = 0;
          if (isShown()) queuePoll();
          if (hView) InvalidateRect(hView, NULL, FALSE);
          EndDialog(dlg, IDOK);
          return TRUE;
        }
        case IDCANCEL:
          EndDialog(dlg, IDCANCEL);
          return TRUE;
      }
      break;
  }
  return FALSE;
}

static void config(void) {
  DialogBoxParamW(plugin.hDllInstance, MAKEINTRESOURCEW(IDD_CONFIG), plugin.hwndParent, configProc, 0);
}

/* ---- plugin entry points ---- */

static int init(void) {
  hwndWinamp = plugin.hwndParent;
  InitializeCriticalSection(&cs);
  loadSettings();

  hInet = openInternet();
  hWake = CreateEventW(NULL, FALSE, FALSE, NULL);
  hQuit = CreateEventW(NULL, TRUE, FALSE, NULL);
  hWorker = CreateThread(NULL, 0, workerProc, NULL, 0, NULL);

  addMenuItem();
  createMsg = RegisterWindowMessageW(L"gen_stereo_create");
  oldWinampProc = (WNDPROC)SetWindowLongPtrW(hwndWinamp, GWLP_WNDPROC, (LONG_PTR)winampProc);
  /* Create the window once Winamp (and a modern skin, if any) has finished starting. */
  PostMessageW(hwndWinamp, createMsg, 0, 0);
  return 0;
}

static void quit(void) {
  /* Only when Winamp unloads us with its windows still up (Windows shutting
     down, plugin removed); after a normal exit the frame is already gone and
     frameProc has saved its geometry. */
  if (hFrame && IsWindow(hFrame)) {
    saveInt(L"visible", IsWindowVisible(hFrame) ? 1 : 0);
    saveGeometry();
    if ((WNDPROC)GetWindowLongPtrW(hFrame, GWLP_WNDPROC) == frameProc)
      SetWindowLongPtrW(hFrame, GWLP_WNDPROC, (LONG_PTR)oldFrameProc);
    DestroyWindow(hFrame);
  }
  hFrame = hView = NULL;

  if ((WNDPROC)GetWindowLongPtrW(hwndWinamp, GWLP_WNDPROC) == winampProc)
    SetWindowLongPtrW(hwndWinamp, GWLP_WNDPROC, (LONG_PTR)oldWinampProc);
  if (hMainMenu) DeleteMenu(hMainMenu, MENU_ID, MF_BYCOMMAND);

  /* Stop the worker; closing the WinINet handle aborts a request in flight. */
  SetEvent(hQuit);
  if (hInet) InternetCloseHandle(hInet);
  hInet = NULL;
  if (hWorker) {
    WaitForSingleObject(hWorker, 8000);
    CloseHandle(hWorker);
  }
  CloseHandle(hWake);
  CloseHandle(hQuit);
  {
    int b, px;
    for (b = 0; b < 2; b++)
      for (px = 0; px < MAX_FONT_PX; px++)
        if (fonts[b][px]) DeleteObject(fonts[b][px]);
  }
  freeSkin();
  DeleteCriticalSection(&cs);
}

__declspec(dllexport) winampGeneralPurposePlugin *winampGetGeneralPurposePlugin(void) {
  return &plugin;
}