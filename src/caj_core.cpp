// caj_core.cpp —— CAJ 文档解析 / PDF 提取重建 / 页面渲染后端
// 逐函数移植自 caj_viewer.py（GUI 部分见 caj_viewer.cpp）。
#include "caj_core.hpp"

#include <cairo.h>
#include <dlfcn.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>
#include <glib.h>
#include <pango/pango.h>
#include <pango/pangocairo.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <regex>
#include <set>
#include <thread>

namespace caj {
namespace {

constexpr size_t NPOS = std::string::npos;

// ================= 基础工具 =================

bool read_file(const std::string& p, std::string& out) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return false; }
    rewind(f);
    out.resize((size_t)n);
    size_t rd = n > 0 ? fread(&out[0], 1, (size_t)n, f) : 0;
    fclose(f);
    out.resize(rd);
    return true;
}

// Python struct 的 "i"/"<I" 均为小端 4 字节
int32_t i32(const char* p) {
    const unsigned char* u = (const unsigned char*)p;
    return (int32_t)((uint32_t)u[0] | ((uint32_t)u[1] << 8) |
                     ((uint32_t)u[2] << 16) | ((uint32_t)u[3] << 24));
}
uint32_t u32(const char* p) { return (uint32_t)i32(p); }
uint16_t u16(const char* p) {
    const unsigned char* u = (const unsigned char*)p;
    return (uint16_t)((uint32_t)u[0] | ((uint32_t)u[1] << 8));
}

std::string dir_name(const std::string& p) {
    size_t s = p.rfind('/');
    if (s == NPOS) return ".";
    if (s == 0) return "/";
    return p.substr(0, s);
}

std::string base_name(const std::string& p) {
    size_t s = p.rfind('/');
    return s == NPOS ? p : p.substr(s + 1);
}

std::string join_path(const std::string& a, const std::string& b) {
    // 同 Python 的 os.path.join：b 为绝对路径时直接返回 b。
    // pdftohtml 以绝对路径调用时会写出绝对 src，拼前缀反而会毁掉路径。
    if (a.empty() || (!b.empty() && b[0] == '/')) return b;
    if (a[a.size() - 1] == '/') return a + b;
    return a + "/" + b;
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (unsigned char)s[b] <= ' ') b++;
    while (e > b && (unsigned char)s[e - 1] <= ' ') e--;
    return s.substr(b, e - b);
}

size_t count_sub(const std::string& s, const std::string& sub) {
    if (sub.empty()) return 0;
    size_t n = 0;
    for (size_t p = s.find(sub); p != NPOS; p = s.find(sub, p + 1)) n++;
    return n;
}

// Python str.strip() 等价：判断是否全为（Unicode）空白
bool is_blank_utf8(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            if (c == ' ' || (c >= 0x09 && c <= 0x0D)) { i++; continue; }
            return false;
        }
        uint32_t cp = 0;
        int len = 1;
        if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else return false;
        if (i + len > s.size()) return false;
        for (int k = 1; k < len; k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        bool blank = (cp == 0x85 || cp == 0xA0 || cp == 0x1680 ||
                      (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 ||
                      cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000);
        if (!blank) return false;
        i += (size_t)len;
    }
    return true;
}

std::string sha1_hex(const std::string& s) {
    gchar* h = g_compute_checksum_for_string(G_CHECKSUM_SHA1, s.c_str(), (gssize)s.size());
    std::string r = h ? h : "";
    g_free(h);
    return r;
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

// gb18030 -> UTF-8（失败时退回原始字节）
std::string gb18030_to_utf8(const char* p, size_t n) {
    gsize wr = 0;
    GError* err = nullptr;
    gchar* out = g_convert(p, (gssize)n, "UTF-8", "GB18030", nullptr, &wr, &err);
    if (err) g_error_free(err);
    if (!out) return std::string(p, n);
    std::string r(out, wr);
    g_free(out);
    return r;
}

// ---------------- 子进程（带超时） ----------------
struct RunResult {
    bool ok = false;      // 成功启动并完成通讯
    int status = -1;      // 退出码，未正常退出为 -1
    std::string out;      // stdout 原始字节
};

RunResult run_capture(const std::vector<std::string>& args, const std::string& cwd,
                      int timeout_ms) {
    RunResult r;
    if (args.empty()) return r;
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    GSubprocessLauncher* launcher = g_subprocess_launcher_new(
        (GSubprocessFlags)(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                           G_SUBPROCESS_FLAGS_STDERR_SILENCE));
    if (!cwd.empty()) g_subprocess_launcher_set_cwd(launcher, cwd.c_str());
    GError* err = nullptr;
    GSubprocess* proc = g_subprocess_launcher_spawnv(launcher, argv.data(), &err);
    g_object_unref(launcher);
    if (!proc) {
        if (err) g_error_free(err);
        return r;
    }

    GCancellable* canc = g_cancellable_new();
    std::mutex m;
    std::condition_variable cv;
    bool finished = false;
    std::thread watchdog([&] {
        std::unique_lock<std::mutex> lk(m);
        if (!cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                         [&] { return finished; })) {
            lk.unlock();
            g_cancellable_cancel(canc);
        }
    });

    GBytes* ob = nullptr;
    GBytes* eb = nullptr;
    GError* cerr = nullptr;
    gboolean ok = g_subprocess_communicate(proc, nullptr, canc, &ob, &eb, &cerr);
    {
        std::lock_guard<std::mutex> lk(m);
        finished = true;
    }
    cv.notify_all();
    watchdog.join();

    if (!ok) {
        if (cerr) g_error_free(cerr);
        g_subprocess_force_exit(proc);
    } else {
        r.ok = true;
        if (ob) {
            gsize n = 0;
            const char* p = (const char*)g_bytes_get_data(ob, &n);
            if (p) r.out.assign(p, n);
        }
    }
    if (ob) g_bytes_unref(ob);
    if (eb) g_bytes_unref(eb);
    if (g_subprocess_get_if_exited(proc)) r.status = g_subprocess_get_exit_status(proc);
    g_object_unref(proc);
    g_object_unref(canc);
    return r;
}

// ================= libjbigdec（HN/C8 私有 JBig 解码） =================
typedef void (*JbigDecodeFn)(const char*, unsigned int, unsigned int,
                             unsigned int, unsigned int, char*);
JbigDecodeFn g_jbig = nullptr;
bool g_jbig_tried = false;
std::mutex g_jbig_mu;

JbigDecodeFn jbig_lib() {
    std::lock_guard<std::mutex> lk(g_jbig_mu);
    if (g_jbig_tried) return g_jbig;
    g_jbig_tried = true;
    std::vector<std::string> cands;
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = 0;
        cands.push_back(join_path(dir_name(buf), "libjbigdec.so"));
    }
    const char* home = g_get_home_dir();
    if (home) cands.push_back(std::string(home) + "/.local/lib/libjbigdec.so");
    cands.push_back("libjbigdec.so");
    for (const auto& c : cands) {
        void* h = dlopen(c.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!h) continue;
        void* sym = dlsym(h, "jbigDecode");
        if (!sym) { dlclose(h); continue; }
        g_jbig = (JbigDecodeFn)sym;
        break;
    }
    return g_jbig;
}

// JLIB: 私有 typ=0 页图像（48B DIB 头 + 私格式 JBig 流）
// -> 每行 (w+7)/8 字节的 1bpp 位图，已按 bottom-up 垂直翻转。
bool jbig_decode_1bpp(const std::string& buf, int& w, int& h, std::string& packed) {
    if (buf.size() < 48) return false;
    uint32_t ww = u32(buf.data() + 4);
    uint32_t hh = u32(buf.data() + 8);
    uint16_t planes = u16(buf.data() + 12);
    uint16_t bpp = u16(buf.data() + 14);
    if (ww == 0 || hh == 0 || bpp == 0 || planes == 0) return false;
    if (ww > 20000 || hh > 20000) return false;
    int64_t bpl64 = ((((int64_t)ww * (int64_t)bpp) + 31) >> 5) << 2;
    if (bpl64 <= 0 || bpl64 > (1 << 24)) return false;
    uint32_t bpl = (uint32_t)bpl64;
    if (buf.size() <= 48) return false;
    JbigDecodeFn fn = jbig_lib();
    if (!fn) return false;
    std::vector<char> raw((size_t)hh * bpl, 0);
    fn(buf.data() + 48, (unsigned int)(buf.size() - 48), hh, ww, bpl, raw.data());
    int rowbytes = (int)((ww + 7) / 8);
    packed.assign((size_t)rowbytes * hh, 0);
    for (uint32_t y = 0; y < hh; y++) {
        const char* src = raw.data() + (size_t)(hh - 1 - y) * bpl;
        char* dst = &packed[(size_t)y * rowbytes];
        for (int x = 0; x < rowbytes; x++) dst[x] = src[x];
    }
    w = (int)ww;
    h = (int)hh;
    return true;
}

