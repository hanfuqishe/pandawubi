// tsf-host —— 端到端 TSF 宿主自测
// 建一个带 EDIT 控件的窗口，程序化把我们的输入法设为当前线程的输入法，
// 合成按键序列，然后把控件文本读回来打印。
// 仅向自己的前台窗口发包：发送前校验 GetForegroundWindow()==本窗口，否则中止。
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <msctf.h>
#include <cstdio>
#include <string>
#include <vector>

#ifndef MSFTEDIT_CLASS
#define MSFTEDIT_CLASS L"RICHEDIT50W"
#endif

static const CLSID CLSID_WubiTsf =
    { 0x7f3b0c21, 0x9d55, 0x4f0e, { 0xb1, 0xc2, 0x5a, 0x6d, 0x8e, 0x9f, 0x0a, 0x11 } };
static const GUID GUID_WubiProfile =
    { 0x7f3b0c21, 0x9d55, 0x4f0e, { 0xb1, 0xc2, 0x5a, 0x6d, 0x8e, 0x9f, 0x0a, 0x12 } };
static const CLSID CLSID_TF_ThreadMgr_L =
    { 0x529a9e6b, 0x6587, 0x4f23, { 0xab, 0x9e, 0x9c, 0x7d, 0x68, 0x3e, 0x3c, 0x50 } };

static HWND g_main = nullptr, g_edit = nullptr;
static std::wstring g_script;      // 见 ParseScript 的记号表
static size_t g_pos = 0;
static int g_stage = 0;

// 一个按键步骤：虚拟键 + 需要按住的控制键
struct Step { WORD vk; bool ctrl; bool shift; wchar_t ch; int holdMs; };
static std::vector<Step> g_steps;

// 控制台进程创建窗口后常常拿不到前台：用 AttachThreadInput 强制一次
static void ForceForeground(HWND h) {
    HWND fg = GetForegroundWindow();
    if (fg == h) return;
    // 不再模拟 ALT 来抢前台：那样会惊动 shell（弹菜单/开始菜单），只用 AttachThreadInput\n
    DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD myThread = GetCurrentThreadId();
    if (fgThread && fgThread != myThread) {
        AttachThreadInput(fgThread, myThread, TRUE);
        BringWindowToTop(h);
        SetForegroundWindow(h);
        SetFocus(h);
        AttachThreadInput(fgThread, myThread, FALSE);
    } else {
        BringWindowToTop(h);
        SetForegroundWindow(h);
        SetFocus(h);
    }
}
static int g_holdMs = 0;   // -hold <ms>：脚本跑完后保持窗口存活，便于观察空闲状态条
static WORD g_holdVk = 0;  // "按住"步骤的键：到点由 (timer 3) 松开
static bool g_postMode = false;  // 用 ITfKeystrokeMgr 正规投递（无需前台）
static ITfKeystrokeMgr* g_ksm = nullptr;
static TfClientId g_tid = 0;

// 脚本字符：'a'..'z' 小写字母键；'A'..'Z' 同一个字母键但**按住 Shift**（验大小写用）；
//           ' ' 空格；'!' Esc。
static WORD VkOf(wchar_t ch) {
    switch (ch) {
    case L' ':  return VK_SPACE;
    case L'!':  return VK_ESCAPE;      // 历史约定：脚本里的 '!' 表示 Esc
    case L',':  return VK_OEM_COMMA;
    case L'.':  return VK_OEM_PERIOD;
    case L';':  return VK_OEM_1;
    case L'\'': return VK_OEM_7;
    case L'/':  return VK_OEM_2;
    case L'-':  return VK_OEM_MINUS;
    case L'=':  return VK_OEM_PLUS;
    default:    return (WORD)towupper(ch);
    }
}

