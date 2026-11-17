# QQ五笔输入法 2.4 逆向分析报告（REA）

- **目标**：本机安装的 QQ五笔输入法 2.4（`C:\Program Files (x86)\Tencent\QQWubi\2.4.629.400`）+ 系统 IME 目录
  `C:\Windows\System32\IME\QQWubiTSF`（x64）与 `C:\Windows\SysWow64\IME\QQWubiTSF`（x86）
- **方法**：REA 6.4.0（`rea` CLI）+ Ghidra 12.1.4 headless（静态反编译）、PE 表机械解析、注册表/文件系统运行时观察
- **分析时间**：2026-11-16
- **证据目录**：`evidence\`（本报告所有引用的 REA Evidence 原始 JSON 均在其中）

---

## 0. 结论摘要

1. QQ五笔 2.4 是**三层结构**：`QQWubiTSF.dll`（COM/TSF 文本服务，装进 `System32\IME`）→ `QQWubi.ime`（真正的 IMM32 五笔引擎，6.6 MB / 26 583 个函数）→ `QQImeUtil(_x64).dll`（词库与皮肤管理服务，28 个导出）。周边还有 21 个 EXE（配置、托盘、换肤、更新、崩溃上报、等级、截图、卸载等）。
2. **没有独立引擎进程**：`QQWubiTSF.dll` + `QQWubi.ime` 由 COM/TSF 机制加载到**每一个**使用输入法的 GUI 进程里。实测当前有 16 个进程已加载（explorer、msedge、Weixin、Signal、VS Code、Everything、SearchHost、DSH Desktop…）。这是 TSF 的标准模型，但意味着这个 6.6 MB 引擎的代码在所有这些进程里都有一份实例。
3. 它是**纯 TSF 输入法**：注册表里只有 TSF 的 CLSID/TIP/LanguageProfile，没有 IMM 的键盘布局项；`QQWubi.ime` 通过 TSF 服务在进程内被加载，用来复用同一套五笔引擎。
4. 代码血缘清晰：引擎里留了构建路径
   `e:\project\qqpinyin\trunk\product\qqinput\win32\qqwubi_2_4\client\qqwubilib\utils.cpp`，RTTI 里全是 `CPy*` 类名 —— **QQ五笔 2.4 与 QQ拼音共用同一套 `qqinput` 框架**，只是把拼音引擎换成了五笔词库引擎。
5. 词库是一族**自定义二进制格式**，每种文件一个 4 字节魔数（`SBWQ`/`CBWQ`/`ACBW`/`CYPQ`/`E2CQ`/`NEPQ`/`2UWQ`/`DBWQ`/`DOFQ`），另有 `.qpyd` 容器（魔数 `0x7D1EA609`、偏移 `+0x48` 处整文件校验和）。
6. 用户数据全在 `%APPDATA%\Tencent\QQWubi`：用户自造词、删除词、词频、输入统计、皮肤、以及 `Sync\` 云同步目录。
7. **确有统计与联网能力**：引擎/服务里出现 `TypeStatistic`、`UploadModules`、`QQWUBI_TYPE_STATISTIC_SHARE_MAP`、`qq.pinyin.cn`、`dict.qq.com` 等，且运行日志里能看到服务器返回的 JSON（`total_words` / `today_words` / `login_days`）。本报告只陈述**静态可达性与已落盘的观察**，**没有**做流量抓包，因此不断言"何时上传了什么"。

---

## 1. 目标身份（观察）

| 组件 | 大小 | SHA-256（前 32 位） | REA Evidence |
|---|---|---|---|
| `System32\IME\QQWubiTSF\QQWubiTSF.dll` | 496 072 | `21fc53013f2f183062784509e273c79c` | `ev_515d1796…b7def7e` |
| `System32\IME\QQWubiTSF\QQWubi.ime` | 6 637 000 | `be8fa6ee8f7267fc34eff69c11859464` | `ev_9e52cc4c…c624ca3a` |
| `QQImeUtil_x64.dll` | 3 573 192 | `9ca447f72d8f1c6fcba35450fa3d4e05` | `ev_9a62341c…1d5dd059`, `ev_4dbf6aa1…e403581c5` |
| `QQImeUtil.dll`（x86） | 2 339 784 | `665e0b353f67b5336df631c96868bf9a` | 本次未做深度分析 |
| `QQWubiConfig.exe` | 5 832 648 | `daace981fa49f94a2e0df2554f2fbd97` | 本次未做深度分析 |

机器架构：`QQWubi.ime` 与 `QQWubiTSF.dll` 为 **x86_64 PE**（Ghidra 镜像基址 `0x180000000`）；
`QQImeUtil.dll` 为 **x86 PE32**，`QQImeUtil_x64.dll` 为 **x86_64**，两者导出集合完全一致（同一份源码两套编译）。

---

## 2. 架构

### 2.1 三个核心部件

| 层 | 文件 | 角色 | 证据 |
|---|---|---|---|
| 框架层 | `QQWubiTSF.dll` | COM 进程内服务，只导出 `DllGetClassObject`/`DllCanUnloadNow`/`DllRegisterServer`/`DllUnregisterServer` | PE 导出表（`evidence\pe_tables_tsf.txt`） |
| 引擎层 | `QQWubi.ime` | 实现完整 IMM32 `Ime*` 导出（`ImeProcessKey`、`ImeToAsciiEx`、`ImeSelect`、`ImeEscape`、`ImeConfigure` …共 18 个） | PE 导出表（`evidence\pe_tables_ime.txt`） |
| 服务层 | `QQImeUtil(_x64).dll` | 词库/皮肤管理 API：`ImeUtil_Init`、`Ime_InstallDict`、`Ime_IsDictFileValid`、`Ime_CompareDictVersion`、`Ime_GetUserDataPath`、`SknEng_LoadSkinFile`、`SknEng_GetCompCandPreview` … 共 28 个 | PE 导出表（`evidence\pe_tables_engine_x64.txt`） |

`QQWubiTSF.dll` 的 RTTI 直接暴露了它实现的 TSF 接口（`evidence\strings_tsf_x64.txt`）：
`ITfTextInputProcessor` / `…ProcessorEx`、`ITfKeyEventSink`、`ITfThreadMgrEventSink`、`ITfThreadFocusSink`、
`ITfCompositionSink`、`ITfTextEditSink`、`ITfTextLayoutSink`、`ITfDisplayAttributeProvider`、
`ITfLangBarItemButton`、`ITfCandidateListUIElement`（含 `Behavior`）、`ITfFunctionProvider`、`ITfFnConfigure`、
`ITfCompartmentEventSink`、`ITfUIElement` 等 25 个接口。

> 观察：TSF 侧自己就把候选窗做成了 TSF UIElement（`ITfCandidateListUIElementBehavior`），
> 说明候选窗在 TSF 路径下由 `QQWubiTSF.dll` 绘制，而不是靠 `QQWubi.ime` 的老式候选窗。

### 2.2 周边可执行文件（按字符串/RTTI 交叉印证）

`QQWubiConfig.exe`（设置界面）、`QQWBTrayBar.exe`（托盘）、`QQWubiFace.exe`（皮肤/外观）、`QQWubiZiGen.exe`（自造词）、
`QQWBDictManager.exe`（词库管理）、`QQWBDictMerge.exe`（词库合并，引擎字符串里有 `\QQWBDictMerge.exe`）、
`QQWBCodeQuery.exe`（编码查询）、`QQWubiTypeStat.exe`（输入统计）、`QQWubiLevel.exe`（等级/积分）、
`QQWBLiveup.exe` + `QQDownload\Tencentdl.exe` + `QQDownload\dlcore.dll`（在线更新）、
`QQWBBugReport.exe`（崩溃上报）、`QQWBUserCenter.exe`、`QQWBService.exe`（服务 `QQWbService`，当前 Stopped）、
`QQWBImeBroker.exe`、`SnapShot.exe`（截图）、`QQPYHandInput.exe`（手写，QQ拼音遗留）。

### 2.3 进程模型（运行时观察）

```
任意 GUI 进程（explorer / msedge / Weixin / Code …）
  └─ QQWubiTSF.dll  (TSF 文本服务, COM)
       └─ QQWubi.ime (IMM32 五笔引擎, 进程内)
             └─ 词库文件 wubi.dict / code.dict / 用户词库（被多进程同时打开 → 观察到的文件锁）
