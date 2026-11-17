// ---------------------------------------------------------------- 日期/时间/星期
// kind: 0=date 1=time 2=week；weekday: 0=周日
static const wchar_t* kWeekNames[] = { L"星期日", L"星期一", L"星期二", L"星期三", L"星期四", L"星期五", L"星期六" };

static std::wstring UpperChineseNumber(int v);   // 前置声明（月/日的中文读法）

void DateTimeCandidates(int kind, int year, int month, int day,
                        int hour, int minute, int second, int weekday,
                        std::vector<std::string>& out) {
    out.clear();
    auto u8 = [](const std::wstring& w) -> std::string {
        if (w.empty()) return std::string();
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string s((size_t)(n > 0 ? n : 0), '\0');
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
        return s;
    };
    wchar_t buf[128];

    // 常用片段
    swprintf_s(buf, L"%04d.%02d.%02d", year, month, day);
    const std::wstring dDot = buf;
    swprintf_s(buf, L"%04d年%d月%d日", year, month, day);
    const std::wstring dCn = buf;
    swprintf_s(buf, L"%04d-%02d-%02d", year, month, day);
    const std::wstring dDash = buf;

    // 二〇二六年十一月十七日
    std::wstring dLong;
    {
        const wchar_t* digits = L"〇一二三四五六七八九";
        int v = year;
        wchar_t ybuf[8] = { 0 };
        swprintf_s(ybuf, L"%04d", v);
        for (int i = 0; i < 4; ++i) dLong += digits[ybuf[i] - '0'];
        dLong += L"年";
        dLong += UpperChineseNumber(month);
        dLong += L"月";
        dLong += UpperChineseNumber(day);
        dLong += L"日";
    }

    swprintf_s(buf, L"%02d:%02d:%02d", hour, minute, second);
    const std::wstring tHms = buf;

    const std::wstring wname = kWeekNames[(weekday >= 0 && weekday <= 6) ? weekday : 0];

    switch (kind) {
    case 0:  // date
        out.push_back(u8(dDot));
        out.push_back(u8(dCn));
        out.push_back(u8(dDash));
        out.push_back(u8(dLong));
        break;

    case 1:  // time
        out.push_back(u8(tHms));
        out.push_back(u8(dCn + L" " + tHms));
        {
            // 2026年11月17日, AM 10:51:40
            const wchar_t* ap = (hour < 12) ? L"AM" : L"PM";
            const int h12 = (hour % 12 == 0) ? 12 : (hour % 12);
            swprintf_s(buf, L"%ls, %ls %02d:%02d:%02d", dCn.c_str(), ap, h12, minute, second);
            out.push_back(u8(buf));
        }
        out.push_back(u8(dLong + L" " + tHms));
        out.push_back(u8(dDash + L" " + tHms));
        break;

    case 2:  // week
        out.push_back(u8(wname));
        out.push_back(u8(dLong + L" " + wname));
        out.push_back(u8(dCn + L" " + wname));
        break;

    default:
        break;
    }
}

// 1..31 的中文读法（十一、二十、二十三…）
static std::wstring UpperChineseNumber(int v) {
    const wchar_t* digits = L"〇一二三四五六七八九";
    if (v <= 0) return L"〇";
    if (v < 10) return std::wstring(1, digits[v]);
    if (v == 10) return L"十";
    if (v < 20) return std::wstring(L"十") + digits[v - 10];
    const int tens = v / 10, ones = v % 10;
    std::wstring s(1, digits[tens]);
    s += L"十";
    if (ones) s += digits[ones];
    return s;
}

