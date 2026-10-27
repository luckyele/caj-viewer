// caj_viewer.cpp —— CAJ 阅读器（GTK3 前端）
// 逐功能移植自 caj_viewer.py 的 Viewer 类：
//   工具栏 / 目录侧栏 / 画布绘制（纸卡 + 版面文本 + 位图页）/ 缩放 /
//   连续滚轮翻页 / 文本选区复制 / 矩形高亮标记 / 格式分析窗口。
//
// 与 Python 版的实现差异（仅内部机制，不影响观感）：
//   1. 整页先渲染进一张 cairo 图像表面缓存，滚动时只平移 blit，不再逐项移动；
//      标记/选区/尺寸/页面变化时重建该表面。
//   2. 高亮用 cairo 半透明 rgba 填充+描边（Python 位图页为 rgba(…,70) 合成、
//      文本页为 gray50 网点），视觉一致。
//   3. 插图按 pdftohtml 的 width/height 正负处理镜像；PDF image mask（stencil）
//      以墨色掩膜绘制。Python 版会把掩膜当普通位图贴（黑底白字），且负宽高被丢弃。
#include <gtk/gtk.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "caj_core.hpp"
#include "sel_brush.hpp"

namespace {

// ================= 调色板（夜读书房） =================
const char* PAL_DESK = "#232B31";
const char* PAL_PANEL = "#2E3942";
const char* PAL_PANEL_EDGE = "#3D4A54";
const char* PAL_PAPER = "#F6F1E4";
const char* PAL_PAGE_BG = "#EFE8D7";
const char* PAL_SHADOW = "#1A2025";
const char* PAL_INK = "#2C2821";
const char* PAL_BRASS = "#C8A43A";
const char* PAL_BRASS_DIM = "#8F7B33";
const char* PAL_MUTED = "#9AA69E";
const char* SEL_FILL = "#BBD6F0";

struct Color {
    double r = 0, g = 0, b = 0;
};

Color hex_color(const std::string& c, Color def = {0.949, 0.788, 0.298}) {
    if (c.size() >= 7 && c[0] == '#') {
        unsigned v = 0;
        if (sscanf(c.c_str() + 1, "%06x", &v) == 1) {
            return Color{((v >> 16) & 0xFF) / 255.0, ((v >> 8) & 0xFF) / 255.0,
                         (v & 0xFF) / 255.0};
        }
    }
    return def;
}

const char* MARK_NAME[4] = {"黄", "绿", "蓝", "红"};
const char* MARK_HEX[4] = {"#F2C94C", "#7BC96F", "#6FB7E8", "#E86F6F"};
const char* SEL_FILL_HEX = SEL_FILL;

// ================= Unicode / UTF-8 工具 =================
bool is_space_cp(uint32_t cp) {
    return cp == 0x20 || (cp >= 0x09 && cp <= 0x0D) || cp == 0x85 || cp == 0xA0 ||
           cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 ||
           cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

bool is_cjk_cp(uint32_t cp) {
    return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3000 && cp <= 0x303F) ||
           (cp >= 0xFF00 && cp <= 0xFFEF);
}

// 解码一个 UTF-8 码点；返回消耗字节数
int utf8_next(const std::string& s, size_t i, uint32_t& cp) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) { cp = c; return 1; }
    int len = 1;
    if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
    else { cp = 0xFFFD; return 1; }
    if (i + (size_t)len > s.size()) { cp = 0xFFFD; return 1; }
    for (int k = 1; k < len; k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
    return len;
}

// 每个字符的起始字节下标（末位为串长）
std::vector<size_t> utf8_bounds(const std::string& s) {
    std::vector<size_t> b;
    size_t i = 0;
    while (i < s.size()) {
        b.push_back(i);
        uint32_t cp;
        i += (size_t)utf8_next(s, i, cp);
    }
    b.push_back(s.size());
    return b;
}

int utf8_len(const std::string& s) { return (int)(utf8_bounds(s).size() - 1); }

std::string utf8_trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e) {
        uint32_t cp; int n = utf8_next(s, b, cp);
        if (!is_space_cp(cp)) break;
        b += (size_t)n;
    }
    while (e > b) {
        // 从 b 起找到最后一个字符的起点
        size_t last = b;
        size_t i = b;
        while (i < e) { size_t p = i; uint32_t cp; i += (size_t)utf8_next(s, i, cp); last = p; }
        uint32_t cp; int n = utf8_next(s, last, cp);
        if (last + (size_t)n != e || !is_space_cp(cp)) break;
        e = last;
    }
    return s.substr(b, e - b);
}

std::string utf8_substr(const std::string& s, size_t b0, size_t b1) {
    if (b1 <= b0 || b0 >= s.size()) return "";
    if (b1 > s.size()) b1 = s.size();
    return s.substr(b0, b1 - b0);
}

// 去掉夹在两个 CJK 字符（含全角标点）之间的空白（pdftohtml 逐字形输出伪迹）
std::string cjk_tight(const std::string& s) {
    std::string out;
    size_t i = 0;
    uint32_t prev_cp = 0;
    bool have_prev = false;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        if (c == ' ' || c == '\t') {
            size_t j = i;
            while (j < s.size() && ((unsigned char)s[j] == ' ' || (unsigned char)s[j] == '\t')) j++;
            uint32_t nx = 0;
            bool have_nx = j < s.size();
            if (have_nx) { size_t t = j; utf8_next(s, t, nx); }
            if (have_prev && have_nx && is_cjk_cp(prev_cp) && is_cjk_cp(nx)) {
                i = j;                 // 丢弃该段空白
                continue;
            }
            out.append(s, i, j - i);
            prev_cp = ' ';
            have_prev = true;
            i = j;
            continue;
        }
        uint32_t cp; int n = utf8_next(s, i, cp);
        out.append(s, i, (size_t)n);
        prev_cp = cp;
        have_prev = true;
        i += (size_t)n;
    }
    return out;
}

std::string sha1_16(const std::string& s) {
    gchar* h = g_compute_checksum_for_string(G_CHECKSUM_SHA1, s.c_str(), (gssize)s.size());
    std::string r = h ? h : "";
    g_free(h);
    return r.substr(0, 16);
}

std::string uuid_hex() {
    gchar* u = g_uuid_string_random();
    std::string r = u ? u : "0";
    g_free(u);
    std::string o;
    for (char c : r)
        if (c != '-') o.push_back(c);
    return o;
}

std::string base_name(const std::string& p) {
    size_t s = p.rfind('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

std::string dir_name(const std::string& p) {
    size_t s = p.rfind('/');
    if (s == std::string::npos) return ".";
    if (s == 0) return "/";
    return p.substr(0, s);
}

std::string join_path(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    return a[a.size() - 1] == '/' ? a + b : a + "/" + b;
}

void rm_rf(const std::string& dir) {
    GDir* d = g_dir_open(dir.c_str(), 0, nullptr);
    if (d) {
        const gchar* n;
        while ((n = g_dir_read_name(d))) {
            std::string p = join_path(dir, n);
            struct stat st;
            if (lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) rm_rf(p);
            else remove(p.c_str());
        }
        g_dir_close(d);
    }
    rmdir(dir.c_str());
}

// ================= 字体（角色字体族 + PDF 字体名归类） =================
const std::set<std::string>& families() {
    static std::set<std::string> fams;
    static bool done = false;
    if (!done) {
        done = true;
        PangoFontMap* fm = pango_cairo_font_map_get_default();
        PangoFontFamily** list = nullptr;
        int n = 0;
        if (fm) pango_font_map_list_families(fm, &list, &n);
        for (int i = 0; i < n; i++) {
            const char* nm = pango_font_family_get_name(list[i]);
            if (nm) fams.insert(nm);
        }
        if (list) g_free(list);
    }
    return fams;
}

std::string pick_role_family(const char* role) {
    static const char* ui[] = {"Noto Sans CJK SC", "Source Han Sans SC", "PingFang SC",
                               "Microsoft YaHei UI", "Microsoft YaHei",
                               "WenQuanYi Micro Hei", "DejaVu Sans", nullptr};
    static const char* serif[] = {"Noto Serif CJK SC", "Source Han Serif SC",
                                  "Songti SC", "STSong", "SimSun", "DejaVu Serif",
                                  nullptr};
    static const char* mono[] = {"DejaVu Sans Mono", "Noto Sans Mono CJK SC",
                                 "Consolas", "Menlo", "Monospace", nullptr};
    const char** list = (strcmp(role, "serif") == 0)  ? serif
                        : (strcmp(role, "mono") == 0) ? mono
                                                      : ui;
    for (int i = 0; list[i]; i++)
        if (families().count(list[i])) return list[i];
    return "Sans";
}

PangoFontDescription* role_font(const char* role, int px, bool bold = false,
                               bool italic = false) {
    static std::map<std::string, PangoFontDescription*> cache;
    char key[128];
    snprintf(key, sizeof(key), "%s|%d|%d|%d", role, px, (int)bold, (int)italic);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    PangoFontDescription* fd =
        pango_font_description_new();
    pango_font_description_set_family(fd, pick_role_family(role).c_str());
    pango_font_description_set_absolute_size(fd, std::max(1, px) * PANGO_SCALE);
    if (bold) pango_font_description_set_weight(fd, PANGO_WEIGHT_BOLD);
    if (italic) pango_font_description_set_style(fd, PANGO_STYLE_ITALIC);
    cache[key] = fd;
    return fd;
}

// PDF 字体名（含子集前缀如 'BGFNPF+SimSun'）-> 本机可用字体族
std::string pdf_font_family(const std::string& raw) {
    static std::map<std::string, std::string> cache;
    auto cit = cache.find(raw);
    if (cit != cache.end()) return cit->second;
    std::string name = raw;
    size_t plus = raw.rfind('+');
    if (plus != std::string::npos) name = raw.substr(plus + 1);
    // 去首尾空白
    size_t b = 0, e = name.size();
    while (b < e && (unsigned char)name[b] <= ' ') b++;
    while (e > b && (unsigned char)name[e - 1] <= ' ') e--;
    name = name.substr(b, e - b);

    std::string fam;
    if (families().count(name)) fam = name;
    if (fam.empty()) {
        std::string low;
        for (char c : name) {
            if (c == ' ' || c == '-') continue;
            low.push_back((char)tolower((unsigned char)c));
        }
        struct Cat { const char* cat; const char* keys[8]; };
        static const Cat cats[] = {
            {"cjk_sans", {"hei", "yahei", "gothic", "heiti", "wentai", "zhonghei", nullptr}},
            {"cjk_kai", {"kai", "xingkai", nullptr}},
            {"cjk_serif", {"song", "sun", "fang", "ming", "mingli", "zhengsong",
                           "xiaobiaosong", nullptr}},
            {"lat_serif", {"times", "georgia", "roman", "garamond", "book", nullptr}},
            {"lat_sans", {"arial", "helvetica", "verdana", "calibri", "tahoma", "sans",
                          "segoe", nullptr}},
            {"mono", {"courier", "mono", "consol", nullptr}},
        };
        static const char* cjk_serif[] = {"Noto Serif CJK SC", "Source Han Serif SC",
                                          "Songti SC", "STSong", "SimSun", "NSimSun",
                                          "AR PL UMing CN", "Droid Sans Fallback",
                                          "DejaVu Serif", nullptr};
        static const char* cjk_sans[] = {"Noto Sans CJK SC", "Source Han Sans SC",
                                         "Microsoft YaHei", "SimHei",
                                         "WenQuanYi Micro Hei", "Droid Sans Fallback",
                                         "DejaVu Sans", nullptr};
        static const char* cjk_kai[] = {"KaiTi", "楷体_GB2312", "STKaiti", "AR PL UKai CN",
                                        "AR PL UMing CN", "Droid Sans Fallback", nullptr};
        static const char* lat_serif[] = {"Times New Roman", "Liberation Serif",
                                         "DejaVu Serif", "Nimbus Roman", "FreeSerif",
                                         nullptr};
        static const char* lat_sans[] = {"Arial", "Liberation Sans", "DejaVu Sans",
                                        "Nimbus Sans", "FreeSans", nullptr};
        static const char* mono[] = {"Courier New", "DejaVu Sans Mono",
                                     "Liberation Mono", "Noto Mono", "FreeMono", nullptr};
        const char* cat = "cjk_serif";   // 默认中文衬线（CAJ 文献以中文为主）
        for (const Cat& c : cats) {
            bool hit = false;
            for (int i = 0; c.keys[i] && !hit; i++)
                if (low.find(c.keys[i]) != std::string::npos) hit = true;
            if (hit) { cat = c.cat; break; }
        }
        const char** cand = strcmp(cat, "cjk_sans") == 0    ? cjk_sans
                            : strcmp(cat, "cjk_kai") == 0   ? cjk_kai
                            : strcmp(cat, "lat_serif") == 0 ? lat_serif
                            : strcmp(cat, "lat_sans") == 0  ? lat_sans
                            : strcmp(cat, "mono") == 0      ? mono
                                                            : cjk_serif;
        for (int i = 0; cand[i]; i++)
            if (families().count(cand[i])) { fam = cand[i]; break; }
    }
    if (fam.empty()) fam = pick_role_family("ui");
    cache[raw] = fam;
    return fam;
}

// 文本段 -> Pango 字体（size 为像素）
PangoFontDescription* layout_font(const std::string& raw_family, double size_pt,
                                  double scale, bool bold, bool italic) {
    int px = std::max(1, (int)lround(size_pt * scale));
    std::string low;
    for (char c : raw_family) low.push_back((char)tolower((unsigned char)c));
    if (low.find("bold") != std::string::npos || low.find("black") != std::string::npos)
        bold = true;
    if (low.find("italic") != std::string::npos ||
        low.find("oblique") != std::string::npos)
        italic = true;
    static std::map<std::string, PangoFontDescription*> cache;
    std::string fam = pdf_font_family(raw_family);
    char key[256];
    snprintf(key, sizeof(key), "%s|%d|%d|%d", fam.c_str(), px, (int)bold, (int)italic);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    PangoFontDescription* fd = pango_font_description_new();
    pango_font_description_set_family(fd, fam.c_str());
    pango_font_description_set_absolute_size(fd, px * PANGO_SCALE);
    if (bold) pango_font_description_set_weight(fd, PANGO_WEIGHT_BOLD);
    if (italic) pango_font_description_set_style(fd, PANGO_STYLE_ITALIC);
    cache[key] = fd;
    return fd;
}

// ================= cairo 小工具 =================
void rr_path(cairo_t* cr, double x1, double y1, double x2, double y2, double r) {
    if (x2 < x1) std::swap(x1, x2);
    if (y2 < y1) std::swap(y1, y2);
    r = std::min(r, std::min((x2 - x1) / 2, (y2 - y1) / 2));
    cairo_new_sub_path(cr);
    cairo_arc(cr, x2 - r, y1 + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x2 - r, y2 - r, r, 0, M_PI / 2);
    cairo_arc(cr, x1 + r, y2 - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x1 + r, y1 + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

void set_color(cairo_t* cr, const Color& c, double a = 1.0) {
    cairo_set_source_rgba(cr, c.r, c.g, c.b, a);
}

// RGB 位图 -> cairo 图像表面（CAIRO_FORMAT_RGB24 为 4 字节/像素）
cairo_surface_t* bitmap_surface(const caj::Bitmap& b) {
    if (b.w <= 0 || b.h <= 0) return nullptr;
    cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, b.w, b.h);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(s);
        return nullptr;
    }
    unsigned char* d = cairo_image_surface_get_data(s);
    int stride = cairo_image_surface_get_stride(s);
    for (int y = 0; y < b.h; y++) {
        const unsigned char* src = &b.rgb[(size_t)y * b.w * 3];
        unsigned char* dst = d + (size_t)y * stride;
        for (int x = 0; x < b.w; x++) {
            dst[x * 4 + 0] = src[x * 3 + 2];
            dst[x * 4 + 1] = src[x * 3 + 1];
            dst[x * 4 + 2] = src[x * 3 + 0];
            dst[x * 4 + 3] = 0;
        }
    }
    cairo_surface_mark_dirty(s);
    return s;
}

// 图像掩膜（PDF 的 image mask，pdftohtml 输出为“白=着色区”的 1 位灰度图）
// -> A8 表面：亮度即 alpha，配合墨色绘制即得原始观感
cairo_surface_t* mask_surface(const caj::Bitmap& b) {
    if (b.w <= 0 || b.h <= 0) return nullptr;
    cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_A8, b.w, b.h);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(s);
        return nullptr;
    }
    unsigned char* d = cairo_image_surface_get_data(s);
    int stride = cairo_image_surface_get_stride(s);
    for (int y = 0; y < b.h; y++) {
        const unsigned char* src = &b.rgb[(size_t)y * b.w * 3];
        unsigned char* dst = d + (size_t)y * stride;
        for (int x = 0; x < b.w; x++) dst[x] = src[x * 3];   // 灰度图，取 R 通道
    }
    cairo_surface_mark_dirty(s);
    return s;
}

