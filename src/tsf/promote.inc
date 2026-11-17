    // Ctrl+数字：把第 N 个候选提到首位（仅四码时生效），并写入用户词固化
    if (ctrl && !alt && !shift && vk >= '1' && vk <= '9' &&
        m_comp && m_composer.Code().size() == 4) {
        auto o = m_composer.Promote((int)(vk - '0'));
        if (o.act == wubi::Composer::Act::Updated) {
            SyncCodeFromComposer();
            UpdateCompositionText(pic);
            RefreshCandidates(pic);
            g_dict.Get().SaveUserWords(GetUserWordsPath());
            LogLine(L"promote candidate %d for code %hs", (int)(vk - '0'), m_composer.Code().c_str());
        }
        *pfEaten = TRUE;
        return S_OK;
    }

