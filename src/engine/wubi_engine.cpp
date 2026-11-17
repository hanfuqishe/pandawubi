#include "wubi_engine.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <queue>
#include <unordered_set>
#include <iterator>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <map>

namespace wubi {

// 编码字符：a..z。z 不是五笔码元，但自定义词组/短语常用拼音缩写（dz=地址、sfz=身份证），
// 所以编码允许 z；系统词库里的码仍然只会是 a..y。
bool IsCodeChar(char c) { return c >= 'a' && c <= 'z'; }

size_t Utf8CharLen(const std::string& s, size_t i) {
    if (i >= s.size()) return 0;
    unsigned char c = static_cast<unsigned char>(s[i]);
    if ((c & 0x80) == 0) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

// ---------------------------------------------------------------- 词组取码
void Dictionary::BuildCharCodes() const {
    charCodes_.clear();
    charCodes_.reserve(32768);
    // 只收单字（一个 UTF-8 字符），并且保留码最长的那个（= 全码，而非简码）
    auto consider = [this](const std::string& word, const std::string& code) {
        if (word.empty() || code.empty()) return;
        if (Utf8CharLen(word, 0) != word.size()) return;   // 多字词不要
        auto it = charCodes_.find(word);
        if (it == charCodes_.end() || it->second.size() < code.size()) charCodes_[word] = code;
    };
    if (bin_) {
        for (size_t i = 0; i < binCount_; ++i) {
            const BinaryRec& r = bin_[i];
            consider(std::string(binBlob_ + r.wordOff, r.wordLen), std::string(r.code, r.codeLen));
        }
    } else {
        for (const Entry& e : entries_) consider(e.word, e.code);
    }
    charCodesBuilt_ = true;
}

std::string Dictionary::FullCodeOfChar(const std::string& utf8Char) const {
    if (!charCodesBuilt_) BuildCharCodes();
    auto it = charCodes_.find(utf8Char);
    return it == charCodes_.end() ? std::string() : it->second;
}

std::vector<std::string> Utf8Chars(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        const size_t n = Utf8CharLen(s, i);
        if (n == 0 || i + n > s.size()) break;
        out.push_back(s.substr(i, n));
        i += n;
    }
    return out;
}

std::string PhraseCode(const std::string& utf8Word, const Dictionary& dict) {
    const std::vector<std::string> chars = Utf8Chars(utf8Word);
    if (chars.size() < 2) return std::string();

    std::vector<std::string> codes;
    codes.reserve(chars.size());
    for (const auto& c : chars) {
        std::string fc = dict.FullCodeOfChar(c);
        if (fc.empty()) return std::string();      // 有字查不到全码：交给调用方兜底
        codes.push_back(std::move(fc));
    }

    // 取第 i 个字的第 k 码（码不够长时退到最后一码，避免越界）
    auto at = [&codes](size_t i, size_t k) -> char {
        const std::string& c = codes[i];
        if (c.empty()) return 'x';
        return c[k < c.size() ? k : c.size() - 1];
    };

    const size_t n = codes.size();
    std::string out;
    out.reserve(4);
    if (n == 2) {
        out += at(0, 0); out += at(0, 1); out += at(1, 0); out += at(1, 1);
    } else if (n == 3) {
        out += at(0, 0); out += at(1, 0); out += at(2, 0); out += at(2, 1);
    } else if (n == 4) {
        for (size_t i = 0; i < 4; ++i) out += at(i, 0);
    } else {
        out += at(0, 0); out += at(1, 0); out += at(2, 0); out += at(n - 1, 0);
    }
    return out;
}

// ---------------------------------------------------------------- Dictionary
void Dictionary::IndexOne(Entry&& e) {
    uint32_t idx = static_cast<uint32_t>(entries_.size());
    buckets_[e.code].push_back(idx);
    entries_.push_back(std::move(e));
    sortedDirty_ = true;
}

long long Dictionary::Load(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return -1;

    long long n = 0;
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (first) {
            first = false;
            if (line.rfind("code\t", 0) == 0) continue;
        }
        size_t p1 = line.find('\t');
        if (p1 == std::string::npos) continue;
        size_t p2 = line.find('\t', p1 + 1);
        if (p2 == std::string::npos) continue;
        size_t p3 = line.find('\t', p2 + 1);

        Entry e;
        e.code   = line.substr(0, p1);
        e.word   = line.substr(p1 + 1, p2 - p1 - 1);
        e.weight = 0;
        if (p3 != std::string::npos) {
            const std::string w = line.substr(p2 + 1, p3 - p2 - 1);
            if (!w.empty()) e.weight = std::atoll(w.c_str());
            e.user = (line.compare(p3 + 1, 4, "user") == 0);
        }
        if (e.code.empty() || e.word.empty()) continue;
        if (e.code.size() > 4) continue;
        bool ok = true;
        for (char c : e.code) { if (!IsCodeChar(c)) { ok = false; break; } }
        if (!ok) continue;
        IndexOne(std::move(e));
        ++n;
    }
    return n;
}

void Dictionary::AddUserWord(const std::string& code, const std::string& word, int64_t weight) {
    const std::string key = code + "|" + word;
    auto it = userIndex_.find(key);
    if (it != userIndex_.end() && it->second < entries_.size()) {
        entries_[it->second].weight = weight;
        entries_[it->second].user = true;
        sortedDirty_ = true;
        return;
    }
    const uint32_t idx = (uint32_t)entries_.size();
    Entry e;
    e.code = code;
    e.word = word;
    e.weight = weight;
    e.user = true;
    IndexOne(std::move(e));
    userIndex_[key] = idx;   // 必须登记，否则二进制模式下 CollectUserCandidates 看不到
}