// ================= 极简 JSON（标注文件读写） =================
struct JVal {
    enum Type { NUL, BOOL, NUM, STR, ARR, OBJ } type = NUL;
    double num = 0;
    bool boolean = false;
    std::string str;
    std::vector<JVal> arr;
    std::vector<std::pair<std::string, JVal>> obj;

    const JVal* get(const char* k) const {
        if (type != OBJ) return nullptr;
        for (const auto& kv : obj)
            if (kv.first == k) return &kv.second;
        return nullptr;
    }
};

void json_escape(const std::string& s, std::string& out) {
    out += '"';
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof(b), "\\u%04x", c);
                    out += b;
                } else {
                    out.push_back((char)c);   // UTF-8 原样输出（ensure_ascii=False）
                }
        }
    }
    out += '"';
}

void utf8_append(uint32_t cp, std::string& out) {
    if (cp < 0x80) {
        out.push_back((char)cp);
    } else if (cp < 0x800) {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

struct JsonParser {
    const std::string& s;
    size_t i = 0;
    bool ok = true;
    explicit JsonParser(const std::string& src) : s(src) {}

    void ws() {
        while (i < s.size() && isspace((unsigned char)s[i])) i++;
    }
    bool lit(const char* w) {
        size_t n = strlen(w);
        if (s.compare(i, n, w) == 0) { i += n; return true; }
        ok = false;
        return false;
    }
    std::string parse_string() {
        std::string out;
        if (i >= s.size() || s[i] != '"') { ok = false; return out; }
        i++;
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\') {
                i++;
                if (i >= s.size()) break;
                char c = s[i++];
                switch (c) {
                    case 'n': out.push_back('\n'); break;
                    case 't': out.push_back('\t'); break;
                    case 'r': out.push_back('\r'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'u': {
                        unsigned v = 0;
                        if (i + 4 <= s.size() && sscanf(s.c_str() + i, "%4x", &v) == 1) {
                            i += 4;
                            uint32_t cp = v;
                            if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= s.size() &&
                                s.compare(i, 2, "\\u") == 0) {
                                unsigned lo = 0;
                                if (sscanf(s.c_str() + i + 2, "%4x", &lo) == 1 &&
                                    lo >= 0xDC00 && lo <= 0xDFFF) {
                                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                    i += 6;
                                }
                            }
                            utf8_append(cp, out);
                        }
                        break;
                    }
                    default: out.push_back(c);
                }
            } else {
                out.push_back(s[i++]);
            }
        }
        if (i < s.size()) i++;   // 收尾引号
        return out;
    }
    JVal parse() {
        JVal v;
        ws();
        if (i >= s.size()) { ok = false; return v; }
        char c = s[i];
        if (c == '{') {
            i++;
            v.type = JVal::OBJ;
            ws();
            if (i < s.size() && s[i] == '}') { i++; return v; }
            while (i < s.size() && ok) {
                ws();
                std::string k = parse_string();
                ws();
                if (i < s.size() && s[i] == ':') i++;
                JVal val = parse();
                v.obj.emplace_back(k, val);
                ws();
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == '}') { i++; break; }
                ok = false;
            }
        } else if (c == '[') {
            i++;
            v.type = JVal::ARR;
            ws();
            if (i < s.size() && s[i] == ']') { i++; return v; }
            while (i < s.size() && ok) {
                v.arr.push_back(parse());
                ws();
                if (i < s.size() && s[i] == ',') { i++; continue; }
                if (i < s.size() && s[i] == ']') { i++; break; }
                ok = false;
            }
        } else if (c == '"') {
            v.type = JVal::STR;
            v.str = parse_string();
        } else if (c == 't') {
            v.type = JVal::BOOL;
            v.boolean = lit("true");
        } else if (c == 'f') {
            v.type = JVal::BOOL;
            v.boolean = lit("false");
        } else if (c == 'n') {
            v.type = JVal::NUL;
            lit("null");
        } else {
            v.type = JVal::NUM;
            char* end = nullptr;
            v.num = strtod(s.c_str() + i, &end);
            if (!end || end == s.c_str() + i) ok = false;
            else i = (size_t)(end - s.c_str());
        }
        return v;
    }
};

bool json_parse(const std::string& s, JVal& out) {
    JsonParser p(s);
    out = p.parse();
    return p.ok;
}

// ================= 标注 =================
struct Annotation {
    int page = 1;
    double rect[4] = {0, 0, 0, 0};
    std::string color = "#F2C94C";
    std::string id;
    double created = 0;
};

// ================= 最近打开 =================
// 欢迎界面上一条最近文件的可点击热区（画布坐标）
struct RecentHit {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    std::string path;
};

// ================= 应用状态 =================
struct App {
    std::string path;
    std::string tmp;
    caj::Info cap;
    std::vector<unsigned char> pdf;
    caj::Renderer* rd = nullptr;
    int N = 0;                    // 总页数
    int cur = 1;
    double zoom = 1.0;
    double scroll = 0;            // 页内纵向滚动偏移（显示像素）
    double max_scroll = 0;
    double flip_until = 0;        // 翻页后的落位保护截止（单调时钟秒）
    double smooth_acc = 0;

    // 页面旋转（视图级，仅本次会话）。旋转角 = rot_all + rot_page[页]，取模 360。
    int rot_all = 0;              // 全部页面附加旋转，0/90/180/270
    std::map<int, int> rot_page;  // 单页附加旋转，叠加在 rot_all 之上

    // 标记
    std::vector<Annotation> annos;
    bool mark_on = false;
    std::string mark_color = "#F2C94C";
    // 拖选状态
    bool dragging = false;        // 标记拖画预览中
    double drag_x0 = 0, drag_y0 = 0;
    bool selpress = false;        // 文本选区拖选中
    double sel_x0 = 0, sel_y0 = 0;
    double ptr_x = 0, ptr_y = 0;  // 当前指针位置（预览用）
    // 文本选区 (page, nx0, ny0, nx1, ny1)
    bool has_sel = false;
    int sel_page = 0;
    double sel[4] = {0, 0, 0, 0};
    // 笔刷式选择（b 键切换；与矩形共用 selpress 拖选流）
    bool brush_mode = false;      // true=笔刷，false=矩形
    double brush_r = 8.0;         // 笔刷半径（显示像素，[ ] 调整）
    std::vector<brush::Point> brush_pts;  // 拖选中的笔画（画布坐标）
    // 落定的笔刷选区：路径为归一化页面坐标，半径按页宽比存储（缩放不变）
    bool sel_is_brush = false;
    double sel_r = 0;             // 半径 / 页宽
    std::vector<brush::Point> sel_path;

    std::string toast;
    int toast_tok = 0;
    int rev = 0;                  // 标注/选区修订号（缓存失效）

    // 最近打开（最多 5 条，持久化到用户目录）
    std::vector<std::string> recent;
    std::vector<RecentHit> recent_hits;   // draw_welcome 填充，供点击命中

    // 当前页缓存表面
    cairo_surface_t* psurf = nullptr;
    int ps_page = -1, ps_dw = 0, ps_dh = 0, ps_rev = -1, ps_rot = -1;
    caj::PagePtr ps_obj;

    // 缩放预览：连续缩放时先拉伸旧表面顶替，停手后再整页重排
    bool zoom_preview = false;
    guint zoom_timer = 0;
    // 位图表面缓存（按 Bitmap 复用；持页确保指针不被回收后复用）
    std::map<const caj::Bitmap*, cairo_surface_t*> bmp_cache;
    caj::PagePtr bmp_holder;
    int cur_dw = 0, cur_dh = 0, cur_top = 0;   // 最近一次绘制的几何
    bool cur_ready = false;
    int pending_page = 0;         // 已发出渲染请求的页号（避免重复排队）

    // 控件
    GtkWidget* win = nullptr;
    GtkWidget* canvas = nullptr;
    GtkWidget* page_lbl = nullptr;
    GtkWidget* title_lbl = nullptr;
    GtkWidget* toc_box = nullptr;
    GtkWidget* toc_frame = nullptr;
    GtkWidget* toc_sw = nullptr;
    GtkWidget* mark_btn = nullptr;
    GtkCssProvider* mark_css = nullptr;