// ================= 占位页（JBig 无法解码时） =================
const char* placeholder_family(bool& cjk_ok) {
    static std::mutex mu;
    static bool done = false;
    static std::string fam;
    static bool cjk = false;
    std::lock_guard<std::mutex> lk(mu);
    if (!done) {
        done = true;
        const char* cjk_fams[] = {"Noto Serif CJK SC", "Noto Sans CJK SC",
                                  "Source Han Serif SC", "Source Han Sans SC",
                                  "WenQuanYi Zen Hei", "WenQuanYi Micro Hei",
                                  "Droid Sans Fallback"};
        PangoFontMap* fm = pango_cairo_font_map_get_default();
        PangoFontFamily** fams = nullptr;
        int n = 0;
        if (fm) pango_font_map_list_families(fm, &fams, &n);
        for (const char* want : cjk_fams) {
            for (int i = 0; i < n; i++) {
                const char* have = pango_font_family_get_name(fams[i]);
                if (have && strcmp(have, want) == 0) {
                    fam = want;
                    cjk = true;
                    break;
                }
            }
            if (!fam.empty()) break;
        }
        if (fams) g_free(fams);
        if (fam.empty()) fam = "Sans";
    }
    cjk_ok = cjk;
    return fam.c_str();
}

// 暖纸底 + 居中提示文字的 RGB 位图（对应 _jbig_placeholder_jpeg）
bool placeholder_rgb(int w, int h, std::vector<unsigned char>& rgb) {
    if (w <= 0 || h <= 0) return false;
    if (w > 20000) w = 20000;
    if (h > 20000) h = 20000;
    rgb.assign((size_t)w * h * 3, 0);
    cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
    cairo_t* cr = surf ? cairo_create(surf) : nullptr;
    if (!cr || cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        // 极端尺寸下 cairo 会拒绝，退化为纯色底
        for (size_t i = 0; i < (size_t)w * h; i++) {
            rgb[i * 3 + 0] = 246;
            rgb[i * 3 + 1] = 241;
            rgb[i * 3 + 2] = 228;
        }
        if (cr) cairo_destroy(cr);
        if (surf) cairo_surface_destroy(surf);
        return true;
    }
    cairo_set_source_rgb(cr, 246 / 255.0, 241 / 255.0, 228 / 255.0);
    cairo_paint(cr);

    bool cjk_ok = false;
    const char* fam = placeholder_family(cjk_ok);
    std::vector<std::string> lines;
    if (cjk_ok)
        lines = {"此页为 CAJViewer 专有 JBig 编码图像", "当前版本暂无法解码显示"};
    else
        lines = {"CAJViewer proprietary JBig encoded page", "not decodable"};

    double px = (double)std::max(24, std::min(w, h) / 30);
    PangoLayout* lay = pango_cairo_create_layout(cr);
    PangoFontDescription* fd = pango_font_description_new();
    pango_font_description_set_family(fd, fam);
    pango_font_description_set_absolute_size(fd, px * PANGO_SCALE);
    pango_font_description_set_weight(fd, PANGO_WEIGHT_BOLD);
    pango_layout_set_font_description(lay, fd);
    pango_font_description_free(fd);

    double lh = px;
    pango_layout_set_text(lay, "中", -1);
    if (pango_layout_get_line_count(lay) > 0) {
        PangoRectangle le;
        PangoLayoutLine* ln = pango_layout_get_line(lay, 0);
        pango_layout_line_get_extents(ln, nullptr, &le);
        if (le.height > 0) lh = (double)le.height / PANGO_SCALE;
    }
    lh += 40;
    double y = ((double)h - (lh * (double)lines.size() + 40)) / 2;
    cairo_set_source_rgb(cr, 44 / 255.0, 40 / 255.0, 33 / 255.0);
    for (const auto& s : lines) {
        pango_layout_set_text(lay, s.c_str(), -1);
        PangoRectangle ie = {0, 0, 0, 0};
        pango_layout_get_pixel_extents(lay, &ie, nullptr);
        cairo_move_to(cr, ((double)w - ie.width) / 2 - ie.x, y - ie.y);
        pango_cairo_show_layout(cr, lay);
        y += lh;
    }
    g_object_unref(lay);

    cairo_surface_flush(surf);
    unsigned char* data = cairo_image_surface_get_data(surf);
    int stride = cairo_image_surface_get_stride(surf);
    if (data) {
        for (int yy = 0; yy < h; yy++) {
            const unsigned char* row = data + (size_t)yy * stride;
            unsigned char* o = &rgb[(size_t)yy * w * 3];
            for (int xx = 0; xx < w; xx++) {
                // RGB24 小端字节序为 B,G,R,X
                o[xx * 3 + 0] = row[xx * 4 + 2];
                o[xx * 3 + 1] = row[xx * 4 + 1];
                o[xx * 3 + 2] = row[xx * 4 + 0];
            }
        }
    }
    cairo_destroy(cr);
    cairo_surface_destroy(surf);
    return true;
}

std::string zlib_deflate(const std::string& in) {
    uLongf cap = compressBound((uLong)in.size());
    std::string out;
    out.resize(cap);
    uLongf n = cap;
    if (compress2((Bytef*)&out[0], &n, (const Bytef*)in.data(), (uLong)in.size(), 6) !=
        Z_OK)
        return std::string();
    out.resize(n);
    return out;
}

// JPEG 尺寸（SOF 标记）
bool jpeg_size(const std::string& d, int& w, int& h) {
    size_t i = 2;
    if (d.size() < 4 || (unsigned char)d[0] != 0xFF || (unsigned char)d[1] != 0xD8)
        return false;
    while (i + 3 < d.size()) {
        if ((unsigned char)d[i] != 0xFF) { i++; continue; }
        unsigned char mk = (unsigned char)d[i + 1];
        if (mk == 0xFF) { i++; continue; }
        if (mk == 0xD8 || (mk >= 0xD0 && mk <= 0xD7) || mk == 0x01) { i += 2; continue; }
        if (mk == 0xD9 || mk == 0xDA) break;   // EOI / SOS 之后不再有 SOF
        int len = ((unsigned char)d[i + 2] << 8) | (unsigned char)d[i + 3];
        if (len < 2) return false;
        if (mk >= 0xC0 && mk <= 0xCF && mk != 0xC4 && mk != 0xC8 && mk != 0xCC) {
            if (i + 9 >= d.size()) return false;
            h = ((unsigned char)d[i + 5] << 8) | (unsigned char)d[i + 6];
            w = ((unsigned char)d[i + 7] << 8) | (unsigned char)d[i + 8];
            return w > 0 && h > 0;
        }
        i += 2 + (size_t)len;
    }
    return false;
}

std::shared_ptr<Bitmap> bitmap_from_pixbuf(GdkPixbuf* pb) {
    if (!pb) return nullptr;
    int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
    int nc = gdk_pixbuf_get_n_channels(pb);
    int rs = gdk_pixbuf_get_rowstride(pb);
    if (w <= 0 || h <= 0) return nullptr;
    auto b = std::make_shared<Bitmap>();
    b->w = w;
    b->h = h;
    b->rgb.resize((size_t)w * h * 3);
    const guchar* px = gdk_pixbuf_get_pixels(pb);
    for (int y = 0; y < h; y++) {
        const guchar* row = px + (size_t)y * rs;
        unsigned char* o = &b->rgb[(size_t)y * w * 3];
        for (int x = 0; x < w; x++) {
            const guchar* p = row + (size_t)x * nc;
            o[x * 3 + 0] = p[0];
            o[x * 3 + 1] = p[1];
            o[x * 3 + 2] = p[2];
        }
    }
    return b;
}

