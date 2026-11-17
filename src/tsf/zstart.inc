    // 分号：进入便捷输入（不是直接出分号）
    if (vk == VK_OEM_1) {
        BeginQuick();
        LogLine(L"quick input: on");
        *pfEaten = TRUE;
        return S_OK;
    }

    // Z 键：进入"重复上次上屏"待定状态（按空格才上屏）
    if (vk == 'Z' && !m_lastCommit.empty()) {
        BeginRepeat();
        if (SUCCEEDED(StartComposition(pic, m_repeatText))) {
            ShowCandidateList(std::vector<std::wstring>{ m_repeatText });
        } else {
            m_repeatPending = false;
            m_repeatText.clear();
        }
        LogLine(L"repeat pending: %zu chars", m_repeatText.size());
        *pfEaten = TRUE;
        return S_OK;
    }

