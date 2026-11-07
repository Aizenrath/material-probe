/* Windows window for the material probe. No Python.
   Double-click once. It copies itself into Startup, then shows the photo,
   the color, and the same three actions as the test page. */

#define WIN32_LEAN_AND_MEAN
#define INITGUID
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <setupapi.h>
#include <gdiplus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define MAX_JPEG (4 * 1024 * 1024)
#define WM_APP_STATE (WM_APP + 1)
#define IDC_AGAIN 1
#define IDC_WHITE 2
#define IDC_FOLDER 3

static CRITICAL_SECTION gLock;
static HWND gWnd;
static HFONT gFont;
static HFONT gHexFont;
static GpBitmap *gBitmap;
static unsigned char *gJpeg;
static int gJpegLen;
static wchar_t gHex[16] = L"";
static wchar_t gDetail[320] = L"Plug the probe into a USB port.";
static wchar_t gFolder[MAX_PATH];
static volatile LONG gCmd;
static HANDLE gLog = INVALID_HANDLE_VALUE;

static void logLine(const char *text) {
    char line[400];
    SYSTEMTIME t;
    DWORD wrote;
    int n;
    if (gLog == INVALID_HANDLE_VALUE) return;
    GetLocalTime(&t);
    n = _snprintf(line, sizeof line, "%04u-%02u-%02u %02u:%02u:%02u %s\r\n",
                  t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, text);
    if (n > 0) WriteFile(gLog, line, (DWORD)n, &wrote, NULL);
}

static void setDetail(const wchar_t *text) {
    EnterCriticalSection(&gLock);
    wcsncpy(gDetail, text, 319);
    gDetail[319] = 0;
    LeaveCriticalSection(&gLock);
    if (gWnd) PostMessageW(gWnd, WM_APP_STATE, 0, 0);
}

static int samePath(const wchar_t *a, const wchar_t *b) {
    return CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE, a, -1, b, -1) == CSTR_EQUAL;
}

static void desktopFolder(wchar_t *out, size_t chars) {
    wchar_t desktop[MAX_PATH];
    if (SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, desktop) != S_OK) {
        wcsncpy(out, L".\\Material Probe", chars);
        return;
    }
    _snwprintf(out, chars, L"%s\\Material Probe", desktop);
}

static void installStartupCopy(void) {
    wchar_t me[MAX_PATH];
    wchar_t startup[MAX_PATH];
    wchar_t dest[MAX_PATH];
    if (!GetModuleFileNameW(NULL, me, MAX_PATH)) return;
    if (SHGetFolderPathW(NULL, CSIDL_STARTUP, NULL, 0, startup) != S_OK) return;
    _snwprintf(dest, MAX_PATH, L"%s\\MatProb.exe", startup);
    if (samePath(me, dest)) return;
    CopyFileW(me, dest, FALSE);
}

