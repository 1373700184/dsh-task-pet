#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <cmath>
#include <cstdlib>
#include <cstdio>

using namespace Gdiplus;

static const wchar_t* kClassName = L"DshPetWindow";
static const int BASE_W = 380;
static const int BASE_H = 320;
static const float PET_CX = 280.0f;
static const float PET_CY = 232.0f;
static const float R = 62.0f;

struct PetState {
    std::wstring status = L"idle";
    int done = 0;
    int total = 0;
    std::wstring current;
    std::wstring title;
    std::wstring url = L"http://127.0.0.1:3080/";
    unsigned long host = 0;
    bool loaded = false;
};

static PetState g_state;
static std::wstring g_statePath;
static std::wstring g_posPath;
static float g_scale = 1.0f;
static ULONGLONG g_t0 = 0;
static FILETIME g_stateTime{};
static ULONGLONG g_stateSize = 0;
static bool g_bubbleLatch = false;

static HDC g_memDC = nullptr;
static HBITMAP g_bmp = nullptr;
static HBITMAP g_oldBmp = nullptr;
static void* g_bits = nullptr;
static int g_surfW = 0;
static int g_surfH = 0;
static Bitmap* g_gpBmp = nullptr;
static Graphics* g_gp = nullptr;
static FontFamily* g_family = nullptr;

static bool g_dragging = false;
static bool g_moved = false;
static POINT g_dragFrom{};
static POINT g_winFrom{};

static ULONGLONG g_lastPoll = 0;
static ULONGLONG g_lastHostCheck = 0;
static ULONGLONG g_deadSince = 0;

static double Now() { return (double)(GetTickCount64() - g_t0) / 1000.0; }

static std::wstring ExeDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf, n);
    size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring(L".") : p.substr(0, slash);
}

static std::wstring Join(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

static bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring ReadFileText(const std::wstring& p, DWORD cap) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (LONGLONG)cap) {
        CloseHandle(h);
        return L"";
    }
    std::string bytes((size_t)sz.QuadPart, '\0');
    DWORD read = 0;
    BOOL ok = ReadFile(h, bytes.data(), (DWORD)bytes.size(), &read, nullptr);
    CloseHandle(h);
    if (!ok) return L"";
    bytes.resize(read);
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), (int)bytes.size(), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring text((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), text.data(), n);
    return text;
}

static std::wstring Clean(std::wstring s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) {
        if (c < 32) out.push_back(L' ');
        else if (c == 127) continue;
        else out.push_back(c);
    }
    while (!out.empty() && out.back() == L' ') out.pop_back();
    size_t i = 0;
    while (i < out.size() && out[i] == L' ') i++;
    return out.substr(i);
}

static int ParseInt(const std::wstring& v) {
    wchar_t* end = nullptr;
    long n = wcstol(v.c_str(), &end, 10);
    if (n < 0) n = 0;
    if (n > 100000) n = 100000;
    return (int)n;
}

static bool ParseState(const std::wstring& text, PetState& out) {
    PetState s;
    bool any = false;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find(L'\n', pos);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        size_t eq = line.find(L'=');
        if (eq != std::wstring::npos) {
            std::wstring k = line.substr(0, eq);
            std::wstring v = Clean(line.substr(eq + 1));
            if (k == L"status") {
                if (v == L"working" || v == L"waiting" || v == L"idle") {
                    s.status = v;
                    any = true;
                }
            } else if (k == L"done") {
                s.done = ParseInt(v);
            } else if (k == L"total") {
                s.total = ParseInt(v);
            } else if (k == L"current") {
                s.current = v;
            } else if (k == L"title") {
                s.title = v;
            } else if (k == L"url") {
                if (!v.empty()) s.url = v;
            } else if (k == L"host") {
                s.host = (unsigned long)ParseInt(v);
            }
        }
        if (end == text.size()) break;
        pos = end + 1;
    }
    if (s.done > s.total) s.done = s.total;
    if (!any) return false;
    s.loaded = true;
    out = s;
    return true;
}

static bool ReloadState(bool force) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(g_statePath.c_str(), GetFileExInfoStandard, &fad)) return false;
    ULONGLONG size = ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    if (!force && size == g_stateSize && CompareFileTime(&fad.ftLastWriteTime, &g_stateTime) == 0) return false;
    std::wstring text = ReadFileText(g_statePath, 65536);
    if (text.empty()) return false;
    PetState next;
    if (!ParseState(text, next)) return false;
    g_state = next;
    g_stateSize = size;
    g_stateTime = fad.ftLastWriteTime;
    return true;
}

