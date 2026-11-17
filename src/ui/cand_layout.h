// cand_layout.h —— 候选窗几何计算（纯逻辑，可独立单测）
// 横排/竖排共用一套算法：每个候选占 (w + 2*padX) × (h + 2*padY)，
// 外框再各加一圈 padding。
#pragma once

#include <utility>
#include <vector>

namespace wubi {

struct ItemBox {
    int x = 0, y = 0, w = 0, h = 0;          // 文字矩形（用于绘制）
    int cx = 0, cy = 0, cw = 0, ch = 0;      // 整格矩形（用于点击命中，更好点中）
};

struct CandLayout {
    int width = 0;
    int height = 0;
    std::vector<ItemBox> items;   // 各候选相对窗口左上角的矩形

    // sizes: 每个候选的 (文字宽, 文字高)，不含内边距
    static CandLayout Compute(const std::vector<std::pair<int, int>>& sizes,
                              bool vertical, int padX, int padY) {
        CandLayout L;
        if (sizes.empty()) return L;
        int cur = 0;
        int cross = 0;
        for (const auto& s : sizes) {
            const int w = s.first + padX * 2;
            const int h = s.second + padY * 2;
            ItemBox b;
            if (vertical) {
                b.x = padX;
                b.y = padY + cur;
                cur += h;
                if (w > cross) cross = w;
            } else {
                b.x = padX + cur;
                b.y = padY;
                cur += w;
                if (h > cross) cross = h;
            }
            b.w = s.first;
            b.h = s.second;
            b.cx = vertical ? 0 : (padX + cur - w);
            b.cy = vertical ? (padY + cur - h) : 0;
            b.cw = w;
            b.ch = h;
            L.items.push_back(b);
        }
        if (vertical) {
            L.width  = cross + padX * 2;
            L.height = cur + padY * 2;
        } else {
            L.width  = cur + padX * 2;
            L.height = cross + padY * 2;
        }
        return L;
    }

    // 命中测试：返回候选下标，未命中返回 -1
    int HitTest(int px, int py) const {
        for (size_t i = 0; i < items.size(); ++i) {
            const ItemBox& b = items[i];
            if (px >= b.cx && px < b.cx + b.cw && py >= b.cy && py < b.cy + b.ch) {
                return (int)i;
            }
        }
        return -1;
    }
};

} // namespace wubi
