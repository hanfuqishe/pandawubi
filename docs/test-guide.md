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
输入法英文半角模式尚未实现，需要时用 `Ctrl+Shift+P` 临时关掉。

## 已知限制（v0.1）

- **仅 x64**：64 位程序可用；32 位程序需要额外的 x86 构建
- 组码文字没有下划线样式（未设置 display attribute，仅影响外观）
- 候选窗不支持鼠标点选（目前只支持数字键 1–9）
- 未实现：简繁转换、拼音混输、标点矩阵、用户词持久化、皮肤
- 词库首次使用时加载（约 3.3 MB TSV），每个宿主进程各一份；
  后续应改为共享内存只读映射

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