static void SavePos(HWND hwnd) {
    RECT rc{};
    GetWindowRect(hwnd, &rc);
    std::wstring body = std::to_wstring(rc.left) + L" " + std::to_wstring(rc.top);
    HANDLE h = CreateFileW(g_posPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(h, body.data(), (DWORD)(body.size() * sizeof(wchar_t)), &wrote, nullptr);
    CloseHandle(h);
}

static void LoadPos(int& x, int& y) {
    std::wstring text = ReadFileText(g_posPath, 256);
    if (!text.empty()) {
        int px = 0, py = 0;
        if (swscanf(text.c_str(), L"%d %d", &px, &py) == 2) {
            x = px;
            y = py;
        }
    }
}

static void ClampToWorkArea(int& x, int& y, int w, int h) {
    POINT pt{ x + w / 2, y + h / 2 };
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return;
    RECT wa = mi.rcWork;
    if (x < wa.left) x = wa.left;
    if (y < wa.top) y = wa.top;
    if (x + w > wa.right) x = wa.right - w;
    if (y + h > wa.bottom) y = wa.bottom - h;
}

static void RecreateSurface(HWND hwnd) {
    delete g_gp;
    delete g_gpBmp;
    g_gp = nullptr;
    g_gpBmp = nullptr;
    if (g_memDC && g_oldBmp) SelectObject(g_memDC, g_oldBmp);
    if (g_bmp) DeleteObject(g_bmp);
    if (g_memDC) DeleteDC(g_memDC);
    g_bmp = nullptr;
    g_memDC = nullptr;
    g_oldBmp = nullptr;
    g_bits = nullptr;

    g_surfW = (int)lround(BASE_W * g_scale);
    g_surfH = (int)lround(BASE_H * g_scale);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = g_surfW;
    bi.bmiHeader.biHeight = -g_surfH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    g_memDC = CreateCompatibleDC(screen);
    g_bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &g_bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!g_memDC || !g_bmp) return;
    g_oldBmp = (HBITMAP)SelectObject(g_memDC, g_bmp);
    g_gpBmp = new Bitmap(g_surfW, g_surfH, g_surfW * 4, PixelFormat32bppPARGB, (BYTE*)g_bits);
    g_gp = new Graphics(g_gpBmp);
    g_gp->SetSmoothingMode(SmoothingModeAntiAlias);
    g_gp->SetTextRenderingHint(TextRenderingHintAntiAlias);
    g_gp->SetCompositingMode(CompositingModeSourceOver);
}

static float Bob() {
    double t = Now();
    if (g_state.status == L"working") return (float)(sin(t * 3.4) * 3.5);
    if (g_state.status == L"waiting") return (float)(sin(t * 6.0) * 1.0);
    return (float)(sin(t * 1.2) * 1.2);
}

static bool Blinking() {
    double t = Now();
    return fmod(t, 4.8) < 0.13;
}

static Color StatusColor() {
    if (g_state.status == L"working") return Color(255, 76, 157, 255);
    if (g_state.status == L"waiting") return Color(255, 255, 168, 38);
    return Color(255, 56, 200, 160);
}

static Color Shade(Color c, float factor) {
    int r = (int)(c.GetR() * factor);
    int g = (int)(c.GetG() * factor);
    int b = (int)(c.GetB() * factor);
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return Color(255, (BYTE)r, (BYTE)g, (BYTE)b);
}

static void MakeRoundedRect(GraphicsPath& p, const RectF& r, float rad) {
    float d = rad * 2;
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    p.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    p.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
    p.CloseFigure();
}

static void DrawCentered(Graphics& g, const std::wstring& text, const RectF& rect,
                         Font& font, Brush& brush) {
    StringFormat fmt;
    fmt.SetAlignment(StringAlignmentCenter);
    fmt.SetLineAlignment(StringAlignmentCenter);
    fmt.SetTrimming(StringTrimmingEllipsisCharacter);
    fmt.SetFormatFlags(StringFormatFlagsNoWrap);
    g.DrawString(text.c_str(), -1, &font, rect, &fmt, &brush);
}

