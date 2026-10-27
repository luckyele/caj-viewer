// 笔刷式文本选区的纯几何命中测试（无 GTK 依赖，可单独编译进测试）。
// 坐标一律是「缩放后页面坐标」（同 run_geom 的 xs0/xs1 空间）。
#ifndef CAJ_SEL_BRUSH_HPP
#define CAJ_SEL_BRUSH_HPP

#include <cstddef>
#include <utility>
#include <vector>

namespace brush {

struct Point {
    double x = 0, y = 0;
};

// 点 (px,py) 到线段 (ax,ay)-(bx,by) 的最短距离（零长线段按点到点算）
double seg_dist(double px, double py, double ax, double ay, double bx, double by);

// 笔刷命中：字符中心 (mids[i], y) 到笔画折线 path（带半径 r）的最短距离 ≤ r
// 即被选中。返回命中的连续字符下标区间 [i0, i1)，升序且互不重叠；
// 一段文字被笔画分开扫过时会得到多个区间。
std::vector<std::pair<size_t, size_t>> brush_ranges(const std::vector<double>& mids,
                                                    double y,
                                                    const std::vector<Point>& path,
                                                    double r);

}  // namespace brush

#endif
