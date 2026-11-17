# 测试指南（v0.1：TSF 外壳 + 引擎）

## 当前状态

| 项 | 状态 |
|---|---|
| 引擎（词库加载 / 码查询 / 候选排序） | 已完成，`panda-cli.exe` 可独立验证 |
| 简繁 / 标点矩阵 / 用户词自学习 | 已完成并验证 |
| 组码状态机（`Composer`，纯逻辑） | 已完成，`wubi-cli sim` 可验证 |
| 配置迁移（QQ五笔 Config.ini） | 已完成并验证 |
| TSF 文本服务（COM、组码、候选窗、上屏） | 已实现，x64 + x86 双架构编译通过 |
| 自注册（CLSID / TIP / LanguageProfile） | 已实现；**需要管理员权限**才能生效 |
| 实测输入 | **待你完成一步提权注册后验证** |

## 为什么需要提权

TSF 的输入法目录是机器级的：`ITfInputProcessorProfiles` 读写
`HKLM\SOFTWARE\Microsoft\CTF\TIP`。当前账户写入该键被拒绝（实测 `E_ACCESSDENIED`），
所以 `AddLanguageProfile` / `EnableLanguageProfile` 全部返回 `E_FAIL`。
程序现在会在权限不足时**明确返回 `E_ACCESSDENIED`**，不再静默假成功。

## 一次性注册（约 10 秒）

1. 打开 `D:\MobileSync\hanfu\src\wubi\dist`（脚本会同时注册 x64 与 x86 两个 DLL，并生成 `%APPDATA%\PandaWubi\config.ini`）
2. 右键 `register.cmd` → **以管理员身份运行**（或双击后在 UAC 点"是"）
3. 脚本会自己校验注册结果：成功打印 `[OK] HKLM TIP + CLSID created.`；失败打印 `[FAIL]` 并自动输出日志与可能原因

反向操作：`unregister.cmd`（同样需要管理员）。

## 启用并试用

1. 设置 → 时间和语言 → 语言和区域 → 中文(简体) → 语言选项
   → **添加键盘** → 选择「QQ五笔替代输入法」
2. 打开记事本，按 `Win+Space` 切到它
3. 输入测试：

| 输入 | 期望 |
|---|---|
| `a` | 候选窗出现，第 1 项「工」 |
| `空格` | 上屏「工」 |
| `trnt` | 候选「我」在第 1 位 |
| `wqvb` | 候选「你好」在第 1 位 |
| `3` | 选第 3 个候选 |
| `Esc` | 取消当前组码 |
| `Backspace` | 删一个码 |
| `Ctrl+Shift+T` | 切换简繁输出 |
| `Ctrl+Shift+P` | 切换中文标点接管 |
| `,` `.` `?` `"` | 输出 `，` `。` `？` `“`（再按变 `”`） |

中文标点默认开启（与 QQ五笔 一致），接管 `, . ; : ? ! ( ) [ ] < > " '' ^ _ -` 等键；
**带 Shift 的标点**（`;`→`:`、`'`→`"`、`1`→`!`、`-`→`_`、`=`→`+`）同样走标点表，
不会被误当成"选第 N 候选"或"翻页"。
输入法英文半角模式尚未实现，需要时用 `Ctrl+Shift+P` 临时关掉。

## 已知限制（v0.1）

- **仅 x64**：64 位程序可用；32 位程序需要额外的 x86 构建
- 组码文字没有下划线样式（未设置 display attribute，仅影响外观）
- 候选窗不支持鼠标点选（目前只支持数字键 1–9）
- 未实现：简繁转换、拼音混输、标点矩阵、用户词持久化、皮肤
- 词库首次使用时加载（约 3.3 MB TSV），每个宿主进程各一份；
  后续应改为共享内存只读映射

## 自定义词组 / 自定义短语

