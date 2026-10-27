// ============================================================================
//  Dream Minecraft Launcher (DML) —— 极简 Minecraft 启动器
//  ---------------------------------------------------------------------------
//  · 纯 Win32 API + GDI+ 手绘界面，无任何第三方 UI 库，单文件实现
//  · 无边框圆角窗口、自绘输入框 / 下拉框 / 滑块 / 按钮 / 日志面板
//  · 配置持久化（%APPDATA%\DreamMinecraftLauncher\config.ini）
//  · 自动探测 .minecraft 目录、已安装版本、Java 运行时
//  · 真实启动：解析 version.json（含 inheritsFrom 继承与 rules 过滤）→ 归类
//    libraries / natives → 替换启动占位符 → CreateProcessW 拉起 Java，
//    子进程 stdout/stderr 实时回灌到日志面板
//  · 离线模式启动（不做微软账号鉴权，也不自动下载缺失的 libraries/assets）
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
#include <shellapi.h>
#include <commdlg.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <gdiplus.h>
#include <mmsystem.h>   // timeBeginPeriod：把系统定时器精度提到 1ms，动画帧间隔才真的短

#include "resource.h"   // IDI_APPICON / IDR_LOGO

#include <stdlib.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <set>
#include <fstream>
#include <algorithm>
#include <functional>
#include <utility>
#include <atomic>
#include <cmath>

using namespace Gdiplus;

// 子进程输出回灌：LPARAM 是 new 出来的 std::wstring*，处理完需 delete
#define WM_APP_LOG (WM_APP + 1)
// 微软登录线程 → UI：wParam = 1 设备码就绪 / 2 流程结束（成功或失败，看 g_msShared.ok）
#define WM_APP_MS  (WM_APP + 2)
// 下载线程 → UI：wParam = 1 版本列表就绪 / 2 下载完成 / 3 下载失败或取消
#define WM_APP_DL  (WM_APP + 3)

#ifdef _MSC_VER
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "uuid.lib")
#endif

// ============================================================================
//  主题配色（浅色：白底深字 + 浅蓝账号胶囊 + 绿色游戏强调色）
// ============================================================================
namespace Theme {
    const DWORD Bg          = RGB(0xF5, 0xF7, 0xFB);   // 页面底色（近白）
    const DWORD Panel       = RGB(0xFF, 0xFF, 0xFF);   // 卡片白
    const DWORD PanelBorder = RGB(0xE4, 0xE9, 0xF1);
    const DWORD Field       = RGB(0xF4, 0xF6, 0xFA);   // 输入框底色
    const DWORD FieldHover  = RGB(0xEA, 0xF0, 0xF9);
    const DWORD Border      = RGB(0xDC, 0xE2, 0xEC);
    const DWORD Text        = RGB(0x1B, 0x20, 0x28);
    const DWORD Muted       = RGB(0x5D, 0x67, 0x76);
    const DWORD Dim         = RGB(0x93, 0x9D, 0xAC);
    const DWORD Accent      = RGB(0x24, 0xA8, 0x62);   // 绿色强调（滑块 / 版本 / 启动）
    const DWORD AccentSoft  = RGB(0x1D, 0x8F, 0x54);
    const DWORD OnAccent    = RGB(0xFF, 0xFF, 0xFF);   // 强调色底上的字
    const DWORD Grass       = RGB(0x54, 0xA6, 0x4B);
    const DWORD Dirt        = RGB(0x8B, 0x6A, 0x4A);
    const DWORD Track       = RGB(0xE7, 0xEB, 0xF2);
    const DWORD Danger      = RGB(0xE0, 0x4C, 0x4C);
    // 账号胶囊 / 微软相关：浅蓝
    const DWORD PillBg      = RGB(0xE3, 0xEF, 0xFE);   // 选中胶囊底色
    const DWORD PillBorder  = RGB(0xA6, 0xC6, 0xF5);
    const DWORD PillText    = RGB(0x1C, 0x61, 0xD4);
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

// 子进程输出解码：Java 的 stdout/stderr 用平台默认字符集（中文系统是 GBK），
// 所以先按 UTF-8 严格试解，失败再退回 OEM 代码页。
static std::wstring DecodeChild(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
    UINT cp = (n > 0) ? CP_UTF8 : CP_OEMCP;
    if (n <= 0) n = MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return U82W(s);   // 兜底：至少保留可显示的部分
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), &w[0], n);
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

static bool InRect(const RectF& r, int x, int y) {
    return (REAL)x >= r.X && (REAL)x <= r.X + r.Width &&
           (REAL)y >= r.Y && (REAL)y <= r.Y + r.Height;
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
    RectF tab[3];        // 标题下方：开始 / 下载 / 设置

    // ---- 开始页 ----
    RectF panel1;        // 昵称 / 账号方式 / 版本
    RectF userBox;
    RectF verBox;
    RectF msOffBtn;      // 昵称框下方：离线登录 / 微软登录
    RectF msOnBtn;
    RectF msUnbind;      // 仅已登录时显示
    RectF launchBtn;
    RectF logArea;

    // ---- 下载页 ----
    RectF dlPanel;

    // ---- 设置页 ----
    RectF dirPanel;
    RectF javaPanel;
    RectF memPanel;
    RectF argPanel;      // 附加 JVM 参数
    RectF argBox;
    RectF browseBtn;
    RectF javaBtn;
    RectF sliderTrack;

    // ---- 弹窗 ----
    RectF dlgCard;       // 验证码弹窗
    RectF dlgOpen;
    RectF dlgOk;
    RectF closeBtn;
    RectF minBtn;
};

static const int WIN_W = 760;
static const int WIN_H = 500;

static Layout ComputeLayout() {
    Layout L;
    L.logo        = RectF(S(28),  S(20),  S(40),  S(40));
    L.tab[0]      = RectF(S(28),  S(64),  S(70),  S(30));
    L.tab[1]      = RectF(S(106), S(64),  S(70),  S(30));
    L.tab[2]      = RectF(S(184), S(64),  S(70),  S(30));

    L.minBtn      = RectF(S(662), S(16),  S(30),  S(30));
    L.closeBtn    = RectF(S(700), S(16),  S(30),  S(30));

    // 开始页
    L.panel1      = RectF(S(28),  S(104), S(704), S(124));
    L.userBox     = RectF(S(52),  S(148), S(300), S(42));
    L.msOffBtn    = RectF(S(52),  S(194), S(78),  S(22));
    L.msOnBtn     = RectF(S(136), S(194), S(78),  S(22));
    L.msUnbind    = RectF(S(222), S(190), S(84),  S(30));
    L.verBox      = RectF(S(376), S(148), S(332), S(42));
    L.launchBtn   = RectF(S(28),  S(240), S(704), S(50));
    L.logArea     = RectF(S(28),  S(306), S(704), S(164));

    // 下载页
    L.dlPanel     = RectF(S(28),  S(104), S(704), S(366));

    // 设置页
    L.dirPanel    = RectF(S(28),  S(104), S(704), S(68));
    L.browseBtn   = RectF(S(636), S(120), S(72),  S(34));
    L.javaPanel   = RectF(S(28),  S(180), S(704), S(68));
    L.javaBtn     = RectF(S(636), S(196), S(72),  S(34));
    L.memPanel    = RectF(S(28),  S(256), S(704), S(70));
    L.sliderTrack = RectF(S(52),  S(306), S(656), S(6));
    L.argPanel    = RectF(S(28),  S(334), S(704), S(70));
    L.argBox      = RectF(S(52),  S(362), S(656), S(32));

    // 微软验证码弹窗
    L.dlgCard     = RectF(S(150), S(140), S(460), S(212));
    L.dlgOpen     = RectF(S(247), S(296), S(150), S(34));
    L.dlgOk       = RectF(S(409), S(296), S(104), S(34));
    return L;
}

// ============================================================================
//  应用状态
// ============================================================================
enum { HIT_NONE = 0, HIT_CLOSE, HIT_MIN, HIT_TAB0, HIT_TAB1, HIT_TAB2,
       HIT_USER, HIT_VER, HIT_SLIDER, HIT_BROWSE, HIT_JAVA, HIT_ARGS, HIT_LAUNCH,
       HIT_MS_OFF, HIT_MS_ON, HIT_MS_UNBIND };
enum { FOCUS_NONE = 0, FOCUS_USER, FOCUS_ARGS };

static const int DD_ITEM_H   = 34;   // 下拉项高度（逻辑像素）
static const int DD_MAX_SHOW = 6;    // 下拉最多显示条数

struct AppState {
    HWND hwnd = nullptr;

    int  page = 0;                   // 0 开始 / 1 下载 / 2 设置

    std::wstring username = L"Steve";
    std::wstring jvmArgs;            // 附加 JVM 参数，原样追加到 java 命令行
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
    float progress  = 0.0f;          // 实际绘制用的进度（由 16ms 定时器平滑逼近 progressTarget）
    float progressTarget = 0.0f;     // 当前步骤的进度目标
    int   step      = 0;
    int   resetTick = 0;
    bool  stepRan[5] = { false, false, false, false, false };  // 该步的真实工作是否已执行
    bool  msDlg     = false;         // 微软登录验证码弹窗（逻辑开关）
    float msAnim    = 0.0f;          // 弹窗动画进度 0..1
    int   dlgWait   = 0;             // 弹窗停留后打开浏览器的倒计时（秒，0 = 已开）

    // ---- 标签下划线滑动动画（由主消息循环驱动，每 4ms 一帧 ≈145fps）----
    float    tabAnim    = 1.0f;      // 0 → 1；1 = 已经停在目标标签下
    REAL     tabFromX   = 0.0f;      // 起点：上一次停下的位置
    REAL     tabFromW   = 0.0f;
    REAL     tabCurX    = 0.0f;      // 当前实际画的位置，中途再切页时当作新起点
    REAL     tabCurW    = 0.0f;
    bool     tabHasCur  = false;     // 第一帧绘制后就为 true
    LONGLONG tabT0      = 0;         // 本次滑动的起始时刻（微秒），进度按真实时间算

    // ---- 下载页 ----
    int   dlPhase  = 0;              // 0 拉列表 / 1 选版本 / 2 下载中 / 3 完成 / 4 失败
    int   dlFilter = 0;              // 0 正式版 / 1 快照 / 2 全部
    int   dlSel    = 0;              // 选中项（在 g_dlView 里的下标）
    int   dlScroll = 0;
    int   dlHover  = -1;
    std::wstring dlTarget;           // 正在下载 / 刚下载完的版本 id
};

static AppState g_app;

// ============================================================================
//  微软账号状态
//    LOGGED_OUT → WAITING（等用户在浏览器输码）→ WORKING（换令牌链）→ LOGGED_IN
//    任何一步失败都回到 LOGGED_OUT 并把原因写进 g_ms.err
// ============================================================================
enum MsState { MS_LOGGED_OUT = 0, MS_WAITING, MS_WORKING, MS_LOGGED_IN };

struct MsAccount {
    MsState      state = MS_LOGGED_OUT;
    std::wstring name;          // Minecraft 用户名（登录后覆盖昵称框）
    std::wstring uuid;          // 去横线的 32 位 uuid
    std::wstring xuid;
    std::wstring accessToken;   // Minecraft access token（只在内存里，不落盘）
    std::wstring refreshToken;  // 微软 refresh token（DPAPI 加密后落盘）
    std::wstring userCode;      // 设备码，展示给用户
    std::wstring verifyUri;     // 输码网址
    std::wstring err;           // 失败原因
};

static MsAccount g_ms;

// 离线昵称：与「登录后显示的 MC 用户名」分开存，退出登录时能还原
static std::wstring g_offlineName = L"Steve";

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

// 不管 .minecraft 在不在，都返回 %APPDATA%\.minecraft：下载新版本时要靠它把目录建出来
static std::wstring AppDataMinecraftDir() {
    wchar_t buf[MAX_PATH] = {0};
    if (GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH))
        return std::wstring(buf) + L"\\.minecraft";
    return L"";
}

static void SaveConfig() {
    std::wstring dir = AppDataConfigDir();
    CreateDirectoryW(dir.c_str(), nullptr);
    std::ofstream f(W2U8(ConfigPath()).c_str());
    if (!f) return;
    f << "username=" << W2U8(g_offlineName) << "\n";
    f << "version="  << W2U8(g_app.versions.empty() ? std::wstring() : g_app.versions[(size_t)g_app.versionIdx]) << "\n";
    f << "memory="   << g_app.memGb << "\n";
    f << "gamedir="  << W2U8(g_app.gameDir) << "\n";
    f << "java="     << W2U8(g_app.javaPath) << "\n";
    f << "jvmargs="  << W2U8(g_app.jvmArgs) << "\n";
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
        else if (k == "java")     g_app.javaPath = v;
        else if (k == "jvmargs")  g_app.jvmArgs = v;
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
    while (g_app.logs.size() > 9) g_app.logs.erase(g_app.logs.begin());
}

// ============================================================================
//  极简 JSON 解析（只覆盖 version.json 用到的子集）
// ============================================================================
struct JVal {
    enum Type { NUL, BOOL, NUM, STR, ARR, OBJ };
    Type t = NUL;
    bool b = false;
    double num = 0.0;
    std::wstring s;
    std::vector<JVal> a;
    std::vector<std::pair<std::wstring, JVal>> o;

    const JVal* get(const wchar_t* k) const {
        if (t != OBJ) return nullptr;
        for (size_t i = 0; i < o.size(); ++i)
            if (o[i].first == k) return &o[i].second;
        return nullptr;
    }
    std::wstring str(const wchar_t* k, const wchar_t* def = L"") const {
        const JVal* v = get(k);
        return (v && v->t == STR) ? v->s : std::wstring(def);
    }
    bool flag(const wchar_t* k, bool def = false) const {
        const JVal* v = get(k);
        return (v && v->t == BOOL) ? v->b : def;
    }
};

struct JsonParser {
    const std::wstring& src;
    size_t i = 0;

    explicit JsonParser(const std::wstring& s) : src(s) {}

