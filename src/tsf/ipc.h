// ipc.h —— 设置窗口与输入法 DLL 之间的轻量通知
#pragma once
#include <windows.h>

// 设置保存后广播这条消息（RegisterWindowMessage 在两端返回同一个 id），
// 输入法收到后就地重新加载 config.ini —— 不需要重启任何应用。
inline UINT PandaReloadMessage() {
    static const UINT msg = RegisterWindowMessageW(L"PandaWubi.ReloadSettings");
    return msg;
}