// pdftohtml 只对 PDF 的 image mask（stencil）输出 1 位灰度 PNG（实测：
// 普通 1 位灰度图会被渲染成 8 位 RGB）。这种图“白 = 着色区”，需按掩膜处理。
bool is_1bit_gray_png(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    unsigned char h[26];
    size_t n = fread(h, 1, sizeof(h), f);
    fclose(f);
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (n < sizeof(h)) return false;
    if (memcmp(h, sig, sizeof(sig)) != 0 || memcmp(h + 12, "IHDR", 4) != 0) return false;
    return h[24] == 1 && h[25] == 0;    // 位深 1、颜色类型 0（灰度）
}

std::shared_ptr<Bitmap> load_bitmap_file(const std::string& path) {
    GError* err = nullptr;
    GdkPixbuf* pb = gdk_pixbuf_new_from_file(path.c_str(), &err);
    if (err) g_error_free(err);
    if (!pb) return nullptr;
    auto b = bitmap_from_pixbuf(pb);
    g_object_unref(pb);
    return b;
}

std::string find_first_match(const std::string& dir, const std::string& prefix,
                             const std::string& suffix) {
    GDir* d = g_dir_open(dir.c_str(), 0, nullptr);
    if (!d) return "";
    std::string best;
    const gchar* name;
    while ((name = g_dir_read_name(d))) {
        std::string s(name);
        if (s.size() < prefix.size() + suffix.size()) continue;
        if (s.compare(0, prefix.size(), prefix) != 0) continue;
        if (s.compare(s.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
        std::string full = join_path(dir, s);
        if (best.empty() || full < best) best = full;
    }
    g_dir_close(d);
    return best;
}

// ================= 容器头部解析 =================

bool read_head4(const std::string& path, std::vector<unsigned char>& head) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    head.assign(4, 0);
    size_t n = fread(&head[0], 1, 4, f);
    fclose(f);
    head.resize(n);
    return true;
}

int64_t file_mtime_ns(const struct stat& st) {
    // 与 Python int(st.st_mtime * 1e9) 的双精度舍入保持一致
    double m = (double)st.st_mtim.tv_sec + (double)st.st_mtim.tv_nsec / 1e9;
    return (int64_t)(m * 1e9);
}

}  // namespace

// ================= 公开：头部解析 =================

bool caj_open(const std::string& path, Info& out, std::string& err) {
    out = Info();
    out.path = path;
    std::vector<unsigned char> head;
    if (!read_head4(path, head)) {
        err = "无法读取文件: " + path;
        return false;
    }
    out.magic = head;
    auto b0 = [&](size_t i) -> unsigned char { return i < head.size() ? head[i] : 0; };
    if (head.size() >= 1 && b0(0) == 0xC8) {
        out.fmt = "C8";
        out.page_off = 0x08;
        out.toc_off = 0;
        out.toc_size = 0;
        return true;
    }
    if (head.size() >= 2 && b0(0) == 'H' && b0(1) == 'N') {
        out.fmt = "HN";
        out.page_off = 0x90;
        out.toc_off = 0x158;
        out.toc_size = 0x134;
        return true;
    }
    // 魔数后常跟版本/空格/NUL（如 "KDH 2.00 ..."、b"CAJ\x00"），
    // 按 NUL+空格剥离后再精确匹配已知格式名
    size_t e = head.size();
    while (e > 0 && (head[e - 1] == 0x00 || head[e - 1] == 0x20)) e--;
    std::string raw((const char*)head.data(), e);
    if (raw == "CAJ" || raw == "HN" || raw == "%PDF" || raw == "KDH" || raw == "TEB")
        out.fmt = raw;
    if (out.fmt == "CAJ") {
        out.page_off = 0x10;
        out.toc_off = 0x110;
        out.toc_size = 0x134;
    } else if (out.fmt == "HN") {
        out.page_off = 0x90;
        out.toc_off = 0x158;
        out.toc_size = 0x134;
    } else {
        out.page_off = out.toc_off = out.toc_size = 0;
    }
    return true;
}

int pdf_page_count(const std::string& path) {
    RunResult r = run_capture({"pdfinfo", path}, "", 10000);
    if (r.ok) {
        size_t pos = 0;
        while (pos <= r.out.size()) {
            size_t nl = r.out.find('\n', pos);
            std::string line = r.out.substr(
                pos, nl == NPOS ? NPOS : nl - pos);
            if (line.compare(0, 6, "Pages:") == 0) {
                size_t p = 6;
                while (p < line.size() && (unsigned char)line[p] <= ' ') p++;
                size_t s = p;
                while (p < line.size() && line[p] >= '0' && line[p] <= '9') p++;
                if (p > s) return atoi(line.substr(s, p - s).c_str());
            }
            if (nl == NPOS) break;
            pos = nl + 1;
        }
    }
    std::string d;
    if (!read_file(path, d)) return 0;
    int n = 0;
    try {
        std::regex re("/Type\\s*/Page[^s]");
        for (auto it = std::sregex_iterator(d.begin(), d.end(), re);
             it != std::sregex_iterator(); ++it)
            n++;
    } catch (...) {
        return 0;
    }
    return n;
}

int caj_page_num(const Info& info) {
    if (info.fmt == "%PDF") return pdf_page_count(info.path);
    if (!info.page_off) return 0;
    FILE* f = fopen(info.path.c_str(), "rb");
    if (!f) return 0;
    if (fseek(f, info.page_off, SEEK_SET) != 0) { fclose(f); return 0; }
    char buf[4] = {0, 0, 0, 0};
    size_t n = fread(buf, 1, 4, f);
    fclose(f);
    if (n < 4) return 0;
    return i32(buf);
}

std::vector<TocItem> caj_toc(const Info& info) {
    std::vector<TocItem> out;
    if (!info.toc_off || info.toc_size < 308) return out;
    FILE* f = fopen(info.path.c_str(), "rb");
    if (!f) return out;
    if (fseek(f, info.toc_off, SEEK_SET) != 0) { fclose(f); return out; }
    char nb[4];
    if (fread(nb, 1, 4, f) != 4) { fclose(f); return out; }
    int n = i32(nb);
    std::vector<char> raw((size_t)info.toc_size);
    for (int i = 0; i < n; i++) {
        if (fread(raw.data(), 1, (size_t)info.toc_size, f) != (size_t)info.toc_size)
            break;
        // struct "256s24s12s12si"
        size_t tl = 0;
        while (tl < 256 && raw[tl] != 0) tl++;
        TocItem it;
        it.title = gb18030_to_utf8(raw.data(), tl);
        size_t pl = 0;
        while (pl < 12 && raw[256 + 24 + pl] != 0) pl++;
        std::string pg(raw.data() + 256 + 24, pl);
        int page = 0;
        try {
            if (!trim(pg).empty()) page = std::stoi(trim(pg));
        } catch (...) {
            page = 0;
        }
        it.page = page;
        it.level = i32(raw.data() + 256 + 24 + 12 + 12);
        out.push_back(it);
    }
    fclose(f);
    return out;
}

