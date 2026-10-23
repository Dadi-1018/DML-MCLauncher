// ============================================================================
//  Dream Minecraft Launcher (DML) —— 极简 Minecraft 启动器示例
//  ---------------------------------------------------------------------------
//  · 纯 Win32 API + GDI+ 手绘界面，无任何第三方 UI 库，单文件实现
//  · 无边框圆角窗口、自绘输入框 / 下拉框 / 滑块 / 按钮 / 日志面板
//  · 配置持久化（%APPDATA%\DreamMinecraftLauncher\config.ini）
//  · 自动探测 .minecraft 目录、已安装版本、Java 运行时
//  · 启动流程为「示例实现」：构建真实命令行并模拟进度，不真正拉起游戏
//    （真实启动的接入点见 BuildLaunchCommand() 上方的注释）
//
//  编译方式见 README.md
// ============================================================================

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <windowsx.h>
#include <shlobj.h>
#include <gdiplus.h>

#include "resource.h"   // IDI_APPICON / IDR_LOGO

#include <stdlib.h>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>
#include <functional>
#include <cmath>

using namespace Gdiplus;

#ifdef _MSC_VER
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#endif

// ============================================================================
//  主题配色（深色，贴近现代启动器观感）
// ============================================================================
namespace Theme {
    const DWORD Bg          = RGB(0x14, 0x16, 0x1A);
    const DWORD Panel       = RGB(0x1B, 0x1E, 0x24);
    const DWORD PanelBorder = RGB(0x28, 0x2D, 0x36);
    const DWORD Field       = RGB(0x23, 0x27, 0x2E);
    const DWORD FieldHover  = RGB(0x2B, 0x30, 0x39);
    const DWORD Border      = RGB(0x2E, 0x33, 0x3C);
    const DWORD Text        = RGB(0xE7, 0xEA, 0xF0);
    const DWORD Muted       = RGB(0x8B, 0x93, 0x9F);
    const DWORD Dim         = RGB(0x5B, 0x63, 0x6F);
    const DWORD Accent      = RGB(0x4A, 0xDE, 0x80);
    const DWORD AccentSoft  = RGB(0x2F, 0x8F, 0x57);
    const DWORD OnAccent    = RGB(0x0B, 0x14, 0x0F);
    const DWORD Grass       = RGB(0x54, 0xA6, 0x4B);
    const DWORD Dirt        = RGB(0x8B, 0x6A, 0x4A);
    const DWORD Track       = RGB(0x2A, 0x2F, 0x37);
    const DWORD Danger      = RGB(0xE0, 0x4C, 0x4C);
}

static Color C(DWORD rgb, BYTE a = 255) {
    return Color(a, GetRValue(rgb), GetGValue(rgb), GetBValue(rgb));
}

// ============================================================================
//  DPI 缩放 & 字符串工具
// ============================================================================
static float g_scale = 1.0f;

static inline int  S(int v)   { return (int)std::lround((double)v * g_scale); }
static inline REAL SR(REAL v) { return v * g_scale; }

