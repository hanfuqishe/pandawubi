// wubi_english.h —— 英文单词补全（数据来自 QQ五笔 engcore.dat 的 NEPQ 词表）
#pragma once
#include <string>
#include <vector>

namespace wubi {

// 把候选词的“还没输入的尾部”补上大小写：
//   · 已输入的位置逐字照抄实际输入的大小写（Shift/CapsLock 决定）；
//   · 其余位置跟随整体风格——连续两个及以上大写字母算"整词大写"（ABOUT），
//     单个大写字母只算"首字母大写"（About），其余小写。
// 例：ApplyCase(L"about", L"Ab") -> L"About"；ApplyCase(L"about", L"AB") -> L"ABOUT"。
// 词表本身是小写 ASCII，这里只做 ASCII 大小写，不依赖 locale。
std::wstring ApplyCase(const std::wstring& word, const std::wstring& typed);

class EnglishDict {
public:
    // 从纯文本载入（每行一个单词）。返回载入条数，失败返回 -1。
    // append=true 保留已有词表、把新词并进去：用户词表必须这么载入，
    // 否则第二次 Load 会把主词表清空（英补全曾因此只剩用户自己的那几十个词）。
    long long Load(const std::wstring& path, bool append = false);
    // 按前缀给出候选（不区分大小写，按字母序），limit 为上限
    std::vector<std::string> Suggest(const std::string& prefix, size_t limit) const;
    size_t Count() const { return words_.size(); }
    bool   Loaded() const { return !words_.empty(); }
    // 用户词表（自动学习）：已经存在的词返回 false，没加进去（重复/非法）也返回 false
    bool   Has(const std::string& word) const;
    bool   Add(const std::string& word);

private:
    std::vector<std::string> words_;   // 已按不区分大小写的字典序排好
};

} // namespace wubi
