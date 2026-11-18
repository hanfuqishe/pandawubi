// wubi-cli —— 引擎验证 harness
// 用法:
//   wubi-cli <dict.tsv>                      交互式查询（输入码回车）
//   wubi-cli <dict.tsv> lookup <code>
//   wubi-cli <dict.tsv> bench [n]            前缀查询性能
//   wubi-cli <dict.tsv> trad <utf8 text>     简->繁
//   wubi-cli <dict.tsv> punc [mode]          标点矩阵（mode 0..3）
#include "../engine/wubi_engine.h"
#include "../engine/wubi_config.h"
#include "../engine/wubi_composer.h"
#include "../engine/wubi_english.h"
#include "../ui/cand_layout.h"

#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")

static std::wstring Widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static void ShowCandidates(const std::vector<wubi::Candidate>& cands, size_t limit) {
    if (cands.empty()) { std::printf("  (no candidate)\n"); return; }
    std::printf("  %zu candidate(s):", cands.size());
    size_t shown = 0;
    for (const auto& e : cands) {
        std::printf(" %s[%s,%lld]%s", e.word.c_str(), e.code.c_str(), (long long)e.weight, e.user ? "(u)" : "");
        if (++shown >= limit) break;
    }
    std::printf("\n");
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::printf("usage: wubi-cli <dict.tsv> [command] [args]\n");
        return 2;
    }
    wubi::Dictionary dict;
    std::wstring wpath(argv[1]);
    bool isBin = wpath.size() > 4 && wpath.compare(wpath.size() - 4, 4, L".bin") == 0;
    auto t0 = std::chrono::high_resolution_clock::now();
    long long n = isBin ? dict.LoadBinary(wpath) : dict.Load(wpath);
    auto t1 = std::chrono::high_resolution_clock::now();
    if (n < 0) { std::printf("failed to load: %ls\n", argv[1]); return 1; }
    std::printf("loaded %lld entries (%s) in %.1f ms\n", n,
                dict.UsingBinary() ? "binary/mmap" : "tsv",
                std::chrono::duration<double, std::milli>(t1 - t0).count());

    std::string cmd = (argc > 2) ? Narrow(argv[2]) : "";

    if (cmd == "pack" && argc > 3) {
        if (!dict.SaveBinary(std::wstring(argv[3]))) { std::printf("pack FAILED\n"); return 1; }
        std::printf("packed -> %ls\n", argv[3]);
        // 自检：重新以二进制载入并比较一个查询
        wubi::Dictionary d2;
        long long n2 = d2.LoadBinary(std::wstring(argv[3]));
        auto a1 = dict.Lookup("wqvb", 5);
        auto a2 = d2.Lookup("wqvb", 5);
        bool same = a1.size() == a2.size();
        if (same) for (size_t i = 0; i < a1.size(); ++i) if (a1[i].word != a2[i].word) same = false;
        std::printf("  verify: bin entries=%lld  wqvb equal=%s\n", n2, same ? "YES" : "NO");
        return same ? 0 : 1;
    }

    if (cmd == "lookup" && argc > 3) {
        std::string code = Narrow(argv[3]);
        auto cands = dict.Lookup(code, 20);
        std::printf("code='%s' exact=%zu prefix=%zu\n", code.c_str(), dict.stats_.exactHits, dict.stats_.prefixHits);
        ShowCandidates(cands, 15);
        return 0;
    }

    if (cmd == "bench") {
        int reps = (argc > 3) ? std::atoi(Narrow(argv[3]).c_str()) : 20000;
        if (reps <= 0) reps = 20000;
        // 用真实输入序列: 单击键、双码、三码、四码混合
        const char* seq[] = { "a", "wq", "wqv", "wqvb", "trnt", "khkh", "sg", "r" };
        size_t idx = 0;
        auto t0 = std::chrono::high_resolution_clock::now();
        size_t total = 0;
        for (int i = 0; i < reps; ++i) {
            total += dict.Lookup(seq[idx], 9).size();
            idx = (idx + 1) % (sizeof(seq) / sizeof(seq[0]));
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
        std::printf("bench: %d lookups in %.2f ms  => %.1f us/lookup  (avg %.1f candidates)\n",
                    reps, us / 1000.0, us / reps, (double)total / reps);
        return 0;
    }

    if (cmd == "fuzz") {
        // 随机按键序列，验证状态机不崩、不变量成立
        int iters = (argc > 3) ? std::atoi(Narrow(argv[3]).c_str()) : 20000;
        if (iters <= 0) iters = 20000;
        unsigned seed = 12345;
        auto rnd = [&seed]() { seed = seed * 1103515245u + 12345u; return (seed >> 16) & 0x7FFF; };

        wubi::Composer comp(&dict);
        comp.SetLimit(9);
        size_t commits = 0, updates = 0, cancels = 0, passes = 0;
        bool ok = true;
        for (int i = 0; i < iters && ok; ++i) {
            wubi::Composer::Outcome o;
            switch (rnd() % 5) {
            case 0: { char c = (char)('a' + (rnd() % 25)); o = comp.TypeCode(c); break; }
            case 1: o = comp.SelectDigit(1 + (rnd() % 9)); break;
            case 2: o = comp.SelectBest(); break;
            case 3: o = comp.Backspace(); break;
            default: o = comp.Escape(); break;
            }
            switch (o.act) {
            case wubi::Composer::Act::Committed:
                commits++;
                if (o.commit.empty()) { std::printf("FUZZ VIOLATION: empty commit at iter %d\n", i); ok = false; }
                break;
            case wubi::Composer::Act::Updated: updates++; break;
            case wubi::Composer::Act::Cancelled: cancels++; break;
            default: passes++; break;
            }
            if (comp.Code().size() > 4) { std::printf("FUZZ VIOLATION: code > 4 at iter %d\n", i); ok = false; }
            if (comp.Candidates().size() > 9) { std::printf("FUZZ VIOLATION: cands > 9 at iter %d\n", i); ok = false; }
            for (char ch : comp.Code()) if (!wubi::IsCodeChar(ch)) { std::printf("FUZZ VIOLATION: bad code char\n"); ok = false; }
        }
        std::printf("fuzz %d iters: committed=%zu updated=%zu cancelled=%zu pass=%zu\n",
                    iters, commits, updates, cancels, passes);
        std::printf("RESULT: %s\n", ok ? "PASS" : "FAIL");
        return ok ? 0 : 1;
    }

    if (cmd == "stress") {
        // 枚举全部 2 码 + 3 码组合，验证无崩溃且候选统计合理
        size_t lookups = 0, totalCands = 0, maxCands = 0, empty = 0;
        wubi::Dictionary& d = dict;
        char buf[5] = { 0, 0, 0, 0, 0 };
        auto t0 = std::chrono::high_resolution_clock::now();
        for (int len = 2; len <= 3; ++len) {
            int combos = 1;
            for (int k = 0; k < len; ++k) combos *= 25;
            for (int c = 0; c < combos; ++c) {
                int v = c;
                for (int k = 0; k < len; ++k) { buf[k] = (char)('a' + (v % 25)); v /= 25; }
                buf[len] = 0;
                auto r = d.Lookup(buf, 9);
                ++lookups;
                totalCands += r.size();
                if (r.size() > maxCands) maxCands = r.size();
                if (r.empty()) ++empty;
                if (r.size() > 9) { std::printf("STRESS VIOLATION: >9 candidates for %s\n", buf); return 1; }
            }
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::printf("stress: %zu lookups in %.0f ms (%.1f us each), cands=%zu avg=%.2f max=%zu empty=%zu\n",
                    lookups, ms, ms * 1000.0 / lookups, totalCands,
                    (double)totalCands / lookups, maxCands, empty);
        std::printf("RESULT: PASS\n");
        return 0;
    }

    if (cmd == "memory") {
        PROCESS_MEMORY_COUNTERS_EX pmc = {};
        pmc.cb = sizeof(pmc);
        if (!GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
            std::printf("GetProcessMemoryInfo failed\n");
            return 1;
        }
        std::printf("workingSet=%.1f MB  privateCommit=%.1f MB  (mode=%s)\n",
                    pmc.WorkingSetSize / 1048576.0, pmc.PrivateUsage / 1048576.0,
                    dict.UsingBinary() ? "binary/mmap" : "tsv");
        return 0;
    }
    if (cmd == "order") {
        // 打印指定码的候选顺序（每行: code<TAB>序号<TAB>词），供与 QQ五笔 双端对比
        int mode = 0;
        if (argc > 3) mode = std::atoi(Narrow(argv[3]).c_str());
        int l1 = 2;
        if (argc > 4) l1 = std::atoi(Narrow(argv[4]).c_str());
        dict.SetSortMode(mode);
        dict.SetLevel1Mode(l1);
        std::printf("# sortMode=%d level1Mode=%d\n", mode, l1);
        const char* codes[] = { "a", "trnt", "wqvb", "khkh", "aaaa", "wqwu" };
        for (const char* c : codes) {
            auto r = dict.Lookup(c, 9);
            std::printf("%s", c);
            for (size_t i = 0; i < r.size(); ++i) std::printf("\t%zu:%s", i + 1, r[i].word.c_str());
            std::printf("\n");
        }
        return 0;
    }
    if (cmd == "toggle" && argc > 4) {
        // 模拟 TSF 的 Ctrl+Shift+T / P：翻转 -> 写回配置 -> 重新读取
        std::string what = Narrow(argv[3]);
        std::wstring ini = argv[4];
        wubi::Settings before = wubi::LoadSettings(ini);
        wubi::Settings s = before;
        if (what == "trad")      s.TradChinese = s.TradChinese ? 0 : 1;
        else if (what == "punc") s.EnablePunction = s.EnablePunction ? 0 : 1;
        else { std::printf("usage: toggle trad|punc <ini>\n"); return 2; }

        const bool saved = wubi::SaveSettings(ini, s);
        wubi::Settings after = wubi::LoadSettings(ini);
        bool ok = false;
        if (what == "trad") ok = (after.TradChinese == s.TradChinese && after.TradChinese != before.TradChinese);
        else                ok = (after.EnablePunction == s.EnablePunction && after.EnablePunction != before.EnablePunction);
        std::printf("toggle %s: before=%d after=%d saved=%d reload-match=%s\n",
                    what.c_str(),
                    what == "trad" ? before.TradChinese : before.EnablePunction,
                    what == "trad" ? after.TradChinese : after.EnablePunction,
                    (int)saved, ok ? "PASS" : "FAIL");
        return ok ? 0 : 1;
    }

    if (cmd == "learnsim") {
        // 模拟 TSF 的上屏学习：每 8 次落盘一次，最后验证顺序变化且文件存在
        int n = (argc > 3) ? std::atoi(Narrow(argv[3]).c_str()) : 16;
        if (n <= 0) n = 16;
        auto before = dict.Lookup("wqvb", 3);
        std::string firstBefore = before.empty() ? "" : before[0].word;

        const std::wstring path = L"build\\user_sim.tsv";
        int commits = 0;
        for (int i = 0; i < n; ++i) {
            dict.BumpUserWord("wqvb", "您好");
            ++commits;
            if (commits % 8 == 0) dict.SaveUserWords(path);
        }
        dict.SaveUserWords(path);
        auto after = dict.Lookup("wqvb", 3);
        std::string firstAfter = after.empty() ? "" : after[0].word;
        const bool fileOk = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
        const bool orderOk = (firstAfter == "您好") && (firstBefore != "您好");
        std::printf("learnsim: %d commits, 首位 %s -> %s, 文件=%s, RESULT=%s\n",
                    commits, firstBefore.c_str(), firstAfter.c_str(),
                    fileOk ? "有" : "无", (orderOk && fileOk) ? "PASS" : "FAIL");
        return (orderOk && fileOk) ? 0 : 1;
    }
    if (cmd == "gatesim" && argc > 3) {
        // 模拟"数字后半角逗号/句号"状态机：数字 -> 待定；, . -> 半角；! -> Esc
        std::string script = Narrow(argv[3]);
        wubi::DigitPunctGate gate;
        wubi::Punctuation punc;
        punc.Load(L"data\\punc.ini");
        std::printf("script: %s\n", script.c_str());
        for (char c : script) {
            if (c == '!') {
                if (gate.TakeEsc()) std::printf("  Esc -> 拦截（取消半角待定，不发给应用）\n");
                else                std::printf("  Esc -> 交给应用（无待定）\n");
                continue;
            }
            const bool isDigit = (c >= '0' && c <= '9');
            const bool isPunct = (c == ',' || c == '.');
            if (gate.pending && isPunct) {
                gate.TakeHalfWidth();
                std::printf("  '%c' -> 半角 '%c'\n", c, c);
                continue;
            }
            gate.NoteKey(isDigit, isPunct);
            if (isDigit) { std::printf("  '%c' -> 数字（进入待定）\n", c); continue; }
            if (isPunct) {
                std::wstring out;
                if (punc.Map(c, wubi::PuncMode::ChineseHalf, out)) {
                    std::printf("  '%c' -> 全角 '%s'\n", c, Narrow(out).c_str());
                } else {
                    std::printf("  '%c' -> 原样 '%c'\n", c, c);
                }
                continue;
            }
            std::printf("  '%c' -> 普通键（退出待定）\n", c);
        }
        std::printf("pending=%d\n", (int)gate.pending);
        return 0;
    }
    if (cmd == "usertest" && argc > 4) {
        // 载入用户词后看某个码的顺序（验证一级简码不会被学习结果顶掉）
        const long long n = dict.LoadUserWords(argv[3]);
        int l1 = 2;
        if (argc > 5) l1 = std::atoi(Narrow(argv[5]).c_str());
        dict.SetLevel1Mode(l1);
        auto r = dict.Lookup(Narrow(argv[4]).c_str(), 5);
        std::printf("user words=%lld l1mode=%d code=%s ->", n, l1, Narrow(argv[4]).c_str());
        for (auto& c : r) std::printf(" %s%s", c.word.c_str(), c.user ? "(u)" : "");
        std::printf("\n");
        return 0;
    }
    if (cmd == "numfmt" && argc > 3) {
        std::vector<std::wstring> forms;
        wubi::NumberForms(Narrow(argv[3]), forms);
        std::printf("%s ->", Narrow(argv[3]).c_str());
        for (size_t i = 0; i < forms.size(); ++i) {
            std::printf("  [%zu]%s", i + 1, Narrow(forms[i]).c_str());
        }
        std::printf("\n");
        return forms.empty() ? 1 : 0;
    }
    if (cmd == "special" && argc > 3) {
        // 固定时间戳，便于验证格式（默认 2026-11-17 10:51:35 星期二）
        std::string what = Narrow(argv[3]);
        int y = 2026, mo = 11, d = 17, h = 10, mi = 51, s = 35, wd = 2;
        if (argc > 10) {
            y = std::atoi(Narrow(argv[4]).c_str());  mo = std::atoi(Narrow(argv[5]).c_str());
            d = std::atoi(Narrow(argv[6]).c_str());  h  = std::atoi(Narrow(argv[7]).c_str());
            mi = std::atoi(Narrow(argv[8]).c_str()); s  = std::atoi(Narrow(argv[9]).c_str());
            wd = std::atoi(Narrow(argv[10]).c_str());
        }
        int kind = (what == "time") ? 1 : (what == "week") ? 2 : 0;
        std::vector<std::string> outWords;
        wubi::DateTimeCandidates(kind, y, mo, d, h, mi, s, wd, outWords);
        std::printf("%s:", what.c_str());
        for (auto& w : outWords) std::printf("  %s", w.c_str());
        std::printf("\n");
        return outWords.empty() ? 1 : 0;
    }
    if (cmd == "codefilter") {
        // 自定义短语窗口的编码输入框：只收英文字母，最多 4 位
        struct C { const wchar_t* in; const wchar_t* want; };
        const C cases[] = {
            { L"1a1x",      L"ax"   },   // 数字被丢弃
            { L"abcdef",    L"abcd" },   // 超长截断
            { L"A1B2C3D4",  L"ABCD" },   // 大写保留、数字丢弃
            { L"--dz--",    L"dz"   },   // 标点丢弃
            { L"wqvb",      L"wqvb" },
        };
        int bad = 0;
        for (const auto& c : cases) {
            const std::wstring got = wubi::FilterCode(c.in, 4);
            const bool ok = (got == c.want);
            std::printf("  %s %-10ls -> %-6ls want %ls\n",
                        ok ? "PASS" : "FAIL", c.in, got.c_str(), c.want);
            if (!ok) ++bad;
        }
        // 单字符判定：字母放行，其它拒绝
        const bool allowA = wubi::FilterCode(L"a", 4) == L"a";
        const bool allowZ = wubi::FilterCode(L"Z", 4) == L"Z";
        const bool deny1  = wubi::FilterCode(L"1", 4).empty();
        const bool denyHan = wubi::FilterCode(L"你", 4).empty();
        std::printf("  %s 单字判定 a/Z 通过, 1/你 被拒\n",
                    (allowA && allowZ && deny1 && denyHan) ? "PASS" : "FAIL");
        if (!(allowA && allowZ && deny1 && denyHan)) ++bad;
        std::printf("codefilter: %s\n", bad == 0 ? "PASS" : "FAIL");
        return bad == 0 ? 0 : 1;
    }
    if (cmd == "engcase") {
        // 英文补全的大小写套用：已输入部分照抄实际大小写，未输入部分跟随整体风格
        struct C { const wchar_t* word; const wchar_t* typed; const wchar_t* want; };
        const C cases[] = {
            { L"about",    L"a",   L"about"    },   // 全小写进 -> 全小写出
            { L"about",    L"A",   L"About"    },   // Shift+A 进 -> 首字母大写
            { L"about",    L"Ab",  L"About"    },   // 之后按小写 b，仍保持首字母大写
            { L"about",    L"AB",  L"ABOUT"    },   // 一直按着 Shift（或 CapsLock）-> 全大写
            { L"about",    L"AbO", L"AbOut"    },   // 逐字照抄，混排也忠实
            { L"computer", L"COM", L"COMPUTER" },
            { L"computer", L"Com", L"Computer" },
            { L"hello",    L"HEL", L"HELLO"    },
            { L"don't",    L"D",   L"Don't"    },
        };
        int bad = 0;
        for (const auto& c : cases) {
            const std::wstring got = wubi::ApplyCase(c.word, c.typed);
            const bool ok = (got == c.want);
            std::printf("  %s %-9ls typed=%-4ls -> %-10ls want %ls\n",
                        ok ? "PASS" : "FAIL", c.word, c.typed, got.c_str(), c.want);
            if (!ok) ++bad;
        }
        std::printf("engcase: %s (%zu cases)\n", bad == 0 ? "PASS" : "FAIL", sizeof(cases) / sizeof(cases[0]));
        return bad == 0 ? 0 : 1;
    }
    if (cmd == "trimtest") {
        // 上屏字队列清洗：文档里还留着的字才保留（退格删掉的不算）
        struct T { std::vector<std::wstring> hist; std::wstring tail; size_t wantDropped; std::wstring wantKept; };
        const T cases[] = {
            { { L"工", L"人", L"了" }, L"工人",   1, L"工人"  },   // 末尾「了」被退格删掉
            { { L"工", L"人" },        L"工人",   0, L"工人"  },   // 没删
            { { L"工", L"人", L"了", L"啊" }, L"工啊", 2, L"工啊" }, // 中间删掉两个字
            { { L"工" },               L"工人",   0, L"工"    },   // 文档里更长：不动
            { { L"工", L"人", L"了" }, L"",       0, L"工人了" },   // 读不到光标：调用方会跳过，函数本身不动
            { { L"好" },               L"工人",   1, L""      },   // 完全对不上：history 变空，调用方放弃清洗
        };
        int bad = 0;
        for (const auto& c : cases) {
            std::vector<std::wstring> h = c.hist;
            size_t dropped = wubi::TrimCharsByText(h, c.tail);
            std::wstring kept;
            for (const auto& x : h) kept += x;
            const bool ok = (dropped == c.wantDropped) && (kept == c.wantKept);
            std::printf("  %s hist=%zu tail=%zu -> dropped=%zu kept=%zu %s\n",
                        ok ? "PASS" : "FAIL", c.hist.size(), c.tail.size(), dropped, h.size(),
                        ok ? "" : "MISMATCH");
            if (!ok) ++bad;
        }
        std::printf("trimtest: %s (%zu cases)\n", bad == 0 ? "PASS" : "FAIL", sizeof(cases) / sizeof(cases[0]));
        return bad == 0 ? 0 : 1;
    }
    if (cmd == "phrasecode") {
        // 自定义词组取码：按五笔标准规则 + 字的全码
        struct T { const wchar_t* word; const wchar_t* want; };
        const T cases[] = {
            { L"你好",           L"wqvb" },
            { L"我们",           L"trwu" },
            { L"计算机",         L"ytsm" },
            { L"中华人民共和国", L"kwwl" },
        };
        int bad = 0;
        for (const auto& c : cases) {
            const std::string u8 = Narrow(c.word);
            const std::string got = wubi::PhraseCode(u8, dict);
            std::printf("  %ls -> %s", c.word, got.empty() ? "(none)" : got.c_str());
            if (!got.empty()) {
                std::printf("   [");
                for (const auto& ch : wubi::Utf8Chars(u8)) std::printf(" %s=%s", ch.c_str(), dict.FullCodeOfChar(ch).c_str());
                std::printf(" ]");
            }
            const bool ok = (Narrow(c.want) == got);
            std::printf("  %s\n", ok ? "OK" : "MISMATCH");
            if (!ok) ++bad;
        }
        if (argc > 3) {   // 额外验证用户给的词
            const std::string u8 = Narrow(argv[3]);
            std::printf("  %ls -> %s\n", argv[3], wubi::PhraseCode(u8, dict).c_str());
        }
        std::printf("phrasecode: %s (%zu cases)\n", bad == 0 ? "PASS" : "FAIL", sizeof(cases) / sizeof(cases[0]));
        return bad == 0 ? 0 : 1;
    }
    if (cmd == "english" && argc > 3) {
        wubi::EnglishDict eng;
        const long long n = eng.Load(L"data\\eng_words.txt");
        const std::string prefix = Narrow(argv[3]);
        auto words = eng.Suggest(prefix, 9);
        std::printf("eng words=%lld prefix=%s ->", n, prefix.c_str());
        for (auto& w : words) std::printf(" %s", w.c_str());
        std::printf("\n");
        return words.empty() ? 1 : 0;
    }
    if (cmd == "appcfg" && argc > 4) {
        const std::wstring ini = std::wstring(argv[3]);
        const std::wstring exe = std::wstring(argv[4]);
        const auto s = wubi::LoadSettings(ini, exe);
        std::printf("exe=%ls -> TradChinese=%d CandVertical=%d CandCount=%d Level1Mode=%d\n",
                    exe.c_str(), s.TradChinese, s.CandVertical, s.CandCount, s.Level1Mode);
        return 0;
    }
    if (cmd == "layout") {
        // 候选窗几何自检（横排/竖排 + 命中测试）
        std::vector<std::pair<int,int>> sizes;
        for (int i = 0; i < 3; ++i) sizes.push_back({ 20, 16 });
        const int padX = 4, padY = 2;

        auto hz = wubi::CandLayout::Compute(sizes, false, padX, padY);
        auto vt = wubi::CandLayout::Compute(sizes, true,  padX, padY);
        std::printf("horizontal: %dx%d  items=%zu\n", hz.width, hz.height, hz.items.size());
        std::printf("vertical  : %dx%d  items=%zu\n", vt.width, vt.height, vt.items.size());

        // 横排: 每项 (20+8)=28 宽, 3 项 => 84+8=92 宽; 高 16+4(内)+4(外)=24
        bool okH = (hz.width == 92 && hz.height == 24);
        // 竖排: 每项 (16+4)=20 高, 3 项 => 60+4=64 高; 宽 20+8(内)+8(外)=36
        bool okV = (vt.width == 36 && vt.height == 64);
        std::printf("  size check: horizontal=%s vertical=%s\n", okH ? "PASS" : "FAIL", okV ? "PASS" : "FAIL");

        // 命中测试: 每个候选中心点应命中自身
        bool okHitH = true, okHitV = true;
        for (size_t i = 0; i < hz.items.size(); ++i) {
            const auto& b = hz.items[i];
            int hit = hz.HitTest(b.x + b.w / 2, b.y + b.h / 2);
            if (hit != (int)i) { okHitH = false; std::printf("  hit H[%zu] -> %d\n", i, hit); }
        }
        for (size_t i = 0; i < vt.items.size(); ++i) {
            const auto& b = vt.items[i];
            int hit = vt.HitTest(b.x + b.w / 2, b.y + b.h / 2);
            if (hit != (int)i) { okHitV = false; std::printf("  hit V[%zu] -> %d\n", i, hit); }
        }
        // 边界: 负坐标/越界应返回 -1
        bool okMiss = (hz.HitTest(-1, -1) == -1) && (hz.HitTest(hz.width + 5, 0) == -1);
        std::printf("  hit check: H=%s V=%s miss=%s\n",
                    okHitH ? "PASS" : "FAIL", okHitV ? "PASS" : "FAIL", okMiss ? "PASS" : "FAIL");

        bool all = okH && okV && okHitH && okHitV && okMiss;
        std::printf("RESULT: %s\n", all ? "PASS" : "FAIL");
        return all ? 0 : 1;
    }
    if (cmd == "sim" && argc > 3) {
        // 按键状态机模拟：a-y 组码 / 1-9 选择 / '_' 空格上屏 / '<' 退格 / '!' 取消
        std::string script = Narrow(argv[3]);
        wubi::Composer comp(&dict);
    comp.SetSpecialProvider([](const std::string& code, std::vector<wubi::Candidate>& out) -> bool {
        int kind = (code == "date") ? 0 : (code == "time") ? 1 : (code == "week") ? 2 : -1;
        if (kind < 0) return false;
        std::vector<std::string> words;
        wubi::DateTimeCandidates(kind, 2026, 11, 17, 10, 51, 35, 2, words);   // 固定时间
        for (auto& w : words) out.push_back(wubi::Candidate{ w, code, 0, false });
        return !words.empty();
    });
        std::printf("script: %s\n", script.c_str());
        int committed = 0;
        bool promoteNext = false;
        for (char k : script) {
            wubi::Composer::Outcome o;
            const char* what = "";
            if (k >= 'a' && k <= 'y')      { o = comp.TypeCode(k);      what = "type"; }
            else if (k >= '1' && k <= '9') {
                if (promoteNext) { promoteNext = false; o = comp.Promote(k - '0'); what = "promote"; }
                else             { o = comp.SelectDigit(k - '0'); what = "digit"; }
            }
            else if (k == '~')             { promoteNext = true;        continue; }
            else if (k == '_')             { o = comp.SelectBest();     what = "space"; }
            else if (k == '<')             { o = comp.Backspace();      what = "back"; }
            else if (k == ';')             { o = comp.SelectDigit(2);    what = "semi2"; }
            else if (k == '\'')            { o = comp.SelectDigit(3);    what = "quote3"; }
            else if (k == '!')             { o = comp.Escape();         what = "esc"; }
            else if (k == '[')             { o = comp.PrevPage();       what = "pgup"; }
            else if (k == ']')             { o = comp.NextPage();       what = "pgdn"; }
            else continue;

            std::string topWord;
            if (!comp.Candidates().empty()) topWord = comp.Candidates()[0].word;
            const char* actName =
                o.act == wubi::Composer::Act::Pass ? "pass" :
                o.act == wubi::Composer::Act::Updated ? "updated" :
                o.act == wubi::Composer::Act::Committed ? "committed" :
                o.act == wubi::Composer::Act::CommitThenContinue ? "commit+cont" : "cancelled";
            std::printf("  '%c' %-6s -> %-9s code='%s' cands=%zu page=%zu/%zu", k, what, actName,
                        comp.Code().c_str(), comp.Candidates().size(),
                        comp.PageIndex() + 1, comp.PageCount() ? comp.PageCount() : 1);
            if (!topWord.empty()) std::printf(" top=%s", topWord.c_str());
            if (o.act == wubi::Composer::Act::Committed ||
                o.act == wubi::Composer::Act::CommitThenContinue) {
                std::printf(" COMMIT='%s'", Narrow(o.commit).c_str());
                committed++;
            }
            std::printf("\n");
        }
        std::printf("commits=%d\n", committed);
        return 0;
    }
    if (cmd == "migrate" && argc > 4) {
        bool found = false;
        wubi::Settings s = wubi::MigrateFromQQ(argv[3], &found);
        std::printf("QQ config found=%d (%ls)\n", (int)found, argv[3]);
        std::printf("  InputMode=%d CandVertical=%d EnablePunction=%d UseGBK=%d TradChinese=%d\n",
                    s.InputMode, s.CandVertical, s.EnablePunction, s.UseGBK, s.TradChinese);
        if (!wubi::SaveSettings(argv[4], s)) { std::printf("  write FAILED\n"); return 1; }
        wubi::Settings back = wubi::LoadSettings(argv[4]);
        bool ok = back.InputMode == s.InputMode && back.CandVertical == s.CandVertical &&
                  back.EnablePunction == s.EnablePunction && back.TradChinese == s.TradChinese;
        std::printf("  written -> %ls ; reload match=%s\n", argv[4], ok ? "PASS" : "FAIL");
        return ok ? 0 : 1;
    }

    if (cmd == "show" && argc > 3) {
        wubi::Settings s = wubi::LoadSettings(argv[3]);
        std::printf("InputMode=%d CandVertical=%d CandCount=%d EnablePunction=%d TradChinese=%d SortMode=%d Level1Mode=%d CandFont=%d CandBgColor=#%06X CandInset=%d DictFile=%ls\n",
                    s.InputMode, s.CandVertical, s.CandCount, s.EnablePunction, s.TradChinese,
                    s.SortMode, s.Level1Mode, s.CandFontPt, (unsigned)(s.CandBgColor & 0xFFFFFF), s.CandInset, s.DictFile.c_str());
    }
    if (cmd == "userdemo") {
        // 词频自学习 + 持久化 自检
        auto show = [&](const char* tag) {
            auto c = dict.Lookup("wqvb", 5);
            std::printf("  [%s]", tag);
            for (const auto& e : c) std::printf(" %s(%lld%s)", e.word.c_str(), (long long)e.weight, e.user ? ",u" : "");
            std::printf("\n");
        };
        show("before");
        dict.BumpUserWord("wqvb", "您好", 80000000);
        show("after bump 您好 +80M");

        const wchar_t* path = L"build\\user_words.tsv";
        if (!dict.SaveUserWords(path)) { std::printf("  save FAILED\n"); return 1; }
        std::printf("  saved %zu user word(s) to build\\user_words.tsv\n", dict.UserWordCount());

        // 重新载入，确认持久化后仍然生效
        wubi::Dictionary d2;
        { std::wstring wp(argv[1]); if (wp.size() > 4 && wp.compare(wp.size() - 4, 4, L".bin") == 0) d2.LoadBinary(wp); else d2.Load(wp); }
        long long n2 = d2.LoadUserWords(path);
        auto c2 = d2.Lookup("wqvb", 5);
        std::printf("  [reloaded %lld user words]", n2);
        for (const auto& e : c2) std::printf(" %s(%lld%s)", e.word.c_str(), (long long)e.weight, e.user ? ",u" : "");
        std::printf("\n");
        bool orderChanged = false;
        { auto c3 = dict.Lookup("wqvb", 5); orderChanged = !c3.empty() && c3[0].word == "您好" && c3[0].user; }
        std::printf("  bump 后同实例首位为用户词: %s\n", orderChanged ? "YES" : "NO");
        bool ok = !c2.empty() && c2[0].word == "您好" && c2[0].user && orderChanged;
        std::printf("  RESULT: %s\n", ok ? "PASS" : "FAIL");
        return ok ? 0 : 1;
    }
    if (cmd == "trad" && argc > 3) {
        wubi::Transliteration tr;
        bool ok = tr.Load(L"data\\simp2trad.dat");
        if (!ok) ok = tr.Load(L"..\\data\\simp2trad.dat");
        std::printf("transliteration table loaded=%d\n", (int)ok);
        std::string in = Narrow(argv[3]);
        std::wstring w = Widen(in);
        std::wstring outw = tr.ToTraditional(w);
        std::printf("  in : %s\n  out: %s\n", in.c_str(), Narrow(outw).c_str());
        return ok ? 0 : 1;
    }

    if (cmd == "punc") {
        wubi::Punctuation pm;
        bool loadedFile = pm.Load(L"data\\punc.ini");
        if (!loadedFile) loadedFile = pm.Load(L"..\\data\\punc.ini");
        std::printf("punc.ini loaded=%d (fallback=builtin)\n", (int)loadedFile);
        const char* keys = ",.;:?!()[]<>-\"'^_";
        for (const char* p = keys; *p; ++p) {
            std::printf("  '%c' ->", *p);
            for (int m = 0; m < 4; ++m) {
                pm.ResetToggle();
                std::wstring o;
                bool hit = pm.Map(*p, (wubi::PuncMode)m, o);
                std::printf("  [%d]%s", m, hit ? Narrow(o).c_str() : "-");
            }
            std::printf("\n");
        }
        // 引号成对行为
        std::wstring q1, q2, q3;
        pm.ResetToggle();
        pm.Map('"', wubi::PuncMode::ChineseHalf, q1);
        pm.Map('"', wubi::PuncMode::ChineseHalf, q2);
        pm.Map('"', wubi::PuncMode::ChineseHalf, q3);
        std::printf("  '\"' toggles: %s %s %s\n", Narrow(q1).c_str(), Narrow(q2).c_str(), Narrow(q3).c_str());
        return 0;
    }

    std::printf("enter code (a-y, empty line quits):\n");
    std::string line;
    while (true) {
        std::printf("> ");
        if (!std::getline(std::cin, line)) break;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) break;
        bool bad = false;
        for (char c : line) { if (!wubi::IsCodeChar(c)) { bad = true; break; } }
        if (bad) { std::printf("  invalid code\n"); continue; }
        ShowCandidates(dict.Lookup(line, 20), 10);
    }
    return 0;
}