    // 方法
    void queue_draw() { gtk_widget_queue_draw(canvas); }
    void bump_rev() { rev++; cancel_zoom_preview(); }
    static gboolean zoom_preview_cb(gpointer ud) {
        App* a = (App*)ud;
        a->zoom_timer = 0;
        a->zoom_preview = false;
        a->queue_draw();
        return G_SOURCE_REMOVE;
    }
    void cancel_zoom_preview() {
        zoom_preview = false;
        if (zoom_timer) {
            g_source_remove(zoom_timer);
            zoom_timer = 0;
        }
    }
    void begin_zoom_preview() {
        cancel_zoom_preview();
        if (!psurf) return;              // 尚无可用表面：本次直接整页渲染
        zoom_preview = true;
        zoom_timer = g_timeout_add(180, zoom_preview_cb, this);
    }
    void cancel_drag();
    void draw(cairo_t* cr, int cw, int ch);
    void draw_footer(cairo_t* cr, int cw, int ch);
    void draw_placeholder(cairo_t* cr, int cw, int ch);
    void draw_welcome(cairo_t* cr, int cw, int ch);
    cairo_surface_t* bitmap_cached(const caj::PagePtr& pg, const caj::Bitmap& b);
    cairo_surface_t* compose(const caj::PagePtr& pg, int uw, int uh, double sc);
    cairo_surface_t* page_surface(const caj::PagePtr& obj, int dw, int dh, double sc,
                                  int rot);
    // 页面旋转：本页的最终旋转角 = rot_all + rot_page[页]（取模 360）
    int eff_rot(int n) const {
        auto it = rot_page.find(n);
        return ((rot_all + (it == rot_page.end() ? 0 : it->second)) % 360 + 360) % 360;
    }
    bool rot_swapped(int n) const { int r = eff_rot(n); return r == 90 || r == 270; }
    void rotate_page() {                 // 顺时针 90°，仅当前页
        if (!rd || N <= 0) { toast_msg("请先打开文档"); return; }
        int r = (eff_rot(cur) + 90) % 360;
        rot_page[cur] = ((r - rot_all) % 360 + 360) % 360;
        bump_rev();
        queue_draw();
        char b[64];
        snprintf(b, sizeof(b), "第 %d 页旋转 %d°", cur, r);
        toast_msg(b);
    }
    void rotate_all_pages() {            // 顺时针 90°，全部页面
        if (!rd || N <= 0) { toast_msg("请先打开文档"); return; }
        rot_all = (rot_all + 90) % 360;
        bump_rev();
        queue_draw();
        char b[64];
        snprintf(b, sizeof(b), "全部页面旋转 %d°", rot_all);
        toast_msg(b);
    }
    double display_scale(double img_w, int cw) const {
        return (std::max(cw, 50) / img_w) * zoom;
    }
    void set_page(int n, bool at_bottom = false);
    void load_toc();
    void first() { set_page(1); }
    void last() { set_page(N); }
    void prev() { set_page(cur - 1); }
    void next() { set_page(cur + 1); }
    void zoom_in() { zoom = std::min(3.0, zoom * 1.25); bump_rev(); begin_zoom_preview(); queue_draw(); }
    void zoom_out() { zoom = std::max(0.3, zoom / 1.25); bump_rev(); begin_zoom_preview(); queue_draw(); }
    void wheel_page(int step);
    void real_wheel(int step);
    void prefetch();
    void request_page(int n);
    void on_page_ready(int n, caj::PagePtr pg);
    void open_file();
    void open_path(const std::string& p);
    void show_info();
    void toast_msg(const std::string& m);
    // 标注
    std::string anno_path() const;
    void load_annotations();
    void save_annotations() const;
    // 最近打开
    std::string recent_path() const;
    void load_recent();
    void save_recent() const;
    void add_recent(const std::string& p);
    Annotation* anno_at(double x, double y);
    void anno_menu(Annotation* a, double x, double y);
    // 选区
    struct Span { size_t run, b0, b1; };
    std::vector<std::vector<Span>> sel_spans(const caj::PageLayout& lay);
    std::string sel_text();
    void copy_selection();
    void clear_selection();
    void sel_menu(double x, double y);
    void toggle_brush();
    void brush_adjust(double d);
    // 几何
    void page_rect(double cw, double ch, double* left, double* top_y) const;
    void norm_from_canvas(double cw, double ch, double x, double y, double* nx,
                          double* ny) const;
};

// ---- 标注持久化 ----
std::string App::anno_path() const {
    std::string p = path + ".annotations.json";
    std::string probe = join_path(dir_name(p), ".cajviewer.write_probe");
    FILE* f = fopen(probe.c_str(), "wb");
    if (f) {
        fwrite("x", 1, 1, f);
        fclose(f);
        remove(probe.c_str());
        return p;
    }
    gchar* abs = g_canonicalize_filename(path.c_str(), nullptr);
    std::string full = abs ? abs : path;
    if (abs) g_free(abs);
    const char* home = g_get_home_dir();
    if (!home) return "";
    std::string d = std::string(home) + "/.local/share/caj-viewer/annotations";
    if (g_mkdir_with_parents(d.c_str(), 0755) != 0) return "";
    return join_path(d, sha1_16(full) + ".annotations.json");
}

void App::load_annotations() {
    annos.clear();
    std::string cp = anno_path();
    if (cp.empty()) return;
    gchar* data = nullptr;
    gsize n = 0;
    if (!g_file_get_contents(cp.c_str(), &data, &n, nullptr)) return;
    std::string txt(data, n);
    g_free(data);
    JVal root;
    if (!json_parse(txt, root)) return;
    const JVal* ver = root.get("version");
    if (!ver || ver->type != JVal::NUM || (int)ver->num != 1) return;
    const JVal* arr = root.get("annotations");
    if (!arr || arr->type != JVal::ARR) return;
    for (const JVal& d : arr->arr) {
        if (d.type != JVal::OBJ) continue;
        const JVal* r = d.get("rect");
        if (!r || r->type != JVal::ARR || r->arr.size() != 4) continue;
        bool nums = true;
        for (const JVal& v : r->arr)
            if (v.type != JVal::NUM) nums = false;
        if (!nums) continue;
        Annotation a;
        const JVal* p = d.get("page");
        a.page = (p && p->type == JVal::NUM) ? (int)p->num : 1;
        for (int k = 0; k < 4; k++) a.rect[k] = r->arr[(size_t)k].num;
        const JVal* c = d.get("color");
        if (c && c->type == JVal::STR) a.color = c->str;
        const JVal* id = d.get("id");
        if (id && id->type == JVal::STR) a.id = id->str;
        const JVal* cr = d.get("created");
        a.created = (cr && cr->type == JVal::NUM) ? cr->num : 0;
        if (a.id.empty()) a.id = uuid_hex().substr(0, 12);
        if (a.created == 0) a.created = (double)g_get_real_time() / 1e6;
        annos.push_back(a);
    }
}

void App::save_annotations() const {
    std::string cp = anno_path();
    if (cp.empty()) return;
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return;
    double m = (double)st.st_mtim.tv_sec + (double)st.st_mtim.tv_nsec / 1e9;
    int64_t mtime_ns = (int64_t)(m * 1e9);
    std::string j = "{\n \"version\": 1,\n \"source\": {\n  \"size\": ";
    char b[64];
    snprintf(b, sizeof(b), "%lld", (long long)st.st_size);
    j += b;
    j += ",\n  \"mtime_ns\": ";
    snprintf(b, sizeof(b), "%lld", (long long)mtime_ns);
    j += b;
    j += "\n },\n \"annotations\": [";
    for (size_t i = 0; i < annos.size(); i++) {
        const Annotation& a = annos[i];
        j += i ? ",\n  {\n" : "\n  {\n";
        j += "   \"id\": ";
        json_escape(a.id, j);
        snprintf(b, sizeof(b), ",\n   \"page\": %d,\n   \"rect\": [", a.page);
        j += b;
        for (int k = 0; k < 4; k++) {
            snprintf(b, sizeof(b), "%s%.6f", k ? ", " : "", a.rect[k]);
            j += b;
        }
        j += "],\n   \"color\": ";
        json_escape(a.color, j);
        snprintf(b, sizeof(b), ",\n   \"created\": %.6f\n  }", a.created);
        j += b;
    }
    j += annos.empty() ? "]\n}\n" : "\n ]\n}\n";
    std::string tmp = cp + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    fwrite(j.data(), 1, j.size(), f);
    fclose(f);
    if (rename(tmp.c_str(), cp.c_str()) != 0) remove(tmp.c_str());
}

// ---- 最近打开持久化 ----
std::string App::recent_path() const {
    const char* ud = g_get_user_data_dir();   // 通常 ~/.local/share（可被 XDG 覆盖）
    std::string base;
    if (ud) {
        base = ud;
    } else {
        const char* home = g_get_home_dir();
        if (!home) return "";
        base = join_path(home, ".local/share");
    }
    std::string d = join_path(base, "caj-viewer");
    if (g_mkdir_with_parents(d.c_str(), 0755) != 0) return "";
    return join_path(d, "recent.json");
}

void App::load_recent() {
    recent.clear();
    std::string cp = recent_path();
    if (cp.empty()) return;
    gchar* data = nullptr;
    gsize n = 0;
    if (!g_file_get_contents(cp.c_str(), &data, &n, nullptr)) return;
    std::string txt(data, n);
    g_free(data);
    JVal root;
    if (!json_parse(txt, root) || root.type != JVal::ARR) return;   // 损坏则当空表
    for (const JVal& v : root.arr) {
        if (v.type != JVal::STR || v.str.empty()) continue;
        if (std::find(recent.begin(), recent.end(), v.str) != recent.end()) continue;
        recent.push_back(v.str);
        if (recent.size() >= 5) break;
    }
}

void App::save_recent() const {
    std::string cp = recent_path();
    if (cp.empty()) return;
    std::string j = "[";
    for (size_t i = 0; i < recent.size(); i++) {
        if (i) j += ",";
        json_escape(recent[i], j);
    }
    j += "]\n";
    std::string tmp = cp + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    fwrite(j.data(), 1, j.size(), f);
    fclose(f);
    if (rename(tmp.c_str(), cp.c_str()) != 0) remove(tmp.c_str());
}

void App::add_recent(const std::string& p) {
    if (p.empty()) return;
    recent.erase(std::remove(recent.begin(), recent.end(), p), recent.end());
    recent.insert(recent.begin(), p);
    if (recent.size() > 5) recent.resize(5);
    save_recent();
}

void App::cancel_drag() {
    dragging = false;
    selpress = false;
}

struct ToastTok {
    App* app;
    int tok;
};

void App::toast_msg(const std::string& m) {
    toast = m;
    int tok = ++toast_tok;
    ToastTok* t = new ToastTok{this, tok};
    g_timeout_add_full(
        G_PRIORITY_DEFAULT, 1800,
        [](gpointer ud) -> gboolean {
            ToastTok* tt = (ToastTok*)ud;
            if (tt->app->toast_tok == tt->tok) {   // 期间未弹出新提示才清除
                tt->app->toast.clear();
                tt->app->queue_draw();
            }
            delete tt;
            return G_SOURCE_REMOVE;
        },
        t, nullptr);
    queue_draw();
}

// ---- 几何 ----
void App::page_rect(double cw, double ch, double* left, double* top_y) const {
    double dw = cur_dw, dh = cur_dh;
    double ch_eff = ch - 44;
    *left = cw / 2 - dw / 2;
    *top_y = (dh <= ch_eff) ? (ch_eff - dh) / 2 : -scroll;
}

void App::norm_from_canvas(double cw, double ch, double x, double y, double* nx,
                           double* ny) const {
    double left, top_y;
    page_rect(cw, ch, &left, &top_y);
    double u = cur_dw ? (x - left) / cur_dw : 0.0;   // 旋转后画面空间归一化
    double v = cur_dh ? (y - top_y) / cur_dh : 0.0;
    // 逆旋转：映射回未旋转的页面空间（标注/选区始终存该空间的归一化坐标）
    int rot = eff_rot(cur);
    if (rot == 90) { *nx = v; *ny = 1.0 - u; }
    else if (rot == 180) { *nx = 1.0 - u; *ny = 1.0 - v; }
    else if (rot == 270) { *nx = 1.0 - v; *ny = u; }
    else { *nx = u; *ny = v; }
    *nx = std::max(0.0, std::min(1.0, *nx));
    *ny = std::max(0.0, std::min(1.0, *ny));
}

// ================= 绘制：页面缓存表面 =================
// 一段文本在缩放布局中的逐字符 x 区间（字符下标 -> 字节切片）
struct RunGeom {
    std::vector<size_t> bounds;    // 字符边界字节下标（n+1 项）
    std::vector<double> xs0, xs1;  // 每字符 x 区间（缩放后页面坐标）
};

// 本机字体与 PDF 内嵌子集字体的度量不一致（如 Noto Serif CJK 每字略窄于
// 方正宋体），pdftohtml 的 run 宽度是 PDF 里的真实占宽；按自然宽度逐段
// 绘制时误差在长 run 内逐字累计，与相邻 run 重叠脱节。把字符区间等比
// 拉伸/压缩到声明宽度（阈值 2.5% 内不动），绘制与选区共用同一映射。
double run_hfit(const caj::TextRun& tr, double sc, double tw_px) {
    double target = tr.w * sc;
    if (tr.w <= 0 || tw_px <= 0) return 1.0;
    double k = target / tw_px;
    if (k > 1.0 - 0.025 && k < 1.0 + 0.025) return 1.0;
    return k;
}

void run_geom(const caj::TextRun& tr, const caj::FontSpec& fs, double sc,
              RunGeom& g) {
    g.bounds = utf8_bounds(tr.text);
    size_t n = g.bounds.empty() ? 0 : g.bounds.size() - 1;
    g.xs0.assign(n, 0);
    g.xs1.assign(n, 0);
    if (!n) return;
    cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 1, 1);
    cairo_t* cr = cairo_create(s);
    PangoLayout* pl = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(
        pl, layout_font(fs.family, fs.size, sc, tr.bold, tr.italic));
    pango_layout_set_text(pl, tr.text.c_str(), (int)tr.text.size());
    std::vector<double> xs(n);
    for (size_t i = 0; i < n; i++) {
        PangoRectangle r;
        pango_layout_index_to_pos(pl, (int)g.bounds[i], &r);
        xs[i] = (double)r.x / PANGO_SCALE;
    }
    int tw = 0, th = 0;
    pango_layout_get_pixel_size(pl, &tw, &th);
    double k = run_hfit(tr, sc, (double)tw);
    for (size_t i = 0; i < n; i++) {
        g.xs0[i] = tr.x * sc + xs[i] * k;
        g.xs1[i] = tr.x * sc + (i + 1 < n ? xs[i + 1] : (double)tw) * k;
    }
    g_object_unref(pl);
    cairo_destroy(cr);
    cairo_surface_destroy(s);
}

// 位图 -> cairo 表面缓存：同一页面内同一 Bitmap 只做一次逐像素转换
cairo_surface_t* App::bitmap_cached(const caj::PagePtr& pg, const caj::Bitmap& b) {
    if (bmp_holder != pg) {              // 换页：丢弃上一页的位图表面
        for (auto& kv : bmp_cache) cairo_surface_destroy(kv.second);
        bmp_cache.clear();
        bmp_holder = pg;
    }
    auto it = bmp_cache.find(&b);
    if (it != bmp_cache.end()) return cairo_surface_reference(it->second);
    cairo_surface_t* s = bitmap_surface(b);
    if (!s) return nullptr;
    bmp_cache[&b] = s;                   // 缓存持有原始引用
    return cairo_surface_reference(s);   // 返回引用给调用方，调用方照旧 destroy
}

