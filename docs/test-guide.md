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

## 自定义短语窗口

`Ctrl+Shift+=` 或设置窗口里打开。**Tab / Shift+Tab 在编码框 → 短语框 → 保存 → 取消 之间循环切换焦点**
（回车=保存，Esc=取消）。

编码框只接受**英文字母**（a–z / A–Z），最多 4 位；数字、标点、汉字都打不进去，
粘贴/输入法带进来的也会被清掉（`wubi::FilterCode`，CLI 里 `panda-cli <dict> codefilter` 有自测）。

## 分号与标点

| 操作 | 结果 |
|---|---|
| `;` | 进入便捷输入（数字转中文），不输出分号 |
| `;` 再按一次 `;`（还没输数字） | 上屏一个**全角分号 `；`**（中文标点关闭时是 `;`） |
| `;123` + `空格`/`;`/`'` | 一二三 / 一百二十三 / 壹佰贰拾叁 |
| `Shift+;` | 冒号 `：` |

## 候选窗外观

设置窗口的「候选窗外观」一组里可以调（改完保存，重启宿主程序生效）：

| 项 | 范围 / 说明 |
|---|---|
| 候选个数 | 1–9 |
| 字号 | 8–24 pt（行高跟着字号自动放大，不会压字） |
| 字体 | 下拉里是所有已装字体（跳过 `@竖排` 字体），也可以直接输入字体名，比如 `KaiTi`、`Microsoft YaHei UI` |
| 背景色 / 文字色 / 选中底色 / 选中文字 / 边框色 | `#RRGGBB`，第一候选（选中项）用「选中底色 + 选中文字」画底和高亮 |

对应 ini 键：`CandCount`、`CandFont`（pt）、`CandFontFace`、`CandBgColor`、`CandTextColor`、
`CandSelBgColor`、`CandSelTextColor`、`CandBorderColor`、`CandInset`、`CandVertical`。

> 注：设置里存的是网页习惯的 `0xRRGGBB`，GDI 的 `COLORREF` 是 `0x00BBGGRR`，
> 代码里必须换序——之前没换，红蓝是颠倒的（默认色都接近白，所以一直没暴露）。

## 数据目录（换机/备份）

输入法不自建云端，改为**让用户指定一个目录**，所有"个人数据"都在里面：

| 文件 | 内容 |
|---|---|
| `config.ini` | 全部设置（候选个数/字号/排序/简繁/标点/数据目录指针之外的项……） |
| `user.tsv` | 自定义词组、自定义短语、词频自学习结果 |

- 默认 `%APPDATA%\PandaWubi`；在设置窗口最下面的「数据目录」里**浏览…**选一个目录即可
  （选 OneDrive / 坚果云 / 百度网盘的同步夹最省事，不用自己架服务器）。
- 切换时会**先把旧目录里缺的 `config.ini`、`user.tsv` 复制到新目录**，不会丢东西；
  之后所有读写都走新目录（输入法侧热加载 user.tsv，配置重启后生效）。
- 「打开」直接打开该目录；「默认」一键指回 `%APPDATA%\PandaWubi`。
- 指针本身存在 `HKCU\Software\PandaWubi\DataDir`（机器级，不进同步目录），
  所以换机器后只要在这个窗口里把目录指回云盘那份，设置和自定义词立刻回来。
- 命令行（脚本/免界面）：
  ```powershell
  .\dist\panda-settings.exe /dump                      # 会打印 datadir / userdict
  .\dist\panda-settings.exe /datadir "D:\OneDrive\PandaWubi"
  .\dist\panda-settings.exe /datadir default            # 指回默认
  ```
- 测试/便携：`WUBI_DATA_DIR` 环境变量优先级最高（verify.ps1 用它验证目录切换）。

> 词库本体（`qqwubi86.bin` 等）是**程序数据**，放在 `dist\data`，不进数据目录——
> 换机时拷贝 `dist` 整个目录 + 指回数据目录，就是完整恢复。

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

## 组码不上屏（编码只在候选窗里显示）

组码阶段的字母**不会写进文档**，编码只显示在候选窗顶部的小框里；汉字上屏后文档里直接出现汉字，
不存在"先出现字母再被替换"的过程。

```powershell
.\build\tsf-host.exe -send "wqvb"     # 文档里是空的（编码 wqvb 只在候选窗顶部）
.\build\tsf-host.exe -send "wqvb "    # -> 你好
.\build\tsf-host.exe -send "wqvbx"    # -> 你好（顶字上屏后继续组码 x）
```

英文补全模式仍然把字母显示在文档里——那本来就是最终要输出的内容。

**任何一个候选都没匹配到时，编码框仍然保留**（不再是整块消失）：屏幕上一眼能看到刚敲的码，
不会出现"还在组码却什么都没有"的困惑。例如 `xxqq`（词库里没有）：

```powershell
.\build\tsf-host.exe -send "xxqq"     # 文档空，候选框只剩顶部编码框显示 xxqq
```

空码（一个候选都没有）时不会卡住，下面几种操作都会**直接把空码清掉**：

| 操作 | 结果 |
|---|---|
| `回车` / `空格` | 空码丢弃，不写任何文字 |
| 继续打字母 | 丢掉旧码，用这一键重新组码（`xxqqa` → 工） |
| 打标点 | 丢掉空码，只输出这个标点（`xxqq,` → ，） |
| `退格` | 照常逐位删码，删到有候选就恢复 |

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

候选窗**顶部小框显示的就是你实际敲的样子**（含大小写），不再显示上一段中文组码：

| 键 | 效果 |
|---|---|
| `空格` | 选第 1 个候选补全，并补一个空格（`Ab ` → `About `） |
| `回车` | **不补全，原样上屏**顶部小框里的内容（`AbC⏎` → `AbC`） |
| `1`–`9` | 选第 N 个候选（同样补空格） |
| `退格` | 删掉一个字母；删空即退出英文模式 |
| `Esc` | 全部取消，什么都不上屏 |

上屏/取消后顶部小框会清空，不会残留到下一次。

**没匹配到任何单词时，候选框不会消失**，而是只剩顶部小框显示当前输入（不再画翻页箭头和熊猫装饰），
这样你随时能看清自己敲了什么；一旦继续敲出匹配的字母，候选行会重新出现。

端到端（真实键盘事件，会短暂抢占前台）：
```powershell
.\build\tsf-host.exe -send "Ab "     # -> About
.\build\tsf-host.exe -send "AB "     # -> ABOUT
.\build\tsf-host.exe -send "wqvb "   # -> 你好（中文不受影响）
.\build\tsf-host.exe -send "AbC[E]"  # -> AbC（回车原样上屏，不补全）
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
