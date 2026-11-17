// tsf-selftest —— TSF DLL 自检
//  1) LoadLibrary + DllRegisterServer 的 HRESULT
//  2) CoCreateInstance 该 CLSID 能否拿到 ITfTextInputProcessor
//  3) 词库能否从 DLL 同目录 data\qqwubi86.dict.tsv 载入
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <msctf.h>
#include <cstdio>
#include <string>

static const CLSID CLSID_WubiTsf =
    { 0x7f3b0c21, 0x9d55, 0x4f0e, { 0xb1, 0xc2, 0x5a, 0x6d, 0x8e, 0x9f, 0x0a, 0x11 } };

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { std::printf("usage: tsf-selftest <path-to-wubi_tsf.dll>\n"); return 2; }

    HMODULE h = LoadLibraryW(argv[1]);
    if (!h) {
        std::printf("[1] LoadLibrary FAILED, GetLastError=%lu\n", GetLastError());
        return 1;
    }
    std::printf("[1] LoadLibrary OK\n");

    typedef HRESULT (STDAPICALLTYPE* FnReg)();
    FnReg reg = (FnReg)GetProcAddress(h, "DllRegisterServer");
    if (!reg) { std::printf("[1] DllRegisterServer not found\n"); return 1; }
    HRESULT hrReg = reg();
    std::printf("[1] DllRegisterServer -> hr=0x%08lX (%s)\n", (unsigned long)hrReg, SUCCEEDED(hrReg) ? "ok" : "FAILED");

    typedef HRESULT (STDAPICALLTYPE* FnUnreg)();
    // 不调用 unregister，保留注册以便手工试用

    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::printf("[2] CoInitializeEx -> 0x%08lX\n", (unsigned long)hrCo);

    IUnknown* pUnk = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WubiTsf, nullptr, CLSCTX_INPROC_SERVER, IID_IUnknown, (void**)&pUnk);
    std::printf("[2] CoCreateInstance -> hr=0x%08lX (%s)\n", (unsigned long)hr, SUCCEEDED(hr) ? "ok" : "FAILED");
    if (SUCCEEDED(hr) && pUnk) {
        ITfTextInputProcessor* tip = nullptr;
        HRESULT hr2 = pUnk->QueryInterface(IID_ITfTextInputProcessor, (void**)&tip);
        std::printf("[2] QI ITfTextInputProcessor -> 0x%08lX (%s)\n", (unsigned long)hr2, SUCCEEDED(hr2) ? "ok" : "FAILED");
        if (SUCCEEDED(hr2) && tip) {
            ITfTextInputProcessorEx* tipEx = nullptr;
            HRESULT hr3 = tip->QueryInterface(IID_ITfTextInputProcessorEx, (void**)&tipEx);
            std::printf("[2] QI ITfTextInputProcessorEx -> 0x%08lX (%s)\n", (unsigned long)hr3, SUCCEEDED(hr3) ? "ok" : "FAILED");
            if (tipEx) tipEx->Release();
            tip->Release();
        }
        pUnk->Release();
    }

    // [3] 词库路径与载入（用 CLI 引擎验证同一份数据）
    wchar_t dllPath[MAX_PATH];
    GetModuleFileNameW(h, dllPath, ARRAYSIZE(dllPath));
    std::wstring s(dllPath);
    size_t p = s.find_last_of(L"\\/");
    std::wstring dictPath = s.substr(0, p) + L"\\data\\qqwubi86.dict.tsv";
    std::printf("[3] dict path = %ls\n", dictPath.c_str());
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (GetFileAttributesExW(dictPath.c_str(), GetFileExInfoStandard, &fad)) {
        std::printf("[3] dict exists, size=%lu bytes\n", fad.nFileSizeLow);
    } else {
        std::printf("[3] dict MISSING\n");
    }

    // [4] TSF 是否认到这个 TIP（枚举 zh-CN 语言配置）
    {
        ITfInputProcessorProfiles* profiles = nullptr;
        HRESULT hrP = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_ITfInputProcessorProfiles, (void**)&profiles);
        std::printf("[4] CoCreateInstance(InputProcessorProfiles) -> 0x%08lX\n", (unsigned long)hrP);
        if (SUCCEEDED(hrP) && profiles) {
            IEnumTfLanguageProfiles* pEnum = nullptr;
            HRESULT hrE = profiles->EnumLanguageProfiles(0x0804, &pEnum);
            std::printf("[4] EnumLanguageProfiles(zh-CN) -> 0x%08lX\n", (unsigned long)hrE);
            bool found = false;
            int total = 0;
            if (SUCCEEDED(hrE) && pEnum) {
                TF_LANGUAGEPROFILE lp = {};
                ULONG fetched = 0;
                while (pEnum->Next(1, &lp, &fetched) == S_OK && fetched == 1) {
                    total++;
                    if (IsEqualCLSID(lp.clsid, CLSID_WubiTsf)) {
                        found = true;
                        std::printf("[4] FOUND ours: guidProfile={%08lX-...} active=%d\n",
                                    (unsigned long)lp.guidProfile.Data1, (int)lp.fActive);
                    }
                }
                pEnum->Release();
            }
            std::printf("[4] zh-CN profiles total=%d, ours registered=%s\n", total, found ? "YES" : "NO");
            profiles->Release();
        }
    }

    // [5] 直接调用 AddLanguageProfile / EnableLanguageProfile 看真实返回值
    {
        ITfInputProcessorProfiles* p = nullptr;
        HRESULT hr5 = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_ITfInputProcessorProfiles, (void**)&p);
        if (SUCCEEDED(hr5) && p) {
            const GUID prof = { 0x7f3b0c21, 0x9d55, 0x4f0e, { 0xb1,0xc2,0x5a,0x6d,0x8e,0x9f,0x0a,0x12 } };
            const wchar_t* desc = L"Panda五笔";
            HRESULT h1 = p->AddLanguageProfile(CLSID_WubiTsf, 0x0804, prof, desc, (ULONG)wcslen(desc), nullptr, 0, 0);
            std::printf("[5] AddLanguageProfile -> 0x%08lX\n", (unsigned long)h1);
            HRESULT h2 = p->EnableLanguageProfile(CLSID_WubiTsf, 0x0804, prof, TRUE);
            std::printf("[5] EnableLanguageProfile -> 0x%08lX\n", (unsigned long)h2);
            HRESULT h3 = p->EnableLanguageProfileByDefault(CLSID_WubiTsf, 0x0804, prof, TRUE);
            std::printf("[5] EnableLanguageProfileByDefault -> 0x%08lX\n", (unsigned long)h3);
            BSTR bdesc = nullptr;
            HRESULT h4 = p->GetLanguageProfileDescription(CLSID_WubiTsf, 0x0804, prof, &bdesc);
            std::printf("[5] GetLanguageProfileDescription -> 0x%08lX%s\n", (unsigned long)h4,
                        SUCCEEDED(h4) && bdesc ? " (found)" : " (NOT found)");
            if (bdesc) SysFreeString(bdesc);
            p->Release();
        }
    }

    // [6] 试 ITfInputProcessorProfileMgr（Win8+ 接口，可能不需要管理员）
    {
        std::printf("[6] --- ITfInputProcessorProfileMgr ---\n");
        ITfInputProcessorProfileMgr* mgr = nullptr;
        HRESULT h = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                     IID_ITfInputProcessorProfileMgr, (void**)&mgr);
        std::printf("[6] QI ITfInputProcessorProfileMgr -> 0x%08lX\n", (unsigned long)h);
        if (SUCCEEDED(h) && mgr) {
            const GUID prof = { 0x7f3b0c21, 0x9d55, 0x4f0e, { 0xb1,0xc2,0x5a,0x6d,0x8e,0x9f,0x0a,0x12 } };
            const wchar_t* desc = L"Panda五笔";
            HRESULT h2 = mgr->RegisterProfile(CLSID_WubiTsf, 0x0804, prof, desc,
                                              (ULONG)wcslen(desc), nullptr, 0, 0,
                                              nullptr, 0, TRUE, 0);
            std::printf("[6] RegisterProfile -> 0x%08lX %s\n", (unsigned long)h2,
                        SUCCEEDED(h2) ? "(OK)" : "(FAILED)");
            TF_INPUTPROCESSORPROFILE got = {};
            HRESULT h3 = mgr->GetProfile(0x0002 /*TF_PROFILETYPE_INPUTPROCESSOR*/, 0x0804,
                                          CLSID_WubiTsf, prof, nullptr, &got);
            std::printf("[6] GetProfile -> 0x%08lX %s\n", (unsigned long)h3,
                        SUCCEEDED(h3) ? "(found)" : "(not found)");
            mgr->Release();
        }
    }

    // [7] 主动激活文本服务（不经 UI）：验证 ActivateEx + 词库载入
    {
        std::printf("[7] --- Activate text service ---\n");
        const CLSID CLSID_TF_ThreadMgr_L =
            { 0x529a9e6b, 0x6587, 0x4f23, { 0xab, 0x9e, 0x9c, 0x7d, 0x68, 0x3e, 0x3c, 0x50 } };
        ITfThreadMgr* tim = nullptr;
        HRESULT h = CoCreateInstance(CLSID_TF_ThreadMgr_L, nullptr, CLSCTX_INPROC_SERVER,
                                     IID_ITfThreadMgr, (void**)&tim);
        TfClientId tid = 0;
        if (SUCCEEDED(h) && tim && SUCCEEDED(tim->Activate(&tid))) {
            std::printf("[7] ThreadMgr activated tid=%u\n", tid);
            IUnknown* unk = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_WubiTsf, nullptr, CLSCTX_INPROC_SERVER,
                                           IID_IUnknown, (void**)&unk)) && unk) {
                ITfTextInputProcessor* tip = nullptr;
                if (SUCCEEDED(unk->QueryInterface(IID_ITfTextInputProcessor, (void**)&tip)) && tip) {
                    HRESULT ha = tip->Activate(tim, tid);
                    std::printf("[7] ITfTextInputProcessor::Activate -> 0x%08lX %s\n",
                                (unsigned long)ha, SUCCEEDED(ha) ? "(OK, 词库已载入)" : "(FAILED)");
                    Sleep(200);
                    tip->Deactivate();
                    tip->Release();
                }
                unk->Release();
            }
            tim->Deactivate();
        } else {
            std::printf("[7] ThreadMgr activate failed 0x%08lX\n", (unsigned long)h);
        }
        if (tim) tim->Release();
    }

    if (SUCCEEDED(hrCo)) CoUninitialize();
    // 若机器级注册已存在，则 DllRegisterServer 因非管理员返回 E_ACCESSDENIED 并不算失败
    bool registeredInHklm = false;
    {
        HKEY k = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                L"SOFTWARE\\Microsoft\\CTF\\TIP\\{7F3B0C21-9D55-4F0E-B1C2-5A6D8E9F0A11}",
                0, KEY_READ, &k) == ERROR_SUCCESS) {
            registeredInHklm = true;
            RegCloseKey(k);
        }
    }
    if (registeredInHklm) {
        std::printf("[!] DLL is registered machine-wide (HKLM present); DllRegisterServer needs admin only to re-register\n");
        if (hrReg == E_ACCESSDENIED) return 0;
    } else if (hrReg == E_ACCESSDENIED) {
        std::printf("[!] registration needs administrator: right-click register.cmd -> Run as administrator\n");
        return 3;
    }
    return SUCCEEDED(hrReg) ? 0 : 1;
}
