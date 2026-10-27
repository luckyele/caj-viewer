#include "sel_brush.hpp"

#include <algorithm>
#include <cmath>

namespace brush {

double seg_dist(double px, double py, double ax, double ay, double bx, double by) {
    double dx = bx - ax, dy = by - ay;
    double len2 = dx * dx + dy * dy;
    if (len2 <= 0) return std::hypot(px - ax, py - ay);   // 零长线段按点距
    // 垂足参数 t 截断到 [0,1]：垂足落在线段外时取更近的端点
    double t = ((px - ax) * dx + (py - ay) * dy) / len2;
    t = std::max(0.0, std::min(1.0, t));
    return std::hypot(px - (ax + t * dx), py - (ay + t * dy));
}

static double path_dist(double x, double y, const std::vector<Point>& path) {
    if (path.size() == 1) return std::hypot(x - path[0].x, y - path[0].y);
    double best = 0;
    for (size_t i = 1; i < path.size(); i++) {
        double d = seg_dist(x, y, path[i - 1].x, path[i - 1].y, path[i].x, path[i].y);
        best = i == 1 ? d : std::min(best, d);
    }
    return best;
}

std::vector<std::pair<size_t, size_t>> brush_ranges(const std::vector<double>& mids,
                                                    double y,
                                                    const std::vector<Point>& path,
                                                    double r) {
    std::vector<std::pair<size_t, size_t>> ranges;
    if (mids.empty() || path.empty()) return ranges;
    bool run = false;
    size_t start = 0;
    for (size_t i = 0; i < mids.size(); i++) {
        if (path_dist(mids[i], y, path) <= r) {
            if (!run) { run = true; start = i; }
        } else if (run) {
            run = false;
            ranges.push_back({start, i});
        }
    }
    if (run) ranges.push_back({start, mids.size()});
    return ranges;
}

}  // namespace brush