```

实测已加载 `QQWubiTSF.dll` + `QQWubi.ime` 的进程：`AISearchUI`、`Code`、`ControlCenterTrayUI`、`DSH Desktop`、
`Everything`、`explorer`、`MessageCenterUI`、`msedge`、`msedgewebview2`、`SearchHost`、`Signal`、`TicketManager`、
`WeChatAppEx`(×2)、`Weixin`(×2)。用户词库 `WBUser3.dict` 等被这些进程占用（读取时需 `FileShare.ReadWrite`），
这从侧面印证了"引擎在宿主进程内直接读写词库"，而不是通过服务进程代理。

---

## 3. 注册与加载（观察）

TSF 注册（`HKLM\SOFTWARE\Microsoft\CTF\TIP`）：

- CLSID `{6AF21238-DB8B-45DB-8567-7F7023AFC493}`，`InprocServer32` = `System32\IME\QQWubiTSF\QQWubiTSF.dll`
  （WOW64 视图指向 `SysWow64\IME\QQWubiTSF\QQWubiTSF.dll`），`ThreadingModel=Apartment`，描述"中文 - QQ五笔输入法"
- LanguageProfile：`0x00000804`（zh-CN）下 profile GUID `{CC73A514-2F8B-4603-9965-C61181A8FCE6}`，`Enable=1`
- 注册了 7 个 TSF 类别（`Category\Category\{…}`）：`{34745C63-…}`（键盘类）、`{13A016DF-…}`、`{25504FB4-…}`、
  `{364215D9-…}`、`{49D2F9CF-…}`、`{534C48C1-…}`、`{CCF05DD7-…}`、`{046B8C80-…}`
- 辅助 COM 类：`{5052B4D0-9DF7-45EF-88EF-F42C0EA33A43}` → `QQImeChecker.dll`（"QQPYChecker Class"）

> 推断：上列 7 个 GUID 属于 Windows 标准的 TSF 类别集合（键盘 / 输入模式 Compartment / 安全模式 / 无 COM 模式 /
> 沉浸式支持 / 系统托盘 / UIElement 等）。GUID 本身是观察到的；"每个 GUID 对应哪个语义"这里按 Windows SDK 的
> 通用知识归类，**未逐项复验**。

**没有** `Keyboard Layouts` 下的 `IME file=QQWubi.ime` 项 —— 即它**不是**传统 IMM 输入法，而是 TSF-only；
`QQWubi.ime` 只是被 TSF 服务在用（`QQWubiTSF.dll` 里有 `QQWubi.ime` 与 `\IME\QQWubiTSF\QQWubi.ime` 两个字符串，
`evidence\strings_tsf_x64.txt#L39`）。

