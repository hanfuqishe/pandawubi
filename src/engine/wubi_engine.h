// wubi_engine.h —— 五笔输入法核心数据层
// 词库: TSV  code <TAB> word <TAB> weight <TAB> source
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wubi {

// 候选值对象：从词库取出后即与词库解除耦合（词库可能重排/卸载）
struct Candidate {
    std::string word;
    std::string code;
    int64_t     weight = 0;
    bool        user = false;
};

struct Entry {
    std::string code;    // 五笔码, 1..4 个 'a'..'y'
    std::string word;    // UTF-8 词条
    int64_t     weight;  // 词频权重, 越大越靠前
    bool        user;    // 是否用户词
};

// 二进制格式（按 code 排序，可 mmap 共享只读映射）
#pragma pack(push, 1)
struct BinaryRec {
    char     code[4];    // 'a'..'y'，不足 4 补 0
    uint8_t  codeLen;    // 1..4
    uint8_t  flags;      // bit0 = user
    uint16_t wordLen;    // UTF-8 字节数
    int64_t  weight;
    uint32_t wordOff;    // 相对 blob 基址
    uint32_t rsvd;
};
struct BinaryHeader {
    char     magic[4];   // 'W','B','D','2'
    uint32_t version;
    uint32_t recCount;
    uint32_t rsvd;
    uint64_t blobSize;
};
#pragma pack(pop)

class Dictionary {
public:
    long long Load(const std::wstring& path);          // TSV
    long long LoadBinary(const std::wstring& path);    // mmap 二进制
    bool      SaveBinary(const std::wstring& path);    // 从已载入的 TSV 导出
    bool      UsingBinary() const { return bin_ != nullptr; }
    void AddUserWord(const std::string& code, const std::string& word, int64_t weight = 0);

    // 按输入码查询候选（完全匹配优先，其次前缀匹配）
    std::vector<Candidate> Lookup(std::string_view input, size_t maxCandidates = 30) const;
    // 仅完全匹配
    std::vector<Candidate> LookupExact(std::string_view code) const;
    bool HasExact(std::string_view code) const;
    // 码前缀是否可能构成更长码（用于判断是否继续吃键）
    bool HasPrefix(std::string_view prefix) const;

    // ---- 用户词（自造词 / 词频自学习）----
    long long LoadUserWords(const std::wstring& path);     // code \t word \t weight
    bool      SaveUserWords(const std::wstring& path) const;
    void      BumpUserWord(const std::string& code, const std::string& word, int64_t delta = 1000000);
    // 候选排序策略：0=码短优先（默认） 1=纯词频优先；用户词始终最优先
    void      SetSortMode(int mode) { sortMode_ = mode; sortedDirty_ = true; }
    int       SortMode() const { return sortMode_; }
    // 输入多键时隐藏一级简码字（它们一键就能打出来，再出现在长码候选里是噪声）
    // 一级简码字在多键输入时的处理：0=不处理 1=隐藏 2=沉到候选末尾（默认）
    void      SetLevel1Mode(int m) { level1Mode_ = m; }
    int       Level1Mode() const { return level1Mode_; }
    size_t    UserWordCount() const { return userIndex_.size(); }

    size_t CodeCount() const { return bin_ ? binCount_ : buckets_.size(); }
    size_t EntryCount() const { return bin_ ? binCount_ : entries_.size(); }

    // 单字的「全码」（最长码），用于给自定义词组取码。
    // 首次调用时扫一遍词库建表（147k 条，几毫秒），之后走哈希。
    std::string FullCodeOfChar(const std::string& utf8Char) const;

private:
    void IndexOne(Entry&& e);
    void BuildSortedIndex() const;
    void BuildCharCodes() const;
    mutable std::unordered_map<std::string, std::string> charCodes_;
    mutable bool charCodesBuilt_ = false;

    std::vector<Entry> entries_;
    std::unordered_map<std::string, std::vector<uint32_t>> buckets_;  // 完全匹配
    mutable std::vector<uint32_t> sorted_;                             // 按 code 升序的下标
    mutable bool sortedDirty_ = true;
    int sortMode_ = 0;
    int level1Mode_ = 2;
    mutable std::unordered_set<std::string> level1_;
    mutable bool level1Built_ = false;
    bool BetterCand(const Candidate& a, const Candidate& b) const;
    void EnsureLevel1() const;
    bool IsLevel1Noise(const Candidate& c, size_t inputLen) const;
    // 1/2/3 码输入时，该码的简码字（精确匹配）必须排第一；
    // 词频/用户词只在四码时参与竞争。
    void EnsureExactFirst(std::string_view input, std::vector<Candidate>& out) const;
    // 单键输入时，第 2 位放"该键重复四次"对应的字（QQ五笔 的行为：k -> 中 / 口(kkkk)）
    void InsertKeyRepeat(std::string_view input, std::vector<Candidate>& out, size_t maxCandidates) const;
    std::unordered_map<std::string, uint32_t> userIndex_;              // "code|word" -> entries_ 下标

    // 二进制模式
    void*        hFile_ = nullptr;
    void*        hMap_  = nullptr;
    const uint8_t* view_ = nullptr;
    size_t       viewSize_ = 0;
    const BinaryRec* bin_ = nullptr;
    const char*  binBlob_ = nullptr;
    size_t       binCount_ = 0;
    void UnmapBinary();
    // 二进制模式下按码定位区间：返回 [lo, hi)
    bool BinRange(std::string_view prefix, size_t& lo, size_t& hi) const;
    // 二进制模式下用户词不在映射区，需要单独并入候选
    void CollectUserCandidates(std::string_view code, std::vector<Candidate>& out) const;

public:
    // 命中统计（供 CLI/自检使用）
    struct Stats { size_t exactHits = 0; size_t prefixHits = 0; };
    mutable Stats stats_;
};

// ---------------------------------------------------------------- 简繁转换
// 数据: QQ五笔 simp2trad.dat —— 65,536 个 uint16 的逐码元映射表
class Transliteration {
public:
    bool Load(const std::wstring& path);
    bool Loaded() const { return loaded_; }
    std::wstring ToTraditional(std::wstring_view s) const;
    std::wstring ToSimplified(std::wstring_view s) const { return std::wstring(s); }  // 预留
private:
    std::vector<uint16_t> map_;   // 65536 项；空表表示恒等
    std::vector<uint16_t> rev_;   // 繁 -> 简（由正向表反推第一匹配）
    bool loaded_ = false;
};

// ---------------------------------------------------------------- 标点矩阵
// QQ五笔 punc.ini 语义: 4 列 = 英文半角 / 中文半角 / 英文全角 / 中文全角
// 每项含 count(=1 或 2 个码元) 与输出串；引号类有“第一次/第二次”两套
enum class PuncMode { AsciiHalf = 0, ChineseHalf = 1, AsciiFull = 2, ChineseFull = 3 };

// 数字之后的下一个逗号/句号输出半角（单次）。
// 该"待定"状态下按 Esc 取消它，并且 Esc 本身应被拦截、不发给应用。
struct DigitPunctGate {
    bool pending = false;

    // 每次按键后调用：数字 -> 进入待定；其它非逗号/句号键 -> 退出待定
    void NoteKey(bool isDigit, bool isPunctKey) {
        if (isDigit) pending = true;
        else if (!isPunctKey) pending = false;
    }
    // 待定时消费掉：返回 true 表示本次逗号/句号应输出半角
    bool TakeHalfWidth() {
        if (!pending) return false;
        pending = false;
        return true;
    }
    // 待定时消费掉：返回 true 表示本次 Esc 应被拦截
    bool TakeEsc() {
        if (!pending) return false;
        pending = false;
        return true;
    }
};
// 阿拉伯数字 -> 三种中文形式
//   [0] 逐位直读：123 -> 一二三
//   [1] 带进位读：123 -> 一百二十三
//   [2] 大写财务：123 -> 壹佰贰拾叁
void NumberForms(const std::string& digits, std::vector<std::wstring>& forms);

// 日期/时间/星期的候选（kind: 0=date 1=time 2=week；weekday: 0=周日）
void DateTimeCandidates(int kind, int year, int month, int day,
                        int hour, int minute, int second, int weekday,
                        std::vector<std::string>& out);

class Punctuation {
public:
    // 从 punc.ini 载入；失败时使用内置表
    bool Load(const std::wstring& path);
    // 返回 true 表示该键被标点表接管
    bool Map(char asciiKey, PuncMode mode, std::wstring& out);
    void ResetToggle() { toggle_ = 0; }
private:
    struct Item { std::wstring first; std::wstring second; };  // 无二段时 second 为空
    struct Row { Item col[4]; };
    std::unordered_map<int, Row> rows_;
    int toggle_ = 0;
    void BuildDefault();
};

// 工具
bool IsCodeChar(char c);
size_t Utf8CharLen(const std::string& s, size_t i);

// 按 UTF-8 拆成单字
std::vector<std::string> Utf8Chars(const std::string& s);

// 用"文档里光标前实际还留着的文本"清洗最近上屏的字队列（每个元素是一个字）：
// 从后往前做子序列匹配——对得上的留下，对不上的就是已经被用户删掉的，丢弃。
// 返回被丢弃的字数；调用方在 kept 为空（一个字都对不上，多半是换了文档）时应放弃清洗。
size_t TrimCharsByText(std::vector<std::wstring>& history, const std::wstring& tail);

// 自定义词组取码（五笔标准规则，用每个字的全码）：
//   2 字词：各取前两码                    你好 -> wq vb
//   3 字词：前两字各首码 + 第三字前两码    计算机 -> y t sm
//   4 字词：各取首码                      中华人民共和国(多字) 见下
//   5 字以上：前三字首码 + 末字首码        中华人民共和国 -> k w w l
// 任一字查不到全码、或不足 2 字时返回空串。
std::string PhraseCode(const std::string& utf8Word, const Dictionary& dict);

} // namespace wubi
