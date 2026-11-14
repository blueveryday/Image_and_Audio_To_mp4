// main.cpp —— 图片 / 视频 + 音频 合成 MP4 (ffmpeg) 的 Windows 界面(纯 Win32,无第三方依赖)
//
//   标签页 1:图片 + 音频 -> MP4
//   标签页 2:无声视频 + 音频(mp3 / aac ...) -> 带声音的 MP4,可设置分辨率、帧率、码率等
//
// 命令生成与 ffprobe 解析都在 core.hpp 中(有单元测试),这里只负责界面、启动 ffmpeg 并显示日志。
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "core.hpp"

#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#endif

using std::string;
using std::vector;
using std::wstring;

// ---------------------------------------------------------------------------
// 控件 ID
// ---------------------------------------------------------------------------
enum : int {
    IDC_FFMPEG_EDIT = 101,
    IDC_FFMPEG_BROWSE = 102,
    IDC_TAB = 103,

    // 标签页 1
    T1_AUDIO = 1001, T1_AUDIO_BTN, T1_IMAGE, T1_IMAGE_BTN, T1_OUT, T1_OUT_BTN,
    T1_WIDTH, T1_HEIGHT, T1_SCALEPAD, T1_VCODEC, T1_TUNE, T1_USETUNE, T1_ACODEC, T1_PIXFMT,
    T1_SHORTEST, T1_OVERWRITE, T1_EXTRA, T1_GEN, T1_CMD, T1_RUN, T1_STOP, T1_CLEAR, T1_LOG,

    // 标签页 2:按钮 / 文本框等
    T2_VIDEO_BTN = 2001, T2_AUDIO_BTN, T2_OUT_BTN, T2_GEN, T2_RUN, T2_STOP, T2_CLEAR, T2_CMD, T2_LOG,
    T2_VINFO, T2_AINFO, T2_HINT, T2_NOTE, T2_CRF_SPIN,

    // 标签页 2:选项(任何改动都会重新生成命令)
    T2_OPT_FIRST = 2100,
    T2_VIDEO = T2_OPT_FIRST, T2_AUDIO, T2_OUT,
    T2_REENCODE, T2_RES, T2_WIDTH, T2_HEIGHT, T2_PAD, T2_FPS, T2_VCODEC, T2_PRESET,
    T2_RC_CRF, T2_RC_ABR, T2_CRF, T2_VBR, T2_MAXRATE, T2_PIXFMT,
    T2_ACODEC, T2_ABITRATE, T2_ARATE, T2_ACHANNELS, T2_DURATION, T2_FASTSTART, T2_OVERWRITE, T2_EXTRA,
    T2_OPT_LAST = T2_EXTRA,
};

enum : UINT {
    WM_APP_LOG = WM_APP + 1,    // wParam: 页, lParam: LogMsg*
    WM_APP_DONE = WM_APP + 2,   // wParam: 页, lParam: 退出码
    WM_APP_PROBE = WM_APP + 3,  // lParam: ProbeMsg*
};

enum { F_W = 1, F_H = 2, F_X = 4 };  // 窗口放大时:拉宽 / 拉高 / 右移

// 页面的设计尺寸(96 DPI 下的逻辑像素)
static const int PW = 972, PH = 632;
static const int WIN_W = 1000, WIN_H = 740;

// ---------------------------------------------------------------------------
// 全局状态
// ---------------------------------------------------------------------------
struct Item {
    HWND hw;
    int x, y, w, h, flags;
};

struct Page {
    HWND hwnd = nullptr;
    vector<Item> items;
    // 运行 ffmpeg 相关
    HWND cmd = nullptr, log = nullptr, runBtn = nullptr, stopBtn = nullptr;
    HANDLE proc = nullptr;
    bool running = false;
    bool lastProgress = false;  // 日志最后一行是否为可被覆盖的进度行
    int progressStart = 0;
};

static HINSTANCE g_inst;
static HWND g_main, g_tab, g_ffGroup;
static Page g_pages[2];
static std::map<int, HWND> g_ctl;
static HFONT g_font, g_mono;
static HBRUSH g_logBrush;
static int g_dpi = 96;
static int g_busy = 0;       // >0 时忽略控件通知(程序自己改控件内容时)
static bool g_ready = false;
static int g_minW = 0, g_minH = 0;
static wstring g_autoOutput2;  // 标签页 2 上一次自动填入的输出路径

static int S(int v) { return MulDiv(v, g_dpi, 96); }
static HWND C(int id) { return g_ctl[id]; }

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
static wstring getText(HWND h) {
    int n = GetWindowTextLengthW(h);
    wstring s(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(h, &s[0], n + 1);
    s.resize(static_cast<size_t>(n));
    return s;
}
static wstring T(int id) { return getText(C(id)); }

static void setText(HWND h, const wstring& s) {
    ++g_busy;
    SetWindowTextW(h, s.c_str());
    --g_busy;
}
static void setTextId(int id, const wstring& s) { setText(C(id), s); }

static void scrollEnd(HWND h) {
    int n = GetWindowTextLengthW(h);
    SendMessageW(h, EM_SETSEL, n, n);
    SendMessageW(h, EM_SCROLLCARET, 0, 0);
}

static bool isChecked(int id) { return SendMessageW(C(id), BM_GETCHECK, 0, 0) == BST_CHECKED; }
static void setChecked(int id, bool on) { SendMessageW(C(id), BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0); }
static int curSel(int id) { return static_cast<int>(SendMessageW(C(id), CB_GETCURSEL, 0, 0)); }

static bool fileExists(const wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static wstring searchPath(const wstring& name, const wchar_t* ext) {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = SearchPathW(nullptr, name.c_str(), ext, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])), buf, nullptr);
    return (n > 0 && n < sizeof(buf) / sizeof(buf[0])) ? wstring(buf) : wstring();
}

static wstring exeDir() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
    return core::path_dirname(wstring(buf, n));
}

static wstring findDefaultFfmpeg() {
    wstring dir = exeDir();
    wstring c1 = dir + L"\\ffmpeg.exe";
    if (fileExists(c1)) return c1;
    wchar_t cwd[MAX_PATH * 4];
    DWORD n = GetCurrentDirectoryW(static_cast<DWORD>(sizeof(cwd) / sizeof(cwd[0])), cwd);
    if (n > 0 && n < sizeof(cwd) / sizeof(cwd[0])) {
        wstring c2 = wstring(cwd, n) + L"\\ffmpeg.exe";
        if (fileExists(c2)) return c2;
    }
    wstring found = searchPath(L"ffmpeg", L".exe");
    if (!found.empty()) return found;
    return c1;
}

// 优先找 ffmpeg 同目录下的 ffprobe,其次找 PATH;找不到返回空
static wstring findFfprobe(const wstring& ffmpegPath) {
    wstring dir = core::path_dirname(ffmpegPath);
    if (!dir.empty()) {
        wstring cand = dir + L"\\ffprobe.exe";
        if (fileExists(cand)) return cand;
    }
    return searchPath(L"ffprobe", L".exe");
}

