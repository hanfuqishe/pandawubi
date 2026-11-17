# 一键验证：构建 + 引擎 + 数据 + TSF 自检
# 退出码: 0 全通过；非 0 表示有检查失败
Add-Type -AssemblyName System.Drawing -ErrorAction SilentlyContinue
$ErrorActionPreference = 'Continue'
$ws = Split-Path $PSScriptRoot -Parent
$cli = Join-Path $ws 'build\panda-cli.exe'
$dictTsv = Join-Path $ws 'data\qqwubi86.dict.tsv'
$dict = Join-Path $ws 'data\qqwubi86.bin'
$selftest = Join-Path $ws 'dist\tsf-selftest.exe'
$dll = Join-Path $ws 'dist\panda_tsf.dll'
$fail = 0
function Check($name, $ok, $detail) {
    if ($ok) { "  PASS  $name  $detail" } else { "  FAIL  $name  $detail"; $script:fail++ }
}

"— 构建 —"
& cmd /c "$ws\build.bat" *> $null
Check 'build' ($LASTEXITCODE -eq 0) "exit=$LASTEXITCODE"

Push-Location $ws
"— 词库格式：二进制 mmap vs TSV —"
$out = & $cli $dictTsv pack (Join-Path $ws 'build\test.bin') 2>&1 | Out-String
Check 'pack 一致性' ($out -match 'wqvb equal=YES') 'TSV 打包后查询结果一致'
$out = & $cli $dict lookup a 2>&1 | Out-String
$m = [regex]::Match($out, 'in ([0-9.]+) ms')
$ms = if ($m.Success) { [double]$m.Groups[1].Value } else { 1e9 }
Check 'bin 载入耗时' ($ms -lt 30) ("$ms ms (mmap，阈值 30ms)")
Check 'bin 模式生效' ($out -match 'binary/mmap') ''
"— 引擎：码查询 —"
$out = & $cli $dict lookup wqvb 2>&1 | Out-String
Check 'lookup wqvb' ($out -match '你好' -and $out -match 'exact=3') '你好/您好/𠛈'
$out = & $cli $dict lookup trnt 2>&1 | Out-String
Check 'lookup trnt (一级常用)' ($out -match '我\[trnt') '我 在第一候选'
$out = & $cli $dict lookup a 2>&1 | Out-String
Check 'lookup a (一级简码)' ($out -match '工\[a,') '工 在第一候选'

"— 引擎：前缀查询性能 —"
$out = & $cli $dict bench 20000 2>&1 | Out-String
$m = [regex]::Match($out, '([\d.]+) us/lookup')
$us = if ($m.Success) { [double]$m.Groups[1].Value } else { 1e9 }
Check 'bench' ($us -lt 200) ("$us us/lookup (<200us)")