static std::string W2U8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring U82W(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static bool PathExists(const std::wstring& p) {
    return !p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// ============================================================================
//  GDI+ 绘图小工具
// ============================================================================
static void AddRoundRect(GraphicsPath& path, const RectF& r, REAL radius) {
    REAL d = radius * 2.0f;
    if (d > r.Width)  d = r.Width;
    if (d > r.Height) d = r.Height;
    path.Reset();
    path.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.0f, 90.0f);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

static void FillRound(Graphics& gr, const RectF& r, REAL radius, const Color& col) {
    GraphicsPath p;
    AddRoundRect(p, r, radius);
    SolidBrush b(col);
    gr.FillPath(&b, &p);
}

static void StrokeRound(Graphics& gr, const RectF& r, REAL radius, const Color& col, REAL w = 1.0f) {
    GraphicsPath p;
    AddRoundRect(p, r, radius);
    Pen pen(col, w);
    gr.DrawPath(&pen, &p);
}

static void DrawStr(Graphics& gr, const std::wstring& s, const RectF& rc, Font& f, const Color& col,
                    StringAlignment align = StringAlignmentNear, bool ellipsis = false, bool vcenter = true) {
    StringFormat fmt;
    fmt.SetAlignment(align);
    fmt.SetLineAlignment(vcenter ? StringAlignmentCenter : StringAlignmentNear);
    fmt.SetFormatFlags(StringFormatFlagsNoWrap);
    if (ellipsis) fmt.SetTrimming(StringTrimmingEllipsisCharacter);
    SolidBrush b(col);
    gr.DrawString(s.c_str(), (INT)s.size(), &f, rc, &fmt, &b);
}

static REAL MeasureW(Graphics& gr, const std::wstring& s, Font& f) {
    StringFormat fmt;
    fmt.SetFormatFlags(StringFormatFlagsNoWrap);
    RectF box(0, 0, 0, 0);
    gr.MeasureString(s.c_str(), (INT)s.size(), &f, PointF(0, 0), &fmt, &box);
    return box.Width;
}

// ============================================================================
//  字体
// ============================================================================
struct FontSet {
    Font* title = nullptr;
    Font* sub   = nullptr;
    Font* label = nullptr;
    Font* input = nullptr;
    Font* btn   = nullptr;
    Font* log   = nullptr;
    Font* value = nullptr;
} F;

static void InitFonts() {
    F.title = new Font(L"Microsoft YaHei UI", SR(19.0f), FontStyleBold,    UnitPixel);
    F.sub   = new Font(L"Microsoft YaHei UI", SR(11.5f), FontStyleRegular, UnitPixel);
    F.label = new Font(L"Microsoft YaHei UI", SR(11.5f), FontStyleRegular, UnitPixel);
    F.input = new Font(L"Microsoft YaHei UI", SR(14.5f), FontStyleRegular, UnitPixel);
    F.btn   = new Font(L"Microsoft YaHei UI", SR(15.5f), FontStyleBold,    UnitPixel);
    F.log   = new Font(L"Microsoft YaHei UI", SR(11.0f), FontStyleRegular, UnitPixel);
    F.value = new Font(L"Microsoft YaHei UI", SR(12.5f), FontStyleBold,    UnitPixel);
}

static void FreeFonts() {
    delete F.title; delete F.sub; delete F.label;
    delete F.input; delete F.btn; delete F.log; delete F.value;
}

// ============================================================================
//  布局（逻辑像素 760 x 500，按系统 DPI 等比放大）
// ============================================================================
struct Layout {
    RectF logo;
    RectF panel1;
    RectF panel2;
    RectF userBox;
    RectF verBox;
    RectF sliderTrack;
    RectF browseBtn;
    RectF launchBtn;
    RectF logArea;
    RectF closeBtn;
    RectF minBtn;
};

static const int WIN_W = 760;
static const int WIN_H = 500;

static Layout ComputeLayout() {
    Layout L;
    L.logo        = RectF(S(28),  S(20),  S(40),  S(40));
    L.minBtn      = RectF(S(662), S(16),  S(30),  S(30));
    L.closeBtn    = RectF(S(700), S(16),  S(30),  S(30));
    L.panel1      = RectF(S(28),  S(82),  S(704), S(166));
    L.userBox     = RectF(S(52),  S(126), S(300), S(42));
    L.verBox      = RectF(S(376), S(126), S(332), S(42));
    L.sliderTrack = RectF(S(52),  S(210), S(656), S(6));
    L.panel2      = RectF(S(28),  S(262), S(704), S(66));
    L.browseBtn   = RectF(S(636), S(278), S(72),  S(34));
    L.launchBtn   = RectF(S(28),  S(340), S(704), S(50));
    L.logArea     = RectF(S(28),  S(406), S(704), S(64));
    return L;
}

// ============================================================================
//  应用状态
// ============================================================================
enum { HIT_NONE = 0, HIT_CLOSE, HIT_MIN, HIT_USER, HIT_VER, HIT_SLIDER, HIT_BROWSE, HIT_LAUNCH };
enum { FOCUS_NONE = 0, FOCUS_USER };

static const int DD_ITEM_H   = 34;   // 下拉项高度（逻辑像素）
static const int DD_MAX_SHOW = 6;    // 下拉最多显示条数

struct AppState {
    HWND hwnd = nullptr;

    std::wstring username = L"Steve";
    std::wstring gameDir;
    std::wstring javaPath;
    std::vector<std::wstring> versions;
    int  versionIdx = 0;
    int  memGb      = 4;              // 1 ~ 16 GB
    bool realScan   = false;          // 是否探测到真实 .minecraft

    std::vector<std::wstring> logs;

    int  focus     = FOCUS_NONE;
    bool caretOn   = true;
    int  caretTick = 0;
    int  hover     = HIT_NONE;

    bool ddOpen   = false;
    int  ddHover  = -1;
    int  ddScroll = 0;

    bool draggingSlider = false;

    bool  launching = false;
    bool  launched  = false;
    float progress  = 0.0f;
    int   step      = 0;
    int   resetTick = 0;
};

static AppState g_app;

// ============================================================================
//  路径 / 配置
// ============================================================================
static std::wstring ExeDir() {
    wchar_t buf[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf);
    size_t i = p.find_last_of(L'\\');
    return (i == std::wstring::npos) ? p : p.substr(0, i);
}

static std::wstring AppDataConfigDir() {
    wchar_t buf[MAX_PATH] = {0};
    if (!GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH)) return ExeDir();
    return std::wstring(buf) + L"\\DreamMinecraftLauncher";
}

static std::wstring ConfigPath() { return AppDataConfigDir() + L"\\config.ini"; }

static std::wstring DefaultMinecraftDir() {
    wchar_t buf[MAX_PATH] = {0};
    if (GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH)) {
        std::wstring p = std::wstring(buf) + L"\\.minecraft";
        if (PathExists(p)) return p;
    }
    return L"";
}

static void SaveConfig() {
    std::wstring dir = AppDataConfigDir();
    CreateDirectoryW(dir.c_str(), nullptr);
    std::ofstream f(W2U8(ConfigPath()).c_str());
    if (!f) return;
    f << "username=" << W2U8(g_app.username) << "\n";
    f << "version="  << W2U8(g_app.versions.empty() ? std::wstring() : g_app.versions[(size_t)g_app.versionIdx]) << "\n";
    f << "memory="   << g_app.memGb << "\n";
    f << "gamedir="  << W2U8(g_app.gameDir) << "\n";
}

static void LoadConfig(std::wstring& outVersion) {
    std::ifstream f(W2U8(ConfigPath()).c_str());
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string  k = line.substr(0, eq);
        std::wstring v = U82W(line.substr(eq + 1));
        if (k == "username")      g_app.username = v;
        else if (k == "version")  outVersion = v;
        else if (k == "memory")   g_app.memGb = std::max(1, std::min(16, (int)wcstol(v.c_str(), nullptr, 10)));
        else if (k == "gamedir")  g_app.gameDir = v;
    }
}