void Dictionary::BuildSortedIndex() const {
    sorted_.resize(entries_.size());
    for (size_t i = 0; i < sorted_.size(); ++i) sorted_[i] = (uint32_t)i;
    std::sort(sorted_.begin(), sorted_.end(), [this](uint32_t a, uint32_t b) {
        const Entry& x = entries_[a];
        const Entry& y = entries_[b];
        if (x.code != y.code) return x.code < y.code;
        return x.weight > y.weight;
    });
    sortedDirty_ = false;
}

bool Dictionary::HasExact(std::string_view code) const {
    return buckets_.find(std::string(code)) != buckets_.end();
}

bool Dictionary::HasPrefix(std::string_view prefix) const {
    // 每次按键都会先做一次 Lookup，这里直接复用其结果判定即可
    return !Lookup(prefix, 1).empty();
}
std::vector<Candidate> Dictionary::LookupExact(std::string_view code) const {
    std::vector<Candidate> hits;
    if (bin_) {
        size_t lo = 0, hi = 0;
        if (!BinRange(code, lo, hi)) return hits;
        for (size_t i = lo; i < hi; ++i) {
            const BinaryRec& r = bin_[i];
            if (r.codeLen != code.size()) continue;
            hits.push_back(Candidate{ std::string(binBlob_ + r.wordOff, r.wordLen),
                                      std::string(code), r.weight, (r.flags & 1) != 0 });
        }
        return hits;
    }
    auto it = buckets_.find(std::string(code));
    if (it != buckets_.end()) {
        for (uint32_t i : it->second) {
            const Entry& e = entries_[i];
            hits.push_back(Candidate{ e.word, e.code, e.weight, e.user });
        }
    }
    std::sort(hits.begin(), hits.end(), [](const Candidate& a, const Candidate& b) {
        if (a.user != b.user) return a.user;
        if (a.weight != b.weight) return a.weight > b.weight;
        return a.word < b.word;
    });
    stats_.exactHits = hits.size();
    return hits;
}
namespace {
// 合并"完全匹配 + 前缀匹配"，按词去重（同一词的多码只显示一次，保留排名最高者）
void MergeDedupe(std::vector<Candidate>&& exact, std::vector<Candidate>&& prefix,
                 size_t maxCandidates, std::vector<Candidate>& out) {
    std::unordered_set<std::string> seen;
    seen.reserve(exact.size() + prefix.size());
    auto push = [&](Candidate&& c) -> bool {
        if (out.size() >= maxCandidates) return false;
        if (!seen.insert(c.word).second) return true;   // 已出现过，跳过但继续
        out.push_back(std::move(c));
        return true;
    };
    for (auto& c : exact)  push(std::move(c));
    for (auto& c : prefix) if (out.size() < maxCandidates) push(std::move(c));
}
}
namespace {
// 候选优劣：用户词优先 > 码短优先 > 权重大优先
inline bool Better(const Candidate& a, const Candidate& b) {
    if (a.user != b.user) return a.user;
    if (a.code.size() != b.code.size()) return a.code.size() < b.code.size();
    if (a.weight != b.weight) return a.weight > b.weight;
    return a.word < b.word;
}
}