"— 英文单词补全（QQ五笔 engcore 词表）—"
$out = & $cli $dict english hel 2>&1 | Out-String
Check 'english hel' ($out -match 'help' -and $out -match 'hello') ''
$out = & $cli $dict english compu 2>&1 | Out-String
Check 'english compu' ($out -match 'computer') ''
"— 英文补全的大小写（Shift/CapsLock 照抄，不再一律小写）—"
$out = & $cli $dict engcase 2>&1 | Out-String
Check '大小写照抄输入' ($out -match 'engcase: PASS') 'Shift+AB -> ABOUT、Shift+A+b -> About、混排逐字保留'
"— 多词库切换（DictFile）—"
$iniDict = Join-Path $ws 'build\dict-test.ini'
[IO.File]::WriteAllText($iniDict, "[Config]`r`nDictFile=dict-qq.bin`r`n", (New-Object System.Text.ASCIIEncoding))
$out = & $cli $dict show $iniDict 2>&1 | Out-String
Check 'DictFile 可配置' ($out -match 'dict-qq\.bin') '设置里可切换词库'
"— 图标文件健康（避免再次出现 23 字节的坏 ICO）—"
$icoPath = Join-Path $ws 'dist\panda.ico'
$icoLen = (Get-Item $icoPath).Length
$icoWant = Get-Content (Join-Path $ws 'build\panda-ico.sha256') -ErrorAction SilentlyContinue
$icoHash = (Get-FileHash $icoPath -Algorithm SHA256).Hash
Check 'panda.ico 大小正常' ($icoLen -gt 20000) ("当前 " + $icoLen + " 字节")
Check 'panda.ico 未被篡改' ($icoWant -and ($icoHash -eq $icoWant.Trim())) 'SHA256 与期望一致'
$icoOk = $true
foreach ($sz in 16, 32, 48) {
    try { $ic = New-Object System.Drawing.Icon($icoPath, $sz, $sz); $ic.Dispose() } catch { $icoOk = $false }
}
Check 'panda.ico 可加载' $icoOk '各尺寸都能被 GDI+ 读取'
"— 候选框外观（字号/背景色/凹陷）—"
$styleIni = Join-Path $ws 'build\styletest.ini'
$out = & $cli $dict show $styleIni 2>&1 | Out-String
Check '样式键解析' ($out -match 'CandFont=12' -and $out -match 'CandBgColor=#F2F6FB' -and $out -match 'CandInset=1') '#RRGGBB 与整数字号都能读'
"— 按应用配置覆盖（[app:<exe>]）—"
$appIni = Join-Path $ws 'tools\fixtures\app-rules.ini'
$out = & $cli $dict appcfg $appIni notepad.exe 2>&1 | Out-String
Check 'app 覆盖生效' ($out -match 'TradChinese=1' -and $out -match 'CandVertical=0' -and $out -match 'CandCount=5') 'notepad.exe 应套用繁体+横排+5'
$out = & $cli $dict appcfg $appIni code.exe 2>&1 | Out-String
Check 'app:* 通配生效' ($out -match 'TradChinese=0' -and $out -match 'Level1Mode=0') 'code.exe 只吃通配规则'
"— 特殊码：date / time / week —"
$out = & $cli $dict special date 2>&1 | Out-String
Check 'date 四种格式' ($out -match '2026\.11\.17' -and $out -match '2026年11月17日' -and $out -match '2026-11-17' -and $out -match '二〇二六年十一月十七日') ''
$out = & $cli $dict special time 2>&1 | Out-String
Check 'time 五种格式' ($out -match '10:51:35' -and $out -match 'AM 10:51:35' -and $out -match '二〇二六年十一月十七日 10:51:35') ''
Check 'time 第2位是短横线日期' ($out -match 'time:\s+10:51:35\s+2026-11-17 10:51:35') '按用户要求：原第 5 位调到第 2 位'
$out = & $cli $dict special week 2>&1 | Out-String
Check 'week 三种格式' ($out -match '星期二' -and $out -match '2026年11月17日 星期二') ''
$out = & $cli $dict sim 'date_' 2>&1 | Out-String
Check 'date 上屏' ($out -match "COMMIT='2026\.11\.17'") ''
$out = & $cli $dict sim 'week_' 2>&1 | Out-String
Check 'week 上屏' ($out -match "COMMIT='星期二'") ''
$out = & $cli $dict sim 'time2' 2>&1 | Out-String
Check 'time 数字选格式' ($out -match "COMMIT='2026-11-17 10:51:35'") 'time2 现在选短横线日期格式'
"— 便捷输入：数字转中文 —"
$out = & $cli $dict numfmt 123 2>&1 | Out-String
Check '数字 123' ($out -match '\[1\]一二三' -and $out -match '\[2\]一百二十三' -and $out -match '\[3\]壹佰贰拾叁') '逐位/进位/大写三种形式'
$out = & $cli $dict numfmt 10 2>&1 | Out-String
Check '数字 10' ($out -match '\[2\]十' -and $out -match '\[3\]壹拾') '十X 与 壹拾'
$out = & $cli $dict numfmt 101 2>&1 | Out-String
Check '数字 101' ($out -match '\[2\]一百零一') '补零'
$out = & $cli $dict numfmt 100010001 2>&1 | Out-String
Check '数字 亿级' ($out -match '\[2\]一亿零一万零一') '跨组补零'
$out = & $cli $dict numfmt 100200 2>&1 | Out-String
Check '数字 十万' ($out -match '\[2\]十万零二百') '最高组 10-19 读作 十X'
"— 分号/单引号选词 —"
$out = & $cli $dict sim 'khkh;' 2>&1 | Out-String
Check '分号选第2候' ($out -match "COMMIT='踊跃'") '; 选择第 2 个候选'
$out = & $cli $dict sim "khkh'" 2>&1 | Out-String
Check '单引号选第3候' ($out -match "COMMIT='跳跃'") "' 选择第 3 个候选"
"— 单键布局（1号=一级简码，2号=该键四次码对应的字）—"
$out = & $cli $dict lookup k 2>&1 | Out-String
Check '单键 k -> 中/口' ($out -match 'candidate\(s\): 中\[k,.* 口\[kkkk,') '中 一级简码，口 是 kkkk'
$out = & $cli $dict lookup g 2>&1 | Out-String
Check '单键 g -> 一/王' ($out -match 'candidate\(s\): 一\[g,.* 王\[gggg,') '一 一级简码，王 是 gggg'
$out = & $cli $dict lookup t 2>&1 | Out-String
Check '单键 t -> 和/禾' ($out -match 'candidate\(s\): 和\[t,.* 禾\[tttt,') '和 一级简码，禾 是 tttt'
$out = & $cli $dict lookup w 2>&1 | Out-String
Check '单键 w 不重复' ($out -match 'candidate\(s\): 人\[w,' -and $out -notmatch '人\[w,.* 人\[wwww') 'wwww 也是「人」，不重复出现'
"— 常见字被学成用户词后仍在首位 —"
$uCommon = Join-Path $ws 'tools\fixtures\user-common.tsv'
$out = & $cli $dict usertest $uCommon rhf 1 2>&1 | Out-String
Check 'rhf -> 看' ($out -match 'code=rhf -> 看') '生僻同码字 𣥆 不能顶掉 看'
$out = & $cli $dict usertest $uCommon qaj 1 2>&1 | Out-String
Check 'qaj -> 错' ($out -match 'code=qaj -> 错') '生僻同码字 𢌳 不能顶掉 错'
"— 简码保护（1/2/3 码不被词频顶掉，四码才看词频）—"
$uFix = Join-Path $ws 'tools\fixtures\user-level1.tsv'
$out = & $cli $dict usertest $uFix g 1 2>&1 | Out-String
Check '一级简码 g=一' ($out -match 'code=g -> 一') '学到 g->来 也不能顶掉 一'
$out = & $cli $dict usertest $uFix b 1 2>&1 | Out-String
Check '一级简码 b=了' ($out -match 'code=b -> 了') '学到 b->子 也不能顶掉 了'
$out = & $cli $dict usertest $uFix aa 1 2>&1 | Out-String
Check '二级简码 aa=式' ($out -match 'code=aa -> 式') '高权重学习词 戒 不能顶掉 式'
$out = & $cli $dict usertest $uFix aaa 1 2>&1 | Out-String
Check '三级简码 aaa=工' ($out -match 'code=aaa -> 工') '隐藏/沉底模式下精确匹配仍豁免'
$out = & $cli $dict usertest $uFix aaaa 1 2>&1 | Out-String
Check '四码才看词频' ($out -match 'code=aaaa -> 恭恭敬敬') '四码不强制简码优先'
"— 简繁转换 —"
$out = & $cli $dict trad '汉语汉字简化龙' 2>&1 | Out-String
Check 'trad' ($out -match '漢語漢字簡化龍') '汉语汉字简化龙 -> 漢語漢字簡化龍'