// ============================================================================
//  环境探测：版本列表 / Java
// ============================================================================
static std::vector<std::wstring> ScanVersions(const std::wstring& dir) {
    std::vector<std::wstring> out;
    if (dir.empty()) return out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\versions\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (PathExists(dir + L"\\versions\\" + n + L"\\" + n + L".json"))
            out.push_back(n);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), std::greater<std::wstring>());
    return out;
}

static std::vector<std::wstring> FallbackVersions() {
    std::vector<std::wstring> v;
    v.push_back(L"1.20.4");
    v.push_back(L"1.20.1");
    v.push_back(L"1.19.4");
    v.push_back(L"1.18.2");
    v.push_back(L"1.16.5");
    v.push_back(L"1.12.2");
    return v;
}

static std::wstring FindJavaUnder(const std::wstring& root) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return L"";
    std::wstring found;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        std::wstring p = root + L"\\" + n + L"\\bin\\java.exe";
        if (PathExists(p)) { found = p; break; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

static std::wstring DetectJava() {
    wchar_t buf[MAX_PATH] = {0};
    if (GetEnvironmentVariableW(L"JAVA_HOME", buf, MAX_PATH)) {
        std::wstring p = std::wstring(buf) + L"\\bin\\java.exe";
        if (PathExists(p)) return p;
    }
    const wchar_t* roots[] = {
        L"C:\\Program Files\\Java",
        L"C:\\Program Files (x86)\\Java",
        L"C:\\Program Files\\Eclipse Adoptium",
        L"C:\\Program Files\\Microsoft"
    };
    for (int i = 0; i < 4; ++i) {
        std::wstring p = FindJavaUnder(roots[i]);
        if (!p.empty()) return p;
    }
    return L"";
}

// ============================================================================
//  日志
// ============================================================================
static void PushLog(const std::wstring& s) {
    g_app.logs.push_back(s);
    while (g_app.logs.size() > 4) g_app.logs.erase(g_app.logs.begin());
}

// ============================================================================
//  启动流程（示例）
//  ---------------------------------------------------------------------------
//  ★ 真实启动接入点：
//    把 BuildLaunchCommand() 的命令行交给 CreateProcessW 即可拉起 Java。
//    还需自行补齐：登录鉴权、version.json 继承解析、library 下载、
//    natives 解压、assets 索引校验。示例只做命令行拼装 + 进度模拟，
//    以免庞大的下载逻辑干扰 UI 演示。
//
//      STARTUPINFOW si{ sizeof(si) };
//      PROCESS_INFORMATION pi{};
//      CreateProcessW(g_app.javaPath.c_str(), cmdBuf, nullptr, nullptr, FALSE,
//                     CREATE_NO_WINDOW, nullptr, g_app.gameDir.c_str(), &si, &pi);
// ============================================================================
static std::wstring BuildLaunchCommand() {
    std::wstring ver  = g_app.versions.empty() ? L"1.20.4" : g_app.versions[(size_t)g_app.versionIdx];
    std::wstring java = g_app.javaPath.empty() ? L"javaw.exe" : g_app.javaPath;
    std::wstring dir  = g_app.gameDir.empty() ? DefaultMinecraftDir() : g_app.gameDir;
    std::wstring user = g_app.username.empty() ? L"Player" : g_app.username;

    std::wstring cmd;
    cmd += L"\"" + java + L"\" ";
    cmd += L"-Xmx" + std::to_wstring(g_app.memGb) + L"G ";
    cmd += L"-Xms" + std::to_wstring(std::max(1, g_app.memGb / 2)) + L"G ";
    cmd += L"-Djava.library.path=\"" + dir + L"\\versions\\" + ver + L"\\natives\" ";
    cmd += L"-cp \"libraries/*;versions\\" + ver + L"\\" + ver + L".jar\" ";
    cmd += L"net.minecraft.client.main.Main ";
    cmd += L"--username " + user + L" ";
    cmd += L"--version " + ver + L" ";
    cmd += L"--gameDir \"" + dir + L"\" ";
    cmd += L"--assetsDir \"" + dir + L"\\assets\" ";
    cmd += L"--accessToken 0 --userType legacy --versionType release";
    return cmd;
}

static const wchar_t* kLaunchSteps[] = {
    L"校验账户信息 …",
    L"读取版本清单 version.json …",
    L"检查资源索引 assets …",
    L"整理类路径 libraries …",
    L"启动 Java 进程 …"
};

static void StartLaunch() {
    if (g_app.launching) return;
    if (g_app.username.empty()) {
        PushLog(L"请先填写玩家昵称");
        g_app.focus = FOCUS_USER;
        return;
    }
    g_app.launching = true;
    g_app.launched  = false;
    g_app.progress  = 0.0f;
    g_app.step      = 0;
    g_app.logs.clear();
    PushLog(kLaunchSteps[0]);
}

static void FinishLaunch() {
    g_app.launching = false;
    g_app.launched  = true;
    g_app.resetTick = 30;    // 约 3 秒后按钮复位
    PushLog(L"启动完成 · 已生成启动命令行（示例流程）");
    PushLog(BuildLaunchCommand());
    SaveConfig();
}

// ============================================================================
//  绘制
// ============================================================================
static void DrawWindowButtons(Graphics& gr, const Layout& L) {
    struct Item { RectF r; bool hover; bool close; };
    Item items[2] = {
        { L.minBtn,   g_app.hover == HIT_MIN,   false },
        { L.closeBtn, g_app.hover == HIT_CLOSE, true  }
    };
    for (int i = 0; i < 2; ++i) {
        const RectF& r = items[i].r;
        if (items[i].hover) FillRound(gr, r, SR(8), C(items[i].close ? Theme::Danger : Theme::FieldHover));
        Pen p(C(items[i].hover && items[i].close ? RGB(0xFF, 0xFF, 0xFF) : Theme::Muted), SR(1.4f));
        p.SetStartCap(LineCapRound);
        p.SetEndCap(LineCapRound);
        REAL cx = r.X + r.Width / 2, cy = r.Y + r.Height / 2, d = SR(4.5f);
        if (items[i].close) {
            gr.DrawLine(&p, cx - d, cy - d, cx + d, cy + d);
            gr.DrawLine(&p, cx + d, cy - d, cx - d, cy + d);
        } else {
            gr.DrawLine(&p, cx - d, cy, cx + d, cy);
        }
    }
}

// ---------------------------------------------------------------------------
//  品牌标志：优先使用内嵌的 dml-logo-256.png（RT_RCDATA），失败则回退手绘草方块
// ---------------------------------------------------------------------------
static Image*   g_logoImg    = nullptr;
static IStream* g_logoStream = nullptr;

static bool LoadLogoResource(HINSTANCE hInst) {
    HRSRC hr = FindResourceW(hInst, MAKEINTRESOURCEW(IDR_LOGO), RT_RCDATA);
    if (!hr) return false;
    DWORD size = SizeofResource(hInst, hr);
    if (size == 0) return false;
    HGLOBAL hres = LoadResource(hInst, hr);
    if (!hres) return false;
    void* src = LockResource(hres);
    if (!src) return false;

    // GDI+ 需要一个持续存在的 IStream，因此把资源复制到堆内存再交给它托管
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!mem) return false;
    void* dst = GlobalLock(mem);
    if (!dst) { GlobalFree(mem); return false; }
    memcpy(dst, src, size);
    GlobalUnlock(mem);

    if (CreateStreamOnHGlobal(mem, TRUE, &g_logoStream) != S_OK) {
        GlobalFree(mem);
        return false;
    }
    g_logoImg = Image::FromStream(g_logoStream, FALSE);
    if (!g_logoImg || g_logoImg->GetLastStatus() != Ok) {
        delete g_logoImg;
        g_logoImg = nullptr;
        g_logoStream->Release();
        g_logoStream = nullptr;
        return false;
    }
    return true;
}

