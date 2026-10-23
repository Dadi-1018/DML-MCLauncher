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
#include <winhttp.h>
#include <wincrypt.h>
#include <gdiplus.h>

#include "resource.h"   // IDI_APPICON / IDR_LOGO

#include <stdlib.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>
#include <functional>
#include <utility>
#include <cmath>

using namespace Gdiplus;

// 子进程输出回灌：LPARAM 是 new 出来的 std::wstring*，处理完需 delete
#define WM_APP_LOG (WM_APP + 1)
// 微软登录线程 → UI：wParam = 1 设备码就绪 / 2 流程结束（成功或失败，看 g_msShared.ok）
#define WM_APP_MS  (WM_APP + 2)

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
    RectF msOffBtn;      // 昵称框下方：离线登录 / 微软登录 两个选项
    RectF msOnBtn;
    RectF msUnbind;      // 仅已登录时显示
    RectF dlgCard;       // 验证码弹窗
    RectF dlgOpen;
    RectF dlgOk;
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
    L.msOffBtn    = RectF(S(52),  S(172), S(78),  S(22));
    L.msOnBtn     = RectF(S(136), S(172), S(78),  S(22));
    L.msUnbind    = RectF(S(222), S(168), S(84),  S(30));
    L.dlgCard     = RectF(S(150), S(140), S(460), S(212));
    L.dlgOpen     = RectF(S(247), S(296), S(150), S(34));
    L.dlgOk       = RectF(S(409), S(296), S(104), S(34));
    L.minBtn      = RectF(S(662), S(16),  S(30),  S(30));
    L.closeBtn    = RectF(S(700), S(16),  S(30),  S(30));
    L.panel1      = RectF(S(28),  S(82),  S(704), S(166));
    L.userBox     = RectF(S(52),  S(126), S(300), S(42));
    L.verBox      = RectF(S(376), S(126), S(332), S(42));
    L.sliderTrack = RectF(S(52),  S(224), S(656), S(6));
    L.panel2      = RectF(S(28),  S(262), S(704), S(66));
    L.browseBtn   = RectF(S(636), S(278), S(72),  S(34));
    L.launchBtn   = RectF(S(28),  S(340), S(704), S(50));
    L.logArea     = RectF(S(28),  S(406), S(704), S(64));
    return L;
}

// ============================================================================
//  应用状态
// ============================================================================
enum { HIT_NONE = 0, HIT_CLOSE, HIT_MIN, HIT_USER, HIT_VER, HIT_SLIDER, HIT_BROWSE, HIT_LAUNCH,
       HIT_MS_OFF, HIT_MS_ON, HIT_MS_UNBIND };
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
    float progress  = 0.0f;          // 实际绘制用的进度（由 16ms 定时器平滑逼近 progressTarget）
    float progressTarget = 0.0f;     // 当前步骤的进度目标
    int   step      = 0;
    int   resetTick = 0;
    bool  stepRan[5] = { false, false, false, false, false };  // 该步的真实工作是否已执行
    bool  msDlg     = false;         // 微软登录验证码弹窗（逻辑开关）
    float msAnim    = 0.0f;          // 弹窗动画进度 0..1
    int   dlgWait   = 0;             // 弹窗停留后打开浏览器的倒计时（秒，0 = 已开）
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

static void SaveConfig() {
    std::wstring dir = AppDataConfigDir();
    CreateDirectoryW(dir.c_str(), nullptr);
    std::ofstream f(W2U8(ConfigPath()).c_str());
    if (!f) return;
    f << "username=" << W2U8(g_offlineName) << "\n";
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
    return g_app.gameDir.empty() ? DefaultMinecraftDir() : g_app.gameDir;
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

        // 内存滑块
        DrawStr(gr, L"内存分配", RectF(S(54), S(200), S(240), S(18)), *F.label, C(Theme::Muted));
        DrawStr(gr, std::to_wstring(g_app.memGb) + L" GB", RectF(S(52), S(198), S(656), S(20)),
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
    if (InRect(L.msOffBtn, x, y))  return HIT_MS_OFF;
    if (InRect(L.msOnBtn, x, y))   return HIT_MS_ON;
    if (g_ms.state == MS_LOGGED_IN && InRect(L.msUnbind, x, y)) return HIT_MS_UNBIND;
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

    case WM_TIMER: {
        if (wParam == 2) {                       // 弹窗动画（15ms 一帧，160ms 跑完）
            const float dt = 0.015f / 0.16f;
            const float target = g_app.msDlg ? 1.0f : 0.0f;
            if (g_app.msAnim < target)      g_app.msAnim = std::min(target, g_app.msAnim + dt);
            else if (g_app.msAnim > target) g_app.msAnim = std::max(target, g_app.msAnim - dt);
            if (std::fabs(g_app.msAnim - target) < 0.001f) {
                g_app.msAnim = target;
                KillTimer(hwnd, 2);
            }
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

        if (g_app.draggingSlider) { SetSliderFromX(x); return 0; }

        int h = HitTest(x, y);
        if (h != g_app.hover) { g_app.hover = h; InvalidateRect(hwnd, nullptr, FALSE); }

        int di = g_app.ddOpen ? DropdownItemAt(x, y) : -1;
        if (di != g_app.ddHover) { g_app.ddHover = di; InvalidateRect(hwnd, nullptr, FALSE); }

        LPCWSTR cur = IDC_ARROW;
        if (h == HIT_CLOSE || h == HIT_MIN || h == HIT_BROWSE || h == HIT_LAUNCH ||
            h == HIT_VER || h == HIT_MS_OFF || h == HIT_MS_ON || h == HIT_MS_UNBIND)
            cur = IDC_HAND;
        else if (h == HIT_USER)
            cur = (g_ms.state == MS_LOGGED_IN) ? IDC_ARROW : IDC_IBEAM;
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
        }
        return 0;
    }

    case WM_KEYDOWN:
        if (g_app.msDlg) {
            if (wParam == VK_ESCAPE || wParam == VK_RETURN) {
                AnimMsDialog(false);
                return 0;
            }
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

    // 有已保存的账号就静默续期（不阻塞首屏）
    MsAutoRefresh();

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
