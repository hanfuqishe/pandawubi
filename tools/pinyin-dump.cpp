// pinyin-dump.cpp —— 用本机 Weasel 的 librime 把「拼音 → 汉字」dump 成一张表
//
// 用途：给熊猫五笔的 Z 反查（拼音反查五笔编码）生成 data\pinyin.tsv。
//   pinyin.tsv 格式：  拼音<TAB>字<TAB>该字的五笔全码
//
// 用法： build\pinyin-dump.exe <输出文件> [每音节候选数]
//   例： build\pinyin-dump.exe data\pinyin.tsv 60
//
// 说明：librime 的 C 接口是"版本化结构体"（RimeTraits/RimeContext 都以 data_size 开头），
// 这里按 rime_api.h 0.17 的字段顺序定义，字段不匹配的话第一个候选就不会是"你"——启动时会自检。
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

typedef int RimeSessionId;
typedef int Bool;

struct RimeTraits {
    int data_size;
    const char* shared_data_dir;
    const char* user_data_dir;
    const char* log_dir;
    const char* prebuilt_data_dir;
    const char* staging_dir;
    const char* distribution_name;
    const char* distribution_code_name;
    const char* distribution_version;
    const char* app_name;
    const char* modules;
    int min_log_level;
    const char* log_file;
};

struct RimeComposition {
    int data_size;
    int cursor_pos;
    int sel_start;
    int sel_end;
    char* preedit;
};

struct RimeCandidate {
    char* text;
    char* comment;
};

struct RimeMenu {
    int data_size;
    int page_size;
    int page_no;
    Bool is_last_page;
    int highlighted_candidate_index;
    int num_candidates;
    RimeCandidate* candidates;
    char* select_keys;
};

struct RimeContext {
    int data_size;
    RimeComposition composition;
    RimeMenu menu;
    char commit_text_preview[512];
};

typedef void (*FnSetup)(RimeTraits*);
typedef void (*FnInitialize)(RimeTraits*);
typedef void (*FnFinalize)();
typedef RimeSessionId (*FnCreateSession)();
typedef Bool (*FnSelectSchema)(RimeSessionId, const char*);
typedef Bool (*FnSimulateKeys)(RimeSessionId, const char*);
typedef Bool (*FnGetContext)(RimeSessionId, RimeContext*);
typedef void (*FnFreeContext)(RimeContext*);

static FnSetup        pSetup = nullptr;
static FnInitialize   pInit = nullptr;
static FnFinalize     pFinalize = nullptr;
static FnCreateSession pCreate = nullptr;
static FnSelectSchema pSelect = nullptr;
static FnSimulateKeys pKeys = nullptr;
static FnGetContext   pGetCtx = nullptr;
static FnFreeContext  pFreeCtx = nullptr;

