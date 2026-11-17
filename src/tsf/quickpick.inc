        if (vk == VK_SPACE || vk == VK_RETURN || vk == VK_OEM_1 || vk == VK_OEM_7 ||
            vk == 'A' || vk == 'B' || vk == 'C') {
            size_t idx = 0;
            if (vk == 'B' || vk == VK_OEM_1)      idx = 1;
            else if (vk == 'C' || vk == VK_OEM_7) idx = 2;
            wubi::NumberForms(m_quickDigits, m_quickForms);
            if (idx < m_quickForms.size()) {
                const std::wstring out = m_quickForms[idx];
                const bool hadComp = (m_comp != nullptr);
                m_quickMode = false;
                m_letterLabels = false;
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
