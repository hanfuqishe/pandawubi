// Panda五笔 —— 语言栏按钮实现
#include "wubi_langbar.h"
#include <msctf.h>
#include <ctfutb.h>
#include <new>
#include <string>

namespace {

class CLangBarItem : public ITfLangBarItemButton {
public:
    CLangBarItem(const CLSID& clsid, const GUID& guid, HINSTANCE inst, int iconId)
        : clsid_(clsid), guid_(guid), inst_(inst), iconId_(iconId) {}

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfLangBarItem)) {
            *ppv = static_cast<ITfLangBarItem*>(this);
        } else if (IsEqualIID(riid, IID_ITfLangBarItemButton)) {
            *ppv = static_cast<ITfLangBarItemButton*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG c = InterlockedDecrement(&ref_);
        if (c == 0) delete this;
        return c;
    }

    // ITfLangBarItem
    STDMETHODIMP GetInfo(TF_LANGBARITEMINFO* info) override {
        if (!info) return E_INVALIDARG;
        info->clsidService = clsid_;
        info->guidItem = guid_;
        // 按钮 + 显示在语言栏的托盘区；不自动隐藏
        info->dwStyle = TF_LBI_STYLE_BTN_BUTTON | TF_LBI_STYLE_SHOWNINTRAY;
        info->ulSort = 0;
        wcsncpy_s(info->szDescription, L"Panda五笔", _TRUNCATE);
        return S_OK;
    }
    STDMETHODIMP GetStatus(DWORD* status) override {
        if (!status) return E_INVALIDARG;
        *status = 0;
        return S_OK;
    }
    STDMETHODIMP Show(BOOL /*show*/) override { return S_OK; }
    STDMETHODIMP GetTooltipString(BSTR* text) override {
        if (!text) return E_INVALIDARG;
        *text = SysAllocString(L"Panda五笔 —— 单击切换中/英");
        return *text ? S_OK : E_OUTOFMEMORY;
    }

    // ITfLangBarItemButton
    STDMETHODIMP OnClick(TfLBIClick /*click*/, POINT /*pt*/, const RECT* /*prcArea*/) override { return S_OK; }
    STDMETHODIMP InitMenu(ITfMenu* /*menu*/) override { return S_OK; }
    STDMETHODIMP OnMenuSelect(UINT /*id*/) override { return S_OK; }
    STDMETHODIMP GetIcon(HICON* icon) override {
        if (!icon) return E_INVALIDARG;
        if (!hicon_) {
            // 从本 DLL 的图标资源里取 16x16（找不到就退到系统默认）
            hicon_ = (HICON)LoadImageW(inst_, MAKEINTRESOURCEW(iconId_), IMAGE_ICON,
                                       16, 16, LR_DEFAULTCOLOR);
            if (!hicon_) {
                hicon_ = (HICON)LoadImageW(inst_, MAKEINTRESOURCEW(iconId_), IMAGE_ICON,
                                           0, 0, LR_DEFAULTCOLOR | LR_DEFAULTSIZE);
            }
        }
        *icon = hicon_;
        return S_OK;
    }
    STDMETHODIMP GetText(BSTR* text) override {
        if (!text) return E_INVALIDARG;
        *text = SysAllocString(L"Panda五笔");
        return *text ? S_OK : E_OUTOFMEMORY;
    }

private:
    ~CLangBarItem() {
        if (hicon_) DestroyIcon(hicon_);
    }
    LONG     ref_ = 1;
    CLSID    clsid_;
    GUID     guid_;
    HINSTANCE inst_ = nullptr;
    int      iconId_ = 0;
    HICON    hicon_ = nullptr;
};

} // namespace

IUnknown* CreateLangBarItem(const CLSID& clsid, const GUID& guidItem, HINSTANCE hInst, int iconId) {
    CLangBarItem* item = new (std::nothrow) CLangBarItem(clsid, guidItem, hInst, iconId);
    if (!item) return nullptr;
    return static_cast<ITfLangBarItemButton*>(item);
}
