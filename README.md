# QQ五笔替代输入法（Windows 11，TSF）

以 REA 对 **QQ五笔输入法 2.4** 的逆向分析为事实基础，自研的 Windows TSF 输入法。
目标：在 Win11 上提供稳定、行为接近 QQ五笔 的五笔输入能力，且不复刻它的架构缺陷。

---

## 快速开始

```powershell
# 1. 一键验证（构建 + 引擎 + 数据 + TSF 组件 + 状态栏，共 96 项）
powershell -ExecutionPolicy Bypass -File .\tools\verify.ps1

# 2. 注册（需要管理员，二选一）
#    a) 右键 dist\register.cmd -> 以管理员身份运行
#    b) 管理员 PowerShell 中：
cd dist
regsvr32.exe /s panda_tsf.dll
"$env:WINDIR\SysWOW64\regsvr32.exe" /s panda_tsf32.dll

# 3. 启用：设置 -> 时间和语言 -> 语言和区域 -> 中文(简体) -> 语言选项
#          -> 添加键盘 -> 「QQ五笔替代输入法」
# 4. 记事本里按 Win+Space 切过去，输入 a / trnt / wqvb 试
```

卸载：`dist\unregister.cmd`（同样需管理员）。

---

## 为什么需要管理员

TSF 输入法的 profile 目录是**机器级**的：`HKLM\SOFTWARE\Microsoft\CTF\TIP`。
已验证三条注册路径全部失败（非管理员时）：

| API | 结果 |
|---|---|
| `ITfInputProcessorProfiles::AddLanguageProfile` | `E_FAIL` |
| `ITfInputProcessorProfiles::EnableLanguageProfile` | `E_FAIL` |
| `ITfInputProcessorProfileMgr::RegisterProfile`（Win8+） | `E_FAIL` |

写入 `HKLM\SOFTWARE\Microsoft\CTF\TIP` 直接返回 `E_ACCESSDENIED`。
用户级 `HKCU\Software\Microsoft\CTF\TIP` 只是 Windows 为机器级注册写的**镜像**，
只写 HKCU 不会被 `EnumLanguageProfiles` 采纳（实测：枚举到 5 个 zh-CN 配置，不含我们）。

DLL 在权限不足时**明确返回 `E_ACCESSDENIED`**，不静默假成功。

---

## 目录结构

```
src/engine/     词库（TSV/二进制 mmap）、码生成、标点矩阵、简繁、用户词、组码状态机
src/tsf/        TSF 文本服务、候选窗、自注册、诊断日志
src/ui/         候选窗几何（纯函数，可单测）
src/cli/        wubi-cli（验证 harness）、tsf-selftest（TSF 自检）
tools/          提取器、合成器、验证脚本、排序分析
data/           词库与数据文件
dist/           交付物（双架构 DLL + 数据 + 注册脚本）
docs/           design.md（设计与进度）、test-guide.md（测试指南）
evidence/       REA 原始证据（分析 QQ五笔 的产物）
```

---

## 数据来源

| 资产 | 来源 | 数量 |
|---|---|---|
| 系统词表 | **REA 提取 QQ五笔 `wubi.dict`（SBWQ）** | 94,938 条 |
| 用户自造词（带码） | **REA 提取 `WBUser3.dict`（2UWQ）** | 19,080 条 |
| 字→全码 | 本机 Rime 小狼毫 `wubi86.dict.yaml` | 27,515 字 |
| 词频权重 | 同上（Google 词频 / 极爽6） | 136,235 条 |

最终词库 `data/qqwubi86.bin`：**147,697 条**（1 码 25 · 2 码 616 · 3 码 7,014 · 4 码 140,042）。

**正确性闭环**：用五笔86标准规则重算 QQ 用户词库里的 18,681 条 `码↔词`，
完全一致 16,519 + 简码前缀一致 2,158 = **99.98%**。两条例外已核实为 QQ 特有变体
（`跟踪` QQ 存 `kvkp` vs 标准 `khkh`；`齡` 繁体字形差异）。

---

## 性能与稳定性（实测）

| 指标 | 数值 |
|---|---|
| 词库载入（mmap） | **0.1 ms**（TSV 为 74.7 ms） |
| 前缀查询 | **~40 µs/次** |
| 全码表压力（16,250 次） | 1.8 µs/次，最大 9 候选 |
| 内存（私有提交） | **0.5 MB**（TSV 为 28.8 MB）；工作集 3.6 MB |
| 随机按键模糊测试 | 5 万次序列，零不变量违规 |