| 快捷键 | 模式 | 用法 |
|---|---|---|
| `Ctrl` + `=` | 自定义词组 | 进入时**自动带上最近输入的两个字**（常见情形直接回车就好）；`←` 再往前取一个字、加到最前面 · `→` **缩一个**（把最近取回的那个字退回队列，可以再用 `←` 取回来） · `Del` 删掉最前面那个字（真正丢弃） · `Backspace` 删掉最后面那个字 · `回车` 按五笔规则自动取码并入库 · `Esc` 取消 |
| `Ctrl` + `Shift` + `=` | 自定义短语 | 弹出**独立窗口** `panda-phrase.exe`：上面填编码（`a–z`，最多 4 位），下面填短语（`RichEdit`，中文直接打、邮箱/身份证号也能写）。`保存`/`回车` 落盘，`Esc` 取消 |

取码规则（`wubi::PhraseCode`，用每个字的**全码**）：2 字各取前两码、3 字前两字首码+第三字前两码、
4 字各取首码、5 字以上前三字首码+末字首码。例如 `你好`→`wqvb`、`计算机`→`ytsm`、
`中华人民共和国`→`kwwl`；两个字的词组如 `工人`→`aaww`（工 aaaa 前两码 + 人 wwww 前两码）。

保存进 `%APPDATA%\PandaWubi\user.tsv`（`编码 <TAB> 内容 <TAB> 权重`，**追加**写入）。
输入法每秒按文件修改时间检查一次，外部窗口或别的进程新加的词会热加载，不用重启。

窗口为什么是独立进程：输入法跑在宿主应用的 UI 线程里，在里面弹模态循环会和 TSF 的焦点/生命周期打架；
窗口用的是 RichEdit 控件，所以本输入法能在里面正常打中文。

**打错再退格的情况**：进词组模式时会读一次"光标前实际还留着的文本"，把最近上屏队列里
已经被删掉的字剔掉（`wubi::TrimCharsByText`，从后往前按子序列匹配）。前提是宿主能把光标位置
报给 TSF；`RichEdit`／记事本这类宿主报的是 0（实测 `ITfRangeACP::GetExtent` 恒为 0），
这种情况下自动剔除会跳过，用 `Del`／`Backspace` 手动删即可。
退格键本身**不能**由输入法声明（声明了宿主会把这一键丢掉、不再删字），所以不走"监听退格"这条路。

端到端（`[C]`=Ctrl+= · `[S]`=Ctrl+Shift+= · `[L]`=← · `[D]`=Del · `[E]`=回车 · `[Q]`=Shift+'）：

```powershell
# 打「工」「人」，再取字入库 -> aaww 工人（← 把新字放最前面，字序不会反）
.\build\tsf-host.exe -send "a w [C][L][L][E]"
.\build\tsf-host.exe -send "aaww "            # -> 工人

# Ctrl+Shift+= 拉起短语窗口（编码/短语由窗口填，保存后热加载）
.\build\tsf-host.exe -send "[S]"
.\dist\panda-phrase.exe                        # 也可以直接手动开
.\build\tsf-host.exe -send "dz "               # -> 窗口里填的那条短语
```

## 设置界面（panda-settings.exe）

`dist\panda-settings.exe`（或设置窗口里的「设置」）打开：

- **编译成 GUI 子系统**（`/SUBSYSTEM:WINDOWS`）。以前是控制台程序，被输入法拉起时会先闪一个黑色控制台窗口；
  现在不会了。`/dump`、`/set` 仍可用：stdout 被重定向时直接用继承的句柄，从终端手动运行时才接父进程控制台。
- 版面按 DPI 缩放、用分组框分段（一级简码 / 候选排序 / 输入选项 / 候选窗外观 / 词库），
  统一 微软雅黑 9pt，底部按钮右对齐，窗口居中后一次性显示（先建在屏幕外、摆好位置再 `ShowWindow`，避免闪一下空窗）。
- 保存后弹提示：新设置对之后启动的程序生效。