// 把整页表面按 90° 整数倍顺时针旋转（仅 90/270 交换宽高）。rot=0 时返回原表面引用。
cairo_surface_t* rotate_surface(cairo_surface_t* src, int rot) {
    rot = ((rot % 360) + 360) % 360;
    if (rot == 0) return cairo_surface_reference(src);
    int sw = cairo_image_surface_get_width(src);
    int sh = cairo_image_surface_get_height(src);
    bool swap = (rot == 90 || rot == 270);
    int dw = swap ? sh : sw, dh = swap ? sw : sh;
    cairo_surface_t* dst = cairo_image_surface_create(CAIRO_FORMAT_RGB24, dw, dh);
    if (cairo_surface_status(dst) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(dst);
        return nullptr;
    }
    cairo_t* cr = cairo_create(dst);
    // 源(surface) -> 目标(device) 顺时针映射：90:(sh-sy,sx) 180:(sw-sx,sh-sy) 270:(sy,sw-sx)
    if (rot == 90) {
        cairo_translate(cr, sh, 0);
        cairo_rotate(cr, M_PI / 2);
    } else if (rot == 180) {
        cairo_translate(cr, sw, sh);
        cairo_rotate(cr, M_PI);
    } else {  // 270
        cairo_translate(cr, 0, sw);
        cairo_rotate(cr, -M_PI / 2);
    }
    cairo_set_source_surface(cr, src, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    cairo_paint(cr);
    cairo_destroy(cr);
    return dst;
}

cairo_surface_t* App::compose(const caj::PagePtr& pg, int uw, int uh, double sc) {
    cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, uw, uh);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(s);
        return nullptr;
    }
    cairo_t* cr = cairo_create(s);
    set_color(cr, hex_color(PAL_PAGE_BG));
    cairo_paint(cr);

    // 高亮标记：矩形画在文字下层（半透明，不遮字）
    std::vector<const Annotation*> marks;
    for (const Annotation& a : annos)
        if (a.page == cur) marks.push_back(&a);

    if (pg->bitmap) {
        // 位图页（扫描件）：标记以 alpha 70 合成在图像之上（同 Python）
        cairo_surface_t* bs = bitmap_cached(pg, *pg->bitmap);
        if (bs) {
            cairo_save(cr);
            cairo_scale(cr, (double)uw / pg->bitmap->w, (double)uh / pg->bitmap->h);
            cairo_set_source_surface(cr, bs, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
            cairo_paint(cr);
            cairo_restore(cr);
            cairo_surface_destroy(bs);
        }
        for (const Annotation* a : marks) {
            Color c = hex_color(a->color);
            cairo_rectangle(cr, a->rect[0] * uw, a->rect[1] * uh,
                            (a->rect[2] - a->rect[0]) * uw,
                            (a->rect[3] - a->rect[1]) * uh);
            set_color(cr, c, 70.0 / 255.0);
            cairo_fill_preserve(cr);
            set_color(cr, c, 230.0 / 255.0);
            cairo_set_line_width(cr, 1);
            cairo_stroke(cr);
        }
    } else if (pg->layout) {
        const caj::PageLayout& lay = *pg->layout;
        for (const Annotation* a : marks) {
            Color c = hex_color(a->color);
            cairo_rectangle(cr, a->rect[0] * uw, a->rect[1] * uh,
                            (a->rect[2] - a->rect[0]) * uw,
                            (a->rect[3] - a->rect[1]) * uh);
            set_color(cr, c, 0.42);
            cairo_fill_preserve(cr);
            set_color(cr, c, 0.9);
            cairo_set_line_width(cr, 1);
            cairo_stroke(cr);
        }
        // 文本选区底（在文字下层）；水平相邻/重叠的 span 合并成一个矩形，
        // 矩形选区视觉不变，笔刷的不连续簇分开涂
        if (has_sel && sel_page == cur) {
            struct HlRect {
                double x0, x1, y0, y1;
            };
            std::vector<HlRect> groups;
            for (const auto& ln : sel_spans(lay)) {
                for (const Span& sp : ln) {
                    const caj::TextRun& tr = lay.texts[sp.run];
                    auto fit = lay.fonts.find(tr.font);
                    caj::FontSpec fs =
                        fit != lay.fonts.end() ? fit->second : caj::FontSpec();
                    RunGeom g;
                    run_geom(tr, fs, sc, g);
                    if (g.xs0.empty()) continue;
                    size_t i0 = 0, i1 = g.xs0.size();
                    while (i0 < i1 && g.bounds[i0] < sp.b0) i0++;
                    while (i1 > i0 && g.bounds[i1 - 1] >= sp.b1) i1--;
                    if (i0 >= i1) continue;
                    HlRect r{g.xs0[i0], g.xs1[i1 - 1], tr.y * sc,
                             (tr.y + tr.h) * sc};
                    // 与上一组 x 区间相接（含 0.5px 容差）且 y 带重叠则合并
                    if (!groups.empty()) {
                        HlRect& p = groups.back();
                        if (r.x0 <= p.x1 + 0.5 && r.x1 >= p.x0 - 0.5 &&
                            r.y0 < p.y1 && r.y1 > p.y0) {
                            p.x0 = std::min(p.x0, r.x0);
                            p.x1 = std::max(p.x1, r.x1);
                            p.y0 = std::min(p.y0, r.y0);
                            p.y1 = std::max(p.y1, r.y1);
                            continue;
                        }
                    }
                    groups.push_back(r);
                }
            }
            for (const HlRect& g : groups) {
                cairo_rectangle(cr, g.x0, g.y0, g.x1 - g.x0, g.y1 - g.y0);
                set_color(cr, hex_color(SEL_FILL_HEX));
                cairo_fill(cr);
            }
        }
        // 插图（pdftohtml 的 width/height 可为负 = 镜像，此时 left/top 是矩形另一侧边缘）
        for (const caj::ImgRun& ir : lay.imgs) {
            if (!ir.bmp || ir.bmp->w <= 0 || ir.bmp->h <= 0) continue;
            double x0 = std::min(ir.x, ir.x + ir.w);
            double y0 = std::min(ir.y, ir.y + ir.h);
            double bw = std::max(1.0, std::fabs(ir.w) * sc);
            double bh = std::max(1.0, std::fabs(ir.h) * sc);
            cairo_save(cr);
            cairo_rectangle(cr, x0 * sc, y0 * sc, bw, bh);
            cairo_clip(cr);
            // clip 已按当前（未缩放）坐标系固定，绘制前必须平移到目标位置
            cairo_translate(cr, ir.w < 0 ? x0 * sc + bw : x0 * sc,
                                ir.h < 0 ? y0 * sc + bh : y0 * sc);
            cairo_scale(cr, (ir.w < 0 ? -bw : bw) / ir.bmp->w,
                            (ir.h < 0 ? -bh : bh) / ir.bmp->h);
            if (ir.mask) {
                // 掩膜以墨色绘制（白色为着色区），不铺底色
                cairo_surface_t* ms = mask_surface(*ir.bmp);
                if (ms) {
                    set_color(cr, hex_color(PAL_INK));
                    cairo_mask_surface(cr, ms, 0, 0);
                    cairo_surface_destroy(ms);
                }
            } else {
                cairo_surface_t* bs = bitmap_cached(pg, *ir.bmp);
                if (bs) {
                    cairo_set_source_surface(cr, bs, 0, 0);
                    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
                    cairo_paint(cr);
                    cairo_surface_destroy(bs);
                }
            }
            cairo_restore(cr);
        }
        // 文字：锚点取各段纵向中点（同 Python 的 anchor="w"）；横向按 run
        // 声明宽度微缩放，抵消本机字体与 PDF 内嵌字体的度量差
        for (const caj::TextRun& tr : lay.texts) {
            auto fit = lay.fonts.find(tr.font);
            caj::FontSpec fs = fit != lay.fonts.end() ? fit->second : caj::FontSpec();
            PangoLayout* pl = pango_cairo_create_layout(cr);
            pango_layout_set_font_description(
                pl, layout_font(fs.family, fs.size, sc, tr.bold, tr.italic));
            pango_layout_set_text(pl, tr.text.c_str(), (int)tr.text.size());
            int tw = 0, th = 0;
            pango_layout_get_pixel_size(pl, &tw, &th);
            double k = run_hfit(tr, sc, (double)tw);
            set_color(cr, hex_color(fs.color, Color{0, 0, 0}));
            cairo_move_to(cr, tr.x * sc, (tr.y + tr.h / 2) * sc - th / 2.0);
            if (k != 1.0) {
                cairo_save(cr);
                cairo_translate(cr, tr.x * sc, 0);
                cairo_scale(cr, k, 1.0);
                cairo_translate(cr, -tr.x * sc, 0);
                pango_cairo_show_layout(cr, pl);
                cairo_restore(cr);
            } else {
                pango_cairo_show_layout(cr, pl);
            }
            g_object_unref(pl);
        }
    }
    cairo_destroy(cr);
    return s;
}

cairo_surface_t* App::page_surface(const caj::PagePtr& obj, int dw, int dh,
                                   double sc, int rot) {
    rot = ((rot % 360) + 360) % 360;
    if (psurf && ps_page == cur && ps_dw == dw && ps_dh == dh && ps_rev == rev &&
        ps_rot == rot && ps_obj == obj)
        return psurf;
    if (psurf) {
        cairo_surface_destroy(psurf);
        psurf = nullptr;
    }
    // compose 产出未旋转表面（uw×uh）；旋转在整页层做，标注/选区/文字一并跟随
    bool swap = (rot == 90 || rot == 270);
    int uw = swap ? dh : dw;
    int uh = swap ? dw : dh;
    cairo_surface_t* s = compose(obj, uw, uh, sc);
    if (s && rot != 0) {
        cairo_surface_t* r = rotate_surface(s, rot);
        cairo_surface_destroy(s);
        s = r;
    }
    psurf = s;
    ps_page = cur;
    ps_dw = dw;
    ps_dh = dh;
    ps_rev = rev;
    ps_rot = rot;
    ps_obj = obj;
    return psurf;
}

void App::draw_footer(cairo_t* cr, int cw, int ch) {
    const int fh = 44;
    set_color(cr, hex_color(PAL_PANEL));
    cairo_rectangle(cr, 0, ch - fh, cw, fh);
    cairo_fill(cr);
    set_color(cr, hex_color(PAL_BRASS));
    cairo_set_line_width(cr, 1);
    cairo_move_to(cr, 0, ch - fh + 0.5);
    cairo_line_to(cr, cw, ch - fh + 0.5);
    cairo_stroke(cr);

    PangoLayout* lay = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(lay, role_font("serif", 15));
    char buf[128];
    snprintf(buf, sizeof(buf), "第 %d 页 · 共 %d 页", cur, N);
    pango_layout_set_text(lay, buf, -1);
    int tw = 0, th = 0;
    pango_layout_get_pixel_size(lay, &tw, &th);
    set_color(cr, hex_color(PAL_MUTED));
    cairo_move_to(cr, (cw - tw) / 2.0, ch - fh + 12);
    pango_cairo_show_layout(cr, lay);
    g_object_unref(lay);

    // 黄铜“页垛”阅读进度条
    double w = std::min(240.0, std::max(120.0, (double)cw - 160));
    double x1 = (cw - w) / 2.0, y1 = ch - fh + 28;
    set_color(cr, hex_color(PAL_PANEL_EDGE));
    cairo_rectangle(cr, x1, y1, w, 5);
    cairo_fill(cr);
    double fw = std::max(5.0, w * cur / std::max(1, N));
    set_color(cr, hex_color(PAL_BRASS));
    cairo_rectangle(cr, x1, y1, fw, 5);
    cairo_fill(cr);
}

void App::draw_placeholder(cairo_t* cr, int cw, int ch) {
    const double pw = 420, ph = 150;
    double cx = cw / 2.0, cy = ch / 2.0;
    set_color(cr, hex_color(PAL_SHADOW));
    for (int k = 0; k < 2; k++) {
        double ox = k == 0 ? 3 : 5, oy = k == 0 ? 4 : 6;
        cairo_rectangle(cr, cx - pw / 2 + ox, cy - ph / 2 + oy, pw, ph);
        cairo_fill(cr);
    }
    cairo_rectangle(cr, cx - pw / 2, cy - ph / 2, pw, ph);
    set_color(cr, hex_color(PAL_PAGE_BG));
    cairo_fill_preserve(cr);
    set_color(cr, hex_color(PAL_SHADOW));
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);

    PangoLayout* lay = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(lay, role_font("serif", 20));
    char buf[128];
    snprintf(buf, sizeof(buf), "正在渲染第 %d 页…", cur);
    pango_layout_set_text(lay, buf, -1);
    int tw = 0, th = 0;
    pango_layout_get_pixel_size(lay, &tw, &th);
    set_color(cr, hex_color(PAL_INK));
    cairo_move_to(cr, cx - tw / 2.0, cy - 12);
    pango_cairo_show_layout(cr, lay);
    g_object_unref(lay);

    set_color(cr, hex_color(PAL_BRASS));
    cairo_set_line_width(cr, 2);
    cairo_move_to(cr, cx - 70, cy + 34);
    cairo_line_to(cr, cx + 70, cy + 34);
    cairo_stroke(cr);
    draw_footer(cr, cw, ch);
}

