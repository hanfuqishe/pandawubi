#include "wubi_composer.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace wubi {

static std::wstring ToWide(const std::string& u8) {
    int n = MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), nullptr, 0);
    std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), &w[0], n);
    return w;
}

void Composer::Reset() {
    code_.clear();
    cands_.clear();
    all_.clear();
    page_ = 0;
}

void Composer::SlicePage() {
    cands_.clear();
    if (all_.empty() || pageSize_ == 0) return;
    const size_t start = page_ * pageSize_;
    if (start >= all_.size()) return;
    const size_t end = (start + pageSize_ < all_.size()) ? start + pageSize_ : all_.size();
    cands_.assign(all_.begin() + (std::ptrdiff_t)start, all_.begin() + (std::ptrdiff_t)end);
}

Composer::Outcome Composer::Refresh() {
    Outcome o;
    o.act = Act::Updated;
    cands_.clear();
    all_.clear();
    page_ = 0;
    if (code_.empty()) { o.act = Act::Cancelled; return o; }
    // 特殊码（date/time/week）优先
    if (special_) {
        std::vector<Candidate> sp;
        if (special_(code_, sp) && !sp.empty()) all_ = std::move(sp);
    }
    if (all_.empty() && dict_) {
        // 多取几页，供翻页使用
        all_ = dict_->Lookup(code_, limit_ * 4);
    }
    SlicePage();
    o.candCount = cands_.size();
    return o;
}

Composer::Outcome Composer::TypeCode(char c) {
    Outcome o;
    if (!IsCodeChar(c)) { o.act = Act::Pass; return o; }

    // 四个码已满还要继续输入：五笔的习惯是"顶字上屏"——
    // 把当前第 1 候选直接上屏，然后用这个键开始新的一轮组码。
    if (code_.size() >= 4) {
        if (cands_.empty()) { o.act = Act::Pass; return o; }   // 没有候选就不接管
        o.act = Act::CommitThenContinue;
        o.commit = ToWide(cands_[0].word);
        o.commitCode = code_;            // 记下旧码（Reset 之前）
        Reset();
        code_.push_back(c);
        Refresh();
        o.candCount = cands_.size();
        return o;
    }

    code_.push_back(c);
    return Refresh();
}

Composer::Outcome Composer::SelectDigit(int digit1to9) {
    Outcome o;
    if (code_.empty()) { o.act = Act::Pass; return o; }
    const size_t idx = (size_t)(digit1to9 - 1);
    if (idx >= cands_.size()) { o.act = Act::Pass; return o; }
    o.act = Act::Committed;
    o.commit = ToWide(cands_[idx].word);
    o.commitCode = code_;
    o.candCount = cands_.size();
    Reset();
    return o;
}

Composer::Outcome Composer::SelectBest() {
    Outcome o;
    if (code_.empty()) { o.act = Act::Pass; return o; }
    if (cands_.empty()) { o.act = Act::Pass; return o; }
    o.act = Act::Committed;
    o.commit = ToWide(cands_[0].word);
    o.commitCode = code_;
    o.candCount = cands_.size();
    Reset();
    return o;
}

Composer::Outcome Composer::Backspace() {
    Outcome o;
    if (code_.empty()) { o.act = Act::Pass; return o; }
    code_.pop_back();
    return Refresh();           // 变空时返回 Cancelled
}

Composer::Outcome Composer::Promote(int digit1to9) {
    Outcome o;
    if (digit1to9 < 1) { o.act = Act::Pass; return o; }
    const size_t idx = (size_t)(digit1to9 - 1);
    if (code_.empty() || idx >= cands_.size() || !dict_) { o.act = Act::Pass; return o; }

    const std::string word = cands_[idx].word;
    int64_t top = 0;
    for (const auto& c : all_) if (c.weight > top) top = c.weight;
    // 写成用户词并给到比现有最高权重还高的值 -> 下一次查询它就在首位
    dict_->AddUserWord(code_, word, top + 1);

    Refresh();
    o.act = Act::Updated;
    o.candCount = cands_.size();
    return o;
}
Composer::Outcome Composer::NextPage() {
    Outcome o;
    if (code_.empty() || all_.empty()) { o.act = Act::Pass; return o; }
    if ((page_ + 1) * pageSize_ >= all_.size()) { o.act = Act::Pass; return o; }
    ++page_;
    SlicePage();
    o.act = Act::Updated;
    o.candCount = cands_.size();
    return o;
}

Composer::Outcome Composer::PrevPage() {
    Outcome o;
    if (code_.empty() || page_ == 0) { o.act = Act::Pass; return o; }
    --page_;
    SlicePage();
    o.act = Act::Updated;
    o.candCount = cands_.size();
    return o;
}

Composer::Outcome Composer::Escape() {
    Outcome o;
    const bool was = !code_.empty();
    Reset();
    o.act = was ? Act::Cancelled : Act::Pass;
    return o;
}

} // namespace wubi
