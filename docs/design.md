# QQ五笔替代输入法 —— 设计与进度

目标：在 Windows 11 上提供一个稳定、行为接近 QQ五笔 2.4 的五笔输入法，且不复制它的架构缺陷
（进程内巨型引擎 + IMM32 + 全局钩子跟踪光标）。

## 一、数据层（已完成并验证）

### 1. 词库来源

| 资产 | 来源 | 条目 |
|---|---|---|
| 系统词表（词 + 3 个未完全定性的字段） | **REA 提取 QQ五笔 `wubi.dict`（SBWQ）** | 94,938 |
| 用户自造词（词 + 五笔码） | **REA 提取 QQ五笔 `WBUser3.dict`（2UWQ）** | 19,080 |
| 字→全码 | 本机 Rime 小狼毫 `wubi86.dict.yaml` | 27,515 字 |
| 词频权重 | 同上（Google 词频 / 极爽6 表） | 136,235 条 |

### 2. 正确性闭环（关键验证）

用五笔86标准规则重算 QQ 用户词库里的 18,681 条 `码↔词`：

```
完全一致      16,519 (88.4%)
简码前缀一致   2,158 (11.6%)
不一致             2 (0.0%)
缺字               2 (0.0%)
--------------------------------
合计正确率            99.98%
```

两条例外（已确认合理，非实现错误）：
- `跟踪` QQ 存 `kvkp`，标准规则为 `khkh`（QQ 特有变体）
- `齡` QQ 存 `hwwc`，标准为 `hbwc`（繁体字形差异）

**结论**：码生成规则与 QQ五笔 一致到 99.98%，可以放心用"公开字表 + 规则"为 QQ 的 9.4 万词补全码。

### 3. 已产出文件

| 文件 | 内容 |
|---|---|
| `data/wubi_words.tsv` | QQ 系统词 94,938 条（offset/word/A/B/C） |
| `data/qqwubi_user_words.tsv` | QQ 用户词 19,080 条（code/word/...） |
| `data/char_fullcodes.tsv` | 27,515 字 → 全码 |
| `data/qqwubi86.dict.tsv` | **最终词库 147,697 条**（code/word/weight/source） |

词库码长分布：1 码 25（一级简码 a–y）· 2 码 616 · 3 码 7,014 · 4 码 140,042。
其中 Rime 136,235 条 + QQ 独有词补全码 11,462 条。

### 4. QQ 二进制格式（REA 结论摘要）

`wubi.dict`（SBWQ）、`code.dict`（CBWQ）、`cache.dict`（ACBW）、`engcore.dat`（NEPQ）、
用户库（2UWQ）、单元格库（CYPQ/E2CQ）、`.qpyd`（魔数 `0x7D1EA609`）——详见分析报告。

`wubi.dict`：
```
u32 magic 'SBWQ' | u32 version | u32 totalSize(须等于文件大小) | u32 count(94939)
+0x10 : 索引区
0xFE68..EOF : 记录区, 记录 = wchar word[] + L'\0' + u32 A + u32 B + u32 C
```
`WBUser3.dict`：
```
u16 '2U' | u16 recLen | u8 flags | 15 字节固定区 | char code[4] | u8 wordLen | wchar word[wordLen]
```
未定性字段：`A`（1..406900，疑似码 id/桶 id）、`B`（疑似哈希或打包值）、
`C`（仅 14 种取值，疑似 类型<<8|属性）。**这些只影响 QQ 原始排序，不影响能否输入。**

## 二、引擎层（已完成，可编译运行）

```
src/engine/wubi_engine.h/.cpp   Dictionary: TSV 载入 / code->entries 桶 / 前缀查询 / 排序
src/cli/wubi-cli.cpp            验证用 harness
build.bat                       vcvars64 + cl 构建
```

排序策略：完全匹配 > 用户词 > 码短 > 权重降序。

已知待优化：`Lookup` 的前缀匹配目前遍历全部桶（~105k），单次约数毫秒；上生产前改为 **trie**
（按码逐字下降，候选直接落在子树上）。

## 二·五、引擎层第二轮（已验证）

| 能力 | 实现 | 验证 |
|---|---|---|
| 前缀查询性能 | 按码字典序排序索引 + `lower_bound` 定位前缀区间 + 堆选 Top-K（不再收集上万候选再全排序） | `bench` 20000 次 **59 µs/次**（原 589 µs，提速 10×） |
| 简繁转换 | 读 `simp2trad.dat`（65,536 项 uint16 映射），逐码元转换 | `汉语汉字简化龙` → `漢語漢字簡化龍` |
| 标点矩阵 | 解析 `punc.ini`（**UTF-16LE+BOM**，4 列 = 英半/中半/英全/中全，含引号“第一次/第二次”配对） | `,`→`，` `^`→`……` `"`→`“ ” “` |
| 用户词自学习 | `BumpUserWord` 提权 + `SaveUserWords`/`LoadUserWords` 持久化 TSV | 提频 `您好` 后置顶且重载保持，`userdemo` PASS |

编码坑（实测）：
- `punc.ini` 是 **UTF-16LE 带 BOM**，不是 UTF-8 也不是 GBK
- `.ps1` 必须带 BOM，否则 PowerShell 5.1 按 ANSI 解析中文注释直接语法错
- `cl` 需先过 `vcvars64.bat`（PATH 里有 cl 但 INCLUDE/LIB 未设置）

## 二·六、第三轮（双架构 / 配置迁移 / 状态机抽离）

| 能力 | 说明 | 验证 |
|---|---|---|
| 32 位构建 | `build-x86.bat`（vcvars32），产出 `dist\panda_tsf32.dll` | PE machine=0x014C，4 个 COM 导出齐全 |
| 双架构注册 | `register.cmd` 同时 `regsvr32` 与 `SysWOW64\regsvr32` | 待提权后验证 |
| 配置迁移 | 读取 QQ五笔 `Config.ini`（键名完全一致）→ `%APPDATA%\PandaWubi\config.ini` | `CandVertical=1 EnablePunction=0 UseGBK=1` 正确搬运，回读 PASS |
| 组码状态机 | `Composer` 抽成纯逻辑（不依赖 TSF/窗口） | `sim 'wqvb_'`→你好、`trnt1`→我、退格/Esc 正确 |
| 配置生效 | TSF 激活时读配置，作用于候选数量/标点/简繁 | 代码已接；待激活实测 |

**第四轮已完成重构**：TSF 的 `OnKeyDown` 现在只做三件事——把键交给 `Composer`、
按返回的 `Act` 决定刷新/上屏/取消、以及处理简繁/标点/热键。按键判定逻辑只存在于
`Composer` 一处，`wubi-cli sim` 测的就是线上逻辑。

同时修掉一个真实 bug：`EnsureComposer()` 原用函数内 `static bool` 做初始化，
多个线程各有一个 `CTextService` 实例时会只有第一个拿到词典；已改为实例成员
`m_composerReady`。

**诊断日志**：`LogLine()` 写 `%LOCALAPPDATA%\PandaWubi\wubi_tsf_<pid>.log`
（可用 `WUBI_LOG` 覆盖）。记录 DllMain 装载、词典载入条目数、Activate/Deactivate、
`StartComposition` 的 HRESULT、每次按键的组码，以及注册被拒的原因。
这是没有激活环境时唯一的现场取证通道。

## 二·七、第五轮（词库二进制化 + 候选去耦合）

**问题**：TSV 词库在每个宿主进程首次按键时解析，实测 74.7 ms；16 个宿主进程各占一份内存。
第一次按键卡顿是真实手感问题。

**方案**：`WBD2` 二进制格式 + **只读内存映射**（多进程共享物理页）。

```
Header  { 'WBD2', version, recCount, blobSize }
Rec[]   { code[4], codeLen, flags, wordLen, weight(int64), wordOff, rsvd }   // 24B，按 code 字典序 + 权重降序
Blob    { 所有词条 UTF-8 连续存放 }
```

| | TSV | 二进制 mmap |
|---|---|---|
| 载入耗时 | 74.7 ms | **0.1 ms** |
| 端到端（含进程启动） | 135–151 ms | **6–8 ms** |
| 多进程内存 | 每进程各一份 | 共享同一份物理页 |
| 文件大小 | 3.16 MB | 4.13 MB |

查询结果逐项比对一致（`wqvb` → 你好/您好/𠛈）。TSF 优先载入 `.bin`，缺失时回退 TSV。

