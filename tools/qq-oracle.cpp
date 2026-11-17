// qq-oracle —— 直接驱动 QQ五笔 的 IMM 引擎（QQWubi.ime）取候选顺序
// 思路：IMM 路径不需要 TSF、不需要前台、不需要用户操作。
//   1) LoadLibrary QQWubi.ime，取 ImeInquire / ImeSelect / ImeProcessKey / ImeToAsciiEx
//   2) ImmCreateContext 造一个 HIMC
//   3) ImeInquire 初始化，ImeSelect(himc, TRUE) 选中
//   4) 逐键 ImeProcessKey + ImeToAsciiEx
//   5) ImmGetCandidateListW 读标准候选列表
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <imm.h>
#include <cstdio>
#include <string>

// IMEINFO / TRANSMSG 系列在 IME DDK 的 ime.h 里，SDK 的 imm.h 未提供，这里自行声明
typedef struct tagIMEINFO_ {
    DWORD dwPrivateDataSize;
    DWORD fdwProperty;
    DWORD fdwConversionCaps;
    DWORD fdwSentenceCaps;
    DWORD fdwUICaps;
    DWORD fdwSCSCaps;
    DWORD fdwSelectCaps;
} IMEINFO_L, *LPIMEINFO_L;

typedef struct tagTRANSMSG_ { UINT message; WPARAM wParam; LPARAM lParam; } TRANSMSG_L;
typedef struct tagTRANSMSGLIST_ { UINT UMsgCount; TRANSMSG_L TransMsg[64]; } TRANSMSGLIST_L, *LPTRANSMSGLIST_L;

typedef BOOL (WINAPI* PFN_ImeInquire)(LPIMEINFO_L, LPWSTR, DWORD);
typedef BOOL (WINAPI* PFN_ImeSelect)(HIMC, BOOL);
typedef BOOL (WINAPI* PFN_ImeProcessKey)(HIMC, UINT, LPARAM, CONST LPBYTE);
typedef UINT (WINAPI* PFN_ImeToAsciiEx)(UINT, UINT, CONST LPBYTE, LPTRANSMSGLIST_L, UINT, HIMC);

static wchar_t g_uiClass[64] = { 0 };

static void DumpCandidates(HIMC hic, const char* tag) {
    DWORD need = ImmGetCandidateListW(hic, 0, nullptr, 0);
    if (need == 0) { std::printf("  [%s] ImmGetCandidateListW: 无候选 (need=0)\n", tag); return; }
    std::printf("  [%s] 候选列表字节数=%lu\n", tag, (unsigned long)need);
    CANDIDATELIST* cl = (CANDIDATELIST*)malloc(need);
    if (!cl) return;
    DWORD got = ImmGetCandidateListW(hic, 0, cl, need);
    if (got == 0) { std::printf("  [%s] 读取失败\n", tag); free(cl); return; }
    std::printf("  [%s] dwCount=%lu dwSelection=%lu\n", tag,
                (unsigned long)cl->dwCount, (unsigned long)cl->dwSelection);
    for (DWORD i = 0; i < cl->dwCount && i < 12; ++i) {
        const wchar_t* w = (const wchar_t*)((const BYTE*)cl + cl->dwOffset[i]);
        char u8[256] = { 0 };
        WideCharToMultiByte(CP_UTF8, 0, w, -1, u8, sizeof(u8) - 1, nullptr, nullptr);
        std::printf("     %lu. %s\n", (unsigned long)(i + 1), u8);
    }
    free(cl);
}

