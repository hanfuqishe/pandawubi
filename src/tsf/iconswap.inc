// ---------------------------------------------------------------- 指示器图标切换
// 语言指示器读的是注册表里那个固定路径的 .ico（dist\ppwubi.ico）。
// 路径不能改（要管理员），但文件内容可以换——所以切换中/英时把源图标复制过去。
void CTextService::SwapIndicatorIcon(bool english) {
    wchar_t modPath[MAX_PATH] = { 0 };
    if (GetModuleFileNameW(g_hInst, modPath, ARRAYSIZE(modPath)) == 0) return;

    std::wstring dir(modPath);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    dir.erase(slash);

    const std::wstring target = dir + L"\\ppwubi.ico";
    const std::wstring source = dir + (english ? L"\\ppwubi-en.ico" : L"\\ppwubi-zh.ico");

    if (GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES) {
        LogLine(L"swap icon: source missing %ls", source.c_str());
        return;
    }
    if (!CopyFileW(source.c_str(), target.c_str(), FALSE)) {
        LogLine(L"swap icon failed err=%lu (src=%ls)", GetLastError(), source.c_str());
        return;
    }
    LogLine(L"swap icon -> %ls", english ? L"en" : L"zh");
}