std::vector<Candidate> Dictionary::Lookup(std::string_view input, size_t maxCandidates) const {
    if (maxCandidates == 0) maxCandidates = 1;
    std::vector<Candidate> out;
    if (input.empty()) return out;

    // ---------- 二进制（mmap）模式 ----------
    if (bin_) {
        size_t lo = 0, hi = 0;
        if (!BinRange(input, lo, hi)) {
            // 系统词库里没有这个前缀（例如自定义短语用的 dz / sfz，含 z），
            // 但用户词可能在：不能直接返回空，否则自定义短语永远打不出来。
            stats_.exactHits = 0;
            stats_.prefixHits = 0;
            CollectUserCandidates(input, out);
            std::sort(out.begin(), out.end(), [this](const Candidate& a, const Candidate& b) {
                return BetterCand(a, b);
            });
            return out;
        }
        std::vector<Candidate> exact;
        auto worseFirst = [this](const Candidate& a, const Candidate& b) { return BetterCand(a, b); }; // top = worst (was inverted)
        std::priority_queue<Candidate, std::vector<Candidate>, decltype(worseFirst)> heap(worseFirst);

        for (size_t i = lo; i < hi; ++i) {
            const BinaryRec& r = bin_[i];
            Candidate c{ std::string(binBlob_ + r.wordOff, r.wordLen),
                         std::string(r.code, r.codeLen), r.weight, (r.flags & 1) != 0 };
            if (r.codeLen == input.size()) {
                exact.push_back(std::move(c));          // 磁盘顺序已是权重降序
            } else {
                if (heap.size() < maxCandidates) heap.push(std::move(c));
                else if (BetterCand(c, heap.top())) { heap.pop(); heap.push(std::move(c)); }
            }
        }
        stats_.exactHits = exact.size();
        stats_.prefixHits = heap.size();

        std::vector<Candidate> pf;
        while (!heap.empty()) { pf.push_back(heap.top()); heap.pop(); }
        std::sort(pf.begin(), pf.end(), [this](const Candidate& a, const Candidate& b) { return BetterCand(a, b); });
        std::vector<Candidate> merged;
        merged.reserve(exact.size() + pf.size() + 8);
        for (auto& c : exact) merged.push_back(std::move(c));
        for (auto& c : pf)    merged.push_back(std::move(c));
        CollectUserCandidates(input, merged);   // 用户词不在映射区，必须单独并入
        std::sort(merged.begin(), merged.end(), [this](const Candidate& a, const Candidate& b) { return BetterCand(a, b); });
        std::unordered_set<std::string> seen;
        std::vector<Candidate> tail;   // mode==2 时把这些沉到末尾
        for (auto& c : merged) {
            if (out.size() >= (size_t)maxCandidates) break;
            if (!seen.insert(c.word).second) continue;
            if (IsLevel1Noise(c, input.size())) {
                if (level1Mode_ == 1) continue;            // 隐藏
                tail.push_back(std::move(c));              // 沉底
                continue;
            }
            out.push_back(std::move(c));
        }
        for (auto& c : tail) {
            if (out.size() >= (size_t)maxCandidates) break;
            out.push_back(std::move(c));
        }
        EnsureExactFirst(input, out);
        InsertKeyRepeat(input, out, maxCandidates);
        return out;
    }

    // ---------- TSV 模式 ----------
    if (sortedDirty_) BuildSortedIndex();
    std::vector<Candidate> exact;
    auto it = buckets_.find(std::string(input));
    if (it != buckets_.end()) {
        for (uint32_t i : it->second) {
            const Entry& e = entries_[i];
            exact.push_back(Candidate{ e.word, e.code, e.weight, e.user });
        }
    }
    std::sort(exact.begin(), exact.end(), [this](const Candidate& a, const Candidate& b) { return BetterCand(a, b); });

    const std::string pfx(input);
    auto lower = std::lower_bound(sorted_.begin(), sorted_.end(), pfx,
        [this](uint32_t idx, const std::string& p) { return entries_[idx].code < p; });

    auto worseFirst = [this](const Candidate& a, const Candidate& b) { return BetterCand(a, b); }; // top = worst (was inverted)
    std::priority_queue<Candidate, std::vector<Candidate>, decltype(worseFirst)> heap(worseFirst);
    size_t scanned = 0;
    for (auto i = lower; i != sorted_.end(); ++i) {
        const Entry& e = entries_[*i];
        if (e.code.size() <= pfx.size()) continue;
        if (e.code.compare(0, pfx.size(), pfx) != 0) break;
        ++scanned;
        Candidate c{ e.word, e.code, e.weight, e.user };
        if (heap.size() < maxCandidates) heap.push(std::move(c));
        else if (BetterCand(c, heap.top())) { heap.pop(); heap.push(std::move(c)); }
    }
    stats_.exactHits = exact.size();
    stats_.prefixHits = scanned;

    std::vector<Candidate> pf;
    while (!heap.empty()) { pf.push_back(heap.top()); heap.pop(); }
    std::sort(pf.begin(), pf.end(), [this](const Candidate& a, const Candidate& b) { return BetterCand(a, b); });
    MergeDedupe(std::move(exact), std::move(pf), maxCandidates, out);
    if (level1Mode_ != 0) {
        std::vector<Candidate> keep, tail;
        for (auto& c : out) {
            if (IsLevel1Noise(c, input.size())) tail.push_back(std::move(c));
            else keep.push_back(std::move(c));
        }
        out.clear();
        for (auto& c : keep) { if (out.size() >= (size_t)maxCandidates) break; out.push_back(std::move(c)); }
        if (level1Mode_ == 2) {
            for (auto& c : tail) { if (out.size() >= (size_t)maxCandidates) break; out.push_back(std::move(c)); }
        }
    }
    EnsureExactFirst(input, out);
    InsertKeyRepeat(input, out, maxCandidates);
    return out;
}
// ---------------------------------------------------------------- 二进制格式（mmap）
void Dictionary::UnmapBinary() {
    if (view_) { UnmapViewOfFile(view_); view_ = nullptr; }
    if (hMap_) { CloseHandle((HANDLE)hMap_); hMap_ = nullptr; }
    if (hFile_) { CloseHandle((HANDLE)hFile_); hFile_ = nullptr; }
    bin_ = nullptr; binBlob_ = nullptr; binCount_ = 0; viewSize_ = 0;
}

long long Dictionary::LoadBinary(const std::wstring& path) {
    UnmapBinary();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return -1;
    LARGE_INTEGER sz = {};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < (LONGLONG)sizeof(BinaryHeader)) {
        CloseHandle(h);
        return -1;
    }
    HANDLE m = CreateFileMappingW(h, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!m) { CloseHandle(h); return -1; }
    const uint8_t* v = (const uint8_t*)MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    if (!v) { CloseHandle(m); CloseHandle(h); return -1; }

    const BinaryHeader* hd = (const BinaryHeader*)v;
    if (memcmp(hd->magic, "WBD2", 4) != 0 || hd->version != 1 ||
        (size_t)hd->recCount == 0 ||
        sizeof(BinaryHeader) + (size_t)hd->recCount * sizeof(BinaryRec) + hd->blobSize > (size_t)sz.QuadPart) {
        UnmapViewOfFile(v); CloseHandle(m); CloseHandle(h);
        return -1;
    }
    hFile_ = (void*)h; hMap_ = (void*)m; view_ = v; viewSize_ = (size_t)sz.QuadPart;
    bin_ = (const BinaryRec*)(v + sizeof(BinaryHeader));
    binBlob_ = (const char*)(v + sizeof(BinaryHeader) + (size_t)hd->recCount * sizeof(BinaryRec));
    binCount_ = hd->recCount;
    return (long long)binCount_;
}

