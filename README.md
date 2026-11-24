# 熊猫五笔 · PandaWubi

Windows 11 上的**自研 TSF 五笔输入法**（五笔86）。以 REA 对 QQ五笔输入法 2.4 的逆向分析为事实基础，
目标是在 Win11 上提供稳定、手感接近 QQ五笔 的五笔输入，同时不复刻它的架构缺陷（IMM32、全局钩子、每进程 6.6 MB 引擎）。

- 纯 TSF（`ITfTextInputProcessorEx` + 自绘候选窗/悬浮栏），无全局钩子、无网络、无剪贴板
- 词库 mmap 加载 0.1 ms，多进程共享物理页（TSV 模式约 75 ms / 29 MB）
- 4 套主题（熊猫 / 狸花猫 / 中华田园犬 / 丹顶鹤）、悬浮状态栏、Z 键拼音反查五笔编码
- 免安装也可用：`regsvr32` 两个 DLL 即可

> **下载**：[最新安装包（GitHub Releases）](https://github.com/hanfuqishe/pandawubi/releases/latest)
> · 直接下载 [PandaWubi-Setup-1.1.2.exe](https://github.com/hanfuqishe/pandawubi/releases/download/v1.1.2/PandaWubi-Setup-1.1.2.exe)（13.9 MB）

---

## 安装

### 方式一：安装包（推荐）

下载 `PandaWubi-Setup-1.1.2.exe`（约 13 MB，Inno Setup 打包），双击安装：

| 项 | 内容 |
|---|---|
| 安装位置 | `C:\Program Files\PandaWubi` |
| 权限 | 需要管理员（TSF 的语言配置是机器级的，见下文） |
| 内置数据 | 主码表 `qqwubi86.dict.tsv`、拼音反查表 `pinyin.tsv`、简繁映射、标点表、英文词表 |
| 卸载 | 控制面板「熊猫五笔」或安装目录下 `unins000.exe`；**用户数据不删** |

安装完成后：`Win+空格` 切到「熊猫五笔」即可。**已经在运行的程序需要重启**才会加载新输入法。

> 用户数据（配置、自定义词、自定义短语）放在 `%APPDATA%\PandaWubi`，卸载/重装都会保留。

### 方式二：源码构建 + 手动注册

```powershell
# 1. 构建（VS2022，x64 + x86）
build.bat
build-x86.bat

# 2. 一键验证（构建 + 引擎 + 数据 + TSF，共 102 项）
powershell -ExecutionPolicy Bypass -File .\tools\verify.ps1

# 3. 注册（管理员）
cd dist
regsvr32.exe /s panda_tsf.dll
"$env:WINDIR\SysWOW64\regsvr32.exe" /s panda_tsf32.dll
```

反注册用 `dist\unregister.cmd`（或 `regsvr32 /u`）。

### 打安装包

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\build-setup.ps1
# 产物：dist\PandaWubi-Setup-1.1.2.exe
```

需要 **Inno Setup 6**（`C:\Program Files (x86)\Inno Setup 6\ISCC.exe`）。

---

## 常用操作

| 按键 | 作用 |
|---|---|
| 字母 | 五笔组码（最多 4 码），候选窗实时出字 |
| 空格 / 回车 | 选第 1 个候选 |
| 数字 `1`–`9` | 选第 N 个候选 |
| `-` / `=` | 上一页 / 下一页 |
| `Backspace` | 删一个码；无码时取消 |
| `Esc` | 取消本次组码 |
| 单击**左** `Shift` | 中/英切换（英文态全键直通） |
| 单击**右** `Shift` | 简/繁切换（组字中也会同步刷新候选） |
| `Shift` + 字母 | 英文单词补全（内置 4.4 万词表） |
| 两个 `Shift` 一起按 | 中/英 与 简/繁 **同时**切换 |
| `Ctrl+Shift+T` | 简/繁切换（与单击右 Shift 等价） |
| `Ctrl+=` | 自定义词组（← 取最近上屏的字，回车保存） |
| `Ctrl+Shift+=` | 打开自定义短语窗口（编码 + 短语） |
| `Z` + 字母 | **拼音反查**：打出拼音找汉字并显示五笔全码 |

悬浮状态栏：左起图标、`中`/`英`、`简`/`繁`、⚙；右键菜单里有主题子菜单。
只在**前台是本进程且窗口有文档焦点**时显示，且同一时刻只保留一个（多进程之间会互相让位）。

---

## 候选排序规则

1. **四码才出词**：一/二/三级简码只出单字；某个不足四码的输入如果"只剩词"（没有任何单字候选），
   才会放开出词，例如 `xky` → 强调 / 强度 / 强记…
2. 四码时的顺序：**一级汉字 → 二级汉字 → 词 → 其余**（同级内按词频）。
   汉字分级用 GB2312 编码页判定（`0xB0–0xD7` 一级、`0xD8–0xF7` 二级），GBK 扩展/生僻字归"其余"。
3. 同码长同级别内按词频；用户自造词与词频自学习结果始终优先。
4. 一级简码字在 1–3 码输入时按设置「不处理 / 隐藏 / 沉底」处理（四码不受此设置影响）。

例：`trnt` → **我** / 特性 / 选择性；`lgyi` → **国** / 𡆡；`aaaa` → **工** / 恭恭敬敬 / 劳斯莱斯 …

---

## Z 键：拼音反查编码

按 `Z` 先列出"最近上屏的 4 个词 + 版本号"，继续打字母即进入拼音反查：

- 打的字母当拼音，候选每项显示 `汉字/词 + 五笔全码`，例如 `zni` → `1. 你 wqiy`、`2. 呢 knxn`；
- 支持词：`znihao` → `你好 wqvb`；
- 匹配数超过一页时用 `-` / `=` 翻页，顶部编码框显示 `ni 1/45` 这样的页码；
- 空格/回车选本页第 1 个，数字选第 N 个，`Backspace` 改拼音，`Esc` 退出。

数据表 `data\pinyin.tsv`（约 9.5 万行 / 1.9 MB，拼音 + 词 + 五笔全码）由
`tools\build-pinyin.ps1` 从 Rime 的 `luna_pinyin.dict.yaml` + `char_fullcodes.tsv` + 主码表权重生成。

---

## 主题

| 主题 | 说明 |
|---|---|
| 熊猫（默认） | 竹子 + 熊猫吃竹子 |
| 狸花猫 | 猫追蝴蝶 |
| 中华田园犬 | 叼骨头的小狗 |
| 丹顶鹤 | 晨雾荷塘（作为候选窗背景，左缘淡出到纯色） |

主题在设置窗口或悬浮栏右键菜单里切换，立即生效并同步到其它进程。

---

## 设置

`panda-settings.exe`（安装版在安装目录，源码版在 `dist\`）：

| 分组 | 选项 |
|---|---|
| 一级简码 | 不处理 / 隐藏 / 沉底（默认） |
| 排序与输入 | 码短优先（默认）/ 纯词频优先；输出繁体 |
| 主题与外观 | 候选个数、字号、字体、主题、配色、内框凹陷、候选竖排 |
| 词库 | 选择词库文件 |
| 数据目录 | 配置/自定义词/短语的位置（可指向网盘同步目录） |

命令行也支持：`panda-settings.exe /dump`、`/set TradChinese=1`、`/set Theme=cat`。

---

## 数据

| 资产 | 来源 | 数量 |
|---|---|---|
| 主码表 | Rime wubi86 + QQ五笔 词库合并 | 147,697 条（1 码 25 · 2 码 616 · 3 码 7,014 · 4 码 140,042） |
| 单字全码 | `char_fullcodes.tsv` | 27,515 字 |
| 拼音反查 | luna_pinyin + 码表权重 | 95,563 行（单字全读音 + 72,140 个词） |
| 简繁映射 | OpenCC `STCharacters` → `simp2trad.dat` | 65,536 项 |
| 英文词表 | QQ五笔 `engcore.dat` → `eng_words.txt` | 44,016 词 |

**注意**：仓库不包含第三方词库（体积与授权原因）。首次从源码使用需要自行生成，
步骤见 [`data/README.md`](data/README.md)；安装包已经内置这些文件。

正确性：用五笔86 标准规则重算 QQ 用户词库中 18,681 条 `码↔词`，
完全一致 16,519 + 简码前缀一致 2,158 = **99.98%**。

---

## 性能与稳定性（实测）

| 指标 | 数值 |
|---|---|
| 词库载入（mmap） | 0.1 ms（TSV 74.7 ms） |
| 前缀查询 | ~40 µs/次 |
| 内存（私有提交） | 0.5 MB（TSV 28.8 MB），工作集 3.6 MB |
| 模糊测试 | 5 万次随机按键序列零不变量违规 |
| 验证套件 | `tools\verify.ps1` → **102 项 PASS / 0 FAIL** |

---

## 为什么需要管理员

TSF 的语言配置在 **`HKLM\SOFTWARE\Microsoft\CTF\TIP`**（机器级）。实测非管理员三条路径全部失败：

| API | 结果 |
|---|---|
| `ITfInputProcessorProfiles::AddLanguageProfile` | `E_FAIL` |
| `ITfInputProcessorProfiles::EnableLanguageProfile` | `E_FAIL` |
| `ITfInputProcessorProfileMgr::RegisterProfile` | `E_FAIL` |

只写 `HKCU\Software\Microsoft\CTF\TIP` 不会被 `EnumLanguageProfiles` 采纳（实测枚举不到）。
DLL 在权限不足时**明确返回 `E_ACCESSDENIED`**，不会静默假成功。

> 反过来，`HKCU\Software\Classes\CLSID\{…}\InprocServer32` 是**优先于 HKLM** 的：
> 如果曾经手工 `regsvr32` 过别的路径，装完安装包也会继续加载旧 DLL。安装包会自动清掉这条按用户注册。

---

## 目录结构

```
src/engine/     词库（TSV / 二进制 mmap）、组码状态机、标点矩阵、简繁、用户词
src/tsf/        TSF 文本服务、候选窗、悬浮状态栏、资源（主题图/图标）
src/ui/         设置窗口、自定义短语窗口
src/cli/        panda-cli（验证 harness）、tsf-selftest、tsf-host（按键注入测试）
tools/          数据生成、验证、打包脚本
data/           词库与数据文件（不进仓库）
dist/           交付物（双架构 DLL + 数据 + 打包产物）
docs/           design.md（设计与进度）、test-guide.md（测试指南）
evidence/       REA 原始证据（QQ五笔 逆向分析产物）
```

## 排障

日志：`%LOCALAPPDATA%\PandaWubi\panda_tsf_<pid>.log`（每个宿主进程一个）

```
DllMain: attach pid=...
dict load(bin): path=... entries=147697 ok=1
ActivateEx: tid=... trad=0 candCount=9
cands: code=wqvb n=3 comp=...
```

按这四行就能区分"没激活 / 词库没载入 / 组码没起来 / 候选没刷新"。
注册失败会出现 `DllRegisterServer: HKLM denied (need admin)`。

常见问题：

| 现象 | 处理 |
|---|---|
| 装完还是旧版行为 | 有程序还在用旧 DLL：重启该程序；或检查 `HKCU\Software\Classes\CLSID\{7F3B0C21-…}` 是否指向别处 |
| 语言栏图标显示「简体」 | 指示器图标路径被删/移动，重装或把 4 个 `.ico` 放回 `%LOCALAPPDATA%\PandaWubi` |
| 打不出字 | 看日志里 `dict load` 是否失败；源码版需要先生成 `data\qqwubi86.dict.tsv` |
| 微信里单击 `Shift` 不切换 | 微信只转发字母/标点、连 `KeyUp` 都不给（日志里没有任何 `vk=0x10` 的 `OnKeyUp`），单击判定靠 20ms 轮询兜底。仍无效时先确认微信进程是在本次构建之后启动的（看日志 `build:` 时间） |
| 鼠标点一下，悬浮窗没跟过去 | 状态窗每 250ms 用 TSF 的光标位置（`GetSelection` + `GetTextExt`）重新贴合，Qt/Chromium 宿主没有 Win32 插入符也能跟。看日志 `idle status: … tsf=1`；`tsf=0` 表示这一拍没取到光标（用旧位置兜底） |
| 上屏后第一次退格没反应 | 退格/删除**不声索**，删字交给应用；日志里 `backspace: undo learning (pass-through)` 说明只是记了账。组码中的退格仍由输入法删编码 |

## 数据位置

| 内容 | 路径 |
|---|---|
| 配置 | `%APPDATA%\PandaWubi\config.ini` |
| 用户词 / 自定义短语 | `%APPDATA%\PandaWubi\user.tsv` |
| 图标（语言指示器） | `%LOCALAPPDATA%\PandaWubi\*.ico` |
| 日志 | `%LOCALAPPDATA%\PandaWubi\panda_tsf_<pid>.log` |

## 已知限制

- 仅 `zh-CN` 语言配置
- 五笔/拼音**混输**未实现（拼音只用于 `Z` 反查）
- 皮肤只做到"主题图 + 配色"，没有独立的皮肤文件格式
- 词库源自腾讯 QQ五笔 与 Rime 词库，**仅供本地个人使用**；公开分发请替换为可再分发词库
  （Rime wubi86 为 Apache-2.0）

## 第三方素材

| 素材 | 来源 | 许可 |
|---|---|---|
| 候选窗主题图（熊猫/猫/狗/鹤） | 项目作者提供（AI 生成后裁切） | 由作者确认可用 |
| 五笔词库 | Rime wubi86（自备） | 遵循上游授权 |
| 拼音词表 | Rime `luna_pinyin`（自备） | 遵循上游授权 |
| 简繁映射 / 英文词表 | OpenCC / QQ五笔（自备） | 遵循上游授权 |
