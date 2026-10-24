/* atkbat.c — ATK A9 Pro Max 电量托盘（原生 Win32，无运行时依赖）
 *
 * 仓库: https://github.com/Lkig/ATK-A9-Battery
 * 许可: MIT
 *
 * 协议: HID usage page 0xFF05 / usage 0x01
 *       64 字节报告，报告 ID 0x08，命令 0x72，子索引 0x07
 *       请求: 08 7D 72 02 00 01 07 01
 *       应答: byte[1]=0x72 byte[5]=0x07 byte[2]=0x00 → byte[7]=百分比, byte[8..9]=电芯电压 mV(小端)
 *             byte[2]!=0x00 → 鼠标休眠（接收器在但设备无响应）
 *
 * 编译: gcc -O2 -s -municode -mwindows -o ATKA9Battery.exe src\atkbat.c ^
 *           -lhid -lsetupapi -lshell32 -luser32 -lgdi32 -ladvapi32
 */
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shellapi.h>
#include <dbt.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <stdio.h>
#include <stdlib.h>

#define APP_NAME      L"ATK A9 Battery"
#define WND_CLASS     L"ATKA9BatteryWnd"
#define TRAY_ID       1
#define WM_TRAY       (WM_APP + 1)
#define TIMER_POLL    1
#define TIMER_KICK    2   /* 设备插拔后的一次性刷新 */

#define VID_MATCH  0x373B
/* 以前按 PID 硬匹配接收器（0x11DC），但插上数据线后鼠标本体是另一个 PID（如 0x1264），
 * 且不同批次的接收器 PID 也会变。所以改成只按厂商 + 厂商自定义 usage page 匹配，
 * 再用应答内容判断读到的是不是真电量。 */
#define USAGE_PAGE_MATCH 0xFF05
#define USAGE_MATCH      0x0001

#ifndef NOTIFYICON_VERSION_4
#define NOTIFYICON_VERSION_4 4
#endif

#define MENU_REFRESH  1001
#define MENU_30S      1002
#define MENU_60S      1003
#define MENU_5MIN     1004
#define MENU_15MIN    1006
#define MENU_STARTUP  1005
#define MENU_EXIT     1009

static HICON      g_icon = NULL;
static HWND       g_hwnd = NULL;
static UINT       g_taskbarCreated = 0;
static int        g_percent = -1;
static wchar_t    g_tip[160] = L"ATK A9 Pro Max";

typedef struct {
    int ok;        /* 读到有效电量 */
    int percent;
    int mv;
    int asleep;    /* 设备在，但没应答（休眠 / 接收器后面没鼠标） */
    int wired;     /* 1 = 数据线直连读到的（link 0），0 = 经 2.4G 接收器转发（link 1） */
    int seen;      /* 至少有一个 ATK 设备应答过，用来区分"没设备"和"设备休眠" */
} BatState;

/* ---------- HID ---------- */

static BOOL ovl_write(HANDLE h, const BYTE *buf, DWORD len) {
    OVERLAPPED ov; DWORD w = 0; BOOL ok;
    ZeroMemory(&ov, sizeof ov); ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    ok = WriteFile(h, buf, len, &w, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, 500) == WAIT_OBJECT_0)
            ok = GetOverlappedResult(h, &ov, &w, FALSE);
        else { CancelIo(h); ok = FALSE; }
    }
    CloseHandle(ov.hEvent);
    return ok;
}

static DWORD ovl_read(HANDLE h, BYTE *buf, DWORD len, DWORD timeout_ms) {
    OVERLAPPED ov; DWORD r = 0; DWORD got = 0;
    ZeroMemory(&ov, sizeof ov); ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (ReadFile(h, buf, len, &r, &ov)) got = r;
    else if (GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, timeout_ms) == WAIT_OBJECT_0 &&
            GetOverlappedResult(h, &ov, &r, FALSE)) got = r;
        else CancelIo(h);
    }
    CloseHandle(ov.hEvent);
    return got;
}