**顺带修掉的隐患**：`Lookup` 原先返回 `const Entry*`，指向词库内部数组；
`AddUserWord`/`BumpUserWord` 一旦扩容就会让候选指针悬垂，而 `Composer` 会跨调用持有它们。
现在改为返回值对象 `Candidate`，候选与词库生命周期彻底解耦。

**REA 侧结论（本轮新增）**：`wubi.dict` 记录里的 `A`/`B` 并非候选排序。
用五笔86规则为 94,858 条词算出码并按码分组后统计序关系：
`A` 与 Rime 词频的一致率 59.3%、`B` 仅 27.9%（随机基线 50%）。
=> **QQ五笔 的候选顺序存在索引区的桶顺序里，不在记录字段中**，静态不可恢复。
替代方案：用 Rime 的 Google 词频排序，并在 QQ五笔 仍可运行时做双端对比。

## 二·八、第六轮（候选窗几何 / 竖排 / 鼠标点选）

候选窗几何抽成纯函数 `src/ui/cand_layout.h`，因此**不需要激活环境就能验证**：

```cpp
struct ItemBox { int x,y,w,h;        // 文字矩形（绘制）
                 int cx,cy,cw,ch; }; // 整格矩形（点击命中，热区更大）
CandLayout Compute(sizes, vertical, padX, padY);
int HitTest(px, py);                 // 未命中返回 -1
```

自检（`wubi-cli layout`）：

| 检查 | 结果 |
|---|---|
| 横排 3 项 (20×16 文字, pad 4/2) | 92×24 |
| 竖排 3 项 | 36×64 |
| 每项中心命中自身 / 越界返回 -1 | PASS |

窗口侧已接入：
- `RecalcAndResize` 用 GDI 量出每个候选宽度后交给 `Compute`
- `Paint` 按 `ItemBox` 逐个绘制（序号灰、词条黑）
- 竖排开关取自迁移来的 `CandVertical`（当前用户 QQ五笔 配置为 `1`，即竖排）
- `WM_LBUTTONDOWN` 抓取、`WM_LBUTTONUP` 命中测试 → 回调 `CTextService`
  → `Composer.SelectDigit(idx+1)` → `CommitText`，与数字键选词走同一条路径

## 二·九、第七轮（稳定性验证）

| 项 | 方法 | 结果 |
|---|---|---|
| 状态机健壮性 | `wubi-cli fuzz 50000`：5 万次随机按键（输入/选词/空格/退格/取消） | PASS，无任何不变量违规（码长 ≤4、候选 ≤9、无非法码元、commit 非空） |
| 码表全覆盖 | `wubi-cli stress`：全部 2 码 + 3 码共 16,250 次查询 | PASS，1.8 µs/次，最大 9 候选，820 个空码 |
| 内存占用 | `wubi-cli memory` | **mmap：工作集 3.6 MB / 私有提交 0.5 MB**；TSV：26.9 / 28.8 MB |

私有提交从 28.8 MB 降到 **0.5 MB（57×）**——这一项在 16 个宿主进程里是线性叠加的，
所以对 Win11 上的实际体验影响最大。工作集 3.6 MB 主要是映射的 4.13 MB 文件页（多进程共享）。

## 二·十、第八轮（注册自检 / 显示属性 / 定位兜底）

**`register.cmd` 改为自检式**：注册后自己 `reg query` 校验 HKLM 的 TIP 与 CLSID 键，
成功打印 `[OK]`，失败打印 `[FAIL]` 并自动把 `%LOCALAPPDATA%\PandaWubi\*.log` 全部吐出来，
再列出三种常见原因（UAC 被取消 / 杀软拦截 / DLL 缺失）。
这样"注册失败"不再是一句无从下手的话。

**组码下划线**：`GUID_ATTR_INPUT` 在 Windows SDK 里**没有 C 符号**（只以 idl 常量存在），
我不去硬编码 GUID（写错会静默渲染成错误样式）。改为运行时
`ITfDisplayAttributeMgr::EnumDisplayAttributeInfo` 取第一个已注册属性，记日志后复用其 atom；
枚举失败则跳过——纯外观，不影响输入。

**候选窗定位兜底**：`ITfContextView::GetTextExt` 失败时，退回到
`GetGUIThreadInfo` 取本线程焦点窗口的插入符矩形（`ClientToScreen`），
仍然不使用全局钩子或鼠标坐标。

## 二·十一、真机激活后的首批实测（用户完成提权注册）

注册后 4 个进程加载了 DLL，日志给出了真实运行证据，也暴露了两个真 bug：

### 观察到的现象

```
[18:47:51] ActivateEx: tid=49 punc=0 trad=0 candCount=9
[18:47:51] StartComposition: hr=0x00000000 comp=000002CE047CF330
[18:47:51] display attribute: { DB1F16A6-... } atom=67
```

`StartComposition` 返回 S_OK、显示属性也拿到了——TSF 通路本来就是通的。
但**每个键都重新开始组码**：`key p -> code=p` 连着出现两次，第二次本该是 `pp`。
关键线索是这条日志只出现在"无组码"分支，且 `OnCompositionTerminated` 一次都没触发
——说明组合串不是被系统终止的，是我们自己清掉的。

### 两个根因与修法

| 根因 | 修法 |
|---|---|
| `StartComposition` 与 `SetText` 分在两个编辑会话，中间系统会终止这个**空组合串**（部分应用如此） | 合并到同一会话：StartComposition → SetText → 设属性 → 收光标 |
| `OnSetFocus` 无条件 `Reset()`，候选窗/无关焦点事件都会打断组码 | 只在**焦点离开所有文档**或**换文档**时重置 |

修后同一记事本进程内的日志（已验证）：

```
[18:54:25.896] key a -> code=a act=1
[18:54:25.898] StartComposition: ... comp=FA60
[18:54:26.536] key(composing) a      ← 以前这里就断了
[18:54:27.140] key(composing) t
[18:54:27.361] key(composing) r
[18:54:27.692] key(composing) n
```

### 顺带修掉

- `.bin` 路径构造错误：`qqwubi86.dict.tsv` 去掉最后一个 `.` 之后得到 `qqwubi86.dict.bin`（不存在），
  导致一直回退到 TSV。修后日志确认 `dict load(bin): entries=147697`，`probe_wqvb=你好`
- 二进制模式下 `EntryCount()/CodeCount()` 取错成员，日志显示 `entries=0`
- 日志里 `%hs` 的 UTF-8 词条按 C locale 转宽字符变成乱码
- 词库改为**激活时预载**（mmap 下 0.1ms），不再等第一次按键

### 端到端自测为什么没能跑起来（结论）

尝试了三代方案，都受同一类限制：

| 方案 | 结果 |
|---|---|
| `SendInput` + 抢占前台 | 后台进程拿不到前台（`SetForegroundWindow` 被前台锁拒绝，ALT 破解也无效），且不能干扰用户正在用的窗口 |
| `PostMessage(WM_KEYDOWN)` 到自己窗口 | 不触发 TSF 按键路由（实测 `TestKeyDown` 全为 0） |
| `ITfKeystrokeMgr::TestKeyDown/KeyDown`（正规接口） | 接口本身可用（`hr=0`），但 `ITfThreadMgr::GetFocus` 返回 `dm=NULL` |

根因是最后一条：**宿主窗口没拿到真实前台/焦点，RichEdit 与普通 EDIT 都不会为该线程建立 TSF 文档**
（普通 `EDIT` 本来就不是 TSF 感知控件；换成 RichEdit 也一样，因为窗口是后台的）。
没有 document manager，`pfEaten` 恒为 0，按键不可能进入输入法。

因此"按键是否经过我们"只能由真实的前台应用验证——用日志判定：
日志里出现 `OnKeyDown vk=0x41 comp=...` 才算真的走到我们这里。
### 新增自测能力

- `tsf-selftest` 增加 `[7]`：程序化创建 ThreadMgr 并 `Activate()` 文本服务，
  不经 UI 即可验证 ActivateEx 与词库载入
- `tsf-host.exe`：建带 EDIT 的窗口、激活输入法、合成按键、回读文本。
  前台被占用时会**主动放弃**（不干扰用户正在用的窗口）；
  `PostMessage` 方式则无法触发 TSF 的按键路由（已实测），因此这个 harness 只能在无人使用机器时跑

## 二·十二、真机端到端验证通过（第 11 轮）

用户实测日志（`wubi_tsf_37256.log`）证明整条链路工作正常：