static void FreeLogoResource() {
    delete g_logoImg;
    g_logoImg = nullptr;
    if (g_logoStream) { g_logoStream->Release(); g_logoStream = nullptr; }
}

static void DrawLogo(Graphics& gr, const RectF& r) {
    if (g_logoImg) {
        // 标志本身已含圆角与透明边，直接高质量缩放贴入即可
        gr.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        gr.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        gr.DrawImage(g_logoImg, r);
        gr.SetInterpolationMode(InterpolationModeDefault);
        gr.SetPixelOffsetMode(PixelOffsetModeHalf);
        return;
    }

    // ---- 回退：手绘草方块 ----
    GraphicsPath clip;
    AddRoundRect(clip, r, SR(11));
    gr.SetClip(&clip);

    SolidBrush dirt(C(Theme::Dirt));
    gr.FillRectangle(&dirt, r);
    SolidBrush grass(C(Theme::Grass));
    gr.FillRectangle(&grass, RectF(r.X, r.Y, r.Width, r.Height * 0.5f));

    Pen grid(Color(46, 0, 0, 0), SR(1));
    for (int i = 1; i < 3; ++i) {
        REAL x = r.X + r.Width * (REAL)i / 3.0f;
        gr.DrawLine(&grid, x, r.Y, x, r.Y + r.Height);
        REAL y = r.Y + r.Height * (REAL)i / 3.0f;
        gr.DrawLine(&grid, r.X, y, r.X + r.Width, y);
    }
    gr.ResetClip();
    StrokeRound(gr, r, SR(11), Color(40, 255, 255, 255), SR(1));
}

static void DrawFieldBox(Graphics& gr, const RectF& r, bool focused) {
    FillRound(gr, r, SR(10), C(Theme::Field));
    StrokeRound(gr, r, SR(10), C(focused ? Theme::AccentSoft : Theme::Border), SR(focused ? 1.4f : 1.0f));
}