static BatState query_battery(void) {
    BatState st; GUID guid; HDEVINFO devs; DWORD i;
    ZeroMemory(&st, sizeof st);

    HidD_GetHidGuid(&guid);
    devs = SetupDiGetClassDevsW(&guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devs == INVALID_HANDLE_VALUE) return st;

    for (i = 0;; i++) {
        SP_DEVICE_INTERFACE_DATA did; DWORD need = 0;
        PSP_DEVICE_INTERFACE_DETAIL_DATA_W det;
        HANDLE h; HIDD_ATTRIBUTES att; PHIDP_PREPARSED_DATA pp = NULL; HIDP_CAPS caps;
        BOOL matched = FALSE;

        ZeroMemory(&did, sizeof did); did.cbSize = sizeof did;
        if (!SetupDiEnumDeviceInterfaces(devs, NULL, &guid, i, &did)) break;
        SetupDiGetDeviceInterfaceDetailW(devs, &did, NULL, 0, &need, NULL);
        if (!need) continue;

        det = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, need);
        if (!det) continue;
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(devs, &did, det, need, NULL, NULL)) {
            HeapFree(GetProcessHeap(), 0, det); continue;
        }

        h = CreateFileW(det->DevicePath, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                        FILE_FLAG_OVERLAPPED, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            ZeroMemory(&att, sizeof att); att.Size = sizeof att;
            ZeroMemory(&caps, sizeof caps);
            if (HidD_GetAttributes(h, &att) && att.VendorID == VID_MATCH &&
                HidD_GetPreparsedData(h, &pp)) {
                if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS &&
                    caps.UsagePage == USAGE_PAGE_MATCH && caps.Usage == USAGE_MATCH) {
                    matched = TRUE;
                }
                HidD_FreePreparsedData(pp);
            }

            if (matched) {
                DWORD olen = caps.OutputReportByteLength, ilen = caps.InputReportByteLength;
                BYTE *out, *in;
                int li;
                if (olen < 9) olen = 9;
                if (ilen < 10) ilen = 10;
                out = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, olen);
                in  = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, ilen);
                if (out && in) {
                    out[0] = 0x08; out[1] = 0x7D; out[2] = 0x72; out[3] = 0x02;
                    out[6] = 0x07; out[7] = 0x01;

                    /* out[5] 是链路：1 = 鼠标挂在接收器后面，0 = 数据线直连。
                     * 先试直连（有线时这条才通），再试接收器转发。 */
                    for (li = 0; li <= 1; li++) {
                        int handler = -1, pct = 0, mv = 0, tries;
                        out[5] = (BYTE)li;
                        if (!ovl_write(h, out, olen)) continue;
                        Sleep(80);
                        for (tries = 0; tries < 5; tries++) {
                            DWORD n = ovl_read(h, in, ilen, 150);
                            if (!n) break;
                            if (n >= 10 && in[1] == 0x72 && in[5] == 0x07) {
                                handler = in[2];
                                pct = in[7];
                                mv = in[8] | (in[9] << 8);
                                break;
                            }
                        }
                        if (handler < 0) continue;      /* 这条链路没人应答 */
                        st.seen = 1;
                        /* handler == 0 才是鼠标本人应答。接收器在、鼠标不在时
                         * 会回 0% / 0 mV 的占位数据，得排掉，否则充电时会显示 0%。 */
                        if (handler == 0 && (mv >= 2000 || pct > 0)) {
                            st.ok = 1; st.percent = pct; st.mv = mv; st.wired = (li == 0);
                            break;
                        }
                    }
                }
                if (out) HeapFree(GetProcessHeap(), 0, out);
                if (in) HeapFree(GetProcessHeap(), 0, in);
            }
            CloseHandle(h);
        }
        HeapFree(GetProcessHeap(), 0, det);
        if (st.ok) break;
    }
    if (!st.ok && st.seen) st.asleep = 1;   /* 有设备应答但没给数：休眠 */
    SetupDiDestroyDeviceInfoList(devs);
    return st;
}

/* ---------- 图标绘制（像素风） ---------- */

#define COL_OK    RGB(0x5C, 0xEA, 0x8A)   /* 正常：亮绿 */
#define COL_MID   RGB(0xFF, 0xC4, 0x40)   /* ≤40%：琥珀 */
#define COL_LOW   RGB(0xFF, 0x56, 0x56)   /* ≤20%：红 */
#define COL_IDLE  RGB(0x9A, 0x9A, 0x9A)   /* 休眠 / 无设备：灰 */