static bool onPath(const wstring& name) { return !searchPath(name, L".exe").empty(); }

static bool askYesNo(const wchar_t* title, const wstring& text) {
    return MessageBoxW(g_main, text.c_str(), title, MB_YESNO | MB_ICONQUESTION) == IDYES;
}
static void showError(const wchar_t* title, const wstring& text) {
    MessageBoxW(g_main, text.c_str(), title, MB_OK | MB_ICONERROR);
}
static void showWarning(const wchar_t* title, const wstring& text) {
    MessageBoxW(g_main, text.c_str(), title, MB_OK | MB_ICONWARNING);
}

static wstring fullPathLower(const wstring& p) {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetFullPathNameW(p.c_str(), static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])), buf, nullptr);
    wstring r = (n > 0 && n < sizeof(buf) / sizeof(buf[0])) ? wstring(buf, n) : p;
    CharLowerBuffW(&r[0], static_cast<DWORD>(r.size()));
    return r;
}

static wstring lastErrorText(DWORD err) {
    wchar_t* msg = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, err, 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
    wstring r = (n && msg) ? wstring(msg, n) : L"错误码 " + std::to_wstring(err);
    if (msg) LocalFree(msg);
    while (!r.empty() && (r.back() == L'\r' || r.back() == L'\n' || r.back() == L' ')) r.pop_back();
    return r;
}

// ffmpeg 输出的编码:优先 UTF-8,不行再试 GBK,最后用系统代码页
static wstring decodeBytes(const string& s) {
    if (s.empty()) return L"";
    const int len = static_cast<int>(s.size());
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), len, nullptr, 0);
    if (n > 0) {
        wstring w(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), len, &w[0], n);
        return w;
    }
    for (UINT cp : {936u, static_cast<UINT>(CP_ACP)}) {
        n = MultiByteToWideChar(cp, 0, s.data(), len, nullptr, 0);
        if (n > 0) {
            wstring w(static_cast<size_t>(n), L'\0');
            MultiByteToWideChar(cp, 0, s.data(), len, &w[0], n);
            return w;
        }
    }
    return wstring(s.begin(), s.end());
}

// ---------------------------------------------------------------------------
// 文件对话框
// ---------------------------------------------------------------------------
typedef vector<std::pair<wstring, wstring>> Filters;  // {说明, "*.a *.b"}

static wstring buildFilter(const Filters& f) {
    wstring r;
    for (auto& p : f) {
        r += p.first;
        r += L'\0';
        wstring pat = p.second;
        for (auto& c : pat)
            if (c == L' ') c = L';';
        r += pat;
        r += L'\0';
    }
    r += L'\0';
    return r;
}

static bool pickFile(const wchar_t* title, const Filters& f, bool save, wstring& out) {
    wchar_t file[MAX_PATH * 4] = L"";
    wstring filter = buildFilter(f);
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = file;
    ofn.nMaxFile = static_cast<DWORD>(sizeof(file) / sizeof(file[0]));
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_EXPLORER | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST;
    BOOL ok;
    if (save) {
        ofn.lpstrDefExt = L"mp4";
        ofn.Flags |= OFN_OVERWRITEPROMPT;
        ok = GetSaveFileNameW(&ofn);
    } else {
        ofn.Flags |= OFN_FILEMUSTEXIST;
        ok = GetOpenFileNameW(&ofn);
    }
    if (!ok) return false;
    out = file;
    return true;
}

static const Filters IMAGE_FILTERS = {{L"图片文件", L"*.jpg *.jpeg *.png *.bmp *.webp *.tif *.tiff *.gif"}, {L"所有文件", L"*.*"}};
static const Filters AUDIO_FILTERS = {{L"音频文件", L"*.mp3 *.wav *.flac *.aac *.m4a *.ogg *.wma *.opus"}, {L"所有文件", L"*.*"}};
static const Filters VIDEO_IN_FILTERS = {{L"视频文件", L"*.mp4 *.m4v *.mov *.mkv *.avi *.webm"}, {L"所有文件", L"*.*"}};
static const Filters VIDEO_OUT_FILTERS = {{L"MP4 文件", L"*.mp4"}, {L"所有文件", L"*.*"}};
static const Filters EXE_FILTERS = {{L"ffmpeg.exe", L"*.exe"}};

// ---------------------------------------------------------------------------
// 控件创建
// ---------------------------------------------------------------------------
struct Builder {
    Page& pg;

    HWND add(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, DWORD ex = 0,
             int flags = 0) {
        HWND hw = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), pg.hwnd,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_inst, nullptr);
        SendMessageW(hw, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
        pg.items.push_back({hw, x, y, w, h, flags});
        if (id) g_ctl[id] = hw;
        return hw;
    }
    HWND group(const wchar_t* text, int x, int y, int w, int h, int flags = F_W) {
        return add(L"BUTTON", text, BS_GROUPBOX, x, y, w, h, 0, 0, flags);
    }
    HWND label(const wchar_t* text, int x, int y, int w, bool right = false, int id = 0, int flags = 0, int h = 18) {
        return add(L"STATIC", text, right ? SS_RIGHT : SS_LEFT, x, y + 4, w, h, id, 0, flags);
    }
    HWND edit(int id, int x, int y, int w, const wstring& text, int flags = 0) {
        HWND h = add(L"EDIT", text.c_str(), WS_TABSTOP | ES_AUTOHSCROLL, x, y, w, 23, id, WS_EX_CLIENTEDGE, flags);
        return h;
    }
    HWND check(int id, const wchar_t* text, int x, int y, int w, bool checked) {
        HWND h = add(L"BUTTON", text, WS_TABSTOP | BS_AUTOCHECKBOX, x, y + 1, w, 20, id);
        setChecked(id, checked);
        return h;
    }
    HWND button(int id, const wchar_t* text, int x, int y, int w, int h = 26, int flags = 0, bool disabled = false) {
        HWND b = add(L"BUTTON", text, WS_TABSTOP | BS_PUSHBUTTON | (disabled ? WS_DISABLED : 0), x, y, w, h, id, 0, flags);
        return b;
    }
    HWND combo(int id, int x, int y, int w, bool editable, const vector<const wchar_t*>& items, int sel = 0) {
        HWND h = add(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | (editable ? (CBS_DROPDOWN | CBS_AUTOHSCROLL) : CBS_DROPDOWNLIST), x,
                     y, w, 220, id);
        for (auto* s : items) SendMessageW(h, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
        SendMessageW(h, CB_SETCURSEL, sel, 0);
        return h;
    }
    // 文件行:标签 + 文本框 + 浏览按钮
    void fileRow(const wchar_t* label_, int editId, int btnId, int y) {
        label(label_, 12, y, 70);
        edit(editId, 88, y, PW - 88 - 12 - 84, L"", F_W);
        button(btnId, L"浏览...", PW - 12 - 76, y - 1, 76, 25, F_X);
    }
    // “完整命令行”框 + 运行/停止/清空日志 + 日志框,返回按钮行的 y 坐标之后的下一个 x
    void runArea(int cmdId, int runId, int stopId, int clearId, int logId, int cmdY, int cmdH, const wchar_t* cmdTitle) {
        group(cmdTitle, 0, cmdY, PW, cmdH, F_W);
        pg.cmd = add(L"EDIT", L"", WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN, 8, cmdY + 18,
                     PW - 16, cmdH - 26, cmdId, WS_EX_CLIENTEDGE, F_W);
        SendMessageW(pg.cmd, WM_SETFONT, reinterpret_cast<WPARAM>(g_mono), TRUE);

        int by = cmdY + cmdH + 6;
        pg.runBtn = button(runId, L"▶ 运行", 8, by, 88, 28);
        pg.stopBtn = button(stopId, L"■ 停止", 104, by, 88, 28, 0, true);
        button(clearId, L"清空日志", 200, by, 88, 28);

        int ly = by + 34;
        group(L"日志输出", 0, ly, PW, PH - ly, F_W | F_H);
        pg.log = add(L"EDIT", L"", WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY, 8, ly + 18, PW - 16,
                     PH - ly - 26, logId, WS_EX_CLIENTEDGE, F_W | F_H);
        SendMessageW(pg.log, WM_SETFONT, reinterpret_cast<WPARAM>(g_mono), TRUE);
        SendMessageW(pg.log, EM_SETLIMITTEXT, 16 * 1024 * 1024, 0);
    }
};

