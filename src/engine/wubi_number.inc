// ---------------------------------------------------------------- 数字转中文
static const wchar_t* kLowerDigitsTbl = L"〇一二三四五六七八九";
static const wchar_t* kUpperDigitsTbl = L"零壹贰叁肆伍陆柒捌玖";
static const wchar_t* kLowerUnitsTbl  = L"十百千";
static const wchar_t* kUpperUnitsTbl  = L"拾佰仟";
static const wchar_t* kGroupUnitsTbl[] = { L"", L"万", L"亿", L"万亿" };

// 读一组（最多 4 位，值是 0..9999 的十进制串）
static std::wstring ReadFourDigits(const std::string& g, bool upper) {
    const wchar_t* digits = upper ? kUpperDigitsTbl : kLowerDigitsTbl;
    const wchar_t* units  = upper ? kUpperUnitsTbl  : kLowerUnitsTbl;
    std::wstring out;
    bool zeroPending = false;
    const int n = (int)g.size();
    for (int i = 0; i < n; ++i) {
        const int d = g[(size_t)i] - '0';
        const int pos = n - 1 - i;
        if (d == 0) {
            if (!out.empty()) zeroPending = true;
            continue;
        }
        if (zeroPending) { out += L'零'; zeroPending = false; }
        out += digits[d];
        if (pos > 0) out += units[pos - 1];
    }
    return out;
}

static int GroupValue(const std::string& g) {
    int v = 0;
    for (char c : g) v = v * 10 + (c - '0');
    return v;
}

void NumberForms(const std::string& digitsIn, std::vector<std::wstring>& forms) {
    forms.clear();
    std::string digits;
    for (char c : digitsIn) if (c >= '0' && c <= '9') digits.push_back(c);
    if (digits.empty()) return;

    // [1] 逐位直读（0 用 〇）
    std::wstring perDigit;
    for (char c : digits) perDigit += kLowerDigitsTbl[c - '0'];
    forms.push_back(perDigit);

    const size_t start = digits.find_first_not_of('0');
    const std::string d = (start == std::string::npos) ? std::string("0") : digits.substr(start);
    if (d == "0") {
        forms.push_back(std::wstring(1, L'零'));
        forms.push_back(std::wstring(1, L'零'));
        return;
    }

    auto buildGrouped = [&](bool upper) -> std::wstring {
        std::vector<std::string> groups;            // 低位组在前
        for (int i = (int)d.size(); i > 0; i -= 4) {
            const int b = (i - 4 > 0) ? i - 4 : 0;
            groups.push_back(d.substr((size_t)b, (size_t)(i - b)));
        }
        std::wstring out;
        for (int gi = (int)groups.size() - 1; gi >= 0; --gi) {
            const std::string& g = groups[(size_t)gi];
            const int v = GroupValue(g);
            if (v == 0) continue;                   // 整组为零，跳过
            // 低一组不足千位（或中间有整组零）时要补「零」：一亿零一万零一
            if (!out.empty() && v < 1000 && out.back() != L'零') out += L'零';
            std::wstring part = ReadFourDigits(g, upper);
            // 最高一组是 10-19 时，小写习惯读作「十X」「十万」「十二万」
            if (out.empty() && !upper && v >= 10 && v <= 19 &&
                !part.empty() && part[0] == kLowerDigitsTbl[1]) {
                part.erase(part.begin());
            }
            out += part;
            out += kGroupUnitsTbl[gi < 4 ? gi : 3];
        }
        return out;
    };

    forms.push_back(buildGrouped(false));
    forms.push_back(buildGrouped(true));
}