/* 3x7 点阵字形，最后一位是 "F"（满电）。3x7 在 16px 图标里可放大到 14x14，最接近像素风的块状感 */
static const unsigned char GLYPH[11][7] = {
    {0x7, 0x5, 0x5, 0x5, 0x5, 0x5, 0x7},   /* 0 */
    {0x2, 0x6, 0x2, 0x2, 0x2, 0x2, 0x7},   /* 1 */
    {0x7, 0x1, 0x1, 0x7, 0x4, 0x4, 0x7},   /* 2 */
    {0x7, 0x1, 0x1, 0x7, 0x1, 0x1, 0x7},   /* 3 */
    {0x5, 0x5, 0x5, 0x7, 0x1, 0x1, 0x1},   /* 4 */
    {0x7, 0x4, 0x4, 0x7, 0x1, 0x1, 0x7},   /* 5 */
    {0x7, 0x4, 0x4, 0x7, 0x5, 0x5, 0x7},   /* 6 */
    {0x7, 0x1, 0x1, 0x2, 0x2, 0x2, 0x2},   /* 7 */
    {0x7, 0x5, 0x5, 0x7, 0x5, 0x5, 0x7},   /* 8 */
    {0x7, 0x5, 0x5, 0x7, 0x1, 0x1, 0x7},   /* 9 */
    {0x7, 0x4, 0x4, 0x7, 0x4, 0x4, 0x4}    /* F */
};

#define GLYPH_H 7

/* 往 32bpp DIB 上写一个不透明像素，(x,y) 以左上角为原点 */
static void px(BYTE *bits, int size, int x, int y, COLORREF c) {
    BYTE *p;
    if (x < 0 || x >= size || y < 0 || y >= size) return;
    p = bits + ((size_t)(size - 1 - y) * size + x) * 4;   /* DIB 自下而上 */
    p[0] = GetBValue(c); p[1] = GetGValue(c); p[2] = GetRValue(c); p[3] = 255;
}

static int glyph_index(wchar_t ch) {
    if (ch >= L'0' && ch <= L'9') return ch - L'0';
    if (ch == L'F' || ch == L'f') return 10;
    return -1;
}

/* 点阵放大绘制：单元边长取整，尽量填满图标（16px 下两位数可占 14x14） */
static void draw_text_pixels(BYTE *bits, int size, const wchar_t *text, COLORREF c) {
    int len = (int)wcslen(text), units, scale, ox, oy, i, gy, gx;
    if (len <= 0) return;

    units = 4 * len - 1;                 /* 每字 3 宽 + 1 间隔 */
    scale = size / units;
    if (scale > size / GLYPH_H) scale = size / GLYPH_H;   /* 高度也要放得下 */
    if (scale < 1) scale = 1;

    ox = (size - units * scale) / 2;
    oy = (size - GLYPH_H * scale) / 2;

    for (i = 0; i < len; i++) {
        int gi = glyph_index(text[i]);
        if (gi < 0) continue;
        for (gy = 0; gy < GLYPH_H; gy++) {
            for (gx = 0; gx < 3; gx++) {
                int x0, y0, dx, dy;
                if (!(GLYPH[gi][gy] & (0x4 >> gx))) continue;
                x0 = ox + (i * 4 + gx) * scale;
                y0 = oy + gy * scale;
                for (dy = 0; dy < scale; dy++)
                    for (dx = 0; dx < scale; dx++)
                        px(bits, size, x0 + dx, y0 + dy, c);
            }
        }
    }
}

/* 休眠：三个方块点 */
static void draw_dots(BYTE *bits, int size, COLORREF c) {
    int d = size / 4, gap, total, x0, y0, i, dx, dy;
    if (d < 3) d = 3;
    gap = (size - 3 * d) / 4;
    if (gap < 1) gap = 1;
    total = 3 * d + 2 * gap;
    x0 = (size - total) / 2;
    if (x0 < 0) x0 = 0;
    y0 = (size - d) / 2;
    for (i = 0; i < 3; i++) {
        int bx = x0 + i * (d + gap);
        for (dy = 0; dy < d; dy++)
            for (dx = 0; dx < d; dx++)
                px(bits, size, bx + dx, y0 + dy, c);
    }
}


/* text == NULL 时画"休眠"三点 */
static HICON make_icon(const wchar_t *text, int size, COLORREF color, BYTE *dump_out) {
    BITMAPINFO bi; HDC dc; HBITMAP bmp; ICONINFO ii; HICON ic; void *bits = NULL;

    if (size < 16) size = 16;

    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = size;          /* 自下而上，符合图标位图约定 */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    dc = CreateCompatibleDC(NULL);
    bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp || !bits) { if (bmp) DeleteObject(bmp); DeleteDC(dc); return NULL; }
    ZeroMemory(bits, (size_t)size * size * 4);

    if (text) draw_text_pixels((BYTE *)bits, size, text, color);
    else      draw_dots((BYTE *)bits, size, color);

    if (dump_out) CopyMemory(dump_out, bits, (size_t)size * size * 4);

    ZeroMemory(&ii, sizeof ii);
    ii.fIcon = TRUE;
    ii.hbmColor = bmp;
    ii.hbmMask = CreateBitmap(size, size, 1, 1, NULL);
    ic = CreateIconIndirect(&ii);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    DeleteObject(bmp);
    DeleteDC(dc);
    return ic;
}