bool Dictionary::SaveBinary(const std::wstring& path) {
    if (entries_.empty()) return false;

    // 按 code 字典序、权重降序排
    std::vector<uint32_t> idx(entries_.size());
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = (uint32_t)i;
    std::sort(idx.begin(), idx.end(), [this](uint32_t a, uint32_t b) {
        const Entry& x = entries_[a]; const Entry& y = entries_[b];
        int c = memcmp(x.code.c_str(), y.code.c_str(), std::min<size_t>(4, std::min(x.code.size(), y.code.size())));
        if (c != 0) return c < 0;
        if (x.code.size() != y.code.size()) return x.code.size() < y.code.size();
        if (x.weight != y.weight) return x.weight > y.weight;
        return x.word < y.word;
    });

    std::string blob;
    blob.reserve(idx.size() * 8);
    std::vector<BinaryRec> recs(idx.size());
    for (size_t k = 0; k < idx.size(); ++k) {
        const Entry& e = entries_[idx[k]];
        BinaryRec& r = recs[k];
        memset(&r, 0, sizeof(r));
        memcpy(r.code, e.code.data(), std::min<size_t>(4, e.code.size()));
        r.codeLen = (uint8_t)e.code.size();
        r.flags   = e.user ? 1 : 0;
        r.wordLen = (uint16_t)std::min<size_t>(65535, e.word.size());
        r.weight  = e.weight;
        r.wordOff = (uint32_t)blob.size();
        blob.append(e.word);
    }
    BinaryHeader hd = {};
    memcpy(hd.magic, "WBD2", 4);
    hd.version = 1;
    hd.recCount = (uint32_t)recs.size();
    hd.blobSize = (uint64_t)blob.size();

    std::ofstream os(path, std::ios::binary | std::ios::trunc);
    if (!os) return false;
    os.write((const char*)&hd, sizeof(hd));
    os.write((const char*)recs.data(), (std::streamsize)(recs.size() * sizeof(BinaryRec)));
    os.write(blob.data(), (std::streamsize)blob.size());
    return os.good();
}

// 在二进制记录上按前缀定位区间（记录已按 code 字典序排序）
bool Dictionary::BinRange(std::string_view prefix, size_t& lo, size_t& hi) const {
    if (!bin_ || prefix.empty() || prefix.size() > 4) return false;
    char key[4] = { 0, 0, 0, 0 };
    memcpy(key, prefix.data(), prefix.size());
    // lower_bound：第一个 code >= prefix
    size_t l = 0, r = binCount_;
    while (l < r) {
        size_t mid = (l + r) / 2;
        if (memcmp(bin_[mid].code, key, 4) < 0) l = mid + 1; else r = mid;
    }
    lo = l;
    size_t e = lo;
    while (e < binCount_ && memcmp(bin_[e].code, key, prefix.size()) == 0) ++e;
    hi = e;
    return lo < hi;
}
// ---------------------------------------------------------------- 用户词
long long Dictionary::LoadUserWords(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return -1;
    long long n = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        size_t p1 = line.find('\t');
        if (p1 == std::string::npos) continue;
        size_t p2 = line.find('\t', p1 + 1);
        std::string code = line.substr(0, p1);
        std::string word = (p2 == std::string::npos) ? line.substr(p1 + 1)
                                                     : line.substr(p1 + 1, p2 - p1 - 1);
        int64_t w = 1;
        if (p2 != std::string::npos) {
            const std::string ws = line.substr(p2 + 1);
            if (!ws.empty()) w = std::atoll(ws.c_str());
        }
        if (code.empty() || word.empty()) continue;
        AddUserWord(code, word, w);
        ++n;
    }
    return n;
}

bool Dictionary::SaveUserWords(const std::wstring& path) const {
    std::string out;
    out.reserve(userIndex_.size() * 24);
    out += "# wubi user dictionary: code <TAB> word <TAB> weight\n";
    // 保持稳定输出：按 code 再按权重排序
    std::vector<const Entry*> list;
    list.reserve(userIndex_.size());
    for (const auto& kv : userIndex_) {
        if (kv.second < entries_.size()) list.push_back(&entries_[kv.second]);
    }
    std::sort(list.begin(), list.end(), [](const Entry* a, const Entry* b) {
        if (a->code != b->code) return a->code < b->code;
        return a->weight > b->weight;
    });
    for (const Entry* e : list) {
        out += e->code;
        out += '\t';
        out += e->word;
        out += '\t';
        out += std::to_string((long long)e->weight);
        out += '\n';
    }
    std::ofstream os(path, std::ios::binary | std::ios::trunc);
    if (!os) return false;
    os.write(out.data(), (std::streamsize)out.size());
    return os.good();
}

// ---------------------------------------------------------------- 数字转中文
static const wchar_t* kLowerDigitsTbl = L"〇一二三四五六七八九";
static const wchar_t* kUpperDigitsTbl = L"零壹贰叁肆伍陆柒捌玖";
static const wchar_t* kLowerUnitsTbl  = L"十百千";
static const wchar_t* kUpperUnitsTbl  = L"拾佰仟";
static const wchar_t* kGroupUnitsTbl[] = { L"", L"万", L"亿", L"万亿" };

