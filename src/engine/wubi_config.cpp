#include "wubi_config.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdio>
#include <vector>

namespace wubi {

// ---------------------------------------------------------------- 热键解析
// 键名表：写起来要"人话"，所以英文全名 + 符号别名都收；不区分大小写。
struct KeyName { const wchar_t* name; unsigned vk; };
static const KeyName kKeyNames[] = {
    { L"A", 'A' }, { L"B", 'B' }, { L"C", 'C' }, { L"D", 'D' }, { L"E", 'E' },
    { L"F", 'F' }, { L"G", 'G' }, { L"H", 'H' }, { L"I", 'I' }, { L"J", 'J' },
    { L"K", 'K' }, { L"L", 'L' }, { L"M", 'M' }, { L"N", 'N' }, { L"O", 'O' },
    { L"P", 'P' }, { L"Q", 'Q' }, { L"R", 'R' }, { L"S", 'S' }, { L"T", 'T' },
    { L"U", 'U' }, { L"V", 'V' }, { L"W", 'W' }, { L"X", 'X' }, { L"Y", 'Y' },
    { L"Z", 'Z' },
    { L"0", '0' }, { L"1", '1' }, { L"2", '2' }, { L"3", '3' }, { L"4", '4' },
    { L"5", '5' }, { L"6", '6' }, { L"7", '7' }, { L"8", '8' }, { L"9", '9' },
    { L"F1", VK_F1 },   { L"F2", VK_F2 },   { L"F3", VK_F3 },   { L"F4", VK_F4 },
    { L"F5", VK_F5 },   { L"F6", VK_F6 },   { L"F7", VK_F7 },   { L"F8", VK_F8 },
    { L"F9", VK_F9 },   { L"F10", VK_F10 }, { L"F11", VK_F11 }, { L"F12", VK_F12 },
    { L"F13", VK_F13 }, { L"F14", VK_F14 }, { L"F15", VK_F15 }, { L"F16", VK_F16 },
    { L"F17", VK_F17 }, { L"F18", VK_F18 }, { L"F19", VK_F19 }, { L"F20", VK_F20 },
    { L"F21", VK_F21 }, { L"F22", VK_F22 }, { L"F23", VK_F23 }, { L"F24", VK_F24 },
    { L"SPACE", VK_SPACE }, { L"ENTER", VK_RETURN }, { L"RETURN", VK_RETURN },
    { L"TAB", VK_TAB }, { L"ESC", VK_ESCAPE }, { L"ESCAPE", VK_ESCAPE },
    { L"BACKSPACE", VK_BACK }, { L"BKSP", VK_BACK },
    { L"DELETE", VK_DELETE }, { L"DEL", VK_DELETE }, { L"INSERT", VK_INSERT }, { L"INS", VK_INSERT },
    { L"HOME", VK_HOME }, { L"END", VK_END },
    { L"PGUP", VK_PRIOR }, { L"PAGEUP", VK_PRIOR },
    { L"PGDN", VK_NEXT }, { L"PAGEDOWN", VK_NEXT },
    { L"UP", VK_UP }, { L"DOWN", VK_DOWN }, { L"LEFT", VK_LEFT }, { L"RIGHT", VK_RIGHT },
    { L"COMMA", VK_OEM_COMMA }, { L"PERIOD", VK_OEM_PERIOD }, { L"SEMICOLON", VK_OEM_1 },
    { L"SLASH", VK_OEM_2 }, { L"BACKQUOTE", VK_OEM_3 }, { L"BRACKETLEFT", VK_OEM_4 },
    { L"BACKSLASH", VK_OEM_5 }, { L"BRACKETRIGHT", VK_OEM_6 }, { L"QUOTE", VK_OEM_7 },
    { L"MINUS", VK_OEM_MINUS }, { L"EQUAL", VK_OEM_PLUS },
    { L",", VK_OEM_COMMA }, { L".", VK_OEM_PERIOD }, { L";", VK_OEM_1 }, { L"/", VK_OEM_2 },
    { L"`", VK_OEM_3 }, { L"[", VK_OEM_4 }, { L"\\", VK_OEM_5 }, { L"]", VK_OEM_6 },
    { L"'", VK_OEM_7 }, { L"-", VK_OEM_MINUS }, { L"=", VK_OEM_PLUS },
};

static std::wstring Upper(std::wstring s) {
    for (auto& c : s) if (c >= L'a' && c <= L'z') c = (wchar_t)(c - L'a' + L'A');
    return s;
}
static std::wstring Trim(const std::wstring& s) {
    size_t b = s.find_first_not_of(L" \t\r\n");
    if (b == std::wstring::npos) return std::wstring();
    size_t e = s.find_last_not_of(L" \t\r\n");
    return s.substr(b, e - b + 1);
}
static bool LookupKey(const std::wstring& name, unsigned& vk) {
    const std::wstring up = Upper(name);
    for (const auto& k : kKeyNames) {
        if (up == k.name) { vk = k.vk; return true; }
    }
    return false;
}
static const wchar_t* KeyNameOf(unsigned vk) {
    for (const auto& k : kKeyNames) {
        // 反查取第一个"像名字"的写法（字母/数字开头），跳过符号别名，保证 Format 出来是 Ctrl+J 这种
        const wchar_t c0 = k.name[0];
        if (k.vk == vk && ((c0 >= L'A' && c0 <= L'Z') || (c0 >= L'0' && c0 <= L'9'))) return k.name;
    }
    return nullptr;
}

bool ParseHotkey(const std::wstring& text, Hotkey& out) {
    out = Hotkey();
    std::wstring s = Trim(text);
    if (s.empty()) return false;
    // 按 '+' 切开；键名本身可以是 '+' 吗？不支持（没有这种常用键），所以切出来空段即视为非法
    std::vector<std::wstring> parts;
    size_t start = 0;
    for (;;) {
        const size_t pos = s.find(L'+', start);
        if (pos == std::wstring::npos) { parts.push_back(s.substr(start)); break; }
        parts.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    if (parts.size() < 2) return false;         // 至少要"修饰键 + 键"
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        const std::wstring t = Upper(Trim(parts[i]));
        if (t == L"CTRL" || t == L"CONTROL") out.ctrl = true;
        else if (t == L"ALT")                out.alt = true;
        else if (t == L"SHIFT")              out.shift = true;
        else if (t == L"WIN" || t == L"WINDOWS") out.win = true;
        else return false;                       // 中间出现非修饰键：写法不对
    }
    unsigned vk = 0;
    if (!LookupKey(Trim(parts.back()), vk)) return false;
    if (!out.ctrl && !out.alt && !out.win) return false;   // 只有 Shift：会跟正常打字抢键
    out.vk = vk;
    return true;
}

std::wstring FormatHotkey(const Hotkey& hk) {
    if (!hk.vk) return std::wstring();
    const wchar_t* key = KeyNameOf(hk.vk);
    if (!key) return std::wstring();
    std::wstring s;
    if (hk.ctrl)  s += L"Ctrl+";
    if (hk.alt)   s += L"Alt+";
    if (hk.shift) s += L"Shift+";
    if (hk.win)   s += L"Win+";
    s += key;
    return s;
}

static int GetInt(const std::wstring& path, const wchar_t* key, int def) {
    return (int)GetPrivateProfileIntW(L"Config", key, def, path.c_str());
}

static int GetIntSec(const std::wstring& path, const wchar_t* sec, const wchar_t* key, int def) {
    return (int)GetPrivateProfileIntW(sec, key, def, path.c_str());
}

// 读颜色：#RRGGBB / 0xRRGGBB / 十进制；没写则用默认值
static int GetColor(const std::wstring& path, const wchar_t* key, int def) {
    wchar_t buf[32] = { 0 };
    GetPrivateProfileStringW(L"Config", key, L"", buf, ARRAYSIZE(buf), path.c_str());
    if (buf[0] == 0) return def;
    const wchar_t* p = buf;
    if (*p == L'#') { ++p; return (int)wcstoul(p, nullptr, 16); }
    if (p[0] == L'0' && (p[1] == L'x' || p[1] == L'X')) return (int)wcstoul(p + 2, nullptr, 16);
    return (int)wcstoul(p, nullptr, 10);
}

// 用某个 app 小节覆盖已加载的设置（小节里没有的键保持原值）
static void ApplyAppSection(const std::wstring& iniPath, const std::wstring& sec, Settings& s) {
    if (sec.empty()) return;
    const wchar_t* p = sec.c_str();
    s.InputMode      = GetIntSec(iniPath, p, L"InputMode",      s.InputMode);
    s.CandVertical   = GetIntSec(iniPath, p, L"CandVertical",   s.CandVertical);
    s.CandCount      = GetIntSec(iniPath, p, L"CandCount",      s.CandCount);
    s.TradChinese    = GetIntSec(iniPath, p, L"TradChinese",    s.TradChinese);
    s.UseGBK         = GetIntSec(iniPath, p, L"UseGBK",         s.UseGBK);
    s.SortMode       = GetIntSec(iniPath, p, L"SortMode",       s.SortMode);
    s.Level1Mode     = GetIntSec(iniPath, p, L"Level1Mode",     s.Level1Mode);
    s.CandFontPt     = GetIntSec(iniPath, p, L"CandFont",       s.CandFontPt);
    s.CandInset      = GetIntSec(iniPath, p, L"CandInset",      s.CandInset);
    {
        wchar_t db[128] = { 0 };
        GetPrivateProfileStringW(p, L"DictFile", s.DictFile.c_str(), db, ARRAYSIZE(db), iniPath.c_str());
        if (db[0]) s.DictFile = db;
    }
    {
        // 单个应用也能覆盖热键（比如某个宿主自己的 Ctrl+J 有用，可以在 [app:x.exe] 里换一个）
        wchar_t hk[64] = { 0 };
        GetPrivateProfileStringW(p, L"HotkeyTrad", s.HotkeyTrad.c_str(), hk, ARRAYSIZE(hk), iniPath.c_str());
        Hotkey parsed;
        if (hk[0] && ParseHotkey(hk, parsed)) s.HotkeyTrad = Trim(hk);
    }
    if (s.CandCount < 1) s.CandCount = 1;
    if (s.CandCount > 9) s.CandCount = 9;
}

Settings LoadSettings(const std::wstring& iniPath, const std::wstring& appExe) {
    Settings s;
    s.InputMode      = GetInt(iniPath, L"InputMode", s.InputMode);
    s.CandVertical   = GetInt(iniPath, L"CandVertical", s.CandVertical);
    s.CandCount      = GetInt(iniPath, L"CandCount", s.CandCount);
    s.TradChinese    = GetInt(iniPath, L"TradChinese", s.TradChinese);
    s.UseGBK         = GetInt(iniPath, L"UseGBK", s.UseGBK);
    s.SortMode       = GetInt(iniPath, L"SortMode", s.SortMode);
    s.Level1Mode     = GetInt(iniPath, L"Level1Mode", s.Level1Mode);
    s.CandFontPt     = GetInt(iniPath, L"CandFont", s.CandFontPt);
    s.CandBgColor    = GetColor(iniPath, L"CandBgColor", s.CandBgColor);
    s.CandInset      = GetInt(iniPath, L"CandInset", s.CandInset);
    s.CandTextColor    = GetColor(iniPath, L"CandTextColor", s.CandTextColor);
    s.CandBorderColor  = GetColor(iniPath, L"CandBorderColor", s.CandBorderColor);
    {
        wchar_t fb[128] = { 0 };
        GetPrivateProfileStringW(L"Config", L"CandFontFace", s.CandFontFace.c_str(), fb, ARRAYSIZE(fb), iniPath.c_str());
        if (fb[0]) s.CandFontFace = fb;
    }
    {
        wchar_t th[32] = { 0 };
        GetPrivateProfileStringW(L"Config", L"Theme", s.Theme.c_str(), th, ARRAYSIZE(th), iniPath.c_str());
        if (th[0]) s.Theme = th;
        if (s.Theme != L"cat" && s.Theme != L"dog" && s.Theme != L"crane") s.Theme = L"panda";   // 未知值一律回默认熊猫
    }
    {
        wchar_t db[128] = { 0 };
        GetPrivateProfileStringW(L"Config", L"DictFile", s.DictFile.c_str(), db, ARRAYSIZE(db), iniPath.c_str());
        if (db[0]) s.DictFile = db;
    }
    {
        // 简/繁热键：写成字符串（默认 Ctrl+J）。解析不出来（手改坏了）就退回默认值，
        // 不要让一个坏值把"简繁切换"整个废掉。
        wchar_t hk[64] = { 0 };
        GetPrivateProfileStringW(L"Config", L"HotkeyTrad", s.HotkeyTrad.c_str(), hk, ARRAYSIZE(hk), iniPath.c_str());
        Hotkey parsed;
        if (hk[0] && ParseHotkey(hk, parsed)) s.HotkeyTrad = Trim(hk);
        else                                  s.HotkeyTrad = L"Ctrl+J";
    }
    if (s.CandCount < 1) s.CandCount = 1;
    if (s.CandCount > 9) s.CandCount = 9;

    // 按应用覆盖：[app:*] 先套，再套 [app:<exe>]（exe 为小写文件名）
    if (!appExe.empty()) {
        ApplyAppSection(iniPath, L"app:*", s);
        ApplyAppSection(iniPath, L"app:" + appExe, s);
    }
    return s;
}

static void PutInt(const std::wstring& path, const wchar_t* key, int v) {
    wchar_t buf[32];
    swprintf_s(buf, L"%d", v);
    WritePrivateProfileStringW(L"Config", key, buf, path.c_str());
}

bool SaveSettings(const std::wstring& iniPath, const Settings& s) {
    PutInt(iniPath, L"InputMode", s.InputMode);
    PutInt(iniPath, L"CandVertical", s.CandVertical);
    PutInt(iniPath, L"CandCount", s.CandCount);
    PutInt(iniPath, L"TradChinese", s.TradChinese);
    PutInt(iniPath, L"UseGBK", s.UseGBK);
    PutInt(iniPath, L"SortMode", s.SortMode);
    PutInt(iniPath, L"Level1Mode", s.Level1Mode);
    PutInt(iniPath, L"CandFont", s.CandFontPt);
    PutInt(iniPath, L"CandInset", s.CandInset);
    {
        auto putColor = [&](const wchar_t* key, int v) {
            wchar_t cb[32];
            swprintf_s(cb, L"#%06X", (unsigned)(v & 0xFFFFFF));
            WritePrivateProfileStringW(L"Config", key, cb, iniPath.c_str());
        };
        putColor(L"CandTextColor", s.CandTextColor);
        putColor(L"CandBorderColor", s.CandBorderColor);
    }
    WritePrivateProfileStringW(L"Config", L"CandFontFace", s.CandFontFace.c_str(), iniPath.c_str());
    const wchar_t* theme = (s.Theme == L"cat") ? L"cat"
                         : (s.Theme == L"dog") ? L"dog"
                         : (s.Theme == L"crane") ? L"crane" : L"panda";
    WritePrivateProfileStringW(L"Config", L"Theme", theme, iniPath.c_str());
    WritePrivateProfileStringW(L"Config", L"DictFile", s.DictFile.c_str(), iniPath.c_str());
    {
        // 热键按"解析得出来"的规范写法落盘；坏值不写，免得下次启动又被退回默认还看不出原因
        Hotkey hk;
        const std::wstring text = ParseHotkey(s.HotkeyTrad, hk) ? FormatHotkey(hk) : L"Ctrl+J";
        WritePrivateProfileStringW(L"Config", L"HotkeyTrad", text.c_str(), iniPath.c_str());
    }
    {
        wchar_t cb[32];
        swprintf_s(cb, L"#%06X", (unsigned)(s.CandBgColor & 0xFFFFFF));
        WritePrivateProfileStringW(L"Config", L"CandBgColor", cb, iniPath.c_str());
    }
    return GetFileAttributesW(iniPath.c_str()) != INVALID_FILE_ATTRIBUTES;
}

Settings MigrateFromQQ(const std::wstring& qqIniPath, bool* found) {
    Settings s;
    const DWORD attr = GetFileAttributesW(qqIniPath.c_str());
    if (found) *found = (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
    if (!*found) return s;

    // QQ五笔 的键名与我们一致，直接沿用；缺失项保留默认
    s.InputMode      = GetInt(qqIniPath, L"InputMode", s.InputMode);
    s.CandVertical   = GetInt(qqIniPath, L"CandVertical", s.CandVertical);
    s.UseGBK         = GetInt(qqIniPath, L"UseGBK", s.UseGBK);
    // QQ 的 ChangeIMEOutput=1 大致对应“简繁输出”类行为，仅作保守映射
    const int changeOut = GetInt(qqIniPath, L"ChangeIMEOutput", 0);
    if (changeOut == 0) s.TradChinese = 0;
    return s;
}

// 编码输入框的过滤规则：只保留英文字母，最多 maxLen 位
std::wstring FilterCode(const std::wstring& in, size_t maxLen) {
    std::wstring out;
    for (wchar_t c : in) {
        if (out.size() >= maxLen) break;
        if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z')) out.push_back(c);
    }
    return out;
}

// ---------------------------------------------------------------- 数据目录
static const wchar_t* kRegKey = L"Software\\PandaWubi";
static const wchar_t* kRegVal = L"DataDir";

static bool EnsureDir(const std::wstring& dir) {
    if (dir.empty()) return false;
    // 只判"路径存在"会把同名普通文件当成目录：之后所有配置文件/用户词写入都会失败，
    // 而调用方不看返回值，用户看到的是"设置改了不生效、用户词不保存"。
    {
        const DWORD a = GetFileAttributesW(dir.c_str());
        if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    // 多级创建
    std::wstring cur;
    for (size_t i = 0; i < dir.size(); ++i) {
        cur.push_back(dir[i]);
        if (dir[i] == L'\\' || dir[i] == L'/') continue;
        if (i + 1 == dir.size() || dir[i + 1] == L'\\' || dir[i + 1] == L'/') {
            CreateDirectoryW(cur.c_str(), nullptr);
        }
    }
    {
        const DWORD a = GetFileAttributesW(dir.c_str());
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
}

std::wstring DefaultDataDir() {
    wchar_t buf[MAX_PATH] = { 0 };
    const DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, ARRAYSIZE(buf));
    std::wstring base = (n > 0 && n < ARRAYSIZE(buf)) ? std::wstring(buf) : std::wstring(L".");
    return base + L"\\PandaWubi";
}

static std::wstring RegDataDir() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return std::wstring();
    wchar_t buf[MAX_PATH * 2] = { 0 };
    DWORD cb = sizeof(buf) - sizeof(wchar_t);
    DWORD type = 0;
    std::wstring out;
    if (RegQueryValueExW(k, kRegVal, nullptr, &type, (LPBYTE)buf, &cb) == ERROR_SUCCESS && type == REG_SZ) {
        buf[cb / sizeof(wchar_t)] = 0;
        out = buf;
    }
    RegCloseKey(k);
    return out;
}

std::wstring GetDataDir() {
    // 1) 环境变量（测试/便携）
    wchar_t env[MAX_PATH * 2] = { 0 };
    const DWORD en = GetEnvironmentVariableW(L"WUBI_DATA_DIR", env, ARRAYSIZE(env));
    if (en > 0 && en < ARRAYSIZE(env) && env[0]) {
        const std::wstring dir = env;
        if (EnsureDir(dir)) return dir;
    }
    // 2) 用户指定
    std::wstring reg = RegDataDir();
    if (!reg.empty()) {
        if (EnsureDir(reg)) return reg;      // 目录不可写时退回默认，不要因此丢配置
    }
    // 3) 默认
    const std::wstring def = DefaultDataDir();
    EnsureDir(def);

    // 一次性迁移：老版本用的是 %APPDATA%\WubiTsf
    const std::wstring newIni = def + L"\\config.ini";
    if (GetFileAttributesW(newIni.c_str()) == INVALID_FILE_ATTRIBUTES) {
        wchar_t base[MAX_PATH] = { 0 };
        const DWORD n = GetEnvironmentVariableW(L"APPDATA", base, ARRAYSIZE(base));
        if (n > 0 && n < ARRAYSIZE(base)) {
            const std::wstring oldDir = std::wstring(base) + L"\\WubiTsf";
            if (GetFileAttributesW((oldDir + L"\\config.ini").c_str()) != INVALID_FILE_ATTRIBUTES) {
                MigrateDataFiles(oldDir, def);
            }
        }
    }
    return def;
}

bool SetDataDir(const std::wstring& dir) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    bool ok = false;
    if (dir.empty()) {
        RegDeleteValueW(k, kRegVal);
        ok = true;                                  // 恢复默认：值不存在即成功
    } else {
        ok = RegSetValueExW(k, kRegVal, 0, REG_SZ, (const BYTE*)dir.c_str(),
                            (DWORD)((dir.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    }
    RegCloseKey(k);
    return ok;
}

std::wstring GetConfigPath() {
    // 显式覆盖（测试、便携部署）
    wchar_t ini[MAX_PATH * 2] = { 0 };
    const DWORD n = GetEnvironmentVariableW(L"WUBI_INI", ini, ARRAYSIZE(ini));
    if (n > 0 && n < ARRAYSIZE(ini) && ini[0]) return ini;
    return GetDataDir() + L"\\config.ini";
}

std::wstring GetUserWordsPath() {
    return GetDataDir() + L"\\user.tsv";
}

int MigrateDataFiles(const std::wstring& fromDir, const std::wstring& toDir) {
    if (fromDir.empty() || toDir.empty()) return 0;
    if (_wcsicmp(fromDir.c_str(), toDir.c_str()) == 0) return 0;
    if (!EnsureDir(toDir)) return 0;

    int copied = 0;
    const wchar_t* names[] = { L"config.ini", L"user.tsv" };
    for (const wchar_t* name : names) {
        const std::wstring src = fromDir + L"\\" + name;
        const std::wstring dst = toDir + L"\\" + name;
        if (GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        if (GetFileAttributesW(dst.c_str()) != INVALID_FILE_ATTRIBUTES) continue;   // 目标已有，不覆盖
        if (CopyFileW(src.c_str(), dst.c_str(), TRUE)) ++copied;
    }
    return copied;
}

} // namespace wubi