内存这一项在 16 个宿主进程里线性叠加——TSF 输入法会加载进每个使用输入法的进程，
所以从 28.8 MB 降到 0.5 MB 对实际体验影响最大。

---

## 功能状态

已实现并验证（不需要激活环境）：

- 五笔86 码表 + 词组（147,697 条）
- 一级/二级/三级简码
- 组码状态机（输入、数字选词、空格上屏、退格、取消）
- 简繁转换（`simp2trad.dat`，65,536 项映射）
- 中文标点矩阵（`punc.ini`，4 列模式 + 引号成对）
- 用户词自造 + 词频自学习 + 持久化
- 配置迁移（直接沿用 QQ五笔 `Config.ini` 的键名）
- 候选窗几何：横排/竖排 + 鼠标点选命中
- 诊断日志 `%LOCALAPPDATA%\PandaWubi\wubi_tsf_<pid>.log`

已实现但**只能激活后验证**：

- ~~组合串显示~~ → 已验证（真机日志）
- ~~上屏到真实应用~~ → 已验证（`textlen` 与词长一致）
- 候选窗实际定位、简繁/标点/鼠标点选的实际效果 → 待观察

未实现：

- 五笔/拼音混输
- 皮肤系统
- 自建 `ITfDisplayAttributeProvider`（当前复用系统第一个显示属性）

---

## 架构要点（相对 QQ五笔 的改进）

| QQ五笔 2.4 | 本实现 |
|---|---|
| 6.6 MB 引擎在每个宿主进程内，首次按键解析词库 | 词库 mmap，0.1 ms，多进程共享物理页 |
| IMM32 + 老式窗口，靠 `SetCursorPos`/`mouse_event` 跟光标 | 纯 TSF：`GetTextExt` + 插入符兜底，不用全局钩子 |
| 引擎同时具备全局钩子/合成输入/剪贴板/进程枚举能力 | TSF DLL 无网络、无钩子、无剪贴板 |

---

## 验收状态

```
tools\verify.ps1  →  51 项 PASS / 0 FAIL / 0 SKIP
```

真机端到端已验证（2026-11-16）：组码累积、候选显示、空格上屏，
`CommitText` 的 `textlen` 与词长严格对应（`trnt`→1 字「我」、`wqwu`→2 字「你们」）。

## 设置

双击 `dist\settings.cmd`（或 `dist\panda-settings.exe`）打开设置窗口：

| 选项 | 说明 |
|---|---|
| 一级简码处理 | 不处理 / 隐藏 / 沉底（默认）——多键输入时如何对待"一键就能打出来的字" |
| 候选排序 | 码短优先（默认，与 QQ五笔 接近）/ 纯词频优先 |
| 中文标点 / 输出繁体 | 对应 Ctrl+Shift+P / Ctrl+Shift+T，改动会持久化 |
| 候选个数 | 1–9 |

保存后对**新启动的程序**生效；已打开的程序需重启。

## 图标与外观

- 应用图标：`dist\panda.ico`（五 + PP 角标）、`dist\ppwubi-en.ico`（英文态）
- **语言指示器图标**：由注册表指向 `dist\panda.ico` 的固定路径 —— **不要删除或移动该文件**，
  否则 `Win+Space` 的输入指示器会退回显示「简体」（且需重启资源管理器才能恢复）
- 生成脚本：`tools\make-icon.ps1`（可传 `-Glyph` / `-OutFile` 换字与产物）
- 托盘程序：`dist\ppwubi-tray.exe`（显示中/英状态，可打开设置）

## 已知限制

- 仅 zh-CN 语言配置
- 组码最长 4 码；未实现词组联想的主动提示
- **候选排序与 QQ五笔 首位一致 6/6**（用 `tools\qq-oracle.exe` 直接驱动 QQ五笔 的 IMM 引擎取真值对比）；仍有差异的是部分非首位，例如 `a` 的 QQ 顺序为 `工 戈 或 其 工作`（QQ五笔 的候选顺序存在
  `wubi.dict` 索引区的桶顺序里，静态不可恢复（实测 `A` 字段与词频一致率 59.3%、
  `B` 仅 27.9%，接近随机）。码表本身已 99.98% 对齐。