// 读一组（最多 4 位，值是 0..9999 的十进制串）
static std::wstring ReadFourDigits(const std::string& g, bool upper) {
    const wchar_t* digits = upper ? kUpperDigitsTbl : kLowerDigitsTbl;
    const wchar_t* units  = upper ? kUpperUnitsTbl  : kLowerUnitsTbl;
    std::wstring out;
    bool zeroPending = false;
    const int n = (int)g.size();
    for (int i = 0; i < n; ++i) {
        const int d = g[(size_t)i] - '0';
        const int pos = n - 1 - i;
        if (d == 0) {
            if (!out.empty()) zeroPending = true;
            continue;
        }
        if (zeroPending) { out += L'零'; zeroPending = false; }
        out += digits[d];
        if (pos > 0) out += units[pos - 1];
    }
    return out;
}

static int GroupValue(const std::string& g) {
    int v = 0;
    for (char c : g) v = v * 10 + (c - '0');
    return v;
}

void NumberForms(const std::string& digitsIn, std::vector<std::wstring>& forms) {
    forms.clear();
    std::string digits;
    for (char c : digitsIn) if (c >= '0' && c <= '9') digits.push_back(c);
    if (digits.empty()) return;

    // [1] 逐位直读（0 用 〇）
    std::wstring perDigit;
    for (char c : digits) perDigit += kLowerDigitsTbl[c - '0'];
    forms.push_back(perDigit);

    const size_t start = digits.find_first_not_of('0');
    const std::string d = (start == std::string::npos) ? std::string("0") : digits.substr(start);
    if (d == "0") {
        forms.push_back(std::wstring(1, L'零'));
        forms.push_back(std::wstring(1, L'零'));
        return;
    }

    auto buildGrouped = [&](bool upper) -> std::wstring {
        std::vector<std::string> groups;            // 低位组在前
        for (int i = (int)d.size(); i > 0; i -= 4) {
            const int b = (i - 4 > 0) ? i - 4 : 0;
            groups.push_back(d.substr((size_t)b, (size_t)(i - b)));
        }
        std::wstring out;
        for (int gi = (int)groups.size() - 1; gi >= 0; --gi) {
            const std::string& g = groups[(size_t)gi];
            const int v = GroupValue(g);
            if (v == 0) continue;                   // 整组为零，跳过
            // 低一组不足千位（或中间有整组零）时要补「零」：一亿零一万零一
            if (!out.empty() && v < 1000 && out.back() != L'零') out += L'零';
            std::wstring part = ReadFourDigits(g, upper);
            // 最高一组是 10-19 时，小写习惯读作「十X」「十万」「十二万」
            if (out.empty() && !upper && v >= 10 && v <= 19 &&
                !part.empty() && part[0] == kLowerDigitsTbl[1]) {
                part.erase(part.begin());
            }
            out += part;
            out += kGroupUnitsTbl[gi < 4 ? gi : 3];
        }
        return out;
    };

    forms.push_back(buildGrouped(false));
    forms.push_back(buildGrouped(true));
}

// ---------------------------------------------------------------- 日期/时间/星期
// kind: 0=date 1=time 2=week；weekday: 0=周日
static const wchar_t* kWeekNames[] = { L"星期日", L"星期一", L"星期二", L"星期三", L"星期四", L"星期五", L"星期六" };

static std::wstring UpperChineseNumber(int v);   // 前置声明（月/日的中文读法）

void DateTimeCandidates(int kind, int year, int month, int day,
                        int hour, int minute, int second, int weekday,
                        std::vector<std::string>& out) {
    out.clear();
    auto u8 = [](const std::wstring& w) -> std::string {
        if (w.empty()) return std::string();
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string s((size_t)(n > 0 ? n : 0), '\0');
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
        return s;
    };
    wchar_t buf[128];

    // 常用片段
    swprintf_s(buf, L"%04d.%02d.%02d", year, month, day);
    const std::wstring dDot = buf;
    swprintf_s(buf, L"%04d年%d月%d日", year, month, day);
    const std::wstring dCn = buf;
    swprintf_s(buf, L"%04d-%02d-%02d", year, month, day);
    const std::wstring dDash = buf;

    // 二〇二六年十一月十七日
    std::wstring dLong;
    {
        const wchar_t* digits = L"〇一二三四五六七八九";
        int v = year;
        wchar_t ybuf[8] = { 0 };
        swprintf_s(ybuf, L"%04d", v);
        for (int i = 0; i < 4; ++i) dLong += digits[ybuf[i] - '0'];
        dLong += L"年";
        dLong += UpperChineseNumber(month);
        dLong += L"月";
        dLong += UpperChineseNumber(day);
        dLong += L"日";
    }

    swprintf_s(buf, L"%02d:%02d:%02d", hour, minute, second);
    const std::wstring tHms = buf;

    const std::wstring wname = kWeekNames[(weekday >= 0 && weekday <= 6) ? weekday : 0];

    switch (kind) {
    case 0:  // date
        out.push_back(u8(dDot));
        out.push_back(u8(dCn));
        out.push_back(u8(dDash));
        out.push_back(u8(dLong));
        break;

    case 1:  // time（用户指定顺序：HMS / 短横线日期 / 中文日期 / AM-PM / 全中文）
        out.push_back(u8(tHms));
        out.push_back(u8(dDash + L" " + tHms));
        out.push_back(u8(dCn + L" " + tHms));
        {
            // 2026年11月17日, AM 10:51:40
            const wchar_t* ap = (hour < 12) ? L"AM" : L"PM";
            const int h12 = (hour % 12 == 0) ? 12 : (hour % 12);
            swprintf_s(buf, L"%ls, %ls %02d:%02d:%02d", dCn.c_str(), ap, h12, minute, second);
            out.push_back(u8(buf));
        }
        out.push_back(u8(dLong + L" " + tHms));
        break;

    case 2:  // week
        out.push_back(u8(wname));
        out.push_back(u8(dLong + L" " + wname));
        out.push_back(u8(dCn + L" " + wname));
        break;

    default:
        break;
    }
}

