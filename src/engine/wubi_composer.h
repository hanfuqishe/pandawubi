// wubi_composer.h —— 组码状态机（纯逻辑，不依赖 TSF/窗口，可独立单测）
#pragma once

#include <cstdint>
#include <string>
#include <functional>
#include <vector>

#include "wubi_engine.h"

namespace wubi {

class Composer {
public:
    enum class Act {
        Pass,        // 不该接管，交给应用
        Updated,     // 组码/候选变化（需要刷新显示）
        Committed,   // 已确定上屏，commit 字段有效
        Cancelled,   // 取消组码
        CommitThenContinue,  // 先把第 1 候选顶上去（commit 有效），再用本键开新组码
    };

    struct Outcome {
        Act          act = Act::Pass;
        std::wstring commit;          // Committed / CommitThenContinue 时有效
        std::string  commitCode;      // 被上屏的词所属的码（用于词频学习）
        size_t       candCount = 0;
    };

    explicit Composer(Dictionary* dict = nullptr) : dict_(dict) {}
    void SetDictionary(Dictionary* d) { dict_ = d; }
    // 特殊码（date/time/week）的候选提供者：返回 true 表示接管本次候选
    using SpecialProvider = std::function<bool(const std::string&, std::vector<Candidate>&)>;
    void SetSpecialProvider(SpecialProvider p) { special_ = std::move(p); }
    void SetLimit(size_t n) { limit_ = n ? n : 9; pageSize_ = limit_; }

    bool Composing() const { return !code_.empty(); }
    const std::string& Code() const { return code_; }
    const std::vector<Candidate>& Candidates() const { return cands_; }

    Outcome TypeCode(char c);          // 'a'..'y'
    Outcome SelectDigit(int digit1to9); // 1..9
    Outcome SelectBest();               // 空格/回车
    Outcome Backspace();
    Outcome Escape();
    Outcome Promote(int digit1to9);   // 把当前页第 N 个候选提到首位（写回用户词固化）
    Outcome NextPage();
    Outcome PrevPage();
    size_t  PageIndex() const { return page_; }
    size_t  PageCount() const { return pageSize_ ? (all_.size() + pageSize_ - 1) / pageSize_ : 0; }
    void    Reset();

private:
    Outcome Refresh();
    Dictionary* dict_ = nullptr;
    std::string code_;
    std::vector<Candidate> cands_;    // 当前页
    std::vector<Candidate> all_;      // 全部候选
    SpecialProvider special_;
    size_t page_ = 0;
    size_t pageSize_ = 9;
    void SlicePage();
    size_t limit_ = 9;
};

} // namespace wubi
