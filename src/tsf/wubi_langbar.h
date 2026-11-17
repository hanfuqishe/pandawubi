// Panda五笔 —— 语言栏按钮（ITfLangBarItemButton）
// Windows 的输入指示器只认语言栏按钮提供的图标，profile 里登记的 IconFile 不生效。
#pragma once
#include <windows.h>
#include <msctf.h>
#include <ctfutb.h>

// 创建一个语言栏按钮（返回 IUnknown*，调用方负责 Release）
// clsid/guidItem 用于区分条目；iconId 是 DLL 内的图标资源 ID
IUnknown* CreateLangBarItem(const CLSID& clsid, const GUID& guidItem, HINSTANCE hInst, int iconId);