// 对一个码：新建 HIMC -> 输入 -> 返回候选（逗号分隔）
static bool QueryOne(HMODULE h, PFN_ImeSelect pSelect, PFN_ImeProcessKey pProcKey,
                     PFN_ImeToAsciiEx pToAscii, const std::wstring& code, std::string& out, bool verbose) {
    HIMC hic = ImmCreateContext();
    if (!hic) return false;
    // IME 需要一个与 ImeInquire 指定类名一致的 UI 窗口，并把 HIMC 关联上去
    HWND hwnd = nullptr;
    if (g_uiClass[0]) {
        WNDCLASSEXW wc2 = { sizeof(wc2) };
        wc2.lpfnWndProc = DefWindowProcW;
        wc2.hInstance = GetModuleHandleW(nullptr);
        wc2.lpszClassName = g_uiClass;
        RegisterClassExW(&wc2);
        hwnd = CreateWindowExW(0, g_uiClass, L"", WS_OVERLAPPED, 0, 0, 10, 10,
                               nullptr, nullptr, wc2.hInstance, nullptr);
        if (hwnd) ImmAssociateContext(hwnd, hic);
    }
    pSelect(hic, TRUE);
    ImmSetOpenStatus(hic, TRUE);

    BYTE ks[256] = { 0 };
    TRANSMSGLIST_L msgs = {};
    for (wchar_t c : code) {
        const UINT vk = (UINT)towupper(c);
        const UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        BYTE kst[256]; memcpy(kst, ks, sizeof(kst)); kst[vk] = 0x80;
        pProcKey(hic, vk, 1 | ((LPARAM)sc << 16), kst);
        kst[vk] = 0;
        pToAscii(vk, sc, kst, &msgs, 0, hic);
    }

    DWORD need = ImmGetCandidateListW(hic, 0, nullptr, 0);
    std::string line;
    if (need > 0) {
        CANDIDATELIST* cl = (CANDIDATELIST*)malloc(need);
        if (cl && ImmGetCandidateListW(hic, 0, cl, need) > 0) {
            for (DWORD i = 0; i < cl->dwCount && i < 9; ++i) {
                const wchar_t* w = (const wchar_t*)((const BYTE*)cl + cl->dwOffset[i]);
                char u8[256] = { 0 };
                WideCharToMultiByte(CP_UTF8, 0, w, -1, u8, sizeof(u8) - 1, nullptr, nullptr);
                // QQ 的候选串可能带尾随码片段，截到第一个非汉字之外的 ASCII 前
                std::string s(u8);
                // QQ 会把"剩余码"附在候选串尾部（如 "或k"），去掉尾部 ASCII 字母
                while (!s.empty() && s.back() >= 0x20 && s.back() < 0x7F) s.pop_back();
                if (verbose) std::printf("     [%lu] len=%zu '%s'\n", (unsigned long)i, wcslen(w), u8);
                if (!line.empty()) line += ',';
                line += s;
            }
        }
        free(cl);
    }
    pSelect(hic, FALSE);
    if (hwnd) { ImmAssociateContext(hwnd, nullptr); DestroyWindow(hwnd); }
    ImmDestroyContext(hic);
    out = line;
    return !line.empty();
}