// 1..31 的中文读法（十一、二十、二十三…）
static std::wstring UpperChineseNumber(int v) {
    const wchar_t* digits = L"〇一二三四五六七八九";
    if (v <= 0) return L"〇";
    if (v < 10) return std::wstring(1, digits[v]);
    if (v == 10) return L"十";
    if (v < 20) return std::wstring(L"十") + digits[v - 10];
    const int tens = v / 10, ones = v % 10;
    std::wstring s(1, digits[tens]);
    s += L"十";
    if (ones) s += digits[ones];
    return s;
}

// UTF-8 字符数（用于"词组优先"判断）
static size_t Utf8CharCount(const std::string& s) {
    size_t n = 0;
    for (size_t i = 0; i < s.size(); ) {
        const unsigned char c = (unsigned char)s[i];
        i += (c < 0x80) ? 1 : (((c >> 5) == 0x6) ? 2 : (((c >> 4) == 0xE) ? 3 : 4));
        ++n;
    }
    return n;
}

// 单键输入时把"该键重复四次"对应的字放到第 2 位（QQ五笔 的做法：k -> 中 / 口）
void Dictionary::InsertKeyRepeat(std::string_view input, std::vector<Candidate>& out, size_t maxCandidates) const {
    if (sortMode_ != 0) return;
    if (input.size() != 1) return;
    const std::string quad(4, input[0]);

    Candidate best;
    bool found = false;
    auto consider = [&](Candidate&& c) {
        if (!found) { best = std::move(c); found = true; return; }
        if (best.user && !c.user) { best = std::move(c); return; }
        if (best.user == c.user && c.weight > best.weight) best = std::move(c);
    };

    if (bin_) {
        size_t lo = 0, hi = 0;
        if (BinRange(quad, lo, hi)) {
            for (size_t i = lo; i < hi; ++i) {
                const BinaryRec& r = bin_[i];
                if (r.codeLen != 4 || std::string_view(r.code, 4) != quad) continue;
                consider(Candidate{ std::string(binBlob_ + r.wordOff, r.wordLen), quad,
                                    r.weight, (r.flags & 1) != 0 });
            }
        }
    } else {
        auto it = buckets_.find(quad);
        if (it != buckets_.end()) {
            for (uint32_t idx : it->second) {
                if (idx >= entries_.size()) continue;
                const Entry& e = entries_[idx];
                if (e.code != quad) continue;
                consider(Candidate{ e.word, e.code, e.weight, e.user });
            }
        }
    }
    if (!found) return;
    if (!out.empty() && out[0].word == best.word) return;   // 与一级简码同一个字，无需重复

    out.erase(std::remove_if(out.begin(), out.end(),
              [&](const Candidate& c) { return c.word == best.word; }), out.end());
    const size_t pos = out.empty() ? 0 : 1;
    out.insert(out.begin() + (std::ptrdiff_t)pos, best);
    if (out.size() > maxCandidates) out.resize(maxCandidates);
}
// 1/2/3 码输入时，把该码的简码字（精确匹配）提到最前。
// 规则来自用户：一级/二级/三级简码不该被词频顶掉；只有四码上屏才考虑词频。
void Dictionary::EnsureExactFirst(std::string_view input, std::vector<Candidate>& out) const {
    // 只在"码短优先"模式下保证；纯词频模式严格按权重
    if (sortMode_ != 0) return;
    if (input.empty() || input.size() > 3 || out.empty()) return;

    // 单码：强制该码的一级简码字（哪怕它被学成了用户词，也要排第一）
    if (input.size() == 1) {
        EnsureLevel1();
        for (size_t i = 0; i < out.size(); ++i) {
            if (out[i].code.size() == 1 && level1_.find(out[i].word) != level1_.end()) {
                if (i != 0) std::rotate(out.begin(), out.begin() + (std::ptrdiff_t)i, out.begin() + (std::ptrdiff_t)i + 1);
                return;
            }
        }
    }

    // 二/三码：取"精确匹配"里权重最高的那个提到首位。
    // 不能用"非用户词"作为条件——用户学过的正字（如 rhf -> 看）反而会被跳过，
    // 导致生僻字（𣥆，权重 0）被顶上来。
    size_t best = out.size();
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i].code.size() != input.size()) continue;
        if (best == out.size() || out[i].weight > out[best].weight) best = i;
    }
    if (best != out.size() && best != 0) {
        std::rotate(out.begin(), out.begin() + (std::ptrdiff_t)best, out.begin() + (std::ptrdiff_t)best + 1);
    }
}
// 收集"一级简码"字：码长为 1 的那些字（共 25 个）
void Dictionary::EnsureLevel1() const {
    if (level1Built_) return;
    level1Built_ = true;
    if (bin_) {
        for (size_t i = 0; i < binCount_; ++i) {
            if (bin_[i].codeLen == 1) {
                level1_.insert(std::string(binBlob_ + bin_[i].wordOff, bin_[i].wordLen));
            }
        }
    } else {
        for (const auto& e : entries_) {
            if (e.code.size() == 1) level1_.insert(e.word);
        }
    }
}