std::string analyze_text(const Info& info) {
    std::vector<std::string> L;
    L.push_back(std::string(56, '='));
    L.push_back(" CAJ 文件格式分析：" + base_name(info.path));
    L.push_back(std::string(56, '='));
    std::string hex;
    for (size_t i = 0; i < info.magic.size(); i++) {
        char b[8];
        snprintf(b, sizeof(b), "%s%02X", i ? " " : "", info.magic[i]);
        hex += b;
    }
    L.push_back("文件头(前4字节): " + hex);
    size_t e = info.magic.size();
    while (e > 0 && info.magic[e - 1] == 0x00) e--;
    L.push_back("标识文本: '" + std::string((const char*)info.magic.data(), e) + "'");
    L.push_back("格式类别: " + (info.fmt.empty() ? std::string("None") : info.fmt));
    if (info.page_off) {
        char b[64];
        snprintf(b, sizeof(b), "页面数:   %d", caj_page_num(info));
        L.push_back(b);
    }
    if (info.toc_off) {
        std::vector<TocItem> toc = caj_toc(info);
        char b[96];
        snprintf(b, sizeof(b), "大纲项数: %d (起始偏移 0x%lX)", (int)toc.size(),
                 info.toc_off);
        L.push_back(b);
        L.push_back("— 大纲预览 —");
        for (size_t i = 0; i < toc.size() && i < 20; i++) {
            std::string ind;
            for (int k = 1; k < toc[i].level; k++) ind += "   ";
            char pb[64];
            snprintf(pb, sizeof(pb), "  %s%s  · 第 %d 页", ind.c_str(),
                     toc[i].title.c_str(), toc[i].page);
            L.push_back(pb);
        }
        if (toc.size() > 20) {
            snprintf(b, sizeof(b), "  ... 共 %d 项", (int)toc.size());
            L.push_back(b);
        }
    }
    L.push_back(std::string(56, '-'));
    if (info.fmt == "CAJ") {
        L.push_back("结构说明：CAJ 是知网私有容器，文件头 'CAJ' 后按特定偏移");
        L.push_back("存放信息。0x10=页数，0x110=大纲表；0x14 处为指向内嵌");
        L.push_back("PDF 对象流的指针。读取其中的 PDF 对象并重建目录/xref");
        L.push_back("即可得到可渲染的标准 PDF——本程序正是采用此方案。");
    } else if (info.fmt == "%PDF") {
        L.push_back("该文件本质是标准 PDF，直接改后缀为 .pdf 即可阅读。");
    } else if (info.fmt == "HN") {
        L.push_back("HN 为另一类专有格式，无内嵌 PDF 流。正文页面多为");
        L.push_back("CAJViewer 专有 JBig 编码图像（无法直接解码），少量为");
        L.push_back("标准 JPEG。本程序解析页信息表，将 JPEG 页直接嵌入");
        L.push_back("PDF，JBig 页以占位白纸代替（可正常翻页/大纲跳转）。");
    } else if (info.fmt == "KDH") {
        L.push_back("KDH 2.00 为新版知网下载的加密格式：头部 0x3C 处指向");
        L.push_back("尾部明文 XML 元数据，其间为多字节循环 XOR 加密的 PDF。");
        L.push_back("本程序按字节频率分析自动恢复密钥并解出内嵌 PDF，");
        L.push_back("同一文件第二次打开走 sidecar 缓存，近瞬时。");
    } else {
        L.push_back("此格式变体暂不完整支持。");
    }
    L.push_back(std::string(56, '='));
    std::string s;
    for (size_t i = 0; i < L.size(); i++) {
        if (i) s += "\n";
        s += L[i];
    }
    return s;
}

// ================= 提取并重建内嵌 PDF =================
namespace {

// CAJ 容器第 0x14 字节起存有内嵌 PDF 流的起始指针。读取全部 PDF 对象、
// 补全目录(Catalog)/根 Pages 与 xref，产出一个可用 PDF。
bool extract_pdf_bytes(const std::string& d, std::string& out_pdf, int& npages,
                       std::string& err) {
    if (d.size() < 0x18) {
        err = "文件过小，不是有效的 CAJ 容器。";
        return false;
    }
    int32_t ptr = i32(d.data() + 0x14);
    if (ptr < 0 || (size_t)ptr + 4 > d.size()) {
        err = "内嵌 PDF 指针越界。";
        return false;
    }
    int32_t pdf_start = i32(d.data() + ptr);
    if (pdf_start < 0 || (size_t)pdf_start >= d.size()) {
        err = "内嵌 PDF 偏移非法。";
        return false;
    }
    std::string blob = d.substr((size_t)pdf_start);
    size_t last_eo = blob.rfind("endobj");
    if (last_eo == NPOS) {
        err = "未在 CAJ 中找到内嵌 PDF 对象(endobj)。";
        return false;
    }
    std::string body = blob.substr(0, last_eo + 6);

    // 所有 endobj 的位置
    std::vector<size_t> eo;
    for (size_t p = blob.find("endobj"); p != NPOS; p = blob.find("endobj", p + 1))
        eo.push_back(p);

    // 收集所有对象头 `N 0 obj` -> 起始偏移
    struct Hdr { long no; size_t pos; };
    std::vector<Hdr> headers;
    try {
        std::regex re("([0-9]+) 0 obj");
        for (auto it = std::sregex_iterator(body.begin(), body.end(), re);
             it != std::sregex_iterator(); ++it)
            headers.push_back({std::stol((*it)[1].str()), (size_t)it->position()});
    } catch (...) {
        err = "解析内嵌 PDF 对象头失败。";
        return false;
    }
    if (headers.empty()) {
        err = "内嵌 PDF 中未找到任何对象。";
        return false;
    }
    std::sort(headers.begin(), headers.end(),
              [](const Hdr& a, const Hdr& b) { return a.pos < b.pos; });

    std::map<long, size_t> blocks;      // 对象号 -> 起始偏移（保留首次出现）
    std::vector<long> block_order;      // 保持 endobj 的出现顺序（同 Python dict）
    for (size_t e : eo) {
        size_t lo = 0, hi = headers.size();
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (headers[mid].pos <= e) lo = mid + 1;
            else hi = mid;
        }
        if (lo == 0) continue;
        long no = headers[lo - 1].no;
        if (blocks.find(no) == blocks.end()) {
            blocks[no] = headers[lo - 1].pos;
            block_order.push_back(no);
        }
    }

    struct Node {
        std::string typ;
        long parent = 0;
        std::vector<long> kids;
    };
    auto classify = [&](long no, Node& nd) {
        size_t s = blocks[no];
        size_t seg_end = body.find("stream", s);
        if (seg_end == NPOS) seg_end = body.find("endobj", s);
        size_t endpos = (seg_end == NPOS) ? body.size() : s + seg_end;
        if (endpos > body.size()) endpos = body.size();
        std::string::const_iterator b = body.begin() + s;
        std::string::const_iterator e = body.begin() + endpos;
        std::smatch m;
        nd.typ.clear();
        nd.parent = 0;
        nd.kids.clear();
        try {
            std::regex re_type("/Type\\s*/([A-Za-z]+)");
            if (std::regex_search(b, e, m, re_type)) nd.typ = m[1].str();
            std::regex re_parent("/Parent\\s*([0-9]+)\\s*0\\s*R");
            if (std::regex_search(b, e, m, re_parent)) nd.parent = std::stol(m[1].str());
            std::regex re_kids("/Kids\\s*\\[([^\\]]*)\\]");
            if (std::regex_search(b, e, m, re_kids)) {
                std::string inner = m[1].str();
                std::regex re_ref("([0-9]+)\\s*0\\s*R");
                for (auto it = std::sregex_iterator(inner.begin(), inner.end(), re_ref);
                     it != std::sregex_iterator(); ++it)
                    nd.kids.push_back(std::stol((*it)[1].str()));
            }
        } catch (...) {
            // 单个对象字典异常：按未识别处理
        }
    };

    std::map<long, Node> nodes;
    std::vector<long> nodes_order;
    long cat_no = -1;
    for (long no : block_order) {
        Node nd;
        classify(no, nd);
        if (nd.typ == "Page" || nd.typ == "Pages" || nd.typ == "Catalog") {
            if (nd.typ == "Catalog") cat_no = no;
            nodes[no] = nd;
            nodes_order.push_back(no);
        }
    }

    std::vector<long> page_leaf;
    for (long no : nodes_order)
        if (nodes[no].typ == "Page") page_leaf.push_back(no);

    std::set<long> parent_targets;
    for (long no : nodes_order)
        if (nodes[no].typ == "Pages" && nodes[no].parent)
            parent_targets.insert(nodes[no].parent);

    std::vector<long> roots;
    for (long no : nodes_order)
        if (nodes[no].typ == "Pages" && parent_targets.find(no) == parent_targets.end())
            roots.push_back(no);

    // 按 /Kids 顺序 DFS 收集叶子页，得到真实文档页序（不能按对象号排序！）
    std::vector<long> leaves;
    std::set<long> seen;
    std::function<void(long)> walk = [&](long n) {
        if (seen.find(n) != seen.end()) return;
        seen.insert(n);
        auto it = nodes.find(n);
        if (it == nodes.end()) return;
        if (it->second.typ == "Page") {
            leaves.push_back(n);
            return;
        }
        for (long c : it->second.kids) walk(c);
    };
    for (long r : roots) walk(r);
    if (leaves.empty()) leaves = page_leaf;   // 兜底：树解析失败则退回原始顺序