    void ws() {
        while (i < src.size()) {
            wchar_t c = src[i];
            if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == 0xFEFF) ++i;
            else break;
        }
    }
    bool eat(wchar_t c) { ws(); if (i < src.size() && src[i] == c) { ++i; return true; } return false; }

    bool parseString(std::wstring& out) {
        ws();
        if (i >= src.size() || src[i] != L'"') return false;
        ++i;
        out.clear();
        while (i < src.size()) {
            wchar_t c = src[i++];
            if (c == L'"') return true;
            if (c != L'\\') { out.push_back(c); continue; }
            if (i >= src.size()) return false;
            wchar_t e = src[i++];
            switch (e) {
            case L'"':  out.push_back(L'"');  break;
            case L'\\': out.push_back(L'\\'); break;
            case L'/':  out.push_back(L'/');  break;
            case L'b':  out.push_back(L'\b'); break;
            case L'f':  out.push_back(L'\f'); break;
            case L'n':  out.push_back(L'\n'); break;
            case L'r':  out.push_back(L'\r'); break;
            case L't':  out.push_back(L'\t'); break;
            case L'u': {
                if (i + 4 > src.size()) return false;
                unsigned v = 0;
                for (int k = 0; k < 4; ++k) {
                    wchar_t h = src[i++];
                    unsigned d;
                    if (h >= L'0' && h <= L'9') d = (unsigned)(h - L'0');
                    else if (h >= L'a' && h <= L'f') d = (unsigned)(h - L'a' + 10);
                    else if (h >= L'A' && h <= L'F') d = (unsigned)(h - L'A' + 10);
                    else return false;
                    v = v * 16 + d;
                }
                out.push_back((wchar_t)v);   // wchar_t 在 Windows 上是 UTF-16 单元，代理对分两次写入
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    bool parseValue(JVal& v) {
        ws();
        if (i >= src.size()) return false;
        wchar_t c = src[i];
        if (c == L'{') {
            v.t = JVal::OBJ;
            ++i;
            if (eat(L'}')) return true;
            do {
                std::wstring key;
                if (!parseString(key)) return false;
                if (!eat(L':')) return false;
                JVal child;
                if (!parseValue(child)) return false;
                v.o.push_back(std::make_pair(key, child));
            } while (eat(L','));
            return eat(L'}');
        }
        if (c == L'[') {
            v.t = JVal::ARR;
            ++i;
            if (eat(L']')) return true;
            do {
                JVal child;
                if (!parseValue(child)) return false;
                v.a.push_back(child);
            } while (eat(L','));
            return eat(L']');
        }
        if (c == L'"') { v.t = JVal::STR; return parseString(v.s); }
        if (src.compare(i, 4, L"true") == 0)  { i += 4; v.t = JVal::BOOL; v.b = true;  return true; }
        if (src.compare(i, 5, L"false") == 0) { i += 5; v.t = JVal::BOOL; v.b = false; return true; }
        if (src.compare(i, 4, L"null") == 0)  { i += 4; v.t = JVal::NUL;  return true; }

        // number
        size_t start = i;
        if (i < src.size() && (src[i] == L'-' || src[i] == L'+')) ++i;
        while (i < src.size() && ((src[i] >= L'0' && src[i] <= L'9') || src[i] == L'.' ||
                                  src[i] == L'e' || src[i] == L'E' || src[i] == L'-' || src[i] == L'+'))
            ++i;
        if (i == start) return false;
        v.t = JVal::NUM;
        v.num = wcstod(src.substr(start, i - start).c_str(), nullptr);
        return true;
    }
};

static bool ParseJsonUtf8(const std::string& utf8, JVal& out) {
    if (utf8.empty()) return false;
    std::wstring wide = U82W(utf8);
    JsonParser p(wide);
    if (!p.parseValue(out)) return false;
    return true;
}

static std::string ReadFileUtf8(const std::wstring& path) {
    FILE* fp = _wfopen(path.c_str(), L"rb");
    if (!fp) return std::string();
    std::string data;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) data.append(buf, n);
    fclose(fp);
    return data;
}

// ============================================================================
//  版本清单解析：rules 过滤 / inheritsFrom 继承 / 类路径与 natives 收集
// ============================================================================
static bool RulesAllow(const JVal* rules) {
    if (!rules || rules->t != JVal::ARR || rules->a.empty()) return true;
    bool allow = false;                       // 出现 rules 时默认禁止，最后一条命中者生效
    for (size_t i = 0; i < rules->a.size(); ++i) {
        const JVal& r = rules->a[i];
        bool match = true;

        const JVal* feat = r.get(L"features");
        if (feat && feat->t == JVal::OBJ && !feat->o.empty()) match = false;  // 不支持可选特性

        const JVal* os = r.get(L"os");
        if (os && os->t == JVal::OBJ) {
            std::wstring name = os->str(L"name");
            std::wstring arch = os->str(L"arch");
            if (!name.empty() && name != L"windows") match = false;
            if (!arch.empty() && arch != L"x86_64")  match = false;
        }
        if (match) allow = (r.str(L"action") == L"allow");
    }
    return allow;
}

// name = group:artifact:version[:classifier] -> 相对 libraries 的路径
static std::wstring MavenRel(const std::wstring& name, const std::wstring& classifier) {
    std::vector<std::wstring> parts;
    size_t start = 0;
    for (;;) {
        size_t p = name.find(L':', start);
        if (p == std::wstring::npos) { parts.push_back(name.substr(start)); break; }
        parts.push_back(name.substr(start, p - start));
        start = p + 1;
    }
    if (parts.size() < 3) return L"";
    std::wstring group = parts[0], artifact = parts[1], version = parts[2];
    std::wstring cls = classifier.empty() && parts.size() >= 4 ? parts[3] : classifier;
    std::wstring gpath = group;
    for (size_t i = 0; i < gpath.size(); ++i)
        if (gpath[i] == L'.') gpath[i] = L'/';
    return gpath + L"/" + artifact + L"/" + version + L"/" + artifact + L"-" + version +
           (cls.empty() ? L"" : (L"-" + cls)) + L".jar";
}

struct LibEntry {
    std::wstring key;      // group:artifact，用于跨继承层去重
    std::wstring rel;      // 相对 libraries 的路径
    bool native = false;
};

static void CollectLibs(const JVal& ver, std::vector<LibEntry>& out) {
    const JVal* libs = ver.get(L"libraries");
    if (!libs || libs->t != JVal::ARR) return;

    for (size_t i = 0; i < libs->a.size(); ++i) {
        const JVal& lib = libs->a[i];
        if (!RulesAllow(lib.get(L"rules"))) continue;

        std::wstring name = lib.str(L"name");
        const JVal* downloads = lib.get(L"downloads");
        const JVal* natives = lib.get(L"natives");

        LibEntry e;
        if (natives && natives->t == JVal::OBJ) {
            std::wstring cls = natives->str(L"windows");
            if (cls.empty()) continue;
            size_t p;
            while ((p = cls.find(L"${arch}")) != std::wstring::npos) cls.replace(p, 7, L"64");
            e.native = true;
            if (downloads) {
                const JVal* dc = downloads->get(L"classifiers");
                if (dc) {
                    const JVal* d = dc->get(cls.c_str());
                    if (d) e.rel = d->str(L"path");
                }
            }
            if (e.rel.empty()) e.rel = MavenRel(name, cls);
        } else {
            size_t c1 = name.find(L':');
            if (c1 != std::wstring::npos) {
                size_t c2 = name.find(L':', c1 + 1);
                e.key = (c2 == std::wstring::npos) ? name : name.substr(0, c2);
            }
            if (downloads) {
                const JVal* art = downloads->get(L"artifact");
                if (art) e.rel = art->str(L"path");
            }
            if (e.rel.empty()) e.rel = MavenRel(name, L"");
        }

        if (e.rel.empty()) continue;
        for (size_t k = 0; k < e.rel.size(); ++k)
            if (e.rel[k] == L'/') e.rel[k] = L'\\';

        // 同一 group:artifact 在子版本里出现时覆盖父版本，位置保持父版本的顺序
        bool replaced = false;
        if (!e.key.empty()) {
            for (size_t k = 0; k < out.size(); ++k) {
                if (out[k].key == e.key && !out[k].native) { out[k].rel = e.rel; replaced = true; break; }
            }
        }
        if (!replaced) out.push_back(e);
    }
}

static void ExpandArgs(const JVal* arr, std::vector<std::wstring>& out) {
    if (!arr || arr->t != JVal::ARR) return;
    for (size_t i = 0; i < arr->a.size(); ++i) {
        const JVal& e = arr->a[i];
        if (e.t == JVal::STR) { out.push_back(e.s); continue; }
        if (e.t != JVal::OBJ) continue;
        if (!RulesAllow(e.get(L"rules"))) continue;
        const JVal* v = e.get(L"value");
        if (!v) continue;
        if (v->t == JVal::STR) out.push_back(v->s);
        else if (v->t == JVal::ARR)
            for (size_t k = 0; k < v->a.size(); ++k)
                if (v->a[k].t == JVal::STR) out.push_back(v->a[k].s);
    }
}

typedef std::vector<std::pair<std::wstring, std::wstring>> VarMap;

static std::wstring Subst(std::wstring s, const VarMap& vars) {
    for (size_t i = 0; i < vars.size(); ++i) {
        std::wstring key = L"${" + vars[i].first + L"}";
        size_t p = 0;
        while ((p = s.find(key, p)) != std::wstring::npos) {
            s.replace(p, key.size(), vars[i].second);
            p += vars[i].second.size();
        }
    }
    size_t p;                                  // 剩余未识别占位符一律清空，避免把 ${x} 原样传给 java
    while ((p = s.find(L"${")) != std::wstring::npos) {
        size_t q = s.find(L'}', p);
        if (q == std::wstring::npos) break;
        s.erase(p, q - p + 1);
    }
    return s;
}

static std::wstring QuoteArg(const std::wstring& s) {
    if (!s.empty() && s.find(L' ') == std::wstring::npos && s.find(L'\t') == std::wstring::npos)
        return s;
    return L"\"" + s + L"\"";
}

// 离线模式下的稳定 UUID：标准做法是 MD5("OfflinePlayer:"+name)，
// 这里用 FNV-1a + xorshift 派生 32 位十六进制，格式合法且只依赖昵称。
static std::wstring OfflineUuid(const std::wstring& name) {
    unsigned long long h = 1469598103934665603ULL;
    for (size_t i = 0; i < name.size(); ++i) {
        h ^= (unsigned long long)(unsigned short)name[i];
        h *= 1099511628211ULL;
    }
    unsigned long long s = h | 1ULL;
    const wchar_t* hex = L"0123456789abcdef";
    std::wstring out;
    out.reserve(32);
    for (int i = 0; i < 32; ++i) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        out.push_back(hex[(size_t)(s & 0xF)]);
    }
    return out;
}

struct LaunchPlan {
    bool ok = false;
    std::wstring id, jarId, mainClass, assetIndex, versionType, nativesDir, command;
    std::vector<std::wstring> classpath;     // 绝对路径（含 client jar）
    std::vector<std::wstring> nativeJars;
};

static LaunchPlan g_plan;

static bool ResolveVersion(const std::wstring& gameDir, const std::wstring& id, LaunchPlan& plan,
                           std::wstring& err, int& libCount) {
    // ---- 1) 沿 inheritsFrom 读出整条继承链（leaf -> root）----
    std::vector<JVal> chain;
    std::wstring cur = id;
    for (int depth = 0; depth < 8 && !cur.empty(); ++depth) {
        std::wstring p = gameDir + L"\\versions\\" + cur + L"\\" + cur + L".json";
        std::string txt = ReadFileUtf8(p);
        if (txt.empty()) { err = L"找不到版本清单 " + cur + L".json"; return false; }
        JVal v;
        if (!ParseJsonUtf8(txt, v) || v.t != JVal::OBJ) {
            err = L"版本清单解析失败：" + cur + L".json";
            return false;
        }
        chain.push_back(v);
        cur = v.str(L"inheritsFrom");
    }

    // ---- 2) 从 root 往 leaf 合并：子版本的字段覆盖父版本 ----
    std::wstring mainClass, assetIndex, legacyArgs, jarId, versionType;
    std::vector<std::wstring> gameArgs, jvmArgs;
    std::vector<LibEntry> libs;

    for (int k = (int)chain.size() - 1; k >= 0; --k) {
        const JVal& v = chain[(size_t)k];

        std::wstring mc = v.str(L"mainClass");
        if (!mc.empty()) mainClass = mc;

        std::wstring t = v.str(L"type");
        if (!t.empty()) versionType = t;

        const JVal* ai = v.get(L"assetIndex");
        std::wstring aiId = ai ? ai->str(L"id") : v.str(L"assets");
        if (!aiId.empty()) assetIndex = aiId;

        std::wstring la = v.str(L"minecraftArguments");
        if (!la.empty()) legacyArgs = la;

        std::wstring jar = v.str(L"jar");
        if (!jar.empty()) jarId = jar;

        CollectLibs(v, libs);

        const JVal* args = v.get(L"arguments");
        if (args) {
            ExpandArgs(args->get(L"game"), gameArgs);
            ExpandArgs(args->get(L"jvm"), jvmArgs);
        }
    }

    if (mainClass.empty()) { err = L"版本清单缺少 mainClass"; return false; }
    if (gameArgs.empty() && legacyArgs.empty()) { err = L"版本清单缺少启动参数"; return false; }

    plan.id          = id;
    plan.jarId       = jarId.empty() ? id : jarId;
    plan.mainClass   = mainClass;
    plan.assetIndex  = assetIndex.empty() ? id : assetIndex;
    plan.versionType = versionType.empty() ? L"release" : versionType;
    plan.nativesDir  = gameDir + L"\\versions\\" + id + L"\\natives";

    for (size_t i = 0; i < libs.size(); ++i) {
        std::wstring abs = gameDir + L"\\libraries\\" + libs[i].rel;
        if (libs[i].native) plan.nativeJars.push_back(abs);
        else                plan.classpath.push_back(abs);
    }
    plan.classpath.push_back(gameDir + L"\\versions\\" + plan.jarId + L"\\" + plan.jarId + L".jar");
    libCount = (int)plan.classpath.size() + (int)plan.nativeJars.size();

    // ---- 3) 拼装命令行 ----
    std::wstring java = g_app.javaPath.empty() ? L"java.exe" : g_app.javaPath;
    // 登录了微软账号就用真令牌，否则退回离线占位值
    const bool msa = (g_ms.state == MS_LOGGED_IN && !g_ms.accessToken.empty());
    std::wstring user = msa ? g_ms.name
                            : (g_offlineName.empty() ? std::wstring(L"Player") : g_offlineName);
    const std::wstring token = msa ? g_ms.accessToken : std::wstring(L"0");

    std::wstring cp;                          // natives 也放进 classpath：
    for (size_t i = 0; i < plan.classpath.size(); ++i) {   // LWJGL3 会自行从 jar 内解压原生库，
        if (!cp.empty()) cp += L";";                       // 这样免去自写 zip 解压
        cp += plan.classpath[i];
    }
    for (size_t i = 0; i < plan.nativeJars.size(); ++i) { cp += L";"; cp += plan.nativeJars[i]; }

    VarMap vars;
    vars.push_back(std::make_pair(L"auth_player_name",  user));
    vars.push_back(std::make_pair(L"version_name",      id));
    vars.push_back(std::make_pair(L"game_directory",    gameDir));
    vars.push_back(std::make_pair(L"assets_root",       gameDir + L"\\assets"));
    vars.push_back(std::make_pair(L"assets_index_name", plan.assetIndex));
    vars.push_back(std::make_pair(L"auth_uuid",         msa ? g_ms.uuid : OfflineUuid(user)));
    vars.push_back(std::make_pair(L"auth_access_token", token));
    vars.push_back(std::make_pair(L"auth_session",      token));
    vars.push_back(std::make_pair(L"user_type",         std::wstring(msa ? L"msa" : L"legacy")));
    vars.push_back(std::make_pair(L"version_type",      plan.versionType));
    vars.push_back(std::make_pair(L"natives_directory", plan.nativesDir));
    vars.push_back(std::make_pair(L"library_directory", gameDir + L"\\libraries"));
    vars.push_back(std::make_pair(L"classpath",         cp));
    vars.push_back(std::make_pair(L"classpath_separator", std::wstring(L";")));
    vars.push_back(std::make_pair(L"launcher_name",     std::wstring(L"DreamMinecraftLauncher")));
    vars.push_back(std::make_pair(L"launcher_version",  std::wstring(L"1.0")));
    vars.push_back(std::make_pair(L"clientid",          std::wstring(L"")));
    vars.push_back(std::make_pair(L"auth_xuid",         msa ? g_ms.xuid : std::wstring(L"")));

    plan.command = QuoteArg(java);
    plan.command += L" -Xmx" + std::to_wstring(g_app.memGb) + L"G";
    plan.command += L" -Xms" + std::to_wstring(std::max(1, g_app.memGb / 2)) + L"G";
    // 设置页里填的附加参数：原样拼进去（带引号的 -Dxxx="a b" 也能用），不逐段转义
    if (!g_app.jvmArgs.empty()) plan.command += L" " + g_app.jvmArgs;

    if (jvmArgs.empty()) {
        plan.command += L" -Djava.library.path=" + QuoteArg(plan.nativesDir);
        plan.command += L" -cp " + QuoteArg(cp);
    } else {
        for (size_t i = 0; i < jvmArgs.size(); ++i)
            plan.command += L" " + QuoteArg(Subst(jvmArgs[i], vars));
    }

    plan.command += L" " + plan.mainClass;

    if (!gameArgs.empty()) {
        for (size_t i = 0; i < gameArgs.size(); ++i)
            plan.command += L" " + QuoteArg(Subst(gameArgs[i], vars));
    } else {
        // 旧版 minecraftArguments：空格分隔，占位符替换后再按空格拆分
        std::wstring rest = legacyArgs;
        size_t p = 0;
        while (p < rest.size()) {
            while (p < rest.size() && rest[p] == L' ') ++p;
            size_t q = rest.find(L' ', p);
            if (q == std::wstring::npos) q = rest.size();
            if (q > p) plan.command += L" " + QuoteArg(Subst(rest.substr(p, q - p), vars));
            p = q;
        }
    }

    plan.ok = true;
    return true;
}

// ============================================================================
//  进程拉起 + stdout/stderr 回灌日志
// ============================================================================
static HANDLE g_hChild = nullptr;

struct PipeCtx { HANDLE pipe; HWND hwnd; };

static DWORD WINAPI PipeReader(LPVOID param) {
    PipeCtx* ctx = (PipeCtx*)param;
    std::string acc;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(ctx->pipe, buf, sizeof(buf) - 1, &n, nullptr) && n > 0) {
        buf[n] = '\0';
        acc.append(buf, n);
        size_t pos;
        while ((pos = acc.find('\n')) != std::string::npos) {
            std::string line = acc.substr(0, pos);
            acc.erase(0, pos + 1);
            if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
            if (!line.empty())
                PostMessageW(ctx->hwnd, WM_APP_LOG, 0, (LPARAM)new std::wstring(DecodeChild(line)));
        }
        if (acc.size() > 16384) acc.clear();
    }
    if (!acc.empty())
        PostMessageW(ctx->hwnd, WM_APP_LOG, 0, (LPARAM)new std::wstring(DecodeChild(acc)));
    CloseHandle(ctx->pipe);
    delete ctx;
    return 0;
}

static bool SpawnGame(const std::wstring& cmd, const std::wstring& cwd, std::wstring& err) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) { err = L"创建输出管道失败"; return false; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError  = wr;
    si.hStdInput  = nul;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');

    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                            nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    DWORD lastErr = GetLastError();

    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);

    if (!ok) {
        CloseHandle(rd);
        err = L"CreateProcess 失败（错误码 " + std::to_wstring((unsigned)lastErr) + L"）";
        return false;
    }

    if (g_hChild) { CloseHandle(g_hChild); g_hChild = nullptr; }
    g_hChild = pi.hProcess;
    CloseHandle(pi.hThread);

    PipeCtx* ctx = new PipeCtx{ rd, g_app.hwnd };
    HANDLE th = CreateThread(nullptr, 0, PipeReader, ctx, 0, nullptr);
    if (th) CloseHandle(th);
    return true;
}

// ============================================================================
//  启动流程（真实拉起 Java）
//  ---------------------------------------------------------------------------
//  已实现：version.json 继承链解析、rules(OS) 过滤、libraries 归类路径、
//          natives 处理、占位符替换、CreateProcessW 拉起、stdout 回灌日志。
//  未实现（见 README「已知限制」）：微软账号鉴权（当前为离线模式）、
//          libraries / assets 缺失文件的自动下载、assets 完整性校验。
// ============================================================================
static std::wstring EffectiveGameDir() {
    if (!g_app.gameDir.empty()) return g_app.gameDir;
    std::wstring d = DefaultMinecraftDir();
    return d.empty() ? AppDataMinecraftDir() : d;
}

