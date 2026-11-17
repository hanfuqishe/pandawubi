# 数据文件说明

本目录的**大部分文件不随仓库分发**（见根目录 `.gitignore`），因为它们是从第三方输入法词库
转换或提取出来的，版权不属于本项目。仓库只包含生成它们的脚本，首次使用请按下面步骤自行生成。

## 仓库里保留的文件

| 文件 | 说明 |
|---|---|
| `punc.ini` | 中文标点映射表（本项目自写，MIT 许可） |
| `README.md` | 本文件 |

## 需要自行生成的五类数据

| 文件 | 用途 | 生成方式 |
|---|---|---|
| `qqwubi86.dict.tsv` | 主码表（码 / 词 / 权重 / 来源） | `tools\build-dict.ps1` |
| `qqwubi86.bin` | 上面 TSV 的二进制形态，载入快 | `panda-cli pack data\qqwubi86.dict.tsv data\qqwubi86.bin` |
| `char_fullcodes.tsv` | 单字五笔 86 全码，用于给词生成码 | `tools\build-dict.ps1 -Mode chars` |
| `eng_words.txt` | 英文单词补全词表 | `tools\extract-eng.ps1` |
| `simp2trad.dat` | 简→繁映射（16 位查表，可选） | `tools\build-simp2trad.ps1` |

### 1. 主码表（必需）

`qqwubi86.dict.tsv` 的格式是一个 TSV，含表头：

```
code	word	weight	source
wqvb	你好	1200000	rime
pygf	祬	44400	rime
```

- `code`：五笔 86 码（1–4 个字母，小写）
- `word`：词条（1–4 字常见，也允许更长）
- `weight`：词频权重，越大越靠前
- `source`：`rime` / `qq` / `user`，仅作标记

推荐用 **Rime 的 wubi86 词库**（开源授权）作为基座：

```powershell
# 先安装 Rime 或下载 wubi86.dict.yaml，然后：
powershell -File tools\build-dict.ps1 -YamlPath "C:\path\to\wubi86.dict.yaml"
powershell -File tools\build-dict.ps1 -Mode chars -YamlPath "C:\path\to\wubi86.dict.yaml"
build\panda-cli.exe data\qqwubi86.dict.tsv pack data\qqwubi86.bin   # 生成 .bin
```

如果你有安装 QQ五笔，`build-dict.ps1` 还会把它系统词库里的、Rime 未覆盖的词补进来
（可用 `-QqWords ""` 关闭）。

### 2. 英文词表（英文补全用，可选）

`tools\extract-eng.ps1` 从 QQ五笔 的 `engcore.dat`（魔数 `NEPQ`，长度前缀字符串）里提取：

```powershell
powershell -File tools\extract-eng.ps1 -Source "C:\Program Files (x86)\Tencent\QQWubi\2.4.629.400\engcore.dat"
# 或从你自备的纯文本词表（每行一个词）生成：
powershell -File tools\extract-eng.ps1 -TextList "C:\path\to\words.txt"
```

没有这个文件时，输入法仍可正常工作，只是**英文单词补全**不生效。

### 3. 简繁映射（可选）

`tools\build-simp2trad.ps1` 读取一个「简体↔繁体」对照文本（每行 `简<TAB>繁`），
生成 65536 项的 16 位查表 `simp2trad.dat`：

```powershell
powershell -File tools\build-simp2trad.ps1 -Source "C:\path\to\STCharacters.txt"
```

可用 [OpenCC](https://github.com/BYVoid/OpenCC) 的 `STCharacters.txt`。
缺这个文件时简繁输出退化为原样输出（仅 `Ctrl+Shift+T` 无效果），不影响其它功能。

## 授权提醒

- 从 QQ五笔 / 其它商业输入法提取的词库**不可再分发**，仅供个人本地使用
- 用于开源的码表请选择授权明确的来源（如 Rime 的 wubi86，注意其 GPL/CC 条款）
- 本仓库代码与 `punc.ini` 采用 MIT 许可；第三方词库的来源与授权由使用者自行确认