static void buildPage1() {
    Page& P = g_pages[0];
    Builder b{P};

    b.group(L"输入 / 输出文件", 0, 0, PW, 108);
    b.fileRow(L"音频文件:", T1_AUDIO, T1_AUDIO_BTN, 20);
    b.fileRow(L"图片文件:", T1_IMAGE, T1_IMAGE_BTN, 48);
    b.fileRow(L"输出文件:", T1_OUT, T1_OUT_BTN, 76);

    b.group(L"命令行参数(可自定义,仅用作生成命令的默认值)", 0, 110, PW, 196);
    int y = 130;
    b.label(L"输出宽度:", 12, y, 130, true);
    b.edit(T1_WIDTH, 148, y, 110, L"1280");
    b.label(L"输出高度:", 290, y, 140, true);
    b.edit(T1_HEIGHT, 436, y, 110, L"720");
    b.check(T1_SCALEPAD, L"使用 scale+pad 保持宽高比并填充黑边", 570, y, 380, true);

    y += 28;
    b.label(L"视频编码器 (-c:v):", 12, y, 130, true);
    b.edit(T1_VCODEC, 148, y, 110, L"libx264");
    b.label(L"tune:", 290, y, 140, true);
    b.edit(T1_TUNE, 436, y, 110, L"stillimage");
    b.check(T1_USETUNE, L"启用 -tune", 570, y, 200, true);

    y += 28;
    b.label(L"音频编码器 (-c:a):", 12, y, 130, true);
    b.edit(T1_ACODEC, 148, y, 110, L"aac");
    b.label(L"像素格式 (-pix_fmt):", 290, y, 140, true);
    b.edit(T1_PIXFMT, 436, y, 110, L"yuv420p");

    y += 28;
    b.check(T1_SHORTEST, L"使用 -shortest(以短的一方为准)", 12, y, 270, true);
    b.check(T1_OVERWRITE, L"覆盖已存在的输出文件 (-y)", 290, y, 300, true);

    y += 28;
    b.label(L"额外自定义参数:", 12, y, 130, true);
    b.edit(T1_EXTRA, 148, y, PW - 148 - 14, L"", F_W);

    y += 30;
    b.button(T1_GEN, L"根据以上选项生成命令 →", 12, y, PW - 24, 26, F_W);

    b.runArea(T1_CMD, T1_RUN, T1_STOP, T1_CLEAR, T1_LOG, 310, 110, L"完整命令行(可直接编辑,运行时以此文本框内容为准)");
}