注册表其它痕迹：`HKCU\Keyboard Layout\Preload` 只有 `00000804`（中文）与 `00000409`；`Software\Tencent\QQWubi`、
`Software\Tencent\QQWubi.user`、`Software\Tencent\QQWubi\Liveup`、`Software\Tencent\QQWubi\RebootFlag`、
`Software\Tencent\QQWubi\ValidKey` 等键名在二进制字符串里均可见。

---

## 4. 引擎内部

### 4.1 类结构（RTTI，观察）

`evidence\strings_ime_x64.txt` 中出现的引擎侧类型：

- 词库：`CWubiDictEngine`、`CWubiCacheDictEngine`、`CWubiCacheDictSearcher`、`CWubiFixOrderDictionary`、
  `CCellDictsConfig`、`CellDictRepeatFinder`、`CAccountMutex`
- 候选：`CQQWubiCandsMgr`、`CQQWubiCandRecordSet`、`CWubiCandRecordSet`、`CWubiUserCandRecordset`、
  `CWubiAutoCandRecordset`、`CAutoMakeWordMgr`（命名空间 `WubiAutoMakeWord`）
- 界面/皮肤：`CQQWubiStyle`、`CPySkinStatus`、`CDefaultStatus`、`CPyStatic`、`CPyMappingTable`、`CPySuEnMap`
- 单例：`ImmSingleton<CWubiDictEngine>`、`ImmSingleton<CWubiCacheDictEngine>`、`ImmSingleton<CWubiFixOrderDictionary>`

窗口类名：`QQWubiUIWnd`、`QQWubiCompWndII`、`QQWubiCandWndII`、`QQWubiSyllToneTipsWndII`、`QQWubiStatusWndII`
（`ImeInquire` 会把 `QQWubiUIWnd` 拷到 IME 的 UI 窗口类名参数里）。

### 4.2 按键到候选的主路径（反编译，观察）

调用链（`evidence\fn_todi.json`、`fn_toascii_core.json`）：

```
ImeToAsciiEx(dwVKey, ...)                      0x180002473  —— 只有 12 行，纯转发
  └─ 取引擎单例 FUN_180078fe0()
      └─ FUN_1800763e0(...)                    0x1800763e0  —— IMM 边界胶水
           ├─ ImmLockIMC / ImmUnlockIMC        打开 IMC（输入上下文）
           ├─ 4 个智能指针/对象校验（失败即返回 0）
           ├─ ToAscii(...)                     只对"非 IME 组合键"做一次透传
           ├─ PostMessageW(hwnd, 0x8351, 0, 0) 给 IME UI 窗口发私有消息（WM_APP+0x351）
           └─ (**(engine+0x30))(...)            真正的五笔处理在引擎对象的 vtable+0x30 槽
```

> 观察：`ImeToAsciiEx` 本身不含五笔逻辑；语义全部下沉到引擎对象虚函数。这一点可以解释为什么
> `QQWubi.ime` 能同时服务 IMM 路径与 TSF 路径（TSF 侧复用同一批类）。
> 未知：`vtable+0x30` 具体指向哪个成员、以及码表查找/候选排序的完整实现，本次未展开
> （26 583 个函数里靠虚表反推，成本较高）。

### 4.3 IMM 兼容层的实际能力（反编译，观察）