// 未打开文档时的欢迎卡片（先进入界面，再由用户选文件）
// 同时展示最近打开列表：每条绘制为一行，并把画布坐标热区存入 recent_hits，
// 由 on_press_cb 负责命中并调用 open_path。
void App::draw_welcome(cairo_t* cr, int cw, int ch) {
    recent_hits.clear();
    const double pw = 520;
    const double row_h = 30;
    const double header_h = 168;
    int n = (int)std::min<size_t>(recent.size(), 5);
    double list_h = n > 0 ? (24 + n * row_h + 10) : 0;
    double ph = header_h + list_h;
    double cx = cw / 2.0, cy = ch / 2.0;
    double left = cx - pw / 2.0, top = cy - ph / 2.0;

    set_color(cr, hex_color(PAL_SHADOW));
    for (int k = 0; k < 2; k++) {
        double ox = k == 0 ? 3 : 5, oy = k == 0 ? 4 : 6;
        cairo_rectangle(cr, left + ox, top + oy, pw, ph);
        cairo_fill(cr);
    }
    cairo_rectangle(cr, left, top, pw, ph);
    set_color(cr, hex_color(PAL_PAGE_BG));
    cairo_fill_preserve(cr);
    set_color(cr, hex_color(PAL_SHADOW));
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);

    PangoLayout* lay = pango_cairo_create_layout(cr);
    int tw = 0, th = 0;
    pango_layout_set_font_description(lay, role_font("serif", 22));
    pango_layout_set_text(lay, "CAJ 阅读器", -1);
    pango_layout_get_pixel_size(lay, &tw, &th);
    set_color(cr, hex_color(PAL_INK));
    cairo_move_to(cr, cx - tw / 2.0, top + 26);
    pango_cairo_show_layout(cr, lay);

    pango_layout_set_font_description(lay, role_font("ui", 14));
    pango_layout_set_text(lay, "尚未打开文档", -1);
    pango_layout_get_pixel_size(lay, &tw, &th);
    set_color(cr, hex_color(PAL_MUTED));
    cairo_move_to(cr, cx - tw / 2.0, top + 66);
    pango_cairo_show_layout(cr, lay);

    pango_layout_set_font_description(lay, role_font("ui", 13));
    pango_layout_set_text(lay,
                          "点击工具栏「打开」或按 Ctrl+O 选择 CAJ / KDH / PDF 文件",
                          -1);
    pango_layout_get_pixel_size(lay, &tw, &th);
    cairo_move_to(cr, cx - tw / 2.0, top + 100);
    pango_cairo_show_layout(cr, lay);
    g_object_unref(lay);

    set_color(cr, hex_color(PAL_BRASS));
    cairo_set_line_width(cr, 2);
    cairo_move_to(cr, cx - 90, top + 146);
    cairo_line_to(cr, cx + 90, top + 146);
    cairo_stroke(cr);

    if (n == 0) return;   // 无最近记录：仅显示卡片，正常早退

    PangoLayout* hl = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(hl, role_font("ui", 13));
    pango_layout_set_text(hl, "最近打开", -1);
    pango_layout_get_pixel_size(hl, &tw, &th);
    set_color(cr, hex_color(PAL_BRASS_DIM));
    cairo_move_to(cr, left + 30, top + 172);
    pango_cairo_show_layout(cr, hl);
    g_object_unref(hl);

    double rx0 = left + 30, rx1 = left + pw - 30;
    for (int i = 0; i < n; i++) {
        const std::string& rp = recent[(size_t)i];
        double ry0 = top + 196 + i * row_h;
        double ry1 = ry0 + row_h;
        recent_hits.push_back(RecentHit{rx0, ry0, rx1, ry1, rp});

        set_color(cr, hex_color(PAL_SHADOW), 0.15);
        cairo_set_line_width(cr, 1);
        cairo_move_to(cr, rx0, ry1 - 0.5);
        cairo_line_to(cr, rx1, ry1 - 0.5);
        cairo_stroke(cr);

        PangoLayout* il = pango_cairo_create_layout(cr);
        pango_layout_set_font_description(il, role_font("mono", 12));
        char nb[16];
        snprintf(nb, sizeof(nb), "%d", i + 1);
        pango_layout_set_text(il, nb, -1);
        pango_layout_get_pixel_size(il, &tw, &th);
        set_color(cr, hex_color(PAL_BRASS_DIM));
        cairo_move_to(cr, rx0 + 4, ry0 + (row_h - th) / 2.0 - 1);
        pango_cairo_show_layout(cr, il);
        g_object_unref(il);

        std::string nm = base_name(rp);
        std::vector<size_t> nbnd = utf8_bounds(nm);
        if (nbnd.size() - 1 > 34) nm = nm.substr(0, nbnd[34]) + "…";
        PangoLayout* nl = pango_cairo_create_layout(cr);
        pango_layout_set_font_description(nl, role_font("serif", 14));
        pango_layout_set_text(nl, nm.c_str(), -1);
        pango_layout_get_pixel_size(nl, &tw, &th);
        set_color(cr, hex_color(PAL_INK));
        cairo_move_to(cr, rx0 + 24, ry0 + (row_h - th) / 2.0 - 1);
        pango_cairo_show_layout(cr, nl);
        g_object_unref(nl);

        std::string dr = dir_name(rp);
        std::vector<size_t> dbnd = utf8_bounds(dr);
        if (dbnd.size() - 1 > 40) dr = "…" + dr.substr(dbnd[dbnd.size() - 1 - 40]);
        PangoLayout* dl = pango_cairo_create_layout(cr);
        pango_layout_set_font_description(dl, role_font("ui", 11));
        pango_layout_set_text(dl, dr.c_str(), -1);
        int dw2 = 0, dh2 = 0;
        pango_layout_get_pixel_size(dl, &dw2, &dh2);
        set_color(cr, hex_color(PAL_MUTED));
        double dx = rx1 - 8 - dw2;
        if (dx > rx0 + 24 + tw + 12) {   // 目录与文件名不重叠才画
            cairo_move_to(cr, dx, ry0 + (row_h - dh2) / 2.0 - 1);
            pango_cairo_show_layout(cr, dl);
        }
        g_object_unref(dl);
    }
}

void App::draw(cairo_t* cr, int cw, int ch) {
    set_color(cr, hex_color(PAL_DESK));
    cairo_paint(cr);

    if (!rd) {                     // 尚未打开文档
        cur_ready = false;
        draw_welcome(cr, cw, ch);
        return;
    }

    caj::PagePtr obj = rd->get(cur);
    if (!obj) {
        cur_ready = false;
        draw_placeholder(cr, cw, ch);
        request_page(cur);
        return;
    }
    double img_w = 1, img_h = 1;
    if (obj->layout) {
        img_w = std::max(1.0, obj->layout->w);
        img_h = std::max(1.0, obj->layout->h);
    } else if (obj->bitmap) {
        img_w = std::max(1, obj->bitmap->w);
        img_h = std::max(1, obj->bitmap->h);
    }
    int rot = eff_rot(cur);
    bool swap = (rot == 90 || rot == 270);
    // 定标基准是旋转后的页面宽度：横向页转 90° 后按高度铺满
    double sc = display_scale(swap ? img_h : img_w, cw);
    int uw = std::max(1, (int)(img_w * sc));
    int uh = std::max(1, (int)(img_h * sc));
    int dw = swap ? uh : uw;
    int dh = swap ? uw : uh;
    double ch_eff = ch - 44;
    max_scroll = std::max(0.0, dh - ch_eff);
    scroll = std::max(0.0, std::min(scroll, max_scroll));
    double top_y = dh <= ch_eff ? (ch_eff - dh) / 2 : -scroll;
    double left = cw / 2.0 - dw / 2.0;
    cur_dw = dw;
    cur_dh = dh;
    cur_top = (int)top_y;
    cur_ready = true;

    const double pad = 10;
    set_color(cr, hex_color(PAL_SHADOW));
    for (int k = 0; k < 2; k++) {
        double ox = k == 0 ? 3 : 5, oy = k == 0 ? 4 : 6;
        rr_path(cr, left - pad + ox, top_y - pad + oy, left + dw + pad + ox,
                top_y + dh + pad + oy, 8);
        cairo_fill(cr);
    }
    rr_path(cr, left - pad, top_y - pad, left + dw + pad, top_y + dh + pad, 8);
    set_color(cr, hex_color(PAL_PAGE_BG));
    cairo_fill_preserve(cr);
    set_color(cr, hex_color(PAL_SHADOW));
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);

    // 缩放预览：直接拉伸上一张整页表面，避免每个缩放步进都在主线程整页重排
    bool preview = zoom_preview && psurf && ps_page == cur && ps_obj == obj &&
                   ps_rot == rot && ps_dw > 0 && ps_dh > 0;
    cairo_surface_t* ps = nullptr;
    if (preview) {
        ps = psurf;
    } else {
        zoom_preview = false;
        ps = page_surface(obj, dw, dh, sc, rot);
    }
    if (ps) {
        cairo_save(cr);
        if (preview) {
            cairo_translate(cr, left, top_y);
            cairo_scale(cr, (double)dw / ps_dw, (double)dh / ps_dh);
            cairo_set_source_surface(cr, ps, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
        } else {
            cairo_set_source_surface(cr, ps, left, top_y);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
        }
        cairo_paint(cr);
        cairo_restore(cr);
    }

    // 拖选预览
    if (dragging) {
        const double dash[2] = {4, 3};
        cairo_save(cr);
        cairo_set_dash(cr, dash, 2, 0);
        set_color(cr, hex_color(mark_color));
        cairo_set_line_width(cr, 2);
        cairo_rectangle(cr, drag_x0, drag_y0, ptr_x - drag_x0, ptr_y - drag_y0);
        cairo_stroke(cr);
        cairo_restore(cr);
    } else if (selpress && brush_mode && !brush_pts.empty()) {
        // 笔刷拖选预览：半透明圆头笔画带
        cairo_save(cr);
        set_color(cr, hex_color(PAL_BRASS), 0.30);
        cairo_set_line_width(cr, brush_r * 2);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
        cairo_move_to(cr, brush_pts[0].x, brush_pts[0].y);
        if (brush_pts.size() == 1)
            cairo_line_to(cr, brush_pts[0].x + 0.01, brush_pts[0].y);
        for (size_t i = 1; i < brush_pts.size(); i++)
            cairo_line_to(cr, brush_pts[i].x, brush_pts[i].y);
        cairo_stroke(cr);
        cairo_restore(cr);
    } else if (selpress) {
        const double dash[2] = {3, 3};
        cairo_save(cr);
        cairo_set_dash(cr, dash, 2, 0);
        set_color(cr, hex_color(PAL_BRASS));
        cairo_set_line_width(cr, 1);
        cairo_rectangle(cr, sel_x0, sel_y0, ptr_x - sel_x0, ptr_y - sel_y0);
        cairo_stroke(cr);
        cairo_restore(cr);
    }

    draw_footer(cr, cw, ch);

    // 轻提示
    if (!toast.empty()) {
        rr_path(cr, cw / 2.0 - 90, 12, cw / 2.0 + 90, 40, 8);
        set_color(cr, hex_color(PAL_PANEL));
        cairo_fill_preserve(cr);
        set_color(cr, hex_color(PAL_BRASS_DIM));
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
        PangoLayout* tl = pango_cairo_create_layout(cr);
        pango_layout_set_font_description(tl, role_font("ui", 13));
        pango_layout_set_text(tl, toast.c_str(), -1);
        int tw = 0, th = 0;
        pango_layout_get_pixel_size(tl, &tw, &th);
        set_color(cr, hex_color(PAL_PAPER));
        cairo_move_to(cr, cw / 2.0 - tw / 2.0, 26 - th / 2.0);
        pango_cairo_show_layout(cr, tl);
        g_object_unref(tl);
    }

    prefetch();
}

// ================= 翻页 / 滚动 / 预取 =================
std::string cut_chars(const std::string& s, int n) {
    std::vector<size_t> b = utf8_bounds(s);
    if ((int)b.size() - 1 <= n) return s;
    return s.substr(0, b[(size_t)n]);
}