/* ---------- 配置 ---------- */

static DWORD reg_get(const wchar_t *name, DWORD def) {
    HKEY k; DWORD v = def, sz = sizeof v, t = REG_DWORD;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\ATKA9Battery", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        RegQueryValueExW(k, name, NULL, &t, (LPBYTE)&v, &sz);
        RegCloseKey(k);
    }
    return v;
}

static void reg_set(const wchar_t *name, DWORD val) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\ATKA9Battery", 0, NULL, 0,
                        KEY_WRITE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&val, sizeof val);
        RegCloseKey(k);
    }
}

static BOOL startup_enabled(void) {
    HKEY k; BOOL on = FALSE;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_READ, &k) == ERROR_SUCCESS) {
        on = (RegQueryValueExW(k, APP_NAME, NULL, NULL, NULL, NULL) == ERROR_SUCCESS);
        RegCloseKey(k);
    }
    return on;
}

static void startup_set(BOOL on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        if (on) {
            wchar_t path[MAX_PATH + 4], quoted[MAX_PATH + 8];
            GetModuleFileNameW(NULL, path, MAX_PATH);
            wsprintfW(quoted, L"\"%s\"", path);
            RegSetValueExW(k, APP_NAME, 0, REG_SZ, (const BYTE *)quoted,
                           (DWORD)((wcslen(quoted) + 1) * sizeof(wchar_t)));
        } else {
            RegDeleteValueW(k, APP_NAME);
        }
        RegCloseKey(k);
    }
}

/* ---------- 托盘 ---------- */

static void tray_add(void) {
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof nid);
    nid.cbSize = sizeof nid;
    nid.hWnd = g_hwnd;
    nid.uID = TRAY_ID;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = g_icon;
    lstrcpynW(nid.szTip, L"ATK A9 Pro Max", 128);
    Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

static void tray_update(const wchar_t *tip) {
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof nid);
    nid.cbSize = sizeof nid;
    nid.hWnd = g_hwnd;
    nid.uID = TRAY_ID;
    nid.uFlags = NIF_ICON | NIF_TIP;
    nid.hIcon = g_icon;
    lstrcpynW(nid.szTip, tip, 128);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void tray_balloon(const wchar_t *title, const wchar_t *text) {
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof nid);
    nid.cbSize = sizeof nid;
    nid.hWnd = g_hwnd;
    nid.uID = TRAY_ID;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_WARNING;
    lstrcpynW(nid.szInfoTitle, title, 64);
    lstrcpynW(nid.szInfo, text, 256);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void poll_now(void) {
    BatState st = query_battery();
    wchar_t num[8];
    static int warned = 0;
    SYSTEMTIME t;
    int size = GetSystemMetrics(SM_CXSMICON);
    HICON nicon;

    if (size < 16) size = 16;
    GetLocalTime(&t);

    if (st.asleep) {
        nicon = make_icon(NULL, size, COL_IDLE, NULL);
        if (g_percent >= 0) wsprintfW(g_tip, L"ATK A9 Pro Max：休眠中（上次 %d%%）", g_percent);
        else lstrcpynW(g_tip, L"ATK A9 Pro Max：休眠中，动一下鼠标", 160);
    } else if (!st.ok) {
        nicon = make_icon(NULL, size, COL_IDLE, NULL);
        lstrcpynW(g_tip, L"ATK A9 Pro Max：未检测到接收器", 160);
        g_percent = -1;
        warned = 0;
    } else {
        COLORREF c = COL_OK;
        int vi = st.mv / 1000, vm = (st.mv % 1000) / 10;
        if (st.percent <= 20) c = COL_LOW;
        else if (st.percent <= 40) c = COL_MID;
        if (st.percent >= 100) lstrcpynW(num, L"F", 8);   /* 满电用 F，两位数位置放得更大 */
        else wsprintfW(num, L"%d", st.percent);
        nicon = make_icon(num, size, c, NULL);
        if (st.mv > 3000 && st.mv < 4700)
            wsprintfW(g_tip, L"ATK A9 Pro Max：%d%%  %d.%02d V%s\n更新于 %02d:%02d",
                      st.percent, vi, vm, st.wired ? L"（有线）" : L"", t.wHour, t.wMinute);
        else
            wsprintfW(g_tip, L"ATK A9 Pro Max：%d%%%s\n更新于 %02d:%02d",
                      st.percent, st.wired ? L"（有线）" : L"", t.wHour, t.wMinute);

        if (st.percent <= 20 && !warned) {
            wchar_t body[96];
            wsprintfW(body, L"当前电量 %d%%，该充电了。", st.percent);
            tray_balloon(L"鼠标电量低", body);
            warned = 1;
        }
        if (st.percent > 30) warned = 0;
        g_percent = st.percent;
    }

    if (nicon) {
        if (g_icon) DestroyIcon(g_icon);
        g_icon = nicon;
    }
    tray_update(g_tip);
}