`ImeEscape`（`evidence\fn_escape.json`）只处理两种 `escape`：`3`（`IME_ESC_QUERY_SUPPORT`）与
`0x1006`（`IME_ESC_GETHELPFILENAME`，返回帮助文件名 `QQWubiHelp.chm`），其它一律返回 0。
即**它不向外部应用提供任何 IMM 私有逃逸命令**，配置/词库/统计等全部走自家 IPC 与文件。

`ImeInquire`（`evidence\fn_inquire.json`）填写 `IMEINFO`：
`dwPrivateDataSize=0x30`、`fdwProperty=0xC000A`、`fdwConversionCaps=0x58B`、`fdwUICaps=0x10002`、`fdwSelectCaps=0`；
并把 `QQWubiUIWnd` 作为 UI 窗口类名。随后它拼出安装目录下的 `ExceptionBinary.bin` 路径并装好自己的异常处理——
对应安装目录里那个 57 420 字节的 `ExceptionBinary.bin`（即自带崩溃转储/现场文件）。
`fdwProperty` 的 `0xC000A` 至少包含 `IME_PROP_UNICODE`(0x8)、`IME_PROP_NO_KEYS_ON_CLOSE`(0x40000)、
`IME_PROP_END_UNLOAD`(0x80000)（按 SDK 位定义解读，属**推断**）。

### 4.4 跨进程 IPC（观察：字符串 + 导入表）

命名内核对象（`evidence\strings_ime_x64.txt#L1085`）：

```
QQWUBI_MUTEX_{6D6341BA-770E-4ab9-9866-A7CEACFE3D05}
QQWUBI_SHAREMAP_{210170F0-533B-49df-A774-1E7A755C34D1}
QQWUBI_RUNTIME_INFO_{BA0170F0-103B-48Ef-ABC4-1E7A755C3891}
QQWUBI_MUTEX_TYPE_STAT_{f64bcd59-faef-42ff-ab43-47a31d8a984a}
QQWUBI_TYPE_STATISTIC_SHARE_MAP_{d9b4cac8-8adf-4f1b-b81b-907e331a862d}
QQWUBI_URL_MAP:{BFB8817C-7A0B-4406-B4F7-12B479586885}_2.02.4.629.400
QQWUBI_NOTIFY_LEVEL_DLG_{7ECCD023-6F8A-4db7-B1C8-5681148C027C}
QQWUBI_AUTO_MAKE_WORD_PINYIN_WORD_BUFFER_v1.0
QQWubi{4E58ECF1-F414-42d7-B694-ACACC94FD32A}
```

配套导入：`CreateFileMappingW`/`OpenFileMappingW`/`MapViewOfFileEx`、`CreateMutexW`/`OpenMutexW`/`ReleaseMutex`、
`CreateSemaphoreW`/`OpenSemaphoreW`、`PeekNamedPipe`，以及一组 **安全描述符** API
（`ConvertStringSecurityDescriptorToSecurityDescriptorW`、`BuildExplicitAccessWithNameW`、`SetEntriesInAclW`、
`SetSecurityInfo`）——典型用途是让不同完整性级别（例如提权进程与非提权进程）之间也能打开同一命名对象。

> 观察：`QQWUBI_URL_MAP` 的对象名里直接拼了版本号 `2.02.4.629.400`，说明跨版本共存时刻意做实例隔离。
> 推断：`SHAREMAP`/`RUNTIME_INFO` 用于在多宿主进程之间共享"当前输入模式/皮肤/配置"等运行态；
> `TYPE_STATISTIC_SHARE_MAP` 用于合并各进程的键入统计计数，之后由 `QQWubiTypeStat.exe` 汇总上报。

---

## 5. 数据文件与词库格式

### 5.1 安装目录数据文件（观察：十六进制头部）