static std::wstring CurrentVersionId() {
    if (g_app.versions.empty()) return L"";
    int idx = g_app.versionIdx;
    if (idx < 0 || idx >= (int)g_app.versions.size()) idx = 0;
    return g_app.versions[(size_t)idx];
}

// 统计缺失文件，返回缺失数量
static int CountMissing(const LaunchPlan& plan, std::wstring& firstMissing) {
    int missing = 0;
    for (size_t i = 0; i < plan.classpath.size(); ++i) {
        if (!PathExists(plan.classpath[i])) {
            if (missing == 0) firstMissing = plan.classpath[i];
            ++missing;
        }
    }
    for (size_t i = 0; i < plan.nativeJars.size(); ++i) {
        if (!PathExists(plan.nativeJars[i])) {
            if (missing == 0) firstMissing = plan.nativeJars[i];
            ++missing;
        }
    }
    return missing;
}

static const wchar_t* kLaunchSteps[] = {
    L"校验账户信息（离线模式）…",
    L"读取版本清单 version.json …",
    L"检查依赖文件 libraries …",
    L"整理类路径与启动参数 …",
    L"启动 Java 进程 …"
};
static const int kLaunchStepCount = 5;
static int g_stepLibCount = 0;

static void AbortLaunch(const std::wstring& why) {
    g_app.launching = false;
    g_app.launched  = false;
    g_app.progress  = 0.0f;
    g_app.progressTarget = 0.0f;
    g_app.step      = 0;
    g_app.resetTick = 0;
    PushLog(L"启动失败：" + why);
}

// 执行第 step 步的真实工作，返回 false 表示中止
static bool RunLaunchStep(int step) {
    std::wstring gameDir = EffectiveGameDir();

    switch (step) {
    case 0: {
        if (g_app.javaPath.empty()) {
            AbortLaunch(L"未检测到 Java，请安装 JRE/JDK 或设置 JAVA_HOME");
            return false;
        }
        const bool msa = (g_ms.state == MS_LOGGED_IN && !g_ms.accessToken.empty());
        PushLog((msa ? L"微软账户 · " : L"离线账户 · ") + (msa ? g_ms.name : g_offlineName));
        return true;
    }
    case 1: {
        std::wstring id = CurrentVersionId();
        if (id.empty())       { AbortLaunch(L"没有可用的游戏版本"); return false; }
        if (gameDir.empty())  { AbortLaunch(L"未找到 .minecraft 游戏目录"); return false; }

        std::wstring err;
        int libCount = 0;
        if (!ResolveVersion(gameDir, id, g_plan, err, libCount)) {
            AbortLaunch(err);
            return false;
        }
        g_stepLibCount = libCount;
        PushLog(L"版本 " + id + L" · " + g_plan.mainClass);
        return true;
    }
    case 2: {
        std::wstring first;
        int missing = CountMissing(g_plan, first);
        if (missing > 0) {
            AbortLaunch(L"缺少 " + std::to_wstring(missing) +
                        L" 个依赖文件，请先用官方启动器安装该版本");
            PushLog(first);
            return false;
        }
        std::wstring idxPath = gameDir + L"\\assets\\indexes\\" + g_plan.assetIndex + L".json";
        if (PathExists(idxPath))
            PushLog(L"依赖文件完整 · 共 " + std::to_wstring(g_stepLibCount) + L" 项");
        else
            PushLog(L"警告：缺少资源索引 " + g_plan.assetIndex + L".json（可进游戏但无音效/贴图）");
        return true;
    }
    case 3: {
        PushLog(L"类路径 " + std::to_wstring(g_plan.classpath.size()) + L" 项 · natives " +
                std::to_wstring(g_plan.nativeJars.size()) + L" 项");
        return true;
    }
    case 4: {
        CreateDirectoryW(g_plan.nativesDir.c_str(), nullptr);

        // 把最终命令行落盘，便于排查启动问题
        CreateDirectoryW(AppDataConfigDir().c_str(), nullptr);
        FILE* fp = _wfopen((AppDataConfigDir() + L"\\last-command.txt").c_str(), L"wb");
        if (fp) {
            std::string u8 = W2U8(g_plan.command);
            fwrite(u8.data(), 1, u8.size(), fp);
            fclose(fp);
        }

        std::wstring err;
        if (!SpawnGame(g_plan.command, gameDir, err)) {
            AbortLaunch(err);
            return false;
        }
        PushLog(L"Java 已拉起 · 等待游戏窗口 …");
        return true;
    }
    }
    return true;
}

static void StartLaunch() {
    if (g_app.launching) return;
    if (g_ms.state == MS_WAITING || g_ms.state == MS_WORKING) {
        PushLog(L"微软账号正在登录，请稍候；或点「离线登录」先用离线模式");
        return;
    }
    if (g_ms.state != MS_LOGGED_IN && g_offlineName.empty()) {
        PushLog(L"请先填写玩家昵称");
        g_app.focus = FOCUS_USER;
        return;
    }
    if (g_hChild) { CloseHandle(g_hChild); g_hChild = nullptr; }

    g_app.launching = true;
    g_app.launched  = false;
    g_app.progress  = 0.0f;
    g_app.progressTarget = 0.0f;
    g_app.step      = 0;
    g_app.resetTick = 0;
    for (int i = 0; i < kLaunchStepCount; ++i) g_app.stepRan[i] = false;
    g_app.logs.clear();
    g_plan = LaunchPlan();
    PushLog(kLaunchSteps[0]);
    // 16ms 一帧单独跑进度条动画，避免跟着 100ms 的业务定时器一顿一顿地跳
    if (g_app.hwnd) SetTimer(g_app.hwnd, 4, 16, nullptr);
}

static void FinishLaunch() {
    g_app.launching = false;
    g_app.progress  = 100.0f;
    // 进程若已提前退出（启动失败），退出提示已经打过，这里不再谎报「已启动」
    if (g_hChild) {
        g_app.launched  = true;
        g_app.resetTick = 30;    // 约 3 秒后按钮复位
        PushLog(L"已启动 · 游戏输出会显示在这里");
    } else {
        g_app.launched  = false;
        g_app.resetTick = 0;
    }
    SaveConfig();
}

// 启动状态机一步：跑当前步骤的真实工作，或在进度动画跑满后推进下一步。
// 由 100ms 业务定时器和 16ms 动画定时器共同驱动，靠 stepRan 幂等，不会重复执行。
// 返回 true 表示状态/日志有变化，需要整窗重绘。
static bool LaunchTick() {
    if (!g_app.launching) return false;
    const int step = g_app.step;
    if (!g_app.stepRan[step]) {
        if (!RunLaunchStep(step)) return true;   // 失败时 AbortLaunch 已重置状态
        g_app.stepRan[step] = true;
        g_app.progressTarget = (float)(step + 1) * 20.0f;
        return false;
    }
    if (g_app.progress < g_app.progressTarget - 0.001f) return false;
    if (step + 1 >= kLaunchStepCount) {
        FinishLaunch();
    } else {
        g_app.step = step + 1;
        PushLog(kLaunchSteps[step + 1]);
    }
    return true;
}

// ============================================================================
//  微软账号
//  ---------------------------------------------------------------------------
//  流程：设备码授权 → Xbox Live 认证 → XSTS 授权 → Minecraft Services 登录 → 取档案
//  选设备码而不是内嵌浏览器：不需要 localhost 回调、不需要 WebView2，
//  单个 exe 也能跑完；用户只需在浏览器里输一次 8 位码。
// ============================================================================
static const wchar_t* kMsClientId = L"00000000402b5328";
static const wchar_t* kMsScope    = L"XboxLive.signin offline_access";

// ---- 小工具：URL 编码 / JSON 转义 / Base64 ----
static std::string UrlEnc(const std::string& u) {
    static const char* hx = "0123456789ABCDEF";
    std::string o;
    o.reserve(u.size() * 3);
    for (size_t i = 0; i < u.size(); ++i) {
        unsigned char c = (unsigned char)u[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            o.push_back((char)c);
        } else {
            o.push_back('%');
            o.push_back(hx[c >> 4]);
            o.push_back(hx[c & 0x0F]);
        }
    }
    return o;
}

static std::string JsonEsc(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '"' || c == '\\') { o.push_back('\\'); o.push_back(c); }
        else if ((unsigned char)c < 0x20) { char b[8]; sprintf(b, "\\u%04x", c); o += b; }
        else o.push_back(c);
    }
    return o;
}

static std::string B64Enc(const std::vector<BYTE>& in) {
    static const char* tb =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    size_t i = 0;
    while (i + 2 < in.size()) {
        unsigned v = ((unsigned)in[i] << 16) | ((unsigned)in[i + 1] << 8) | in[i + 2];
        o.push_back(tb[(v >> 18) & 63]); o.push_back(tb[(v >> 12) & 63]);
        o.push_back(tb[(v >> 6) & 63]);  o.push_back(tb[v & 63]);
        i += 3;
    }
    if (i + 1 == in.size()) {
        unsigned v = (unsigned)in[i] << 16;
        o.push_back(tb[(v >> 18) & 63]); o.push_back(tb[(v >> 12) & 63]);
        o += "==";
    } else if (i + 2 == in.size()) {
        unsigned v = ((unsigned)in[i] << 16) | ((unsigned)in[i + 1] << 8);
        o.push_back(tb[(v >> 18) & 63]); o.push_back(tb[(v >> 12) & 63]);
        o.push_back(tb[(v >> 6) & 63]);  o.push_back('=');
    }
    return o;
}

static std::vector<BYTE> B64Dec(const std::string& s) {
    std::vector<BYTE> o;
    int val = 0, bits = -8;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        int d;
        if (c >= 'A' && c <= 'Z')      d = c - 'A';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 26;
        else if (c >= '0' && c <= '9') d = c - '0' + 52;
        else if (c == '+')             d = 62;
        else if (c == '/')             d = 63;
        else continue;
        val = (val << 6) | d;
        bits += 6;
        if (bits >= 0) { o.push_back((BYTE)((val >> bits) & 0xFF)); bits -= 8; }
    }
    return o;
}

// ---- HTTPS（WinHTTP） ----
struct HttpResp { int status = 0; std::string body; std::wstring err; };

static HttpResp HttpCall(const std::wstring& verb, const std::wstring& url,
                         const std::string& body, const std::wstring& contentType,
                         const std::wstring& bearer) {
    HttpResp r;

    URL_COMPONENTS uc;
    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256]  = {0};
    wchar_t path[2048] = {0};
    uc.lpszHostName      = host; uc.dwHostNameLength = 255;
    uc.lpszUrlPath       = path; uc.dwUrlPathLength  = 2047;
    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc)) {
        r.err = L"URL 解析失败";
        return r;
    }
    const bool secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    HINTERNET ses = WinHttpOpen(L"DreamMinecraftLauncher/0.2",
                                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) { r.err = L"初始化网络失败"; return r; }
    WinHttpSetTimeouts(ses, 15000, 15000, 30000, 30000);

    HINTERNET con = nullptr, req = nullptr;
    do {
        con = WinHttpConnect(ses, host, uc.nPort, 0);
        if (!con) { r.err = L"无法连接 " + std::wstring(host); break; }

        req = WinHttpOpenRequest(con, verb.c_str(), path, nullptr, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 secure ? WINHTTP_FLAG_SECURE : 0);
        if (!req) { r.err = L"创建请求失败"; break; }

        std::wstring hdr;
        if (!contentType.empty()) hdr += L"Content-Type: " + contentType + L"\r\n";
        if (!bearer.empty())      hdr += L"Authorization: Bearer " + bearer + L"\r\n";
        hdr += L"Accept: application/json\r\n";

        if (!WinHttpSendRequest(req, hdr.c_str(), (DWORD)-1L,
                                body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
                                (DWORD)body.size(), (DWORD)body.size(), 0)) {
            r.err = L"发送请求失败（" + std::to_wstring(GetLastError()) + L"）";
            break;
        }
        if (!WinHttpReceiveResponse(req, nullptr)) {
            r.err = L"接收响应失败（" + std::to_wstring(GetLastError()) + L"）";
            break;
        }

        DWORD st = 0, stLen = sizeof(st);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &st, &stLen, WINHTTP_NO_HEADER_INDEX);
        r.status = (int)st;

        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
            std::string chunk;
            chunk.resize(avail);
            DWORD got = 0;
            if (!WinHttpReadData(req, &chunk[0], avail, &got) || got == 0) break;
            chunk.resize(got);
            r.body += chunk;
            if (r.body.size() > (4u << 20)) break;      // 防御：响应不会这么大
        }
    } while (0);

    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return r;
}

// ---- refresh token 加密落盘（DPAPI，只能本机本用户解开） ----
static std::wstring AccountPath() { return AppDataConfigDir() + L"\\account.dat"; }

static bool DpapiProtect(const std::wstring& plain, std::vector<BYTE>& out) {
    std::string u8 = W2U8(plain);
    DATA_BLOB in;
    in.pbData = (BYTE*)u8.data();
    in.cbData = (DWORD)u8.size();
    DATA_BLOB enc;
    ZeroMemory(&enc, sizeof(enc));
    if (!CryptProtectData(&in, L"DML refresh token", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &enc)) return false;
    out.assign(enc.pbData, enc.pbData + enc.cbData);
    LocalFree(enc.pbData);
    return true;
}

static bool DpapiUnprotect(const std::vector<BYTE>& in, std::wstring& plain) {
    if (in.empty()) return false;
    DATA_BLOB src;
    src.pbData = (BYTE*)in.data();
    src.cbData = (DWORD)in.size();
    DATA_BLOB dec;
    ZeroMemory(&dec, sizeof(dec));
    if (!CryptUnprotectData(&src, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &dec)) return false;
    std::string u8((char*)dec.pbData, dec.cbData);
    LocalFree(dec.pbData);
    plain = U82W(u8);
    return true;
}

static void SaveAccount(const std::wstring& refresh, const std::wstring& name,
                        const std::wstring& uuid, const std::wstring& xuid) {
    std::vector<BYTE> blob;
    if (!DpapiProtect(refresh, blob)) return;
    CreateDirectoryW(AppDataConfigDir().c_str(), nullptr);
    std::ofstream f(W2U8(AccountPath()).c_str(), std::ios::binary);
    if (!f) return;
    f << "v1\n" << W2U8(name) << "\n" << W2U8(uuid) << "\n" << W2U8(xuid) << "\n"
      << B64Enc(blob) << "\n";
}

static bool LoadAccount(std::wstring& refresh, std::wstring& name,
                        std::wstring& uuid, std::wstring& xuid) {
    std::ifstream f(W2U8(AccountPath()).c_str(), std::ios::binary);
    if (!f) return false;
    std::vector<std::string> ln;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        ln.push_back(line);
    }
    if (ln.size() < 5 || ln[0] != "v1") return false;
    name = U82W(ln[1]);
    uuid = U82W(ln[2]);
    xuid = U82W(ln[3]);
    std::vector<BYTE> blob = B64Dec(ln[4]);
    return DpapiUnprotect(blob, refresh) && !refresh.empty();
}

static void ClearAccountFile() { DeleteFileW(AccountPath().c_str()); }

// ---- 各段接口 ----
static bool MsDeviceCode(std::wstring& userCode, std::wstring& uri,
                         std::wstring& deviceCode, int& interval, std::wstring& err) {
    const std::string body = "client_id=" + UrlEnc(W2U8(kMsClientId)) +
                             "&scope=" + UrlEnc(W2U8(kMsScope)) +
                             "&response_type=device_code";
    HttpResp r = HttpCall(L"POST", L"https://login.live.com/oauth20_connect.srf",
                          body, L"application/x-www-form-urlencoded", L"");
    if (r.status != 200) {
        err = L"申请登录码失败（HTTP " + std::to_wstring(r.status) + L"）" +
              (r.err.empty() ? L"" : L"：" + r.err);
        return false;
    }
    JVal j;
    if (!ParseJsonUtf8(r.body, j)) { err = L"登录码响应无法解析"; return false; }

    userCode   = j.str(L"user_code");
    uri        = j.str(L"verification_uri");
    deviceCode = j.str(L"device_code");
    const JVal* iv = j.get(L"interval");
    interval = (iv && iv->t == JVal::NUM && iv->num >= 1.0) ? (int)iv->num : 5;
    if (uri.empty()) uri = L"https://www.microsoft.com/link";
    if (userCode.empty() || deviceCode.empty()) { err = L"登录码响应字段缺失"; return false; }
    return true;
}

// 1 = 成功 / 0 = 等待 / -3 = 需要放慢 / -2 = 失败
static int MsPollToken(const std::wstring& deviceCode, std::wstring& refresh,
                       std::wstring& msToken, std::wstring& err) {
    const std::string body = "client_id=" + UrlEnc(W2U8(kMsClientId)) +
                             "&device_code=" + UrlEnc(W2U8(deviceCode)) +
                             "&grant_type=" +
                             UrlEnc("urn:ietf:params:oauth:grant-type:device_code");
    HttpResp r = HttpCall(L"POST", L"https://login.live.com/oauth20_token.srf",
                          body, L"application/x-www-form-urlencoded", L"");
    JVal j;
    const bool parsed = ParseJsonUtf8(r.body, j);
    if (r.status == 200 && parsed) {
        refresh = j.str(L"refresh_token");
        msToken = j.str(L"access_token");
        if (refresh.empty()) { err = L"令牌响应缺少 refresh_token"; return -2; }
        return 1;
    }
    const std::wstring e = parsed ? j.str(L"error") : L"";
    if (e == L"authorization_pending")  return 0;
    if (e == L"slow_down")              return -3;
    if (e == L"expired_token")          { err = L"登录码已过期，请重新点击登录"; return -2; }
    if (e == L"authorization_declined") { err = L"浏览器里取消了授权"; return -2; }
    err = L"获取令牌失败（HTTP " + std::to_wstring(r.status) + L"）" +
          (e.empty() ? L"" : L"：" + e);
    return -2;
}