```
[19:03:30.275] OnKeyDown vk=0x54 comp=0        code=            ← 按 t
[19:03:30.279] StartComposition: start=0x00000000 comp=...EE20
[19:03:30.812] OnKeyDown vk=0x52 comp=...EE20  code=t           ← 按 r（组码存活）
[19:03:31.579] OnKeyDown vk=0x4E comp=...EE20  code=tr
[19:03:32.024] OnKeyDown vk=0x54 comp=...EE20  code=trn
[19:03:32.612] OnKeyDown vk=0x20 comp=...EE20  code=trnt         ← 空格
[19:03:32.613] CommitText: comp=...EE20 textlen=1               ← 上屏「我」（1 字）
[19:03:39.611] CommitText: comp=...EF60 textlen=2               ← wqwu 上屏「你们」（2 字）
```

`textlen` 与词长严格对应，说明组合串、候选选择、上屏都正确。

### 本轮同时改进的两处可用性

| 问题 | 改法 |
|---|---|
| `a` 有 20 个候选，只能取前 9 | **候选翻页**：`Composer` 支持分页（页大小 = `CandCount`），组码时按 `,` / `.` 翻页；`a` 有 4 页 |
| 同一个词因多码重复出现（`工` 出现在 `a`/`aaa`/`aaaa`） | **按词去重**：`Lookup` 合并完全匹配与前缀匹配后去重，保留排名最高者。`a` 的候选从 20 条降到 19 条唯一词 |

## 二·十三、让用户选过的功能真正生效（第 12 轮）

### 发现一个"功能存在但静默失效"的 bug

`用户自造词 + 词频自学习` 是用户明确勾选的功能，引擎侧也早就实现了（`BumpUserWord`/`SaveUserWords`），
但**在 mmap 模式下实际无效**：

- `Lookup` 的二进制分支只扫映射区，从不去看 `userIndex_` → 学到的词永远不出现在候选里
- `AddUserWord`（`LoadUserWords` 用它）没有登记 `userIndex_` → 从磁盘载入的用户词同样不可见

表现为 `wubi-cli qqwubi86.bin userdemo`：
```
[before]            你好(70500000) 您好(34700000) 𠛈(0)
[after bump 您好 +80M]  你好(70500000) 您好(34700000) 𠛈(0)   ← 完全没变
```

修法：二进制分支在合并完全匹配与前缀匹配之后，再用 `CollectUserCandidates` 并入用户词，
统一按 (用户词 > 码短 > 权重) 排序并去重；`AddUserWord` 补上 `userIndex_` 登记。

修后：
```
[before]            你好(70500000) 您好(34700000) 𠛈(0)
[after bump 您好 +80M]  您好(80000000,u) 你好(70500000) 𠛈(0)
[reloaded 1 user words] 您好(80000000,u) 你好(70500000) 𠛈(0)   ← 重启后仍在
```

### 接进输入法本体

- 每次上屏记下「本次码 → 词」，调用 `BumpUserWord`
- 每 8 次上屏落盘一次到 `%APPDATA%\PandaWubi\user.tsv`（减少多进程写同一文件的冲突）
- `Deactivate` 时再落盘一次
- 激活时载入用户词，日志里带 `userWords=N`（首次运行为 -1，表示文件不存在）

> 已知妥协：多个宿主进程各自持有一份内存用户词，落盘是"最后写入者胜"。
> 对单用户本机足够；要更严谨需要共享内存 + 单一写者。

### 开关持久化

`Ctrl+Shift+T`（简繁）/ `Ctrl+Shift+P`（中文标点）以前只改内存，重启即失效。
现在会写回 `%APPDATA%\PandaWubi\config.ini`，日志记录 `toggle T -> trad=... saved=1`。

## 二·十四、候选排序可调 + 双端对比工具（第 13 轮）

### 又抓到一个排序 bug（影响很大）

`a` 的候选长期全是 `aa*` 开头的词（工艺/葡萄牙/工期…），而权重更高的二级简码
（东 `ai` 111M、世 `an` 101M）根本不出现。

根因是**优先队列方向写反**：

```cpp
// 错：comp(a,b) = "b 比 a 好"  =>  top 是"最好"的候选
auto worseFirst = [](const Candidate& a, const Candidate& b) { return Better(b, a); };
...
else if (Better(c, heap.top())) { /* 只有当新候选优于"最好的"才替换 */ }
```

于是堆只留住了**最先扫到的** 9/20/36 个候选（磁盘顺序是码字典序，`aa*` 最靠前），
后缀码（`ai`/`an`/`aw`…）永远进不来。tsv 与 mmap 两条路径共用这段逻辑，
所以之前 bin↔tsv 交叉比对也发现不了。

修正为 `BetterCand(a, b)`（`top` = 最差，便于替换）。修正后：

```
a → 工 区 东 世 或 式 切 基 共
```

这才是五笔应有的样子（一级简码在前，其后是高频二级简码）。

### 排序策略可配置

`config.ini` 新增 `SortMode`：

| 值 | 行为 | `a` 的首位 |
|---|---|---|
| 0（默认） | 码短优先 → 权重 | 工（一级简码） |
| 1 | 纯词频优先 → 码短 | 区（237M） |

用户词在任何模式下都最优先。`wubi-cli order <mode>` 可离线对比两种策略。

### 双端对比工具

```
data\qq-order.tsv          模板：第一列码，第二列留给你填 QQ五笔 的顺序
tools\compare-order.ps1    读该文件，与我们逐位对比并给结论
```

输出示例（用模拟数据自测）：

```
码        我们首位      QQ首位        首位一致  前3重合
a         工            工             YES      3/3
wqvb      你好           您好            no       2/3
共对比 4 个码：首位一致 3/4 (75%)，前 3 重合 4/4
结论: 首位大体一致
```

用 `tools\fixtures\qq-order-sample.tsv` 做了回归用例，验证脚本能识别"顺序相反"的码。

## 二·十五、把"开关持久化"和"学习节流"变成可执行证据（第 14 轮）

之前这两项只有"代码存在"，缺少能跑的验证。现在补了两个 CLI 命令，直接复现 TSF 里的逻辑：

```
$ wubi-cli qqwubi86.bin toggle trad build\toggle-test.ini
toggle trad: before=0 after=1 saved=1 reload-match=PASS
$ wubi-cli qqwubi86.bin toggle punc build\toggle-test.ini
toggle punc: before=0 after=1 saved=1 reload-match=PASS

$ wubi-cli qqwubi86.bin learnsim 16
learnsim: 16 commits, 首位 你好 -> 您好, 文件=有, RESULT=PASS
```

`learnsim` 复刻了 TSF 上屏路径的节流策略（每 8 次落盘一次），验证两件事：
顺序真的翻转、用户词文件真的写出来了。三条都已并入 `verify.ps1`。

## 二·十六、直接用 QQ五笔 引擎取真值做双端对比（第 15 轮）

### 不用手工取数：驱动 IMM 引擎

QQ五笔 虽然只注册了 TSF，但它的 `QQWubi.ime` 仍导出完整的 IMM 接口。IMM 路径**不需要 TSF、
不需要前台、不需要用户操作**，因此可以当"真值探针"：

```
tools\qq-oracle.cpp
  1) LoadLibrary QQWubi.ime；取 ImeInquire/ImeSelect/ImeProcessKey/ImeToAsciiEx
  2) ImeInquire → 得到 IMEINFO 与 UI 窗口类名（QQWubiUIWnd），按该名字建隐藏窗口
  3) ImmCreateContext + ImmAssociateContext + ImeSelect(himc, TRUE)
  4) 逐键 ImeProcessKey + ImeToAsciiEx
  5) ImmGetCandidateListW 读标准候选列表
```

一个坑：QQ 会把"剩余码"附在候选串尾部（`或k`、`其dw`、`工作awt`），必须在取词后剥掉尾部 ASCII。

### 拿到真实顺序后的发现

| 码 | QQ五笔 真实顺序 | 我们（修复前） |
|---|---|---|
| `trnt` | 特性, 选择性, **我** | **我**, 特性, 选择性 |
| `aaaa` | 恭恭敬敬, 花花草草, 劳斯莱斯, **工** | **工**, 恭恭敬敬, … |
| `wqvb` | 你好, 您好 | 同 |
| `khkh` | 跟踪, 踊跃, 跳跃, 踩踩, 跑跑 | 前三个同 |
| `wqwu` | 你们, 您们, 爷们, 低位 | 你们, 您们, 低位, 爷们 |

规律很明确：**同码长时，词组排在单字前面**。这条规则我们原先没有。

### 落地规则并复测

`BetterCand` 在"码短优先"模式下增加：同码长时词组（≥2 字）优先于单字。
（纯词频模式不加这条，否则权重会被压过。）