    std::string out = "%PDF-1.3\r\n" + body;
    long mb = 0;
    for (auto& kv : blocks) mb = std::max(mb, kv.first);
    long ml = 0;
    for (long p : leaves) ml = std::max(ml, p);
    long root_pages = std::max(std::max(mb + 1, ml + 1), 1L);

    std::string kids;
    for (size_t i = 0; i < leaves.size(); i++) {
        if (i) kids += " ";
        kids += std::to_string(leaves[i]) + " 0 R";
    }
    {
        char b[64];
        snprintf(b, sizeof(b), "%ld 0 obj\r<</Type /Pages /Kids [", root_pages);
        out += b;
        out += kids;
        snprintf(b, sizeof(b), "] /Count %d>>\rendobj\r", (int)leaves.size());
        out += b;
        blocks[root_pages] = body.size();
    }
    if (cat_no < 0) {
        long mb2 = 0;
        for (auto& kv : blocks) mb2 = std::max(mb2, kv.first);
        cat_no = std::max(std::max(mb2 + 1, root_pages + 1), 1L);
        char b[96];
        snprintf(b, sizeof(b), "%ld 0 obj\r<</Type /Catalog /Pages %ld 0 R>>\rendobj\r",
                 cat_no, root_pages);
        out += b;
        blocks[cat_no] = body.size();
    }

    // 计算每个对象在输出文件中的绝对偏移
    std::map<long, size_t> abs_off;
    try {
        std::regex re("([0-9]+) 0 obj");
        for (auto it = std::sregex_iterator(out.begin(), out.end(), re);
             it != std::sregex_iterator(); ++it) {
            long no = std::stol((*it)[1].str());
            if (abs_off.find(no) == abs_off.end())
                abs_off[no] = (size_t)it->position();
        }
    } catch (...) {
    }

    long size = cat_no + 1;
    std::string xref = "xref\r\n0 " + std::to_string(size) + "\r\n";
    xref += "0000000000 65535 f\r\n";
    for (long no = 1; no < size; no++) {
        auto it = abs_off.find(no);
        if (it != abs_off.end()) {
            char b[32];
            snprintf(b, sizeof(b), "%010zu 00000 n\r\n", it->second);
            xref += b;
        } else {
            xref += "0000000000 65535 f\r\n";
        }
    }
    size_t startxref = out.size();
    char tb[160];
    snprintf(tb, sizeof(tb),
             "trailer\r\r<</Size %ld /Root %ld 0 R>>\rstartxref\r%zu\r%%%%EOF\r", size,
             cat_no, startxref);
    out += xref;
    out += tb;

    out_pdf = out;
    npages = (int)leaves.size();
    return true;
}

// ================= HN / C8：从页图像构建 PDF =================
// 每页图像：JPEG 直接嵌入；JBig 解码为 1bpp 灰度；失败则占位。
struct EmbedImg {
    int w = 0, h = 0;
    int bpc = 8;
    std::string cs = "/DeviceRGB";
    std::string extra;   // 例如 " /Decode [1 0]"
    std::string filter;
    std::string data;
};

EmbedImg embed_from_rgb(int w, int h, const std::vector<unsigned char>& rgb) {
    EmbedImg im;
    im.w = w;
    im.h = h;
    im.bpc = 8;
    im.cs = "/DeviceRGB";
    im.filter = "/Filter /FlateDecode";
    std::string raw((const char*)rgb.data(), rgb.size());
    im.data = zlib_deflate(raw);
    if (im.data.empty()) im.data = raw;   // zlib 失败则裸数据
    return im;
}

EmbedImg embed_placeholder(int w, int h) {
    std::vector<unsigned char> rgb;
    if (!placeholder_rgb(w, h, rgb)) {
        rgb.assign((size_t)std::max(1, w) * std::max(1, h) * 3, 0);
        for (size_t i = 0; i < rgb.size() / 3; i++) {
            rgb[i * 3 + 0] = 246;
            rgb[i * 3 + 1] = 241;
            rgb[i * 3 + 2] = 228;
        }
        if (w <= 0) w = 1;
        if (h <= 0) h = 1;
    }
    return embed_from_rgb(w, h, rgb);
}

// 返回 (toc_end, entries)
bool hn_offsets(const std::string& d, std::vector<std::array<long, 4>>& entries) {
    if (d.size() < 0x15C) return false;
    int32_t toc_num = i32(d.data() + 0x158);
    long toc_end = 0x158 + 4 + (long)0x134 * toc_num;
    if (toc_num < 0) return false;
    int32_t n = i32(d.data() + 0x90);
    for (int i = 0; i < n; i++) {
        long off = toc_end + (long)i * 20;
        if (off < 0 || (size_t)off + 20 > d.size()) break;
        const char* p = d.data() + off;
        entries.push_back({i32(p), i32(p + 4), (long)(int16_t)u16(p + 8),
                           (long)(int16_t)u16(p + 10)});
    }
    return true;
}