static void SendOneRaw(WORD vk, wchar_t ch) {
    if (g_ksm) {
        // 正规路径：先问输入法要不要这个键。
        // 本输入法的 OnTestKeyDown 会按 WantsKey 声明，宿主按声明转给 KeyDown。
        LPARAM lp = 1 | ((LPARAM)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) << 16);
        BOOL eaten = FALSE;
        HRESULT ht = g_ksm->TestKeyDown(vk, lp, &eaten);
        std::printf("    TestKeyDown('%c') hr=0x%08lX eaten=%d\n",
                    (char)ch, (unsigned long)ht, (int)eaten);
        BOOL eaten2 = FALSE;
        HRESULT hk = g_ksm->KeyDown(vk, lp, &eaten2);
        std::printf("    KeyDown   ('%c') hr=0x%08lX eaten=%d\n",
                    (char)ch, (unsigned long)hk, (int)eaten2);
        if (!(SUCCEEDED(hk) && eaten2)) {
            // 输入法不要这个键：回落成普通字符
            PostMessageW(g_edit, WM_CHAR, (WPARAM)ch, 1);
        }
        return;
    }
    if (g_postMode) {
        // 直接投递到本进程自己的控件：不影响任何其它窗口
        LPARAM down = 1 | ((LPARAM)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) << 16);
        LPARAM up   = down | (1LL << 30) | (1LL << 31);
        PostMessageW(g_edit, WM_KEYDOWN, vk, down);
        PostMessageW(g_edit, WM_KEYUP, vk, up);
        return;
    }
    INPUT in[2] = {};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = vk;
    in[1].type = INPUT_KEYBOARD;
    in[1].ki.wVk = vk;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

// 大写字母 = 按住 Shift 再打这个键：输入法是用 GetKeyState(VK_SHIFT) 判大小写的，
// 所以必须真的把 Shift 按下去，光改 lParam 没用。
static void SendOne(wchar_t ch) {
    const bool shift = (ch >= L'A' && ch <= L'Z');
    if (shift) keybd_event(VK_LSHIFT, 0, 0, 0);
    SendOneRaw(VkOf(ch), ch);
    if (shift) keybd_event(VK_LSHIFT, 0, KEYEVENTF_KEYUP, 0);
}

// 一个步骤：按住需要的 Ctrl/Shift，发这个键，再松开。
// 输入法用 GetKeyState 读修饰键状态，所以必须真的按下去。
static void SendStep(const Step& st) {
    if (st.holdMs > 0) {
        // 真实"单击"：按住一段再松。不能在主线程 Sleep —— 那样会把 20ms 一次的
        // 轮询采样(timer)全堵住，采样器只会看到"已经松开"，看不到按下。
        g_holdVk = st.vk;
        keybd_event((BYTE)st.vk, 0, 0, 0);
        SetTimer(g_main, 3, (UINT)st.holdMs, nullptr);
        return;
    }
    if (st.ctrl)  keybd_event(VK_CONTROL, 0, 0, 0);
    if (st.shift) keybd_event(VK_LSHIFT, 0, 0, 0);
    SendOneRaw(st.vk, st.ch);
    if (st.shift) keybd_event(VK_LSHIFT, 0, KEYEVENTF_KEYUP, 0);
    if (st.ctrl)  keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
}

