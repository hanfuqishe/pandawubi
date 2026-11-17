#include "wubi_english.h"
#include <cstdio>
#include <algorithm>
#include <cctype>

namespace wubi {

static wchar_t UpperA(wchar_t c) { return (c >= L'a' && c <= L'z') ? (wchar_t)(c - L'a' + L'A') : c; }
static wchar_t LowerA(wchar_t c) { return (c >= L'A' && c <= L'Z') ? (wchar_t)(c - L'A' + L'a') : c; }
static bool    IsAlphaA(wchar_t c) { return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z'); }

std::wstring ApplyCase(const std::wstring& word, const std::wstring& typed) {
    size_t letters = 0;
    bool allUpper = true;
    for (wchar_t c : typed) {
        if (!IsAlphaA(c)) continue;
        ++letters;
        if (c != UpperA(c)) allUpper = false;   // 出现了小写
    }
    // 单个大写字母是"首字母大写"（About），连续两个及以上大写才是"整词大写"（ABOUT）——
    // 与常见输入法的英文模式一致，否则 Shift+A 一个字母就会把整词顶成大写。
    const bool shout = (letters >= 2 && allUpper);

    std::wstring out;
    out.reserve(word.size());
    for (size_t i = 0; i < word.size(); ++i) {
        bool upper;
        if (i < typed.size()) {
            const wchar_t t = typed[i];
            upper = IsAlphaA(t) && (t == UpperA(t));   // 照抄这一位实际输入的大小写
        } else {
            upper = shout;                             // 尾部跟着整体风格
        }
        out.push_back(upper ? UpperA(word[i]) : LowerA(word[i]));
    }
    return out;
}

static std::string LowerAscii(const std::string& s) {
    std::string r(s);
    for (auto& c : r) c = (char)std::tolower((unsigned char)c);
    return r;
}

// 常用词优先（让 hel -> hello、wor -> word、compu -> computer 排在前面）
static const char* kCommonWords[] = {
    "the","be","to","of","and","a","in","that","have","i","it","for","not","on","with","he","as","you",
    "do","at","this","but","his","by","from","they","we","say","her","she","or","an","will","my","one",
    "all","would","there","their","what","so","up","out","if","about","who","get","which","go","me",
    "when","make","can","like","time","no","just","him","know","take","people","into","year","your",
    "good","some","could","them","see","other","than","then","now","look","only","come","its","over",
    "think","also","back","after","use","two","how","our","work","first","well","way","even","new",
    "want","because","any","these","give","day","most","us","is","are","was","were","been","has","had",
    "did","does","said","made","went","got","very","much","more","many","here","where","why","while",
    "before","between","under","still","own","same","right","long","great","little","world","life",
    "hand","part","place","case","week","company","system","program","question","government","number",
    "night","point","home","water","room","mother","area","money","story","fact","month","lot","study",
    "book","eye","job","word","business","issue","side","kind","head","house","service","friend",
    "father","power","hour","game","line","end","member","law","car","city","community","name","team",
    "minute","idea","kid","body","information","nothing","ago","lead","social","group","party","school",
    "office","student","paper","food","war","history","health","music","science","computer","hello",
    "help","hold","hope","idea","keep","last","late","learn","leave","let","live","love","need","next",
    "open","order","pay","play","put","read","real","run","say","school","seem","show","small","start",
    "state","stop","talk","tell","thing","try","turn","use","wait","walk","want","watch","water","way",
    "week","well","west","white","whole","woman","word","world","write","young","zero"
};

static bool IsCommon(const std::string& w) {
    for (const char* c : kCommonWords) {
        if (LowerAscii(w) == c) return true;
    }
    return false;
}
long long EnglishDict::Load(const std::wstring& path) {
    words_.clear();
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, path.c_str(), L"rb") != 0 || !fp) return -1;

    std::string line;
    char buf[512];
    while (fgets(buf, sizeof(buf), fp)) {
        std::string s(buf);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (s.size() < 2 || s.size() > 40) continue;
        bool ok = true;
        for (char c : s) {
            if (!std::isalpha((unsigned char)c) && c != '\'' && c != '-') { ok = false; break; }
        }
        if (ok) words_.push_back(std::move(s));
    }
    fclose(fp);

    std::sort(words_.begin(), words_.end(), [](const std::string& a, const std::string& b) {
        return LowerAscii(a) < LowerAscii(b);
    });
    words_.erase(std::unique(words_.begin(), words_.end()), words_.end());
    return (long long)words_.size();
}

std::vector<std::string> EnglishDict::Suggest(const std::string& prefix, size_t limit) const {
    std::vector<std::string> out;
    if (prefix.empty() || words_.empty() || limit == 0) return out;

    const std::string p = LowerAscii(prefix);
    // 二分找第一个 >= prefix 的位置
    size_t lo = 0, hi = words_.size();
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (LowerAscii(words_[mid]) < p) lo = mid + 1;
        else hi = mid;
    }
    // 先收集全部匹配项，排序后再截断（否则"常用词优先"会因过早截断而失效）
    for (size_t i = lo; i < words_.size() && out.size() < 4000; ++i) {
        const std::string key = LowerAscii(words_[i]);
        if (key.size() < p.size() || key.compare(0, p.size(), p) != 0) break;
        out.push_back(words_[i]);
    }
    std::stable_sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        const bool ca = IsCommon(a), cb = IsCommon(b);
        if (ca != cb) return ca;
        if (a.size() != b.size()) return a.size() < b.size();
        return LowerAscii(a) < LowerAscii(b);
    });
    if (out.size() > limit) out.resize(limit);
    return out;
}

} // namespace wubi