static bool MsRefreshGrant(const std::wstring& refreshIn, std::wstring& msToken,
                           std::wstring& refreshOut, std::wstring& err) {
    const std::string body = "client_id=" + UrlEnc(W2U8(kMsClientId)) +
                             "&refresh_token=" + UrlEnc(W2U8(refreshIn)) +
                             "&grant_type=refresh_token&scope=" + UrlEnc(W2U8(kMsScope));
    HttpResp r = HttpCall(L"POST", L"https://login.live.com/oauth20_token.srf",
                          body, L"application/x-www-form-urlencoded", L"");
    if (r.status != 200) {
        err = L"刷新登录态失败（HTTP " + std::to_wstring(r.status) + L"）";
        return false;
    }
    JVal j;
    if (!ParseJsonUtf8(r.body, j)) { err = L"刷新响应无法解析"; return false; }
    msToken    = j.str(L"access_token");
    refreshOut = j.str(L"refresh_token");
    if (refreshOut.empty()) refreshOut = refreshIn;
    if (msToken.empty()) { err = L"刷新响应缺少 access_token"; return false; }
    return true;
}

static bool MsXblAuth(const std::wstring& msToken, std::wstring& xbl,
                      std::wstring& uhs, std::wstring& err) {
    const std::string json =
        "{\"Properties\":{\"AuthMethod\":\"RPS\","
        "\"SiteName\":\"user.auth.xboxlive.com\","
        "\"RpsTicket\":\"d=" + JsonEsc(W2U8(msToken)) + "\"},"
        "\"RelyingParty\":\"http://auth.xboxlive.com\",\"TokenType\":\"JWT\"}";
    HttpResp r = HttpCall(L"POST", L"https://user.auth.xboxlive.com/user/authenticate",
                          json, L"application/json", L"");
    if (r.status != 200) {
        err = L"Xbox Live 认证失败（HTTP " + std::to_wstring(r.status) + L"）";
        return false;
    }
    JVal j;
    if (!ParseJsonUtf8(r.body, j)) { err = L"XBL 响应无法解析"; return false; }
    xbl = j.str(L"Token");
    const JVal* dc  = j.get(L"DisplayClaims");
    const JVal* xui = dc ? dc->get(L"xui") : nullptr;
    if (xui && xui->t == JVal::ARR && !xui->a.empty()) uhs = xui->a[0].str(L"uhs");
    if (xbl.empty() || uhs.empty()) { err = L"XBL 响应缺少 Token / uhs"; return false; }
    return true;
}

static bool MsXsts(const std::wstring& xbl, std::wstring& xsts, std::wstring& uhs,
                   std::wstring& xuid, std::wstring& err) {
    const std::string json =
        "{\"Properties\":{\"SandboxId\":\"RETAIL\",\"UserTokens\":[\"" +
        JsonEsc(W2U8(xbl)) + "\"]},"
        "\"RelyingParty\":\"rp://api.minecraftservices.com/\",\"TokenType\":\"JWT\"}";
    HttpResp r = HttpCall(L"POST", L"https://xsts.auth.xboxlive.com/xsts/authorize",
                          json, L"application/json", L"");
    if (r.status != 200) {
        JVal j;
        if (ParseJsonUtf8(r.body, j)) {
            const JVal* xe = j.get(L"XErr");
            const unsigned long long code =
                (xe && xe->t == JVal::NUM) ? (unsigned long long)xe->num : 0ULL;
            if (code == 2148916233ULL) {
                err = L"该微软账号还没有 Xbox 档案，请先到 xbox.com 建一个";
                return false;
            }
            if (code == 2148916235ULL) { err = L"该账号所在地区不支持 Xbox Live"; return false; }
            if (code == 2148916238ULL) { err = L"未成年账号需要先加入家庭组"; return false; }
        }
        err = L"XSTS 授权失败（HTTP " + std::to_wstring(r.status) + L"）";
        return false;
    }
    JVal j;
    if (!ParseJsonUtf8(r.body, j)) { err = L"XSTS 响应无法解析"; return false; }
    xsts = j.str(L"Token");
    const JVal* dc  = j.get(L"DisplayClaims");
    const JVal* xui = dc ? dc->get(L"xui") : nullptr;
    if (xui && xui->t == JVal::ARR && !xui->a.empty()) {
        uhs  = xui->a[0].str(L"uhs");
        xuid = xui->a[0].str(L"xid");
    }
    if (xsts.empty() || uhs.empty()) { err = L"XSTS 响应缺少 Token / uhs"; return false; }
    return true;
}

static bool MsMcLogin(const std::wstring& xsts, const std::wstring& uhs,
                      std::wstring& mcToken, std::wstring& err) {
    const std::string json = "{\"identityToken\":\"XBL3.0 x=" + JsonEsc(W2U8(uhs)) +
                             ";" + JsonEsc(W2U8(xsts)) + "\"}";
    HttpResp r = HttpCall(L"POST",
                          L"https://api.minecraftservices.com/authentication/login_with_xbox",
                          json, L"application/json", L"");
    if (r.status != 200) {
        err = L"Minecraft 登录失败（HTTP " + std::to_wstring(r.status) + L"）";
        return false;
    }
    JVal j;
    if (!ParseJsonUtf8(r.body, j)) { err = L"Minecraft 登录响应无法解析"; return false; }
    mcToken = j.str(L"access_token");
    if (mcToken.empty()) { err = L"Minecraft 登录响应缺少 access_token"; return false; }
    return true;
}

static bool MsProfile(const std::wstring& mcToken, std::wstring& name,
                      std::wstring& uuid, std::wstring& err) {
    HttpResp r = HttpCall(L"GET", L"https://api.minecraftservices.com/minecraft/profile",
                          std::string(), L"", mcToken);
    if (r.status == 404) {
        err = L"这个账号没有 Minecraft: Java Edition";
        return false;
    }
    if (r.status != 200) {
        err = L"读取游戏档案失败（HTTP " + std::to_wstring(r.status) + L"）";
        return false;
    }
    JVal j;
    if (!ParseJsonUtf8(r.body, j)) { err = L"档案响应无法解析"; return false; }
    name = j.str(L"name");
    uuid = j.str(L"id");
    // MC 命令行要的是不带横线的 uuid
    std::wstring plain;
    for (size_t i = 0; i < uuid.size(); ++i) if (uuid[i] != L'-') plain.push_back(uuid[i]);
    uuid = plain;
    if (name.empty() || uuid.empty()) { err = L"档案响应缺少 name / id"; return false; }
    return true;
}

// ---- 线程之间传结果 ----
static CRITICAL_SECTION g_msLock;
static bool g_msLockReady = false;

struct MsShared {
    bool ok = false;
    bool silent = false;
    std::wstring err, name, uuid, xuid, mcToken, refresh, userCode, uri;
};
static MsShared g_msShared;

static void InitMsLock() {
    if (g_msLockReady) return;
    InitializeCriticalSection(&g_msLock);
    g_msLockReady = true;
}

static void PublishMs(const MsShared& s) {
    EnterCriticalSection(&g_msLock);
    g_msShared = s;
    LeaveCriticalSection(&g_msLock);
    if (g_app.hwnd) PostMessageW(g_app.hwnd, WM_APP_MS, 2, 0);
}

static void MsThreadLog(const std::wstring& s) {
    if (g_app.hwnd) PostMessageW(g_app.hwnd, WM_APP_LOG, 0, (LPARAM)new std::wstring(s));
}

// XBL → XSTS → Minecraft 登录 → 档案，一条链跑完
static bool MsChain(const std::wstring& msToken, MsShared& out, std::wstring& err) {
    std::wstring xbl, uhs, xsts, xuid, mc, name, uuid;
    if (!MsXblAuth(msToken, xbl, uhs, err))       return false;
    if (!MsXsts(xbl, xsts, uhs, xuid, err))       return false;
    if (!MsMcLogin(xsts, uhs, mc, err))           return false;
    if (!MsProfile(mc, name, uuid, err))          return false;
    out.ok = true;
    out.mcToken = mc;
    out.name = name;
    out.uuid = uuid;
    out.xuid = xuid;
    return true;
}

static DWORD WINAPI MsLoginThread(LPVOID) {
    MsShared sh;
    std::wstring err;

    std::wstring userCode, uri, deviceCode;
    int interval = 5;
    if (!MsDeviceCode(userCode, uri, deviceCode, interval, err)) {
        sh.err = err;
        PublishMs(sh);
        return 0;
    }

    EnterCriticalSection(&g_msLock);
    g_msShared.userCode = userCode;
    g_msShared.uri      = uri;
    LeaveCriticalSection(&g_msLock);
    if (g_app.hwnd) PostMessageW(g_app.hwnd, WM_APP_MS, 1, 0);

    std::wstring refresh, msToken;
    const DWORD began = GetTickCount();
    bool got = false;
    for (;;) {
        Sleep((DWORD)interval * 1000);
        if (GetTickCount() - began > 15UL * 60UL * 1000UL) { err = L"等待授权超时"; break; }
        int rc = MsPollToken(deviceCode, refresh, msToken, err);
        if (rc == 1)  { got = true; break; }
        if (rc == -3) { interval += 2; continue; }
        if (rc == -2) break;
    }
    if (!got) {
        sh.err = err.empty() ? L"未拿到授权" : err;
        PublishMs(sh);
        return 0;
    }

    MsThreadLog(L"授权成功，正在换取游戏令牌 …");
    if (!MsChain(msToken, sh, err)) {
        sh.ok = false;
        sh.err = err;
        PublishMs(sh);
        return 0;
    }
    sh.refresh = refresh;
    SaveAccount(refresh, sh.name, sh.uuid, sh.xuid);
    PublishMs(sh);
    return 0;
}

// 启动时静默续期：用落盘的 refresh token 重新走一遍链
static DWORD WINAPI MsRefreshThread(LPVOID) {
    MsShared sh;
    sh.silent = true;
    std::wstring err;

    std::wstring refresh, name, uuid, xuid;
    if (!LoadAccount(refresh, name, uuid, xuid)) {
        sh.err = L"没有已保存的账号";
        PublishMs(sh);
        return 0;
    }

    std::wstring msToken, newRefresh;
    if (!MsRefreshGrant(refresh, msToken, newRefresh, err) ||
        !MsChain(msToken, sh, err)) {
        sh.ok = false;
        sh.silent = true;
        sh.err = err;
        PublishMs(sh);
        return 0;
    }
    sh.refresh = newRefresh;
    SaveAccount(newRefresh, sh.name, sh.uuid, sh.xuid);
    PublishMs(sh);
    return 0;
}

static void StartMsLogin() {
    if (g_ms.state == MS_WAITING || g_ms.state == MS_WORKING) return;
    InitMsLock();
    g_ms.err.clear();
    g_ms.userCode.clear();
    g_ms.verifyUri.clear();
    g_ms.state = MS_WAITING;
    if (g_app.hwnd) KillTimer(g_app.hwnd, 3);     // 上一轮的自动开浏览器倒计时作废
    g_app.dlgWait = 0;
    PushLog(L"正在申请微软登录码 …");
    HANDLE h = CreateThread(nullptr, 0, MsLoginThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
    InvalidateRect(g_app.hwnd, nullptr, FALSE);
}

static void LogoutMs() {
    ClearAccountFile();
    g_ms = MsAccount();
    if (g_app.hwnd) KillTimer(g_app.hwnd, 3);
    g_app.dlgWait  = 0;
    g_app.username = g_offlineName;
    g_app.focus    = FOCUS_NONE;
    PushLog(L"已退出微软账号，回到离线模式");
    SaveConfig();
    InvalidateRect(g_app.hwnd, nullptr, FALSE);
}

static void MsAutoRefresh() {
    std::wstring refresh, name, uuid, xuid;
    if (!LoadAccount(refresh, name, uuid, xuid)) return;
    InitMsLock();
    g_ms.state      = MS_WORKING;
    g_ms.name       = name;
    g_ms.uuid       = uuid;
    g_ms.refreshToken = refresh;
    g_app.username  = name;
    HANDLE h = CreateThread(nullptr, 0, MsRefreshThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
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
    StrokeRound(gr, r, SR(11), Color(36, 0, 0, 0), SR(1));
}

// ============================================================================
//  下载 Minecraft 版本（BMCLAPI 镜像）
//    /mc/game/version_manifest_v2.json   版本清单
//    /version/<id>/json                  版本 JSON
//    /version/<id>/client                客户端 jar
//    /maven/<path>                       依赖库
//    /v1/packages/<sha>/<file>.json      资源索引
//    /assets/<h2>/<hash>                 资源对象
//  文件完整性按 size 校验，已存在的文件直接跳过（天然支持断点续传）
// ============================================================================
static const wchar_t* kDlBase = L"https://bmclapi2.bangbang93.com";
static const int      kDlThreads = 16;      // 并发连接数：资源对象都是小文件，靠并发拉吞吐
static const int      DL_ROWS    = 6;       // 列表可见行数
static const int      DL_ROW_H   = 32;      // 行高（逻辑像素）

struct DlVersion {
    std::wstring id;
    std::wstring type;      // release / snapshot / old_beta / old_alpha
    std::wstring date;      // releaseTime 前 10 位
};

struct DlTask {
    std::wstring url;
    std::wstring path;      // 绝对路径
    long long    size = -1; // 期望字节数（-1 = 未知）
};

static std::vector<DlVersion> g_dlList;     // 远端版本列表（清单线程写，UI 读）
static std::vector<int>       g_dlView;     // 按筛选条件过滤后的下标
static std::wstring           g_dlErr;      // 失败原因
static std::wstring           g_dlCur;      // 当前正在下载的文件

static std::atomic<int>       g_dlFilesDone{0};
static std::atomic<int>       g_dlFilesTotal{0};
static std::atomic<long long> g_dlBytesDone{0};
static std::atomic<long long> g_dlBytesTotal{0};
static std::atomic<bool>      g_dlCancel{false};
static std::atomic<bool>      g_dlBusy{false};     // 有下载线程在跑（防止并发两份）

static CRITICAL_SECTION g_dlLock;
static bool             g_dlLockReady = false;

static void InitDlLock() {
    if (!g_dlLockReady) { InitializeCriticalSection(&g_dlLock); g_dlLockReady = true; }
}

// ---- 路径 / 文件小工具 ----
static std::wstring FileNameOf(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? p : p.substr(s + 1);
}

static std::wstring ParentDirOf(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? std::wstring() : p.substr(0, s);
}

static void MakeDirs(const std::wstring& path) {
    if (path.size() < 3) return;
    for (size_t i = 3; i <= path.size(); ++i) {
        if (i < path.size() && path[i] != L'\\' && path[i] != L'/') continue;
        CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
    }
}

static long long FileSizeOf(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa)) return -1;
    return ((long long)fa.nFileSizeHigh << 32) | (long long)fa.nFileSizeLow;
}

static bool WriteAllBytes(const std::wstring& path, const std::string& data) {
    FILE* fp = _wfopen(path.c_str(), L"wb");
    if (!fp) return false;
    const size_t n = data.empty() ? 0 : fwrite(data.data(), 1, data.size(), fp);
    fclose(fp);
    return n == data.size();
}

static std::wstring HumanSize(long long n) {
    if (n < 1024)        return std::to_wstring(n) + L" B";
    if (n < 1048576LL)   return std::to_wstring(n / 1024) + L" KB";
    wchar_t b[48];
    if (n < 1073741824LL) swprintf(b, 48, L"%.1f MB", (double)n / 1048576.0);
    else                  swprintf(b, 48, L"%.2f GB", (double)n / 1073741824.0);
    return b;
}

// 把 Mojang 官方地址换成 BMCLAPI 镜像（版本 JSON 里仍是官方 URL）
static std::wstring MirrorUrl(std::wstring u) {
    struct { const wchar_t* from; const wchar_t* to; } map[] = {
        { L"https://libraries.minecraft.net",          L"https://bmclapi2.bangbang93.com/maven"  },
        { L"https://resources.download.minecraft.net", L"https://bmclapi2.bangbang93.com/assets" },
        { L"https://piston-meta.mojang.com",           L"https://bmclapi2.bangbang93.com"        },
        { L"https://launchermeta.mojang.com",          L"https://bmclapi2.bangbang93.com"        },
        { L"https://piston-data.mojang.com",           L"https://bmclapi2.bangbang93.com"        },
        { L"https://launcher.mojang.com",              L"https://bmclapi2.bangbang93.com"        },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); ++i) {
        size_t p = u.find(map[i].from);
        if (p != std::wstring::npos)
            u.replace(p, wcslen(map[i].from), map[i].to);
    }
    return u;
}