// 普通话全部无声调音节
static const char* kSyllables[] = {
    "a","ai","an","ang","ao","ba","bai","ban","bang","bao","bei","ben","beng","bi","bian","biao","bie","bin","bing","bo","bu",
    "ca","cai","can","cang","cao","ce","cen","ceng","cha","chai","chan","chang","chao","che","chen","cheng","chi","chong","chou","chu","chua","chuai","chuan","chuang","chui","chun","chuo","ci","cong","cou","cu","cuan","cui","cun","cuo",
    "da","dai","dan","dang","dao","de","dei","den","deng","di","dia","dian","diao","die","ding","diu","dong","dou","du","duan","dui","dun","duo",
    "e","ei","en","eng","er","fa","fan","fang","fei","fen","feng","fo","fou","fu",
    "ga","gai","gan","gang","gao","ge","gei","gen","geng","gong","gou","gu","gua","guai","guan","guang","gui","gun","guo",
    "ha","hai","han","hang","hao","he","hei","hen","heng","hong","hou","hu","hua","huai","huan","huang","hui","hun","huo",
    "ji","jia","jian","jiang","jiao","jie","jin","jing","jiong","jiu","ju","juan","jue","jun",
    "ka","kai","kan","kang","kao","ke","ken","keng","kong","kou","ku","kua","kuai","kuan","kuang","kui","kun","kuo",
    "la","lai","lan","lang","lao","le","lei","leng","li","lia","lian","liang","liao","lie","lin","ling","liu","lo","long","lou","lu","luan","lue","lun","luo","lv","lve",
    "ma","mai","man","mang","mao","me","mei","men","meng","mi","mian","miao","mie","min","ming","miu","mo","mou","mu",
    "na","nai","nan","nang","nao","ne","nei","nen","neng","ni","nian","niang","niao","nie","nin","ning","niu","nong","nou","nu","nuan","nue","nuo","nv","nve",
    "o","ou","pa","pai","pan","pang","pao","pei","pen","peng","pi","pian","piao","pie","pin","ping","po","pou","pu",
    "qi","qia","qian","qiang","qiao","qie","qin","qing","qiong","qiu","qu","quan","que","qun",
    "ran","rang","rao","re","ren","reng","ri","rong","rou","ru","rua","ruan","rui","run","ruo",
    "sa","sai","san","sang","sao","se","sen","seng","sha","shai","shan","shang","shao","she","shei","shen","sheng","shi","shou","shu","shua","shuai","shuan","shuang","shui","shun","shuo","si","song","sou","su","suan","sui","sun","suo",
    "ta","tai","tan","tang","tao","te","teng","ti","tian","tiao","tie","ting","tong","tou","tu","tuan","tui","tun","tuo",
    "wa","wai","wan","wang","wei","wen","weng","wo","wu",
    "xi","xia","xian","xiang","xiao","xie","xin","xing","xiong","xiu","xu","xuan","xue","xun",
    "ya","yan","yang","yao","ye","yi","yin","ying","yo","yong","you","yu","yuan","yue","yun",
    "za","zai","zan","zang","zao","ze","zei","zen","zeng","zha","zhai","zhan","zhang","zhao","zhe","zhei","zhen","zheng","zhi","zhong","zhou","zhu","zhua","zhuai","zhuan","zhuang","zhui","zhun","zhuo","zi","zong","zou","zu","zuan","zui","zun","zuo",
};

static std::string Utf8ToAnsiFree(const std::string& s) { return s; }

