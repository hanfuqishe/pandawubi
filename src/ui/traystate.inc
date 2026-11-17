// ---------------------------------------------------------------- 中/英状态
// 由 IME（wubi_tsf.dll）通过 WM_APP+10 通知：wParam=0 中文 / 1 英文
static bool  g_english = false;
static HICON g_iconZh = nullptr;
static HICON g_iconEn = nullptr;

static std::wstring StateFilePath() {
    wchar_t dir[MAX_PATH] = { 0 };
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, ARRAYSIZE(dir));
    if (n == 0 || n >= ARRAYSIZE(dir)) return std::wstring();
    std::wstring d = std::wstring(dir) + L"\\WubiTsf";
    CreateDirectoryW(d.c_str(), nullptr);
    return d + L"\\ime_state.txt";
}

static void NotifyState(bool english, bool enable) {
    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd = g_wnd;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_TIP;
    nid.hIcon = english ? (g_iconEn ? g_iconEn : g_iconZh)
                        : (g_iconZh ? g_iconZh : g_iconEn);
    wcscpy_s(nid.szTip, english ? L"PP五笔 —— 英文（单击打开设置）"
                                : L"PP五笔 —— 中文（单击打开设置）");
    Shell_NotifyIconW(enable ? NIM_MODIFY : NIM_ADD, &nid);
    TrayLog("state -> %ls", english ? L"EN" : L"ZH");
}

static void LoadStateFromFile() {
    const std::wstring p = StateFilePath();
    if (p.empty()) return;
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, p.c_str(), L"rb") != 0 || !fp) return;
    char buf[16] = { 0 };
    size_t rd = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    if (rd > 0) g_english = (buf[0] == 'e' || buf[0] == 'E');
}