| 文件 | 大小 | 头 4 字节 | 已观察到的结构（偏移均为文件内字节偏移） |
|---|---|---|---|
| `wubi.dict` | 1 773 306 | `SBWQ` | `+0x04`=`u16 0`/`u16 1`、`+0x08` = 文件长度（`0x1B0EFA`，与实际大小一致）、`+0x0C` = 疑似条目数（`0x172DB` = 94 939）、`+0x10` 起为递增偏移表（`0xFE4C, 0xFE6C, …`） |
| `code.dict` | 111 328 | `CBWQ` | 同族头：`+0x04` 版本、`+0x08` 长度、`+0x0C` 疑似计数、其后偏移表 |
| `cache.dict` | 107 194 | `ACBW` | 同族头（`+0x04` 版本 2、`+0x08` 长度、`+0x0C` 疑似计数、`+0x10` 起递增偏移） |
| `pinyin.dict` | 3 788 425 | 非文本（混淆/加密头） | `0x00–0x1F` 呈随机分布；`+0x20` 之后出现疑似偏移表（`0x70`/`0xC890`/`0x190B0`/`0x42B70`…） |
| `pinyincode.dict` | 262 021 | `01 01` + GUID | 含 ASCII GUID `e2fbe37f-32f8-477a-b369-aa731d845f07`，其后 `0xFF` 填充 |
| `engcore.dat` | 733 988 | `NEPQ` | `+0x04` 版本 1、`+0x08` 疑似计数（`0xC0E5`）、`+0x0C`=`0x54`、`+0x10` 疑似长度；`+0x58` 起为 6 字节定长记录表 |
| `CDict\3770.qpyd`、`pynewword.qpyd` | 256 432 / 307 759 | `09 A6 1E 7D` | `.qpyd` 容器（见 5.2） |
| `trans2.dict`（用户目录） | 8 946 968 | `E2CQ` | `+0x04`=`0x7DB`、`+0x08`=`0x1D259`、`+0x0C`=`0x54`、`+0x10`=`0x749B8` |
| `pymergecell.dict`（用户目录） | 812 146 | `CYPQ` | `+0x04` 版本 2，`+0x08` 起 16 字节 GUID `{E6902CA1-4333-4E59-882B-27CF89C19C26}` |
| `WBUser3.dict` / `WBDel.dict` / `FO.dict` | 61 / 61 / 49 | `2UWQ` / `DBWQ` / `DOFQ` | 魔数 + 版本/计数，空库时其后全 0 |
| `simp2trad.dat` | 131 072 | — | 65 536 项 `uint16` 映射表（简繁转换，起始为恒等映射） |
| `url.dat` | 48 874 | UTF-16 文本 | 每行"关键字 权重 说明"，如 `2010.qq.com 900 …`（网址快捷输入表） |
| `wordrankinfo.tbl` | 27 870 | `00 01` + GUID | GUID `7c8e76fc-5624-4807-b675-4c053fa2593c` 后接逐词 rank 字节数组 |

> 观察：`pymergecell.dict` 里的 GUID 与用户 `cdictlist.ini` 中 `[PYmergedict] guid={E6902CA1-…}` 完全一致，
> 说明"单元格/合并词库"用 GUID 做身份标识；`pinyincode.dict` 同一个 GUID 就是它的库 ID。
> 观察：`Ime_CompareDictVersion`（`evidence\fn_dictversion.json`）并不读 GUID，而是先 `FindFirstFileW` 确认两个文件
> 都存在，再用 `FUN_18010ac70` 各取一个 `uint32` 版本号比较（返回 0 相等 / 1 / -1），用于判断词库是否需要更新。

### 5.2 `.qpyd` 容器（反编译，观察）

`Ime_IsDictFileValid` → `FUN_180109d30`（`evidence\fn_dictvalid.json`、`fn_dictparse.json`）的行为非常明确：

1. `CreateFile` 以只读打开，取文件大小；**要求 > 0x5B（91）字节**，否则判为非法；
2. 一次性把整个文件读进堆缓冲；
3. 校验首 dword == **`0x7D1EA609`**（磁盘上即 `09 A6 1E 7D`，与 `pynewword.qpyd`、`CDict\3770.qpyd` 一致）；
4. 读出 `+0x48` 处的 dword 作为"存储校验和"，**将该字段清零后**对整个镜像计算哈希（`FUN_180114700`），
   与存储值比较；不一致则判为非法。

`.qpyd` 里还能看到 UTF-16 字段名 `Name: `、`Type: `、`FirstType: `、`Int…`，以及两个 `FILETIME`（编译/更新时间）。

> 所以 `.qpyd` 是"带自校验的单元格词库容器"，而 `*.dict`（`SBWQ` 等）是另一套带偏移表的码表格式。
> 未知：`SBWQ` 偏移表的具体语义（键序 / 桶划分）与条目编码，未在本次展开。

### 5.3 词库安装/管理 API（观察：导出语义）

`Ime_InstallDict`、`Ime_IsDictExist`、`Ime_IsDictInUse`、`Ime_IsDictEngSupport`、`Ime_GetDictInternalFileName`、
`Ime_GetDictExample`、`Ime_CompareDictVersion`、`Ime_InstallSkin`、`Ime_SetSkinToUse`、`Ime_IsSkinExist` ——
词库与皮肤走同一套"安装/校验/版本比较"管线，`dict\`、`Skin\2.0\`、`*.skn`、`cdictlist.ini`、`[cdict%d]`、
`[WBmergedict]`、`[PYmergedict]`、`%s_%d.dict` 等字符串印证了多词库（`cdict1..N`）+ 合并词库的组织方式。

---

## 6. 功能细节（配置文件，观察）

`punc.ini`：标点映射四列矩阵 —— `英文半角 / 中文半角 / 英文全角 / 中文全角`，值形如 `!=1！1！1！`，
`^=2……1＾2……`；数字 1/2 表示"该列输出 1 个还是 2 个字符"（如 `"` 第一次 `“`、第二次 `”`）。

