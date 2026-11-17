        // 其它符号（非 ; ' , . 与数字）：先把第 1 候选上屏，再输出这个符号。
        // 这样"打码后直接打标点"能得到"候选+标点"，而不是让标点漏给应用把组合串杀掉。
        {
            BYTE ksSym[256];
            GetKeyboardState(ksSym);
            wchar_t symChar = 0;
            if (ToUnicode(vk, (UINT)lParam, ksSym, &symChar, 1, 0) == 1 &&
                symChar > 0x20 && symChar < 0x7F) {
                handle(m_composer.SelectBest());   // 提交 #1 并结束组合串
                LoadDataOnce();
                std::wstring outSym;
                if (m_puncEnabled && m_punc.Map((char)symChar, wubi::PuncMode::ChineseHalf, outSym)) {
                    CommitString(pic, outSym);
                } else {
                    CommitString(pic, std::wstring(1, symChar));
                }
                *pfEaten = TRUE;
                return S_OK;
            }
        }

