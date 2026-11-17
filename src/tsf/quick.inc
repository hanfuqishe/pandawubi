// ---------------------------------------------------------------- Z 重复 / 便捷输入
static std::wstring AsciiToWide(const std::string& s) {
    std::wstring w;
    w.reserve(s.size());
    for (char c : s) w.push_back((wchar_t)(unsigned char)c);
    return w;
}

void CTextService::BeginRepeat() {
    if (m_lastCommit.empty()) return;
    m_repeatText = m_lastCommit;
    m_repeatPending = true;
}

void CTextService::BeginQuick() {
    m_quickMode = true;
    m_quickDigits.clear();
    m_quickForms.clear();
}

void CTextService::RefreshQuickCandidates(ITfContext* pic) {
    (void)pic;
    wubi::NumberForms(m_quickDigits, m_quickForms);
    ShowCandidateList(m_quickForms);
}

void CTextService::ExitQuick(ITfContext* pic, bool cancel) {
    m_quickMode = false;
    m_quickDigits.clear();
    m_quickForms.clear();
    if (cancel) {
        if (m_comp) CancelComposition(pic);
        m_candWnd.Hide();
    }
}