static void Paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT rcWin;
    GetClientRect(hwnd, &rcWin);
    int W = rcWin.right, H = rcWin.bottom;

    HDC     memDC = CreateCompatibleDC(hdc);
    HBITMAP bmp   = CreateCompatibleBitmap(hdc, W, H);
    HBITMAP old   = (HBITMAP)SelectObject(memDC, bmp);

    {
        Graphics gr(memDC);
        gr.SetSmoothingMode(SmoothingModeAntiAlias);
        gr.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        gr.SetPixelOffsetMode(PixelOffsetModeHalf);
        gr.Clear(C(Theme::Bg));

        Layout L = ComputeLayout();

        // ---------------- 头部 ----------------
        DrawLogo(gr, L.logo);
        DrawStr(gr, L"Dream Minecraft Launcher", RectF(S(80), S(14), S(460), S(28)),
                *F.title, C(Theme::Text));
        DrawStr(gr, L"DML · 示例启动器 · C++ / Win32 / GDI+",
                RectF(S(81), S(44), S(460), S(18)), *F.sub, C(Theme::Muted));
        DrawWindowButtons(gr, L);

        // ---------------- 卡片一：账户 / 版本 / 内存 ----------------
        FillRound(gr, L.panel1, SR(14), C(Theme::Panel));
        StrokeRound(gr, L.panel1, SR(14), C(Theme::PanelBorder), SR(1));

        DrawStr(gr, L"玩家昵称", RectF(S(54), S(100), S(240), S(18)), *F.label, C(Theme::Muted));
        DrawStr(gr, L"游戏版本", RectF(S(378), S(100), S(240), S(18)), *F.label, C(Theme::Muted));

        // 昵称输入框
        DrawFieldBox(gr, L.userBox, g_app.focus == FOCUS_USER);
        {
            RectF tr(L.userBox.X + SR(14), L.userBox.Y, L.userBox.Width - SR(28), L.userBox.Height);
            DrawStr(gr, g_app.username, tr, *F.input, C(Theme::Text), StringAlignmentNear, false, true);
            if (g_app.focus == FOCUS_USER && g_app.caretOn) {
                REAL w  = MeasureW(gr, g_app.username, *F.input);
                REAL cx = tr.X + w + SR(1);
                REAL cy = L.userBox.Y + L.userBox.Height / 2;
                Pen caretp(C(Theme::Accent), SR(1.6f));
                gr.DrawLine(&caretp, cx, cy - SR(9), cx, cy + SR(9));
            }
        }

        // 版本下拉框
        DrawFieldBox(gr, L.verBox, g_app.ddOpen);
        {
            std::wstring ver = g_app.versions.empty() ? L"(无可用版本)"
                                                      : g_app.versions[(size_t)g_app.versionIdx];
            DrawStr(gr, ver, RectF(L.verBox.X + SR(14), L.verBox.Y,
                                   L.verBox.Width - SR(52), L.verBox.Height),
                    *F.input, C(Theme::Text), StringAlignmentNear, true, true);
            REAL cx = L.verBox.X + L.verBox.Width - SR(24);
            REAL cy = L.verBox.Y + L.verBox.Height / 2;
            Pen p(C(g_app.ddOpen ? Theme::Accent : Theme::Muted), SR(1.5f));
            p.SetStartCap(LineCapRound);
            p.SetEndCap(LineCapRound);
            gr.DrawLine(&p, cx - SR(5), cy - SR(2), cx, cy + SR(3));
            gr.DrawLine(&p, cx + SR(5), cy - SR(2), cx, cy + SR(3));
        }

        // 内存滑块
        DrawStr(gr, L"内存分配", RectF(S(54), S(178), S(240), S(18)), *F.label, C(Theme::Muted));
        DrawStr(gr, std::to_wstring(g_app.memGb) + L" GB", RectF(S(52), S(176), S(656), S(20)),
                *F.value, C(Theme::Accent), StringAlignmentFar);

        FillRound(gr, L.sliderTrack, SR(3), C(Theme::Track));
        {
            float t  = (float)(g_app.memGb - 1) / 15.0f;
            REAL  kx = L.sliderTrack.X + t * L.sliderTrack.Width;
            REAL  cy = L.sliderTrack.Y + L.sliderTrack.Height / 2;
            REAL  fw = kx - L.sliderTrack.X;
            if (fw < SR(3)) fw = SR(3);
            FillRound(gr, RectF(L.sliderTrack.X, L.sliderTrack.Y, fw, L.sliderTrack.Height),
                      SR(3), C(Theme::Accent));
            SolidBrush white(C(RGB(0xFF, 0xFF, 0xFF)));
            gr.FillEllipse(&white, kx - SR(8.5f), cy - SR(8.5f), SR(17), SR(17));
            Pen ring(C(Theme::AccentSoft), SR(1.4f));
            gr.DrawEllipse(&ring, kx - SR(8.5f), cy - SR(8.5f), SR(17), SR(17));
        }

        // ---------------- 卡片二：游戏目录 ----------------
        FillRound(gr, L.panel2, SR(14), C(Theme::Panel));
        StrokeRound(gr, L.panel2, SR(14), C(Theme::PanelBorder), SR(1));
        DrawStr(gr, L"游戏目录", RectF(S(52), S(272), S(200), S(16)), *F.label, C(Theme::Muted));
        {
            bool ok = PathExists(g_app.gameDir);
            std::wstring path = ok ? g_app.gameDir : L"未找到 .minecraft，点击右侧「浏览…」选择";
            DrawStr(gr, path, RectF(S(52), S(290), S(560), S(20)), *F.sub,
                    C(ok ? Theme::Muted : Theme::Dim), StringAlignmentNear, true);
        }
        {
            bool hov = (g_app.hover == HIT_BROWSE);
            FillRound(gr, L.browseBtn, SR(9), C(hov ? Theme::FieldHover : Theme::Track));
            StrokeRound(gr, L.browseBtn, SR(9), C(Theme::Border), SR(1));
            DrawStr(gr, L"浏览…", L.browseBtn, *F.sub, C(hov ? Theme::Text : Theme::Muted),
                    StringAlignmentCenter, false, true);
        }

        // ---------------- 启动按钮 ----------------
        {
            RectF b = L.launchBtn;
            if (g_app.launching) {
                FillRound(gr, b, SR(13), C(RGB(0x1E, 0x24, 0x22)));
                GraphicsPath pth;
                AddRoundRect(pth, b, SR(13));
                gr.SetClip(&pth);
                REAL w = b.Width * (g_app.progress / 100.0f);
                if (w > SR(4)) FillRound(gr, RectF(b.X, b.Y, w, b.Height), SR(13), C(Theme::AccentSoft));
                gr.ResetClip();
                StrokeRound(gr, b, SR(13), Color(70, 120, 220, 160), SR(1));
            } else {
                DWORD base = g_app.launched ? RGB(0x36, 0xA8, 0x66)
                            : (g_app.hover == HIT_LAUNCH ? RGB(0x5C, 0xEB, 0x90) : Theme::Accent);
                DWORD base2 = g_app.launched ? RGB(0x2A, 0x8C, 0x52) : RGB(0x3E, 0xC9, 0x71);
                LinearGradientBrush lg(b, C(base), C(base2), LinearGradientModeVertical);
                GraphicsPath pth;
                AddRoundRect(pth, b, SR(13));
                gr.FillPath(&lg, &pth);
            }

            std::wstring text = g_app.launching
                ? (L"正在启动 " + std::to_wstring((int)g_app.progress) + L"%")
                : (g_app.launched ? L"游戏已启动" : L"启动游戏");
            Color tc = g_app.launching ? Color(255, 240, 255, 246) : C(Theme::OnAccent);

            REAL tw    = MeasureW(gr, text, *F.btn);
            REAL iconW = g_app.launching ? 0.0f : SR(23);
            REAL startX = b.X + (b.Width - (tw + iconW)) / 2;
            REAL cy     = b.Y + b.Height / 2;

            if (!g_app.launching) {
                PointF triPts[3] = {
                    PointF(startX, cy - SR(6)),
                    PointF(startX + SR(11), cy),
                    PointF(startX, cy + SR(6))
                };
                GraphicsPath tri;
                tri.AddPolygon(triPts, 3);
                SolidBrush tb(tc);
                gr.FillPath(&tb, &tri);
            }
            DrawStr(gr, text, RectF(startX + iconW, b.Y, tw + SR(6), b.Height), *F.btn, tc,
                    StringAlignmentNear, false, true);
        }

        // ---------------- 日志 ----------------
        for (size_t i = 0; i < g_app.logs.size(); ++i) {
            bool last = (i + 1 == g_app.logs.size());
            DrawStr(gr, L"› " + g_app.logs[i],
                    RectF(L.logArea.X, L.logArea.Y + (REAL)i * SR(16), L.logArea.Width, SR(16)),
                    *F.log, C(last ? Theme::Muted : Theme::Dim), StringAlignmentNear, true);
        }

        // ---------------- 下拉浮层（最后绘制，覆盖在上层） ----------------
        if (g_app.ddOpen && !g_app.versions.empty()) {
            int n    = (int)g_app.versions.size();
            int show = std::min(n, DD_MAX_SHOW);
            RectF panel(L.verBox.X, L.verBox.Y + L.verBox.Height + SR(6), L.verBox.Width,
                        (REAL)(show * S(DD_ITEM_H) + S(8)));
            FillRound(gr, panel, SR(11), C(RGB(0x1E, 0x22, 0x29)));
            StrokeRound(gr, panel, SR(11), C(RGB(0x35, 0x3B, 0x45)), SR(1));

            GraphicsPath clip;
            AddRoundRect(clip, panel, SR(11));
            gr.SetClip(&clip);
            for (int i = 0; i < show; ++i) {
                int idx = i + g_app.ddScroll;
                if (idx >= n) break;
                RectF ir(panel.X + SR(5), panel.Y + SR(4) + i * S(DD_ITEM_H),
                         panel.Width - SR(10), S(DD_ITEM_H));
                bool sel = (idx == g_app.versionIdx);
                if (g_app.ddHover == idx || sel)
                    FillRound(gr, ir, SR(8), C(sel ? RGB(0x25, 0x33, 0x2B) : Theme::FieldHover));
                DrawStr(gr, g_app.versions[(size_t)idx],
                        RectF(ir.X + SR(12), ir.Y, ir.Width - SR(30), ir.Height),
                        *F.sub, C(sel ? Theme::Accent : Theme::Text), StringAlignmentNear, true, true);
                if (sel) {
                    SolidBrush dot(C(Theme::Accent));
                    gr.FillEllipse(&dot, ir.X + ir.Width - SR(14), ir.Y + ir.Height / 2 - SR(3),
                                   SR(6), SR(6));
                }
            }
            gr.ResetClip();
        }
    }

    BitBlt(hdc, 0, 0, W, H, memDC, 0, 0, SRCCOPY);
    SelectObject(memDC, old);
    DeleteObject(bmp);
    DeleteDC(memDC);
    EndPaint(hwnd, &ps);
}

