// ---------------------------------------------------------------- 设置入口（ITfFnConfigure）
// 文档：Text Services 控制面板 / 输入法菜单会 CoCreateInstance(本 TIP 的 CLSID, IID_ITfFnConfigure)，
// 拿到这个对象后显示「设置」并调用 Show()。
static HRESULT LaunchSettingsWindow(HWND parent) {
    wchar_t mod[MAX_PATH] = { 0 };
    if (GetModuleFileNameW(g_hInst, mod, ARRAYSIZE(mod)) == 0) return E_FAIL;

    std::wstring dir(mod);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return E_FAIL;
    dir.erase(slash);

    const std::wstring exe = dir + L"\\wubi-settings.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        LogLine(L"configure: settings exe missing: %ls", exe.c_str());
        return E_FAIL;
    }
    const HINSTANCE h = ShellExecuteW(parent, L"open", exe.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
    const bool ok = (INT_PTR)h > 32;
    LogLine(L"configure: launch %ls -> %s", exe.c_str(), ok ? L"ok" : L"failed");
    return ok ? S_OK : E_FAIL;
}

class CConfigureFn : public ITfFnConfigure {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfFnConfigure)) {
            *ppv = static_cast<ITfFnConfigure*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override {
        const LONG c = InterlockedDecrement(&ref_);
        if (c == 0) delete this;
        return c;
    }

    STDMETHODIMP GetDisplayName(BSTR* name) override {
        if (!name) return E_INVALIDARG;
        *name = SysAllocString(L"Panda五笔 设置");
        return *name ? S_OK : E_OUTOFMEMORY;
    }
    STDMETHODIMP Show(TfEditCookie, HWND wndParent, LANGID) override {
        return LaunchSettingsWindow(wndParent);
    }

private:
    LONG ref_ = 1;
};