`phrase.ini`：内置短语库，键就是五笔编码，值 `权重,输出`，并支持宏变量：
`date=2,$(Year).$(month).$(date)`、`aaem=2,A股`、`cyyy=2,C语言`。

`Face\FaceWhiteList.ini`：**按进程名的兼容白名单**（`winword.exe`、`wps.exe`、`qq.exe`、`360se.exe`…，第三列 `1`），
配套 `Face\WhiteListIcon\*.ico`（12 个图标文件）——即"在哪些应用里显示/如何显示状态条"的兼容表。

`speclist.ini`：另一张特殊处理表（`CodePage=936`、`LangId=5`），含 Photoshop/Chrome/Firefox 以及
`MapleStory.exe`、`GameApp.exe`(QQ飞车)、`Audition.exe`(劲舞团)、`tw2.exe`(天下贰) 等游戏，标记 `1`/`2`
——用于全屏游戏/特殊窗口下的输入与候选窗行为适配。

配置项（`%APPDATA%\Tencent\QQWubi\SAccount\<md5>\Config.ini`）：`InputMode=0`（五笔）、`UseGBK=1`、
`CandVertical`、`EnablePunction`、`ChangeIMEOutput`、`PuncMarkMatch`、`SoftKeyBoardState`、`AutoSynch=1`、
`AutoSyncCfgSet=1`，热键以 VK/modifier 组合数字形式保存在 `[HotKey]`（`SimpleTradKey=838`、`ShowHideStatusKey=851`、
`QueryInfoKey=191`）。二进制里同名字符串出现在 `evidence\strings_qimeutil_x64.txt` 与 `strings_ime_x64.txt`。

---

## 7. 联网与统计（区分观察/推断）

**静态可见的端点**（字符串，观察）：

| 端点 | 所在二进制 |
|---|---|
| `http://qq.pinyin.cn/wubi` | `QQWubi.ime` |
| `http://skin.qq.pinyin.cn` | `QQWubi.ime`（皮肤） |
| `http://qq.pinyin.cn/help/feedback.php` | `QQWubi.ime`（反馈） |
| `http://dict.py.qq.com` | `QQWubi.ime`（词库） |
| `http://dict.qq.com/` | `QQWubi.ime`、`QQImeUtil_x64.dll` |
| `http://pyhelp.qq.com/cgi-bin/pyidjump.so?` | `QQWubi.ime`、`QQImeUtil_x64.dll` |
| `http://100189996.qzone.qq.com/`、`http://t.qq.com/QQWubi` | `QQWubi.ime`（推广链接） |

**网络栈**：`QQWubi.ime` 的导入表里有 `WS2_32.dll`（**按序号导入**：`#52 #8 #115 #23 #3 #20 #116 #14 #9`，
x86 与 x64 两份 `.ime` 的序号集合相同）与 `NETAPI32.dll`（`NetWkstaTransportEnum`、`Netbios`、`NetApiBufferFree`）。
`QQImeUtil_x64.dll` 只导入了 `WS2_32` 的两个序号（`#14 #8`）。

> 观察：WinSock 只按序号导入，等于**对按名字匹配的导入分析隐藏了 API 名**。
> 推断：按 Windows 上 `ws2_32` 的常见序号约定，`#115`/`#116` 通常是 `WSAStartup`/`WSACleanup`、`#23` 是 `socket`、
> `#52` 是 `gethostbyname`、`#3` 是 `closesocket`；**该序号映射未逐项复验**。加之二进制同时导入了
> `LoadLibraryW`/`GetProcAddress`，也可能存在运行时动态解析。
`ConvertStringSecurityDescriptor…`、`DeviceIoControl`/`GetDriveTypeW`/`GetLogicalDrives` 也在导入表里。

> 推断：`NetWkstaTransportEnum`/`Netbios` 是取本机网络传输绑定信息的经典做法（MAC/适配器枚举），
> 常用于设备指纹；`SBWQ` 词库外层的统计与 `qimei` 类设备 ID 体系是否联动，本次**无法证实**——
> 注意 `%APPDATA%\Tencent\qimei`、`Tencent\beacon` 是**多个腾讯产品共用**的目录，不能直接归因给 QQ五笔。

