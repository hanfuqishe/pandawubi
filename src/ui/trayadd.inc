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
        BOOL ok = Shell_NotifyIconW(NIM_ADD, &nid);
        DWORD err = GetLastError();
        TrayLog("NIM_ADD attempt=%d ok=%d err=%lu cbSize=%u", attempt, (int)ok, err, (unsigned)nid.cbSize);

        // 183 = 已存在（多半是上次异常退出留下的残影，或句柄被复用）
        // 先清除残留再重试一次；仍报 183 就视为图标已在托盘上
        if (!ok && err == ERROR_ALREADY_EXISTS) {
            Shell_NotifyIconW(NIM_DELETE, &nid);
            Sleep(150);
            SetLastError(0);
            ok = Shell_NotifyIconW(NIM_ADD, &nid);
            err = GetLastError();
            TrayLog("NIM_ADD after delete ok=%d err=%lu", (int)ok, err);
            if (!ok && err == ERROR_ALREADY_EXISTS) {
                TrayLog("treat ERROR_ALREADY_EXISTS as success");
                ok = TRUE;
            }
        }

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