int wmain(int argc, wchar_t** argv) {
    const wchar_t* imePath = (argc > 1) ? argv[1] : L"C:\\WINDOWS\\system32\\IME\\QQWubiTSF\\QQWubi.ime";
    const wchar_t* keys = (argc > 2) ? argv[2] : L"a";

    HMODULE h = LoadLibraryW(imePath);
    if (!h) { std::printf("LoadLibrary failed: %lu\n", GetLastError()); return 1; }

    auto pInquire = (PFN_ImeInquire)GetProcAddress(h, "ImeInquire");
    auto pSelect  = (PFN_ImeSelect)GetProcAddress(h, "ImeSelect");
    auto pProcKey = (PFN_ImeProcessKey)GetProcAddress(h, "ImeProcessKey");
    auto pToAscii = (PFN_ImeToAsciiEx)GetProcAddress(h, "ImeToAsciiEx");
    if (!pInquire || !pSelect || !pProcKey || !pToAscii) {
        std::printf("导出缺失: Inquire=%p Select=%p ProcessKey=%p ToAsciiEx=%p\n",
                    (void*)pInquire, (void*)pSelect, (void*)pProcKey, (void*)pToAscii);
        return 1;
    }

    // 批量模式：argv[2] 形如 "a,trnt,wqvb"，每码一行 TSV
    {
        std::wstring keyArg(keys);
        if (keyArg.find(L',') != std::wstring::npos) {
            IMEINFO_L tmp = {};
            pInquire(&tmp, g_uiClass, 0);
            size_t pos = 0;
            while (pos <= keyArg.size()) {
                size_t c = keyArg.find(L',', pos);
                std::wstring code = keyArg.substr(pos, (c == std::wstring::npos ? keyArg.size() : c) - pos);
                pos = (c == std::wstring::npos) ? keyArg.size() + 1 : c + 1;
                if (code.empty()) continue;
                std::string words;
                QueryOne(h, pSelect, pProcKey, pToAscii, code, words, true);
                char codeU8[64] = { 0 };
                WideCharToMultiByte(CP_UTF8, 0, code.c_str(), (int)code.size(), codeU8, sizeof(codeU8) - 1, nullptr, nullptr);
                std::printf("%s\t%s\n", codeU8, words.c_str());
            }
            return 0;
        }
    }
    IMEINFO_L info = {};
    wchar_t uiClass[64] = { 0 };
    BOOL okInquire = pInquire(&info, uiClass, 0);
    std::printf("[1] ImeInquire -> %d  uiClass='%ls'\n", (int)okInquire, uiClass);
    std::printf("    fdwProperty=0x%08lX fdwConversionCaps=0x%08lX fdwUICaps=0x%08lX\n",
                (unsigned long)info.fdwProperty, (unsigned long)info.fdwConversionCaps,
                (unsigned long)info.fdwUICaps);

    // IMM 要求 ImeInquire 指定的 UI 窗口类存在；这里建一个隐藏窗口挂上去
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = uiClass[0] ? uiClass : L"QQWubiUIWnd";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 10, 10,
                                nullptr, nullptr, wc.hInstance, nullptr);
    std::printf("[2] UI 窗口 %ls -> %p\n", wc.lpszClassName, (void*)hwnd);

    HIMC hic = ImmCreateContext();
    if (!hic) { std::printf("ImmCreateContext failed: %lu\n", GetLastError()); return 1; }
    if (hwnd) ImmAssociateContext(hwnd, hic);
    std::printf("[3] HIMC=%p\n", (void*)hic);

    std::printf("[4] ImeSelect(TRUE) -> %d\n", (int)pSelect(hic, TRUE));
    ImmSetOpenStatus(hic, TRUE);

    BYTE ks[256] = { 0 };
    GetKeyboardState(ks);
    TRANSMSGLIST_L msgs = {};

    for (const wchar_t* p = keys; *p; ++p) {
        if (*p == L' ') continue;
        const UINT vk = (UINT)towupper(*p);
        const UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        BYTE kst[256];
        memcpy(kst, ks, sizeof(kst));
        kst[VK_SHIFT] = 0;
        kst[vk] = 0x80;

        BOOL used = pProcKey(hic, vk, 1 | ((LPARAM)sc << 16), kst);
        kst[vk] = 0;
        UINT n = pToAscii(vk, sc, kst, &msgs, 0, hic);
        wchar_t buf[64] = { 0 };
        LONG cl = ImmGetCompositionStringW(hic, GCS_COMPSTR, buf, sizeof(buf) - 2);
        char u8[128] = { 0 };
        if (cl > 0) WideCharToMultiByte(CP_UTF8, 0, buf, cl / 2, u8, sizeof(u8) - 1, nullptr, nullptr);
        std::printf("[5] key '%c' vk=0x%02X ProcessKey=%d ToAsciiEx=%u  compstr='%s'\n",
                    (char)*p, vk, (int)used, n, u8);
    }

    DumpCandidates(hic, "after keys");
    // 再试一次"空格"（上屏），看候选是否变化
    {
        const UINT vk = VK_SPACE, sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        BYTE kst[256]; memcpy(kst, ks, sizeof(kst)); kst[vk] = 0x80;
        pProcKey(hic, vk, 1 | ((LPARAM)sc << 16), kst);
        kst[vk] = 0;
        pToAscii(vk, sc, kst, &msgs, 0, hic);
        DumpCandidates(hic, "after space");
    }

    pSelect(hic, FALSE);
    ImmDestroyContext(hic);
    if (hwnd) DestroyWindow(hwnd);
    return 0;
}