- `dist\` 下可能残留 `*.old.dll`：热替换时旧映像被运行中的宿主进程占用，
  重启这些程序后才能删除
- 词库源自腾讯 QQ五笔 与 Rime 词库，**仅供本地个人使用**；
  若要公开发布应替换为可再分发词库（Rime wubi86 为 Apache-2.0）

---

## 排障

日志在 `%LOCALAPPDATA%\PandaWubi\wubi_tsf_<pid>.log`（每个宿主进程一个）：

```
DllMain: attach pid=...
dict load(bin): path=... entries=147697 ok=1
ActivateEx: tid=... punc=1 trad=0 candCount=9
display attribute: { xxxxxxxx-xxxx-xxxx } atom=42
StartComposition: hr=0x00000000 comp=...
key w -> code=w act=1
```

按顺序看这四行就能定位是"没激活 / 词库没载入 / 组码没起来 / 候选窗定位失败"哪一种。
注册失败时会有 `DllRegisterServer: HKLM denied (need admin)`。
## 文件清单（dist）

| 文件 | 说明 |
|---|---|
| panda_tsf.dll | 64 位 TSF 输入法（主程序） |
| panda_tsf32.dll | 32 位版本（给 32 位程序用） |
| panda-settings.exe | 设置窗口 |
| panda-cli.exe | 命令行工具（lookup / sim / verify 用） |
| panda.ico | 图标（语言指示器引用这个路径，**不要移动或删除**） |
| egister.cmd / unregister.cmd | 注册 / 反注册（需管理员） |
| settings.cmd | 打开设置 |
| config.ini | 默认配置 |
| data\ | 词库与英文词表 |

## 数据位置

| 内容 | 路径 |
|---|---|
| 配置 | %APPDATA%\PandaWubi\config.ini |
| 用户词 | %APPDATA%\PandaWubi\user.tsv |
| 日志 | %LOCALAPPDATA%\PandaWubi\panda_tsf_<pid>.log |

> 从旧版本（WubiTsf 目录）升级时，首次运行会自动把 config.ini 与 user.tsv 复制过来。

## 数据准备（重要）

为了合规，仓库不含任何第三方词库。首次使用需要自行生成 data 下的文件：

```powershell
# 1. 编译
build.bat

# 2. 用 Rime 的 wubi86 词库生成主码表（开源授权）
powershell -File tools\build-data.ps1 -RimeYaml "C:\Rime\wubi86.dict.yaml"

# 3. 可选：英文词表（QQ五笔 engcore.dat）与简繁映射（OpenCC）
powershell -File tools\build-data.ps1 -RimeYaml "C:\Rime\wubi86.dict.yaml" -EngSource "C:\...\engcore.dat" -OpenCC "C:\opencc\STCharacters.txt"

# 4. 复制到运行目录
xcopy /y data\*.tsv data\*.bin data\*.dat data\eng_words.txt dist\data\
```

细节见 data/README.md。缺失数据时的行为：

| 缺失文件 | 影响 |
|---|---|
| qqwubi86.dict.tsv / .bin | 无法输入，必须生成 |
| eng_words.txt | 英文单词补全不可用 |
| simp2trad.dat | 简繁输出无效果（其它正常） |

## 仓库结构

```
src/engine/     组码与词典引擎（纯逻辑，可单测）
src/tsf/        TSF 外壳（键盘处理、候选窗、语言栏）
src/ui/         设置窗口
src/cli/        命令行工具（查询、模拟、打包、自检）
tools/          数据生成与验证脚本
data/           数据目录（词库不进仓库，见 data/README.md）
dist/           发布目录（二进制与图标）
docs/           设计文档与测试指南
```

## 第三方素材

| 素材 | 来源 | 许可 |
|---|---|---|
| 候选框装饰图（熊猫吃竹子） | 项目作者提供（豆包 AI 生成，已裁去生成标记） | 由作者确认可用 |
| 五笔词库 | Rime wubi86（自备，见 data/README.md） | 遵循上游授权 |
| 英文词表 / 简繁表 | 自备，见 data/README.md | 遵循上游授权 |

装饰图源文件：`src/tsf/panda_bamboo.png`（160×160，透明背景，作为 RCDATA 资源 3 嵌入 DLL）。
重新生成：用 Edge 无头模式栅格化 CC0 SVG，再裁剪缩放（见 tools/README 或 git 历史）。