int main(int argc, char** argv) {
    if (argc < 2) { printf("用法: pinyin-dump.exe <输出.tsv> [每音节候选数]\n"); return 2; }
    const char* outPath = argv[1];
    const int perSyllable = (argc >= 3) ? atoi(argv[2]) : 60;
    if (perSyllable <= 0) return 2;

    HMODULE rime = LoadLibraryW(L"C:\\Program Files\\Rime\\weasel-0.17.4\\rime.dll");
    if (!rime) { printf("加载 rime.dll 失败\n"); return 3; }
    pSetup = (FnSetup)GetProcAddress(rime, "RimeSetup");
    pInit = (FnInitialize)GetProcAddress(rime, "RimeInitialize");
    pFinalize = (FnFinalize)GetProcAddress(rime, "RimeFinalize");
    pCreate = (FnCreateSession)GetProcAddress(rime, "RimeCreateSession");
    pSelect = (FnSelectSchema)GetProcAddress(rime, "RimeSelectSchema");
    pKeys = (FnSimulateKeys)GetProcAddress(rime, "RimeSimulateKeySequence");
    pGetCtx = (FnGetContext)GetProcAddress(rime, "RimeGetContext");
    pFreeCtx = (FnFreeContext)GetProcAddress(rime, "RimeFreeContext");
    if (!pSetup || !pInit || !pCreate || !pKeys || !pGetCtx || !pFreeCtx) {
        printf("缺少 librime 导出函数\n");
        return 3;
    }

    wchar_t appdata[MAX_PATH] = { 0 };
    GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    char userDir[MAX_PATH * 2] = { 0 };
    WideCharToMultiByte(CP_ACP, 0, appdata, -1, userDir, sizeof(userDir) - 1, nullptr, nullptr);
    std::string user = std::string(userDir) + "\\Rime";

    RimeTraits traits = {};
    traits.data_size = sizeof(traits);
    traits.shared_data_dir = "C:\\Program Files\\Rime\\weasel-0.17.4\\data";
    traits.user_data_dir = user.c_str();
    traits.distribution_name = "Weasel";
    traits.distribution_code_name = "weasel";
    traits.distribution_version = "0.17.4";
    traits.app_name = "rime.pinyin-dump";
    traits.min_log_level = 3;
    printf("[1] setup...\n"); fflush(stdout);
    pSetup(&traits);
    printf("[2] initialize...\n"); fflush(stdout);
    pInit(&traits);
    printf("[3] initialized\n"); fflush(stdout);

    printf("[4] create session...\n"); fflush(stdout);
    RimeSessionId sid = pCreate();
    printf("[5] session=%d\n", sid); fflush(stdout);
    if (!sid) { printf("创建 rime 会话失败\n"); return 4; }
    if (pSelect) pSelect(sid, "luna_pinyin");

    auto query = [&](const char* keys, std::vector<std::string>& out) {
        out.clear();
        if (!pKeys(sid, keys)) return;
        RimeContext ctx = {};
        ctx.data_size = sizeof(ctx);
        if (!pGetCtx(sid, &ctx)) { printf("[ctx] failed\n"); fflush(stdout); return; }
        printf("[ctx] n=%d first=%s\n", ctx.menu.num_candidates,
               ctx.menu.num_candidates > 0 && ctx.menu.candidates && ctx.menu.candidates[0].text
                   ? ctx.menu.candidates[0].text : "(null)"); fflush(stdout);
        const int n = ctx.menu.num_candidates;
        for (int i = 0; i < n && (int)out.size() < perSyllable; ++i) {
            const char* t = ctx.menu.candidates[i].text;
            if (!t) continue;
            std::string s = t;
            // 只要单个汉字（UTF-8 下 3 字节一组）
            int chars = 0;
            for (size_t k = 0; k < s.size();) {
                unsigned char c = (unsigned char)s[k];
                int len = (c < 0x80) ? 1 : ((c >> 5) == 6 ? 2 : ((c >> 4) == 14 ? 3 : 4));
                k += (size_t)len;
                ++chars;
            }
            if (chars == 1 && s.size() >= 3) out.push_back(s);
        }
        // 先不 free，避免结构体不匹配时在释放阶段崩
        // 清掉输入里的拼音，避免影响下一次
        pKeys(sid, "{Escape}");
    };

    // 自检：ni 的第一个候选应该是 你
    {
        std::vector<std::string> probe;
        query("ni", probe);
        if (probe.empty()) { printf("自检失败：ni 没有候选\n"); pFinalize(); return 5; }
        printf("自检: ni -> %s (共 %zu 个)\n", probe[0].c_str(), probe.size());
        if (probe[0] != "你") { printf("自检失败：ni 的首选不是 你 —— RimeTraits/RimeContext 结构对不上\n"); pFinalize(); return 5; }
    }

    FILE* fp = nullptr;
    fopen_s(&fp, outPath, "wb");
    if (!fp) { printf("写不了 %s\n", outPath); pFinalize(); return 6; }
    fprintf(fp, "pinyin\tchar\n");
    int total = 0, syllables = 0;
    for (const char* syl : kSyllables) {
        std::vector<std::string> chars;
        query(syl, chars);
        if (chars.empty()) continue;
        ++syllables;
        for (const auto& c : chars) {
            fprintf(fp, "%s\t%s\n", syl, c.c_str());
            ++total;
        }
        if ((syllables % 50) == 0) printf("  ... %d 个音节, %d 行\n", syllables, total);
    }
    fclose(fp);
    printf("完成：%d 个音节，%d 行 -> %s\n", syllables, total, outPath);
    pFinalize();
    return 0;
}
