// sel_brush 单元测试：测试缝 = brush::seg_dist / brush::brush_ranges 两个公开函数。
// 期望值全部手算自独立几何事实，不复用实现公式（防同义反复）。
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "sel_brush.hpp"

static int g_total = 0, g_failed = 0;

static void check_near(double got, double expect, double eps, const char* expr,
                       int line) {
    g_total++;
    if (!(std::fabs(got - expect) <= eps)) {
        g_failed++;
        std::printf("FAIL L%d: %s = %.6f, 期望 %.6f\n", line, expr, got, expect);
    }
}
#define CHECK_NEAR(expr, expect, eps) check_near((expr), (expect), (eps), #expr, __LINE__)

[[maybe_unused]] static void check(bool ok, const char* expr, int line) {
    g_total++;
    if (!ok) {
        g_failed++;
        std::printf("FAIL L%d: %s\n", line, expr);
    }
}
#define CHECK(cond) check((cond), #cond, __LINE__)

// 便于失败定位：把区间列表打印成 "[a,b) [c,d) ..."
[[maybe_unused]] static std::string ranges_str(const std::vector<std::pair<size_t, size_t>>& v) {
    std::string s;
    for (const auto& r : v) {
        s += "[" + std::to_string(r.first) + "," + std::to_string(r.second) + ") ";
    }
    return s;
}

[[maybe_unused]] static void check_ranges(const std::vector<std::pair<size_t, size_t>>& got,
                         const std::vector<std::pair<size_t, size_t>>& expect,
                         int line) {
    g_total++;
    if (got != expect) {
        g_failed++;
        std::printf("FAIL L%d: ranges = %s, 期望 %s\n", line, ranges_str(got).c_str(),
                    ranges_str(expect).c_str());
    }
}
#define CHECK_RANGES(got, ...) check_ranges((got), __VA_ARGS__, __LINE__)

int main() {
    using brush::Point;

    // ---- seg_dist ----
    // 切片 1：垂足落在线段内部 → 距离 = 点到直线的垂距
    CHECK_NEAR(brush::seg_dist(0, 3, -4, 0, 4, 0), 3.0, 1e-9);
    // 切片 2：垂足落在线段外 → 距离取到更近端点的距离
    //   点 (6,8) 对线段 (-4,0)-(4,0)：垂足 x=6 超出右端点，距离 = |(6,8)-(4,0)| = √68
    CHECK_NEAR(brush::seg_dist(6, 8, -4, 0, 4, 0), std::sqrt(68.0), 1e-9);
    //   点 (-9,0) 在左端点延长线上：距离 = 5
    CHECK_NEAR(brush::seg_dist(-9, 0, -4, 0, 4, 0), 5.0, 1e-9);
    //   零长线段（起点=终点）按点到点算
    CHECK_NEAR(brush::seg_dist(3, 4, 0, 0, 0, 0), 5.0, 1e-9);

    // ---- brush_ranges ----
    // 切片 3：横贯一行的笔画命中全部字符（中心到路径距离为 0）；空路径/不接触返回空
    {
        std::vector<double> mids = {10, 20, 30, 40, 50};
        std::vector<Point> path = {{5, 0}, {55, 0}};
        CHECK_RANGES(brush::brush_ranges(mids, 0, path, 6), {{0, 5}});
        CHECK(brush::brush_ranges(mids, 0, {}, 6).empty());
        CHECK(brush::brush_ranges({}, 0, path, 6).empty());
        // 笔画在行下方 10px 处平行扫过，半径 6 够不着 → 无命中
        std::vector<Point> far_path = {{5, 10}, {55, 10}};
        CHECK(brush::brush_ranges(mids, 0, far_path, 6).empty());
    }
    // 切片 4：笔画只覆盖行中段 → 两端字符截掉，只命中 [1,4)
    {
        std::vector<double> mids = {10, 20, 30, 40, 50};
        std::vector<Point> path = {{22, 0}, {38, 0}};
        // 距离手算：char10→12 miss；char20→2、char30→0、char40→2 hit；char50→12 miss
        CHECK_RANGES(brush::brush_ranges(mids, 0, path, 5), {{1, 4}});
        // 距离恰等于半径（5）也算命中（≤）
        CHECK_RANGES(brush::brush_ranges({0}, -5, {{-10, 0}, {10, 0}}, 5), {{0, 1}});
    }
    // 切片 5：拐角笔画（中途抬到行外再落下）命中两簇 → 两个不连续区间
    {
        std::vector<double> mids = {0, 10, 20, 30, 40, 50, 60};
        std::vector<Point> path = {{0, 0}, {5, 0}, {5, -100},
                                   {50, -100}, {50, -2}, {60, -2}};
        // 命中手算：char0 压在首段上；char50 距 (50,-2) 为 2；char60 压在末段上；
        //           其余到任何段的最近距离都 > 4
        CHECK_RANGES(brush::brush_ranges(mids, 0, path, 4), {{0, 1}, {5, 7}});
    }
    // 切片 6：竖向笔画穿过一行 → 只选中横穿点附近的字符
    {
        std::vector<double> mids = {0, 10, 20, 30, 40};
        CHECK_RANGES(brush::brush_ranges(mids, 0, {{25, -30}, {25, 30}}, 6),
                     {{2, 4}});
    }
    // 切片 7：单点路径（轻点）→ 命中该点半径内的字符
    {
        std::vector<double> mids = {0, 10, 20, 30, 40};
        CHECK_RANGES(brush::brush_ranges(mids, 0, {{25, 0}}, 6), {{2, 4}});
    }

    std::printf("%d/%d 通过\n", g_total - g_failed, g_total);
    return g_failed ? 1 : 0;
}