// 是否为"多键输入时的一级简码噪声"（一键就能打出来的单字）
bool Dictionary::IsLevel1Noise(const Candidate& c, size_t inputLen) const {
    if (level1Mode_ == 0 || inputLen <= 1) return false;
    // 精确匹配豁免：输入长度 <= 3 且候选的码正好等于输入时，这条就是"该码的简码"，
    // 必须留在原位（一级/二级/三级简码不该被任何规则顶掉）。
    if (inputLen <= 3 && c.code.size() == inputLen) return false;
    if (Utf8CharCount(c.word) != 1) return false;
    EnsureLevel1();
    return level1_.find(c.word) != level1_.end();
}
bool Dictionary::BetterCand(const Candidate& a, const Candidate& b) const {
    if (a.user != b.user) return a.user;              // 用户词永远最优先
    if (sortMode_ == 0 && a.code.size() != b.code.size()) return a.code.size() < b.code.size();
    // 同码长时词组优先于单字：实测 QQ五笔 就是这个行为（trnt→特性/选择性/我，aaaa→词组/工）。
    // 仅作用于"码短优先"模式；纯词频模式应严格按权重。
    if (sortMode_ == 0) {
        const bool aw = Utf8CharCount(a.word) >= 2;
        const bool bw = Utf8CharCount(b.word) >= 2;
        if (aw != bw) return aw;
    }
    if (a.weight != b.weight) return a.weight > b.weight;
    if (a.code.size() != b.code.size()) return a.code.size() < b.code.size();
    return a.word < b.word;
}

void Dictionary::CollectUserCandidates(std::string_view code, std::vector<Candidate>& out) const {
    for (const auto& kv : userIndex_) {
        const uint32_t idx = kv.second;
        if (idx >= entries_.size()) continue;
        const Entry& e = entries_[idx];
        if (e.code.size() < code.size()) continue;
        if (e.code.compare(0, code.size(), code) != 0) continue;
        out.push_back(Candidate{ e.word, e.code, e.weight, true });
    }
}

void Dictionary::BumpUserWord(const std::string& code, const std::string& word, int64_t delta) {
    const std::string key = code + "|" + word;
    auto it = userIndex_.find(key);
    if (it != userIndex_.end() && it->second < entries_.size()) {
        entries_[it->second].weight += delta;
        entries_[it->second].user = true;
        sortedDirty_ = true;
        return;
    }
    Entry e;
    e.code = code;
    e.word = word;
    e.weight = delta;
    e.user = true;
    uint32_t idx = (uint32_t)entries_.size();
    buckets_[e.code].push_back(idx);
    entries_.push_back(std::move(e));
    userIndex_[key] = idx;
    sortedDirty_ = true;
}
// ---------------------------------------------------------------- Transliteration
bool Transliteration::Load(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    std::streamoff sz = in.tellg();
    if (sz < 65536) return false;
    in.seekg(0, std::ios::beg);
    map_.resize(65536);
    in.read(reinterpret_cast<char*>(map_.data()), 65536 * 2);
    if (!in) { map_.clear(); return false; }
    rev_.assign(65536, 0);
    for (size_t i = 0; i < map_.size(); ++i) {
        uint16_t t = map_[i];
        if (t != i && rev_[t] == 0) rev_[t] = (uint16_t)i;
    }
    loaded_ = true;
    return true;
}

std::wstring Transliteration::ToTraditional(std::wstring_view s) const {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) {
        if (!loaded_ || (size_t)c >= map_.size()) { out.push_back(c); continue; }
        uint16_t t = map_[(size_t)c];
        out.push_back(t ? (wchar_t)t : c);
    }
    return out;
}

// ---------------------------------------------------------------- Punctuation
void Punctuation::BuildDefault() {
    // 内置兜底表（QQ五笔 punc.ini 的常见子集）：
    //   col0 英文半角 = 原字符, col1 中文半角, col2 英文全角, col3 中文全角
    auto add = [this](char k, const wchar_t* half, const wchar_t* full, const wchar_t* half2 = nullptr, const wchar_t* full2 = nullptr) {
        Row r;
        std::wstring base(1, (wchar_t)k);
        r.col[(int)PuncMode::AsciiHalf]   = { base, L"" };
        r.col[(int)PuncMode::ChineseHalf] = { half, half2 ? half2 : L"" };
        r.col[(int)PuncMode::AsciiFull]   = { full ? full : base.c_str(), full2 ? full2 : L"" };
        r.col[(int)PuncMode::ChineseFull] = { half, half2 ? half2 : L"" };
        rows_[(int)k] = r;
    };
    add(',',  L"，", L"，");
    add('.',  L"。", L"．");
    add('?',  L"？", L"？");
    add('!',  L"！", L"！");
    add(':',  L"：", L"：");
    add(';',  L"；", L"；");
    add('(',  L"（", L"（");
    add(')',  L"）", L"）");
    add('[',  L"【", L"［");
    add(']',  L"】", L"］");
    add('{',  L"｛", L"｛");
    add('}',  L"｝", L"｝");
    add('<',  L"《", L"＜");
    add('>',  L"》", L"＞");
    add('@',  L"@",  L"＠");
    add('#',  L"#",  L"＃");
    add('$',  L"￥", L"＄");
    add('%',  L"%",  L"％");
    add('&',  L"&",  L"＆");
    add('*',  L"*",  L"＊");
    add('+',  L"+",  L"＋");
    add('-',  L"-",  L"－", L"—", L"—");
    add('=',  L"=",  L"＝");
    add('/',  L"/",  L"／");
    add('\\', L"、", L"＼");
    add('|',  L"|",  L"｜");
    add('_',  L"——", L"＿");
    add('^',  L"……", L"＾");
    add('~',  L"～", L"～");
    add('`',  L"`",  L"｀");
    add('\'', L"‘", L"’");
    add('"',  L"“", L"＂", L"”", L"＂");
}