"— 标点矩阵 —"
$out = & $cli $dict punc 2>&1 | Out-String
Check 'punc 逗号' ($out -match '\[1\]，') 'comma -> ，'
Check 'punc 省略号' ($out -match '\[1\]……') '^ -> ……'
Check 'punc 引号成对' ($out -match 'toggles: “ ” “') '“ ” “'

"— 用户词自学习 + 持久化 —"
$out = & $cli $dict userdemo 2>&1 | Out-String
Check 'userdemo' ($out -match 'RESULT: PASS') '提频后置顶并可持久化重载'

$out = & $cli $dictTsv userdemo 2>&1 | Out-String
Check 'userdemo(TSV 模式)' ($out -match 'RESULT: PASS') '两种加载模式都能学习并持久化'
"— 设置界面（wubi-settings）—"
$setExe = Join-Path $ws 'build\panda-settings.exe'
$iniT   = Join-Path $ws 'build\settings-test.ini'
Copy-Item (Join-Path $ws 'dist\config.ini') $iniT -Force
$env:WUBI_INI = $iniT
$out = & $setExe /set Level1Mode=1 SortMode=1 2>&1 | Out-String
Check '设置写入' ($out -match 'saved -> Level1Mode=1 SortMode=1') '通过命令行写入两项'
$out = & $setExe /dump 2>&1 | Out-String
Check '设置回读' ($out -match 'Level1Mode=1 SortMode=1') '写后能读回同样的值'
$fileText = Get-Content $iniT -Encoding UTF8 -Raw
Check '设置落盘' ($fileText -match 'Level1Mode=1' -and $fileText -match 'SortMode=1') 'config.ini 里确实有这两项'
$out = & $setExe /set NoSuchKey=1 2>&1 | Out-String
Check '设置拒绝未知键' ($out -match 'no known key') '未知键不静默通过'
Remove-Item Env:\WUBI_INI
"— 数字后的半角逗号/句号 —"
$out = & $cli $dict gatesim '3,' 2>&1 | Out-String
Check '数字后半角逗号' ($out -match "半角 ','") '3 之后按逗号输出半角'
$out = & $cli $dict gatesim '3.' 2>&1 | Out-String
Check '数字后半角句号' ($out -match "半角 '\.'") '3 之后按句号输出半角'
$out = & $cli $dict gatesim '3!' 2>&1 | Out-String
Check 'Esc 拦截待定' ($out -match 'Esc -> 拦截') '待定时 Esc 被拦截并取消'
$out = & $cli $dict gatesim '3!,' 2>&1 | Out-String
Check 'Esc 后回到全角' ($out -match "全角 '，'") '取消待定后逗号为全角'
$out = & $cli $dict gatesim '3a,' 2>&1 | Out-String
Check '普通键退出待定' ($out -match "全角 '，'") '数字与逗号之间夹了字母则不打半角'
"— 四码后继续输入（顶字上屏）—"
$out = & $cli $dict sim 'trntx' 2>&1 | Out-String
Check '顶字上屏' ($out -match 'commit\+cont' -and $out -match "COMMIT='特性'" -and $out -match "code='x'") 'trnt 满四码再按 x：上屏 特性 并以 x 开新码'
$out = & $cli $dict sim 'aaaaa' 2>&1 | Out-String
Check '连续顶字' ($out -match "COMMIT='恭恭敬敬'" -and $out -match "code='a'") 'aaaa 满四码再按 a：上屏 恭恭敬敬 并以 a 开新码'
$out = & $cli $dict sim 'trnt_' 2>&1 | Out-String
Check '四码空格仍正常' ($out -match "committed" -and $out -match "COMMIT='特性'") '空格路径未被顶字逻辑影响'
"— 组码状态机（纯逻辑，等价于 TSF 按键路径）—"
$out = & $cli $dict sim 'wqvb_' 2>&1 | Out-String
Check 'sim 组码上屏' ($out -match "COMMIT='你好'") 'wqvb + 空格 -> 你好'
$out = & $cli $dict sim 'trnt3' 2>&1 | Out-String
Check 'sim 数字选词' ($out -match "COMMIT='我'") 'trnt + 3 -> 我（词组优先后我在第 3 位）'
$out = & $cli $dict sim 'wqvb<<!' 2>&1 | Out-String
Check 'sim 退格/取消' ($out -match 'cancelled' -and $out -match 'commits=0') '退格收窄 + Esc 取消'
$qqOrder = Join-Path $ws 'data\qq-order.tsv'
if (Test-Path $qqOrder) {
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $ws 'tools\compare-order.ps1') -QqFile $qqOrder 2>&1 | Out-String
    Check '双端首位一致率' ($out -match '首位一致\s+: 6 / 6') '与 QQ五笔 真实候选顺序（oracle 取得）首位 100% 一致'
} else {
    "  SKIP  双端对比  data\qq-order.tsv 不存在"
}
$out = & $cli $dict order 0 0 2>&1 | Out-String
Check 'level1 模式0 不处理' ($out -match 'aaaa\t1:恭恭敬敬\t2:劳斯莱斯\t3:花花草草\t4:工') 'aaaa 第 4 位是 工'
$out = & $cli $dict order 0 1 2>&1 | Out-String
$aaaaLine = ($out -split "`n" | Where-Object { $_ -match '^aaaa\t' })
Check 'level1 模式1 隐藏' ($aaaaLine -match '1:恭恭敬敬' -and $aaaaLine -notmatch '工') 'aaaa 那一行里不再出现 工'
$out = & $cli $dict order 0 2 2>&1 | Out-String
Check 'level1 模式2 沉底' ($out -match 'aaaa\t.*5:工') '工 被沉到 aaaa 的末位'
"— 开关持久化 / 上屏学习节流 —"
$cfgTest = Join-Path $ws 'build\toggle-test.ini'
Copy-Item (Join-Path $ws 'dist\config.ini') $cfgTest -Force
$out = & $cli $dict toggle trad $cfgTest 2>&1 | Out-String
Check 'toggle trad 持久化' ($out -match 'reload-match=PASS') 'Ctrl+Shift+T 写回配置并可回读'
$out = & $cli $dict toggle punc $cfgTest 2>&1 | Out-String
Check 'toggle punc 持久化' ($out -match 'reload-match=PASS') 'Ctrl+Shift+P 写回配置并可回读'
$out = & $cli $dict learnsim 16 2>&1 | Out-String
Check 'learnsim 节流落盘' ($out -match 'RESULT=PASS' -and $out -match '文件=有') '16 次提交后首位翻转且落盘'
"— 候选排序可调 / 双端对比工具 —"
$out = & $cli $dict order 0 2>&1 | Out-String
Check 'order 模式0' ($out -match 'a\t1:工\t2:区\t3:东') '一级简码在前，其后按频率的二级简码'
$out = & $cli $dict order 1 2>&1 | Out-String
Check 'order 模式1' ($out -match 'a\t1:区') '纯词频模式下首位变为 区'
$cmp = Join-Path $ws 'tools\compare-order.ps1'
$fx  = Join-Path $ws 'tools\fixtures\qq-order-sample.tsv'
$out = & powershell -NoProfile -ExecutionPolicy Bypass -File $cmp -QqFile $fx 2>&1 | Out-String
Check 'compare-order 判定' ($out -match '首位一致\s+: 3 / 4' -and $out -match 'wqvb.*no') '能识别顺序相反的码'
"— 稳定性 —"
$out = & $cli $dict fuzz 20000 2>&1 | Out-String
Check 'fuzz 随机按键' ($out -match 'RESULT: PASS') '2 万次随机序列，不变量成立'
$out = & $cli $dict stress 2>&1 | Out-String
Check 'stress 全码表' ($out -match 'RESULT: PASS') '全部 2/3 码组合 16,250 次查询'
$out = & $cli $dict memory 2>&1 | Out-String
$m = [regex]::Match($out, 'privateCommit=([0-9.]+) MB')
$priv = if ($m.Success) { [double]$m.Groups[1].Value } else { 1e9 }
Check 'memory 私有提交' ($priv -lt 2.0) ("$priv MB (mmap，16 进程合计更省)")
"— 候选窗几何 / 点选命中 —"
$out = & $cli $dict layout 2>&1 | Out-String
Check 'layout 横竖排尺寸' ($out -match 'size check: horizontal=PASS vertical=PASS') '92x24 / 36x64'
Check 'layout 命中测试' ($out -match 'hit check: H=PASS V=PASS miss=PASS') '每项中心命中自身，越界返回 -1'
"— 配置迁移（QQ五笔 Config.ini -> 我们）—"
$qqIni = Join-Path $env:APPDATA 'Tencent\QQWubi\SAccount\94af6dee31e85ea767e75a5d275da376\Config.ini'
if (Test-Path $qqIni) {
    $out = & $cli $dict migrate $qqIni (Join-Path $ws 'build\migrated.ini') 2>&1 | Out-String
    Check 'migrate' ($out -match 'reload match=PASS') '键名一致，迁移后可回读'
    $out = & $cli $dict show (Join-Path $ws 'build\migrated.ini') 2>&1 | Out-String
    Check 'config show' ($out -match 'InputMode=0') 'InputMode=0 (五笔)'
} else {
    "  SKIP  migrate  QQ五笔 配置不存在"
}

