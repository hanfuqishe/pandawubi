#include "wubi_english.h"
#include <cstdio>
#include <algorithm>
#include <cctype>
#include <unordered_set>

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

// 常用词表一次性灌进哈希表：原来每次判定都要把 word 小写一遍再和 200 个词逐个比，
// 排序比较器里一次就要做几百万次字符串分配（Shift+字母第一下要卡 400 多毫秒）。
static const std::unordered_set<std::string>& CommonSet() {
    static const std::unordered_set<std::string> s = [] {
        std::unordered_set<std::string> t;
        t.reserve(sizeof(kCommonWords) / sizeof(kCommonWords[0]) * 2);
        for (const char* w : kCommonWords) t.insert(w);
        return t;
    }();
    return s;
}

static bool IsCommon(const std::string& w) {   // 传入的必须是已小写的 key
    return CommonSet().count(w) != 0;
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

    // 按小写 key 排序并去重：词表里同一个词常有 An/an、Word/word 两种写法，
    // 补全时套上大小写后会变成两条一模一样的候选。同 key 保留全小写那条。
    {
        struct Row { std::string key, word; };
        std::vector<Row> rows;
        rows.reserve(words_.size());
        for (auto& w : words_) rows.push_back({ LowerAscii(w), std::move(w) });
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            if (a.key != b.key) return a.key < b.key;
            const bool la = (a.word == a.key), lb = (b.word == b.key);
            if (la != lb) return la;          // 全小写优先
            return a.word < b.word;
        });
        words_.clear();
        words_.reserve(rows.size());
        for (size_t i = 0; i < rows.size(); ++i) {
            if (i > 0 && rows[i].key == rows[i - 1].key) continue;
            words_.push_back(std::move(rows[i].word));
        }
    }
    return (long long)words_.size();
}

// 词是否已在表里（不区分大小写）
bool EnglishDict::Has(const std::string& word) const {
    if (word.empty()) return false;
    const std::string key = LowerAscii(word);
    auto it = std::lower_bound(words_.begin(), words_.end(), key,
        [](const std::string& w, const std::string& k) { return LowerAscii(w) < k; });
    return it != words_.end() && LowerAscii(*it) == key;
}

// 加入词表（保持按小写 key 升序，Suggest 的二分才成立）。只在词表里没有时插入。
bool EnglishDict::Add(const std::string& word) {
    if (word.size() < 2 || word.size() > 40) return false;
    for (char c : word) {
        if (!std::isalpha((unsigned char)c) && c != '\'' && c != '-') return false;
    }
    const std::string key = LowerAscii(word);
    auto it = std::lower_bound(words_.begin(), words_.end(), key,
        [](const std::string& w, const std::string& k) { return LowerAscii(w) < k; });
    if (it != words_.end() && LowerAscii(*it) == key) return false;   // 已有
    words_.insert(it, word);
    return true;
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
    // 先收集匹配项（每个词的小写 key / 长度 / 是否常用词都只算一次），
    // 排序后再截断，否则"常用词优先"会因为过早截断而失效。
    struct Hit {
        const std::string* word;
        std::string key;      // 小写 key（同一前缀下比较用）
        size_t len;
        bool   common;
        bool   allLower;      // 原词就是全小写（词表里既有 word 又有 Word 时优先小的）
    };
    std::vector<Hit> hits;
    hits.reserve(256);
    for (size_t i = lo; i < words_.size() && hits.size() < 4000; ++i) {
        std::string key = LowerAscii(words_[i]);
        if (key.size() < p.size() || key.compare(0, p.size(), p) != 0) break;
        const bool lower = (key == words_[i]);
        hits.push_back({ &words_[i], std::move(key), 0, false, lower });
        hits.back().len    = hits.back().key.size();
        hits.back().common = IsCommon(hits.back().key);   // key 已小写，直接查表
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
        if (a.common != b.common) return a.common;
        if (a.len != b.len)       return a.len < b.len;
        const int c = a.key.compare(b.key);
        if (c != 0) return c < 0;
        return a.allLower && !b.allLower;
    });
    out.reserve(hits.size() < limit ? hits.size() : limit);
    for (size_t i = 0; i < hits.size() && out.size() < limit; ++i) out.push_back(*hits[i].word);
    return out;
}

} // namespace wubi
