// ---------------------------------------------------------------- 中/英 状态上报
// Windows 的语言指示器读的是 TSF 隔间，而不是我们自己内部的 bool。
//   GUID_COMPARTMENT_KEYBOARD_OPENCLOSE             1=打开(中文) 0=关闭(英文)
//   GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION  TF_CONVERSIONMODE_NATIVE / ALPHANUMERIC
void CTextService::SetInputModeCompartments(bool chinese) {
    if (!m_context) { LogLine(L"compartments: no context"); return; }

    ITfCompartmentMgr* cm = nullptr;
    if (FAILED(m_context->QueryInterface(IID_ITfCompartmentMgr, (void**)&cm)) || !cm) {
        LogLine(L"compartments: no ITfCompartmentMgr");
        return;
    }

    VARIANT v;
    VariantInit(&v);
    v.vt = VT_I4;
    v.lVal = chinese ? 1 : 0;
    ITfCompartment* comp = nullptr;
    if (SUCCEEDED(cm->GetCompartment(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE, &comp)) && comp) {
        const HRESULT hr = comp->SetValue(m_tid, &v);
        LogLine(L"OPENCLOSE=%d hr=0x%08lX", (int)chinese, (unsigned long)hr);
        comp->Release();
    } else {
        LogLine(L"compartments: GetCompartment(OPENCLOSE) failed");
    }

    VariantInit(&v);
    v.vt = VT_I4;
    v.lVal = chinese ? TF_CONVERSIONMODE_NATIVE : TF_CONVERSIONMODE_ALPHANUMERIC;
    comp = nullptr;
    if (SUCCEEDED(cm->GetCompartment(GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION, &comp)) && comp) {
        const HRESULT hr = comp->SetValue(m_tid, &v);
        LogLine(L"CONVERSION=%d hr=0x%08lX", (int)chinese, (unsigned long)hr);
        comp->Release();
    }

    cm->Release();
}