**本地统计相关（观察：文件名 + 运行日志）**：
`typeSate_v2.dat`（66 B，含 32 位计数）、`typeStat.dat`、`InputAnalysis2.dat`、`local.stat`（235 B 计数区）、
`LevelPoint\`、`level_%u.dat`；日志 `log\QQWubi.log` 记录 `Use Stat: cmd=1203 … 1210`；
`profile.ini` 有 `[QQPYSwitchReport] TimeStamp=…`（切换上报时间戳）。

**服务器交互的已落盘证据（运行时观察，非 REA）**：`log\QQWubiConfig..2.4.629.400.log` 里保存了服务器返回的 JSON：

```json
{"status":0,"msg":"ok","timestamp":1794803270,
 "data":[{"platform":1,"total_words":2797022,"today_words":18,"login_days":1},
         {"platform":2,"total_words":2,"today_words":0,"login_days":1}]}
```

> 观察：字段是分平台（`platform` 1/2，对应五笔/拼音两条输入线）的字数与登录天数统计，且服务端会回写该数值。
> 推断：这是"打字统计/等级"功能的服务端侧；`total_words` 数值较大，可能是累计或聚合口径，**具体语义未知**。
> 未知：上传时机、字段全集、是否包含输入内容（本次**没有**抓到任何请求报文，不能断言内容粒度）。

---

## 8. 安全面观察

以下均为**导入表能力**（机械解析 PE，`evidence\pe_tables_*.txt`）。"导入"只证明链接了该 API，
**不证明被调用**；本节所有条目都属于"能力存在"，**未做调用点核验**（`rea xrefs` 逐条查询在本次超时未完成，
见第 10 节），因此不能据此断定存在具体行为。

`QQWubi.ime`（引擎，跑在每个宿主进程里）：

- **全局输入钩子能力**：`SetWindowsHookExW` / `CallNextHookEx` / `UnhookWindowsHookEx`
- **合成输入**：`SendInput`、`keybd_event`、`mouse_event`、`SetCursorPos`、`SetForegroundWindow`、`AttachThreadInput`
- **剪贴板**：`OpenClipboard` / `GetClipboardData` / `SetClipboardData` / `EmptyClipboard`
- **进程枚举**：`CreateToolhelp32Snapshot`、`Process32FirstW/NextW`、`FindWindowW`、`GetClassNameW`、`EnumChildWindows`
- **跨进程读内存**（仅 x86 版 `SysWow64\...\QQWubi.ime`）：`ReadProcessMemory`、`VirtualQueryEx`、`GetThreadSelectorEntry`
- **注册表写入**：`RegSetValueExW` / `RegCreateKeyExW` / `RegDeleteValueW`（含 `OpenProcessToken`、ACL API）
- **外发/执行**：`ShellExecuteW`、`ShellExecuteExW`、`SHFileOperationW`、`WS2_32`（按序号）、`NETAPI32`、`PlaySoundW`
- **管道**：`PeekNamedPipe`

`QQImeUtil_x64.dll`：`WS2_32`、`CoCreateInstance`（COM）、`StgCreateDocfile`/`StgOpenStorage`（OLE 结构化存储）、
`CreateToolhelp32Snapshot`、`DeviceIoControl`、`GetDriveTypeW`、`GetLogicalDrives`、`mouse_event`/`SetCursorPos`。

`QQWubiTSF.dll`：`SendMessageW`/`PostMessageW`、`SetWindowLongPtrW`、`ToUnicode`、`MapVirtualKeyW`、
`ShellExecuteW`、`SHFileOperationW`、`WTSQuerySessionInformationW`、`ImmCreateContext` 等；**无网络 API**。

> 结论性判断（谨慎）：`QQWubi.ime` 同时具备**全局键盘钩子 + 合成输入 + 剪贴板 + 进程枚举 + WinSock** 的导入能力
> （x86 版还多出跨进程读内存），而它恰恰运行在浏览器、聊天工具、编辑器等所有宿主进程内。
> 这是**能力面很大**的设计：仅就权限而言，它在这些进程里都"能做"上述操作。
> 这些能力在成熟输入法里通常用于候选窗跟随、兼容性适配、软键盘、剪贴板取词等正当功能；
> 但"哪些能力真被调用、在什么条件下触发"需要动态验证（本次未做，见第 10 节）。
> 同理，`QQWubiTSF.dll` 完全没有网络导入，而引擎有——这也反过来说明联网逻辑属于引擎/服务层，不在 TSF 胶水层。

其它观察：`Software\Tencent\QQWubi\ValidKey`、`qqwbacc.dat`、`openacc3.dat`（3 KB 高熵内容）、
`QQWBUserCenter.exe`、`SAccount\<32位MD5>`（按账号隔离配置目录）——存在账号/激活/等级一类的机制，
本次未深入（未知）。

---

## 9. 已确认 / 推断 / 未知 一览

**已确认（静态直接证据）**

- 组件层次、导出集合、架构位数、哈希
- TSF 注册项与"无 IMM 键盘布局"→ TSF-only
- 引擎类结构（词库/候选/皮肤三簇）、词库文件族与魔数
- `.qpyd` 容器校验算法（魔数 `0x7D1EA609` + `+0x48` 整文件校验和）
- `ImeToAsciiEx` → `FUN_1800763e0` → 引擎 vtable+0x30 的调用结构
- `ImeEscape` 只支持 `3` / `0x1006`
- 与 QQ拼音共用 `qqinput/qqwubi_2_4` 代码库（构建路径字符串）
- 引擎/服务被加载进 16 个现有 GUI 进程
- x86 与 x64 两份 `.ime` 导出集合完全相同（同样 18 个 `Ime*`）；x86 版额外导入
  `ReadProcessMemory`/`VirtualQueryEx`/`GetThreadSelectorEntry`，x64 版没有

**推断（有依据但未逐条验证）**

- 各命名对象的具体用途（SHAREMAP=运行态共享、TYPE_STATISTIC_SHARE_MAP=统计合并）
- `NetWkstaTransportEnum`/`Netbios` 用于设备指纹
- 服务器 JSON 属于"打字统计/等级"而非词库同步
- 7 个 TSF 类别 GUID 各自的标准语义
- `ws2_32` 的 9 个序号各自对应的 API 名

**未知（本次未覆盖）**

- `SBWQ` 码表偏移表与条目编码细节
- 五笔码→候选的完整算法（码表查找、简码/全码、词组、自动造词打分、用户词频提升）
- 上传报文的字段与触发条件（**无抓包**）
- 更新链路（`QQWBLiveup.exe` + `Tencentdl.exe` + `dlcore.dll`）的下载校验方式
- `pinyin.dict` 头部混淆/加密算法
- 第 8 节所列高危 API（钩子、`SendInput`、剪贴板、`ReadProcessMemory`）的**实际调用点与触发条件**：
  `rea xrefs` 对 `.ime` 大快照逐条查询过慢（单次数十秒、10 条超时），本次未完成

---

## 10. 复现方式

```powershell
# 快照分析（一次生成，后续反编译复用，避免重复 Ghidra 全量分析）
rea analyze 'C:\WINDOWS\system32\IME\QQWubiTSF\QQWubi.ime' `
  --snapshot .\evidence\ime_x64.snapshot.json --format json

# 从字符串找引用 → 反编译
rea search  <ime> --pattern 'wubi.dict' --mode literal --snapshot .\evidence\ime_x64.snapshot.json
rea xrefs   <ime> --address 0x1804c2018 --snapshot .\evidence\ime_x64.snapshot.json
rea function <ime> --procedure 0x18022c909 --snapshot .\evidence\ime_x64.snapshot.json

# 词库校验算法
rea function '...\QQImeUtil_x64.dll' --procedure Ime_IsDictFileValid --snapshot .\evidence\qimeutil_x64.snapshot.json
```