// ============================================================================
//  命中测试 & 交互
// ============================================================================
static bool InRect(const RectF& r, int x, int y) {
    return (REAL)x >= r.X && (REAL)x <= r.X + r.Width &&
           (REAL)y >= r.Y && (REAL)y <= r.Y + r.Height;
}

static RectF DropdownPanel() {
    int show = std::min((int)g_app.versions.size(), DD_MAX_SHOW);
    Layout L = ComputeLayout();
    return RectF(L.verBox.X, L.verBox.Y + L.verBox.Height + SR(6), L.verBox.Width,
                 (REAL)(show * S(DD_ITEM_H) + S(8)));
}

static int DropdownItemAt(int x, int y) {
    RectF p = DropdownPanel();
    if (!InRect(p, x, y)) return -1;
    int i = (int)(((REAL)y - (p.Y + SR(4))) / S(DD_ITEM_H));
    if (i < 0 || i >= DD_MAX_SHOW) return -1;
    int idx = i + g_app.ddScroll;
    return (idx >= 0 && idx < (int)g_app.versions.size()) ? idx : -1;
}

static int HitTest(int x, int y) {
    Layout L = ComputeLayout();
    if (InRect(L.closeBtn, x, y))  return HIT_CLOSE;
    if (InRect(L.minBtn, x, y))    return HIT_MIN;
    if (InRect(L.userBox, x, y))   return HIT_USER;
    if (InRect(L.verBox, x, y))    return HIT_VER;
    if (InRect(L.browseBtn, x, y)) return HIT_BROWSE;
    if (InRect(L.launchBtn, x, y)) return HIT_LAUNCH;
    RectF track = L.sliderTrack;
    track.Y -= SR(10);
    track.Height += SR(20);
    if (InRect(track, x, y))       return HIT_SLIDER;
    return HIT_NONE;
}

