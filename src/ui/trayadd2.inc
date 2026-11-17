static DWORD QueryIntegrityOf(HANDLE hProcess) {
    HANDLE tok = nullptr;
    DWORD integ = 0xFFFFFFFF;
    if (!hProcess || !OpenProcessToken(hProcess, TOKEN_QUERY, &tok)) return integ;
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
    return integ;
}

static void AddTrayIcon() {
    if (AddTrayIconWithRetry()) return;

    const DWORD err = GetLastError();
    wchar_t exeP[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, exeP, ARRAYSIZE(exeP));

    const DWORD selfInt = QueryIntegrityOf(GetCurrentProcess());
    HWND shell = GetShellWindow();
    DWORD spid = 0;
    if (shell) GetWindowThreadProcessId(shell, &spid);
    HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, spid);
    const DWORD shInt = ph ? QueryIntegrityOf(ph) : 0;
    if (ph) CloseHandle(ph);

    wchar_t logP[MAX_PATH + 32] = { 0 };
    wcscpy_s(logP, exeP);
    wchar_t* slash = wcsrchr(logP, L'\\');
    if (slash) wcscpy_s(slash + 1, 32, L"tray.log");

    wchar_t msg[900];
    swprintf_s(msg,
        L"托盘图标注册失败\n\n"
        L"错误码：%lu\n"
        L"本进程完整性：0x%lX\n"
        L"资源管理器完整性：0x%lX\n"
        L"程序路径：%ls\n"
        L"日志文件：%ls",
        err, selfInt, shInt, exeP, logP);
    MessageBoxW(nullptr, msg, L"PP五笔", MB_OK | MB_ICONWARNING);
}
