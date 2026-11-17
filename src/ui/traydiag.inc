// 诊断增强：记录会话/完整性/窗口/重试/系统托盘窗口
static void TrayLogEnv() {
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);

    HANDLE tok = nullptr;
    DWORD integ = 0xFFFFFFFF;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        DWORD sz = 0;
        GetTokenInformation(tok, TokenIntegrityLevel, nullptr, 0, &sz);
        if (sz) {
            TOKEN_MANDATORY_LABEL* tml = (TOKEN_MANDATORY_LABEL*)malloc(sz);
            if (tml && GetTokenInformation(tok, TokenIntegrityLevel, tml, sz, &sz)) {
                BYTE* sid = (BYTE*)tml->Label.Sid;
                DWORD cnt = *GetSidSubAuthorityCount((PSID)sid);
                integ = *GetSidSubAuthority((PSID)sid, cnt - 1);
            }
            free(tml);
        }
        CloseHandle(tok);
    }
    HWND shellTray = FindWindowW(L"Shell_TrayWnd", nullptr);
    HWND shellWnd  = GetShellWindow();
    TrayLog("env session=%lu integrity=0x%lX Shell_TrayWnd=%p ShellWindow=%p explorer=%p",
            session, integ, (void*)shellTray, (void*)shellWnd,
            (void*)FindWindowW(L"Progman", nullptr));
}

static BOOL AddTrayIconWithRetry() {
    if (!g_wnd || !IsWindow(g_wnd)) {
        TrayLog("retry: window invalid hwnd=%p", (void*)g_wnd);
        return FALSE;
    }
    for (int attempt = 1; attempt <= 3; ++attempt) {
        NOTIFYICONDATAW nid = { sizeof(nid) };
        nid.hWnd = g_wnd;
        nid.uID = kTrayId;
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid.uCallbackMessage = WM_TRAY;
        nid.hIcon = g_icon;
        wcsncpy_s(nid.szTip, kTip, _TRUNCATE);

        SetLastError(0);
        const BOOL ok = Shell_NotifyIconW(NIM_ADD, &nid);
        const DWORD err = GetLastError();
        TrayLog("NIM_ADD attempt=%d ok=%d err=%lu cbSize=%u", attempt, (int)ok, err, (unsigned)nid.cbSize);
        if (ok) {
            nid.uVersion = NOTIFYICON_VERSION_4;
            const BOOL v = Shell_NotifyIconW(NIM_SETVERSION, &nid);
            TrayLog("NIM_SETVERSION ok=%d err=%lu", (int)v, GetLastError());
            return TRUE;
        }
        Sleep(300);
    }
    return FALSE;
}
