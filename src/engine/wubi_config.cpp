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

} // namespace wubi
