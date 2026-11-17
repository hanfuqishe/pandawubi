    // —— Z 待定：按空格才上屏，Esc 取消，其它键放弃重复并继续处理 ——
    if (m_repeatPending) {
        if (vk == VK_SPACE || vk == VK_RETURN) {
            m_repeatPending = false;
            const std::wstring text = m_repeatText;
            m_repeatText.clear();
            CommitText(pic, text);
            *pfEaten = TRUE;
            return S_OK;
        }
        if (vk == VK_ESCAPE) {
            m_repeatPending = false;
            m_repeatText.clear();
            if (m_comp) CancelComposition(pic);
            m_candWnd.Hide();
            *pfEaten = TRUE;
            return S_OK;
        }
        m_repeatPending = false;
        m_repeatText.clear();
        if (m_comp) CancelComposition(pic);
        m_candWnd.Hide();
    }

    // —— 分号便捷输入：数字转三种中文形式 ——
    if (m_quickMode) {
        if (vk >= '0' && vk <= '9') {
            if (m_quickDigits.size() < 12) m_quickDigits.push_back((char)vk);
            const std::wstring shown = AsciiToWide(m_quickDigits);
            if (m_comp) UpdateCompositionTextWith(pic, shown);
            else        StartComposition(pic, shown);
            RefreshQuickCandidates(pic);
            *pfEaten = TRUE;
            return S_OK;
        }
        if (vk == VK_BACK) {
            if (m_quickDigits.empty()) { ExitQuick(pic, true); *pfEaten = TRUE; return S_OK; }
            m_quickDigits.pop_back();
            if (m_quickDigits.empty()) { ExitQuick(pic, true); *pfEaten = TRUE; return S_OK; }
            UpdateCompositionTextWith(pic, AsciiToWide(m_quickDigits));
            RefreshQuickCandidates(pic);
            *pfEaten = TRUE;
            return S_OK;
        }
        if (vk == VK_ESCAPE) { ExitQuick(pic, true); *pfEaten = TRUE; return S_OK; }
        if (vk == VK_SPACE || vk == VK_RETURN || vk == VK_OEM_1 || vk == VK_OEM_7) {
            const size_t idx = (vk == VK_OEM_1) ? 1u : ((vk == VK_OEM_7) ? 2u : 0u);
            wubi::NumberForms(m_quickDigits, m_quickForms);
            if (idx < m_quickForms.size()) {
                const std::wstring out = m_quickForms[idx];
                const bool hadComp = (m_comp != nullptr);
                m_quickMode = false;
                m_quickDigits.clear();
                m_quickForms.clear();
                if (hadComp) CommitText(pic, out);
                else         CommitString(pic, out);
            } else {
                ExitQuick(pic, true);
            }
            *pfEaten = TRUE;
            return S_OK;
        }
        // 其它键：退出便捷输入，继续按正常流程处理这个键
        ExitQuick(pic, true);
    }