static void DrawPet(HWND hwnd) {
    if (!g_gp) return;
    Graphics& g = *g_gp;
    g.ResetTransform();
    g.Clear(Color(0, 0, 0, 0));
    g.ScaleTransform(g_scale, g_scale);

    float bob = Bob();
    double t = Now();
    Color main = StatusColor();
    bool working = g_state.status == L"working";
    bool waiting = g_state.status == L"waiting";
    bool blink = Blinking() && !waiting;

    // hover state is derived from the cursor so transparent gaps stay click-through
    POINT cur{};
    GetCursorPos(&cur);
    ScreenToClient(hwnd, &cur);
    float lx = cur.x / g_scale, ly = cur.y / g_scale;
    float pdx = lx - PET_CX, pdy = ly - (PET_CY + bob);
    bool inPet = (pdx * pdx + pdy * pdy) <= (R + 14) * (R + 14);
    RectF bubble(18, 14, 344, 142);
    bool inBubble = g_bubbleLatch && lx >= bubble.X && lx <= bubble.GetRight() &&
                    ly >= bubble.Y && ly <= bubble.GetBottom();
    bool showBubble = waiting || inPet || inBubble;
    g_bubbleLatch = showBubble;

    if (showBubble) {
        Color border = main;
        border = Color(150, border.GetR(), border.GetG(), border.GetB());
        SolidBrush fill(Color(246, 255, 255, 255));
        Pen pen(border, 1.4f);
        GraphicsPath box;
        MakeRoundedRect(box, bubble, 16.0f);
        g.FillPath(&fill, &box);
        g.DrawPath(&pen, &box);
        PointF tail[3] = { PointF(PET_CX - 11, bubble.GetBottom() - 1),
                           PointF(PET_CX + 11, bubble.GetBottom() - 1),
                           PointF(PET_CX, bubble.GetBottom() + 15) };
        g.FillPolygon(&fill, tail, 3);

        Font fTitle(g_family, 12.0f, FontStyleRegular, UnitPixel);
        Font fStatus(g_family, 11.0f, FontStyleBold, UnitPixel);
        Font fBody(g_family, 14.0f, FontStyleBold, UnitPixel);
        Font fSmall(g_family, 11.0f, FontStyleRegular, UnitPixel);

        SolidBrush grey(Color(255, 107, 114, 128));
        SolidBrush dark(Color(255, 31, 41, 55));
        SolidBrush faint(Color(255, 156, 163, 175));
        SolidBrush accent(main);

        std::wstring title = g_state.title.empty() ? L"dsh" : g_state.title;
        g.DrawString(title.c_str(), -1, &fTitle, RectF(34, 24, 240, 18), nullptr, &grey);

        std::wstring label = waiting ? L"等待确认" : (working ? L"工作中" : L"空闲");
        StringFormat right;
        right.SetAlignment(StringAlignmentFar);
        right.SetFormatFlags(StringFormatFlagsNoWrap);
        right.SetTrimming(StringTrimmingEllipsisCharacter);
        g.DrawString(label.c_str(), -1, &fStatus, RectF(250, 24, 94, 18), &right, &accent);

        Pen sep(Color(255, 238, 240, 243), 1.0f);
        g.DrawLine(&sep, 34.0f, 47.0f, 346.0f, 47.0f);

        std::wstring body = g_state.current;
        if (body.empty()) body = waiting ? L"有操作等待你确认" : L"暂无进行中的任务";
        StringFormat wrap;
        wrap.SetTrimming(StringTrimmingEllipsisCharacter);
        wrap.SetFormatFlags(StringFormatFlagsLineLimit);
        g.DrawString(body.c_str(), -1, &fBody, RectF(34, 55, 312, 42), &wrap,
                     g_state.current.empty() ? (Brush*)&faint : (Brush*)&dark);

        std::wstring progress = g_state.total > 0
            ? (L"进度 " + std::to_wstring(g_state.done) + L"/" + std::to_wstring(g_state.total))
            : L"暂无任务清单";
        g.DrawString(progress.c_str(), -1, &fSmall, RectF(34, 103, 200, 16), nullptr, &grey);

        StringFormat right2;
        right2.SetAlignment(StringAlignmentFar);
        right2.SetFormatFlags(StringFormatFlagsNoWrap);
        g.DrawString(L"点击打开 dsh", -1, &fSmall, RectF(206, 103, 140, 16), &right2, &faint);

        RectF track(34, 124, 312, 6);
        GraphicsPath tpath;
        MakeRoundedRect(tpath, track, 3.0f);
        SolidBrush trackBrush(Color(255, 229, 231, 235));
        g.FillPath(&trackBrush, &tpath);
        if (g_state.total > 0 && g_state.done > 0) {
            float frac = (float)g_state.done / (float)g_state.total;
            if (frac > 1.0f) frac = 1.0f;
            RectF fillRect(34, 124, 312 * frac, 6);
            GraphicsPath fpath;
            MakeRoundedRect(fpath, fillRect, 3.0f);
            g.FillPath(&accent, &fpath);
        }
    }

    // shadow
    SolidBrush shadow(Color(38, 20, 30, 40));
    g.FillEllipse(&shadow, RectF(PET_CX - 46, PET_CY + 44 + bob * 0.3f, 92, 18));

    if (waiting) {
        int alpha = (int)(60 + 45 * (0.5 + 0.5 * sin(t * 4.0)));
        Pen halo(Color(alpha, main.GetR(), main.GetG(), main.GetB()), 3.0f);
        float hr = R + 20 + (float)(sin(t * 4.0) * 3.0);
        g.DrawEllipse(&halo, RectF(PET_CX - hr, PET_CY - hr + bob, hr * 2, hr * 2));
    }

    // antenna
    Pen stem(Shade(main, 0.8f), 3.0f);
    stem.SetStartCap(LineCapRound);
    stem.SetEndCap(LineCapRound);
    float sway = (float)(sin(t * 2.2) * 3.0);
    g.DrawLine(&stem, PET_CX, PET_CY - R + 6 + bob, PET_CX + 8 + sway, PET_CY - R - 12 + bob);
    SolidBrush bulb(Color(255, 255, 255, 255));
    SolidBrush bulbCore(Shade(main, 0.9f));
    g.FillEllipse(&bulb, RectF(PET_CX + 4 + sway, PET_CY - R - 20 + bob, 12, 12));
    g.FillEllipse(&bulbCore, RectF(PET_CX + 7 + sway, PET_CY - R - 17 + bob, 6, 6));

    // body
    LinearGradientBrush body(PointF(PET_CX, PET_CY - R + bob), PointF(PET_CX, PET_CY + R + bob),
                             Shade(main, 1.16f), Shade(main, 0.86f));
    g.FillEllipse(&body, RectF(PET_CX - R, PET_CY - R + bob, R * 2, R * 2));
    SolidBrush shine(Color(70, 255, 255, 255));
    g.FillEllipse(&shine, RectF(PET_CX - R * 0.58f, PET_CY - R * 0.82f + bob, R * 0.72f, R * 0.42f));

    // eyes
    float eyeH = blink ? 2.5f : 17.0f;
    float eyeY = PET_CY - 10 + bob - (blink ? 0.0f : eyeH * 0.5f);
    SolidBrush white(Color(255, 255, 255, 255));
    SolidBrush pupil(Color(255, 32, 38, 52));
    g.FillEllipse(&white, RectF(PET_CX - 27, eyeY, 14, eyeH));
    g.FillEllipse(&white, RectF(PET_CX + 13, eyeY, 14, eyeH));
    if (!blink) {
        g.FillEllipse(&pupil, RectF(PET_CX - 23.5f, eyeY + 4.5f, 7, 9));
        g.FillEllipse(&pupil, RectF(PET_CX + 16.5f, eyeY + 4.5f, 7, 9));
    }

    // cheeks
    SolidBrush cheeks(Color(70, 255, 110, 130));
    g.FillEllipse(&cheeks, RectF(PET_CX - 44, PET_CY + 8 + bob, 16, 10));
    g.FillEllipse(&cheeks, RectF(PET_CX + 28, PET_CY + 8 + bob, 16, 10));

    // mouth
    if (working) {
        g.FillEllipse(&pupil, RectF(PET_CX - 6, PET_CY + 13 + bob, 12, 9));
    } else if (waiting) {
        g.FillEllipse(&pupil, RectF(PET_CX - 5, PET_CY + 14 + bob, 10, 9));
    } else {
        Pen smile(Color(255, 32, 38, 52), 3.2f);
        smile.SetStartCap(LineCapRound);
        smile.SetEndCap(LineCapRound);
        g.DrawArc(&smile, RectF(PET_CX - 11, PET_CY + 11 + bob, 22, 13), 20.0f, 140.0f);
    }

    // progress ring
    float ringR = R + 14;
    RectF ringRect(PET_CX - ringR, PET_CY - ringR + bob, ringR * 2, ringR * 2);
    Pen track(Color(60, 0, 0, 0), 7.0f);
    g.DrawEllipse(&track, ringRect);
    if (g_state.total > 0 && g_state.done > 0) {
        float sweep = 360.0f * (float)g_state.done / (float)g_state.total;
        if (sweep > 360.0f) sweep = 360.0f;
        Pen arc(main, 7.0f);
        arc.SetStartCap(LineCapRound);
        arc.SetEndCap(LineCapRound);
        g.DrawArc(&arc, ringRect, -90.0f, sweep);
    }

    if (g_state.total > 0) {
        RectF pill(PET_CX + 28, PET_CY + 42 + bob, 62, 20);
        GraphicsPath pp;
        MakeRoundedRect(pp, pill, 10.0f);
        SolidBrush pfill(Color(150, 17, 24, 39));
        g.FillPath(&pfill, &pp);
        Font fNum(g_family, 11.0f, FontStyleBold, UnitPixel);
        SolidBrush white2(Color(255, 255, 255, 255));
        DrawCentered(g, std::to_wstring(g_state.done) + L"/" + std::to_wstring(g_state.total),
                     pill, fNum, white2);
    }

    if (waiting) {
        float pr = 13 + (float)(sin(t * 5.0) * 1.5);
        SolidBrush badge(main);
        g.FillEllipse(&badge, RectF(PET_CX + 46 - pr, PET_CY - 48 + bob - pr, pr * 2, pr * 2));
        Font fMark(g_family, 15.0f, FontStyleBold, UnitPixel);
        DrawCentered(g, L"!", RectF(PET_CX + 46 - pr, PET_CY - 48 + bob - pr, pr * 2, pr * 2), fMark, white);
    }

    RECT rc{};
    GetWindowRect(hwnd, &rc);
    POINT dst{ rc.left, rc.top };
    SIZE size{ g_surfW, g_surfH };
    POINT src{ 0, 0 };
    BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(hwnd, nullptr, &dst, &size, g_memDC, &src, 0, &blend, ULW_ALPHA);
}