void toc_clear(GtkWidget* box) {
    GList* rows = gtk_container_get_children(GTK_CONTAINER(box));
    for (GList* l = rows; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(rows);
}

void App::load_toc() {
    if (!toc_box) return;
    for (const caj::TocItem& it : caj::caj_toc(cap)) {
        std::string ind;
        for (int k = 1; k < it.level; k++) ind += "   ";
        char b[64];
        snprintf(b, sizeof(b), "  · p%d", it.page);
        std::string txt = ind + it.title + b;
        GtkWidget* lbl = gtk_label_new(txt.c_str());
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
        GtkWidget* row = gtk_list_box_row_new();
        gtk_container_add(GTK_CONTAINER(row), lbl);
        g_object_set_data(G_OBJECT(row), "page", GINT_TO_POINTER(it.page));
        gtk_list_box_insert(GTK_LIST_BOX(toc_box), row, -1);
    }
    gtk_widget_show_all(toc_box);
}

void App::set_page(int n, bool at_bottom) {
    cur = std::max(1, std::min(N, n));
    if (page_lbl) {
        char b[64];
        snprintf(b, sizeof(b), "%d / %d", cur, N);
        gtk_label_set_text(GTK_LABEL(page_lbl), b);
    }
    if (toc_box && gtk_list_box_get_row_at_index(GTK_LIST_BOX(toc_box), 0) == nullptr)
        load_toc();
    // 翻页定位到页首；at_bottom 时先给超大偏移，由 draw 按实际高度钳到页底
    scroll = at_bottom ? 1e9 : 0;
    cancel_drag();
    queue_draw();
    request_page(cur);
}

void App::wheel_page(int step) {
    // 翻页后短暂保护窗口内忽略滚轮，避免高精度滚轮/触摸板的尾随事件
    double now = g_get_monotonic_time() / 1e6;
    if (now < flip_until) return;
    real_wheel(step);
}

void App::real_wheel(int step) {
    if (!cur_ready) return;
    double dh = cur_dh;
    double ch = std::max(gtk_widget_get_allocated_height(canvas), 50);
    double ms = std::max(0.0, dh - (ch - 44));
    double step_px = std::max(30.0, dh / 12.0);
    if (step < 0) {                              // 上滑
        if (scroll > 0) {                        // 页内还有上方内容：上移
            scroll = std::max(0.0, scroll - step_px);
            queue_draw();
        } else if (cur > 1) {                    // 已在页首：翻上一页并定位到其底部
            set_page(cur - 1, true);
            flip_until = g_get_monotonic_time() / 1e6 + 0.15;
        }
    } else {                                     // 下滑
        if (scroll < ms) {                       // 未越过页底：下移
            scroll = std::min(ms, scroll + step_px);
            queue_draw();
        } else if (cur < N) {                    // 已越过页底：翻下一页（定位页首）
            set_page(cur + 1);
            flip_until = g_get_monotonic_time() / 1e6 + 0.15;
        } else {
            queue_draw();
        }
    }
}

void App::prefetch() {
    if (!rd) return;
    int cand[2] = {cur - 1, cur + 1};
    for (int m : cand) {
        if (m >= 1 && m <= N && !rd->get(m))
            rd->get_async(m, [](int, caj::PagePtr) {});
    }
}

void App::request_page(int n) {
    if (!rd || n <= 0 || pending_page == n) return;
    pending_page = n;
    rd->get_async(n, [this](int pn, caj::PagePtr pg) {
        pending_page = 0;      // 失败（pg 为空）时允许后续重试
        on_page_ready(pn, pg);
    });
}

void App::on_page_ready(int n, caj::PagePtr pg) {
    if (!pg || n != cur) return;
    queue_draw();
    prefetch();
}

// ================= 打开文件 / 格式信息 =================
void show_error(GtkWindow* parent, const std::string& msg) {
    GtkWidget* d = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL,
                                          GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s",
                                          msg.c_str());
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

void App::open_file() {
    GtkWidget* d = gtk_file_chooser_dialog_new(
        "打开文档", GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_OPEN, "取消",
        GTK_RESPONSE_CANCEL, "打开", GTK_RESPONSE_ACCEPT, nullptr);
    GtkFileFilter* f1 = gtk_file_filter_new();
    gtk_file_filter_set_name(f1, "CAJ/知网文档 (*.caj *.kdh *.hn *.nh)");
    for (const char* pat : {"*.caj", "*.kdh", "*.hn", "*.nh"})
        gtk_file_filter_add_pattern(f1, pat);
    GtkFileFilter* f2 = gtk_file_filter_new();
    gtk_file_filter_set_name(f2, "PDF 文档 (*.pdf)");
    gtk_file_filter_add_pattern(f2, "*.pdf");
    GtkFileFilter* f3 = gtk_file_filter_new();
    gtk_file_filter_set_name(f3, "所有文件");
    gtk_file_filter_add_pattern(f3, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(d), f1);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(d), f2);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(d), f3);
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) {
        char* p = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(d));
        std::string sp = p ? p : "";
        g_free(p);
        gtk_widget_destroy(d);
        if (!sp.empty()) open_path(sp);
    } else {
        gtk_widget_destroy(d);
    }
}

// 后台渲染完成唤醒（定义在后文；Renderer 的唤醒钩子需引用它）
gboolean drain_render_cb(gpointer ud);

void App::open_path(const std::string& p) {
    GError* err = nullptr;
    char* tdp = g_dir_make_tmp("cajview_XXXXXX", &err);
    if (err) g_error_free(err);
    if (!tdp) {
        show_error(GTK_WINDOW(win), "无法创建临时目录");
        return;
    }
    std::string ntmp = tdp;
    g_free(tdp);
    caj::Info ncap;
    std::vector<unsigned char> npdf;
    std::string e;
    if (!caj::prepare_doc(p, ncap, npdf, e)) {
        rm_rf(ntmp);
        show_error(GTK_WINDOW(win),
                   "无法打开 " + base_name(p) + "：\n" + e);
        return;
    }
    caj::Renderer* nrd = nullptr;
    try {
        nrd = new caj::Renderer(npdf, ntmp);
    } catch (...) {
        rm_rf(ntmp);
        show_error(GTK_WINDOW(win), "无法初始化渲染器：" + base_name(p));
        return;
    }
    // 渲染完成时从后台线程唤醒主循环（替代 60ms 轮询）
    nrd->set_wakeup([this] { g_main_context_invoke(nullptr, drain_render_cb, this); });
    std::string old_tmp = tmp;
    path = p;
    cap = ncap;
    pdf.swap(npdf);
    delete rd;
    rd = nrd;
    tmp = ntmp;
    N = caj::caj_page_num(cap);
    if (N <= 0) N = caj::pdf_page_count(rd->path());
    if (N <= 0) N = 1;
    cur = 1;
    zoom = 1.0;
    scroll = 0;
    max_scroll = 0;
    flip_until = 0;
    pending_page = 0;
    // 旋转仅本次会话，不持久化：换文档即恢复原始方向
    rot_all = 0;
    rot_page.clear();
    cancel_drag();
    has_sel = false;
    sel_page = 0;
    sel_is_brush = false;
    sel_path.clear();
    if (psurf) {
        cairo_surface_destroy(psurf);
        psurf = nullptr;
    }
    ps_page = -1;
    ps_obj.reset();
    bump_rev();
    load_annotations();
    add_recent(p);   // 成功打开：记入最近列表（去重置顶，最多 5 条）
    std::string t = "CAJ 阅读器 - " + base_name(p);
    gtk_window_set_title(GTK_WINDOW(win), t.c_str());
    if (title_lbl)
        gtk_label_set_text(GTK_LABEL(title_lbl), cut_chars(base_name(p), 36).c_str());
    if (toc_box) toc_clear(toc_box);
    set_page(1);
    if (!old_tmp.empty()) rm_rf(old_tmp);
}

void App::show_info() {
    if (!rd) {                    // 空界面：还没有可分析的文档
        toast_msg("请先打开文档");
        return;
    }
    std::string txt = caj::analyze_text(cap);
    GtkWidget* w = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(w), "格式分析");
    gtk_window_set_default_size(GTK_WINDOW(w), 640, 560);
    if (win) gtk_window_set_transient_for(GTK_WINDOW(w), GTK_WINDOW(win));
    GtkWidget* sw = gtk_scrolled_window_new(nullptr, nullptr);
    GtkWidget* tv = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(tv), TRUE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(tv), 14);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(tv), 10);
    GtkTextBuffer* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
    gtk_text_buffer_set_text(buf, txt.c_str(), -1);
    gtk_container_add(GTK_CONTAINER(sw), tv);
    gtk_container_add(GTK_CONTAINER(w), sw);
    gtk_widget_show_all(w);
}

// ================= 标记命中/菜单 =================
Annotation* App::anno_at(double x, double y) {
    if (!cur_ready) return nullptr;
    int cw = gtk_widget_get_allocated_width(canvas);
    int ch = gtk_widget_get_allocated_height(canvas);
    double nx = 0, ny = 0;
    norm_from_canvas(cw, ch, x, y, &nx, &ny);
    // 后画的优先
    for (auto it = annos.rbegin(); it != annos.rend(); ++it) {
        if (it->page != cur) continue;
        if (it->rect[0] <= nx && nx <= it->rect[2] && it->rect[1] <= ny &&
            ny <= it->rect[3])
            return &*it;
    }
    return nullptr;
}

struct AnnoAction {
    App* app;
    std::string id;
    std::string color;
    bool del;
};

void anno_action_cb(GtkMenuItem*, gpointer ud) {
    AnnoAction* a = (AnnoAction*)ud;
    for (auto it = a->app->annos.begin(); it != a->app->annos.end(); ++it) {
        if (it->id != a->id) continue;
        if (a->del) a->app->annos.erase(it);
        else it->color = a->color;
        break;
    }
    a->app->save_annotations();
    a->app->bump_rev();
    a->app->queue_draw();
}

GtkWidget* action_item(App* app, const std::string& id, const std::string& color,
                       bool del, const char* label) {
    GtkWidget* it = gtk_menu_item_new();
    GtkWidget* lbl = gtk_label_new(nullptr);
    if (color.empty()) {
        gtk_label_set_text(GTK_LABEL(lbl), label);
    } else {
        char mk[128];
        snprintf(mk, sizeof(mk), "<span foreground=\"%s\">%s</span>", color.c_str(),
                 label);
        gtk_label_set_markup(GTK_LABEL(lbl), mk);
    }
    gtk_container_add(GTK_CONTAINER(it), lbl);
    AnnoAction* act = new AnnoAction{app, id, color, del};
    g_signal_connect_data(
        it, "activate", G_CALLBACK(anno_action_cb), act,
        [](gpointer d, GClosure*) { delete (AnnoAction*)d; }, (GConnectFlags)0);
    return it;
}

void App::anno_menu(Annotation* a, double, double) {
    if (!a) return;
    std::string id = a->id;   // 用 id 定位，避免容器变化导致悬垂指针
    GtkWidget* m = gtk_menu_new();
    for (int i = 0; i < 4; i++) {
        char lbl[64];
        snprintf(lbl, sizeof(lbl), "改色 · %s", MARK_NAME[i]);
        gtk_menu_shell_append(GTK_MENU_SHELL(m),
                              action_item(this, id, MARK_HEX[i], false, lbl));
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(m), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(m),
                          action_item(this, id, "", true, "删除标记"));
    g_signal_connect_swapped(m, "selection-done", G_CALLBACK(gtk_widget_destroy), m);
    gtk_widget_show_all(m);
    gtk_menu_popup_at_pointer(GTK_MENU(m), nullptr);
}

// ================= 文本选区 =================
std::vector<std::vector<App::Span>> App::sel_spans(const caj::PageLayout& lay) {
    std::vector<std::vector<Span>> lines;
    if (!has_sel || sel_page != cur) return lines;
    int cw = gtk_widget_get_allocated_width(canvas);
    double sc = display_scale(lay.w, cw);
    // 笔刷：命中 = 字符中心到笔画路径距离 ≤ 半径；矩形：x0/x1/y0/y1 包围盒
    double br = sel_r * lay.w * sc;              // 半径（缩放后页面坐标）
    std::vector<brush::Point> bpath;             // 笔画（缩放后页面坐标）
    double bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    if (sel_is_brush) {
        for (const brush::Point& p : sel_path) {
            double px = p.x * lay.w * sc, py = p.y * lay.h * sc;
            if (bpath.empty()) {
                bx0 = bx1 = px;
                by0 = by1 = py;
            } else {
                bx0 = std::min(bx0, px);
                bx1 = std::max(bx1, px);
                by0 = std::min(by0, py);
                by1 = std::max(by1, py);
            }
            bpath.push_back({px, py});
        }
        if (bpath.empty()) return lines;
    }
    double x0 = sel[0] * lay.w * sc, x1 = sel[2] * lay.w * sc;
    double y0 = sel[1] * lay.h * sc, y1 = sel[3] * lay.h * sc;
    std::vector<Span> spans;
    for (size_t ri = 0; ri < lay.texts.size(); ri++) {
        const caj::TextRun& tr = lay.texts[ri];
        double mid = tr.y * sc + tr.h * sc / 2;      // 段中线
        double ty0 = tr.y * sc, ty1 = (tr.y + tr.h) * sc;
        if (sel_is_brush) {
            // 包围盒外扩半径的粗筛，省掉逐 run 的 Pango 度量
            double rx0 = tr.x * sc, rx1 = (tr.x + tr.w) * sc;
            if (bx1 + br < rx0 || bx0 - br > rx1 || by1 + br < ty0 ||
                by0 - br > ty1)
                continue;
        } else if (!((y0 <= mid && mid <= y1) ||
                     (std::min(y1, ty1) - std::max(y0, ty0) >=
                      0.5 * tr.h * sc))) {
            continue;
        }
        auto fit = lay.fonts.find(tr.font);
        caj::FontSpec fs = fit != lay.fonts.end() ? fit->second : caj::FontSpec();
        RunGeom g;
        run_geom(tr, fs, sc, g);
        if (sel_is_brush) {
            std::vector<double> mids(g.xs0.size());
            for (size_t i = 0; i < g.xs0.size(); i++)
                mids[i] = (g.xs0[i] + g.xs1[i]) / 2;
            for (const auto& rg : brush::brush_ranges(mids, mid, bpath, br))
                spans.push_back(Span{ri, g.bounds[rg.first], g.bounds[rg.second]});
            continue;
        }
        long i0 = -1, i1 = -1;
        for (size_t i = 0; i < g.xs0.size(); i++) {
            double cx = (g.xs0[i] + g.xs1[i]) / 2;   // 字符中点落在选区内
            if (x0 <= cx && cx <= x1) {
                if (i0 < 0) i0 = (long)i;
                i1 = (long)i + 1;
            }
        }
        if (i0 >= 0)
            spans.push_back(Span{ri, g.bounds[(size_t)i0], g.bounds[(size_t)i1]});
    }
    std::sort(spans.begin(), spans.end(), [&](const Span& a, const Span& b) {
        const caj::TextRun& ta = lay.texts[a.run];
        const caj::TextRun& tb = lay.texts[b.run];
        if (ta.y != tb.y) return ta.y < tb.y;
        return ta.x < tb.x;
    });
    for (const Span& s : spans) {                    // 按段中线归入同一行
        const caj::TextRun& tr = lay.texts[s.run];
        if (!lines.empty()) {
            const caj::TextRun& pt = lay.texts[lines.back().front().run];
            if (std::fabs((tr.y + tr.h / 2) - (pt.y + pt.h / 2)) <=
                0.5 * std::max(tr.h, pt.h)) {
                lines.back().push_back(s);
                continue;
            }
        }
        lines.push_back({s});
    }
    for (auto& ln : lines)
        std::sort(ln.begin(), ln.end(), [&](const Span& a, const Span& b) {
            return lay.texts[a.run].x < lay.texts[b.run].x;
        });
    return lines;
}