static void SetSliderFromX(int x) {
    Layout L = ComputeLayout();
    float t = ((REAL)x - L.sliderTrack.X) / L.sliderTrack.Width;
    t = std::max(0.0f, std::min(1.0f, t));
    int v = (int)std::lround(1.0f + t * 15.0f);
    if (v != g_app.memGb) {
        g_app.memGb = v;
        InvalidateRect(g_app.hwnd, nullptr, FALSE);
    }
}

static void BrowseGameDir() {
    BROWSEINFOW bi{};
    bi.hwndOwner = g_app.hwnd;
    bi.lpszTitle = L"选择 .minecraft 目录";
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;

    wchar_t path[MAX_PATH] = {0};
    if (SHGetPathFromIDListW(pidl, path)) {
        g_app.gameDir = path;
        auto vs = ScanVersions(g_app.gameDir);
        if (!vs.empty()) {
            std::wstring cur = g_app.versions.empty() ? L"" : g_app.versions[(size_t)g_app.versionIdx];
            g_app.versions = vs;
            g_app.realScan = true;
            g_app.versionIdx = 0;
            for (size_t i = 0; i < vs.size(); ++i)
                if (vs[i] == cur) { g_app.versionIdx = (int)i; break; }
        }
        PushLog(L"目录已更新 · 版本数 " + std::to_wstring(g_app.versions.size()));
        SaveConfig();
    }
    CoTaskMemFree(pidl);
    InvalidateRect(g_app.hwnd, nullptr, FALSE);
}

