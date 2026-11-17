// ---------------------------------------------------------------- 降权自愈
// 若本进程被以低完整性启动（沙箱/受限环境），托盘注册和日志写入都会失败。
// 这类环境无法自行提权，因此改为"请 Explorer 帮忙启动"——Explorer 是 medium，
// 由它启动的实例也是 medium，托盘才能正常注册。
static DWORD ParentProcessId(DWORD self) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = { sizeof(pe) };
    DWORD parent = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == self) { parent = pe.th32ParentProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return parent;
}

static std::wstring ProcessNameOf(DWORD pid) {
    if (!pid) return std::wstring();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return std::wstring();
    PROCESSENTRY32W pe = { sizeof(pe) };
    std::wstring name;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == pid) { name = pe.szExeFile; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return name;
}

// 返回 true 表示"已经交给 Explorer 重启，本进程应退出"
static bool RelaunchViaExplorerIfSandboxed() {
    const DWORD integ = QueryIntegrityOf(GetCurrentProcess());
    if (integ != 0x1000) return false;                 // 不是低完整性，正常继续

    const std::wstring parent = ProcessNameOf(ParentProcessId(GetCurrentProcess()));
    if (_wcsicmp(parent.c_str(), L"explorer.exe") == 0) return false;  // 已由 Explorer 启动

    wchar_t exe[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    TrayLog("detected low integrity, relaunch via explorer: parent=%ls", parent.c_str());

    // explorer.exe "<路径>" 会把请求转交给运行中的 Explorer，由它（medium）启动
    HINSTANCE h = ShellExecuteW(nullptr, L"open", L"explorer.exe", exe, nullptr, SW_SHOWNORMAL);
    TrayLog("ShellExecute(explorer) ret=%p err=%lu", (void*)h, GetLastError());
    return true;
}