static void OpenDsh() {
    std::wstring url = g_state.url.empty() ? L"http://127.0.0.1:3080/" : g_state.url;
    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

static void ExitPet(HWND hwnd) {
    SavePos(hwnd);
    DestroyWindow(hwnd);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        g_dragging = true;
        g_moved = false;
        g_dragFrom.x = GET_X_LPARAM(lp);
        g_dragFrom.y = GET_Y_LPARAM(lp);
        POINT p{ g_dragFrom.x, g_dragFrom.y };
        ClientToScreen(hwnd, &p);
        RECT rc{};
        GetWindowRect(hwnd, &rc);
        g_winFrom.x = rc.left;
        g_winFrom.y = rc.top;
        SetCapture(hwnd);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (g_dragging) {
            POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ClientToScreen(hwnd, &p);
            int dx = p.x - (g_winFrom.x + g_dragFrom.x);
            int dy = p.y - (g_winFrom.y + g_dragFrom.y);
            if (!g_moved && (abs(dx) > 4 || abs(dy) > 4)) g_moved = true;
            if (g_moved) SetWindowPos(hwnd, HWND_TOPMOST, g_winFrom.x + dx, g_winFrom.y + dy,
                                      0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g_dragging) {
            g_dragging = false;
            ReleaseCapture();
            if (g_moved) SavePos(hwnd);
            else OpenDsh();
        }
        return 0;
    }
    case WM_RBUTTONUP: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ClientToScreen(hwnd, &p);
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 1, L"打开 dsh");
        AppendMenuW(menu, MF_STRING, 2, L"刷新状态");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 3, L"退出桌宠");
        SetForegroundWindow(hwnd);
        int cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                      p.x, p.y, 0, hwnd, nullptr);
        PostMessage(hwnd, WM_NULL, 0, 0);
        DestroyMenu(menu);
        if (cmd == 1) OpenDsh();
        else if (cmd == 2) ReloadState(true);
        else if (cmd == 3) ExitPet(hwnd);
        return 0;
    }
    case WM_TIMER: {
        ULONGLONG now = GetTickCount64();
        if (now - g_lastPoll >= 200) {
            g_lastPoll = now;
            ReloadState(false);
        }
        if (now - g_lastHostCheck >= 2000) {
            g_lastHostCheck = now;
            if (g_state.loaded) {
                bool alive = false;
                if (g_state.host != 0) {
                    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, g_state.host);
                    if (h) {
                        alive = WaitForSingleObject(h, 0) != WAIT_OBJECT_0;
                        CloseHandle(h);
                    }
                }
                if (alive) {
                    g_deadSince = 0;
                } else {
                    if (g_deadSince == 0) g_deadSince = now;
                    else if (now - g_deadSince > 6000) {
                        ExitPet(hwnd);
                        return 0;
                    }
                }
            }
        }
        DrawPet(hwnd);
        return 0;
    }
    case WM_DPICHANGED: {
        UINT dpi = HIWORD(wp);
        if (dpi == 0) dpi = 96;
        g_scale = dpi / 96.0f;
        RecreateSurface(hwnd);
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, g_surfW, g_surfH,
                     SWP_NOMOVE | SWP_NOACTIVATE);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static UINT GetDpiForWindowSafe(HWND hwnd) {
    typedef UINT(WINAPI* Fn)(HWND);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        Fn fn = (Fn)GetProcAddress(user32, "GetDpiForWindow");
        if (fn) return fn(hwnd);
    }
    HDC dc = GetDC(nullptr);
    UINT dpi = 96;
    if (dc) {
        dpi = GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(nullptr, dc);
    }
    return dpi ? dpi : 96;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int) {
    HANDLE single = CreateMutexW(nullptr, FALSE, L"Local\\DshPet.SingleInstance");
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(single);
        return 0;
    }

    typedef BOOL(WINAPI* DpiFn)(HANDLE);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        DpiFn setCtx = (DpiFn)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (setCtx) setCtx((HANDLE)-4);
        else SetProcessDPIAware();
    }

    GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    if (GdiplusStartup(&token, &gsi, nullptr) != Ok) return 1;

    static FontFamily yahei(L"Microsoft YaHei UI");
    if (yahei.IsAvailable()) g_family = &yahei;
    else {
        static FontFamily segoe(L"Segoe UI");
        g_family = &segoe;
    }

    wchar_t stateEnv[2048];
    DWORD n = GetEnvironmentVariableW(L"DSH_PET_STATE", stateEnv, 2048);
    if (n > 0) {
        g_statePath = stateEnv;
    } else {
        wchar_t profile[1024];
        DWORD m = GetEnvironmentVariableW(L"USERPROFILE", profile, 1024);
        std::wstring home = m > 0 ? std::wstring(profile, m) : ExeDir();
        g_statePath = Join(Join(Join(home, L".dsh"), L"dsh-pet"), L"state.txt");
    }
    size_t slash = g_statePath.find_last_of(L"\\/");
    std::wstring dir = slash == std::wstring::npos ? std::wstring(L".") : g_statePath.substr(0, slash);
    g_posPath = Join(dir, L"pos.txt");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32649));
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) {
        GdiplusShutdown(token);
        return 1;
    }

    int x = -1, y = -1;
    LoadPos(x, y);
    POINT pt{ 0, 0 };
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    int w = (int)lround(BASE_W * g_scale);
    int h = (int)lround(BASE_H * g_scale);
    if (x < -10000 || y < -10000) x = y = -1;
    if (x == -1) {
        x = mi.rcWork.right - w - 12;
        y = mi.rcWork.bottom - h - 6;
    }
    ClampToWorkArea(x, y, w, h);

    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kClassName, L"DshPet", WS_POPUP,
        x, y, w, h, nullptr, nullptr, inst, nullptr);
    if (!hwnd) {
        GdiplusShutdown(token);
        if (single) CloseHandle(single);
        return 1;
    }

    g_scale = GetDpiForWindowSafe(hwnd) / 96.0f;
    w = (int)lround(BASE_W * g_scale);
    h = (int)lround(BASE_H * g_scale);
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
    RecreateSurface(hwnd);
    ReloadState(true);

    g_t0 = GetTickCount64();
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetTimer(hwnd, 1, 40, nullptr);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    delete g_gp;
    delete g_gpBmp;
    if (g_memDC && g_oldBmp) SelectObject(g_memDC, g_oldBmp);
    if (g_bmp) DeleteObject(g_bmp);
    if (g_memDC) DeleteDC(g_memDC);
    GdiplusShutdown(token);
    if (single) CloseHandle(single);
    return 0;
}