bool build_hn_pdf(const std::string& d, std::string& out_pdf, std::string& err) {
    (void)err;
    std::vector<std::array<long, 4>> entries;
    hn_offsets(d, entries);
    int n = (int)entries.size();
    int base_w = 1600, base_h = 2400;      // 无法解析尺寸时的兜底

    std::vector<EmbedImg> pages;
    pages.reserve((size_t)n);
    for (const auto& e : entries) {
        long pdoff = e[0], tsize = e[1];
        long cur = pdoff + tsize;
        if (cur < 0 || (size_t)cur + 12 > d.size()) {
            pages.push_back(embed_placeholder(base_w, base_h));
            continue;
        }
        int32_t typ = i32(d.data() + cur);
        int32_t ioff = i32(d.data() + cur + 4);
        int32_t isize = i32(d.data() + cur + 8);
        std::string buf;
        if (ioff >= 0 && isize > 0 && (size_t)ioff + (size_t)isize <= d.size())
            buf = d.substr((size_t)ioff, (size_t)isize);

        // 标准 JPEG（typ=1/2）：直接嵌入
        if ((typ == 1 || typ == 2) && buf.size() >= 2 &&
            (unsigned char)buf[0] == 0xFF && (unsigned char)buf[1] == 0xD8) {
            int jw = 0, jh = 0;
            if (jpeg_size(buf, jw, jh)) {
                EmbedImg im;
                im.w = jw;
                im.h = jh;
                im.bpc = 8;
                im.cs = "/DeviceRGB";
                im.filter = "/Filter /DCTDecode";
                im.data = buf;
                pages.push_back(im);
                continue;
            }
        }
        // 私有 JBig（typ=0）：用 libjbigdec.so 解码
        if (typ == 0 && jbig_lib() != nullptr) {
            int jw = 0, jh = 0;
            std::string packed;
            if (jbig_decode_1bpp(buf, jw, jh, packed)) {
                EmbedImg im;
                im.w = jw;
                im.h = jh;
                im.bpc = 1;
                im.cs = "/DeviceGray";
                // 位=1 -> 黑（对应 Python PIL 的 "1;I" 原始模式）
                im.extra = " /Decode [1 0]";
                im.filter = "/Filter /FlateDecode";
                im.data = zlib_deflate(packed);
                if (im.data.empty()) im.data = packed;
                pages.push_back(im);
                continue;
            }
        }
        // 兜底：占位白纸（尺寸尽量取 JBig 头里的真实宽高）
        if (buf.size() >= 12) {
            int32_t jw = i32(buf.data() + 4);
            int32_t jh = i32(buf.data() + 8);
            if (jw > 0 && jh > 0) {
                base_w = jw;
                base_h = jh;
            }
        }
        pages.push_back(embed_placeholder(base_w, base_h));
    }

    // 每页分配三个对象号：图像 / Contents / Page
    std::vector<long> im_no((size_t)n), co_no((size_t)n), pg_no((size_t)n);
    long cur_no = 1;
    for (int i = 0; i < n; i++) {
        im_no[i] = cur_no++;
        co_no[i] = cur_no++;
        pg_no[i] = cur_no++;
    }
    long root_pg = cur_no++;
    long catalog = cur_no++;
    long total = cur_no - 1;

    std::string body;
    for (int i = 0; i < n; i++) {
        const EmbedImg& im = pages[(size_t)i];
        char b[256];
        snprintf(b, sizeof(b),
                 "%ld 0 obj\r<</Type /XObject /Subtype /Image /Width %d /Height %d "
                 "/ColorSpace %s /BitsPerComponent %d%s %s /Length %zu>>\rstream\r",
                 im_no[i], im.w, im.h, im.cs.c_str(), im.bpc, im.extra.c_str(),
                 im.filter.c_str(), im.data.size());
        body += b;
        body += im.data;
        body += "\rendstream\rendobj\r\r\n";

        // Contents：必须用显式 cm 缩放矩阵把图像铺满整页（poppler 对恒等变换
        // 直接 Do 的整页图像会塌缩成约 1pt 而不可见）。
        char cs[128];
        snprintf(cs, sizeof(cs), "q\r%d 0 0 %d 0 0 cm\r/Im%d Do\rQ", im.w, im.h, i);
        std::string cs_txt = cs;
        snprintf(b, sizeof(b), "%ld 0 obj\r<</Length %zu>>\rstream\r", co_no[i],
                 cs_txt.size());
        body += b;
        body += cs_txt;
        body += "\rendstream\rendobj\r\r\n";

        snprintf(b, sizeof(b),
                 "%ld 0 obj\r<</Type /Page /Parent %ld 0 R /MediaBox [0 0 %d %d] "
                 "/Resources << /XObject << /Im%d %ld 0 R >> >> /Contents %ld 0 R "
                 ">>\rendobj\r\r\n",
                 pg_no[i], root_pg, im.w, im.h, i, im_no[i], co_no[i]);
        body += b;
    }
    {
        std::string kids;
        for (int i = 0; i < n; i++) {
            if (i) kids += " ";
            kids += std::to_string(pg_no[i]) + " 0 R";
        }
        char b[64];
        snprintf(b, sizeof(b), "%ld 0 obj\r<</Type /Pages /Kids [", root_pg);
        body += b;
        body += kids;
        snprintf(b, sizeof(b), "] /Count %d>>\rendobj\r\r\n", n);
        body += b;
        char c[96];
        snprintf(c, sizeof(c), "%ld 0 obj\r<</Type /Catalog /Pages %ld 0 R>>\rendobj\r\r\n",
                 catalog, root_pg);
        body += c;
    }

    std::string out = "%PDF-1.4\r\n" + body;
    std::map<long, size_t> xref_off;
    try {
        std::regex re("([0-9]+) 0 obj");
        for (auto it = std::sregex_iterator(out.begin(), out.end(), re);
             it != std::sregex_iterator(); ++it) {
            long no = std::stol((*it)[1].str());
            if (xref_off.find(no) == xref_off.end())
                xref_off[no] = (size_t)it->position();
        }
    } catch (...) {
    }
    long size = total + 1;
    std::string xref = "xref\r\n0 " + std::to_string(size) + "\r\n";
    xref += "0000000000 65535 f\r\n";
    for (long no = 1; no < size; no++) {
        auto it = xref_off.find(no);
        if (it != xref_off.end()) {
            char b[32];
            snprintf(b, sizeof(b), "%010zu 00000 n\r\n", it->second);
            xref += b;
        } else {
            xref += "0000000000 65535 f\r\n";
        }
    }
    size_t startxref = out.size();
    char tb[160];
    snprintf(tb, sizeof(tb),
             "trailer\r\r<</Size %ld /Root %ld 0 R>>\rstartxref\r%zu\r%%%%EOF\r", size,
             catalog, startxref);
    out += xref;
    out += tb;
    out_pdf = out;
    return true;
}

// ================= KDH 2.00：加密容器解密 =================
// 头部 0x3C 处指向尾部明文 XML 元数据；0xFC 起为 N 字节循环 XOR 加密体。
// 密钥由字节频率分析恢复（详见 Python 版注释）。
std::string xor_rep(const std::string& data, const std::string& key) {
    if (key.empty()) return data;
    std::string out(data.size(), 0);
    for (size_t i = 0; i < data.size(); i++) out[i] = (char)(data[i] ^ key[i % key.size()]);
    return out;
}

std::string kdh_pdf(const std::string& body, const std::string& key) {
    std::string plain = xor_rep(body, key);
    size_t off = plain.find("%PDF-");
    if (off == NPOS) return "";
    std::string pdf = plain.substr(off);
    size_t e = pdf.rfind("%%EOF");
    if (e != NPOS) pdf.resize(e + 5);
    if (count_sub(pdf, "endobj") == 0 && count_sub(pdf, " obj") == 0) return "";
    return pdf;
}

std::string kdh_try(const std::string& body, int L) {
    if (L <= 0) return "";
    std::string tops((size_t)L, 0);
    for (int i = 0; i < L; i++) {
        size_t cnt[256] = {0};
        for (size_t j = (size_t)i; j < body.size(); j += (size_t)L)
            cnt[(unsigned char)body[j]]++;
        int best = 0;
        for (int k = 1; k < 256; k++)
            if (cnt[k] > cnt[best]) best = k;
        tops[(size_t)i] = (char)best;
    }
    std::string cand = xor_rep(body, tops);
    // 若密钥正确，cand = 明文 ^ 常数；其相邻差分序列含 "endobj" 的相邻 XOR
    // 模式（与常数无关），每个命中即投一票确定该常数
    const char* crib = "endobj";
    char dpat[5];
    for (int i = 0; i < 5; i++) dpat[i] = (char)(crib[i] ^ crib[i + 1]);
    std::string z;
    if (cand.size() >= 2)
        z = xor_rep(cand.substr(0, cand.size() - 1), cand.substr(1));
    std::vector<int> seq;
    size_t cnt[256] = {0};
    std::string pat(dpat, 5);
    for (size_t i = z.find(pat); i != NPOS; i = z.find(pat, i + 1)) {
        size_t idx = i;
        if (idx >= cand.size()) break;
        unsigned char b = (unsigned char)(cand[idx] ^ crib[0]);
        seq.push_back((int)b);
        cnt[b]++;
    }
    if (!seq.empty()) {
        int best = seq[0];
        size_t bc = cnt[(size_t)best];
        for (int b : seq) {
            if (cnt[(size_t)b] > bc) { bc = cnt[(size_t)b]; best = b; }
        }
        if ((int)bc >= 3) {          // 票数集中：密钥可信
            std::string key = tops;
            for (size_t i = 0; i < key.size(); i++)
                key[i] = (char)((unsigned char)tops[i] ^ (unsigned char)best);
            return kdh_pdf(body, key);
        }
    }
    const unsigned char fallbacks[] = {0x30, 0x00, 0x0D, 0x20};
    for (unsigned char m0 : fallbacks) {   // 投票不足：按 PDF 常见最高频字节验证
        std::string key = tops;
        for (size_t i = 0; i < key.size(); i++)
            key[i] = (char)((unsigned char)tops[i] ^ m0);
        std::string pdf = kdh_pdf(body, key);
        if (!pdf.empty()) return pdf;
    }
    return "";
}

bool kdh_decrypt(const std::string& d, std::string& out_pdf, std::string& err) {
    if (d.size() < 0x100) {
        err = "KDH 文件过小";
        return false;
    }
    int32_t end = i32(d.data() + 0x3C);
    if (!(end > 0xFC && (size_t)end <= d.size())) end = (int32_t)d.size();
    std::string body = d.substr(0xFC, (size_t)end - 0xFC);
    if (body.size() < 0x200) {
        err = "KDH 加密体过小";
        return false;
    }
    const int lens[] = {6, 4, 8, 12, 16, 32, 64};   // 实测为 6，其余为兼容变体
    for (int L : lens) {
        std::string pdf = kdh_try(body, L);
        if (!pdf.empty()) {
            out_pdf = pdf;
            return true;
        }
    }
    err = "KDH 解密失败：无法恢复 XOR 密钥";
    return false;
}