// ---- 单文件下载：流式写 .part，完成后再改名，断点续传由「跳过已存在」实现 ----
static bool HttpDownload(const std::wstring& url, const std::wstring& dst, long long expect,
                         std::wstring& err) {
    err.clear();
    URL_COMPONENTS uc;
    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256]  = {0};
    wchar_t path[2048] = {0};
    uc.lpszHostName    = host; uc.dwHostNameLength = 255;
    uc.lpszUrlPath     = path; uc.dwUrlPathLength  = 2047;
    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc)) { err = L"URL 解析失败"; return false; }

    HINTERNET ses = WinHttpOpen(L"DreamMinecraftLauncher/0.2", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) { err = L"网络初始化失败"; return false; }
    WinHttpSetTimeouts(ses, 15000, 15000, 30000, 60000);

    bool ok = false;
    HINTERNET con = nullptr, req = nullptr;
    FILE* fp = nullptr;
    long long got = 0;
    const std::wstring tmp = dst + L".part";

    do {
        con = WinHttpConnect(ses, host, uc.nPort, 0);
        if (!con) { err = L"无法连接服务器"; break; }
        req = WinHttpOpenRequest(con, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
        if (!req) { err = L"创建请求失败"; break; }
        // 镜像会给 302 跳到 CDN，WinHTTP 默认策略会自动跟随
        if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(req, nullptr)) {
            err = L"请求失败（" + std::to_wstring(GetLastError()) + L"）";
            break;
        }
        DWORD st = 0, sl = sizeof(st);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &st, &sl, WINHTTP_NO_HEADER_INDEX);
        if (st != 200) { err = L"HTTP " + std::to_wstring(st); break; }

        MakeDirs(ParentDirOf(dst));
        fp = _wfopen(tmp.c_str(), L"wb");
        if (!fp) { err = L"无法写入文件"; break; }

        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
            std::string buf;
            buf.resize(avail);
            DWORD rd = 0;
            if (!WinHttpReadData(req, &buf[0], avail, &rd) || rd == 0) break;
            if (fwrite(buf.data(), 1, rd, fp) != rd) { err = L"写入文件失败"; break; }
            got += rd;
            g_dlBytesDone.fetch_add((long long)rd);
        }
        ok = err.empty();
    } while (0);

    if (fp) fclose(fp);
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);

    if (!ok) {
        DeleteFileW(tmp.c_str());
        if (err.empty()) err = L"下载中断";
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        err = L"重命名失败";
        return false;
    }
    if (expect > 0 && got != expect) {
        DeleteFileW(dst.c_str());
        err = L"大小不符（" + std::to_wstring(got) + L"/" + std::to_wstring(expect) + L"）";
        return false;
    }
    return true;
}

// ---- 版本列表 ----
static const wchar_t* DlTypeName(const std::wstring& t) {
    if (t == L"release")   return L"正式版";
    if (t == L"snapshot")  return L"快照";
    if (t == L"old_beta")  return L"旧 Beta";
    if (t == L"old_alpha") return L"旧 Alpha";
    return L"其他";
}

static void RebuildDlView() {
    g_dlView.clear();
    for (size_t i = 0; i < g_dlList.size(); ++i) {
        const std::wstring& t = g_dlList[i].type;
        if (g_app.dlFilter == 0 && t != L"release")  continue;
        if (g_app.dlFilter == 1 && t != L"snapshot") continue;
        g_dlView.push_back((int)i);
    }
    if (g_app.dlSel >= (int)g_dlView.size()) g_app.dlSel = (int)g_dlView.size() - 1;
    if (g_app.dlSel < 0) g_app.dlSel = 0;
    if (g_app.dlScroll > (int)g_dlView.size() - DL_ROWS)
        g_app.dlScroll = std::max(0, (int)g_dlView.size() - DL_ROWS);
}

static DWORD WINAPI DlManifestThread(LPVOID) {
    HttpResp r = HttpCall(L"GET", std::wstring(kDlBase) + L"/mc/game/version_manifest_v2.json",
                          std::string(), std::wstring(), std::wstring());
    std::vector<DlVersion> list;
    std::wstring err;

    if (r.status == 200 && !r.body.empty()) {
        JVal root;
        if (ParseJsonUtf8(r.body, root)) {
            const JVal* vs = root.get(L"versions");
            if (vs && vs->t == JVal::ARR) {
                for (size_t i = 0; i < vs->a.size(); ++i) {
                    DlVersion d;
                    d.id   = vs->a[i].str(L"id");
                    d.type = vs->a[i].str(L"type");
                    d.date = vs->a[i].str(L"releaseTime");
                    if (d.date.size() >= 10) d.date = d.date.substr(0, 10);
                    if (!d.id.empty()) list.push_back(d);
                }
            }
        }
        if (list.empty()) err = L"版本列表为空或解析失败";
    } else {
        err = r.status ? (L"获取版本列表失败（HTTP " + std::to_wstring(r.status) + L"）")
                       : (r.err.empty() ? L"获取版本列表失败" : r.err);
    }

    EnterCriticalSection(&g_dlLock);
    g_dlList.swap(list);
    g_dlErr = err;
    LeaveCriticalSection(&g_dlLock);

    if (g_app.hwnd) PostMessageW(g_app.hwnd, WM_APP_DL, 1, 0);
    return 0;
}

// ---- 下载计划：版本 JSON + 客户端 jar + 依赖库 + 资源索引 + 资源对象 ----
static void AddTask(std::vector<DlTask>& tasks, std::set<std::wstring>& seen,
                    const std::wstring& url, const std::wstring& path, long long size) {
    if (url.empty() || path.empty()) return;
    if (!seen.insert(path).second) return;
    DlTask t;
    t.url  = url;
    t.path = path;
    t.size = size;
    tasks.push_back(t);
}

static bool BuildDlPlan(const std::wstring& gameDir, const std::wstring& id,
                        std::vector<DlTask>& tasks, std::wstring& err) {
    const std::wstring base(kDlBase);
    const std::wstring vdir = gameDir + L"\\versions\\" + id;

    HttpResp r = HttpCall(L"GET", base + L"/version/" + id + L"/json",
                          std::string(), std::wstring(), std::wstring());
    if (r.status != 200 || r.body.empty()) {
        err = r.status ? (L"获取版本清单失败（HTTP " + std::to_wstring(r.status) + L"）")
                       : (r.err.empty() ? L"获取版本清单失败" : r.err);
        return false;
    }
    JVal root;
    if (!ParseJsonUtf8(r.body, root) || root.t != JVal::OBJ) { err = L"版本清单解析失败"; return false; }

    MakeDirs(vdir);
    if (!WriteAllBytes(vdir + L"\\" + id + L".json", r.body)) { err = L"版本清单写入失败"; return false; }

    std::set<std::wstring> seen;

    // 客户端 jar：直接走镜像的 /version/<id>/client，比改写 JSON 里的 URL 更稳
    long long clientSize = -1;
    if (const JVal* dl = root.get(L"downloads")) {
        const JVal* c = dl->get(L"client");
        const JVal* s = c ? c->get(L"size") : nullptr;
        if (s && s->t == JVal::NUM) clientSize = (long long)s->num;
    }
    AddTask(tasks, seen, base + L"/version/" + id + L"/client",
            vdir + L"\\" + id + L".jar", clientSize);

    // 依赖库：artifact 与 windows 分类器（老版本的 natives 在 classifiers 里）
    const JVal* libs = root.get(L"libraries");
    if (libs && libs->t == JVal::ARR) {
        for (size_t i = 0; i < libs->a.size(); ++i) {
            const JVal& lib = libs->a[i];
            if (!RulesAllow(lib.get(L"rules"))) continue;
            const std::wstring name = lib.str(L"name");
            const JVal* dl = lib.get(L"downloads");

            if (!dl) {                                  // 很老的版本：没有 downloads，按 maven 规则拼
                std::wstring rel = MavenRel(name, L"");
                if (!rel.empty()) {
                    std::wstring url = base + L"/maven/" + rel;
                    for (size_t k = 0; k < rel.size(); ++k) if (rel[k] == L'/') rel[k] = L'\\';
                    AddTask(tasks, seen, url, gameDir + L"\\libraries\\" + rel, -1);
                }
                continue;
            }

            const JVal* art = dl->get(L"artifact");
            if (art) {
                std::wstring rel = art->str(L"path"), url = art->str(L"url");
                long long sz = -1;
                if (const JVal* s = art->get(L"size")) if (s->t == JVal::NUM) sz = (long long)s->num;
                if (!rel.empty()) {
                    for (size_t k = 0; k < rel.size(); ++k) if (rel[k] == L'/') rel[k] = L'\\';
                    if (url.empty()) {
                        std::wstring mr = MavenRel(name, L"");
                        for (size_t k = 0; k < mr.size(); ++k) if (mr[k] == L'/') mr[k] = L'\\';
                        url = base + L"/maven/" + mr;
                    }
                    AddTask(tasks, seen, MirrorUrl(url), gameDir + L"\\libraries\\" + rel, sz);
                }
            }

            const JVal* cls = dl->get(L"classifiers");
            if (cls && cls->t == JVal::OBJ) {
                for (size_t k = 0; k < cls->o.size(); ++k) {
                    if (cls->o[k].first.find(L"windows") == std::wstring::npos) continue;
                    const JVal& c = cls->o[k].second;
                    std::wstring rel = c.str(L"path"), url = c.str(L"url");
                    long long sz = -1;
                    if (const JVal* s = c.get(L"size")) if (s->t == JVal::NUM) sz = (long long)s->num;
                    if (rel.empty()) continue;
                    for (size_t q = 0; q < rel.size(); ++q) if (rel[q] == L'/') rel[q] = L'\\';
                    AddTask(tasks, seen, MirrorUrl(url), gameDir + L"\\libraries\\" + rel, sz);
                }
            }
        }
    }

    // 资源索引：必须同步拿到内容才能列出资源对象
    std::wstring aiId, aiUrl;
    if (const JVal* ai = root.get(L"assetIndex")) {
        aiId  = ai->str(L"id");
        aiUrl = MirrorUrl(ai->str(L"url"));
    }
    if (!aiUrl.empty() && !aiId.empty()) {
        HttpResp ir = HttpCall(L"GET", aiUrl, std::string(), std::wstring(), std::wstring());
        if (ir.status != 200 || ir.body.empty()) { err = L"资源索引下载失败"; return false; }
        MakeDirs(gameDir + L"\\assets\\indexes");
        if (!WriteAllBytes(gameDir + L"\\assets\\indexes\\" + aiId + L".json", ir.body)) {
            err = L"资源索引写入失败";
            return false;
        }
        JVal idx;
        if (ParseJsonUtf8(ir.body, idx)) {
            const JVal* objs = idx.get(L"objects");
            if (objs && objs->t == JVal::OBJ) {
                for (size_t i = 0; i < objs->o.size(); ++i) {
                    const JVal& o = objs->o[i].second;
                    std::wstring hash = o.str(L"hash");
                    if (hash.size() < 2) continue;
                    long long sz = -1;
                    if (const JVal* s = o.get(L"size")) if (s->t == JVal::NUM) sz = (long long)s->num;
                    const std::wstring h2 = hash.substr(0, 2);
                    AddTask(tasks, seen, base + L"/assets/" + h2 + L"/" + hash,
                            gameDir + L"\\assets\\objects\\" + h2 + L"\\" + hash, sz);
                }
            }
        }
    }
    return true;
}

struct DlPool {
    std::vector<DlTask>* tasks = nullptr;
    std::atomic<size_t>  next{0};
    std::atomic<int>     fails{0};
};

static DWORD WINAPI DlWorker(LPVOID param) {
    DlPool* p = (DlPool*)param;
    for (;;) {
        if (g_dlCancel.load()) break;
        const size_t i = p->next.fetch_add(1);
        if (i >= p->tasks->size()) break;
        const DlTask& t = (*p->tasks)[i];

        const long long have = FileSizeOf(t.path);
        const bool skip = (t.size > 0) ? (have == t.size) : (have > 0);
        if (skip) {
            if (t.size > 0) g_dlBytesDone.fetch_add(t.size);
            g_dlFilesDone.fetch_add(1);
            continue;
        }

        EnterCriticalSection(&g_dlLock);
        g_dlCur = t.path;
        LeaveCriticalSection(&g_dlLock);

        std::wstring e;
        if (!HttpDownload(t.url, t.path, t.size, e)) {
            p->fails.fetch_add(1);
            EnterCriticalSection(&g_dlLock);
            if (g_dlErr.empty()) g_dlErr = FileNameOf(t.path) + L" 下载失败：" + e;
            LeaveCriticalSection(&g_dlLock);
        }
        g_dlFilesDone.fetch_add(1);
    }
    return 0;
}

static DWORD WINAPI DlThread(LPVOID param) {
    const std::wstring id = *(std::wstring*)param;
    delete (std::wstring*)param;

    g_dlCancel.store(false);
    g_dlFilesDone.store(0);
    g_dlFilesTotal.store(0);
    g_dlBytesDone.store(0);
    g_dlBytesTotal.store(0);
    EnterCriticalSection(&g_dlLock);
    g_dlErr.clear();
    g_dlCur.clear();
    LeaveCriticalSection(&g_dlLock);
    const std::wstring gameDir = EffectiveGameDir();
    std::vector<DlTask> tasks;
    std::wstring err;
    bool ok = false;
    if (gameDir.empty()) {
        err = L"无法确定游戏目录（%APPDATA% 不可用）";
    } else {
        ok = BuildDlPlan(gameDir, id, tasks, err);
    }

    if (ok) {
        long long total = 0;
        for (size_t i = 0; i < tasks.size(); ++i)
            if (tasks[i].size > 0) total += tasks[i].size;
        g_dlFilesTotal.store((int)tasks.size());
        g_dlBytesTotal.store(total);

        if (!tasks.empty()) {
            DlPool pool;
            pool.tasks = &tasks;
            HANDLE hs[kDlThreads];
            for (int i = 0; i < kDlThreads; ++i)
                hs[i] = CreateThread(nullptr, 0, DlWorker, &pool, 0, nullptr);
            for (int i = 0; i < kDlThreads; ++i) {
                if (hs[i]) { WaitForSingleObject(hs[i], INFINITE); CloseHandle(hs[i]); }
            }
            if (pool.fails.load() > 0) {
                ok = false;
                EnterCriticalSection(&g_dlLock);
                if (g_dlErr.empty())
                    g_dlErr = L"有 " + std::to_wstring(pool.fails.load()) + L" 个文件下载失败";
                LeaveCriticalSection(&g_dlLock);
            }
        }
    } else {
        EnterCriticalSection(&g_dlLock);
        g_dlErr = err;
        LeaveCriticalSection(&g_dlLock);
    }

    if (g_dlCancel.load()) {
        ok = false;
        EnterCriticalSection(&g_dlLock);
        g_dlErr = L"已取消下载";
        LeaveCriticalSection(&g_dlLock);
    }
    if (ok) g_dlBytesDone.store(g_dlBytesTotal.load());

    if (g_app.hwnd) PostMessageW(g_app.hwnd, WM_APP_DL, ok ? 2 : 3, 0);
    return 0;
}

// ---- 下载页几何（绘制与命中测试共用，避免两边算错） ----
static RectF DlPageRect() { return ComputeLayout().dlPanel; }

static RectF DlSubRect(const RectF& p, REAL x, REAL y, REAL w, REAL h) {
    return RectF(p.X + SR(x), p.Y + SR(y), SR(w), SR(h));
}

static RectF DlFilterRect(const RectF& p, int i) {
    return DlSubRect(p, 24 + i * 78, 72, 70, 26);
}
static RectF DlGoRect(const RectF& p)     { return DlSubRect(p, 704 - 24 - 110, 312, 110, 34); }
static RectF DlCancelRect(const RectF& p) { return DlSubRect(p, 704 - 24 - 110 - 10 - 96, 312, 96, 34); }

static RectF DlListRect(const RectF& p) {
    return DlSubRect(p, 24, 106, 704 - 48, DL_ROWS * DL_ROW_H);
}
static RectF DlRowRect(const RectF& p, int i) {
    RectF l = DlListRect(p);
    return RectF(l.X + SR(6), l.Y + SR(4) + (REAL)i * SR(DL_ROW_H),
                 l.Width - SR(12), SR(DL_ROW_H - 4));
}

static int DlRowAt(const RectF& p, int x, int y) {
    RectF l = DlListRect(p);
    if (!InRect(l, x, y)) return -1;
    const int i = (int)(((REAL)y - (l.Y + SR(4))) / SR(DL_ROW_H));
    if (i < 0 || i >= DL_ROWS) return -1;
    const int vi = g_app.dlScroll + i;
    return (vi >= 0 && vi < (int)g_dlView.size()) ? vi : -1;
}

// 首次进入下载页时才去拉版本清单；切页不清下载状态
static void OpenDownloadPage() {
    InitDlLock();
    if (!g_dlList.empty()) {
        if (g_app.dlPhase == 0) g_app.dlPhase = 1;
        return;
    }
    g_app.dlFilter = 0;
    g_app.dlSel    = 0;
    g_app.dlScroll = 0;
    g_app.dlHover  = -1;
    if (g_app.dlPhase == 2) return;               // 正在下载时别重拉
    g_app.dlPhase = 0;
    HANDLE h = CreateThread(nullptr, 0, DlManifestThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
}

// 标签下划线滑动：240ms 跑完，主循环每 4ms 推一帧（≈250fps 上限，实测多在 150fps 以上）。
// 4ms 这种等待只有配合 timeBeginPeriod(1) 才真的成立，否则会被系统拉回 15.6ms（≈64fps）。
static const int kTabAnimMs  = 240;
static const int kTabFrameMs = 4;

// 标签行（含下划线、分隔线）在设备像素下的矩形。
// 切页动画期间只有这一条带子在变，只重绘它：面积从 760x500 缩到 760x39，高帧率才稳得住。
static RECT TabBandRect(HWND hwnd) {
    RECT rc = {0, 0, 0, 0};
    if (hwnd) GetClientRect(hwnd, &rc);
    RECT b;
    b.left   = 0;
    b.right  = rc.right;
    b.top    = S(62);      // 页眉 logo 底边在 S(60)，从它下面起画才不会把 logo 擦掉
    b.bottom = S(101);     // 分隔线在 S(98)，留几像素余量吃掉抗锯齿的边
    return b;
}

// 微秒级时钟。GetTickCount64 的粒度是 15.6ms，直接拿它算动画进度会让下划线一跳一跳。
static LONGLONG NowUs() {
    static LONGLONG freq = 0;
    if (!freq) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        freq = f.QuadPart ? f.QuadPart : 1;
    }
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart * 1000000LL / freq;
}