bool Punctuation::Load(const std::wstring& path) {
    rows_.clear();
    std::ifstream in(path, std::ios::binary);
    if (!in) { BuildDefault(); return false; }
    std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (raw.size() < 4) { BuildDefault(); return false; }

    // punc.ini 实测为 UTF-16LE + BOM；同时兼容 UTF-8(BOM/无) 与 ANSI
    std::wstring text;
    const unsigned char* u = reinterpret_cast<const unsigned char*>(raw.data());
    if (raw.size() >= 2 && u[0] == 0xFF && u[1] == 0xFE) {
        size_t n = (raw.size() - 2) / 2;
        text.resize(n);
        memcpy(&text[0], raw.data() + 2, n * 2);
    } else if (raw.size() >= 2 && u[0] == 0xFE && u[1] == 0xFF) {   // UTF-16BE
        size_t n = (raw.size() - 2) / 2;
        text.resize(n);
        for (size_t i = 0; i < n; ++i) {
            text[i] = (wchar_t)((u[2 + i * 2] << 8) | u[3 + i * 2]);
        }
    } else {
        size_t off = (raw.size() >= 3 && u[0] == 0xEF && u[1] == 0xBB && u[2] == 0xBF) ? 3 : 0;
        const char* s = raw.data() + off;
        int len = (int)(raw.size() - off);
        int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, len, nullptr, 0);
        UINT cp = CP_UTF8;
        if (n <= 0) { cp = CP_ACP; n = MultiByteToWideChar(CP_ACP, 0, s, len, nullptr, 0); }
        if (n > 0) {
            text.resize((size_t)n);
            MultiByteToWideChar(cp, 0, s, len, &text[0], n);
        }
    }
    if (text.empty()) { BuildDefault(); return false; }

    // 按行切分
    size_t pos = 0;
    bool header = true;
    while (pos <= text.size()) {
        size_t nl = text.find(L'\n', pos);
        std::wstring line = text.substr(pos, (nl == std::wstring::npos ? text.size() : nl) - pos);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        pos = (nl == std::wstring::npos) ? text.size() + 1 : nl + 1;

        if (header) { header = false; continue; }     // 跳过表头
        if (line.size() < 3) continue;
        const char key = (char)line[0];
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos || eq + 1 >= line.size()) continue;

        // payload: 3 组，每组 = 数字 + 该数量码元（顺序: 中文半角 / 英文全角 / 中文全角）
        const std::wstring payload = line.substr(eq + 1);
        Item cols[3];
        size_t i = 0;
        for (int g = 0; g < 3 && i < payload.size(); ++g) {
            int cnt = (payload[i] >= L'1' && payload[i] <= L'9') ? (payload[i] - L'0') : 1;
            ++i;
            for (int k = 0; k < cnt && i < payload.size(); ++k) cols[g].first.push_back(payload[i++]);
        }

        auto it = rows_.find((int)key);
        if (it == rows_.end()) {
            Row r;
            r.col[(int)PuncMode::AsciiHalf]   = { std::wstring(1, (wchar_t)key), L"" };
            r.col[(int)PuncMode::ChineseHalf] = cols[0];
            r.col[(int)PuncMode::AsciiFull]   = cols[1];
            r.col[(int)PuncMode::ChineseFull] = cols[2];
            rows_[(int)key] = r;
        } else {
            // 同键第二行（引号的“第二次”）：记为配对后半
            Row& r = it->second;
            r.col[(int)PuncMode::ChineseHalf].second = cols[0].first;
            r.col[(int)PuncMode::AsciiFull].second   = cols[1].first;
            r.col[(int)PuncMode::ChineseFull].second = cols[2].first;
        }
    }
    if (rows_.empty()) { BuildDefault(); return false; }
    return true;
}
bool Punctuation::Map(char asciiKey, PuncMode mode, std::wstring& out) {
    auto it = rows_.find((int)asciiKey);
    if (it == rows_.end()) return false;
    const Item& item = it->second.col[(int)mode];
    if (item.first.empty()) return false;
    if (!item.second.empty()) {
        // 成对引号：第一次 -> first，第二次 -> second
        out = (toggle_ == 0) ? item.first : item.second;
        toggle_ ^= 1;
    } else {
        out = item.first;
    }
    return true;
}

} // namespace wubi