复测结果：

```
码      我们首位    QQ首位    首位一致  前3重合
a       工          工         YES      1/3
trnt    特性        特性        YES      3/3
wqvb    你好        你好        YES      2/3
khkh    跟踪        跟踪        YES      3/3
aaaa    恭恭敬敬     恭恭敬敬      YES      3/3
wqwu    你们        你们        YES      2/3
=> 首位一致 6/6 (100%)，前 3 重合 5/6
```

唯一明显差异是 `a`：QQ 为 `工 戈 或 其 工作`，我们是 `工 区 东 世 或`。
QQ 把「戈」排第二并不符合实际用字频率，这里我们认为自己的顺序更合理，未跟随。

该对比已并入 `verify.ps1`（`data\qq-order.tsv` 由 oracle 生成，作为回归基线）。

## 二·十七、一级简码在多键输入时的处理（用户建议）

用户指出：既然 `a` 一键就能打出「工」，那它在 `aaaa` 的候选里再出现就是噪声。
按此建议实现为三档，配置项 `Level1Mode`：

| 值 | 行为 | `aaaa` | `trnt` |
|---|---|---|---|
| 0 | 不处理 | 恭恭敬敬/劳斯莱斯/花花草草/**工**/㠭 | 特性/选择性/**我** |
| 1 | 隐藏 | …/**工消失**/㠭 | 特性/选择性（**我消失**） |
| 2 | 沉底（默认） | …/㠭/**工** | 特性/选择性/**我** |

**为什么默认取 2 而不是 1**：25 个一级简码里包含「我」（`Q` 键）和「工」（`A` 键）等高频字。
若选 1，用户完整敲 `trnt` 时将**打不出「我」**——而 QQ五笔 在 `trnt` 下是把「我」放第 3 位的。
沉底既把噪声移出视野，又保留可达性。

实现上：`EnsureLevel1()` 在首次调用时扫描码长为 1 的记录建立集合（bin/tsv 两种模式各一条路径），
`IsLevel1Noise()` 判定"多键输入 + 单字 + 属于一级简码"，随后按 `Level1Mode` 跳过或沉底。

## 二·十八、四码后继续输入 = 顶字上屏（用户建议）

原来的实现：`TypeCode` 在码长已达 4 时直接 `Pass`，于是第 5 个键会被**原样透传给应用**
（等于在文档里打了个字母）——这是错的。

五笔习惯是"顶字上屏"：四码打满后继续输入，先把当前第 1 候选上屏，再用这个键开新的一轮组码。

状态机新增 `Act::CommitThenContinue`：

```
't' updated code=t
'r' updated code=tr
'n' updated code=trn
't' updated code=trnt       ← 满四码
'x' commit+cont code=x  COMMIT='特性'   ← 上屏第 1 候选，并以 x 开新码
```

顺带修了一个连带 bug：顶字上屏时 `m_composer.Code()` 已经变成**新码**，
如果还用它做词频学习，就会把旧词记到新码上。现在 `Outcome` 额外带出 `commitCode`（Reset 之前记下），
学习与上屏都用它。

TSF 侧的处理顺序也相应明确：先用 `CommitText(..., resetComposer=false)` 结束当前组合串并写入文本，
再用新码重新 `StartComposition`。

## 二·十九、设置界面（用户要求"放在设置里给用户选择"）

之前 `Level1Mode`/`SortMode` 只能手改 ini，太隐晦。现在提供独立设置工具 `panda-settings.exe`：

- 双击 `dist\settings.cmd`（或直接跑 exe）弹出设置窗口
- 选项：一级简码处理（不处理/隐藏/沉底）、候选排序（码短优先/纯词频）、
  中文标点、输出繁体、候选个数
- 保存写回 `%APPDATA%\PandaWubi\config.ini`

同时保留命令行用法，便于脚本与自检：

```
panda-settings.exe /dump                       # 打印当前设置
panda-settings.exe /set Level1Mode=1 SortMode=1  # 批量修改
```

### 两个实现细节

- 无参数启动时先 `FreeConsole()` 再建窗口，双击不会闪黑框；带参数时保留控制台输出，
  这样一个 exe 同时是 GUI 与 CLI 工具。
- 支持 `WUBI_INI` 环境变量覆盖配置文件路径——既是便携部署能力，也让自检能在工作区里验证写入
  （DSH 沙箱会拒绝其派生子进程写 `%APPDATA%`，返回 `ERROR_ACCESS_DENIED`，
  这曾让我误以为设置保存有 bug）。

## 二·二十、Ctrl 组合键被当成组码吞掉（用户报告的 bug）

现象：输入法激活时按 `Ctrl+C` 无法复制——输入法把 `C` 当成了组码。

原因：`OnKeyDown` 里判断组码时**没有检查修饰键**，字母键一律走 `IsCodeKey`。
更糟的是这个分支在"组码进行中"的路径里，所以只要有组合串，连 `Ctrl+V`、`Ctrl+Z` 也会被吃掉。

修法：把修饰键判定提到函数最前，顺序变成

1. `Ctrl+Shift+T/P`（我们的快捷键，组码中也生效）
2. **`ctrl || alt` → 原样交还应用**（`*pfEaten` 保持 FALSE）
3. 组码进行中 → 状态机
4. 标点接管 → 组码

日志新增 `pass-through: ctrl=1 alt=0 vk=0x43` 这样的行，便于确认按键确实交还给了应用。

> 附带好处：`Alt` 组合键（菜单访问键）也不会再被输入法拦截。

## 二·二十一、数字后的单次半角逗号/句号（用户需求）

需求：中文输入时符号转全角（空格除外）；但**前面刚输入数字**时，按 `,`/`.` 应输出**半角**，
且只此一次；该"待定"状态下按 `Esc` 取消它，并**拦截 Esc**（不发给应用）。

实现为一个可复用的状态机 `wubi::DigitPunctGate`（放在引擎头文件里，TSF 与 CLI 共用）：

| 事件 | 行为 |
|---|---|
| 数字键 | 进入待定 |
| 待定中 `,` / `.` | 输出半角 `,` / `.`，退出待定（单次） |
| 待定中 `Esc` | 取消待定，**吃掉 Esc** |
| 其它普通键 | 退出待定 |

TSF 侧要点：

- 待定判定放在 `OnKeyDown` 最前面（修饰键守卫之后），早于标点接管与组码
- 组码期间的数字是"选词"不是"输入数字"，所以组码路径会把待定清掉
- 开始组码也会清掉待定（上一字符不再是数字）
- 空格不参与标点映射（处理器本来就要求 `buf[0] > 0x20`），符合"除了空格"

CLI 侧有对应模拟命令，便于回归：

```
$ wubi-cli qqwubi86.bin gatesim '3,'      # 3 -> 数字（待定）；, -> 半角 ','
$ wubi-cli qqwubi86.bin gatesim '3!,'     # Esc 被拦截；, -> 全角 '，'
$ wubi-cli qqwubi86.bin gatesim '3a,'     # 夹了字母 -> 全角 '，'
$ wubi-cli qqwubi86.bin gatesim '3.14'    # 3.14 -> 半角小数点
```

## 二·二十二、半角符号上屏后光标停在符号左边（用户报告）

现象：半角逗号/句号插入成功，但插入点停在符号**左侧**。

原因：`CommitString` 用的是 `ITfInsertAtSelection::InsertTextAtSelection(ec, 0, …)`。
文档里 flags=0 的"quirk"语义是"把插入点移到插入文本之后"，但**实际很多应用不会这么做**，
于是选区仍停在插入点之前——光标看起来"退了一位"。

修法：插入成功后，克隆插入范围、`Collapse(TF_ANCHOR_END)`，再用 `ITfContext::SetSelection`
显式把选区收拢到末尾：

```cpp
ITfRange* after = nullptr;
if (SUCCEEDED(r->Clone(&after)) && after) {
    if (SUCCEEDED(after->Collapse(ec, TF_ANCHOR_END))) {
        TF_SELECTION sel = {};
        sel.range = after;
        sel.style.ase = TF_AE_NONE;
        sel.style.fInterimChar = FALSE;
        ctx->SetSelection(ec, 1, &sel);
    }
    after->Release();
}
```

这条修复同时覆盖了中文标点（全角）路径，因为它走的是同一个 `CommitString`。

### 附带：编译戳

每次激活都会记一行 `build: Nov 16 2026 20:17:24`。
这一轮排查中曾误判"功能没生效"，实际是用户测试的程序里加载着**部署前**的旧 DLL
（已加载的 DLL 不会被替换，必须重启宿主进程）。有了编译戳就能一眼确认。

## 二·二十三、一级简码被"词频自学习"顶掉（用户报告）

现象：很多单键码打出来，一级简码不在首位。

排查：字典层 25 个一级简码**全部正确**（`lookup g` → 一 第一）。
问题出在用户词表 `%APPDATA%\PandaWubi\user.tsv`：

```
g	来	3000000      <- 学习结果把「一」顶到第 2
b	子	2000000      <- 把「了」顶到第 2
```

用户词在排序里无条件最优先，所以只要在一个单码上选过别的字，一级简码立刻掉位。
更糟的是这件事**会自我强化**：掉位后更容易被再次选到，权重继续涨。

修法两条：

1. `Dictionary::EnsureLevel1First()`：单码输入时，把该码的一级简码字强制提到首位
   （仅在"码短优先"模式；纯词频模式仍然严格按权重）。
2. 上屏学习**不再记录单码**（`code.size() > 1` 才学）。一级简码是固定约定，
   不应该被使用频次改写。

验证（用真实 user.tsv，25 个一级简码逐个比对，全部命中）：

```
usertest g -> 一 来(u) 不 到 下      # 学的「来」退到第 2
usertest b -> 了 子(u) 也 出 阿
usertest a -> 工(u) 区 东 世 或
```

回归用例落在 `tools\fixtures\user-level1.tsv`，已并入 `verify.ps1`。

### 附带：build.bat 被污染

排查中发现 `build\panda-settings.exe` 消失。原因是早前一次字符串替换在 `build.bat` 里
留下了两个多余的引号（`'echo === settings ===`），cmd 报错后跳过了那一步编译。
已清理，并在文档里记一笔：改批处理脚本时要检查引号是否成对。

## 二·二十四、简码优先级规则（用户定稿）

用户明确规则：**一级/二级/三级简码不该被词频顶掉；只有四码上屏的字词才考虑词频。**

落地为三条：

1. `EnsureExactFirst()`：输入长度 1–3 时，把**系统词库里码长等于输入长度的那条**提到首位。
   注意必须是"系统条目"——用户自造词即使码长相同也不算，否则学到的「来」(g) 会继续顶掉「一」。
2. `IsLevel1Noise()` 增加**精确匹配豁免**：输入长度 ≤3 且候选码正好等于输入时，
   不参与"一级简码隐藏/沉底"。否则 `aaa` 的三级简码「工」会被当成"一级简码噪声"隐藏掉
   ——这正是本轮实测到的 `工` 消失的真正原因（`Level1Mode=1`）。
3. 上屏学习只在 `code.size() == 4` 时记录。

各长度的实际效果（带高权重干扰用户词）：

```
g    -> 一 来(u) 不 到 下            # 学到 g->来 也顶不掉 一
aa   -> 式 戒(u) 区(u) 芽 苷          # 学到 aa->戒 也顶不掉 式
aaa  -> 工 区(u) 工艺 葡萄牙 工期      # 隐藏模式下精确匹配仍豁免
aaaa -> 恭恭敬敬 劳斯莱斯 花花草草 …   # 四码按词频/用户词自由竞争
```

### 已知取舍

三码就是完整码的字（如 `试 yaa`、`真 fhw`）在新规则下**不参与学习**，
因为无法从码长区分"三级简码"与"三码完整码"。它们的顺序由字典决定，
但精确匹配保护保证它们仍在该码首位。要放开需要再加"该词是否存在四码条目"的判断。

用户词表里 1–3 码的历史学习记录已清理（备份 `user.tsv.bak`）。

## 二·二十五、单键布局：1号一级简码，2号该键四次码（QQ五笔 行为）

用户指出 QQ五笔 的单键候选布局：按 `k` 时第 1 位是「中」（一级简码），第 2 位是「口」（`kkkk`）。

实现 `Dictionary::InsertKeyRepeat()`：

- 仅对 `input.size() == 1` 生效，且只在"码短优先"模式
- 取该键重复四次形成的码（`k` → `kkkk`），在字典里找精确匹配，选出权重最高且优先非用户词的条目
- 若该字与第 1 位的一级简码相同（`a`→本字 `工`、`w`→本字 `人`），跳过，不重复
- 否则插到第 2 位（先按词去重，避免同一字出现两次），并裁剪到候选上限

实测：

```
k -> 中 口 只          kkkk = 口
g -> 一 王 不          gggg = 王
t -> 和 禾 得          tttt = 禾
j -> 是 日 时          jjjj = 日
m -> 同 山 几          mmmm = 山
q -> 我 金 然          qqqq = 金
a -> 工 区 东          aaaa = 工，与一级简码相同 -> 跳过
w -> 人 他 个          wwww = 人，同上
```

四条已并入 `verify.ps1`。

## 二·二十六、分号/单引号选词 + Shift 切中英文（用户需求）

### 分号 = 第 2 候选，单引号 = 第 3 候选

组码期间在 `OnKeyDown` 里加了两个分支（`VK_OEM_1` = `;`，`VK_OEM_7` = `'`）：

```
khkh;  -> 上屏 踊跃（第 2）
khkh'  -> 上屏 跳跃（第 3）
```

这两个键在**非组码**时不拦截，所以标点接管开启时仍会输出 `；`/`’`。

### 单击 Shift 切换中英文

实现要点：

- `OnKeyDown` 里遇到 Shift 记 `m_shiftDown = true`，并**不拦截**该键（避免影响应用）
- 按下 Shift 期间若出现了其它键，置 `m_shiftUsed = true`（这样 Shift+字母不算切换）
- `OnKeyUp` 里若 `m_shiftDown && !m_shiftUsed` 则切换 `m_english`
- 切到英文时如果正在组码，先取消组合串，避免残留
- 英文模式下 `OnKeyDown` 在快捷键判定之后直接返回（不组码、不接管标点、全键直通）
- 状态提示复用候选窗显示单字「中」/「英」，**下一次按键时自动隐藏**（不用定时器）

日志会记录 `toggle english=0/1`，便于确认。

## 二·二十七、Z 键重复上一次上屏（QQ五笔 特性）

- `CommitText` 成功后把实际写入的文本存进 `m_lastCommit`（是转换过大写/繁体之后的最终文本）
- `OnKeyDown` 非组码路径里，`VK 'Z'` 且 `m_lastCommit` 非空 → 用 `CommitString` 重放该文本并吞掉按键
- 组码进行中不触发（组码分支更早返回）；英文模式下也不触发（英文模式在更早处直通）
- `z` 本来就不是五笔码键（`IsCodeKey` 只认 A–Y），所以不与组码冲突
- 没有任何上屏记录时按 Z 仍然原样输出字母 `z`

日志会记 `repeat last commit: N chars`。

> 注：这条行为只能真机验证。重启记事本后打一个词、再按 Z，应重复出同样的词。

## 二·二十八、Z 需空格上屏 + 分号便捷输入（用户需求）

### Z 改为"待定后空格上屏"

上一版 Z 是直接上屏；本版改成：

- 按 `Z` → 开始一个组合串，内容就是上次上屏的文本（候选窗同时显示该文本作为提示）
- 按 `空格`/`回车` → 上屏
- 按 `Esc` → 取消（组合串一并结束）
- 按其它键 → 放弃这次重复，**并继续按正常流程处理该键**（不会丢键）

### 分号="便捷输入"（数字转中文）

单按 `;`（非组码、非英文模式）不再输出分号，而是进入便捷输入：

- 继续按数字键 → 组合串显示数字，候选为三种形式：
  1. 逐位直读：`123` → `一二三`
  2. 进位读法：`123` → `一百二十三`
  3. 大写财务：`123` → `壹佰贰拾叁`
- `空格` 选第 1 个，`;` 选第 2 个，`'` 选第 3 个（与组码时的选词键一致）
- `退格` 删掉一位；`Esc` 或其它键退出便捷输入
- **还没输数字时再按一次 `;`** → 用户其实就是想要分号：退出便捷输入并上屏一个**全角分号 `；`**
  （中文标点关闭时输出半角 `;`）。因为分号被便捷输入吃掉了，这里是"补发"。
  `;;;` 则是"分号 + 重新进便捷输入"。

数字转换在引擎里实现为 `wubi::NumberForms()`，纯函数、可离线验证：

```
123         -> 一二三 / 一百二十三 / 壹佰贰拾叁
10          -> 一〇 / 十 / 壹拾
101         -> 一〇一 / 一百零一 / 壹佰零壹
100200      -> 一〇〇二〇〇 / 十万零二百 / 壹拾万零贰佰
100010001   -> 一〇〇〇一〇〇〇一 / 一亿零一万零一 / 壹亿零壹万零壹
```

实现规则要点：
- 4 位一组，组间按 万/亿 进位
- 组内缺位补「零」；低一组不足千位时也要补「零」
- 最高一组为 10–19 时，小写读作「十X」（十万、十二万），大写形式不省「壹」

## 二·二十九、便捷输入的选词改为字母（用户反馈）

问题：便捷输入里数字是"输入内容"，不能用数字选候选。

改动：

- 候选序号在便捷输入下改成 `a. b. c.`（`CCandidateWindow::SetLetterLabels`）
- `a` / 空格 → 第 1 个（逐位读法）
- `b` / `;` → 第 2 个（进位读法）
- `c` / `'` → 第 3 个（大写财务）
- 退出便捷输入或回到组码时，序号自动恢复成数字
- 单按 `;` 进入时立刻弹出提示框「便捷输入：请输入数字」，不再毫无反馈

### 这一轮暴露的流程问题

用户在 **DSH Desktop（pid 34272，12:33 启动）** 里测试，那个进程锁着几小时前的 DLL，
所以"分号没反应 / 四码自动上屏不对"都不是新代码的问题。已加载的 DLL 不会被替换，
必须重启宿主进程才能换新版；`explorer` 这类系统进程不能随便重启。

结论：**新功能一律在新建的记事本里验证**，并在日志里核对 `build:` 编译戳。

## 二·三十、Z 待定时不要往文档里写字（用户反馈）

第一版实现是"按 Z 就创建组合串并把上次的文本写进去"，逻辑上正确（日志证明空格才 CommitText），
但**文字已经出现**在文档里（组合串没有可见下划线时和已上屏没有区别），用户看到的就是"没按空格就上屏了"。

改为：

- 按 `Z`：**完全不碰文档**，只在候选窗显示提示「空格重复：<上次内容>」
- 按 `空格`/回车：这时才用 `CommitString` 写入文档
- 按 `Esc`：仅清状态、隐藏提示
- 按其它键：清状态后继续正常处理该键

日志相应改为 `repeat pending: N chars (waiting for space)`，可与 `CommitText` 时间戳对照。

## 二·三十一、改名 Panda五笔 + 图标 + 托盘 + 候选框美化

### 改名

源码里 5 处 `QQ五笔替代输入法` 全部改为 `Panda五笔`（TSF 描述、自检输出、设置窗口标题）。
注意：**语言栏里显示的名字存在 HKLM**，要在名称上生效需要以管理员重跑一次 `dist\register.cmd`。

### 图标

`tools\make-icon.ps1` 用 GDI+ 现画再手工封装 ICO（不依赖外部库）：

- 设计：深蓝→青绿渐变圆角方块 + 白色「五」+ 右下角金色「PP」角标
- 尺寸：16/24/32/48/64/128/256（≤128 用 BMP 载荷，256 用 PNG 载荷）
- 产物：`dist\panda.ico`（约 110KB）

### 托盘程序 `ppwubi-tray.exe`

原生 Win32，常驻托盘：

- 左键单击/双击 → 打开设置
- 右键菜单：设置 / 关于 / 退出
- 监听 `TaskbarCreated` 消息，资源管理器重启后自动重新挂载图标
- 互斥体保证单实例
- 图标优���从同目录 `panda.ico` 加载（多尺寸），失败回退系统默认图标

启动方式：双击 `dist\tray.cmd`（可放进启动目录）。

### 候选框美化

| 项 | 改动 |
|---|---|
| 阴影 | 窗口类加 `CS_DROPSHADOW` |
| 背景 | 纯白 → `#FCFCFD` |
| 边框 | `GRAY_BRUSH` → 1px `#D6D8DC` 细边 |
| 序号色 | `#808080` → `#96989E`（更浅） |
| 词条色 | 纯黑 → 首选 `#143C78` 深蓝，其余 `#1C1C20` |
| 字体 | Microsoft YaHei → Microsoft YaHei UI（ClearType） |

## 二·三十二、组码中按符号：先上屏 1 号候选（用户需求）

需求：组码时输入符号，应先把当前 1 号候选上屏，再输出符号；`;` 与 `'` 例外（它们用于选第 2/3 候选）。

实现（`OnKeyDown` 组码分支）：

- `;` → 选第 2；`'` → 选第 3（保留）
- `-` / `=` → 上下翻页（**从 `,` / `.` 移过来**，因为用户要求 `,` `.` 也按符号处理）
- **其它可打印符号** → `SelectBest()` 提交 1 号候选，随后按标点表输出该符号（标点接管关闭时输出原字符）
- `Esc` 取消、`退格` 删码不变

### 顺带修掉一个键泄漏

原来翻页键在"没有下一页"时状态机返回 `Pass`，而 `handle()` 的 `default` 分支不设置 `*pfEaten`，
于是 `,`/`.` 会漏给应用 → 应用插入字符 → TSF 终止组合串（日志里表现为
`OnKeyDown vk=0xBE ... code=pfwg` 紧跟 `OnCompositionTerminated: code=pfwg`）。

修法：`default` 分支也置 `*pfEaten = TRUE`——进入组码后，这些键都属于输入法，不该漏出去。

## 二·三十三、特殊码：date / time / week（用户需求）

打 `date` / `time` / `week` 直出日期时间，格式与 QQ五笔 对齐：

```
date:  2026.11.17
       2026年11月17日
       2026-11-17
       二〇二六年十一月十七日

time:  10:51:35
       2026年11月17日 10:51:35
       2026年11月17日, AM 10:51:35
       二〇二六年十一月十七日 10:51:35
       2026-11-17 10:51:35

week:  星期二
       二〇二六年十一月十七日 星期二
       2026年11月17日 星期二
```

实现分三层：

1. 引擎 `DateTimeCandidates(kind, y,m,d,h,mi,s,weekday, out)` —— 纯函数，时间由调用方传入，
   因此可以离线验证（`wubi-cli special date|time|week`，可带固定时间戳）。
2. `Composer::SetSpecialProvider()` —— 组码到 `date/time/week` 时优先用这个回调的候选，
   否则再查字典。上屏/选词/翻页逻辑完全复用。
3. TSF 在 `EnsureComposer()` 里安装 provider，用 `GetLocalTime()` 取当前时间。

细节：
- 12 小时制的 `AM/PM` 与 `12:05` 这种边界已处理（0 点显示 AM 12:05）
- 月/日的中文读法用「十一、二十、二十三」，年份逐位「二〇二六」
- 特殊码优先于字典和用户词——即使用户词表里误学了 `date -> 春花秋月` 也不会干扰

## 二·三十四、锁定标记 + 托盘状态（用户反馈）

### Ctrl+数字 提频（已生效）

组码满四码时 `Ctrl+数字` 把第 N 个候选提到首位，并把权重设为「当前最高 + 1」写回用户词固化。

### 候选框标记

只有**人工强制指定**的候选（自造词 `Ctrl+=`、自定义短语）才加记号：**词尾一个小淡蓝色 `*`**
（约为正文 60% 的字号，`RGB(0x6F,0xA8,0xD8)`；选中项用更亮的天蓝 `RGB(0xD8,0xEA,0xFA)`）。
词频自学习把某个词顶到前面时**不加任何记号**——那是输入法自己学的，不该打扰用户。

```
1.踊跃*   2.跟踪   3.跳跃 …
```

判定：`Candidate::IsPinned()` = `user && weight >= kPinnedWeight`（`0x7FFFFFFF`）。
自造词与自定义短语写入 user.tsv 时都用这个权重（`2147483647`），自学习只在普通权重上做加法。

### 托盘图标（先前失败的真正原因）

排查结论：`Shell_NotifyIcon` 返回 `ACCESS_DENIED(5)`，但**不是权限策略**——是 `dist\` 目录及其中所有文件
被我（沙箱进程）创建时继承了 **Low 完整性标签**，导致托盘进程以 low integrity 运行，
既无法给 medium 的 Explorer 发消息，也写不了用户目录。

修复：`icacls "dist" /setintegritylevel (OI)(CI)Medium /T` 递归改为 Medium。

修复后的真实日志：

```
env session=1 integrity=0x2000     ← 不再是 0x1000(Low)
NIM_ADD attempt=1 ok=1 err=0       ← 成功
NIM_SETVERSION ok=1 err=0
```

> 教训：在沙箱/低权限环境里产出并分发 Windows 可执行文件时，必须检查并修正文件完整性标签，
> 否则用户侧会出现"莫名权限失败"。

## 二·三十五、语言指示器显示自定义图标（成功方案，查文档后定案）

目标：`Win+Space` 切换输入法时，任务栏/输入指示器显示 Panda五笔 的图标，而不是「简体」两个字。

### 先排除的两条错路

| 尝试 | 结果 |
|---|---|
| 用**通知区托盘图标**来指示 | 用户明确否定：那是 trayicon，不是语言指示器 |
| 注册 `IconFile` 到 DLL + `AddLanguageProfile` | 仍显示「简体」 |
| 实现 `ITfLangBarItemButton` + `ITfLangBarItemMgr::AddItem` | 真实应用里一律 `AddItem failed hr=0x80004005`（Win11 语言栏默认关闭，管理器拒绝条目） |

### 成功的三步（依据 [msime-windows PR #863](https://github.com/metasequoiaime/msime-windows/pull/863)）

1. **`ITfInputProcessorProfileMgr::RegisterProfile` 带图标注册**（不是只写注册表）：
   ```cpp
   pm->RegisterProfile(CLSID_PandaWubi, kLangId, GUID_WubiProfile,
                       desc, (ULONG)wcslen(desc),
                       iconFile.c_str(), (ULONG)iconFile.size(), 0,  // index 0 = .ico 第一组
                       nullptr, 0, TRUE, 0);
   ```
2. **图标必须是固定路径的独立 `.ico`**，不能指向 DLL：
   - 取 `dist\panda.ico`，找不到才回退 DLL + 内嵌资源索引
   - 原因：DLL 路径随版本变化；任务栏某次取不到图标就**缓存「简体」直到 Explorer 重启**
3. **补注册 TIP 能力类别**（原来只有 `TIP_KEYBOARD`）：
   ```cpp
   cm->RegisterCategory(CLSID_PandaWubi, GUID_TFCAT_TIP_KEYBOARD,            CLSID_PandaWubi);
   cm->RegisterCategory(CLSID_PandaWubi, GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT,  CLSID_PandaWubi);
   cm->RegisterCategory(CLSID_PandaWubi, GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT,    CLSID_PandaWubi);
   ```

注册后必须**重启资源管理器**，否则任务栏仍用上次缓存的「简体」。

最终日志：

```
RegisterProfile icon=D:\MobileSync\hanfu\src\wubi\dist\panda.ico
RegisterProfile(icon) hr=0x00000000
RegisterCategory: keyboard=0x0 immersive=0x0 systray=0x0
```

图标成功显示在输入指示器上（用户确认）。

> ⚠️ 维护要点：`dist\panda.ico` 现在是注册表里被引用的**固定路径文件**。
> 删除它或移动 `dist` 目录，图标会退回「简体」，且需要重启资源管理器才能恢复。

## 二·三十六、中/英切换的状态提示（查开源实现后定案）

### 两条路都验证过

| 尝试 | 结果 |
|---|---|
| 切换时改写 `dist\panda.ico` 文件内容（路径不变） | **无效**：shell 缓存的是图标句柄，不重读文件 |
| 写入 TSF 隔间 `GUID_COMPARTMENT_KEYBOARD_OPENCLOSE` / `INPUTMODE_CONVERSION` | 写入成功（`hr=0x0`），但**系统指示器不刷新** |

### 开源的结论

参考 [msime-windows PR #504](https://github.com/metasequoiaime/MSIME-Windows/pull/504) 的注释：

> Only explicit user actions call this: shortcuts (including the system Ctrl+Space toggle), Caps Lock,
> and moving focus into another text field.

也就是说：**只有"系统自己"处理的中/英切换（如 Ctrl+Space）才会让 Windows 更新指示器**；
输入法自己写的切换，系统不刷新。所以开源实现都改为**自己画一个光标旁的状态徽标**。

### 我们的实现

- 隔间写入保留（`OPENCLOSE` + `CONVERSION`），保证与系统状态一致
- 另外在光标旁显示**纯文字徽标**「中」/「英」：
  - 复用候选窗，但 `SetPlain(true)` → **不显示序号**（避免被误认为候选词）
  - `ShowBriefly(rect, 700ms)` → 700ms 后由 `WM_TIMER` 自动隐藏
  - 显示真实候选时 `SetPlain(false)` 恢复数字序号

## 二·三十七、大写字母自动进英文 + 英文单词补全（用户需求）

需求：
1. 正常输入时打**大写字母**（Shift+字母 或 CapsLock）→ 自动进入英文模式，并随输入提示合法英文单词
2. **便捷输入（分号）模式**下打字母 → 也转入英文模式

### 词表来源

从 QQ五笔 的 `engcore.dat`（魔数 `NEPQ`）提取。格式是**长度前缀的 ASCII 串**：

```
06 41 61 63 68 65 6E   ->  "Aachen"
03 41 41 46            ->  "AAF"
```

解析后得到 45,535 个词，写入 `data\eng_words.txt`（437 KB）。

### 排序策略

词表本身是字母序、无词频。为了让常用词优先，`EnglishDict::Suggest()` 的排序是：

1. 内置常用词表（~300 高频词）优先
2. 词更短的优先
3. 字母序

效果：`hel → help hello held …`、`compu → computer compute computers …`。

> 注意：必须**先收集全部匹配项再排序截断**——一开始在收集阶段就用 limit 截断，
> 导致 hello 排在 9 名之外被丢掉。

### 性能：Shift+字母第一下曾经卡 440ms

用户反馈"按 Shift+字母要顿一会儿候选窗才出来"。打点测出耗时全在 `Suggest()`：第一次 439.9ms。

原因：`IsCommon()` 每次都 `LowerAscii(word)` 再和 ~300 个常用词逐个比较，而它在排序比较器里
被调用——单字母前缀能匹配到 4000 个词，排序约 4.8 万次比较，等于 **1900 万次字符串分配**。

改法：

1. 常用词表一次性灌进 `unordered_set`，判定变成一次哈希查找；
2. 收集阶段就把每个词的**小写 key / 长度 / 是否常用**算好存进 `Hit`，比较器只看结构体字段；
3. 顺带修掉的显示问题：词表里 `An/an`、`Word/word` 两种写法会让同一候选出现两次，
   改成按小写 key 去重（同 key 保留全小写那条），比较器最后再用小写 key + 全小写优先做 tiebreak。

结果：`suggest` 439.9ms → 0.4ms（后续 0.0ms），首键总延迟约 20ms（其余是 TSF 组词会话）。
`panda-cli <dict> engsuggest <eng_words.txt>` 里有回归测试：排序用例 + 重复候选检查 + 200 次计时。

### TSF 接入

- 启动时从 `<DLL目录>\data\eng_words.txt` 载入（`eng load: words=45535`）
- 大写字母且未组码 → `SetEnglish(true)` + 开始组词
- 英文模式：字母继续组词、**空格 = 选第 1 个候选补全并补空格**、**回车 = 原样上屏不补全**、
  数字选候选、退格删字母、Esc 取消、其它键先把当前词上屏再放行
- `EngUpdate` 里 `m_candWnd.SetCode(m_engTyped)`：候选窗顶部小框显示**实际敲入的样子**（含大小写），
  不再是上一段中文组码；上屏/取消时清空，避免下次复用时残留
- 没有匹配单词时**不隐藏候选窗**：`RecalcAndResize` 支持"只有顶部小框、没有候选行"的布局
  （宽度按当前输入文本量算，不画翻页箭头和熊猫装饰），`ShowAt` 也不再因为 items 为空而隐藏
- 便捷输入模式下按字母 → `ExitQuick` + 进英文 + 开始组词

## 二·三十八、按应用使用不同配置（[app:<exe>]）

需求：同一个输入法，A 程序用简体、B 程序用繁体（而不是切换输入法本身）。

### 实现

配置加载多了一个参数：宿主进程的可执行文件名（小写）。

```cpp
m_settings = wubi::LoadSettings(GetConfigPath(), HostAppName());
```

`HostAppName()` 用 `GetModuleFileNameW(nullptr, ...)` 取**当前进程**（也就是宿主程序）的 exe 名。
`LoadSettings(ini, appExe)` 在读完 `[Config]` 后，依次套用：

1. `[app:*]` —— 通配规则，对所有程序生效
2. `[app:<appExe>]` —— 具体程序，覆盖通配

小节里没写的键保持原值（把"当前值"作为 `GetPrivateProfileIntW` 的默认值即可）。

### 配置示例

```ini
[Config]
CandVertical=1
CandCount=9
TradChinese=0

[app:notepad.exe]
TradChinese=1          ; 记事本用繁体
CandVertical=0         ; 横排候选
CandCount=5

[app:*]
Level1Mode=0           ; 所有程序都隐藏一级简码
```

### 验证

`wubi-cli appcfg <ini> <exe>` 打印该 exe 的最终生效值（不再启动 TSF，纯离线）。

日志里每次激活都会记录：

```
app config: exe=notepad.exe trad=1 punc=1 vertical=0
```

### 注意

- 每个进程各读一次配置，所以**改完配置要重启宿主程序**（DLL 已加载的进程不会重读）
- `TradChinese` 等值在激活时应用到实例，所以同一程序的不同窗口行为一致

## 二·三十九、候选框外观可配置（字号 / 背景色 / 内框凹陷）

### 配置项

| 键 | 默认 | 说明 |
|---|---|---|
| `CandFont` | 14 | 字号（pt），范围 8–24，原来写死 15 |
| `CandBgColor` | `#F7F9FC` | 背景色，支持 `#RRGGBB` / `0xRRGGBB` / 十进制 |
| `CandInset` | 1 | 内框凹陷效果开关 |

三个键同样支持放进 `[app:<exe>]` 小节，按程序区分外观。

### 绘制

`Paint()` 里：

1. 先用 `bgCol_` 填充背景
2. 画 1px 外框（RGB 206,209,214）
3. `inset_` 打开时，在内缩 2px 的矩形上画"左上暗、右下亮"两条折线：

```
左上：RGB(198,201,207)   右下：纯白
```

这是经典的 3D 内凹画法，不需要额外控件，也不引入主题依赖。

### 设置界面

`panda-settings.exe` 新增三项（窗口加高到 560）：

- 候选框字号（8-24）
- 背景色（#RRGGBB）
- 内框凹陷效果（勾选框）

保存后写入 `[Config]`，`CandBgColor` 以 `#RRGGBB` 形式回写，便于手改。

### 验证

`wubi-cli show <ini>` 会打印最终生效值：

```
... CandFont=12 CandBgColor=#F2F6FB CandInset=1
```

## 二·四十、多词库切换（DictFile）—— 用于对比手感

### 配置

```ini
[Config]
DictFile=qqwubi86.dict.tsv     ; 默认：Rime + QQ 合并
; DictFile=dict-rime.bin       ; 纯 Rime（136,235 条）
; DictFile=dict-qq.bin         ; 纯 QQ（11,462 条）
```

也支持放进 `[app:<exe>]` 小节，让不同程序用不同词库。

### 界面

设置窗口新增「词库（data 目录）」下拉框，自动枚举 `data\*.tsv` 与 `data\*.bin`。

### 词库是怎么切出来的

合并库 `qqwubi86.dict.tsv` 的第 4 列是来源：

```
code	word	weight	source
wqvb	你好	70500000	rime
...
```

按 `source` 分组即得两个变体（`dict-rime.bin` / `dict-qq.bin`），再用
`panda-cli <tsv> pack <bin>` 打包成 mmap 可用的二进制。

### 实测差异（同一批码）

| 码 | 纯 Rime | 纯 QQ |
|---|---|---|
| `wqvb` | 你好 / 您好 / 𠛈 | **无候选** |
| `khkh` | 跟踪 / 踊跃 / 跳跃 | 中路 / 嘘嘘 / 足足 / 跑跑 |

结论：Rime 词频（来自 Google 词频）更贴近现代书面语，QQ 词库更偏口语短词。
用户可据此选默认。

## 二·四十一、输入指示器图标：最终可用方案（实测结论）

### 排查过程（三个坑叠加）

| # | 现象 | 根因 |
|---|---|---|
| 1 | 指示器显示「简体」 | `IconFile` 被 **32 位 regsvr32 最后执行**覆盖成 `panda_tsf32.dll` |
| 2 | 改成 `panda.ico` 仍不显示 | **`panda.ico` 只有 23 字节**（生成脚本的 byte[] 展开 bug），是坏文件 |
| 3 | 文件修好后仍不显示 | 图标放在**深层路径** `D:\MobileSync\hanfu\src\wubi\dist\`，指示器读不到 |

### 可用组合

```
IconFile  = %LOCALAPPDATA%\PandaWubi\panda.ico
IconIndex = 0
```

即：**把图标复制到用户目录**再指向它。`dist\panda.ico` 与 DLL 内资源（`RT_GROUP_ICON 1`）都保留作为回退，
但实测只有用户目录这条路在 Win11 输入指示器上生效。

### 落地

- `register.cmd`：复制 `panda.ico` → `%LOCALAPPDATA%\PandaWubi\`，`attrib +R` 置只读，再写注册表
- `DllRegisterServer`：同样的优先级（LOCALAPPDATA 副本 → DLL 同目录 → DLL 自身）
- `dist\seticon.cmd`：保留为诊断工具（file / dir / dll / dllneg 四种模式）
- `verify.ps1`：校验 `panda.ico` 的大小与 **SHA256**（期望值存 `build\panda-ico.sha256`），
  防止再次出现"文件被改成 23 字节却没人发现"的情况

### 关键教训

> 语言指示器图标这种"系统帮我们显示"的功能，失败时**不要只看注册表**——
> 要同时验证：① 路径是否被后续注册覆盖；② 文件本身是否完整；③ 路径是否在系统可读的常规位置。

## 三、TSF 外壳（已实现，真机已验证工作）

```
src/tsf/
  dllmain.cpp        DllMain / DllGetClassObject / DllRegisterServer
  text_service.cpp   ITfTextInputProcessorEx + ITfThreadMgrEventSink
  key_sink.cpp       ITfKeyEventSink: a-y 组码, 空格/数字上屏, Esc 取消, 退格
  composition.cpp    ITfComposition 显示组码串
  candidate_ui.cpp   ITfCandidateListUIElement(+Behavior) 或自绘分层窗
  config.cpp         Config.ini 兼容 QQ五笔 键名
```

平台要点（针对 QQ五笔 在 Win11 上的两个病根）：
1. **每宿主进程一份引擎** 无法避免（TSF 模型如此），但要把初始化做成惰性 + 轻量，
   不在 `ImeInquire` 阶段碰文件/网络；词库用共享内存只读映射，避免 16 个进程各读一遍 3 MB。
2. **候选窗跟随** 必须走 TSF 的 `ITfContextView::GetTextExt` + 每屏 DPI 感知（`WM_DPICHANGED`），
   绝不使用 `SetCursorPos`/`mouse_event`/全局钩子。

注册：`HKLM\SOFTWARE\Microsoft\CTF\TIP\{CLSID}` + LanguageProfile `0x0804`，
CLSID 由我们自己生成，不与 QQ五笔 冲突（它用 `{6AF21238-…}`）。

现状：`src/tsf/wubi_tsf.cpp` 已实现 COM 服务、组码、候选窗、上屏、自注册，
编译产物 `dist/panda_tsf.dll`（304 KB，x64），已接入简繁与标点。

已验证：`LoadLibrary` · `DllGetClassObject` 四个导出 · `CoCreateInstance` ·
`ITfTextInputProcessor` / `Ex` QI 全部成功。

阻塞点：TSF 的 profile 目录在 `HKLM\SOFTWARE\Microsoft\CTF\TIP`，当前账户不可写
（实测 `E_ACCESSDENIED`），因此 `AddLanguageProfile`/`EnableLanguageProfile` 全部 `E_FAIL`，
`EnumLanguageProfiles(zh-CN)` 能看到 5 个既有配置但不含我们。
→ 需要一次管理员权限的 `dist\register.cmd`（见 [test-guide.md](test-guide.md)）。
DLL 已改为权限不足时返回 `E_ACCESSDENIED`，不再静默假成功。

功能顺序（对应用户选择）：
1. 五笔86 码表 + 词组（数据已就绪）
2. 用户自造词 + 词频自学习（`AddUserWord` 已具备，需持久化）
3. 简繁转换 + 标点矩阵（`simp2trad.dat` 65,536 项映射表结构已摸清，`punc.ini` 四列矩阵已摸清）
4. 五笔/拼音混输
5. 迁移 QQ五笔 配置与用户词库（提取器已就绪）

## 四、版权与分发说明

`data/*.tsv` 中的词表源自腾讯 QQ五笔 与 Rime 词库，仅供本地个人使用；
若要公开发布，应替换为可再分发词库（Rime wubi86 为 Apache-2.0），仅保留引擎与规则。