std::string App::sel_text() {
    caj::PagePtr obj = rd ? rd->get(cur) : nullptr;
    if (!obj || !obj->layout || !has_sel) return "";
    const caj::PageLayout& lay = *obj->layout;
    std::vector<std::string> parts;
    for (const auto& ln : sel_spans(lay)) {
        std::string buf;
        const caj::TextRun* prev = nullptr;
        size_t pi1 = 0;
        for (const Span& sp : ln) {
            const caj::TextRun& tr = lay.texts[sp.run];
            if (prev && pi1 >= prev->text.size() && sp.b0 == 0) {
                // 段间隙明显大于字距时补空格
                double gap = tr.x - (prev->x + prev->w);
                auto fit = lay.fonts.find(tr.font);
                double size = fit != lay.fonts.end() ? fit->second.size : 12.0;
                if (size == 0) size = 12;
                bool ends_sp = !buf.empty() && buf[buf.size() - 1] == ' ';
                bool starts_sp = !tr.text.empty() && tr.text[0] == ' ';
                if (gap > 0.6 * size && !ends_sp && !starts_sp) buf += " ";
            }
            buf += cjk_tight(utf8_substr(tr.text, sp.b0, sp.b1));
            prev = &tr;
            pi1 = sp.b1;
        }
        std::string t = utf8_trim(buf);
        if (!t.empty()) parts.push_back(t);
    }
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i) out += "\n";
        out += parts[i];
    }
    return out;
}

void App::copy_selection() {
    std::string t = sel_text();
    if (t.empty()) {
        toast_msg("未选中文本");
        return;
    }
    GtkClipboard* cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text(cb, t.c_str(), (int)t.size());
    std::string no_nl;
    for (char c : t)
        if (c != '\n') no_nl.push_back(c);
    char b[64];
    snprintf(b, sizeof(b), "已复制 %d 字", utf8_len(no_nl));
    toast_msg(b);
}

void App::clear_selection() {
    if (!has_sel) return;
    has_sel = false;
    sel_is_brush = false;
    sel_path.clear();
    bump_rev();
    queue_draw();
}

struct SelAction {
    App* app;
    bool copy;
};

void sel_action_cb(GtkMenuItem*, gpointer ud) {
    SelAction* s = (SelAction*)ud;
    if (s->copy) s->app->copy_selection();
    else s->app->clear_selection();
}

void App::sel_menu(double, double) {
    if (!has_sel || sel_page != cur || sel_text().empty()) return;
    GtkWidget* m = gtk_menu_new();
    SelAction* c = new SelAction{this, true};
    GtkWidget* i1 = gtk_menu_item_new_with_label("复制选中文本（Ctrl+C）");
    g_signal_connect_data(
        i1, "activate", G_CALLBACK(sel_action_cb), c,
        [](gpointer d, GClosure*) { delete (SelAction*)d; }, (GConnectFlags)0);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), i1);
    SelAction* e = new SelAction{this, false};
    GtkWidget* i2 = gtk_menu_item_new_with_label("清除选区（Esc）");
    g_signal_connect_data(
        i2, "activate", G_CALLBACK(sel_action_cb), e,
        [](gpointer d, GClosure*) { delete (SelAction*)d; }, (GConnectFlags)0);
    gtk_menu_shell_append(GTK_MENU_SHELL(m), i2);
    g_signal_connect_swapped(m, "selection-done", G_CALLBACK(gtk_widget_destroy), m);
    gtk_widget_show_all(m);
    gtk_menu_popup_at_pointer(GTK_MENU(m), nullptr);
}

// ================= GTK 事件 =================
void update_cursor(App* a) {
    GdkWindow* w = gtk_widget_get_window(a->canvas);
    if (!w) return;
    GdkCursor* c = gdk_cursor_new_for_display(
        gdk_display_get_default(), a->mark_on ? GDK_CROSSHAIR
                                  : (a->brush_mode ? GDK_PENCIL : GDK_XTERM));
    gdk_window_set_cursor(w, c);
    g_object_unref(c);
}

void App::toggle_brush() {
    brush_mode = !brush_mode;
    update_cursor(this);
    toast_msg(brush_mode ? "笔刷选择模式（b 切回矩形）" : "矩形选择模式");
    queue_draw();
}

void App::brush_adjust(double d) {
    brush_r = std::max(3.0, std::min(40.0, brush_r + d));
    char b[48];
    snprintf(b, sizeof(b), "笔刷半径 %.0f px", brush_r);
    toast_msg(b);
}

gboolean on_draw_cb(GtkWidget* w, cairo_t* cr, gpointer ud) {
    App* a = (App*)ud;
    a->draw(cr, gtk_widget_get_allocated_width(w), gtk_widget_get_allocated_height(w));
    return TRUE;
}

gboolean on_press_cb(GtkWidget*, GdkEventButton* ev, gpointer ud) {
    App* a = (App*)ud;
    if (ev->button == 1) {
        if (!a->rd) {                         // 欢迎界面：点击最近文件行即打开
            for (const RecentHit& h : a->recent_hits) {
                if (ev->x >= h.x0 && ev->x <= h.x1 && ev->y >= h.y0 &&
                    ev->y <= h.y1) {
                    a->open_path(h.path);
                    return TRUE;
                }
            }
            return FALSE;
        }
        if (a->mark_on) {                     // 标记模式：拖画高亮
            if (!a->cur_ready) { a->dragging = false; return TRUE; }
            a->selpress = false;
            a->dragging = true;
            a->drag_x0 = a->ptr_x = ev->x;
            a->drag_y0 = a->ptr_y = ev->y;
            return TRUE;
        }
        a->dragging = false;                  // 普通模式：拖选文字
        a->selpress = a->cur_ready;
        a->sel_x0 = a->ptr_x = ev->x;
        a->sel_y0 = a->ptr_y = ev->y;
        if (a->brush_mode) a->brush_pts.assign(1, {ev->x, ev->y});
        return TRUE;
    }
    if (ev->button == 3) {
        a->sel_menu(ev->x, ev->y);
        return TRUE;
    }
    return FALSE;
}

gboolean on_motion_cb(GtkWidget*, GdkEventMotion* ev, gpointer ud) {
    App* a = (App*)ud;
    if (!a->dragging && !a->selpress) return FALSE;
    a->ptr_x = ev->x;
    a->ptr_y = ev->y;
    if (a->selpress && a->brush_mode && !a->brush_pts.empty()) {
        const brush::Point& lp = a->brush_pts.back();   // 采样间距 ≥2px
        if (std::hypot(ev->x - lp.x, ev->y - lp.y) >= 2.0)
            a->brush_pts.push_back({ev->x, ev->y});
    }
    a->queue_draw();
    return TRUE;
}

gboolean on_release_cb(GtkWidget*, GdkEventButton* ev, gpointer ud) {
    App* a = (App*)ud;
    if (ev->button != 1) return FALSE;
    if (!a->cur_ready) {
        a->cancel_drag();
        return TRUE;
    }
    int cw = gtk_widget_get_allocated_width(a->canvas);
    int ch = gtk_widget_get_allocated_height(a->canvas);
    if (a->mark_on) {
        if (!a->dragging) return TRUE;
        double sx = a->drag_x0, sy = a->drag_y0;
        a->dragging = false;
        // 位移过小视为点击：命中已有标记则弹出换色/删除菜单
        if (std::fabs(ev->x - sx) < 6 && std::fabs(ev->y - sy) < 6) {
            Annotation* hit = a->anno_at(ev->x, ev->y);
            if (hit) a->anno_menu(hit, ev->x, ev->y);
            a->queue_draw();
            return TRUE;
        }
        double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        a->norm_from_canvas(cw, ch, sx, sy, &x0, &y0);
        a->norm_from_canvas(cw, ch, ev->x, ev->y, &x1, &y1);
        double nx0 = std::min(x0, x1), nx1 = std::max(x0, x1);
        double ny0 = std::min(y0, y1), ny1 = std::max(y0, y1);
        if ((nx1 - nx0) * (ny1 - ny0) < 0.0005) {   // 过滤误触
            a->queue_draw();
            return TRUE;
        }
        Annotation an;
        an.page = a->cur;
        an.rect[0] = nx0;
        an.rect[1] = ny0;
        an.rect[2] = nx1;
        an.rect[3] = ny1;
        an.color = a->mark_color;
        an.id = uuid_hex().substr(0, 12);
        an.created = (double)g_get_real_time() / 1e6;
        a->annos.push_back(an);
        a->save_annotations();
        a->bump_rev();
        a->queue_draw();
        return TRUE;
    }
    // 普通模式：落定文字选区
    if (!a->selpress) return TRUE;
    double sx = a->sel_x0, sy = a->sel_y0;
    a->selpress = false;
    if (std::fabs(ev->x - sx) < 5 && std::fabs(ev->y - sy) < 5) {   // 单击：清除
        a->brush_pts.clear();
        a->clear_selection();
        return TRUE;
    }
    caj::PagePtr obj = a->rd ? a->rd->get(a->cur) : nullptr;
    if (!obj || !obj->layout) {
        a->brush_pts.clear();
        a->toast_msg("此页为扫描图像，无可选文字");
        return TRUE;
    }
    a->has_sel = true;
    a->sel_page = a->cur;
    if (a->brush_mode) {
        // 笔刷：笔画存归一化页面坐标，半径按页宽比存（缩放不变）
        const caj::PageLayout& lay = *obj->layout;
        double sc = a->display_scale(lay.w, cw);
        a->sel_is_brush = true;
        a->sel_path.clear();
        for (const brush::Point& p : a->brush_pts) {
            double nx = 0, ny = 0;
            a->norm_from_canvas(cw, ch, p.x, p.y, &nx, &ny);
            a->sel_path.push_back({nx, ny});
        }
        a->sel_r = a->brush_r / (lay.w * sc);
        a->brush_pts.clear();
    } else {
        a->sel_is_brush = false;
        a->sel_path.clear();
        double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        a->norm_from_canvas(cw, ch, sx, sy, &x0, &y0);
        a->norm_from_canvas(cw, ch, ev->x, ev->y, &x1, &y1);
        a->sel[0] = std::min(x0, x1);
        a->sel[1] = std::min(y0, y1);
        a->sel[2] = std::max(x0, x1);
        a->sel[3] = std::max(y0, y1);
    }
    a->bump_rev();
    a->queue_draw();
    return TRUE;
}

gboolean on_scroll_cb(GtkWidget*, GdkEventScroll* ev, gpointer ud) {
    App* a = (App*)ud;
    if (ev->direction == GDK_SCROLL_UP) {
        a->wheel_page(-1);
        return TRUE;
    }
    if (ev->direction == GDK_SCROLL_DOWN) {
        a->wheel_page(1);
        return TRUE;
    }
    if (ev->direction == GDK_SCROLL_SMOOTH) {
        double dx = 0, dy = 0;
        if (gdk_event_get_scroll_deltas((GdkEvent*)ev, &dx, &dy) && dy != 0) {
            // 平滑滚动按累积量触发，避免一个手势翻出多页
            a->smooth_acc += dy;
            while (a->smooth_acc <= -0.5) {
                a->smooth_acc += 0.5;
                a->wheel_page(-1);
            }
            while (a->smooth_acc >= 0.5) {
                a->smooth_acc -= 0.5;
                a->wheel_page(1);
            }
        }
        return TRUE;
    }
    return FALSE;
}

gboolean on_key_cb(GtkWidget*, GdkEventKey* ev, gpointer ud) {
    App* a = (App*)ud;
    bool ctrl = (ev->state & GDK_CONTROL_MASK) != 0;
    switch (ev->keyval) {
        case GDK_KEY_Left:
        case GDK_KEY_Prior:
            a->prev();
            return TRUE;
        case GDK_KEY_Right:
        case GDK_KEY_Next:
            a->next();
            return TRUE;
        case GDK_KEY_plus:
        case GDK_KEY_equal:
        case GDK_KEY_KP_Add:
            a->zoom_in();
            return TRUE;
        case GDK_KEY_minus:
        case GDK_KEY_KP_Subtract:
            a->zoom_out();
            return TRUE;
        case GDK_KEY_r:
            a->rotate_page();
            return TRUE;
        case GDK_KEY_R:
            a->rotate_all_pages();
            return TRUE;
        case GDK_KEY_b:
        case GDK_KEY_B:
            a->toggle_brush();
            return TRUE;
        case GDK_KEY_bracketleft:
            a->brush_adjust(-2);
            return TRUE;
        case GDK_KEY_bracketright:
            a->brush_adjust(2);
            return TRUE;
        case GDK_KEY_Escape:
            a->clear_selection();
            return TRUE;
        case GDK_KEY_o:
        case GDK_KEY_O:
            if (ctrl) {
                a->open_file();
                return TRUE;
            }
            break;
        case GDK_KEY_c:
        case GDK_KEY_C:
            if (ctrl) {
                a->copy_selection();
                return TRUE;
            }
            break;
        default:
            break;
    }
    return FALSE;
}