// ============================================================================
//  窗口过程
// ============================================================================
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        g_app.hwnd = hwnd;
        SetTimer(hwnd, 1, 100, nullptr);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        Paint(hwnd);
        return 0;

    case WM_TIMER: {
        if (wParam != 1) break;
        g_app.caretTick++;
        if (g_app.caretTick % 5 == 0) g_app.caretOn = !g_app.caretOn;

        if (g_app.launching) {
            g_app.progress += 2.5f;
            if (g_app.progress >= 100.0f) {
                g_app.progress = 100.0f;
                FinishLaunch();
            } else {
                int step = (int)(g_app.progress / 20.0f);
                if (step > g_app.step && step < 5) {
                    g_app.step = step;
                    PushLog(kLaunchSteps[step]);
                }
            }
        } else if (g_app.launched && g_app.resetTick > 0) {
            if (--g_app.resetTick == 0) g_app.launched = false;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);

        if (g_app.draggingSlider) { SetSliderFromX(x); return 0; }

        int h = HitTest(x, y);
        if (h != g_app.hover) { g_app.hover = h; InvalidateRect(hwnd, nullptr, FALSE); }

        int di = g_app.ddOpen ? DropdownItemAt(x, y) : -1;
        if (di != g_app.ddHover) { g_app.ddHover = di; InvalidateRect(hwnd, nullptr, FALSE); }

        LPCWSTR cur = IDC_ARROW;
        if (h == HIT_CLOSE || h == HIT_MIN || h == HIT_BROWSE || h == HIT_LAUNCH || h == HIT_VER)
            cur = IDC_HAND;
        else if (h == HIT_USER)
            cur = IDC_IBEAM;
        else if (h == HIT_SLIDER)
            cur = IDC_SIZEWE;
        SetCursor(LoadCursorW(nullptr, cur));
        return 0;
    }

    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
        SetFocus(hwnd);

        if (g_app.ddOpen) {                        // 浮层优先
            int di = DropdownItemAt(x, y);
            if (di >= 0) { g_app.versionIdx = di; SaveConfig(); }
            g_app.ddOpen  = false;
            g_app.ddHover = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        switch (HitTest(x, y)) {
        case HIT_CLOSE:  DestroyWindow(hwnd); return 0;
        case HIT_MIN:    ShowWindow(hwnd, SW_MINIMIZE); return 0;
        case HIT_USER:   g_app.focus = FOCUS_USER; g_app.caretOn = true; g_app.caretTick = 0; break;
        case HIT_VER:    g_app.ddOpen = true; g_app.ddScroll = 0; break;
        case HIT_BROWSE: BrowseGameDir(); return 0;
        case HIT_LAUNCH: StartLaunch(); break;
        case HIT_SLIDER: g_app.draggingSlider = true; SetCapture(hwnd); SetSliderFromX(x); return 0;
        default:
            g_app.focus = FOCUS_NONE;
            if (y < S(70)) {                       // 顶部空白区域拖动窗口
                ReleaseCapture();
                SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            }
            break;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONUP:
        if (g_app.draggingSlider) {
            g_app.draggingSlider = false;
            ReleaseCapture();
            SaveConfig();
        }
        return 0;

    case WM_MOUSEWHEEL: {
        if (!g_app.ddOpen) break;
        int delta     = GET_WHEEL_DELTA_WPARAM(wParam);
        int maxScroll = std::max(0, (int)g_app.versions.size() - DD_MAX_SHOW);
        if (delta > 0) g_app.ddScroll = std::max(0, g_app.ddScroll - 1);
        else           g_app.ddScroll = std::min(maxScroll, g_app.ddScroll + 1);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_CHAR: {
        if (g_app.focus == FOCUS_USER) {
            wchar_t ch = (wchar_t)wParam;
            if (ch == L'\b') {
                if (!g_app.username.empty()) g_app.username.pop_back();
            } else if (ch >= 32 && ch != 127) {
                if (g_app.username.size() < 16) g_app.username.push_back(ch);
            }
            g_app.caretOn = true;
            g_app.caretTick = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            if (g_app.ddOpen) { g_app.ddOpen = false; InvalidateRect(hwnd, nullptr, FALSE); }
            return 0;
        }
        if (wParam == VK_RETURN) {
            StartLaunch();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) return TRUE;    // 光标交由 WM_MOUSEMOVE 控制
        break;

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        SaveConfig();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ============================================================================
//  入口
// ============================================================================
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    SetProcessDPIAware();

    HDC screen = GetDC(nullptr);
    g_scale = (float)GetDeviceCaps(screen, LOGPIXELSX) / 96.0f;
    if (g_scale < 1.0f) g_scale = 1.0f;
    ReleaseDC(nullptr, screen);

    GdiplusStartupInput gsi;
    ULONG_PTR gpToken = 0;
    if (GdiplusStartup(&gpToken, &gsi, nullptr) != Ok) return 1;

    InitFonts();
    OleInitialize(nullptr);

    // ---- 品牌资源（图标 + 页眉标志）----
    if (!LoadLogoResource(hInst))
        OutputDebugStringW(L"[DML] 内嵌标志加载失败，回退手绘草方块\n");

    // ---- 配置 & 环境 ----
    std::wstring savedVer;
    LoadConfig(savedVer);
    if (g_app.gameDir.empty() || !PathExists(g_app.gameDir))
        g_app.gameDir = DefaultMinecraftDir();

    g_app.versions = ScanVersions(g_app.gameDir);
    g_app.realScan = !g_app.versions.empty();
    if (!g_app.realScan) g_app.versions = FallbackVersions();

    if (!savedVer.empty()) {
        for (size_t i = 0; i < g_app.versions.size(); ++i)
            if (g_app.versions[i] == savedVer) { g_app.versionIdx = (int)i; break; }
    }
    g_app.javaPath = DetectJava();

    // ---- 建窗 ----
    // 图标取自内嵌的 dml.ico（多尺寸），按系统要求的大/小图标尺寸各取一张
    HICON hIconBig = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                       GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
                                       LR_DEFAULTCOLOR);
    HICON hIconSm  = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                       GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                       LR_DEFAULTCOLOR);
    if (!hIconBig) hIconBig = LoadIconW(nullptr, IDI_APPLICATION);
    if (!hIconSm)  hIconSm  = hIconBig;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"DreamMinecraftLauncherWnd";
    wc.hIcon         = hIconBig;
    wc.hIconSm       = hIconSm;
    RegisterClassExW(&wc);

    int w = S(WIN_W), h = S(WIN_H);
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, L"DreamMinecraftLauncherWnd",
                                L"Dream Minecraft Launcher",
                                WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX,
                                x, y, w, h, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    // 显式设置窗口图标，确保任务栏与 Alt+Tab 也用的是品牌图标
    SendMessageW(hwnd, WM_SETICON, ICON_BIG,   (LPARAM)hIconBig);
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconSm);

    HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, S(18), S(18));
    SetWindowRgn(hwnd, rgn, TRUE);

    // ---- 首屏日志 ----
    PushLog(g_app.javaPath.empty() ? L"未检测到 Java，将回退 javaw.exe"
                                   : (L"Java: " + g_app.javaPath));
    if (g_app.realScan)
        PushLog(L"游戏目录: " + g_app.gameDir);
    else if (PathExists(g_app.gameDir))
        PushLog(L"目录存在但无已安装版本，使用内置示例列表");
    else
        PushLog(L"未找到 .minecraft，当前为内置示例版本列表");
    PushLog(L"可用版本 " + std::to_wstring(g_app.versions.size()) + L" 个 · 内存 " +
            std::to_wstring(g_app.memGb) + L" GB");
    PushLog(L"准备就绪，点击「启动游戏」");

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    FreeFonts();
    FreeLogoResource();
    if (hIconBig && hIconBig != hIconSm) DestroyIcon(hIconBig);
    if (hIconSm) DestroyIcon(hIconSm);
    GdiplusShutdown(gpToken);
    OleUninitialize();
    return 0;
}
