// bar-singleton-test —— 悬浮状态栏的「单实例 + 前台显隐」回归测试
//
// 复现的缺陷（真实桌面截图 + EnumWindows 实测）：
//   TSF 为每个线程各建一个 CTextService 实例，同一进程里也常有多个（实测某进程 3 个实例）；
//   旧实现每个实例各 Create 一条 StatusBar 窗口，而切走应用不会触发 Deactivate，
//   于是整机同时挂着 8~10 条「中 简 ，。 ⚙」叠在桌面上。
//
// 断言（进程内计数，忽略别的进程留下的窗口）：
//   [A] 激活前 0 条
//   [B] 激活 1 个实例 → 恰好 1 条
//   [C] 再激活第 2 个实例（另一线程）→ 仍然恰好 1 条   ← 关键回归点
//   [D] 释放第 2 个实例 → 仍然恰好 1 条
//   [E] 全部释放 → 0 条（窗口真的销毁，不是只隐藏）
//   [F] 本进程不在前台时那条栏必须是隐藏的（前台判定，防止各进程各留一条）
//
// 用 DllGetClassObject 直接拿类工厂，不依赖 HKLM 注册，所以能测任意一份构建产物。
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <msctf.h>
#include <cstdio>
#include <vector>

static const CLSID CLSID_WubiTsf =
    { 0x7f3b0c21, 0x9d55, 0x4f0e, { 0xb1, 0xc2, 0x5a, 0x6d, 0x8e, 0x9f, 0x0a, 0x11 } };
static const wchar_t* kBarClass = L"PandaWubiStatusBar";

// 本进程内的悬浮栏窗口数（别的进程的同类窗口不算）
// 注意：注册表里那份已注册的旧 DLL 会被 TSF 顺带激活，它也用同一个窗口类名。
// 所以这里按“窗口过程属于哪一份模块”来认领，只数本测试加载的这份 DLL 建的窗。
static HMODULE g_module = nullptr;

static int CountOwnBars(std::vector<HWND>* out = nullptr) {
    const DWORD me = GetCurrentProcessId();
    int n = 0;
    HWND h = nullptr;
    while ((h = FindWindowExW(nullptr, h, kBarClass, nullptr)) != nullptr) {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid != me) continue;
        HMODULE mod = nullptr;
        FARPROC wndProc = (FARPROC)GetWindowLongPtrW(h, GWLP_WNDPROC);
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCWSTR)wndProc, &mod))
            continue;
        if (mod != g_module) continue;
        ++n;
        if (out) out->push_back(h);
    }
    return n;
}

// 本进程内所有同类窗口（含被顺带激活的已注册旧 DLL），只为打印对比
static int CountAllOwnProcessBars() {
    const DWORD me = GetCurrentProcessId();
    int n = 0;
    HWND h = nullptr;
    while ((h = FindWindowExW(nullptr, h, kBarClass, nullptr)) != nullptr) {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == me) ++n;
    }
    return n;
}

static bool SelfIsForeground() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static int g_fail = 0;
static void Check(const char* name, bool ok, const char* detail) {
    std::printf("  %s  %s  %s\n", ok ? "PASS" : "FAIL", name, detail);
    if (!ok) ++g_fail;
}

// ---- 一个“应用线程”：建自己的 ThreadMgr + 文本服务实例，然后停在那儿等指令
struct Worker {
    HANDLE started = nullptr;
    HANDLE stop = nullptr;
    HANDLE done = nullptr;
    HRESULT hrActivate = E_FAIL;
    int barsAfterActivate = 0;
    bool barVisible = false;
};

typedef HRESULT (STDAPICALLTYPE* FnGetClassObject)(REFCLSID, REFIID, void**);

struct WorkerArg {
    Worker* w;
    IClassFactory* factory;
};