## 悬浮状态栏（中 简 ，。 ⚙）

桌面右下角那条自绘状态栏由 `src/tsf/wubi_bar.cpp` 实现，**每个进程最多一条**：

- 进程内唯一：TSF 会为每个线程各建一个 `CTextService` 实例，实例共用一条栏，实例计数归零才销毁窗口；
- 只在宿主进程处于前台时显示（线程焦点回调立即响应，500ms 前台自检兜底），所以切走应用后不会留下一条悬在别人窗口上；
- 状态栏点击回到"当前拥有者"实例，中/英、简/繁、标点三项都走同一份 `config.ini`。

回归测试（不需要注册、不依赖 HKLM）：

```powershell
.\build\bar-singleton-test.exe .\dist\panda_tsf.dll
```

`bar-singleton-test` 断言：激活前 0 条 → 1 个实例 1 条 → **2 个实例仍 1 条** → 部分释放仍 1 条 →
全部释放 0 条 → 非前台时必须隐藏。旧实现会在第 3 步变成 2 条、并在切走应用后保持可见。
`tools\verify.ps1` 已包含这三项检查。

升级后如果桌面上还留着旧 DLL 副本建的栏（它们属于仍在运行的进程），跑一次：

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\hide-stale-bars.ps1          # 隐藏
powershell -ExecutionPolicy Bypass -File .\tools\hide-stale-bars.ps1 -List    # 只列出
```

或者干脆重启那些宿主程序（实测是 explorer、微信、Everything 等）；新代码只在宿主进程位于前台时显示 1 条栏。

## 英文单词补全（Shift+字母）

`Shift+字母` 进入补全模式后，**大小写照抄键盘输入**（Shift 与 CapsLock 相异即为大写），
不再一律当小写：

| 实际按键 | 候选/上屏 |
|---|---|
| `a` `b` | `about` |
| `Shift+A` `b` | `About`（单个大写字母 = 首字母大写） |
| `Shift+A` `Shift+B`（或开 CapsLock） | `ABOUT`（连续大写 = 整词大写） |
| `Shift+A` `b` `Shift+O` | `AbOut`（逐字保留） |

规则实现在 `wubi::ApplyCase()`（`src/engine/wubi_english.cpp`），纯函数、可单测：
`panda-cli <dict> engcase` 跑 9 组用例，`tools\verify.ps1` 里对应「大小写照抄输入」一项。

端到端（真实键盘事件，会短暂抢占前台）：
```powershell
.\build\tsf-host.exe -send "Ab "     # -> About
.\build\tsf-host.exe -send "AB "     # -> ABOUT
.\build\tsf-host.exe -send "wqvb "   # -> 你好（中文不受影响）
```

## 如果行为不对，怎么报给我

日志在 `%LOCALAPPDATA%\PandaWubi\wubi_tsf_<pid>.log`（每个宿主进程一个）。里面记录：

```
DllMain: attach pid=...
dict load: path=... entries=147697 codes=104961 ok=1
ActivateEx: tid=... punc=1 trad=0 candCount=9
StartComposition: hr=0x00000000 comp=0000...
key w -> code=w act=1
```

把它发给我就能定位是"没激活 / 词典没载入 / 组码没起来 / 候选窗定位失败"中的哪一种。
注册失败时日志里会有 `DllRegisterServer: HKLM denied (need admin)`。

## 自检命令

```powershell
# 一键全量验证（构建 + 引擎 + 数据 + TSF 组件）
powershell -ExecutionPolicy Bypass -File .\tools\verify.ps1

# 引擎（不依赖 TSF）
.\dist\..\build\panda-cli.exe .\dist\data\qqwubi86.dict.tsv

# TSF 注册 + COM 激活 + 目录枚举
.\dist\tsf-selftest.exe .\dist\panda_tsf.dll
```

`tsf-selftest` 退出码：`0` 全部通过 · `1` 失败 · `3` 需要管理员权限。
