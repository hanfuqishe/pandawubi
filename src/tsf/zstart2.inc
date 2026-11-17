    // Z 键：进入"重复上次上屏"待定状态（只提示，不写文档；按空格才上屏）
    if (vk == 'Z' && !m_lastCommit.empty()) {
        m_repeatPending = true;
        m_repeatText = m_lastCommit;
        ShowCandidateList(std::vector<std::wstring>{ L"空格重复：" + m_lastCommit });
        LogLine(L"repeat pending: %zu chars (waiting for space)", m_repeatText.size());
        *pfEaten = TRUE;
        return S_OK;
    }
