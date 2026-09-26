// caj_core.hpp —— CAJ 文档解析 / PDF 提取重建 / 页面渲染后端
// 对应 caj_viewer.py 中的 CAJParser / extract_pdf / _build_hn_pdf /
// _kdh_decrypt / _prepare_doc / analyze_text / Renderer 等非 GUI 部分。
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

namespace caj {

// ---------------- 容器头部解析 ----------------
struct TocItem {
    int level = 0;          // 大纲层级（1 起）
    std::string title;      // UTF-8 标题
    int page = 0;
};

struct Info {
    std::string path;
    std::vector<unsigned char> magic;   // 文件头前 4 字节
    std::string fmt;                    // CAJ/HN/C8/%PDF/KDH/TEB，空表示未知
    long page_off = 0;                  // 0 表示无（Python 的 None/0 等价处理）
    long toc_off = 0;
    long toc_size = 0;
};

// 解析文件头。返回 false 时 err 为原因（文件不可读等）。
bool caj_open(const std::string& path, Info& out, std::string& err);

// 容器头声明的页数（%PDF 走 pdfinfo/正则统计；无 page_off 返回 0）。
int caj_page_num(const Info& info);

// 大纲项（仅 CAJ/HN 有效；标题按 gb18030 解码）。
std::vector<TocItem> caj_toc(const Info& info);

// 标准 PDF 页数：优先 pdfinfo，失败回退正则计数。
int pdf_page_count(const std::string& path);

// 格式分析文本（供“格式信息”对话框 / --info 使用）。
std::string analyze_text(const Info& info);

// ---------------- 文档装载 ----------------
// 解析并提取文档 -> (cap, pdf)。优先 sidecar 缓存；未命中按格式提取，
// 并异步写缓存。失败返回 false 且 err 有内容。
bool prepare_doc(const std::string& path, Info& cap,
                 std::vector<unsigned char>& pdf, std::string& err);

// ---------------- 页面版面模型 ----------------
struct Bitmap {
    int w = 0, h = 0;
    std::vector<unsigned char> rgb;     // w*h*3
};

struct FontSpec {
    double size = 12.0;
    std::string family;                 // PDF 里的原始字体名
    std::string color = "#000000";
};

struct TextRun {
    double x = 0, y = 0, w = 0, h = 0;  // PDF 磅，左上原点
    std::string text;                   // UTF-8
    int font = -1;
    bool bold = false, italic = false;
};

struct ImgRun {
    double x = 0, y = 0, w = 0, h = 0;
    std::shared_ptr<const Bitmap> bmp;
};

struct PageLayout {
    double w = 0, h = 0;
    std::map<int, FontSpec> fonts;
    std::vector<TextRun> texts;
    std::vector<ImgRun> imgs;
};

struct Page {
    std::shared_ptr<const PageLayout> layout;   // 文本页
    std::shared_ptr<const Bitmap> bitmap;       // 扫描位图页
};

using PagePtr = std::shared_ptr<const Page>;
using PageCallback = std::function<void(int, PagePtr)>;

// ---------------- 渲染器（后台线程 + LRU 缓存） ----------------
class Renderer {
public:
    // pdf 为重建后的 PDF 字节，tmpdir 为其落盘目录（须已存在）。
    Renderer(std::vector<unsigned char> pdf, const std::string& tmpdir);
    ~Renderer();

    const std::string& path() const { return pdf_path_; }

    // 读缓存；未命中返回 nullptr（绝不触发同步渲染）。
    PagePtr get(int n);

    // 异步渲染第 n 页，完成后经 drain() 回调（线程安全）。
    void get_async(int n, PageCallback cb);

    // 取回已完成的渲染结果（须在 GUI 主线程调用）。
    std::vector<std::tuple<PageCallback, int, PagePtr>> drain();

private:
    PagePtr render(int n);
    std::shared_ptr<const PageLayout> render_layout(int n);
    std::shared_ptr<const Bitmap> render_bitmap(int n);
    std::shared_ptr<const PageLayout> parse_layout(const std::string& xml, const std::string& tmpdir);
    bool probe_fonts();

    std::string pdf_path_;
    std::string tmpdir_;
    bool text_mode_ = true;
    std::mutex mu_;
    std::map<int, PagePtr> cache_;
    std::vector<int> lru_;
    std::map<int, std::vector<PageCallback>> inflight_;
    std::vector<std::tuple<PageCallback, int, PagePtr>> done_;
    size_t max_ = 10;
    int dpi_ = 120;
};

}  // namespace caj