static void buildPage2() {
    Page& P = g_pages[1];
    Builder b{P};

    // 文件
    b.group(L"输入 / 输出文件(视频为无声 mp4,音频为 mp3 / aac 等)", 0, 0, PW, 146);
    b.fileRow(L"视频文件:", T2_VIDEO, T2_VIDEO_BTN, 20);
    b.fileRow(L"音频文件:", T2_AUDIO, T2_AUDIO_BTN, 48);
    b.fileRow(L"输出文件:", T2_OUT, T2_OUT_BTN, 76);
    b.label(L"", 20, 100, PW - 40, false, T2_VINFO, F_W, 18);
    b.label(L"", 20, 120, PW - 40, false, T2_AINFO, F_W, 18);

    // 视频参数
    const int gy = 148;
    b.group(L"视频参数", 0, gy, 590, 206, 0);
    b.check(T2_REENCODE, L"重新编码视频(取消勾选则直接复制视频流,画质无损、速度快)", 12, gy + 20, 560, true);

    int y = gy + 46;
    b.label(L"分辨率:", 12, y, 70, true);
    {
        vector<const wchar_t*> labels;
        for (auto& r : core::res_presets()) labels.push_back(r.label);
        b.combo(T2_RES, 88, y, 150, false, labels, 0);
    }
    b.label(L"宽:", 252, y, 22, true);
    b.edit(T2_WIDTH, 278, y, 60, L"1920");
    b.label(L"高:", 346, y, 22, true);
    b.edit(T2_HEIGHT, 372, y, 60, L"1080");

    y = gy + 74;
    b.check(T2_PAD, L"保持宽高比并填充黑边", 12, y, 240, true);
    b.label(L"帧率 (fps):", 296, y, 74, true);
    b.combo(T2_FPS, 376, y, 100, true, core::fps_values(), 0);

    y = gy + 102;
    b.label(L"视频编码器:", 12, y, 70, true);
    b.combo(T2_VCODEC, 88, y, 130, true, core::vcodec_values(), 0);
    b.label(L"编码速度:", 296, y, 74, true);
    b.combo(T2_PRESET, 376, y, 100, false, core::preset_values(), 5);

    y = gy + 130;
    HWND radioCrf = b.add(L"BUTTON", L"恒定质量 CRF:", WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON, 12, y + 1, 120, 20, T2_RC_CRF);
    (void)radioCrf;
    b.edit(T2_CRF, 136, y, 60, L"23");
    {
        HWND spin = b.add(UPDOWN_CLASSW, L"", UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS, 0, 0, 0, 0,
                          T2_CRF_SPIN);
        SendMessageW(spin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(C(T2_CRF)), 0);
        SendMessageW(spin, UDM_SETRANGE32, 0, 51);
        SendMessageW(spin, UDM_SETPOS32, 0, 23);
        P.items.pop_back();  // 位置由 buddy 决定,不参与布局
    }
    b.add(L"BUTTON", L"目标码率(kbps):", BS_AUTORADIOBUTTON, 296, y + 1, 124, 20, T2_RC_ABR);
    b.edit(T2_VBR, 426, y, 70, L"4000");
    setChecked(T2_RC_CRF, true);

    y = gy + 158;
    b.label(L"最大码率(kbps):", 12, y, 100, true);
    b.edit(T2_MAXRATE, 118, y, 70, L"");
    b.label(L"像素格式:", 296, y, 74, true);
    b.combo(T2_PIXFMT, 376, y, 100, true, core::pixfmt_values(), 0);

    b.label(L"CRF 越小画质越高(常用 18~28);码率单位 kbps,可写 4000 或 5M", 12, gy + 184, 560, false, T2_HINT, 0, 16);

    // 音频 / 合成参数
    const int ax = 596;
    b.group(L"音频 / 合成参数", ax, gy, PW - ax, 206, F_W);
    y = gy + 20;
    b.label(L"音频编码器:", ax + 12, y, 76, true);
    b.combo(T2_ACODEC, ax + 94, y, 96, true, core::acodec_values(), 0);
    b.label(L"音频码率:", ax + 196, y, 70, true);
    b.combo(T2_ABITRATE, ax + 272, y, 80, true, core::abitrate_values(), 3);

    y = gy + 48;
    b.label(L"采样率 (Hz):", ax + 12, y, 76, true);
    b.combo(T2_ARATE, ax + 94, y, 96, true, core::arate_values(), 0);
    b.label(L"声道:", ax + 196, y, 70, true);
    b.combo(T2_ACHANNELS, ax + 272, y, 80, false, core::achannel_values(), 0);

    y = gy + 76;
    b.label(L"时长处理:", ax + 12, y, 76, true);
    {
        vector<const wchar_t*> labels;
        for (auto& d : core::duration_modes()) labels.push_back(d.label);
        b.combo(T2_DURATION, ax + 94, y, 258, false, labels, 0);
    }

    y = gy + 106;
    b.check(T2_FASTSTART, L"faststart(便于网页边下边播)", ax + 12, y, 340, true);
    y = gy + 132;
    b.check(T2_OVERWRITE, L"覆盖输出文件 (-y)", ax + 12, y, 340, true);
    y = gy + 160;
    b.label(L"额外参数:", ax + 12, y, 76, true);
    b.edit(T2_EXTRA, ax + 94, y, PW - ax - 94 - 12, L"", F_W);

    // 命令 + 运行
    b.runArea(T2_CMD, T2_RUN, T2_STOP, T2_CLEAR, T2_LOG, 358, 74, L"完整命令行(随上方选项自动更新;也可直接编辑,运行时以此文本框内容为准)");
    int by = 358 + 74 + 6;
    b.button(T2_GEN, L"↻ 按选项重新生成命令", 296, by, 160, 28);
    b.label(L"", 466, by - 1, PW - 466 - 8, false, T2_NOTE, F_W, 32);
}