// ================= sidecar 缓存 =================
// 把“提取/重建 PDF”的结果落到 <源文件>.cajviewer.pdf，同一文件二次打开近瞬时。
std::string cache_path_for(const std::string& path) {
    try {
        std::string p = path + ".cajviewer.pdf";
        std::string probe = join_path(dir_name(p), ".cajviewer.write_probe");
        FILE* f = fopen(probe.c_str(), "wb");
        if (f) {
            fwrite("x", 1, 1, f);
            fclose(f);
            remove(probe.c_str());
            return p;
        }
    } catch (...) {
    }
    gchar* abs = g_canonicalize_filename(path.c_str(), nullptr);
    std::string full = abs ? abs : path;
    if (abs) g_free(abs);
    std::string h = sha1_hex(full).substr(0, 16);
    const char* home = g_get_home_dir();
    if (!home) return "";
    std::string dir = std::string(home) + "/.cache/caj-viewer";
    if (g_mkdir_with_parents(dir.c_str(), 0755) != 0) return "";
    return join_path(dir, h + ".cajviewer.pdf");
}

bool try_load_cache(const std::string& path, const std::string& fmt, std::string& pdf) {
    std::string cp = cache_path_for(path);
    if (cp.empty()) return false;
    struct stat st;
    if (stat(cp.c_str(), &st) != 0) return false;
    struct stat st_src;
    if (stat(path.c_str(), &st_src) != 0) return false;
    std::string data;
    if (!read_file(cp, data)) return false;
    // 缓存头：源 mtime(ns)|源 size|fmt
    char head[16];
    int64_t ns = file_mtime_ns(st_src);
    int64_t sz = (int64_t)st_src.st_size;
    memcpy(head, &ns, 8);
    memcpy(head + 8, &sz, 8);
    std::string want(head, 16);
    want += fmt.empty() ? "?" : fmt;
    if (data.size() < want.size() || data.compare(0, want.size(), want) != 0) return false;
    // 按内容定位 PDF 起点，兼容头部未补齐 64 字节的新旧缓存
    size_t off = data.find("%PDF-");
    if (off == NPOS) return false;
    pdf = data.substr(off);
    return true;
}

void save_cache(const std::string& path, const std::string& fmt,
                const std::string& pdf_bytes) {
    std::string cp = cache_path_for(path);
    if (cp.empty()) return;
    struct stat st_src;
    if (stat(path.c_str(), &st_src) != 0) return;
    int64_t ns = file_mtime_ns(st_src);
    int64_t sz = (int64_t)st_src.st_size;
    std::string head;
    head.append((const char*)&ns, 8);
    head.append((const char*)&sz, 8);
    head += fmt.empty() ? "?" : fmt;
    head.resize(std::max<size_t>(head.size(), 64), '\0');   // 头补齐 64B
    std::string tmp = cp + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    fwrite(head.data(), 1, head.size(), f);
    if (!pdf_bytes.empty()) fwrite(pdf_bytes.data(), 1, pdf_bytes.size(), f);
    fclose(f);
    if (rename(tmp.c_str(), cp.c_str()) != 0) remove(tmp.c_str());
}

}  // namespace

bool prepare_doc(const std::string& path, Info& cap, std::vector<unsigned char>& pdf,
                 std::string& err) {
    if (!caj_open(path, cap, err)) return false;
    if (cap.fmt.empty()) {
        err = "无法识别的文件类型（未知魔数）。";
        return false;
    }
    std::string cached;
    if (try_load_cache(path, cap.fmt, cached)) {
        pdf.assign(cached.begin(), cached.end());
        return true;
    }
    std::string raw;
    if (!read_file(path, raw)) {
        err = "无法读取文件: " + path;
        return false;
    }
    std::string out;
    bool ok = false;
    if (cap.fmt == "HN") {
        ok = build_hn_pdf(raw, out, err);
    } else if (cap.fmt == "%PDF") {
        out = raw;                       // 本质是标准 PDF，直接使用
        ok = true;
    } else if (cap.fmt == "KDH") {
        ok = kdh_decrypt(raw, out, err); // 新版知网加密格式，自动解密
    } else {
        int np = 0;
        ok = extract_pdf_bytes(raw, out, np, err);
    }
    if (!ok) return false;
    pdf.assign(out.begin(), out.end());
    // 异步写缓存（避免阻塞调用方）
    std::string fmt = cap.fmt;
    std::thread([path, fmt, out] { save_cache(path, fmt, out); }).detach();
    return true;
}

// ================= 渲染器 =================
namespace {

// GMarkup 解析 pdftohtml 输出
struct LayoutCtx {
    std::shared_ptr<PageLayout> out;
    std::string tmpdir;
    bool whole_page_img = false;
    int depth = 0;
    int page_depth = -1;
    int text_depth = -1;
    bool in_text = false;
    TextRun cur;
    std::string cur_text;
};

std::string gattr(const gchar** names, const gchar** values, const gchar* key) {
    for (int i = 0; names && names[i]; i++)
        if (strcmp(names[i], key) == 0) return values[i] ? values[i] : "";
    return "";
}

double gattr_num(const gchar** names, const gchar** values, const gchar* key,
                 double def) {
    std::string s = gattr(names, values, key);
    if (s.empty()) return def;
    try {
        return std::stod(s);
    } catch (...) {
        return def;
    }
}

void layout_start(GMarkupParseContext*, const gchar* name, const gchar** anames,
                  const gchar** avalues, gpointer ud, GError**) {
    LayoutCtx* c = (LayoutCtx*)ud;
    c->depth++;
    if (strcmp(name, "page") == 0) {
        if (!c->out) {
            c->out = std::make_shared<PageLayout>();
            c->out->w = gattr_num(anames, avalues, "width", 0);
            c->out->h = gattr_num(anames, avalues, "height", 0);
        }
        c->page_depth = c->depth;
        return;
    }
    if (!c->out || c->depth != c->page_depth + 1) return;
    if (strcmp(name, "fontspec") == 0) {
        int id = (int)gattr_num(anames, avalues, "id", -1);
        FontSpec fs;
        fs.size = gattr_num(anames, avalues, "size", 12);
        fs.family = gattr(anames, avalues, "family");
        std::string col = gattr(anames, avalues, "color");
        fs.color = col.empty() ? "#000000" : col;
        if (id >= 0) c->out->fonts[id] = fs;
        return;
    }
    if (strcmp(name, "text") == 0) {
        c->cur = TextRun();
        c->cur.x = gattr_num(anames, avalues, "left", 0);
        c->cur.y = gattr_num(anames, avalues, "top", 0);
        c->cur.w = gattr_num(anames, avalues, "width", 0);
        c->cur.h = gattr_num(anames, avalues, "height", 0);
        c->cur.font = (int)gattr_num(anames, avalues, "font", -1);
        c->cur_text.clear();
        c->in_text = true;
        c->text_depth = c->depth;
        return;
    }
    if (c->in_text && c->depth == c->text_depth + 1) {
        if (strcmp(name, "b") == 0) c->cur.bold = true;
        else if (strcmp(name, "i") == 0) c->cur.italic = true;
        return;
    }
    if (strcmp(name, "image") == 0) {
        double iw = gattr_num(anames, avalues, "width", 0);
        double ih = gattr_num(anames, avalues, "height", 0);
        if (iw * ih >= 0.9 * c->out->w * c->out->h) {
            c->whole_page_img = true;   // 整页扫描图：改按位图渲染
            return;
        }
        std::string src = gattr(anames, avalues, "src");
        if (src.empty()) return;
        std::string path = join_path(c->tmpdir, src);
        struct stat st;
        if (stat(path.c_str(), &st) != 0) return;
        auto bmp = load_bitmap_file(path);
        if (!bmp) return;
        ImgRun ir;
        ir.x = gattr_num(anames, avalues, "left", 0);
        ir.y = gattr_num(anames, avalues, "top", 0);
        ir.w = iw;
        ir.h = ih;
        ir.mask = is_1bit_gray_png(path);
        ir.bmp = bmp;
        c->out->imgs.push_back(ir);
        return;
    }
}

void layout_end(GMarkupParseContext*, const gchar* name, gpointer ud, GError**) {
    LayoutCtx* c = (LayoutCtx*)ud;
    if (c->in_text && c->depth == c->text_depth && strcmp(name, "text") == 0) {
        if (!is_blank_utf8(c->cur_text)) {
            c->cur.text = c->cur_text;
            c->out->texts.push_back(c->cur);
        }
        c->in_text = false;
        c->text_depth = -1;
        c->cur_text.clear();
    } else if (strcmp(name, "page") == 0) {
        c->page_depth = -1;
    }
    c->depth--;
}

void layout_text(GMarkupParseContext*, const gchar* text, gsize len, gpointer ud,
                 GError**) {
    LayoutCtx* c = (LayoutCtx*)ud;
    if (c->in_text && text && len) c->cur_text.append(text, len);
}

std::shared_ptr<const PageLayout> parse_layout_impl(const std::string& xml,
                                                    const std::string& tmpdir) {
    // 剥掉 <?xml?> / <!DOCTYPE ...> 前导，GMarkup 只认元素起始
    size_t start = xml.find("<pdf2xml");
    if (start == NPOS) return nullptr;
    std::string doc;
    doc.reserve(xml.size() - start);
    for (size_t i = start; i < xml.size(); i++) {
        unsigned char ch = (unsigned char)xml[i];
        if (ch < 0x20 && ch != '\t' && ch != '\n' && ch != '\r') continue;
        doc.push_back((char)ch);
    }
    LayoutCtx ctx;
    ctx.tmpdir = tmpdir;
    GMarkupParser p;
    memset(&p, 0, sizeof(p));
    p.start_element = layout_start;
    p.end_element = layout_end;
    p.text = layout_text;
    GMarkupParseContext* gc =
        g_markup_parse_context_new(&p, (GMarkupParseFlags)0, &ctx, nullptr);
    GError* err = nullptr;
    gboolean ok = g_markup_parse_context_parse(gc, doc.data(), (gssize)doc.size(), &err);
    if (ok) ok = g_markup_parse_context_end_parse(gc, &err);
    if (err) g_error_free(err);
    g_markup_parse_context_free(gc);
    if (!ok || !ctx.out) return nullptr;
    if (ctx.out->w <= 0 || ctx.out->h <= 0) return nullptr;
    if (ctx.whole_page_img) return nullptr;
    if (ctx.out->texts.empty()) return nullptr;   // 纯图页/空页 -> 位图渲染
    return ctx.out;
}

}  // namespace