// 推进一帧下划线滑动。由主消息循环在动画期间每轮调用 —— 不走 WM_TIMER，
// 因为 SetTimer 的实际粒度受系统时钟限制（实测 20~30ms 才来一帧，帧率高不上去）。
static void TickTabSlide() {
    if (!g_app.hwnd) return;
    // 节流：不到一帧的时间就别重画，消息密集时也保持稳定的帧间隔
    static LONGLONG lastUs = 0;
    const LONGLONG now = NowUs();
    if (lastUs && now - lastUs < (LONGLONG)kTabFrameMs * 1000) return;
    lastUs = now;

    const double el = (double)(now - g_app.tabT0) / 1000.0;   // 已经过去多少毫秒
    g_app.tabAnim = (el >= (double)kTabAnimMs) ? 1.0f : (float)(el / (double)kTabAnimMs);
    RECT band = TabBandRect(g_app.hwnd);
    InvalidateRect(g_app.hwnd, &band, FALSE);     // 只刷标签这一条窄带，面积小十几倍
}

static void SwitchPage(int page) {
    if (page < 0 || page > 2 || page == g_app.page) return;
    if (page == 1) OpenDownloadPage();
    // 下划线从「此刻画在哪儿」滑向新标签：滑到一半又切页时能接着滑，不会瞬移
    if (g_app.tabHasCur) {
        g_app.tabFromX = g_app.tabCurX;
        g_app.tabFromW = g_app.tabCurW;
        g_app.tabAnim  = 0.0f;
    }
    g_app.page    = page;
    g_app.ddOpen  = false;
    g_app.ddHover = -1;
    g_app.focus   = FOCUS_NONE;
    g_app.hover   = HIT_NONE;
    if (g_app.hwnd) {
        g_app.tabT0 = NowUs();              // 主循环看到 tabAnim < 1 就会自己按 kTabFrameMs 推帧
        InvalidateRect(g_app.hwnd, nullptr, FALSE);     // 整页换掉，这一帧必须全画
    }
}

static void StartDlDownload() {
    if (g_app.dlPhase != 1) return;
    if (g_app.dlSel < 0 || g_app.dlSel >= (int)g_dlView.size()) return;

    g_app.dlTarget = g_dlList[(size_t)g_dlView[(size_t)g_app.dlSel]].id;
    g_app.dlPhase  = 2;
    g_dlFilesDone.store(0);
    g_dlFilesTotal.store(0);
    g_dlBytesDone.store(0);
    g_dlBytesTotal.store(0);
    g_dlCancel.store(false);

    HANDLE h = CreateThread(nullptr, 0, DlThread, new std::wstring(g_app.dlTarget), 0, nullptr);
    if (h) CloseHandle(h);

    PushLog(L"开始下载 " + g_app.dlTarget + L" …");
    InvalidateRect(g_app.hwnd, nullptr, FALSE);
}

// ---- 下载页绘制（内嵌页面，不再是弹窗） ----
static void DrawDlPage(Graphics& gr) {
    RectF page = DlPageRect();
    FillRound(gr, page, SR(14), C(Theme::Panel));
    StrokeRound(gr, page, SR(14), C(Theme::PanelBorder), SR(1));

    DrawStr(gr, L"下载游戏版本", DlSubRect(page, 24, 12, 420, 28), *F.title, C(Theme::Text));
    DrawStr(gr, L"BMCLAPI 镜像 · 下载后即可直接启动", DlSubRect(page, 24, 42, 560, 18),
            *F.sub, C(Theme::Muted));

    const bool running = (g_app.dlPhase == 2);

    if (running) {
        // ---------------- 下载中：进度面板 ----------------
        DrawStr(gr, g_app.dlTarget, DlSubRect(page, 24, 112, 656, 26), *F.title,
                C(Theme::Text), StringAlignmentCenter);

        const int   fn  = g_dlFilesDone.load(), ft = g_dlFilesTotal.load();
        const long long bd = g_dlBytesDone.load(), bt = g_dlBytesTotal.load();
        int pct = bt > 0 ? (int)(bd * 100 / bt) : (ft > 0 ? fn * 100 / ft : 0);
        if (pct > 100) pct = 100;

        DrawStr(gr, std::to_wstring(pct) + L"%", DlSubRect(page, 24, 146, 656, 44),
                *F.title, C(Theme::Accent), StringAlignmentCenter);

        RectF bar = DlSubRect(page, 44, 202, 704 - 88, 10);
        FillRound(gr, bar, SR(5), C(Theme::Track));
        if (pct > 0) {
            REAL w = bar.Width * (REAL)pct / 100.0f;
            if (w < SR(6)) w = SR(6);
            FillRound(gr, RectF(bar.X, bar.Y, w, bar.Height), SR(5), C(Theme::Accent));
        }

        DrawStr(gr, L"已下载 " + std::to_wstring(fn) + L" / " + std::to_wstring(ft) + L" 个文件",
                DlSubRect(page, 24, 220, 656, 18), *F.sub, C(Theme::Muted),
                StringAlignmentCenter);
        DrawStr(gr, HumanSize(bd) + L" / " + HumanSize(bt),
                DlSubRect(page, 24, 238, 656, 18), *F.sub, C(Theme::Dim),
                StringAlignmentCenter);

        std::wstring cur;
        EnterCriticalSection(&g_dlLock);
        cur = FileNameOf(g_dlCur);
        LeaveCriticalSection(&g_dlLock);
        DrawStr(gr, cur, DlSubRect(page, 24, 264, 656, 18), *F.log, C(Theme::Dim),
                StringAlignmentCenter, true);
    } else {
        // ---------------- 选择 / 完成 / 失败 ----------------
        for (int i = 0; i < 3; ++i) {
            static const wchar_t* names[3] = { L"正式版", L"快照", L"全部" };
            RectF r = DlFilterRect(page, i);
            const bool act = (g_app.dlFilter == i);
            FillRound(gr, r, SR(9), C(act ? Theme::PillBg : Theme::Field));
            StrokeRound(gr, r, SR(9), C(act ? Theme::PillBorder : Theme::Border),
                        SR(act ? 1.3f : 1.0f));
            DrawStr(gr, names[i], r, *F.sub, C(act ? Theme::PillText : Theme::Muted),
                    StringAlignmentCenter);
        }

        RectF lr = DlListRect(page);
        FillRound(gr, lr, SR(12), C(Theme::Field));
        StrokeRound(gr, lr, SR(12), C(Theme::Border), SR(1));

        GraphicsPath clip;
        AddRoundRect(clip, lr, SR(12));
        gr.SetClip(&clip);
        if (g_dlView.empty()) {
            DrawStr(gr, g_app.dlPhase == 0 ? L"正在获取版本列表…" : L"没有可下载的版本",
                    lr, *F.sub, C(Theme::Dim), StringAlignmentCenter);
        }
        for (int i = 0; i < DL_ROWS; ++i) {
            const int vi = g_app.dlScroll + i;
            if (vi >= (int)g_dlView.size()) break;
            const DlVersion& v = g_dlList[(size_t)g_dlView[(size_t)vi]];
            RectF r = DlRowRect(page, i);
            const bool sel = (vi == g_app.dlSel);
            if (sel || g_app.dlHover == vi)
                FillRound(gr, r, SR(8), C(sel ? RGB(0xE7, 0xF5, 0xEC) : RGB(0xEA, 0xF0, 0xF9)));

            DrawStr(gr, v.id, RectF(r.X + SR(12), r.Y, r.Width - SR(190), r.Height), *F.sub,
                    C(sel ? Theme::Accent : Theme::Text), StringAlignmentNear, true);
            DrawStr(gr, v.date, RectF(r.X + r.Width - SR(196), r.Y, SR(104), r.Height), *F.log,
                    C(Theme::Dim), StringAlignmentFar);

            const bool rel = (v.type == L"release");
            RectF tag(r.X + r.Width - SR(84), r.Y + SR(7), SR(72), SR(18));
            FillRound(gr, tag, SR(9), C(rel ? RGB(0xE7, 0xF5, 0xEC) : Theme::PillBg));
            DrawStr(gr, DlTypeName(v.type), tag, *F.log,
                    C(rel ? Theme::Accent : Theme::PillText), StringAlignmentCenter);
        }
        gr.ResetClip();
    }

    // 分隔线
    {
        Pen line(C(Theme::PanelBorder), SR(1));
        gr.DrawLine(&line, page.X + SR(24), page.Y + SR(304),
                    page.X + page.Width - SR(24), page.Y + SR(304));
    }

    // 状态文字
    std::wstring status;
    Color statusCol = C(Theme::Muted);
    if (g_app.dlPhase == 0)      status = L"正在获取版本列表…";
    else if (g_app.dlPhase == 1) status = L"共 " + std::to_wstring(g_dlView.size()) + L" 个版本 · 滚轮翻页";
    else if (g_app.dlPhase == 2) status = L"正在下载 · 切到别的页面也会继续";
    else if (g_app.dlPhase == 3) status = L"下载完成，已加入版本列表";
    else {
        EnterCriticalSection(&g_dlLock);
        status = g_dlErr.empty() ? std::wstring(L"下载失败") : g_dlErr;
        LeaveCriticalSection(&g_dlLock);
        statusCol = C(Theme::Danger);
    }
    DrawStr(gr, status, DlSubRect(page, 24, 316, 420, 18), *F.log, statusCol);

    // 按钮
    if (running) {
        RectF cc = DlCancelRect(page);
        FillRound(gr, cc, SR(9), C(Theme::Field));
        StrokeRound(gr, cc, SR(9), C(Theme::Border), SR(1));
        DrawStr(gr, L"取消", cc, *F.sub, C(Theme::Muted), StringAlignmentCenter);
    } else {
        RectF go = DlGoRect(page);
        const bool canGo = (g_app.dlPhase == 1);
        FillRound(gr, go, SR(9), C(canGo ? Theme::Accent : Theme::Field));
        if (!canGo) StrokeRound(gr, go, SR(9), C(Theme::Border), SR(1));
        DrawStr(gr, canGo ? L"开始下载" : L"返回列表", go, *F.sub,
                C(canGo ? Theme::OnAccent : Theme::Muted), StringAlignmentCenter);
    }
}

static void HandleDlClick(int x, int y) {
    RectF page = DlPageRect();
    if (!InRect(page, x, y)) return;                                  // 页面外的事件不归这里管

    if (InRect(DlGoRect(page), x, y)) {
        if (g_app.dlPhase == 1) StartDlDownload();
        else if (g_app.dlPhase == 3 || g_app.dlPhase == 4) {
            g_app.dlPhase = 1;
            InvalidateRect(g_app.hwnd, nullptr, FALSE);
        }
        return;
    }
    if (g_app.dlPhase == 2 && InRect(DlCancelRect(page), x, y)) {
        g_dlCancel.store(true);
        EnterCriticalSection(&g_dlLock);
        g_dlErr = L"已取消下载";
        LeaveCriticalSection(&g_dlLock);
        g_app.dlPhase = 4;
        InvalidateRect(g_app.hwnd, nullptr, FALSE);
        return;
    }
    if (g_app.dlPhase != 1) return;

    for (int i = 0; i < 3; ++i) {
        if (InRect(DlFilterRect(page, i), x, y)) {
            if (g_app.dlFilter != i) {
                g_app.dlFilter = i;
                g_app.dlSel    = 0;
                g_app.dlScroll = 0;
                RebuildDlView();
            }
            InvalidateRect(g_app.hwnd, nullptr, FALSE);
            return;
        }
    }

    const int row = DlRowAt(page, x, y);
    if (row >= 0) {
        g_app.dlSel = row;                             // 单击只选中，开始按右侧按钮
        InvalidateRect(g_app.hwnd, nullptr, FALSE);
    }
}

static void DrawFieldBox(Graphics& gr, const RectF& r, bool focused) {
    FillRound(gr, r, SR(10), C(Theme::Field));
    StrokeRound(gr, r, SR(10), C(focused ? Theme::AccentSoft : Theme::Border), SR(focused ? 1.4f : 1.0f));
}

// 账号方式选项：昵称框下方并排两个小胶囊，默认选中「离线登录」
// 选中态用浅蓝底 + 蓝字，未选中是极浅的灰底
static void DrawAuthPill(Graphics& gr, const RectF& r, const std::wstring& text,
                         bool active, bool hover) {
    FillRound(gr, r, SR(11),
              active ? C(Theme::PillBg) : C(hover ? Theme::FieldHover : Theme::Field));
    StrokeRound(gr, r, SR(11), C(active ? Theme::PillBorder : Theme::Border),
                SR(active ? 1.3f : 1.0f));
    DrawStr(gr, text, r, *F.sub,
            C(active ? Theme::PillText : (hover ? Theme::Text : Theme::Muted)),
            StringAlignmentCenter);
}

static void DrawAuthRow(Graphics& gr, const Layout& L) {
    const bool busy   = (g_ms.state == MS_WAITING || g_ms.state == MS_WORKING);
    const bool msMode = (g_ms.state != MS_LOGGED_OUT);

    DrawAuthPill(gr, L.msOffBtn, L"离线登录", !msMode, g_app.hover == HIT_MS_OFF);
    DrawAuthPill(gr, L.msOnBtn,
                 g_ms.state == MS_WAITING ? L"授权中…"
                 : (g_ms.state == MS_WORKING ? L"登录中…" : L"微软登录"),
                 msMode, g_app.hover == HIT_MS_ON);

    if (g_ms.state == MS_LOGGED_IN)
        DrawStr(gr, L"解除绑定", L.msUnbind, *F.sub, C(Theme::PillText),
                StringAlignmentNear, true);
    else if (busy)
        DrawStr(gr, L"去浏览器输码", L.msUnbind, *F.sub, C(Theme::PillText),
                StringAlignmentNear, true);
}

// 验证码弹窗：告诉用户码已复制，并给出继续去浏览器验证的入口
// a = 动画进度（0 收起 / 1 完全展开）：遮罩淡入 + 卡片从下方 14px 滑入
static void DrawMsDialog(Graphics& gr, const Layout& L, int W, int H, float a) {
    const float e  = 1.0f - (1.0f - a) * (1.0f - a) * (1.0f - a);   // ease-out
    const BYTE  ca = (BYTE)(255.0f * e);

    SolidBrush scrim(Color((BYTE)(104.0f * e), 18, 26, 40));
    gr.FillRectangle(&scrim, 0, 0, W, H);

    const REAL dy = (1.0f - e) * SR(14);
    RectF card = L.dlgCard;
    card.Y += dy;

    FillRound(gr, card, SR(16), C(Theme::Panel, ca));
    StrokeRound(gr, card, SR(16), C(Theme::PanelBorder, ca), SR(1));

    const REAL x = card.X, w = card.Width;

    DrawStr(gr, L"微软账号登录", RectF(x, card.Y + S(18), w, S(22)),
            *F.title, C(Theme::Text, ca), StringAlignmentCenter);
    DrawStr(gr, L"验证码已复制到剪贴板", RectF(x, card.Y + S(46), w, S(18)),
            *F.sub, C(Theme::Muted, ca), StringAlignmentCenter);

    // 大号验证码
    DrawStr(gr, g_ms.userCode.empty() ? L"…" : g_ms.userCode,
            RectF(x, card.Y + S(70), w, S(34)), *F.title, C(Theme::Accent, ca),
            StringAlignmentCenter);

    // 浏览器唤起前带倒计时，唤起后换成「已打开」
    const std::wstring tip = g_app.dlgWait > 0
        ? (L"浏览器将在 " + std::to_wstring(g_app.dlgWait) + L" 秒后自动打开")
        : std::wstring(L"浏览器已打开 microsoft.com/link");
    DrawStr(gr, tip, RectF(x, card.Y + S(110), w, S(16)),
            *F.sub, C(Theme::Muted, ca), StringAlignmentCenter);
    DrawStr(gr, L"粘贴验证码并完成登录，窗口会自动刷新",
            RectF(x, card.Y + S(128), w, S(16)),
            *F.sub, C(Theme::Dim, ca), StringAlignmentCenter);

    const REAL bdy = dy;
    RectF openBtn = L.dlgOpen; openBtn.Y += bdy;
    RectF okBtn   = L.dlgOk;   okBtn.Y   += bdy;

    FillRound(gr, openBtn, SR(9), C(Theme::PillText, ca));
    DrawStr(gr, L"重新打开浏览器", openBtn, *F.sub, C(Theme::OnAccent, ca),
            StringAlignmentCenter);

    FillRound(gr, okBtn, SR(9), C(Theme::Field, ca));
    StrokeRound(gr, okBtn, SR(9), C(Theme::Border, ca), SR(1));
    DrawStr(gr, L"知道了", okBtn, *F.sub, C(Theme::Muted, ca), StringAlignmentCenter);
}

// 打开 / 收起弹窗：起一个 15ms 的动画定时器，跑完自动停
static void AnimMsDialog(bool open) {
    g_app.msDlg = open;
    if (open && g_app.msAnim <= 0.001f) g_app.msAnim = 0.0f;
    if (g_app.hwnd) SetTimer(g_app.hwnd, 2, 15, nullptr);
    if (g_app.hwnd) InvalidateRect(g_app.hwnd, nullptr, FALSE);
}