"— 双架构产物 —"
$x86dll = Join-Path $ws 'dist\panda_tsf32.dll'
Check 'x86 DLL 存在' (Test-Path $x86dll) $(if (Test-Path $x86dll) { "$((Get-Item $x86dll).Length) bytes" } else { 'missing' })
if (Test-Path $x86dll) {
    $b = [IO.File]::ReadAllBytes($x86dll)
    $peOff = [BitConverter]::ToInt32($b, 0x3C)
    $machine = [BitConverter]::ToUInt16($b, $peOff + 4)
    Check 'x86 机器类型' ($machine -eq 0x014C) ("0x{0:X4}" -f $machine)
}

"— TSF 组件 —"
$out = & $selftest $dll 2>&1 | Out-String
Check 'LoadLibrary' ($out -match 'LoadLibrary OK') ''
Check 'CoCreateInstance' ($out -match 'CoCreateInstance -> hr=0x00000000') ''
Check 'QI ITfTextInputProcessorEx' ($out -match 'QI ITfTextInputProcessorEx -> 0x00000000') ''
Check 'dict 路径解析' ($out -match 'dict exists') ''
if ($out -match 'registered machine-wide') {
    # 已机器级注册：非管理员调用 DllRegisterServer 返回 E_ACCESSDENIED 属预期
    Check 'DllRegisterServer' ($true) '已机器级注册（HKLM 键存在）'
    Check 'TSF 目录可见' ($out -match 'ours registered=YES') ''
    Check '文本服务可激活' ($out -match 'ITfTextInputProcessor::Activate -> 0x00000000') ''
} elseif ($out -match 'needs administrator') {
    "  SKIP  DllRegisterServer  需要管理员权限（右键 dist\register.cmd 以管理员身份运行）"
} else {
    Check 'DllRegisterServer' ($out -match 'DllRegisterServer -> hr=0x00000000') ''
    Check 'TSF 目录可见' ($out -match 'ours registered=YES') ''
}
"— 悬浮状态栏（单实例 + 前台显隐）—"
# 回归：TSF 每个线程各建一个文本服务实例，旧实现每实例各建一条悬浮栏，切走应用又收不掉，
# 桌面上会攒出 8~10 条「中 简 ，。 ⚙」。这里断言进程内始终只有 1 条、全释放后归零。
$barTest = Join-Path $ws 'build\bar-singleton-test.exe'
if (-not (Test-Path $barTest)) { $barTest = Join-Path $ws 'dist\bar-singleton-test.exe' }
if (Test-Path $barTest) {
    $barOut = & $barTest $dll 2>&1 | Out-String
    Check '状态栏单实例' ($barOut -match '两个实例仍只 1 条') ''
    Check '状态栏随前台显隐' ($barOut -match '非前台时栏隐藏') ''
    Check '状态栏窗口可销毁' ($barOut -match '全部释放后窗口销毁') ''
} else {
    "  SKIP  bar-singleton-test.exe 未构建（先跑 build.bat）"
}

"— 诊断日志 —"
$logPath = Join-Path $ws 'build\wubi_tsf.log'
Remove-Item $logPath -ErrorAction SilentlyContinue
$env:WUBI_LOG = $logPath
& $selftest $dll *> $null
Remove-Item Env:\WUBI_LOG -ErrorAction SilentlyContinue
$logOk = (Test-Path $logPath) -and ((Get-Content $logPath -Raw) -match 'DllMain: attach')
Check 'log 写入' $logOk 'WUBI_LOG 覆盖路径（默认 %LOCALAPPDATA%\PandaWubi\)'

Pop-Location

""
if ($fail -eq 0) { "全部检查通过"; exit 0 } else { "$fail 项失败"; exit 1 }