static int findPort(wchar_t *out, size_t outChars) {
    HDEVINFO devs;
    SP_DEVINFO_DATA info;
    DWORD index;
    int found = 0;
    devs = SetupDiGetClassDevsW(NULL, L"USB", NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (devs == INVALID_HANDLE_VALUE) return 0;
    info.cbSize = sizeof info;
    for (index = 0; SetupDiEnumDeviceInfo(devs, index, &info); index++) {
        wchar_t hardware[512];
        const wchar_t *scan;
        int match = 0;
        HKEY key;
        wchar_t port[32];
        DWORD portBytes;
        DWORD kind;
        if (!SetupDiGetDeviceRegistryPropertyW(
                devs, &info, SPDRP_HARDWAREID, NULL,
                (PBYTE)hardware, sizeof hardware, NULL)) {
            continue;
        }
        scan = hardware;
        while (*scan) {
            if (wcsstr(scan, L"VID_303A") && wcsstr(scan, L"PID_1001")) match = 1;
            scan += wcslen(scan) + 1;
        }
        if (!match) continue;
        key = SetupDiOpenDevRegKey(devs, &info, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE) continue;
        portBytes = sizeof port;
        kind = REG_SZ;
        if (RegQueryValueExW(key, L"PortName", NULL, &kind, (LPBYTE)port, &portBytes) == ERROR_SUCCESS &&
            wcsncmp(port, L"COM", 3) == 0) {
            wcsncpy(out, port, outChars - 1);
            out[outChars - 1] = 0;
            found = 1;
        }
        RegCloseKey(key);
        if (found) break;
    }
    SetupDiDestroyDeviceInfoList(devs);
    return found;
}

static HANDLE openPort(const wchar_t *com) {
    wchar_t path[40];
    HANDLE handle;
    DCB dcb;
    COMMTIMEOUTS timeouts;
    _snwprintf(path, 40, L"\\\\.\\%s", com);
    handle = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (handle == INVALID_HANDLE_VALUE) return handle;
    dcb.DCBlength = sizeof dcb;
    if (!GetCommState(handle, &dcb)) {
        CloseHandle(handle);
        return INVALID_HANDLE_VALUE;
    }
    dcb.BaudRate = CBR_115200;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fDtrControl = DTR_CONTROL_DISABLE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fBinary = TRUE;
    if (!SetCommState(handle, &dcb)) {
        CloseHandle(handle);
        return INVALID_HANDLE_VALUE;
    }
    timeouts.ReadIntervalTimeout = 20;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = 200;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 2000;
    SetCommTimeouts(handle, &timeouts);
    EscapeCommFunction(handle, CLRDTR);
    EscapeCommFunction(handle, SETRTS);
    Sleep(120);
    EscapeCommFunction(handle, CLRRTS);
    PurgeComm(handle, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return handle;
}

static const char *findBytes(const unsigned char *hay, int hayLen, const char *needle) {
    int n = (int)strlen(needle);
    int i;
    if (n == 0 || hayLen < n) return NULL;
    for (i = 0; i <= hayLen - n; i++) {
        if (memcmp(hay + i, needle, (size_t)n) == 0) return (const char *)(hay + i);
    }
    return NULL;
}

static void takeHex(const char *json, int jsonLen, char *hex, size_t hexCap) {
    const char *key = "\"hex\":\"";
    const char *at = findBytes((const unsigned char *)json, jsonLen, key);
    size_t i = 0;
    hex[0] = 0;
    if (!at) return;
    at += strlen(key);
    while (i + 1 < hexCap && at[i] && at[i] != '"') {
        hex[i] = at[i];
        i++;
    }
    hex[i] = 0;
}

static int jsonOk(const char *json, int jsonLen) {
    return findBytes((const unsigned char *)json, jsonLen, "\"ok\":true") != NULL;
}

static void publish(const char *hex, const unsigned char *jpeg, int jpegLen, const wchar_t *detail) {
    wchar_t wideHex[16];
    EnterCriticalSection(&gLock);
    if (jpeg && jpegLen > 0) {
        free(gJpeg);
        gJpeg = (unsigned char *)malloc((size_t)jpegLen);
        if (gJpeg) {
            memcpy(gJpeg, jpeg, (size_t)jpegLen);
            gJpegLen = jpegLen;
        }
    }
    MultiByteToWideChar(CP_UTF8, 0, hex, -1, wideHex, 16);
    wcsncpy(gHex, wideHex, 15);
    gHex[15] = 0;
    wcsncpy(gDetail, detail, 319);
    gDetail[319] = 0;
    LeaveCriticalSection(&gLock);
    if (gWnd) PostMessageW(gWnd, WM_APP_STATE, 0, 0);
}

static void saveReading(const char *hex, const unsigned char *jpeg, int jpegLen) {
    wchar_t jpgPath[MAX_PATH];
    wchar_t txtPath[MAX_PATH];
    wchar_t detail[MAX_PATH];
    SYSTEMTIME t;
    const char *src = hex;
    wchar_t hexPart[8];
    int i;
    HANDLE file;
    DWORD wrote;
    char text[16];
    int textLen;
    if (jpegLen < 2 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) {
        publish(hex, NULL, 0, L"The photo was not a valid picture.");
        return;
    }
    if (*src == '#') src++;
    for (i = 0; i < 6 && src[i]; i++) hexPart[i] = (wchar_t)src[i];
    hexPart[i] = 0;
    if (i != 6) return;
    CreateDirectoryW(gFolder, NULL);
    GetLocalTime(&t);
    _snwprintf(jpgPath, MAX_PATH, L"%s\\%04u%02u%02u-%02u%02u%02u-%s.jpg",
               gFolder, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, hexPart);
    _snwprintf(txtPath, MAX_PATH, L"%s\\%04u%02u%02u-%02u%02u%02u-%s.txt",
               gFolder, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, hexPart);
    file = CreateFileW(jpgPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        publish(hex, jpeg, jpegLen, L"Could not save the photo.");
        return;
    }
    WriteFile(file, jpeg, (DWORD)jpegLen, &wrote, NULL);
    CloseHandle(file);
    file = CreateFileW(txtPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        textLen = _snprintf(text, sizeof text, "#%c%c%c%c%c%c\r\n", src[0], src[1], src[2], src[3], src[4], src[5]);
        if (textLen > 0) WriteFile(file, text, (DWORD)textLen, &wrote, NULL);
        CloseHandle(file);
    }
    _snwprintf(detail, MAX_PATH, L"Saved %04u%02u%02u-%02u%02u%02u-%s.jpg",
               t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, hexPart);
    publish(hex, jpeg, jpegLen, detail);
    logLine("Saved a photo and its hex.");
}

static void listenPort(HANDLE port) {
    unsigned char *buf = (unsigned char *)malloc(MAX_JPEG + 65536);
    int cap = MAX_JPEG + 65536;
    int len = 0;
    if (!buf) return;
    for (;;) {
        LONG cmd = InterlockedExchange(&gCmd, 0);
        int room;
        int got;
        const char *color;
        const char *nl;
        const char *hdr;
        const char *nl2;
        int jpegLen;
        int headerAt;
        int payload;
        int consumed;
        char hex[16];
        if (cmd == 'G' || cmd == 'W') {
            char byte = (char)cmd;
            DWORD wrote = 0;
            setDetail(cmd == 'W' ? L"Hold the sensor flat on a white card…" : L"Reading the sensor and taking a photo…");
            WriteFile(port, &byte, 1, &wrote, NULL);
        }
        room = cap - len;
        if (room > 8192) room = 8192;
        if (room <= 0) {
            memmove(buf, buf + len - 1024, 1024);
            len = 1024;
            room = 8192;
        }
        got = 0;
        if (!ReadFile(port, buf + len, (DWORD)room, (DWORD *)&got, NULL)) break;
        len += got;

        color = findBytes(buf, len, "##COLOR ");
        if (!color) {
            if (len > 4096) {
                memmove(buf, buf + len - 64, 64);
                len = 64;
            }
            continue;
        }
        headerAt = (int)(color - (const char *)buf);
        if (headerAt > 0) {
            memmove(buf, buf + headerAt, (size_t)(len - headerAt));
            len -= headerAt;
        }
        nl = findBytes(buf, len, "\n");
        if (!nl) continue;
        hdr = findBytes((const unsigned char *)nl, len - (int)(nl - (const char *)buf), "##JPEG ");
        if (!hdr) continue;
        nl2 = findBytes((const unsigned char *)hdr, len - (int)(hdr - (const char *)buf), "\n");
        if (!nl2) continue;
        jpegLen = atoi(hdr + 7);  // "##JPEG " is 7 characters
        if (jpegLen < 0 || jpegLen > MAX_JPEG) {
            memmove(buf, buf + 8, (size_t)(len - 8));
            len -= 8;
            continue;
        }
        payload = (int)(nl2 - (const char *)buf) + 1;
        if (len < payload + jpegLen) continue;
        takeHex((const char *)buf, (int)(nl - (const char *)buf), hex, sizeof hex);
        if (!jsonOk((const char *)buf, (int)(nl - (const char *)buf))) {
            publish(hex[0] ? hex : "#000000", NULL, 0, L"The probe could not read the color.");
        } else if (jpegLen > 0) {
            saveReading(hex, buf + payload, jpegLen);
        } else {
            publish(hex, NULL, 0, L"White saved");
        }
        consumed = payload + jpegLen;
        if (consumed < len) {
            memmove(buf, buf + consumed, (size_t)(len - consumed));
            len -= consumed;
        } else {
            len = 0;
        }
    }
    free(buf);
}

static DWORD WINAPI serialThread(LPVOID) {
    for (;;) {
        wchar_t com[32];
        HANDLE port;
        if (!findPort(com, 32)) {
            setDetail(L"Plug the probe into a USB port.");
            Sleep(400);
            continue;
        }
        port = openPort(com);
        if (port == INVALID_HANDLE_VALUE) {
            Sleep(800);
            continue;
        }
        logLine("Probe connected.");
        setDetail(L"Probe connected.");
        if (InterlockedCompareExchange(&gCmd, 'G', 0) == 0) {
            /* First look when the probe is plugged in. A button press still works. */
        }
        listenPort(port);
        CloseHandle(port);
        logLine("Probe unplugged.");
        setDetail(L"Plug the probe into a USB port.");
        Sleep(400);
    }
    return 0;
}

static COLORREF hexColor(const wchar_t *hex) {
    unsigned value;
    if (hex[0] == L'#') hex++;
    value = (unsigned)wcstoul(hex, NULL, 16);
    return RGB((value >> 16) & 255, (value >> 8) & 255, value & 255);
}

static void loadBitmapFromJpeg(void) {
    unsigned char *copy = NULL;
    int len = 0;
    HGLOBAL mem;
    void *locked;
    IStream *stream = NULL;
    EnterCriticalSection(&gLock);
    if (gJpeg && gJpegLen > 0) {
        copy = (unsigned char *)malloc((size_t)gJpegLen);
        if (copy) {
            memcpy(copy, gJpeg, (size_t)gJpegLen);
            len = gJpegLen;
        }
    }
    LeaveCriticalSection(&gLock);
    if (!copy) return;
    if (gBitmap) {
        GdipDisposeImage((GpImage *)gBitmap);
        gBitmap = NULL;
    }
    mem = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)len);
    if (!mem) {
        free(copy);
        return;
    }
    locked = GlobalLock(mem);
    memcpy(locked, copy, (size_t)len);
    GlobalUnlock(mem);
    free(copy);
    if (CreateStreamOnHGlobal(mem, TRUE, &stream) != S_OK) {
        GlobalFree(mem);
        return;
    }
    if (GdipCreateBitmapFromStream(stream, &gBitmap) != Ok) gBitmap = NULL;
    stream->lpVtbl->Release(stream);
}