static void show_menu(void) {
    POINT pt; HMENU m; int cmd; DWORD iv = reg_get(L"PollSeconds", 60);
    GetCursorPos(&pt);
    m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, MENU_REFRESH, L"立即刷新");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (iv == 30 ? MF_CHECKED : 0), MENU_30S, L"刷新间隔 30 秒");
    AppendMenuW(m, MF_STRING | (iv == 60 ? MF_CHECKED : 0), MENU_60S, L"刷新间隔 60 秒");
    AppendMenuW(m, MF_STRING | (iv == 300 ? MF_CHECKED : 0), MENU_5MIN, L"刷新间隔 5 分钟");
    AppendMenuW(m, MF_STRING | (iv == 900 ? MF_CHECKED : 0), MENU_15MIN, L"刷新间隔 15 分钟");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (startup_enabled() ? MF_CHECKED : 0), MENU_STARTUP, L"开机自启");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, MENU_EXIT, L"退出");

    SetForegroundWindow(g_hwnd);
    cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, NULL);
    DestroyMenu(m);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);

    switch (cmd) {
    case MENU_REFRESH:
        poll_now();
        break;
    case MENU_30S:
    case MENU_60S:
    case MENU_5MIN:
    case MENU_15MIN: {
        DWORD sec = (cmd == MENU_30S) ? 30 : (cmd == MENU_60S ? 60 : (cmd == MENU_5MIN ? 300 : 900));
        reg_set(L"PollSeconds", sec);
        SetTimer(g_hwnd, TIMER_POLL, sec * 1000, NULL);
        break;
    }
    case MENU_STARTUP:
        startup_set(!startup_enabled());
        break;
    case MENU_EXIT:
        DestroyWindow(g_hwnd);
        break;
    default:
        break;
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_taskbarCreated != 0 && msg == g_taskbarCreated) {
        tray_add();
        poll_now();
        return 0;
    }
    switch (msg) {
    case WM_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_LBUTTONDBLCLK) poll_now();
        else if (LOWORD(lp) == WM_RBUTTONUP) show_menu();
        return 0;
    case WM_DEVICECHANGE:
        /* 插拔数据线 / 接收器后 1.2 秒自动刷新一次（等系统枚举完成），
         * 不用等下一个轮询周期。 */
        if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE)
            SetTimer(hwnd, TIMER_KICK, 1200, NULL);
        return TRUE;
    case WM_TIMER:
        if (wp == TIMER_POLL) poll_now();
        else if (wp == TIMER_KICK) { KillTimer(hwnd, TIMER_KICK); poll_now(); }
        return 0;
    case WM_DESTROY: {
        NOTIFYICONDATAW nid;
        KillTimer(hwnd, TIMER_POLL);
        KillTimer(hwnd, TIMER_KICK);
        ZeroMemory(&nid, sizeof nid);
        nid.cbSize = sizeof nid; nid.hWnd = hwnd; nid.uID = TRAY_ID;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show) {
    HANDLE mutex; MSG msg; WNDCLASSEXW wc; DWORD interval;
    (void)prev; (void)show;

    if (cmdline && wcsstr(cmdline, L"--once")) {
        BatState st = query_battery();
        if (st.ok) printf("%d%% (%d mV)%s\n", st.percent, st.mv, st.wired ? " wired" : "");
        else if (st.asleep) printf("asleep\n");
        else printf("no device\n");
        return 0;
    }

    /* 开发用: --icon <out.bmp> 把图标合成到深色底上导出，便于肉眼检查 */
    if (cmdline && wcsstr(cmdline, L"--icon")) {
        int size = 32, x, y, stride;
        const wchar_t *text = L"32";
        wchar_t tbuf[8];
        if (wcsstr(cmdline, L"--s16")) size = 16;
        else if (wcsstr(cmdline, L"--s20")) size = 20;
        else if (wcsstr(cmdline, L"--s24")) size = 24;
        {
            const wchar_t *tp = wcsstr(cmdline, L"--text");
            if (tp) {
                int n = 0;
                tp += 6;
                while (*tp == L' ') tp++;
                while (*tp && *tp != L' ' && n < 7) tbuf[n++] = *tp++;
                tbuf[n] = 0;
                if (n) text = tbuf;
            }
        }
        BYTE *raw = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)size * size * 4);
        FILE *f;
        BITMAPFILEHEADER bfh;
        BITMAPINFOHEADER bih;
        wchar_t *arg = wcsstr(cmdline, L"--icon");
        wchar_t outpath[MAX_PATH] = L"icon.bmp";
        int k = 0;
        if (!raw) return 1;
        if (arg) {
            arg += 6;
            while (*arg == L' ') arg++;
            while (arg[k] && arg[k] != L' ' && k < MAX_PATH - 1) { outpath[k] = arg[k]; k++; }
            outpath[k] = 0;
        }
        {
            int dots = wcsstr(cmdline, L"--dots") != NULL;
            COLORREF c = dots ? COL_IDLE : COL_OK;
            if (!dots && text[0] >= L'0' && text[0] <= L'9') {
                int v = _wtoi(text);
                if (v <= 20) c = COL_LOW;
                else if (v <= 40) c = COL_MID;
            }
            make_icon(dots ? NULL : text, size, c, raw);
        }
        stride = ((size * 3 + 3) / 4) * 4;
        ZeroMemory(&bih, sizeof bih);
        bih.biSize = sizeof bih; bih.biWidth = size; bih.biHeight = size;
        bih.biPlanes = 1; bih.biBitCount = 24; bih.biCompression = BI_RGB;
        bih.biSizeImage = (DWORD)stride * size;
        ZeroMemory(&bfh, sizeof bfh);
        bfh.bfType = 0x4D42;
        bfh.bfOffBits = sizeof bfh + sizeof bih;
        bfh.bfSize = bfh.bfOffBits + bih.biSizeImage;
        f = _wfopen(outpath, L"wb");
        if (!f) { HeapFree(GetProcessHeap(), 0, raw); return 1; }
        fwrite(&bfh, sizeof bfh, 1, f);
        fwrite(&bih, sizeof bih, 1, f);
        for (y = 0; y < size; y++) {
            BYTE *row = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, stride);
            for (x = 0; x < size; x++) {
                BYTE *p = raw + ((size_t)y * size + x) * 4;
                int a = p[3], i;
                for (i = 0; i < 3; i++) {
                    int c = p[i];               /* DIB 内存本身即 B,G,R 顺序 */
                    row[x * 3 + i] = (BYTE)((c * a + 32 * (255 - a)) / 255);
                }
            }
            fwrite(row, 1, stride, f);
            HeapFree(GetProcessHeap(), 0, row);
        }
        fclose(f);
        HeapFree(GetProcessHeap(), 0, raw);
        return 0;
    }

    mutex = CreateMutexW(NULL, TRUE, L"ATKA9BatterySingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = WND_CLASS;
    if (!RegisterClassExW(&wc)) return 1;

    g_hwnd = CreateWindowExW(0, WND_CLASS, APP_NAME, WS_POPUP,
                             0, 0, 0, 0, NULL, NULL, inst, NULL);
    if (!g_hwnd) return 1;

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    if (!reg_get(L"InitDone", 0)) {
        startup_set(TRUE);      /* 首次运行即设为开机自启 */
        reg_set(L"InitDone", 1);
    }

    poll_now();                 /* 先取真实电量生成数字图标（此时模组还未注册，刷新无效但无害） */
    tray_add();                 /* 用真实图标注册，避免托盘里先闪一个占位符 */
    tray_update(g_tip);

    interval = reg_get(L"PollSeconds", 60);
    if (interval < 10 || interval > 3600) interval = 60;
    SetTimer(g_hwnd, TIMER_POLL, interval * 1000, NULL);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_icon) DestroyIcon(g_icon);
    if (mutex) CloseHandle(mutex);
    return 0;
}