注意：本机 DSH 沙箱在 `workspace-write` 模式下会禁止 Node 通过管道 spawn 子进程，
导致 REA 的 Ghidra provider 报 `runtime_missing`（"Update JAVA_HOME…"）——**那是沙箱假象，不是 JDK 问题**。
需在 `danger-full-access`（或不限制管道的环境）下运行 Ghidra headless。

---

## 附：证据索引

| 文件 | 内容 |
|---|---|
| `evidence\analyze_tsf_x64.json` | `QQWubiTSF.dll` 概览（1 163 函数 / 639 字符串） |
| `evidence\analyze_ime_x64.json` | `QQWubi.ime` 概览（26 583 函数 / 3 565 字符串 / 12 段含 `.ShareDa`） |
| `evidence\analyze_qimeutil_x64.json` | `QQImeUtil_x64.dll` 概览（12 482 函数 / 2 098 字符串） |
| `evidence\qimeutil_x64_artifact.json` | 产物图清单 + SHA-256 + 后续探查建议 |
| `evidence\pe_tables_ime.txt` | x64 `.ime` 导出（18 个 `Ime*`）与导入表 |
| `evidence\pe_tables_ime_x86.txt` | x86 `.ime`（`SysWow64`）导出/导入，用于两版对照 |
| `evidence\pe_tables_engine_x64.txt` / `pe_tables_engine_x86.txt` | 引擎服务 DLL 的 28 个导出（Ime*/SknEng*）与导入表 |
| `evidence\pe_tables_tsf.txt` | TSF 的 4 个 COM 导出与导入表 |
| `evidence\strings_ime_x64.txt` / `strings_tsf_x64.txt` / `strings_qimeutil_x64.txt` | 全量字符串（`地址<TAB>内容`） |
| `evidence\fn_toascii_core.json` 等 `fn_*.json` | 反编译结果（含伪代码、调用者/被调用者、引用字符串） |
| `evidence\ime_x64.snapshot.json` / `tsf_x64.snapshot.json` / `qimeutil_x64.snapshot.json` | REA 分析快照（可复用） |