static const wchar_t* kAppVersion = L"0.3.1";   // 与 res/app.rc 的版本资源保持一致

// 标题下方的三个标签：开始 / 下载 / 设置
static const wchar_t* kTabName[3] = { L"开始", L"下载", L"设置" };

// 某个标签下划线的位置与宽度（居中在标签文字下方）
static void TabUnderline(Graphics& gr, const Layout& L, int i, REAL* x, REAL* w) {
    const RectF r  = L.tab[i];
    const REAL  tw = MeasureW(gr, kTabName[i], *F.sub);
    *x = r.X + (r.Width - tw) / 2;
    *w = tw;
}

static void DrawTabs(Graphics& gr, const Layout& L) {
    for (int i = 0; i < 3; ++i) {
        const RectF r   = L.tab[i];
        const bool  act = (g_app.page == i);
        const bool  hov = (g_app.hover == HIT_TAB0 + i);
        DrawStr(gr, kTabName[i], r, *F.sub,
                C(act ? Theme::Text : (hov ? Theme::Muted : Theme::Dim)), StringAlignmentCenter);
    }

    // 下划线只有一条，切页时从旧标签缓动滑到新标签（ease-out，16ms 一帧）
    REAL tx = 0, tw = 0;
    TabUnderline(gr, L, g_app.page, &tx, &tw);
    if (!g_app.tabHasCur) {                  // 首帧：直接落到当前标签，不播动画
        g_app.tabCurX   = tx;
        g_app.tabCurW   = tw;
        g_app.tabHasCur = true;
    }
    REAL x = tx, w = tw;
    if (g_app.tabAnim < 1.0f) {
        const float t = g_app.tabAnim;
        const float u = 1.0f - t;
        const float e = 1.0f - u * u * u;    // easeOutCubic：起步快、收尾轻
        x = g_app.tabFromX + (tx - g_app.tabFromX) * e;
        w = g_app.tabFromW + (tw - g_app.tabFromW) * e;
    }
    g_app.tabCurX = x;
    g_app.tabCurW = w;

    const RectF tr = L.tab[g_app.page];
    FillRound(gr, RectF(x, tr.Y + tr.Height - SR(5), w, SR(2.5f)), SR(1.4f), C(Theme::Accent));

    Pen line(C(Theme::PanelBorder), SR(1));
    gr.DrawLine(&line, L.panel1.X, SR(98), L.panel1.X + L.panel1.Width, SR(98));
}

// 小按钮（设置页的「浏览…／选择…」）
static void DrawSmallBtn(Graphics& gr, const RectF& r, const std::wstring& text, bool hover) {
    FillRound(gr, r, SR(9), C(hover ? Theme::FieldHover : Theme::Track));
    StrokeRound(gr, r, SR(9), C(Theme::Border), SR(1));
    DrawStr(gr, text, r, *F.sub, C(hover ? Theme::Text : Theme::Muted),
            StringAlignmentCenter, false, true);
}