// 脚本记号：
//   a..z        字母键（A..Z = 按住 Shift）
//   ' ' 空格 · ',' '.' ';' '\'' '/' '-' '=' '@' '_' 标点 · '!' Esc
//   [C] Ctrl+=（自定义词组）   [S] Ctrl+Shift+=（自定义短语）
//   [L] ←  [D] Del  [E] 回车  [B] 退格  [X] Esc  [H] 单击左 Shift（切中英）  [J] 单击右 Shift（切简繁）
static void ParseScript(const std::wstring& s) {
    g_steps.clear();
    for (size_t i = 0; i < s.size();) {
        if (s[i] == L'[') {
            const size_t close = s.find(L']', i);
            if (close == i + 2) {
                WORD vk = 0;
                bool ctrl = false, sh = false;
                switch (s[i + 1]) {
                case L'C': vk = VK_OEM_PLUS; ctrl = true; break;
                case L'S': vk = VK_OEM_PLUS; ctrl = true; sh = true; break;
            case L'T': vk = 'T'; ctrl = true; sh = true; break;   // Ctrl+Shift+T 切简繁
                case L'L': vk = VK_LEFT; break;
                case L'R': vk = VK_RIGHT; break;
                case L'D': vk = VK_DELETE; break;
                case L'E': vk = VK_RETURN; break;
                case L'B': vk = VK_BACK; break;
                case L'X': vk = VK_ESCAPE; break;
                case L'H': vk = VK_LSHIFT; break;
                case L'J': vk = VK_RSHIFT; break;   // 右 Shift（切简繁）   // 用 LSHIFT：注入 VK_SHIFT 有时不会作为按键送到 TSF
                case L'h': vk = VK_LSHIFT; break;   // 小写 = 按住 150ms 再松（轮询采样才看得见）
                case L'j': vk = VK_RSHIFT; break;
                case L'F': vk = VK_F2; break;
                case L'Q': vk = VK_OEM_7; sh = true; break;   // Shift+' = 双引号
                default: break;
                }
                if (vk) { g_steps.push_back({ vk, ctrl, sh, L' ',
                                              (s[i + 1] == L'h' || s[i + 1] == L'j') ? 150 : 0 });
                          i = close + 1; continue; }
            }
        }
        const wchar_t ch = s[i];
        WORD vk = VkOf(ch);
        bool sh = (ch >= L'A' && ch <= L'Z');
        if (ch == L'@') { vk = L'2'; sh = true; }        // Shift+2
        else if (ch == L'_') { vk = VK_OEM_MINUS; sh = true; }
        else if (ch == L':') { vk = VK_OEM_1; sh = true; }      // Shift+;
        else if (ch == L'"') { vk = VK_OEM_7; sh = true; }      // Shift+'
        else if (ch == L'?') { vk = VK_OEM_2; sh = true; }      // Shift+/
        else if (ch == L'<') { vk = VK_OEM_COMMA; sh = true; }  // Shift+,
        else if (ch == L'>') { vk = VK_OEM_PERIOD; sh = true; } // Shift+.
        g_steps.push_back({ vk, false, sh, ch, 0 });
        ++i;
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER:
        if (wp == 1) {
            if (g_pos < g_steps.size()) {
                const bool needFg = !g_postMode;
                if (needFg) ForceForeground(g_main);
                if (needFg && GetForegroundWindow() != g_main) {
                    HWND fg = GetForegroundWindow();
                    wchar_t cls[128] = { 0 };
                    if (fg) GetClassNameW(fg, cls, 127);
                    static int tries = 0;
                    ++tries;
                    std::printf("  [retry %d] fg=%p class=%ls\n", tries, (void*)fg, cls);
                    if (tries >= 8) {
                        std::printf("[ABORT] 8 次仍拿不到前台，放弃（不影响其它应用）\n");
                        PostQuitMessage(2);
                    }
                    return 0;
                }
                SetFocus(g_edit);
                SendStep(g_steps[g_pos]);
                std::printf("  sent vk=0x%02X%s%s\n", g_steps[g_pos].vk,
                            g_steps[g_pos].ctrl ? " ctrl" : "", g_steps[g_pos].shift ? " shift" : "");
                ++g_pos;
            } else {
                KillTimer(hwnd, 1);
                SetTimer(hwnd, 2, 400, nullptr);
            }
        } else if (wp == 3) {
            KillTimer(hwnd, 3);
            keybd_event((BYTE)g_holdVk, 0, KEYEVENTF_KEYUP, 0);
            std::printf("  released vk=0x%02X (held)\n", g_holdVk);
        } else if (wp == 2) {
            KillTimer(hwnd, 2);
            wchar_t buf[512] = { 0 };
            GetWindowTextW(g_edit, buf, ARRAYSIZE(buf) - 1);
            int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
            std::string u8((size_t)(n > 0 ? n - 1 : 0), '\0');
            if (n > 1) WideCharToMultiByte(CP_UTF8, 0, buf, -1, &u8[0], n, nullptr, nullptr);
            std::printf("[RESULT] edit text = \"%s\"\n", u8.c_str());
            PostQuitMessage(u8.empty() ? 1 : 0);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int wmain(int argc, wchar_t** argv) {
    // 默认脚本：a a -> 组码轮转；t r n t 空格 -> 上屏「我」
    // 参数：第一个是脚本；带 -send 时改用真实键盘事件（SendInput）而不是 keystroke mgr，
    //       这条才是应用里实际的路径（TSF 在消息循环里分发按键）。
    std::wstring script = L"atrnt ";
    bool useSendInput = false;
    bool postMode = false;
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"-send") == 0) useSendInput = true;
        else if (wcscmp(argv[i], L"-post") == 0) postMode = true;   // 不抢前台、不注入按键
        else if (wcscmp(argv[i], L"-hold") == 0 && i + 1 < argc) g_holdMs = _wtoi(argv[++i]);
        else script = argv[i];
    }
    g_postMode = postMode;
    g_script = script;
    ParseScript(script);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // 先让 RichEdit 建立 TSF 文档，再激活输入法；顺序颠倒会拿不到 document manager
    #define PUMP(ms) { DWORD _e = GetTickCount() + (ms); MSG _m; \
        while (GetTickCount() < _e) { while (PeekMessageW(&_m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&_m); DispatchMessageW(&_m); } Sleep(5); } }

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"WubiTsfHostTest";
    RegisterClassExW(&wc);

    g_main = CreateWindowExW(WS_EX_TOPMOST, L"WubiTsfHostTest", L"wubi tsf host test",
                             WS_OVERLAPPEDWINDOW, 200, 200, 640, 200,
                             nullptr, nullptr, hInst, nullptr);
    if (!g_main) { std::printf("CreateWindow failed %lu\n", GetLastError()); return 1; }
    // 关键：普通 EDIT 不是 TSF 感知控件，GetFocus 的 document manager 为 NULL，
    // 按键不会经过输入法。RichEdit（记事本同款）支持 TSF。
    LoadLibraryW(L"Msftedit.dll");
    g_edit = CreateWindowExW(0, MSFTEDIT_CLASS, L"",
                             WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL,
                             10, 10, 600, 140, g_main, (HMENU)1, hInst, nullptr);
    if (!g_edit) {
        std::printf("[!] RichEdit 创建失败(%lu)，回退普通 EDIT（按键不会经过 TSF）\n", GetLastError());
        g_edit = CreateWindowExW(0, L"EDIT", L"",
                                 WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL,
                                 10, 10, 600, 140, g_main, (HMENU)1, hInst, nullptr);
    } else {
        std::printf("[0] RichEdit 就绪，TSF 文档将随焦点建立\n");
    }
    ShowWindow(g_main, SW_SHOW);
    UpdateWindow(g_main);
    SetForegroundWindow(g_main);
    SetFocus(g_edit);
    PUMP(400);   // 让 RichEdit 建好 TSF 文档

    // 激活本线程的 TSF 线程管理器，并把我们的输入法设为当前 profile
    ITfThreadMgr* tim = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_TF_ThreadMgr_L, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ITfThreadMgr, (void**)&tim);
    TfClientId tid = 0;
    if (SUCCEEDED(hr) && tim) {
        hr = tim->Activate(&tid);
        std::printf("[1] ITfThreadMgr::Activate -> 0x%08lX tid=%u\n", (unsigned long)hr, tid);
    } else {
        std::printf("[1] CoCreate ThreadMgr -> 0x%08lX\n", (unsigned long)hr);
    }

    // 两条激活路径都试：新的 profile manager 与旧的 language profile
    if (tim) {
        HRESULT hk = tim->QueryInterface(IID_ITfKeystrokeMgr, (void**)&g_ksm);
        std::printf("[1b] QI ITfKeystrokeMgr -> 0x%08lX ksm=%p%s\n", (unsigned long)hk, (void*)g_ksm,
                    useSendInput ? "（-send：改走真实键盘事件）" : "");
        g_tid = tid;
        if (useSendInput && g_ksm) { g_ksm->Release(); g_ksm = nullptr; }
    }

    ITfInputProcessorProfileMgr* mgr = nullptr;
    hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                          IID_ITfInputProcessorProfileMgr, (void**)&mgr);
    if (SUCCEEDED(hr) && mgr) {
        HRESULT h2 = mgr->ActivateProfile(TF_PROFILETYPE_INPUTPROCESSOR, 0x0804,
                                          CLSID_WubiTsf, GUID_WubiProfile, nullptr, 0);
        std::printf("[2] ActivateProfile -> 0x%08lX %s\n", (unsigned long)h2,
                    SUCCEEDED(h2) ? "(OK)" : "(FAILED)");
        // 回读当前激活的 profile，确认真的切过去了
        TF_INPUTPROCESSORPROFILE active = {};
        static const GUID GUID_TFCAT_TIP_KEYBOARD_L =
            { 0x34745c63, 0xb2f0, 0x4784, { 0x8b, 0x67, 0x5e, 0x12, 0xc8, 0x70, 0x1a, 0x31 } };
        HRESULT h3 = mgr->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD_L, &active);
        std::printf("[2] GetActiveProfile -> 0x%08lX clsid={%08lX} langid=0x%04X\n",
                    (unsigned long)h3, (unsigned long)active.clsid.Data1, active.langid);
        mgr->Release();
    } else {
        std::printf("[2] CoCreate ProfileMgr -> 0x%08lX\n", (unsigned long)hr);
    }

    ITfInputProcessorProfiles* profs = nullptr;
    hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                          IID_ITfInputProcessorProfiles, (void**)&profs);
    if (SUCCEEDED(hr) && profs) {
        HRESULT h2 = profs->ActivateLanguageProfile(CLSID_WubiTsf, 0x0804, GUID_WubiProfile);
        std::printf("[2b] ActivateLanguageProfile -> 0x%08lX %s\n", (unsigned long)h2,
                    SUCCEEDED(h2) ? "(OK)" : "(FAILED)");
        profs->Release();
    }

    PUMP(300);   // 等输入法激活完成

    // 焦点里有没有 document manager（TSF 是否已接管这个线程的编辑控件）
    if (tim) {
        ITfDocumentMgr* dm = nullptr;
        HRESULT hf = tim->GetFocus(&dm);
        std::printf("[3] ThreadMgr::GetFocus -> 0x%08lX dm=%p\n", (unsigned long)hf, (void*)dm);
        if (dm) dm->Release();
    }

    std::printf("[4] send script：%zu 步\n", g_steps.size());

    // 先把自己弄到前台并让 RichEdit 建立 TSF 文档：GetFocus 拿不到 document manager 时，
    // 按键根本不会进输入法（表现为 eaten=0、文本就是原样打进去的字母）。
    if (!g_postMode) {
        ForceForeground(g_main);
        SetFocus(g_edit);
        PUMP(600);
        ITfDocumentMgr* dm2 = nullptr;
        if (tim && SUCCEEDED(tim->GetFocus(&dm2)) && dm2) {
            std::printf("[4b] 文档已就绪（TSF 会接管按键）\n");
            dm2->Release();
        } else {
            std::printf("[4b] 警告：仍无 document manager，按键将直通控件\n");
        }
    }

    SetTimer(g_main, 1, 500, nullptr);   // 每 0.5s 一个键

    MSG msg;
    int rc = 0;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    rc = (int)msg.wParam;

    if (tim) { tim->Deactivate(); tim->Release(); }
    CoUninitialize();
    return rc;
}
