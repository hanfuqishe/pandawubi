// wubi_config.h —— 配置读写，键名与 QQ五笔 Config.ini 保持一致，便于直接迁移
#pragma once

#include <string>

namespace wubi {

struct Settings {
    int  InputMode      = 0;   // 0=五笔 1=拼音（预留）
    int  CandVertical   = 0;   // 候选窗竖排
    int  CandCount      = 9;   // 候选个数上限
    int  EnablePunction = 1;   // 中文标点接管
    int  TradChinese    = 0;   // 简繁输出
    int  SortMode       = 0;   // 0=码短优先 1=纯词频优先（用户词始终优先）
    int  Level1Mode     = 2;   // 一级简码字在多键输入时: 0=不处理 1=隐藏 2=沉底
    int  UseGBK         = 1;   // 兼容 QQ 键名（暂未使用）
    int  CandFontPt     = 14;       // 候选框字号（pt）
    int  CandBgColor    = 0xF7F9FC; // 候选框背景色（0xRRGGBB）
    int  CandInset      = 1;        // 内框凹陷效果
    std::wstring DictFile = L"qqwubi86.dict.tsv";   // 词库文件名（data 目录下）
};

// 读：缺失的键保留默认值
// appExe 非空时，按 [app:*] 和 [app:<appExe>] 小节覆盖（appExe 为小写文件名，如 notepad.exe）
Settings LoadSettings(const std::wstring& iniPath, const std::wstring& appExe = std::wstring());
// 写：只写我们关心的键，保留文件其他内容
bool SaveSettings(const std::wstring& iniPath, const Settings& s);
// 从 QQ五笔 的 Config.ini 迁移（键名相同，容忍缺失）
Settings MigrateFromQQ(const std::wstring& qqIniPath, bool* found);

// ---------------------------------------------------------------- 数据目录
// 设置（config.ini）和自定义词/短语（user.tsv）都放在同一个数据目录里，
// 用户可以把目录指到网盘同步夹，换机器后指回来就能恢复，不需要自己的服务器。
// 查找优先级：环境变量 WUBI_DATA_DIR > HKCU\Software\PandaWubi\DataDir > %APPDATA%\PandaWubi
// 返回的目录一定已经创建好（失败则退回默认目录）。
std::wstring GetDataDir();
// 指定数据目录（空串 = 恢复默认）。只写注册表，不动文件。
bool         SetDataDir(const std::wstring& dir);
// 默认数据目录（%APPDATA%\PandaWubi）
std::wstring DefaultDataDir();
// config.ini / user.tsv 的完整路径（config.ini 允许用 WUBI_INI 直接覆盖，供测试与便携使用）
std::wstring GetConfigPath();
std::wstring GetUserWordsPath();
// 把 fromDir 里缺失的文件（config.ini、user.tsv）复制到 toDir，返回复制成功的个数
int          MigrateDataFiles(const std::wstring& fromDir, const std::wstring& toDir);

} // namespace wubi
