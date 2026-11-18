#include "wubi_config.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdio>

namespace wubi {

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
    s.EnablePunction = GetIntSec(iniPath, p, L"EnablePunction", s.EnablePunction);
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
    if (s.CandCount < 1) s.CandCount = 1;
    if (s.CandCount > 9) s.CandCount = 9;
}

Settings LoadSettings(const std::wstring& iniPath, const std::wstring& appExe) {
    Settings s;
    s.InputMode      = GetInt(iniPath, L"InputMode", s.InputMode);
    s.CandVertical   = GetInt(iniPath, L"CandVertical", s.CandVertical);
    s.CandCount      = GetInt(iniPath, L"CandCount", s.CandCount);
    s.EnablePunction = GetInt(iniPath, L"EnablePunction", s.EnablePunction);
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
    PutInt(iniPath, L"EnablePunction", s.EnablePunction);
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
    s.EnablePunction = GetInt(qqIniPath, L"EnablePunction", s.EnablePunction);
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
    if (GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    // 多级创建
    std::wstring cur;
    for (size_t i = 0; i < dir.size(); ++i) {
        cur.push_back(dir[i]);
        if (dir[i] == L'\\' || dir[i] == L'/') continue;
        if (i + 1 == dir.size() || dir[i + 1] == L'\\' || dir[i + 1] == L'/') {
            CreateDirectoryW(cur.c_str(), nullptr);
        }
    }
    return GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES;
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