// ---------------- 开始页 ----------------
static void DrawStartPage(Graphics& gr, const Layout& L) {
    FillRound(gr, L.panel1, SR(14), C(Theme::Panel));
    StrokeRound(gr, L.panel1, SR(14), C(Theme::PanelBorder), SR(1));

    DrawStr(gr, L"玩家昵称", RectF(S(54), S(122), S(240), S(18)), *F.label, C(Theme::Muted));
    DrawStr(gr, L"游戏版本", RectF(S(378), S(122), S(200), S(18)), *F.label, C(Theme::Muted));

    // 昵称输入框
    DrawFieldBox(gr, L.userBox, g_app.focus == FOCUS_USER);
    {
        RectF tr(L.userBox.X + SR(14), L.userBox.Y, L.userBox.Width - SR(28), L.userBox.Height);
        DrawStr(gr, g_app.username, tr, *F.input, C(Theme::Text), StringAlignmentNear, false, true);
        if (g_app.focus == FOCUS_USER && g_app.caretOn && g_ms.state != MS_LOGGED_IN) {
            REAL w  = MeasureW(gr, g_app.username, *F.input);
            REAL cx = tr.X + w + SR(1);
            REAL cy = L.userBox.Y + L.userBox.Height / 2;
            Pen caretp(C(Theme::Accent), SR(1.6f));
            gr.DrawLine(&caretp, cx, cy - SR(9), cx, cy + SR(9));
        }
    }

    // 账号方式：离线登录 / 微软登录（默认离线）
    DrawAuthRow(gr, L);

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

    // ---------------- 启动按钮 ----------------
    {
        RectF b = L.launchBtn;
        if (g_app.launching) {
            FillRound(gr, b, SR(13), C(RGB(0xE8, 0xF2, 0xEC)));
            GraphicsPath pth;
            AddRoundRect(pth, b, SR(13));
            gr.SetClip(&pth);
            REAL w = b.Width * (g_app.progress / 100.0f);
            if (w > SR(4)) FillRound(gr, RectF(b.X, b.Y, w, b.Height), SR(13), C(RGB(0x3F, 0xBF, 0x7C)));
            gr.ResetClip();
            StrokeRound(gr, b, SR(13), Color(90, 36, 168, 98), SR(1));
        } else {
            DWORD base  = g_app.launched ? RGB(0x2E, 0x9E, 0x62)
                        : (g_app.hover == HIT_LAUNCH ? RGB(0x3F, 0xCC, 0x80) : RGB(0x2F, 0xB8, 0x6C));
            DWORD base2 = g_app.launched ? RGB(0x1D, 0x84, 0x4E) : RGB(0x24, 0x9E, 0x5A);
            LinearGradientBrush lg(b, C(base), C(base2), LinearGradientModeVertical);
            GraphicsPath pth;
            AddRoundRect(pth, b, SR(13));
            gr.FillPath(&lg, &pth);
        }

        std::wstring text = g_app.launching
            ? (L"正在启动 " + std::to_wstring((int)g_app.progress) + L"%")
            : (g_app.launched ? L"游戏已启动" : L"启动游戏");
        Color tc = g_app.launching ? C(Theme::Text) : C(Theme::OnAccent);

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
    {
        RectF lp(L.logArea.X, L.logArea.Y - SR(6), L.logArea.Width, L.logArea.Height + SR(6));
        FillRound(gr, lp, SR(12), C(Theme::Panel));
        StrokeRound(gr, lp, SR(12), C(Theme::PanelBorder), SR(1));
    }
    for (size_t i = 0; i < g_app.logs.size(); ++i) {
        bool last = (i + 1 == g_app.logs.size());
        DrawStr(gr, L"› " + g_app.logs[i],
                RectF(L.logArea.X + SR(16), L.logArea.Y + SR(2) + (REAL)i * SR(18),
                      L.logArea.Width - SR(32), SR(18)),
                *F.log, C(last ? Theme::Muted : Theme::Dim), StringAlignmentNear, true);
    }
}

// ---------------- 设置页 ----------------
static void DrawSettingsPage(Graphics& gr, const Layout& L) {
    // 游戏目录
    FillRound(gr, L.dirPanel, SR(14), C(Theme::Panel));
    StrokeRound(gr, L.dirPanel, SR(14), C(Theme::PanelBorder), SR(1));
    DrawStr(gr, L"游戏目录", RectF(S(52), S(116), S(200), S(16)), *F.label, C(Theme::Muted));
    {
        const bool ok = PathExists(g_app.gameDir);
        DrawStr(gr, ok ? g_app.gameDir
                       : std::wstring(L"未找到 .minecraft，点右侧「浏览…」选择"),
                RectF(S(52), S(136), S(560), S(20)), *F.sub,
                C(ok ? Theme::Muted : Theme::Dim), StringAlignmentNear, true);
    }
    DrawSmallBtn(gr, L.browseBtn, L"浏览…", g_app.hover == HIT_BROWSE);

    // Java 路径
    FillRound(gr, L.javaPanel, SR(14), C(Theme::Panel));
    StrokeRound(gr, L.javaPanel, SR(14), C(Theme::PanelBorder), SR(1));
    DrawStr(gr, L"Java 路径", RectF(S(52), S(192), S(200), S(16)), *F.label, C(Theme::Muted));
    {
        const bool ok = PathExists(g_app.javaPath);
        DrawStr(gr, ok ? g_app.javaPath
                       : std::wstring(L"未检测到 Java，点右侧「选择…」指定"),
                RectF(S(52), S(212), S(560), S(20)), *F.sub,
                C(ok ? Theme::Muted : Theme::Dim), StringAlignmentNear, true);
    }
    DrawSmallBtn(gr, L.javaBtn, L"选择…", g_app.hover == HIT_JAVA);

    // 内存分配
    FillRound(gr, L.memPanel, SR(14), C(Theme::Panel));
    StrokeRound(gr, L.memPanel, SR(14), C(Theme::PanelBorder), SR(1));
    DrawStr(gr, L"内存分配", RectF(S(54), S(268), S(240), S(18)), *F.label, C(Theme::Muted));
    DrawStr(gr, std::to_wstring(g_app.memGb) + L" GB", RectF(S(52), S(266), S(656), S(20)),
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

    // 附加 JVM 参数
    FillRound(gr, L.argPanel, SR(14), C(Theme::Panel));
    StrokeRound(gr, L.argPanel, SR(14), C(Theme::PanelBorder), SR(1));
    DrawStr(gr, L"附加 JVM 参数", RectF(S(52), S(346), S(200), S(16)), *F.label, C(Theme::Muted));
    DrawStr(gr, L"原样追加到 java 命令行，留空则不追加",
            RectF(S(160), S(346), S(500), S(16)), *F.log, C(Theme::Dim));

    DrawFieldBox(gr, L.argBox, g_app.focus == FOCUS_ARGS);
    {
        RectF tr(L.argBox.X + SR(12), L.argBox.Y, L.argBox.Width - SR(24), L.argBox.Height);
        if (g_app.jvmArgs.empty() && g_app.focus != FOCUS_ARGS) {
            DrawStr(gr, L"例如 -XX:+UseG1GC -Dfile.encoding=UTF-8", tr, *F.sub,
                    C(Theme::Dim), StringAlignmentNear, true);
        } else {
            DrawStr(gr, g_app.jvmArgs, tr, *F.sub, C(Theme::Text), StringAlignmentNear, true);
            if (g_app.focus == FOCUS_ARGS && g_app.caretOn) {
                REAL w  = MeasureW(gr, g_app.jvmArgs, *F.sub);
                REAL cx = tr.X + w + SR(1);
                REAL cy = L.argBox.Y + L.argBox.Height / 2;
                Pen caretp(C(Theme::Accent), SR(1.6f));
                gr.DrawLine(&caretp, cx, cy - SR(8), cx, cy + SR(8));
            }
        }
    }

    // 关于
    DrawStr(gr, L"Dream Minecraft Launcher · v" + std::wstring(kAppVersion) +
                L" · MIT License © 2026 Dadi",
            RectF(S(52), S(414), S(656), S(18)), *F.sub, C(Theme::Muted));
    // 显示 %APPDATA% 字面量而不是展开后的真实路径：既不把当前 Windows 用户名画进截图，
    // 对用户来说也比一长串 C:\Users\... 更好认
    DrawStr(gr, L"配置：%APPDATA%\\DreamMinecraftLauncher\\config.ini",
            RectF(S(52), S(434), S(656), S(18)), *F.log, C(Theme::Dim),
            StringAlignmentNear, true);
}

// ---- 选 java.exe（设置页） ----
static void ChooseJava() {
    wchar_t buf[MAX_PATH] = {0};
    if (!g_app.javaPath.empty())
        wcsncpy(buf, g_app.javaPath.c_str(), MAX_PATH - 1);
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = g_app.hwnd;
    ofn.lpstrFilter = L"Java 可执行文件\0java.exe;javaw.exe\0所有文件\0*.*\0";
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrTitle  = L"选择 java.exe";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return;
    g_app.javaPath = buf;
    PushLog(L"Java: " + g_app.javaPath);
    SaveConfig();
    InvalidateRect(g_app.hwnd, nullptr, FALSE);
}

static void Paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT rcWin;
    GetClientRect(hwnd, &rcWin);
    int W = rcWin.right, H = rcWin.bottom;

    // 切页动画只脏了标签行 → 只重画这一条窄带（整窗 760x500 重绘撑不起上百帧）
    const RECT band     = TabBandRect(hwnd);
    const bool bandOnly = (ps.rcPaint.top    >= band.top  && ps.rcPaint.bottom <= band.bottom &&
                           ps.rcPaint.left   >= band.left && ps.rcPaint.right  <= band.right);
    const RECT rcDraw   = bandOnly ? band : rcWin;
    const int  DW = rcDraw.right - rcDraw.left;
    const int  DH = rcDraw.bottom - rcDraw.top;

    HDC     memDC = CreateCompatibleDC(hdc);
    HBITMAP bmp   = CreateCompatibleBitmap(hdc, DW, DH);
    HBITMAP old   = (HBITMAP)SelectObject(memDC, bmp);

    {
        Graphics gr(memDC);
        gr.SetSmoothingMode(SmoothingModeAntiAlias);
        gr.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        gr.SetPixelOffsetMode(PixelOffsetModeHalf);
        gr.Clear(C(Theme::Bg));

        if (bandOnly) {
            // 窄带重绘：仅标签与分隔线。整窗坐标系平移进带内，绘制代码不必改坐标
            gr.TranslateTransform((REAL)-rcDraw.left, (REAL)-rcDraw.top);
            Layout Lb = ComputeLayout();
            DrawTabs(gr, Lb);
            BitBlt(hdc, rcDraw.left, rcDraw.top, DW, DH, memDC, 0, 0, SRCCOPY);
            SelectObject(memDC, old);
            DeleteObject(bmp);
            DeleteDC(memDC);
            EndPaint(hwnd, &ps);
            return;
        }

        {
            // 无边框窗口：客户区外圈描一条极淡的分界线，避免白底与浅色桌面糊在一起
            Pen edge(C(Theme::Border), SR(1));
            gr.DrawRectangle(&edge, 0, 0, (INT)W - 1, (INT)H - 1);
        }

        Layout L = ComputeLayout();

        // ---------------- 头部 ----------------
        DrawLogo(gr, L.logo);
        // 单行标题：与 40px 高的标志垂直居中对齐（标志 y=20..60，标题高 28 -> y=26）
        DrawStr(gr, L"Dream Minecraft Launcher", RectF(S(80), S(26), S(460), S(28)),
                *F.title, C(Theme::Text));
        DrawWindowButtons(gr, L);

        // ---------------- 标签页 ----------------
        DrawTabs(gr, L);

        if (g_app.page == 0)      DrawStartPage(gr, L);
        else if (g_app.page == 1) DrawDlPage(gr);
        else                      DrawSettingsPage(gr, L);

        // ---------------- 版本下拉浮层（仅开始页，覆盖在上层） ----------------
        if (g_app.page == 0 && g_app.ddOpen && !g_app.versions.empty()) {
            int n    = (int)g_app.versions.size();
            int show = std::min(n, DD_MAX_SHOW);
            RectF panel(L.verBox.X, L.verBox.Y + L.verBox.Height + SR(6), L.verBox.Width,
                        (REAL)(show * S(DD_ITEM_H) + S(8)));
            FillRound(gr, panel, SR(11), C(RGB(0xFF, 0xFF, 0xFF)));
            StrokeRound(gr, panel, SR(11), C(RGB(0xDC, 0xE2, 0xEC)), SR(1));

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
                    FillRound(gr, ir, SR(8), C(sel ? RGB(0xE7, 0xF5, 0xEC) : Theme::FieldHover));
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

        // ---------------- 微软登录验证码弹窗（最上层） ----------------
        if (g_app.msAnim > 0.001f) DrawMsDialog(gr, L, W, H, g_app.msAnim);
    }

    BitBlt(hdc, rcDraw.left, rcDraw.top, DW, DH, memDC, 0, 0, SRCCOPY);
    SelectObject(memDC, old);
    DeleteObject(bmp);
    DeleteDC(memDC);
    EndPaint(hwnd, &ps);
}

// ============================================================================
//  命中测试 & 交互
// ============================================================================
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

// 切到离线：只是这次不用账号，account.dat 保留，随时能切回来
static void SelectOffline() {
    if (g_ms.state == MS_LOGGED_OUT) return;
    g_ms.state     = MS_LOGGED_OUT;
    g_app.username = g_offlineName;
    g_app.focus    = FOCUS_NONE;
    if (g_app.hwnd) KillTimer(g_app.hwnd, 3);     // 取消还在跑的自动开浏览器
    g_app.dlgWait  = 0;
    AnimMsDialog(false);
    PushLog(L"已切换到离线登录 · " + g_offlineName);
    SaveConfig();
    InvalidateRect(g_app.hwnd, nullptr, FALSE);
}

// 切到微软账号：以前登录过就静默续期，否则走设备码
static void SelectMs() {
    if (g_ms.state == MS_LOGGED_IN || g_ms.state == MS_WAITING || g_ms.state == MS_WORKING)
        return;
    if (!g_ms.refreshToken.empty()) {
        InitMsLock();
        g_ms.state = MS_WORKING;
        PushLog(L"正在恢复微软登录 …");
        HANDLE h = CreateThread(nullptr, 0, MsRefreshThread, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    } else {
        StartMsLogin();
    }
    InvalidateRect(g_app.hwnd, nullptr, FALSE);
}

static int HitTest(int x, int y) {
    Layout L = ComputeLayout();
    if (InRect(L.closeBtn, x, y))  return HIT_CLOSE;
    if (InRect(L.minBtn, x, y))    return HIT_MIN;
    for (int i = 0; i < 3; ++i)
        if (InRect(L.tab[i], x, y)) return HIT_TAB0 + i;

    if (g_app.page == 0) {
        if (InRect(L.msOffBtn, x, y))  return HIT_MS_OFF;
        if (InRect(L.msOnBtn, x, y))   return HIT_MS_ON;
        if (g_ms.state == MS_LOGGED_IN && InRect(L.msUnbind, x, y)) return HIT_MS_UNBIND;
        if (InRect(L.userBox, x, y))   return HIT_USER;
        if (InRect(L.verBox, x, y))    return HIT_VER;
        if (InRect(L.launchBtn, x, y)) return HIT_LAUNCH;
    } else if (g_app.page == 2) {
        if (InRect(L.browseBtn, x, y)) return HIT_BROWSE;
        if (InRect(L.javaBtn, x, y))   return HIT_JAVA;
        if (InRect(L.argBox, x, y))    return HIT_ARGS;
        RectF track = L.sliderTrack;
        track.Y -= SR(10);
        track.Height += SR(20);
        if (InRect(track, x, y))       return HIT_SLIDER;
    }
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
        timeBeginPeriod(1);          // 定时器精度提到 1ms，8ms 一帧的滑动动画才不被系统拉成 15.6ms
        SetTimer(hwnd, 1, 100, nullptr);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        Paint(hwnd);
        return 0;

    case WM_APP_LOG: {
        std::wstring* line = (std::wstring*)lParam;
        if (line) {
            PushLog(*line);
            delete line;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_APP_MS: {
        if (wParam == 1) {
            // 登录码就绪：复制到剪贴板 + 打开授权页 + 写日志
            EnterCriticalSection(&g_msLock);
            g_ms.userCode  = g_msShared.userCode;
            g_ms.verifyUri = g_msShared.uri;
            LeaveCriticalSection(&g_msLock);
            g_ms.state = MS_WAITING;

            const std::wstring code = g_ms.userCode;
            if (OpenClipboard(hwnd)) {
                EmptyClipboard();
                HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, (code.size() + 1) * sizeof(wchar_t));
                if (hg) {
                    void* p = GlobalLock(hg);
                    if (p) {
                        memcpy(p, code.c_str(), (code.size() + 1) * sizeof(wchar_t));
                        GlobalUnlock(hg);
                        SetClipboardData(CF_UNICODETEXT, hg);
                    } else {
                        GlobalFree(hg);
                    }
                }
                CloseClipboard();
            }
            PushLog(L"验证码 " + code + L"（已复制到剪贴板）");
            // 先弹窗把码亮出来，停留 5 秒（带倒计时）再唤起浏览器，让用户有时间看清
            g_app.dlgWait = 5;
            AnimMsDialog(true);
            SetTimer(hwnd, 3, 1000, nullptr);
        } else {
            KillTimer(hwnd, 3);
            g_app.dlgWait = 0;
            AnimMsDialog(false);
            EnterCriticalSection(&g_msLock);
            const MsShared s = g_msShared;
            LeaveCriticalSection(&g_msLock);

            if (s.ok) {
                g_ms.state        = MS_LOGGED_IN;
                g_ms.name         = s.name;
                g_ms.uuid         = s.uuid;
                g_ms.xuid         = s.xuid;
                g_ms.accessToken  = s.mcToken;
                if (!s.refresh.empty()) g_ms.refreshToken = s.refresh;
                g_ms.err.clear();
                // 昵称框只显示账号的正版游戏 ID（正版账号不允许再手改）
                g_app.username    = s.name;
                g_app.focus       = FOCUS_NONE;
                PushLog((s.silent ? L"已恢复正版 · " : L"已登录正版 · ") + s.name);
            } else {
                g_ms.state     = MS_LOGGED_OUT;
                g_ms.err       = s.err;
                g_app.username = g_offlineName;
                if (!s.silent)                          PushLog(L"登录失败：" + s.err);
                else if (s.err != L"没有已保存的账号")   PushLog(L"自动登录失败：" + s.err);
            }
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_APP_DL: {
        if (wParam == 1) {                       // 版本列表就绪
            if (!g_dlList.empty()) {
                g_app.dlPhase  = 1;
                g_app.dlSel    = 0;
                g_app.dlScroll = 0;
                RebuildDlView();
            } else {
                g_app.dlPhase = 4;
            }
        } else if (wParam == 2) {                // 下载完成
            g_app.dlPhase = 3;
            if (g_app.gameDir.empty()) g_app.gameDir = AppDataMinecraftDir();
            auto vs = ScanVersions(g_app.gameDir);
            if (!vs.empty()) {
                g_app.versions = vs;
                g_app.realScan = true;
                g_app.versionIdx = 0;
                for (size_t i = 0; i < vs.size(); ++i)
                    if (vs[i] == g_app.dlTarget) { g_app.versionIdx = (int)i; break; }
            }
            PushLog(L"下载完成 · " + g_app.dlTarget);
            SaveConfig();
        } else {                                 // 失败 / 取消
            g_app.dlPhase = 4;
            EnterCriticalSection(&g_dlLock);
            const std::wstring why = g_dlErr;
            LeaveCriticalSection(&g_dlLock);
            PushLog(L"下载未完成：" + (why.empty() ? std::wstring(L"未知原因") : why));
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_TIMER: {
        if (wParam == 2) {                       // 弹窗动画（15ms 一帧，160ms 跑完）
            const float dt = 0.015f / 0.16f;
            const float t1 = g_app.msDlg ? 1.0f : 0.0f;
            if (g_app.msAnim < t1)      g_app.msAnim = std::min(t1, g_app.msAnim + dt);
            else if (g_app.msAnim > t1) g_app.msAnim = std::max(t1, g_app.msAnim - dt);

            const bool d1 = std::fabs(g_app.msAnim - t1) < 0.001f;
            if (d1) g_app.msAnim = t1;
            if (d1) KillTimer(hwnd, 2);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (wParam == 3) {                       // 弹窗倒计时：到点后唤起浏览器
            InvalidateRect(hwnd, nullptr, FALSE);
            if (g_app.dlgWait > 0 && --g_app.dlgWait > 0) return 0;
            KillTimer(hwnd, 3);
            if (!g_ms.verifyUri.empty()) {
                ShellExecuteW(nullptr, L"open", g_ms.verifyUri.c_str(), nullptr, nullptr,
                              SW_SHOWNORMAL);
                PushLog(L"已打开浏览器 " + g_ms.verifyUri);
            }
            return 0;
        }
        if (wParam == 4) {                       // 启动进度条：16ms 一帧平滑逼近目标值
            if (!g_app.launching) { KillTimer(hwnd, 4); return 0; }
            const float d = g_app.progressTarget - g_app.progress;
            if (d > 0.005f) {
                float v = d * 0.12f;             // 指数逼近，起步快收尾缓
                if (v < 0.30f) v = 0.30f;        // 末段保底速度，别拖成爬
                g_app.progress = std::min(g_app.progressTarget, g_app.progress + v);
            }
            // 进度跑满就地推进下一步：不必等下一次 100ms 业务定时器，避免每段末尾卡一下
            if (LaunchTick()) {
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            Layout L = ComputeLayout();
            RECT rc;
            rc.left   = (LONG)(L.launchBtn.X - SR(2));
            rc.top    = (LONG)(L.launchBtn.Y - SR(2));
            rc.right  = (LONG)(L.launchBtn.X + L.launchBtn.Width + SR(2));
            rc.bottom = (LONG)(L.launchBtn.Y + L.launchBtn.Height + SR(2));
            InvalidateRect(hwnd, &rc, FALSE);
            return 0;
        }
        if (wParam != 1) break;
        g_app.caretTick++;
        if (g_app.caretTick % 5 == 0) g_app.caretOn = !g_app.caretOn;

        // 游戏进程退出检测
        if (g_hChild && WaitForSingleObject(g_hChild, 0) == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(g_hChild, &code);
            CloseHandle(g_hChild);
            g_hChild = nullptr;
            g_app.launched  = false;
            g_app.resetTick = 0;
            PushLog(L"游戏进程已退出（退出码 " + std::to_wstring((unsigned)code) + L"）");
        }

        if (g_app.launching) {
            LaunchTick();
        } else if (g_app.launched && g_app.resetTick > 0) {
            if (--g_app.resetTick == 0) g_app.launched = false;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);

        if (g_app.page == 1) {                   // 下载页：列表行 hover + 手型光标
            RectF page = DlPageRect();
            const int r = (g_app.dlPhase == 1) ? DlRowAt(page, x, y) : -1;
            if (r != g_app.dlHover) {
                g_app.dlHover = r;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            bool hand = (r >= 0) || InRect(DlGoRect(page), x, y) ||
                        (g_app.dlPhase == 2 && InRect(DlCancelRect(page), x, y));
            if (!hand && g_app.dlPhase == 1) {
                for (int i = 0; i < 3 && !hand; ++i)
                    hand = InRect(DlFilterRect(page, i), x, y);
            }
            int hh = HitTest(x, y);              // 标签页按钮仍要能点
            if (hh >= HIT_TAB0 && hh <= HIT_TAB2) hand = true;
            SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
            g_app.hover = hh;
            return 0;
        }

        if (g_app.draggingSlider) { SetSliderFromX(x); return 0; }

        int h = HitTest(x, y);
        if (h != g_app.hover) { g_app.hover = h; InvalidateRect(hwnd, nullptr, FALSE); }

        int di = g_app.ddOpen ? DropdownItemAt(x, y) : -1;
        if (di != g_app.ddHover) { g_app.ddHover = di; InvalidateRect(hwnd, nullptr, FALSE); }

        LPCWSTR cur = IDC_ARROW;
        if (h == HIT_CLOSE || h == HIT_MIN || h == HIT_BROWSE || h == HIT_LAUNCH ||
            h == HIT_VER || h == HIT_MS_OFF || h == HIT_MS_ON || h == HIT_MS_UNBIND ||
            h == HIT_JAVA || (h >= HIT_TAB0 && h <= HIT_TAB2))
            cur = IDC_HAND;
        else if (h == HIT_USER)
            cur = (g_ms.state == MS_LOGGED_IN) ? IDC_ARROW : IDC_IBEAM;
        else if (h == HIT_ARGS)
            cur = IDC_IBEAM;
        else if (h == HIT_SLIDER)
            cur = IDC_SIZEWE;
        SetCursor(LoadCursorW(nullptr, cur));
        return 0;
    }

    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
        SetFocus(hwnd);

        if (g_app.msDlg) {                         // 弹窗吃掉所有点击
            Layout L = ComputeLayout();
            if (InRect(L.dlgOpen, x, y)) {
                if (!g_ms.verifyUri.empty())
                    ShellExecuteW(nullptr, L"open", g_ms.verifyUri.c_str(), nullptr, nullptr,
                                  SW_SHOWNORMAL);
                // 手动开过就别再自动开了
                KillTimer(hwnd, 3);
                g_app.dlgWait = 0;
                PushLog(L"已打开浏览器 " + g_ms.verifyUri);
                InvalidateRect(hwnd, nullptr, FALSE);
            } else {                               // 点其他任何地方 = 知道了
                AnimMsDialog(false);
            }
            return 0;
        }

        // 标签页在任何页面都能点
        {
            int t = HitTest(x, y);
            if (t >= HIT_TAB0 && t <= HIT_TAB2) { SwitchPage(t - HIT_TAB0); return 0; }
        }

        if (g_app.page == 1) { HandleDlClick(x, y); return 0; }

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
        case HIT_MS_OFF: SelectOffline(); return 0;
        case HIT_MS_ON:  SelectMs(); return 0;
        case HIT_MS_UNBIND: LogoutMs(); return 0;
        case HIT_JAVA:   ChooseJava(); return 0;
        case HIT_ARGS:
            g_app.focus = FOCUS_ARGS; g_app.caretOn = true; g_app.caretTick = 0;
            break;
        case HIT_USER:
            if (g_ms.state == MS_LOGGED_IN) return 0;      // 已登录时昵称由账号决定
            g_app.focus = FOCUS_USER; g_app.caretOn = true; g_app.caretTick = 0;
            break;
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
        if (g_app.page == 1) {                   // 下载列表翻页
            if (g_app.dlPhase == 1) {
                const int maxScroll = std::max(0, (int)g_dlView.size() - DL_ROWS);
                const int step = (GET_WHEEL_DELTA_WPARAM(wParam) > 0) ? -1 : 1;
                g_app.dlScroll = std::max(0, std::min(maxScroll, g_app.dlScroll + step));
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        if (!g_app.ddOpen) break;
        int delta     = GET_WHEEL_DELTA_WPARAM(wParam);
        int maxScroll = std::max(0, (int)g_app.versions.size() - DD_MAX_SHOW);
        if (delta > 0) g_app.ddScroll = std::max(0, g_app.ddScroll - 1);
        else           g_app.ddScroll = std::min(maxScroll, g_app.ddScroll + 1);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_CHAR: {
        if (g_app.focus == FOCUS_USER && g_ms.state != MS_LOGGED_IN) {
            wchar_t ch = (wchar_t)wParam;
            if (ch == L'\b') {
                if (!g_app.username.empty()) g_app.username.pop_back();
            } else if (ch >= 32 && ch != 127) {
                if (g_app.username.size() < 16) g_app.username.push_back(ch);
            }
            g_offlineName = g_app.username;      // 离线昵称单独记住，退出登录可还原
            g_app.caretOn = true;
            g_app.caretTick = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (g_app.focus == FOCUS_ARGS) {
            wchar_t ch = (wchar_t)wParam;
            if (ch == L'\b') {
                if (!g_app.jvmArgs.empty()) g_app.jvmArgs.pop_back();
            } else if (ch >= 32 && ch != 127) {
                if (g_app.jvmArgs.size() < 160) g_app.jvmArgs.push_back(ch);
            }
            g_app.caretOn = true;
            g_app.caretTick = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_KEYDOWN:
        if (g_app.page == 1) {
            if (wParam == VK_ESCAPE) { SwitchPage(0); return 0; }
            if (wParam == VK_RETURN) {
                if (g_app.dlPhase == 1) StartDlDownload();
                else if (g_app.dlPhase == 3 || g_app.dlPhase == 4) {
                    g_app.dlPhase = 1;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            if (g_app.dlPhase == 1 && (wParam == VK_UP || wParam == VK_DOWN)) {
                const int n = (int)g_dlView.size();
                if (n > 0) {
                    g_app.dlSel += (wParam == VK_UP ? -1 : 1);
                    g_app.dlSel = std::max(0, std::min(n - 1, g_app.dlSel));
                    if (g_app.dlSel < g_app.dlScroll) g_app.dlScroll = g_app.dlSel;
                    if (g_app.dlSel >= g_app.dlScroll + DL_ROWS)
                        g_app.dlScroll = g_app.dlSel - DL_ROWS + 1;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            return 0;
        }
        if (g_app.msDlg) {
            if (wParam == VK_ESCAPE || wParam == VK_RETURN) {
                AnimMsDialog(false);
                return 0;
            }
        }
        // 正在编辑文本框时，Esc / Enter 只用来收起焦点，不触发启动
        if (g_app.focus != FOCUS_NONE && (wParam == VK_ESCAPE || wParam == VK_RETURN)) {
            g_app.focus = FOCUS_NONE;
            SaveConfig();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
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
        KillTimer(hwnd, 2);
        timeEndPeriod(1);
        g_dlCancel.store(true);          // 让下载线程尽快收尾
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
    g_offlineName = g_app.username.empty() ? L"Steve" : g_app.username;
    if (g_app.gameDir.empty() || !PathExists(g_app.gameDir))
        g_app.gameDir = DefaultMinecraftDir();

    g_app.versions = ScanVersions(g_app.gameDir);
    g_app.realScan = !g_app.versions.empty();
    if (!g_app.realScan) g_app.versions = FallbackVersions();

    if (!savedVer.empty()) {
        for (size_t i = 0; i < g_app.versions.size(); ++i)
            if (g_app.versions[i] == savedVer) { g_app.versionIdx = (int)i; break; }
    }
    // 设置页里手动指定过的 java 优先，无效或没配过才自动探测
    if (g_app.javaPath.empty() || !PathExists(g_app.javaPath)) g_app.javaPath = DetectJava();
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

    // 有已保存的账号就静默续期（不阻塞首屏）
    MsAutoRefresh();

    MSG msg;
    // 动画期间不靠 WM_TIMER：SetTimer 受系统时钟粒度限制，实测最快也只有 20~30ms 一帧。
    // 改成消息循环自驱动 —— 滑动进行中最多等 kTabFrameMs 就回来推一帧，
    // 帧率于是由重绘耗时决定（标签窄带约 0.8ms），能跑到 150fps 以上。
    for (;;) {
        const bool  anim   = (g_app.tabAnim < 1.0f);
        const DWORD waitMs = anim ? kTabFrameMs : INFINITE;
        MsgWaitForMultipleObjectsEx(0, nullptr, waitMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        bool quit = false;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { quit = true; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit) break;
        // 每轮都推进（内部按 kTabFrameMs 节流，进度按真实时间算）：
        // 只在等待超时时推的话，消息一多就迟迟不落帧，动画会断续
        if (anim) TickTabSlide();
    }

    FreeFonts();
    FreeLogoResource();
    if (hIconBig && hIconBig != hIconSm) DestroyIcon(hIconBig);
    if (hIconSm) DestroyIcon(hIconSm);
    GdiplusShutdown(gpToken);
    OleUninitialize();
    return 0;
}