// ---------------------------------------------------------------------------
// 布局
// ---------------------------------------------------------------------------
static void layoutPage(int idx) {
    Page& P = g_pages[idx];
    RECT rc;
    GetClientRect(P.hwnd, &rc);
    int dw = std::max(0, static_cast<int>(rc.right) - S(PW));
    int dh = std::max(0, static_cast<int>(rc.bottom) - S(PH));
    for (auto& it : P.items) {
        if (!it.flags) continue;
        int x = S(it.x) + ((it.flags & F_X) ? dw : 0);
        int w = S(it.w) + ((it.flags & F_W) ? dw : 0);
        int h = S(it.h) + ((it.flags & F_H) ? dh : 0);
        SetWindowPos(it.hw, nullptr, x, S(it.y), w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
    }
    RedrawWindow(P.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);  // 清掉分组框旧边缘的残影
}

static void layoutMain(int cw, int ch) {
    // ffmpeg 路径
    MoveWindow(g_ffGroup, S(8), S(4), cw - S(16), S(56), TRUE);
    int btnW = S(84);
    int btnX = cw - S(8) - S(10) - btnW;
    MoveWindow(C(IDC_FFMPEG_BROWSE), btnX, S(26), btnW, S(26), TRUE);
    MoveWindow(C(IDC_FFMPEG_EDIT), S(18), S(27), btnX - S(18) - S(8), S(24), TRUE);

    // 标签页
    int ty = S(66);
    MoveWindow(g_tab, S(8), ty, cw - S(16), ch - ty - S(8), TRUE);
    RECT rc;
    GetClientRect(g_tab, &rc);
    SendMessageW(g_tab, TCM_ADJUSTRECT, FALSE, reinterpret_cast<LPARAM>(&rc));
    POINT tl = {rc.left, rc.top};  // 页面是标签控件的子窗口,坐标即标签控件客户区坐标
    for (int i = 0; i < 2; ++i) {
        MoveWindow(g_pages[i].hwnd, tl.x, tl.y, rc.right - rc.left, rc.bottom - rc.top, TRUE);
        layoutPage(i);
    }
    RedrawWindow(g_main, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

// ---------------------------------------------------------------------------
// 标签页 1:读取选项 / 生成命令 / 自动输出名
// ---------------------------------------------------------------------------
static core::ImageAudioOpts readT1() {
    core::ImageAudioOpts o;
    o.ffmpeg = T(IDC_FFMPEG_EDIT);
    o.image = T(T1_IMAGE);
    o.audio = T(T1_AUDIO);
    o.output = T(T1_OUT);
    o.width = T(T1_WIDTH);
    o.height = T(T1_HEIGHT);
    o.scale_pad = isChecked(T1_SCALEPAD);
    o.vcodec = T(T1_VCODEC);
    o.tune = T(T1_TUNE);
    o.use_tune = isChecked(T1_USETUNE);
    o.acodec = T(T1_ACODEC);
    o.pixfmt = T(T1_PIXFMT);
    o.shortest = isChecked(T1_SHORTEST);
    o.overwrite = isChecked(T1_OVERWRITE);
    o.extra = T(T1_EXTRA);
    return o;
}

static void updatePreview1() { setText(g_pages[0].cmd, core::join_cmd(core::build_image_audio(readT1()))); }

static void autoFillOutput1() {
    if (!core::trim(T(T1_OUT)).empty()) return;
    wstring out = core::auto_output_image_audio(core::trim(T(T1_AUDIO)), core::trim(T(T1_IMAGE)));
    if (!out.empty()) {
        setTextId(T1_OUT, out);
        scrollEnd(C(T1_OUT));
    }
}

// ---------------------------------------------------------------------------
// 标签页 2:读取选项 / 生成命令 / 控件联动
// ---------------------------------------------------------------------------
static core::VideoAudioOpts readT2() {
    core::VideoAudioOpts o;
    o.ffmpeg = T(IDC_FFMPEG_EDIT);
    o.video = T(T2_VIDEO);
    o.audio = T(T2_AUDIO);
    o.output = T(T2_OUT);
    o.reencode = isChecked(T2_REENCODE);
    o.res_keep = curSel(T2_RES) <= 0;
    o.width = T(T2_WIDTH);
    o.height = T(T2_HEIGHT);
    o.pad = isChecked(T2_PAD);
    o.fps = T(T2_FPS);
    o.vcodec = T(T2_VCODEC);
    o.preset = T(T2_PRESET);
    o.crf_mode = isChecked(T2_RC_CRF);
    o.crf = T(T2_CRF);
    o.vbr = T(T2_VBR);
    o.maxrate = T(T2_MAXRATE);
    o.pixfmt = T(T2_PIXFMT);
    o.acodec = T(T2_ACODEC);
    o.abitrate = T(T2_ABITRATE);
    o.arate = T(T2_ARATE);
    o.achannels = T(T2_ACHANNELS);
    int d = curSel(T2_DURATION);
    const auto& modes = core::duration_modes();
    o.mode = (d >= 0 && d < static_cast<int>(modes.size())) ? modes[static_cast<size_t>(d)].mode : core::DurMode::Shortest;
    o.faststart = isChecked(T2_FASTSTART);
    o.overwrite = isChecked(T2_OVERWRITE);
    o.extra = T(T2_EXTRA);
    return o;
}

static void syncStates2() {
    core::VideoAudioOpts o = readT2();
    bool reenc = core::effective_reencode(o);
    bool keepRes = o.res_keep;
    bool crfMode = o.crf_mode;

    auto en = [](int id, bool on) { EnableWindow(C(id), on ? TRUE : FALSE); };
    en(T2_RES, reenc);
    en(T2_WIDTH, reenc && !keepRes);
    en(T2_HEIGHT, reenc && !keepRes);
    en(T2_PAD, reenc && !keepRes);
    en(T2_PRESET, reenc);
    for (int id : {T2_FPS, T2_VCODEC, T2_PIXFMT, T2_MAXRATE, T2_RC_CRF, T2_RC_ABR}) en(id, reenc);
    en(T2_CRF, reenc && crfMode);
    en(T2_CRF_SPIN, reenc && crfMode);
    en(T2_VBR, reenc && !crfMode);

    bool acopy = core::effective_acodec(o) == L"copy";
    en(T2_ABITRATE, !acopy);
    en(T2_ARATE, !acopy);
    en(T2_ACHANNELS, !acopy);
}

static void updatePreview2() {
    core::Built b = core::build_video_audio(readT2());
    setText(g_pages[1].cmd, core::join_cmd(b.cmd));
    setTextId(T2_NOTE, core::notes_text(b.notes));
}

static void onOptionsChanged2() {
    if (!g_ready) return;
    syncStates2();
    updatePreview2();
}

static void autoFillOutput2() {
    wstring current = core::trim(T(T2_OUT));
    if (!current.empty() && current != g_autoOutput2) return;
    wstring video = core::trim(T(T2_VIDEO));
    if (video.empty()) return;
    wstring out = core::auto_output_video(video);
    if (out.empty()) return;
    g_autoOutput2 = out;
    setTextId(T2_OUT, out);
    scrollEnd(C(T2_OUT));
}

// ---------------------------------------------------------------------------
// 运行 ffmpeg(子进程 + 管道 + 后台线程读输出)
// ---------------------------------------------------------------------------
struct LogMsg {
    int page;
    wstring text;
    int term;  // 1: 以换行结束;2: 以 \r 结束(进度行,下一条会覆盖它)
};
struct ReaderCtx {
    int page;
    HANDLE read;
    HANDLE proc;
};

static void postLog(int page, const string& bytes, int term) {
    LogMsg* m = new LogMsg{page, decodeBytes(bytes), term};
    PostMessageW(g_main, WM_APP_LOG, static_cast<WPARAM>(page), reinterpret_cast<LPARAM>(m));
}

static DWORD WINAPI readerThread(LPVOID param) {
    ReaderCtx* ctx = static_cast<ReaderCtx*>(param);
    string pend;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(ctx->read, buf, sizeof(buf), &n, nullptr) && n > 0) {
        pend.append(buf, n);
        size_t start = 0;
        for (size_t i = 0; i < pend.size(); ++i) {
            char c = pend[i];
            if (c == '\n') {
                postLog(ctx->page, pend.substr(start, i - start), 1);
                start = i + 1;
            } else if (c == '\r') {
                if (i + 1 >= pend.size()) break;  // 还不知道后面是不是 \n,等下一批数据
                if (pend[i + 1] == '\n') {
                    postLog(ctx->page, pend.substr(start, i - start), 1);
                    start = i + 2;
                    ++i;
                } else {
                    postLog(ctx->page, pend.substr(start, i - start), 2);
                    start = i + 1;
                }
            }
        }
        pend.erase(0, start);
    }
    if (!pend.empty()) {
        bool prog = pend.back() == '\r';
        if (prog) pend.pop_back();
        postLog(ctx->page, pend, prog ? 2 : 1);
    }
    WaitForSingleObject(ctx->proc, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(ctx->proc, &code);
    CloseHandle(ctx->read);
    PostMessageW(g_main, WM_APP_DONE, static_cast<WPARAM>(ctx->page), static_cast<LPARAM>(code));
    delete ctx;
    return 0;
}

// 在日志末尾原样追加文字(并结束“进度行”状态)
static void logRaw(Page& P, const wstring& text) {
    P.lastProgress = false;
    int len = GetWindowTextLengthW(P.log);
    SendMessageW(P.log, EM_SETSEL, len, len);
    SendMessageW(P.log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
    SendMessageW(P.log, EM_SCROLLCARET, 0, 0);
}

static void appendLog(Page& P, const wstring& text, int term) {
    if (P.lastProgress) {  // 覆盖上一条进度
        SendMessageW(P.log, EM_SETSEL, P.progressStart, -1);
        SendMessageW(P.log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
    }
    int len = GetWindowTextLengthW(P.log);
    P.progressStart = len;
    wstring t = text;
    if (term != 2) t += L"\r\n";
    SendMessageW(P.log, EM_SETSEL, len, len);
    SendMessageW(P.log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(t.c_str()));
    SendMessageW(P.log, EM_SCROLLCARET, 0, 0);
    P.lastProgress = (term == 2);
}

static void clearLog(Page& P) {
    SetWindowTextW(P.log, L"");
    P.lastProgress = false;
}

static void setRunning(Page& P, bool running) {
    P.running = running;
    EnableWindow(P.runBtn, running ? FALSE : TRUE);
    EnableWindow(P.stopBtn, running ? TRUE : FALSE);
}

// 运行前检查输入;返回 false 则不运行
static bool checkInputs(int idx, const wstring& cmdStr) {
    auto has = [&](const wchar_t* s) { return cmdStr.find(s) != wstring::npos; };
    if (idx == 0) {
        wstring image = core::trim(T(T1_IMAGE)), audio = core::trim(T(T1_AUDIO));
        if (has(core::PH_IMAGE) || (!image.empty() && !fileExists(image)))
            if (!askYesNo(L"提示", L"图片文件路径可能无效,仍要继续吗?")) return false;
        if (has(core::PH_AUDIO) || (!audio.empty() && !fileExists(audio)))
            if (!askYesNo(L"提示", L"音频文件路径可能无效,仍要继续吗?")) return false;
        return true;
    }
    wstring video = core::trim(T(T2_VIDEO)), audio = core::trim(T(T2_AUDIO)), output = core::trim(T(T2_OUT));
    if (has(core::PH_VIDEO) || (!video.empty() && !fileExists(video)))
        if (!askYesNo(L"提示", L"视频文件路径可能无效,仍要继续吗?")) return false;
    if (has(core::PH_AUDIO) || (!audio.empty() && !fileExists(audio)))
        if (!askYesNo(L"提示", L"音频文件路径可能无效,仍要继续吗?")) return false;
    if (output.empty() || has(core::PH_OUTPUT)) {
        showError(L"错误", L"请先设置输出文件。");
        return false;
    }
    wstring out = fullPathLower(output);
    for (const wstring& in : {video, audio}) {
        if (!in.empty() && fullPathLower(in) == out) {
            showError(L"错误", L"输出文件不能与输入文件相同,请换一个文件名。");
            return false;
        }
    }
    return true;
}

static void runPage(int idx) {
    Page& P = g_pages[idx];
    if (P.running) {
        showWarning(L"提示", L"已有任务正在运行,请先停止或等待完成。");
        return;
    }
    wstring cmdStr = core::trim(getText(P.cmd));
    if (cmdStr.empty()) {
        showError(L"错误", L"命令为空。");
        return;
    }
    vector<wstring> args = core::split_cmd(cmdStr);
    if (args.empty()) {
        showError(L"错误", L"命令为空。");
        return;
    }
    if (!(fileExists(args[0]) || onPath(args[0]))) {
        if (!askYesNo(L"找不到 ffmpeg", L"未找到 ffmpeg 可执行文件:\n" + args[0] + L"\n\n仍然尝试运行吗?")) return;
    }
    if (!checkInputs(idx, cmdStr)) return;

    clearLog(P);
    logRaw(P, L"执行命令:\r\n" + cmdStr + L"\r\n\r\n");

    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        logRaw(P, L"\r\n[异常] 无法创建管道\r\n");
        return;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi = {};
    wstring line = core::win_cmdline(args);
    vector<wchar_t> mut(line.begin(), line.end());
    mut.push_back(L'\0');
    BOOL ok = CreateProcessW(nullptr, mut.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    DWORD err = ok ? 0 : GetLastError();
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(rd);
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
            logRaw(P, L"\r\n[错误] 找不到 ffmpeg 可执行文件,请检查路径设置。\r\n");
        else
            logRaw(P, L"\r\n[异常] " + lastErrorText(err) + L"\r\n");
        return;
    }
    CloseHandle(pi.hThread);
    P.proc = pi.hProcess;
    setRunning(P, true);

    ReaderCtx* ctx = new ReaderCtx{idx, rd, pi.hProcess};
    HANDLE th = CreateThread(nullptr, 0, readerThread, ctx, 0, nullptr);
    if (th)
        CloseHandle(th);
    else {
        delete ctx;
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(rd);
        CloseHandle(pi.hProcess);
        P.proc = nullptr;
        setRunning(P, false);
        logRaw(P, L"\r\n[异常] 无法创建读取线程\r\n");
    }
}

static void stopPage(int idx) {
    Page& P = g_pages[idx];
    if (P.running && P.proc) {
        if (TerminateProcess(P.proc, 1))
            logRaw(P, L"\r\n[已请求停止]\r\n");
        else
            logRaw(P, L"\r\n[停止失败] " + lastErrorText(GetLastError()) + L"\r\n");
    }
}

// ---------------------------------------------------------------------------
// ffprobe:读取输入文件的信息(后台线程)
// ---------------------------------------------------------------------------
struct ProbeMsg {
    int kind;  // 0 视频 1 音频
    wstring path;
    bool ok;
    string text;
};
struct ProbeCtx {
    int kind;
    wstring path, ffprobe;
};

// 运行命令并收集 stdout(stderr/stdin 丢弃)。超时则结束进程。退出码为 0 返回 true。
static bool runCapture(const vector<wstring>& args, DWORD timeoutMs, string& out) {
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = nul;
    PROCESS_INFORMATION pi = {};
    wstring line = core::win_cmdline(args);
    vector<wchar_t> mut(line.begin(), line.end());
    mut.push_back(L'\0');
    BOOL ok = CreateProcessW(nullptr, mut.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(rd);
        return false;
    }
    CloseHandle(pi.hThread);

    bool timedOut = false;
    DWORD start = GetTickCount();
    char buf[4096];
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr)) break;  // 写端已关闭
        if (avail) {
            DWORD got = 0;
            if (!ReadFile(rd, buf, std::min<DWORD>(avail, sizeof(buf)), &got, nullptr) || got == 0) break;
            out.append(buf, got);
            continue;
        }
        if (WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0) {
            // 进程已结束:把管道里剩下的数据读完
            DWORD left = 0;
            if (!PeekNamedPipe(rd, nullptr, 0, nullptr, &left, nullptr) || left == 0) break;
            continue;
        }
        if (GetTickCount() - start > timeoutMs) {
            TerminateProcess(pi.hProcess, 1);
            timedOut = true;
            break;
        }
    }
    DWORD code = 1;
    WaitForSingleObject(pi.hProcess, 2000);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    return !timedOut && code == 0;
}

static DWORD WINAPI probeThread(LPVOID param) {
    ProbeCtx* c = static_cast<ProbeCtx*>(param);
    ProbeMsg* m = new ProbeMsg{c->kind, c->path, false, string()};
    string entries = core::probe_show_entries();
    vector<wstring> args = {c->ffprobe, L"-v", L"error", L"-show_entries", wstring(entries.begin(), entries.end()), c->path};
    m->ok = runCapture(args, 30000, m->text);
    PostMessageW(g_main, WM_APP_PROBE, 0, reinterpret_cast<LPARAM>(m));
    delete c;
    return 0;
}

static void probeAsync(int kind) {
    int editId = kind == 0 ? T2_VIDEO : T2_AUDIO;
    int infoId = kind == 0 ? T2_VINFO : T2_AINFO;
    wstring path = core::trim(T(editId));
    if (path.empty() || !fileExists(path)) {
        setTextId(infoId, L"");
        return;
    }
    wstring ffprobe = findFfprobe(core::trim(T(IDC_FFMPEG_EDIT)));
    if (ffprobe.empty()) {
        setTextId(infoId, L"(未找到 ffprobe,无法显示媒体信息,不影响合成)");
        return;
    }
    setTextId(infoId, L"正在读取媒体信息…");
    ProbeCtx* c = new ProbeCtx{kind, path, ffprobe};
    HANDLE th = CreateThread(nullptr, 0, probeThread, c, 0, nullptr);
    if (th)
        CloseHandle(th);
    else
        delete c;
}

// ---------------------------------------------------------------------------
// 消息处理
// ---------------------------------------------------------------------------
static bool isClass(HWND h, const wchar_t* cls) {
    wchar_t name[64] = L"";
    GetClassNameW(h, name, 64);
    return lstrcmpiW(name, cls) == 0;
}

// 可编辑下拉框在 CBN_SELCHANGE 时文本框还是旧内容,这里把选中项写进去
static void syncComboText(HWND h) {
    if (GetWindowLongPtrW(h, GWL_STYLE) & CBS_DROPDOWNLIST) return;
    int sel = static_cast<int>(SendMessageW(h, CB_GETCURSEL, 0, 0));
    if (sel < 0) return;
    int n = static_cast<int>(SendMessageW(h, CB_GETLBTEXTLEN, sel, 0));
    if (n < 0) return;
    wstring s(static_cast<size_t>(n) + 1, L'\0');
    SendMessageW(h, CB_GETLBTEXT, sel, reinterpret_cast<LPARAM>(&s[0]));
    s.resize(static_cast<size_t>(n));
    setText(h, s);
}

static void chooseFfmpeg() {
    wstring p;
    if (pickFile(L"选择 ffmpeg 可执行文件", EXE_FILTERS, false, p)) {
        setTextId(IDC_FFMPEG_EDIT, p);
        scrollEnd(C(IDC_FFMPEG_EDIT));
        onOptionsChanged2();
        // 标签页 1 的预览按原版行为:不自动刷新
    }
}

static void onCommand(int id, int code, HWND ctl) {
    if (g_busy || !g_ready) return;

    // ---- 标签页 2:任何选项改动 -> 重新生成命令 ----
    if ((id >= T2_OPT_FIRST && id <= T2_OPT_LAST) || id == IDC_FFMPEG_EDIT) {
        bool relevant = false;
        if (isClass(ctl, L"Edit"))
            relevant = (code == EN_CHANGE);
        else if (isClass(ctl, L"ComboBox")) {
            if (code == CBN_SELCHANGE) {
                syncComboText(ctl);
                if (id == T2_RES) {
                    int sel = curSel(T2_RES);
                    const auto& presets = core::res_presets();
                    if (sel > 0 && sel < static_cast<int>(presets.size()) && presets[static_cast<size_t>(sel)].w > 0) {
                        setTextId(T2_WIDTH, std::to_wstring(presets[static_cast<size_t>(sel)].w));
                        setTextId(T2_HEIGHT, std::to_wstring(presets[static_cast<size_t>(sel)].h));
                    }
                }
                relevant = true;
            } else if (code == CBN_EDITCHANGE) {
                relevant = true;
            }
        } else if (isClass(ctl, L"Button")) {
            relevant = (code == BN_CLICKED);
        }
        if (relevant) onOptionsChanged2();
        // 手动粘贴 / 输入路径后,离开输入框时读取媒体信息
        if (code == EN_KILLFOCUS && id == T2_VIDEO) probeAsync(0);
        if (code == EN_KILLFOCUS && id == T2_AUDIO) probeAsync(1);
        return;
    }

    if (code != BN_CLICKED && id != IDOK) return;

    switch (id) {
    case IDC_FFMPEG_BROWSE:
        chooseFfmpeg();
        break;

    // ---- 标签页 1 ----
    case T1_AUDIO_BTN: {
        wstring p;
        if (pickFile(L"选择音频文件", AUDIO_FILTERS, false, p)) {
            setTextId(T1_AUDIO, p);
            scrollEnd(C(T1_AUDIO));
            autoFillOutput1();
            updatePreview1();
        }
        break;
    }
    case T1_IMAGE_BTN: {
        wstring p;
        if (pickFile(L"选择图片文件", IMAGE_FILTERS, false, p)) {
            setTextId(T1_IMAGE, p);
            scrollEnd(C(T1_IMAGE));
            autoFillOutput1();
            updatePreview1();
        }
        break;
    }
    case T1_OUT_BTN: {
        wstring p;
        if (pickFile(L"选择输出文件位置", VIDEO_OUT_FILTERS, true, p)) {
            setTextId(T1_OUT, p);
            scrollEnd(C(T1_OUT));
            updatePreview1();
        }
        break;
    }
    case T1_GEN: updatePreview1(); break;
    case T1_RUN: runPage(0); break;
    case T1_STOP: stopPage(0); break;
    case T1_CLEAR: clearLog(g_pages[0]); break;

    // ---- 标签页 2 ----
    case T2_VIDEO_BTN: {
        wstring p;
        if (pickFile(L"选择无声视频文件", VIDEO_IN_FILTERS, false, p)) {
            setTextId(T2_VIDEO, p);
            scrollEnd(C(T2_VIDEO));
            autoFillOutput2();
            onOptionsChanged2();
            probeAsync(0);
        }
        break;
    }
    case T2_AUDIO_BTN: {
        wstring p;
        if (pickFile(L"选择音频文件(mp3 / aac 等)", AUDIO_FILTERS, false, p)) {
            setTextId(T2_AUDIO, p);
            scrollEnd(C(T2_AUDIO));
            onOptionsChanged2();
            probeAsync(1);
        }
        break;
    }
    case T2_OUT_BTN: {
        wstring p;
        if (pickFile(L"选择输出文件位置", VIDEO_OUT_FILTERS, true, p)) {
            setTextId(T2_OUT, p);
            scrollEnd(C(T2_OUT));
            onOptionsChanged2();
        }
        break;
    }
    case T2_GEN: updatePreview2(); break;
    case T2_RUN: runPage(1); break;
    case T2_STOP: stopPage(1); break;
    case T2_CLEAR: clearLog(g_pages[1]); break;

    case IDOK: {  // 在输入框里按回车:读取媒体信息
        HWND f = GetFocus();
        if (f == C(T2_VIDEO)) probeAsync(0);
        if (f == C(T2_AUDIO)) probeAsync(1);
        break;
    }
    }
}

static void showPage(int sel) {
    for (int i = 0; i < 2; ++i)
        if (i != sel) ShowWindow(g_pages[i].hwnd, SW_HIDE);
    ShowWindow(g_pages[sel].hwnd, SW_SHOW);
    RedrawWindow(g_pages[sel].hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

static void createUi(HWND hwnd) {
    g_main = hwnd;

    g_ffGroup = CreateWindowExW(0, L"BUTTON", L"ffmpeg 可执行文件路径", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, hwnd, nullptr,
                                g_inst, nullptr);
    SendMessageW(g_ffGroup, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    HWND ffEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", findDefaultFfmpeg().c_str(),
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_FFMPEG_EDIT)), g_inst, nullptr);
    SendMessageW(ffEdit, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    g_ctl[IDC_FFMPEG_EDIT] = ffEdit;
    HWND ffBtn = CreateWindowExW(0, L"BUTTON", L"浏览...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_FFMPEG_BROWSE)), g_inst, nullptr);
    SendMessageW(ffBtn, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    g_ctl[IDC_FFMPEG_BROWSE] = ffBtn;

    g_tab = CreateWindowExW(WS_EX_CONTROLPARENT, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, 0, 0, hwnd,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TAB)), g_inst, nullptr);
    SendMessageW(g_tab, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    const wchar_t* names[2] = {L"图片 + 音频", L"视频 + 音频"};
    for (int i = 0; i < 2; ++i) {
        TCITEMW it = {};
        it.mask = TCIF_TEXT;
        it.pszText = const_cast<wchar_t*>(names[i]);
        SendMessageW(g_tab, TCM_INSERTITEMW, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(&it));
    }

    for (int i = 0; i < 2; ++i)
        g_pages[i].hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, L"PageWnd", L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 10, 10, g_tab,
                                          nullptr, g_inst, nullptr);

    buildPage1();
    buildPage2();
    showPage(0);
}

static LRESULT CALLBACK PageProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        onCommand(LOWORD(wp), HIWORD(wp), reinterpret_cast<HWND>(lp));
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wp);
        HWND c = reinterpret_cast<HWND>(lp);
        if (c == g_pages[0].log || c == g_pages[1].log) {
            SetTextColor(dc, RGB(0, 255, 0));
            SetBkColor(dc, RGB(17, 17, 17));
            return reinterpret_cast<LRESULT>(g_logBrush);
        }
        SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
        SetBkMode(dc, TRANSPARENT);
        if (c == C(T2_NOTE))
            SetTextColor(dc, RGB(180, 83, 9));
        else if (c == C(T2_VINFO) || c == C(T2_AINFO) || c == C(T2_HINT))
            SetTextColor(dc, RGB(102, 102, 102));
        else if (isClass(c, L"Edit"))  // 禁用状态的文本框
            SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
        else
            SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
    }
    case WM_CTLCOLORBTN:
        SetBkColor(reinterpret_cast<HDC>(wp), GetSysColor(COLOR_BTNFACE));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        createUi(hwnd);
        return 0;
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED && g_tab) layoutMain(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = reinterpret_cast<MINMAXINFO*>(lp);
        mm->ptMinTrackSize.x = g_minW;
        mm->ptMinTrackSize.y = g_minH;
        return 0;
    }
    case WM_COMMAND:
        onCommand(LOWORD(wp), HIWORD(wp), reinterpret_cast<HWND>(lp));
        return 0;
    case WM_NOTIFY: {
        NMHDR* nm = reinterpret_cast<NMHDR*>(lp);
        if (nm->idFrom == IDC_TAB && nm->code == TCN_SELCHANGE) showPage(TabCtrl_GetCurSel(g_tab));
        return 0;
    }
    case WM_APP_LOG: {
        LogMsg* m = reinterpret_cast<LogMsg*>(lp);
        appendLog(g_pages[m->page], m->text, m->term);
        delete m;
        return 0;
    }
    case WM_APP_DONE: {
        Page& P = g_pages[wp];
        if (P.proc) {
            CloseHandle(P.proc);
            P.proc = nullptr;
        }
        setRunning(P, false);
        DWORD code = static_cast<DWORD>(lp);
        if (code == 0)
            logRaw(P, L"\r\n[完成] ffmpeg 正常退出。\r\n");
        else
            logRaw(P, L"\r\n[结束] ffmpeg 退出码: " + std::to_wstring(static_cast<unsigned long>(code)) + L"\r\n");
        return 0;
    }
    case WM_APP_PROBE: {
        ProbeMsg* m = reinterpret_cast<ProbeMsg*>(lp);
        int editId = m->kind == 0 ? T2_VIDEO : T2_AUDIO;
        int infoId = m->kind == 0 ? T2_VINFO : T2_AINFO;
        if (core::trim(T(editId)) == m->path)  // 期间没换文件才更新
            setTextId(infoId, core::describe_media(m->ok, m->text, m->kind == 0));
        delete m;
        return 0;
    }
    case WM_CLOSE:
        for (auto& P : g_pages)
            if (P.running && P.proc) TerminateProcess(P.proc, 1);
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    g_inst = hInst;

    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_TAB_CLASSES | ICC_UPDOWN_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    HDC dc = GetDC(nullptr);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
    if (g_dpi < 96) g_dpi = 96;

    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
        g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    if (!g_font) g_font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    LOGFONTW mono = {};
    mono.lfHeight = -MulDiv(9, g_dpi, 72);
    mono.lfWeight = FW_NORMAL;
    mono.lfCharSet = DEFAULT_CHARSET;
    mono.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    lstrcpyW(mono.lfFaceName, L"Consolas");
    g_mono = CreateFontIndirectW(&mono);
    g_logBrush = CreateSolidBrush(RGB(17, 17, 17));

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpfnWndProc = PageProc;
    wc.lpszClassName = L"PageWnd";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = MainProc;
    wc.lpszClassName = L"MainWnd";
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    // 注意:主窗口和页面都不要加 WS_CLIPCHILDREN,否则父窗口擦背景时会跳过分组框区域,缩放后分组框内会留下残影
    const DWORD style = WS_OVERLAPPEDWINDOW;
    RECT want = {0, 0, S(WIN_W), S(WIN_H)};
    AdjustWindowRect(&want, style, FALSE);
    g_minW = want.right - want.left;
    g_minH = want.bottom - want.top;
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int w = std::min<int>(g_minW, wa.right - wa.left);
    int h = std::min<int>(g_minH, wa.bottom - wa.top);
    int x = wa.left + (wa.right - wa.left - w) / 2;
    int y = wa.top + (wa.bottom - wa.top - h) / 2;

    HWND hwnd = CreateWindowExW(0, L"MainWnd", L"图片 / 视频 + 音频 合成 MP4 (ffmpeg)", style, x, y, w, h, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    g_ready = true;
    syncStates2();
    updatePreview1();
    updatePreview2();

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return static_cast<int>(msg.wParam);
}