void on_toc_row(GtkListBox*, GtkListBoxRow* row, gpointer ud) {
    App* a = (App*)ud;
    int page = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "page"));
    a->set_page(std::max(1, std::min(page, a->N)));
}

void on_toc_toggle(GtkToggleButton* b, gpointer ud) {
    App* a = (App*)ud;
    if (a->toc_sw)
        gtk_widget_set_visible(a->toc_sw,
                               gtk_toggle_button_get_active(b));
}

void on_mark_toggle(GtkToggleButton* b, gpointer ud) {
    App* a = (App*)ud;
    a->mark_on = gtk_toggle_button_get_active(b);
    a->cancel_drag();
    update_cursor(a);
    a->queue_draw();
}

void on_mark_color(GtkMenuItem* item, gpointer ud) {
    App* a = (App*)ud;
    const char* hex = (const char*)g_object_get_data(G_OBJECT(item), "hex");
    if (!hex) return;
    a->mark_color = hex;
    GtkStyleContext* sc = gtk_widget_get_style_context(a->mark_btn);
    if (!a->mark_css) {
        a->mark_css = gtk_css_provider_new();
        gtk_style_context_add_provider(sc, GTK_STYLE_PROVIDER(a->mark_css),
                                       GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    }
    char sheet[128];
    snprintf(sheet, sizeof(sheet),
             "button { background-color: %s; background-image: none;"
             " color: #2C2821; font-weight: bold; }",
             hex);
    gtk_css_provider_load_from_data(a->mark_css, sheet, -1, nullptr);
    a->queue_draw();
}

// 后台渲染结果收割：由渲染线程经 g_main_context_invoke 投递到主线程执行一次。
gboolean drain_render_cb(gpointer ud) {
    App* a = (App*)ud;
    if (!a->rd) return G_SOURCE_REMOVE;
    for (auto& t : a->rd->drain()) {
        try {
            std::get<0>(t)(std::get<1>(t), std::get<2>(t));
        } catch (...) {
        }
    }
    return G_SOURCE_REMOVE;
}

// 已带文件路径启动：界面出现后再加载文档
gboolean open_initial_cb(gpointer ud) {
    App* a = (App*)ud;
    a->open_path(a->path);
    return G_SOURCE_REMOVE;
}

// 无参启动：界面出现后弹出文件选择对话框
gboolean choose_file_cb(gpointer ud) {
    App* a = (App*)ud;
    a->open_file();
    return G_SOURCE_REMOVE;
}

void on_destroy_cb(GtkWidget*, gpointer) {
    gtk_main_quit();
}

// ================= 界面搭建 =================
void build_ui(App* a) {
    GtkCssProvider* css = gtk_css_provider_new();
    static const char* sheet =
        "window { background-color: #232B31; }\n"
        ".toolbar { background-color: #2E3942; }\n"
        ".sidebar { background-color: #2E3942; }\n"
        "button { background-image: none; background-color: #2E3942;"
        "  color: #F6F1E4; border: 1px solid #3D4A54; padding: 5px 12px;"
        "  font-size: 10pt; }\n"
        "button:hover { background-color: #3D4A54; }\n"
        "button:active, button:checked { background-color: #C8A43A;"
        "  color: #2C2821; }\n"
        "checkbutton { color: #9AA69E; font-size: 10pt; }\n"
        "checkbutton:checked { color: #F6F1E4; }\n"
        ".title { color: #9AA69E; font-size: 10pt; }\n"
        ".pagelabel { background-color: #C8A43A; color: #2C2821;"
        "  font-weight: bold; padding: 3px 12px; font-size: 11pt; }\n"
        "list { background-color: #2E3942; color: #F6F1E4; font-size: 9pt; }\n"
        "list row { padding: 2px 4px; }\n"
        "list row:selected { background-color: #C8A43A; color: #2C2821; }\n"
        "separator { background-color: #3D4A54; color: #3D4A54; }\n"
        "menu, menuitem { background-color: #2E3942; color: #F6F1E4; }\n"
        "menuitem:hover { background-color: #C8A43A; }\n"
        "textview, textview text { background-color: #232B31;"
        "  color: #F6F1E4; font-size: 10pt; }\n";
    gtk_css_provider_load_from_data(css, sheet, -1, nullptr);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    bool has_doc = !a->path.empty();
    a->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(
        GTK_WINDOW(a->win),
        (has_doc ? "CAJ 阅读器 - " + base_name(a->path) : std::string("CAJ 阅读器"))
            .c_str());
    gtk_window_set_default_size(GTK_WINDOW(a->win), 800, 600);
    gtk_widget_set_size_request(a->win, 640, 480);

    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(a->win), vbox);

    // ---- 工具栏 ----
    GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_style_context_add_class(gtk_widget_get_style_context(bar), "toolbar");
    gtk_box_pack_start(GTK_BOX(vbox), bar, FALSE, FALSE, 0);
    a->title_lbl = gtk_label_new(has_doc ? cut_chars(base_name(a->path), 36).c_str()
                                         : "未打开文档");
    gtk_style_context_add_class(gtk_widget_get_style_context(a->title_lbl), "title");
    gtk_box_pack_start(GTK_BOX(bar), a->title_lbl, FALSE, FALSE, 6);
    gtk_box_pack_start(GTK_BOX(bar),
                       gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(bar), gtk_label_new("   "), FALSE, FALSE, 0);

    GtkWidget* b = gtk_button_new_with_label("打开");
    g_signal_connect_swapped(b, "clicked", G_CALLBACK(+[](App* p) { p->open_file(); }),
                             a);
    gtk_box_pack_start(GTK_BOX(bar), b, FALSE, FALSE, 0);
    const std::pair<const char*, void (*)(App*)> navs[] = {
        {"首页", [](App* p) { p->first(); }}, {"上一页", [](App* p) { p->prev(); }},
        {"下一页", [](App* p) { p->next(); }}, {"末页", [](App* p) { p->last(); }},
    };
    for (const auto& n : navs) {
        b = gtk_button_new_with_label(n.first);
        g_signal_connect_swapped(b, "clicked", G_CALLBACK(n.second), a);
        gtk_box_pack_start(GTK_BOX(bar), b, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(bar),
                       gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 4);
    const std::pair<const char*, void (*)(App*)> tools[] = {
        {"放大", [](App* p) { p->zoom_in(); }},
        {"缩小", [](App* p) { p->zoom_out(); }},
        {"旋转本页", [](App* p) { p->rotate_page(); }},
        {"旋转全部", [](App* p) { p->rotate_all_pages(); }},
        {"格式信息", [](App* p) { p->show_info(); }},
    };
    for (const auto& n : tools) {
        b = gtk_button_new_with_label(n.first);
        g_signal_connect_swapped(b, "clicked", G_CALLBACK(n.second), a);
        gtk_box_pack_start(GTK_BOX(bar), b, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(bar),
                       gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 4);
    // 标记模式 + 颜色选择
    GtkWidget* mk = gtk_check_button_new_with_label("标记模式");
    g_signal_connect(mk, "toggled", G_CALLBACK(on_mark_toggle), a);
    gtk_box_pack_start(GTK_BOX(bar), mk, FALSE, FALSE, 0);
    a->mark_btn = gtk_menu_button_new();
    gtk_widget_set_name(a->mark_btn, "markbtn");
    {
        GtkWidget* lbl = gtk_label_new("色");
        gtk_container_add(GTK_CONTAINER(a->mark_btn), lbl);
        GtkWidget* menu = gtk_menu_new();
        for (int i = 0; i < 4; i++) {
            GtkWidget* it = gtk_menu_item_new();
            GtkWidget* li = gtk_label_new(nullptr);
            char markup[96];
            snprintf(markup, sizeof(markup),
                     "<span foreground=\"%s\">%s</span>", MARK_HEX[i], MARK_NAME[i]);
            gtk_label_set_markup(GTK_LABEL(li), markup);
            gtk_container_add(GTK_CONTAINER(it), li);
            g_object_set_data(G_OBJECT(it), "hex", (gpointer)MARK_HEX[i]);
            g_signal_connect(it, "activate", G_CALLBACK(on_mark_color), a);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
        }
        gtk_widget_show_all(menu);
        gtk_menu_button_set_popup(GTK_MENU_BUTTON(a->mark_btn), menu);
        gtk_widget_set_tooltip_text(a->mark_btn, "高亮颜色");
    }
    gtk_box_pack_start(GTK_BOX(bar), a->mark_btn, FALSE, FALSE, 4);
    // 黄铜书签页码（右对齐）
    a->page_lbl = gtk_label_new(has_doc ? "1 / 1" : "– / –");
    gtk_style_context_add_class(gtk_widget_get_style_context(a->page_lbl), "pagelabel");
    gtk_box_pack_end(GTK_BOX(bar), a->page_lbl, FALSE, FALSE, 12);
    if (has_doc) {
        char bf[64];
        snprintf(bf, sizeof(bf), "%d / %d", a->cur, a->N);
        gtk_label_set_text(GTK_LABEL(a->page_lbl), bf);
    }

    // ---- 正文：目录侧栏 + 画布 ----
    GtkWidget* body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(vbox), body, TRUE, TRUE, 0);

    a->toc_frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_style_context_add_class(gtk_widget_get_style_context(a->toc_frame), "sidebar");
    gtk_box_pack_start(GTK_BOX(body), a->toc_frame, FALSE, FALSE, 0);
    GtkWidget* toc_chk = gtk_check_button_new_with_label("目录");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toc_chk), TRUE);
    g_signal_connect(toc_chk, "toggled", G_CALLBACK(on_toc_toggle), a);
    gtk_box_pack_start(GTK_BOX(a->toc_frame), toc_chk, FALSE, FALSE, 6);
    a->toc_sw = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_widget_set_size_request(a->toc_sw, 230, -1);
    gtk_widget_set_vexpand(a->toc_sw, TRUE);
    a->toc_box = gtk_list_box_new();
    gtk_container_add(GTK_CONTAINER(a->toc_sw), a->toc_box);
    g_signal_connect(a->toc_box, "row-activated", G_CALLBACK(on_toc_row), a);
    gtk_box_pack_start(GTK_BOX(a->toc_frame), a->toc_sw, TRUE, TRUE, 6);

    a->canvas = gtk_drawing_area_new();
    gtk_widget_set_can_focus(a->canvas, TRUE);
    gtk_widget_add_events(a->canvas,
                          GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                              GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK |
                              GDK_SMOOTH_SCROLL_MASK);
    g_signal_connect(a->canvas, "draw", G_CALLBACK(on_draw_cb), a);
    g_signal_connect(a->canvas, "button-press-event", G_CALLBACK(on_press_cb), a);
    g_signal_connect(a->canvas, "button-release-event", G_CALLBACK(on_release_cb), a);
    g_signal_connect(a->canvas, "motion-notify-event", G_CALLBACK(on_motion_cb), a);
    g_signal_connect(a->canvas, "scroll-event", G_CALLBACK(on_scroll_cb), a);
    gtk_box_pack_start(GTK_BOX(body), a->canvas, TRUE, TRUE, 0);

    g_signal_connect(a->win, "key-press-event", G_CALLBACK(on_key_cb), a);
    g_signal_connect(a->win, "destroy", G_CALLBACK(on_destroy_cb), a);
}

const char* USAGE =
    "用法: caj-viewer 文献.caj [--info]\n"
    "      caj-viewer            （先进界面，再弹出文件选择对话框）\n"
    "  --info  仅打印格式分析，不打开阅读窗口\n";

}  // namespace

int main(int argc, char** argv) {
    std::string path;
    bool info_only = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--info")
            info_only = true;
        else if (!a.empty() && a[0] == '-')
            continue;                     // 未知选项忽略
        else if (path.empty())
            path = a;
    }
    bool gui = gtk_init_check(&argc, &argv) != FALSE;

    caj::Info cap;
    std::string err;
    if (!path.empty()) {
        if (access(path.c_str(), R_OK) != 0) {
            fprintf(stderr, "文件不存在: %s\n", path.c_str());
            return 1;
        }
        if (!caj::caj_open(path, cap, err)) {
            fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        if (cap.fmt.empty()) {
            fprintf(stderr, "无法识别的文件类型（未知魔数）。\n");
            return 1;
        }
        printf("%s\n", caj::analyze_text(cap).c_str());
        if (info_only) return 0;
    } else if (info_only) {
        fprintf(stderr, "%s", USAGE);
        return 1;
    }

    if (!gui) {
        fprintf(stderr, "无法初始化 GTK（无显示环境），仅支持 --info。\n");
        return 1;
    }

    // 先进入软件界面，再加载命令行文件 / 弹出文件选择对话框
    App app;
    app.path = path;
    app.load_recent();          // 供欢迎界面展示
    build_ui(&app);
    gtk_widget_show_all(app.win);
    update_cursor(&app);
    g_idle_add(path.empty() ? choose_file_cb : open_initial_cb, &app);
    gtk_main();

    for (auto& kv : app.bmp_cache) cairo_surface_destroy(kv.second);
    app.bmp_cache.clear();
    if (app.psurf) cairo_surface_destroy(app.psurf);
    delete app.rd;
    app.rd = nullptr;
    if (!app.tmp.empty()) rm_rf(app.tmp);
    return 0;
}
