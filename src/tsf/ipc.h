// ipc.h —— 设置窗口与输入法 DLL 之间的轻量通知
#pragma once
#include <windows.h>

// 设置保存后广播这条消息（RegisterWindowMessage 在两端返回同一个 id），
// 输入法收到后就地重新加载 config.ini —— 不需要重启任何应用。
inline UINT PandaReloadMessage() {
    static const UINT msg = RegisterWindowMessageW(L"PandaWubi.ReloadSettings");
    return msg;
}
// 悬浮栏"让位"消息：整个桌面只允许一条可见的栏。
// 谁把自己那条显示出来，就广播这条消息给**别的进程**的栏，让它们自行隐藏
// （跨进程不能直接 DestroyWindow —— 那正是之前炸出十几条幽灵栏的根因）。
inline UINT PandaBarYieldMessage() {
    static const UINT msg = RegisterWindowMessageW(L"PandaWubi.BarYield");
    return msg;
}