Renderer::Renderer(std::vector<unsigned char> pdf, const std::string& tmpdir)
    : tmpdir_(tmpdir) {
    pdf_path_ = join_path(tmpdir_, "doc.pdf");
    FILE* f = fopen(pdf_path_.c_str(), "wb");
    if (f) {
        if (!pdf.empty()) fwrite(pdf.data(), 1, pdf.size(), f);
        fclose(f);
    }
    cache_.clear();
    text_mode_ = probe_fonts();
}

Renderer::~Renderer() {
    std::unique_lock<std::mutex> lk(mu_);
    closing_ = true;
    // 等在跑的渲染线程结束，避免其继续访问已析构的成员
    // （pdftoppm/pdftohtml 均有超时看门狗，等待时间有上界）
    cv_.wait(lk, [this] { return running_ == 0; });
}

bool Renderer::probe_fonts() {
    // pdffonts 列出至少一种字体 -> 可能存在文本层
    RunResult r = run_capture({"pdffonts", pdf_path_}, "", 20000);
    if (!r.ok) return true;      // 探测失败则逐页尝试，失败自动回退位图
    int idx = 0;
    size_t pos = 0;
    bool any = false;
    while (pos <= r.out.size()) {
        size_t nl = r.out.find('\n', pos);
        std::string line = r.out.substr(pos, nl == NPOS ? NPOS : nl - pos);
        if (idx >= 2 && !trim(line).empty()) any = true;
        idx++;
        if (nl == NPOS) break;
        pos = nl + 1;
    }
    return any;
}

std::shared_ptr<const PageLayout> Renderer::render_layout(int n) {
    std::string ns = std::to_string(n);
    std::vector<std::string> args = {"pdftohtml", "-xml", "-q", "-noroundcoord",
                                     "-zoom",     "1",   "-f",  ns,  "-l",
                                     ns,          "-stdout", "-fmt", "png",
                                     pdf_path_};
    RunResult r = run_capture(args, dir_name(pdf_path_), 60000);
    if (!r.ok || r.status != 0 || r.out.empty()) return nullptr;
    return parse_layout_impl(r.out, tmpdir_);
}

std::shared_ptr<const Bitmap> Renderer::render_bitmap(int n) {
    // 每次渲染使用唯一前缀，避免并发渲染互相干扰/误删文件
    std::string prefix = join_path(dir_name(pdf_path_),
                                   "pg_" + std::to_string(n) + "_" + uuid_hex());
    // 输出 PPM（P6 原始像素）而非 PNG：光栅化成本相同，但省掉 PNG 编码
    // （实测 120dpi 单页 1.7s -> 0.3s）与解码（0.08s -> 0.01s）。
    // 注意 pdftoppm 没有 -ppm 开关，PPM 就是「不指定格式」时的默认输出；
    // 传 -ppm 会被当成非法参数直接报错。gdk-pixbuf 内置 PNM loader，无需新依赖。
    RunResult r = run_capture({"pdftoppm", "-f", std::to_string(n), "-l",
                               std::to_string(n), "-r", std::to_string(dpi_),
                               pdf_path_, prefix},
                              "", 30000);
    if (!r.ok) return nullptr;
    std::string img = find_first_match(dir_name(prefix), base_name(prefix) + "-", ".ppm");
    if (img.empty()) return nullptr;
    auto bmp = load_bitmap_file(img);
    remove(img.c_str());
    return bmp;
}

std::shared_ptr<const PageLayout> Renderer::parse_layout(const std::string& xml,
                                                         const std::string& tmpdir) {
    return parse_layout_impl(xml, tmpdir);
}

PagePtr Renderer::render(int n) {
    // 优先文本版面；失败或无文本（纯扫描页）回退位图
    if (text_mode_) {
        std::shared_ptr<const PageLayout> lay;
        try {
            lay = render_layout(n);
        } catch (...) {
            lay = nullptr;
        }
        if (lay) {
            auto p = std::make_shared<Page>();
            p->layout = lay;
            return p;
        }
    }
    std::shared_ptr<const Bitmap> bmp;
    try {
        bmp = render_bitmap(n);
    } catch (...) {
        bmp = nullptr;
    }
    if (!bmp) return nullptr;
    auto p = std::make_shared<Page>();
    p->bitmap = bmp;
    return p;
}

void Renderer::lru_touch(int n) {
    lru_.erase(std::remove(lru_.begin(), lru_.end(), n), lru_.end());
    lru_.push_back(n);
}

void Renderer::store(int n, PagePtr p) {
    std::lock_guard<std::mutex> lk(mu_);
    cache_[n] = p;
    lru_touch(n);
    while (lru_.size() > max_) {
        int old = lru_.front();
        lru_.erase(lru_.begin());
        cache_.erase(old);
    }
}

PagePtr Renderer::get(int n) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = cache_.find(n);
    if (it == cache_.end()) return nullptr;
    lru_touch(n);
    return it->second;
}

void Renderer::get_async(int n, PageCallback cb) {
    // 已在缓存则直接回调；渲染中的页可追加回调，随渲染完成一并触发
    PagePtr p = get(n);
    if (p) {
        cb(n, p);
        return;
    }
    bool first = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (closing_) return;
        auto& v = inflight_[n];
        v.push_back(cb);
        first = (v.size() == 1);
        if (first) running_++;
    }
    if (!first) return;
    std::thread([this, n] {
        PagePtr res;
        try {
            res = render(n);
        } catch (...) {
            res = nullptr;
        }
        std::vector<PageCallback> cbs;
        std::function<void()> wk;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = inflight_.find(n);
            if (it != inflight_.end()) {
                cbs = std::move(it->second);
                inflight_.erase(it);
            }
        }
        if (res) store(n, res);
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (auto& c : cbs) done_.emplace_back(c, n, res);
            running_--;
            wk = wakeup_;      // 先取出副本：解锁后本对象可能已被析构
        }
        cv_.notify_all();
        if (wk) wk();
    }).detach();
}

std::vector<std::tuple<PageCallback, int, PagePtr>> Renderer::drain() {
    std::vector<std::tuple<PageCallback, int, PagePtr>> out;
    std::lock_guard<std::mutex> lk(mu_);
    out.swap(done_);
    return out;
}

void Renderer::set_wakeup(std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(mu_);
    wakeup_ = std::move(fn);
}

}  // namespace caj