static DWORD WINAPI WorkerProc(LPVOID param) {
    WorkerArg* a = (WorkerArg*)param;
    Worker* w = a->w;
    IClassFactory* factory = a->factory;

    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const CLSID CLSID_TF_ThreadMgr_L =
        { 0x529a9e6b, 0x6587, 0x4f23, { 0xab, 0x9e, 0x9c, 0x7d, 0x68, 0x3e, 0x3c, 0x50 } };

    ITfThreadMgr* tim = nullptr;
    TfClientId tid = 0;
    IUnknown* unk = nullptr;
    ITfTextInputProcessorEx* tip = nullptr;

    HRESULT hr = CoCreateInstance(CLSID_TF_ThreadMgr_L, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ITfThreadMgr, (void**)&tim);
    if (SUCCEEDED(hr) && tim && SUCCEEDED(tim->Activate(&tid))) {
        if (SUCCEEDED(factory->CreateInstance(nullptr, IID_IUnknown, (void**)&unk)) && unk) {
            if (SUCCEEDED(unk->QueryInterface(IID_ITfTextInputProcessorEx, (void**)&tip)) && tip) {
                w->hrActivate = tip->ActivateEx(tim, tid, 0);
            }
        }
    }

    w->barsAfterActivate = CountOwnBars();
    {
        std::vector<HWND> bars;
        CountOwnBars(&bars);
        w->barVisible = !bars.empty() && IsWindowVisible(bars[0]) != FALSE;
    }
    SetEvent(w->started);

    // 保持激活，泵消息（栏的 500ms 自检计时器跑在宿主线程上）
    for (;;) {
        if (WaitForSingleObject(w->stop, 20) == WAIT_OBJECT_0) break;
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (tip) { tip->Deactivate(); tip->Release(); }
    if (unk) unk->Release();
    if (tim) { tim->Deactivate(); tim->Release(); }
    if (SUCCEEDED(hrCo)) CoUninitialize();
    SetEvent(w->done);
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    const wchar_t* dllPath = (argc > 1) ? argv[1] : L"..\\dist\\panda_tsf.dll";
    std::printf("[bar] dll = %ls\n", dllPath);

    HMODULE h = LoadLibraryW(dllPath);
    if (!h) {
        std::printf("[bar] LoadLibrary FAILED, GetLastError=%lu\n", GetLastError());
        return 2;
    }
    g_module = h;
    FnGetClassObject getClassObject = (FnGetClassObject)GetProcAddress(h, "DllGetClassObject");
    if (!getClassObject) { std::printf("[bar] DllGetClassObject not found\n"); return 2; }

    IClassFactory* factory = nullptr;
    HRESULT hr = getClassObject(CLSID_WubiTsf, IID_IClassFactory, (void**)&factory);
    if (FAILED(hr) || !factory) {
        std::printf("[bar] class factory FAILED hr=0x%08lX\n", (unsigned long)hr);
        return 2;
    }

    std::printf("[A] 激活前的栏窗口数 = %d\n", CountOwnBars());
    Check("激活前无栏", CountOwnBars() == 0, "");

    Worker w1, w2;
    w1.started = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    w1.stop    = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    w1.done    = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    w2.started = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    w2.stop    = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    w2.done    = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    WorkerArg a1 = { &w1, factory };
    WorkerArg a2 = { &w2, factory };

    HANDLE t1 = CreateThread(nullptr, 0, WorkerProc, &a1, 0, nullptr);
    WaitForSingleObject(w1.started, 15000);
    std::printf("[B] 1 个实例激活后：本份 DLL 的栏 = %d 条（进程内同类窗口合计 %d 条）activate hr=0x%08lX\n",
                CountOwnBars(), CountAllOwnProcessBars(), (unsigned long)w1.hrActivate);
    Check("单实例激活后有且仅有 1 条", CountOwnBars() == 1, "");

    HANDLE t2 = CreateThread(nullptr, 0, WorkerProc, &a2, 0, nullptr);
    WaitForSingleObject(w2.started, 15000);
    std::printf("[C] 第 2 个实例激活后：本份 DLL 的栏 = %d 条（旧实现这里会变成 2）\n", CountOwnBars());
    Check("两个实例仍只 1 条", CountOwnBars() == 1, "");

    SetEvent(w2.stop);
    WaitForSingleObject(w2.done, 15000);
    std::printf("[D] 释放第 2 个实例后：本份 DLL 的栏 = %d 条\n", CountOwnBars());
    Check("部分释放不误删", CountOwnBars() == 1, "");

    // [F] 前台判定：本进程不在前台时（控制台几乎总是这种情况），栏必须藏着
    {
        const bool selfFg = SelfIsForeground();
        std::vector<HWND> bars;
        CountOwnBars(&bars);
        const bool visible = !bars.empty() && IsWindowVisible(bars[0]) != FALSE;
        std::printf("[F] 本进程在前台 = %s，栏可见 = %s\n", selfFg ? "YES" : "NO", visible ? "YES" : "NO");
        if (!selfFg) Check("非前台时栏隐藏", !visible, "");
        else         std::printf("  SKIP  非前台时栏隐藏  （测试进程恰好在前台，无法判定）\n");
    }

    SetEvent(w1.stop);
    WaitForSingleObject(w1.done, 15000);
    std::printf("[E] 全部释放后，进程内栏窗口数 = %d\n", CountOwnBars());
    Check("全部释放后窗口销毁", CountOwnBars() == 0, "");

    if (t1) CloseHandle(t1);
    if (t2) CloseHandle(t2);
    CloseHandle(w1.started); CloseHandle(w1.stop); CloseHandle(w1.done);
    CloseHandle(w2.started); CloseHandle(w2.stop); CloseHandle(w2.done);
    factory->Release();

    if (g_fail == 0) std::printf("[bar] 全部通过\n");
    else             std::printf("[bar] %d 项失败\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