static void layout(HWND hwnd) {
    RECT rc;
    int y;
    GetClientRect(hwnd, &rc);
    y = 52 + 440 + 16 + 112 + 16;
    MoveWindow(GetDlgItem(hwnd, IDC_AGAIN), 24, y, 130, 36, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_WHITE), 162, y, 150, 36, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_FOLDER), 320, y, 150, 36, TRUE);
}

static void paintWindow(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    RECT photo;
    RECT swatch;
    RECT text;
    wchar_t hex[16];
    wchar_t detail[320];
    HBRUSH bg;
    HBRUSH frame;
    HBRUSH chip;
    GetClientRect(hwnd, &rc);
    bg = CreateSolidBrush(RGB(243, 239, 230));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(28, 25, 21));
    SelectObject(dc, gFont);
    text.left = 24;
    text.top = 16;
    text.right = rc.right - 24;
    text.bottom = 44;
    DrawTextW(dc, L"Material probe", -1, &text, DT_LEFT | DT_SINGLELINE);

    photo.left = 24;
    photo.top = 52;
    photo.right = rc.right - 24;
    photo.bottom = 52 + 440;
    frame = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(dc, &photo, frame);
    DeleteObject(frame);
    if (gBitmap) {
        GpGraphics *graphics = NULL;
        if (GdipCreateFromHDC(dc, &graphics) == Ok && graphics) {
            GdipSetInterpolationMode(graphics, InterpolationModeHighQualityBicubic);
            GdipDrawImageRectI(graphics, (GpImage *)gBitmap,
                               (INT)(photo.left + 8), (INT)(photo.top + 8),
                               (INT)(photo.right - photo.left - 16),
                               (INT)(photo.bottom - photo.top - 16));
            GdipDeleteGraphics(graphics);
        }
    }

    EnterCriticalSection(&gLock);
    wcsncpy(hex, gHex, 15);
    hex[15] = 0;
    wcsncpy(detail, gDetail, 319);
    detail[319] = 0;
    LeaveCriticalSection(&gLock);

    swatch.left = 24;
    swatch.top = photo.bottom + 16;
    swatch.right = swatch.left + 112;
    swatch.bottom = swatch.top + 112;
    chip = CreateSolidBrush(hex[0] ? hexColor(hex) : RGB(0, 0, 0));
    FillRect(dc, &swatch, chip);
    DeleteObject(chip);
    FrameRect(dc, &swatch, (HBRUSH)GetStockObject(BLACK_BRUSH));

    SelectObject(dc, gHexFont);
    text.left = swatch.right + 16;
    text.top = swatch.top + 8;
    text.right = rc.right - 24;
    text.bottom = text.top + 48;
    DrawTextW(dc, hex[0] ? hex : L"waiting", -1, &text, DT_LEFT | DT_SINGLELINE);
    SelectObject(dc, gFont);
    SetTextColor(dc, RGB(92, 86, 76));
    text.top = text.bottom;
    text.bottom = swatch.bottom;
    DrawTextW(dc, detail, -1, &text, DT_LEFT | DT_WORDBREAK);

    text.left = 24;
    text.top = swatch.bottom + 16 + 44;
    text.right = rc.right - 24;
    text.bottom = text.top + 48;
    DrawTextW(dc, gFolder, -1, &text, DT_LEFT | DT_WORDBREAK);
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        CreateWindowW(L"BUTTON", L"Read again", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      0, 0, 10, 10, hwnd, (HMENU)IDC_AGAIN, NULL, NULL);
        CreateWindowW(L"BUTTON", L"This is white", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      0, 0, 10, 10, hwnd, (HMENU)IDC_WHITE, NULL, NULL);
        CreateWindowW(L"BUTTON", L"Open pictures", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      0, 0, 10, 10, hwnd, (HMENU)IDC_FOLDER, NULL, NULL);
        SendMessageW(GetDlgItem(hwnd, IDC_AGAIN), WM_SETFONT, (WPARAM)gFont, TRUE);
        SendMessageW(GetDlgItem(hwnd, IDC_WHITE), WM_SETFONT, (WPARAM)gFont, TRUE);
        SendMessageW(GetDlgItem(hwnd, IDC_FOLDER), WM_SETFONT, (WPARAM)gFont, TRUE);
        layout(hwnd);
        return 0;
    case WM_SIZE:
        layout(hwnd);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_AGAIN) InterlockedExchange(&gCmd, 'G');
        if (LOWORD(wp) == IDC_WHITE) InterlockedExchange(&gCmd, 'W');
        if (LOWORD(wp) == IDC_FOLDER) {
            CreateDirectoryW(gFolder, NULL);
            ShellExecuteW(hwnd, L"open", gFolder, NULL, NULL, SW_SHOWNORMAL);
        }
        return 0;
    case WM_APP_STATE:
        loadBitmapFromJpeg();
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    case WM_PAINT:
        paintWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    GdiplusStartupInput input;
    ULONG_PTR token = 0;
    WNDCLASSW wc;
    MSG msg;
    HANDLE mutex;
    wchar_t logDir[MAX_PATH];
    wchar_t logPath[MAX_PATH];
    (void)show;

    mutex = CreateMutexW(NULL, FALSE, L"Local\\MatProb");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(L"MatProbWindow", NULL);
        if (existing) {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        } else {
            MessageBoxW(NULL,
                        L"MatProb is already running. Look for it in the taskbar.",
                        L"MatProb", MB_OK);
        }
        return 0;
    }

    InitializeCriticalSection(&gLock);
    desktopFolder(gFolder, MAX_PATH);
    CreateDirectoryW(gFolder, NULL);
    if (SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, logDir) == S_OK) {
        wcscat(logDir, L"\\MaterialProbe");
        CreateDirectoryW(logDir, NULL);
        _snwprintf(logPath, MAX_PATH, L"%s\\probe.log", logDir);
        gLog = CreateFileW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    gFont = CreateFontW(-18, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH, L"Segoe UI");
    gHexFont = CreateFontW(-36, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           FIXED_PITCH, L"Consolas");

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"MatProbWindow";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(243, 239, 230));
    if (!RegisterClassW(&wc)) {
        MessageBoxW(NULL, L"MatProb could not open its window.", L"MatProb", MB_OK | MB_ICONERROR);
        return 1;
    }
    gWnd = CreateWindowW(L"MatProbWindow", L"Material probe",
                         WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                         CW_USEDEFAULT, CW_USEDEFAULT, 980, 860,
                         NULL, NULL, instance, NULL);
    if (!gWnd) {
        MessageBoxW(NULL, L"MatProb could not open its window.", L"MatProb", MB_OK | MB_ICONERROR);
        return 1;
    }
    ShowWindow(gWnd, SW_SHOWNORMAL);
    UpdateWindow(gWnd);
    memset(&input, 0, sizeof input);
    input.GdiplusVersion = 1;
    GdiplusStartup(&token, &input, NULL);
    installStartupCopy();
    CreateThread(NULL, 0, serialThread, NULL, 0, NULL);

    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (token) GdiplusShutdown(token);
    return 0;
}
