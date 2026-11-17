// ---------------------------------------------------------------- 英文字母补全
// 数据：data\eng_words.txt（48k 词，取自 QQ五笔 engcore.dat 的 NEPQ 词表）
//
// 注意：这些函数已经是 src/tsf/wubi_tsf.cpp 的一部分（本文件不被任何 TU 包含，
// 只是同逻辑的参考副本，方便单独阅读）。改动请以 wubi_tsf.cpp 为准，否则两边会讲不通。
//
// 大小写：m_engTyped 保存**实际输入**的字母（Shift/CapsLock 决定的真实大小写），
// 查词用全小写前缀 EngPrefix()，候选与上屏用 wubi::ApplyCase() 套回大小写。

std::string CTextService::EngPrefix() const {
    std::string s;
    s.reserve(m_engTyped.size());
    for (wchar_t c : m_engTyped) {
        s.push_back((c >= L'A' && c <= L'Z') ? (char)(c - L'A' + 'a') : (char)c);
    }
    return s;
}

void CTextService::SetEnglish(bool on) {
    m_english = on;
    SetInputModeCompartments(!on);
    FlashStatus(on ? L"\u82f1" : L"\u4e2d");
    LogLine(L"SetEnglish=%d", (int)on);
}

void CTextService::EngUpdate(ITfContext* pic) {
    if (m_engTyped.empty()) { EngCancel(pic); return; }
    // 组码预览显示用户实际打出来的大小写
    if (m_comp) UpdateCompositionTextWith(pic, m_engTyped);
    else        StartComposition(pic, m_engTyped);

    const auto words = m_eng.Suggest(EngPrefix(), (size_t)m_candCount);
    std::vector<std::wstring> disp;
    disp.reserve(words.size());
    for (const auto& w : words) disp.push_back(wubi::ApplyCase(AsciiToWide(w), m_engTyped));
    m_candWnd.SetPlain(false);
    if (!disp.empty()) {
        m_candWnd.SetItems(disp, m_settings.CandVertical != 0);
        m_candWnd.ShowAt(m_lastCaret);
    } else {
        m_candWnd.Hide();
    }
    LogLine(L"eng type %s -> %zu cands", m_engTyped.c_str(), disp.size());
}

void CTextService::EngType(ITfContext* pic, wchar_t ch) {
    if (m_engTyped.size() >= 32) EngCommit(pic, 0, false);
    m_engTyped.push_back(ch);
    EngUpdate(pic);
}

void CTextService::EngCommit(ITfContext* pic, int idx, bool withSpace) {
    const auto words = m_eng.Suggest(EngPrefix(), (size_t)m_candCount);
    std::wstring out;
    if (idx >= 0 && idx < (int)words.size()) out = wubi::ApplyCase(AsciiToWide(words[(size_t)idx]), m_engTyped);
    else                                     out = m_engTyped;

    m_engTyped.clear();
    m_engActive = false;
    if (m_comp) CancelComposition(pic);
    m_candWnd.Hide();
    if (withSpace) out += L' ';
    CommitString(pic, out);
    LogLine(L"eng commit idx=%d len=%zu out=%s", idx, out.size(), out.c_str());
}

void CTextService::EngCancel(ITfContext* pic) {
    m_engTyped.clear();
    m_engActive = false;
    if (m_comp) CancelComposition(pic);
    m_candWnd.Hide();
}
