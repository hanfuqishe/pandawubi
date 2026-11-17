    // —— Z 待定：此时不往文档里写任何文字，只提示"按空格重复" ——
    if (m_repeatPending) {
        if (vk == VK_SPACE || vk == VK_RETURN) {
            m_repeatPending = false;
            const std::wstring text = m_repeatText;
            m_repeatText.clear();
            CommitString(pic, text);          // 此刻才真正写入文档
            *pfEaten = TRUE;
            return S_OK;
        }
        if (vk == VK_ESCAPE) {
            m_repeatPending = false;
            m_repeatText.clear();
            m_candWnd.Hide();
            *pfEaten = TRUE;
            return S_OK;
        }
        // 其它键：放弃这次重复，继续按正常流程处理该键
        m_repeatPending = false;
        m_repeatText.clear();
        m_candWnd.Hide();
    }
