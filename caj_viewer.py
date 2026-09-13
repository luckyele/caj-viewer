#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
caj_viewer.py —— 中国知网 CAJ 文档阅读器

功能
----
1. 分析 .caj 文件的格式（魔数、格式类别、版本、页数、大纲等）。
2. 提取并重建 CAJ 容器内嵌的 PDF 流（纯 Python 实现，无需 mutool）。
3. 用 poppler 的 pdftoppm 渲染页面并用 tkinter 以 GUI 形式阅读。

用法
----
    python3 caj_viewer.py 文献.caj            # 打印格式分析并打开阅读窗口
    python3 caj_viewer.py 文献.caj --info     # 仅打印格式分析，不打开窗口

依赖
----
    python3 + tkinter + Pillow + poppler-utils(pdftoppm)
"""
import os
import re
import io
import sys
import time
import glob
import ctypes
import uuid
import queue
import shutil
import struct
import tempfile
import threading
import subprocess
from collections import OrderedDict

try:
    from PIL import Image, ImageTk
    import tkinter as tk
    from tkinter import ttk
    import tkinter.font as tkfont
except Exception as e:                     # 仅 --info 模式下可以不需要 GUI
    if "--info" not in sys.argv:
        raise SystemExit("缺少 GUI 依赖: %s" % e)


# ---- 可选：HN/C8 私格式 JBig 解码器 ----
# caj2pdf 项目（Hin-Tak Leung, 2020-2021）逆向的 libreaderex 内部 JBig
# 解码器，配套仓库下的 libjbigdec.so（与 JBigDecode.{h,cc} 一同编译）。
# 解码 HN/C8 文件里 typ=0 的页图像（48 字节 DIB 头 + 私格式 JBig 流）。
_JBIG_LIB = None
_JBIG_SO_CANDIDATES = [
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "libjbigdec.so"),
    os.path.expanduser("~/.local/lib/libjbigdec.so"),
    "libjbigdec.so",
]
for _cand in _JBIG_SO_CANDIDATES:
    try:
        _JBIG_LIB = ctypes.CDLL(_cand)
        _JBIG_LIB.jbigDecode.restype = None
        _JBIG_LIB.jbigDecode.argtypes = [
            ctypes.c_char_p, ctypes.c_uint, ctypes.c_uint,
            ctypes.c_uint, ctypes.c_uint, ctypes.c_char_p,
        ]
        break
    except Exception:
        _JBIG_LIB = None
del _cand, _JBIG_SO_CANDIDATES

# ---------------- CAJ 头部解析 ----------------
def _pdf_page_count(path):
    """统计标准 PDF 的页数：优先用 poppler 的 pdfinfo，失败则回退正则计数。"""
    try:
        out = subprocess.run(["pdfinfo", path], capture_output=True,
                             text=True, timeout=10).stdout
        m = re.search(r"^Pages:\s+(\d+)", out, re.M)
        if m:
            return int(m.group(1))
    except Exception:
        pass
    with open(path, "rb") as f:
        d = f.read()
    return len(re.findall(rb"/Type\s*/Page[^s]", d))


# 已知的知网 CAJ 私有魔数：
#   CAJ   -> 专有 CAJ 容器（内部嵌套 PDF 对象流，本程序主要支持）
#   HN    -> 另一类专有格式（HN 头，未完整支持正文渲染）
#   \xC8  -> C8 变体
#   %PDF  -> 其实是标准 PDF，直接改后缀即可
#   KDH / TEB -> 其他变体
class CAJParser:
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            head = f.read(4)
        self.magic = head
        m0 = head[0:1]
        if m0 == b"\xc8":
            self.fmt = "C8"; self.page_off = 0x08
            self.toc_off = 0; self.toc_size = 0
        elif head[0:2] == b"HN":
            self.fmt = "HN"; self.page_off = 0x90
            self.toc_off = 0x158; self.toc_size = 0x134
        else:
            fmt = head.rstrip(b"\x00").decode("latin-1")
            self.fmt = fmt if fmt in ("CAJ", "HN", "%PDF", "KDH", "TEB") else None
            if self.fmt == "CAJ":
                self.page_off = 0x10; self.toc_off = 0x110; self.toc_size = 0x134
            elif self.fmt == "HN":
                self.page_off = 0x90; self.toc_off = 0x158; self.toc_size = 0x134
            else:
                self.page_off = self.toc_off = self.toc_size = None

    @property
    def page_num(self):
        if self.fmt == "%PDF":
            return _pdf_page_count(self.path)
        if not self.page_off:
            return 0
        with open(self.path, "rb") as f:
            f.seek(self.page_off); return struct.unpack("i", f.read(4))[0]

    def toc(self):
        """返回 [(level, title, page)]，仅在 CAJ / HN 下有效。"""
        if not self.toc_off:
            return []
        out = []
        with open(self.path, "rb") as f:
            f.seek(self.toc_off)
            (n,) = struct.unpack("i", f.read(4))
            for _ in range(n):
                raw = f.read(self.toc_size)
                u = struct.unpack("256s24s12s12si", raw)
                ttl = u[0].split(b"\x00", 1)[0].decode("gb18030", "replace")
                pg = u[2].split(b"\x00", 1)[0]
                try:
                    page = int(pg)
                except ValueError:
                    page = 0
                out.append((u[4], ttl, page))
        return out

# ---------------- 提取并重建内嵌 PDF ----------------
def extract_pdf(cap, blob=None):
    """CAJ 容器第 0x14 字节起存有内嵌 PDF 流的起始指针。
    读取全部 PDF 对象、补全目录(Catalog)/根 Pages 与 xref，产出一个可用 PDF。
    blob: 预先读好的整文件 bytes（可避免重复 IO，None 时内部读）。"""
    if blob is None:
        with open(cap.path, "rb") as f:
            blob = f.read()
    d = blob
    ptr = struct.unpack("i", d[0x14:0x18])[0]
    pdf_start = struct.unpack("i", d[ptr:ptr + 4])[0]
    blob = d[pdf_start:]
    endobj = [i for i in range(len(blob)) if blob.startswith(b"endobj", i)]
    if not endobj:
        raise ValueError("未在 CAJ 中找到内嵌 PDF 对象(endobj)。")
    body = blob[:endobj[-1] + 6]

    # 收集所有唯一对象号 -> 其在 body 中的起始偏移
    headers = [(int(m.group(1)), m.start()) for m in re.finditer(rb"([0-9]+) 0 obj", body)]
    headers.sort(key=lambda t: t[1])
    blocks = {}
    for e in endobj:
        # 找到覆盖该 endobj 的最后一个对象头
        lo, hi = 0, len(headers)
        while lo < hi:
            mid = (lo + hi) // 2
            if headers[mid][1] <= e:
                lo = mid + 1
            else:
                hi = mid
        no, s = headers[lo - 1]
        blocks.setdefault(no, s)          # 保留首次出现的偏移

    def classify(no):
        """解析单个对象字典字段：类型(Page/Pages/Catalog)、Parent、Kids。"""
        content = body[blocks[no]:]
        seg_end = content.find(b"stream")
        if seg_end == -1:
            seg_end = content.find(b"endobj")
        dreg = content if seg_end == -1 else content[:seg_end]
        m = re.search(rb"/Type\s*/([A-Za-z]+)", dreg)
        typ = m.group(1).decode() if m else None
        parent = None
        mp = re.search(rb"/Parent\s*(\d+)\s+0\s+R", dreg)
        if mp:
            parent = int(mp.group(1))
        kids = []
        mk = re.search(rb"/Kids\s*\[([^\]]*)\]", dreg)
        if mk:
            kids = [int(x) for x in re.findall(rb"(\d+)\s+0\s+R", mk.group(1))]
        return typ, parent, kids

    nodes = {}
    cat_no = None
    for no in blocks:
        typ, parent, kids = classify(no)
        if typ in ("Page", "Pages", "Catalog"):
            nodes[no] = (typ, parent, kids)
            if typ == "Catalog":
                cat_no = no

    # 找出所有 `Pages` 节点，并确定根节点（不被任何 Pages 节点作为子节点引用）
    page_leaf = [n for n, (t, p, k) in nodes.items() if t == "Page"]
    parent_targets = {p for (t, p, k) in nodes.values() if t == "Pages" and p}
    roots = [n for n, (t, p, k) in nodes.items() if t == "Pages" and n not in parent_targets]

    # 按 /Kids 顺序 DFS 收集叶子页，得到真实文档页序（不能按对象号排序！）
    leaves = []
    seen = set()
    def walk(n):
        if n in seen:
            return
        seen.add(n)
        if nodes.get(n, (None,))[0] == "Page":
            leaves.append(n)
            return
        for c in nodes.get(n, (None, None, []))[2]:
            walk(c)
    for r in roots:
        walk(r)
    if not leaves:                      # 兜底：树解析失败则退回原始顺序
        leaves = page_leaf

    out = bytearray(b"%PDF-1.3\r\n" + body)
    # 新建一个根 Pages，按真实文档顺序挂接全部叶子页
    root_pages = max(max(blocks, default=0) + 1, max(leaves, default=0) + 1, 1)
    kids = " ".join("%d 0 R" % p for p in leaves)
    txt = "%d 0 obj\r<</Type /Pages /Kids [%s] /Count %d>>\rendobj\r" % (
        root_pages, kids, len(leaves))
    out += txt.encode(); blocks[root_pages] = len(body)
    # 若没有 Catalog，则新建一个指向根 Pages
    if cat_no is None:
        cat_no = max(max(blocks, default=0) + 1, root_pages + 1, 1)
        txt = "%d 0 obj\r<</Type /Catalog /Pages %d 0 R>>\rendobj\r" % (cat_no, root_pages)
        out += txt.encode(); blocks[cat_no] = len(body)

    # 计算每个对象在输出文件中的绝对偏移
    abs_off = {}
    for m in re.finditer(rb"([0-9]+) 0 obj", bytes(out)):
        no = int(m.group(1))
        abs_off.setdefault(no, m.start())

    size = cat_no + 1
    xref = bytearray(b"xref\r\n0 %d\r\n" % size)
    xref += b"0000000000 65535 f\r\n"
    for no in range(1, size):
        if no in abs_off:
            xref += b"%010d 00000 n\r\n" % abs_off[no]
        else:
            xref += b"0000000000 65535 f\r\n"
    startxref = len(out)
    trailer = ("trailer\r\r<</Size %d /Root %d 0 R>>\rstartxref\r%d\r%%%%EOF\r"
               % (size, cat_no, startxref)).encode()
    out += xref + trailer
    return bytes(out), len(leaves)

# ---------------- HN 格式：从页图像构建 PDF ----------------
# HN(含 C8 变体)没有内嵌 PDF 流，正文为专有 JBig 编码 + 少量标准 JPEG。
# 这里解析页信息表，把 JPEG 页直接嵌入 PDF；JBig 页生成“无法解码”占位单色页。
# 这样查看器可正常打开/翻页/缩放/大纲跳转，遇到 JBig 页有明确提示而非崩溃。
def _hn_offsets(d):
    """返回 (toc_end, page_entries)。page_entries 为每个逻辑页的
    (date_offset, text_size, images_per_page, page_no)。"""
    toc_num = struct.unpack("i", d[0x158:0x15C])[0]
    toc_end = 0x158 + 4 + 0x134 * toc_num
    entries = []
    for i in range(struct.unpack("i", d[0x90:0x94])[0]):
        off = toc_end + i * 20
        if off + 20 > len(d):
            break
        pdoff, tsize, imgs, pno, _, _ = struct.unpack("iihhii", d[off:off + 20])
        entries.append((pdoff, tsize, imgs, pno))
    return toc_end, entries


def _jbig_placeholder_jpeg(w, h):
    """生成一张带提示文字的占位 JPEG（供 JBig 页嵌入）。
    尺寸按 JBig 页真实宽高，居中提示“专有编码无法解码”。"""
    from PIL import Image as _Img, ImageDraw as _Idr, ImageFont as _Ifont
    im = _Img.new("RGB", (w, h), (246, 241, 228))       # 暖纸色背景
    dr = _Idr.Draw(im)
    font = None
    for fp in ("/usr/share/fonts/opentype/noto/NotoSerifCJK-Bold.ttc",
               "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc",
               "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc"):
        try:
            font = _Ifont.truetype(fp, max(24, min(w, h) // 30))
            break
        except Exception:
            continue
    lines = ["此页为 CAJViewer 专有 JBig 编码图像", "当前版本暂无法解码显示"]
    if font is None:
        lines = ["CAJViewer proprietary JBig encoded page", "not decodable"]
        font = _Ifont.load_default()
    # 计算整体高度以垂直居中
    lh = font.getbbox("中")[3] - font.getbbox("中")[1] + 40
    total_h = lh * len(lines) + 40
    y = (h - total_h) // 2
    for ln in lines:
        bb = font.getbbox(ln)
        tw = bb[2] - bb[0]
        dr.text(((w - tw) // 2, y), ln, font=font, fill=(44, 40, 33))
        y += lh
    import io as _io
    buf = _io.BytesIO()
    im.save(buf, "JPEG", quality=85)
    return buf.getvalue()


def _decode_hn_jbig(buf):
    """HN/C8 私有的 typ=0 页图像：DIB 头(48B) + 私格式 JBig 压缩流。
    借助本目录下 libjbigdec.so（来自 caj2pdf / Hin-Tak Leung）解码为 1bit
    packed 灰度图，再经 PIL 转 JPEG 返回 (width, height, jpeg_bytes)。
    解码失败返回 None，调用方应回退到占位图。"""
    if len(buf) < 48:
        return None
    w, h, planes, bpp = struct.unpack("<IIHH", buf[4:16])
    if w <= 0 or h <= 0 or bpp <= 0 or planes <= 0:
        return None
    bpl = ((w * bpp + 31) >> 5) << 2       # 行字节数，向 4B 对齐
    body = buf[48:]
    if not body:
        return None
    if _JBIG_LIB is None:
        return None
    try:
        out = ctypes.create_string_buffer(h * bpl)
        _JBIG_LIB.jbigDecode(body, len(body), h, w, bpl, out)
    except Exception:
        return None
    raw = bytes(out)[: h * bpl]
    # JBIG 是 bottom-up 存储，先在解码位图里垂直翻转
    im = Image.frombytes("1", (bpl * 8, h), raw, "raw", "1;I", bpl, 1)
    im = im.crop((0, 0, w, h)).transpose(Image.FLIP_TOP_BOTTOM).convert("L")
    buf_jpg = io.BytesIO()
    im.save(buf_jpg, "JPEG", quality=85, optimize=True)
    return w, h, buf_jpg.getvalue()


def _build_hn_pdf(d):
    """把 HN/C8 的页图像重组成标准 PDF。JPEG 页直接嵌入；JBig（typ=0）
    页经 libjbigdec.so 解码为灰度 JPEG 后嵌入；解码失败回退到占位图。"""
    _, entries = _hn_offsets(d)
    n = len(entries)
    base_w, base_h = 1600, 2400            # 无法解析尺寸时的兜底

    # 预解析每页图像：JPEG 直接嵌入；JBig 解码为 JPEG；无法解码则占位
    pages = []                             # (w, h, jpeg_bytes)
    for pdoff, tsize, _imgs, _pno in entries:
        cur = pdoff + tsize
        if cur + 12 > len(d):
            pages.append((base_w, base_h,
                          _jbig_placeholder_jpeg(base_w, base_h)))
            continue
        typ, ioff, isize = struct.unpack("iii", d[cur:cur + 12])
        buf = d[ioff:ioff + isize] if 0 <= ioff and isize > 0 and ioff + isize <= len(d) else b""
        # 标准 JPEG（typ=1/2）：直接嵌入
        if typ in (1, 2) and buf[:2] == b"\xff\xd8":
            try:
                im = Image.open(io.BytesIO(buf)); im.load()
                pages.append((im.width, im.height, buf))
                continue
            except Exception:
                pass
        # 私有 JBig（typ=0）：用 libjbigdec.so 解码
        if typ == 0 and _JBIG_LIB is not None:
            jpg = _decode_hn_jbig(buf)
            if jpg is not None:
                pages.append(jpg)
                continue
        # 兜底：占位白纸
        if len(buf) >= 12:
            jw = struct.unpack("i", buf[4:8])[0]
            jh = struct.unpack("i", buf[8:12])[0]
            if jw > 0 and jh > 0:
                base_w, base_h = jw, jh
        pages.append((base_w, base_h,
                      _jbig_placeholder_jpeg(base_w, base_h)))

    # 每页分配三个对象号：图像 / Contents / Page
    alloc = {}
    cur_no = 1
    for i in range(n):
        alloc["im%d" % i] = cur_no; cur_no += 1
        alloc["co%d" % i] = cur_no; cur_no += 1
        alloc["pg%d" % i] = cur_no; cur_no += 1
    root_pg = cur_no; cur_no += 1
    catalog = cur_no; cur_no += 1
    total = cur_no - 1

    body = bytearray()
    for i in range(n):
        w, h, data = pages[i]
        if data is None:
            data = _jbig_placeholder_jpeg(base_w, base_h)
        img_txt = ("%d 0 obj\r<</Type /XObject /Subtype /Image "
                   "/Width %d /Height %d /ColorSpace /DeviceRGB "
                   "/BitsPerComponent 8 /Filter /DCTDecode /Length %d>>\rstream\r"
                   % (alloc["im%d" % i], w, h, len(data)))
        body += img_txt.encode() + data + b"\rendstream\rendobj\r\r\n"
        # Contents 流：必须用显式缩放 cm 矩阵把图像铺满整页。
        # poppler(实测 26.x)对“恒等变换直接 Do”的整页图像会塌缩成
        # 约 1pt 的盒子而不可见（整页白屏）；cm 显式缩放后正常渲染。
        cs_txt = "q\r%d 0 0 %d 0 0 cm\r/Im%d Do\rQ" % (w, h, alloc["im%d" % i])
        len_cs = len(cs_txt)
        body += ("%d 0 obj\r<</Length %d>>\rstream\r" % (alloc["co%d" % i], len_cs)).encode()
        body += cs_txt.encode() + b"\rendstream\rendobj\r\r\n"
        # Page 对象
        pg_txt = ("%d 0 obj\r<</Type /Page /Parent %d 0 R "
                  "/MediaBox [0 0 %d %d] /Resources "
                  "<< /XObject << /Im%d %d 0 R >> >> "
                  "/Contents %d 0 R >>\rendobj\r" %
                  (alloc["pg%d" % i], root_pg, w, h,
                   alloc["im%d" % i], alloc["im%d" % i], alloc["co%d" % i]))
        body += pg_txt.encode() + b"\r\r\n"
    kids_str = " ".join("%d 0 R" % alloc["pg%d" % i] for i in range(n))
    body += ("%d 0 obj\r<</Type /Pages /Kids [%s] /Count %d>>\rendobj\r"
             % (root_pg, kids_str, n)).encode() + b"\r\n"
    body += ("%d 0 obj\r<</Type /Catalog /Pages %d 0 R>>\rendobj\r"
             % (catalog, root_pg)).encode() + b"\r\n"

    out = bytearray(b"%PDF-1.4\r\n") + body
    xref_off = {}
    for m in re.finditer(rb"([0-9]+) 0 obj", bytes(out)):
        xref_off.setdefault(int(m.group(1)), m.start())
    size = total + 1
    xref = bytearray(b"xref\r\n0 %d\r\n" % size + b"0000000000 65535 f\r\n")
    for no in range(1, size):
        xref += (b"%010d 00000 n\r\n" % xref_off[no]) if no in xref_off \
                else b"0000000000 65535 f\r\n"
    startxref = len(out)
    trailer = ("trailer\r\r<</Size %d /Root %d 0 R>>\rstartxref\r%d\r%%%%EOF\r"
               % (size, catalog, startxref)).encode()
    out += xref + trailer
    return bytes(out), n

# ---------------- 渲染器 ----------------
class Renderer:
    """页面渲染器：pdftoppm 在后台线程执行，主线程绝不阻塞。
    渲染完成结果放入队列，由 GUI 定时轮询取回。"""
    def __init__(self, pdf_bytes, tmpdir):
        self.pdf = os.path.join(tmpdir, "doc.pdf")
        with open(self.pdf, "wb") as f:
            f.write(pdf_bytes)
        self.cache = OrderedDict()
        self._lock = threading.Lock()
        self._inflight = {}                # 页号 -> [回调列表]，同一页多次请求可复用
        self._done = queue.Queue()         # (cb, page, img) 结果队列
        self.max = 10                      # LRU 缓存页数（含预取相邻页）
        self.dpi = 120

    def _store(self, n, img):
        with self._lock:
            self.cache[n] = img
            self.cache.move_to_end(n)
            while len(self.cache) > self.max:
                self.cache.popitem(last=False)

    def get(self, n):
        """读取缓存；未命中返回 None，绝不触发同步渲染。"""
        with self._lock:
            if n in self.cache:
                self.cache.move_to_end(n)
                return self.cache[n]
        return None

    def _render(self, n):
        # 每次渲染使用唯一前缀，避免并发渲染互相干扰/误删文件
        out = os.path.join(os.path.dirname(self.pdf), "pg_%d_%s" % (n, uuid.uuid4().hex))
        subprocess.run(["pdftoppm", "-f", str(n), "-l", str(n), "-png",
                        "-r", str(self.dpi), self.pdf, out],
                       check=True, timeout=30,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        pngs = glob.glob(out + "-*.png")
        if not pngs:
            raise FileNotFoundError("pdftoppm 未生成任何页面图像（页号 %d）" % n)
        png = pngs[0]
        try:
            return Image.open(png).convert("RGB")
        finally:
            os.remove(png)

    def get_async(self, n, cb):
        """异步渲染第 n 页，完成后回调 cb(n, img)（img 可能为 None）。
        已在缓存则直接回调；渲染中的页可追加回调，随渲染完成一并触发。"""
        img = self.get(n)
        if img is not None:
            cb(n, img)
            return
        with self._lock:
            cbs = self._inflight.setdefault(n, [])
            cbs.append(cb)
            first = len(cbs) == 1
        if not first:
            return
        def worker():
            try:
                im = self._render(n)
            except Exception:
                im = None
            with self._lock:
                cbs = self._inflight.pop(n, [])
            if im is not None:
                self._store(n, im)
            for c in cbs:
                self._done.put((c, n, im))
        threading.Thread(target=worker, daemon=True).start()

    def drain(self):
        """取回所有已完成的渲染结果（由 GUI 主线程调用）。"""
        out = []
        while True:
            try:
                out.append(self._done.get_nowait())
            except queue.Empty:
                break
        return out

# ---------------- GUI ----------------
# 设计主题：“夜读书房”——深黛青墨色书桌上，暖纸页卡浮起，
# 黄铜(bronze)为唯一强调色；签名元素为底部黄铜“页垛”阅读进度条。
PAL = dict(
    desk="#232B31",        # 桌面主背景（墨蓝黑）
    panel="#2E3942",       # 工具栏/侧栏面板
    panel_edge="#3D4A54",  # 面板描边/分隔
    paper="#F6F1E4",       # 暖纸（页卡）
    page_bg="#EFE8D7",     # 页卡内衬
    paper_shadow="#1A2025",# 页卡阴影
    ink="#2C2821",         # 纸上墨色文字
    brass="#C8A43A",       # 黄铜强调（唯一亮色）
    brass_dim="#8F7B33",   # 黄铜暗调（描边）
    muted="#9AA69E",       # 次要文字（暗底上可读）
)

_FONT_CACHE = {}
def _font(role, size, weight="normal"):
    """按角色挑选可用字体：ui=中文界面无衬线，serif=书卷衬线，mono=等宽数字。"""
    key = (role, size, weight)
    if key in _FONT_CACHE:
        return _FONT_CACHE[key]
    try:
        avail = set(tkfont.families())
    except Exception:
        avail = set()
    fam = {
        "ui": ["Noto Sans CJK SC", "Source Han Sans SC", "PingFang SC",
               "Microsoft YaHei UI", "Microsoft YaHei", "WenQuanYi Micro Hei",
               "DejaVu Sans"],
        "serif": ["Noto Serif CJK SC", "Source Han Serif SC", "Songti SC",
                  "STSong", "SimSun", "DejaVu Serif"],
        "mono": ["DejaVu Sans Mono", "Noto Sans Mono CJK SC", "Consolas",
                 "Menlo", "Monospace"],
    }[role]
    for f in fam:
        if f in avail:
            r = (f, size) if weight == "normal" else (f, size, weight)
            _FONT_CACHE[key] = r
            return r
    r = ("TkDefaultFont", size) if weight == "normal" else ("TkDefaultFont", size, weight)
    _FONT_CACHE[key] = r
    return r

def _rr(c, x1, y1, x2, y2, r, **kw):
    """圆角矩形（smooth 多边形近似）。"""
    pts = [x1 + r, y1, x2 - r, y1, x2, y1, x2, y1 + r,
           x2, y2 - r, x2, y2, x2 - r, y2, x1 + r, y2,
           x1, y2, x1, y2 - r, x1, y1 + r, x1, y1]
    return c.create_polygon(pts, smooth=True, **kw)

class Viewer:
    def __init__(self, path):
        self.path = path
        self.cap = CAJParser(path)
        self.tmp = tempfile.mkdtemp(prefix="cajview_")
        t0 = time.monotonic()
        # 1) 优先尝试从 sidecar 缓存读提取结果（同一文件二次打开近瞬时）
        cache_pdf = self._try_load_cache()
        if cache_pdf is not None:
            self.pdf = cache_pdf
        else:
            try:
                # 一次 read 复用给所有需要整文件的分支
                with open(path, "rb") as f:
                    raw = f.read()
                if self.cap.fmt == "HN":
                    self.pdf, _ = _build_hn_pdf(raw)
                elif self.cap.fmt == "%PDF":
                    self.pdf = raw                       # 本质是标准 PDF，直接使用
                else:
                    self.pdf, _ = extract_pdf(self.cap, blob=raw)
            except Exception as e:
                raise SystemExit("内嵌 PDF 提取失败: %s" % e)
            # 2) 解析成功后写 sidecar 缓存（异步线程，避免阻塞主线程）
            threading.Thread(target=self._save_cache, args=(self.pdf,),
                             daemon=True).start()
        # 3) Renderer 创建
        self.rd = Renderer(self.pdf, self.tmp)
        self._t_extract = time.monotonic() - t0
        self.N = self.cap.page_num
        self.cur = 1
        self.zoom = 1.0
        self.scroll = 0                  # 当前页内纵向滚动偏移（显示像素）
        self._max_scroll = 0             # 当前页可滚动上限（页面高度 - 视口高度）
        self._flip_until = 0.0           # 翻页后的“落位保护”截止时间
        self._disp = None                # 当前已绘制的显示尺寸 (dw, dh)
        self._img_id = None              # 画布上页面图像项的 id
        self._card_ids = []              # 页卡所有画布项（阴影/纸/图像）
        self._resize_job = None          # 窗口尺寸变化的防抖句柄
        self._foot_h = 44                # 底部页垛页脚高度（滚动可视区须扣除）

        # className 使 WM_CLASS 与 .desktop 的 StartupWMClass 一致，
        # GNOME Dock 才能把运行窗口关联到应用图标（否则显示通用齿轮）。
        self.root = tk.Tk(className="caj-viewer")
        self.root.title("CAJ 阅读器 - %s" % os.path.basename(path))
        self.root.geometry("800x600")        # 启动窗口尺寸
        self.root.minsize(640, 480)         # 防止窗口缩得过小导致布局错乱
        self._set_icon()
        self._setup_style()
        self._build_toolbar()
        self._build_toc()
        self._build_canvas()
        self.root.bind("<Configure>", self._on_configure)
        self._poll_render()              # 启动后台渲染结果轮询
        self.root.bind("<Left>", lambda e: self.prev())
        self.root.bind("<Right>", lambda e: self.next())
        self.root.bind("<Prior>", lambda e: self.prev())
        self.root.bind("<Next>", lambda e: self.next())
        self.root.bind("<plus>", lambda e: self.zoom_in())        # 主键盘 +
        self.root.bind("<equal>", lambda e: self.zoom_in())      # 某些布局下 + 是 =
        self.root.bind("<KP_Add>", lambda e: self.zoom_in())     # 小键盘 +
        self.root.bind("<minus>", lambda e: self.zoom_out())     # 主键盘 -
        self.root.bind("<KP_Subtract>", lambda e: self.zoom_out())  # 小键盘 -
        # 滚动（绑定在页面画布上，不影响大纲列表的原生滚动）：
        # Linux(X11) 下鼠标滚轮/触摸板双指为 Button-4/5，Windows/macOS 为 MouseWheel。
        # 先在当前页内纵向移动；越过页底才翻下一页，回到页首继续上翻上一页。
        self.canvas.bind("<Button-4>", lambda e: self._wheel_page(-1))
        self.canvas.bind("<Button-5>", lambda e: self._wheel_page(1))
        self.canvas.bind("<MouseWheel>", self._on_mousewheel)
        self._set_page(1)
        # 4) 预热：在 mainloop 启动前就触发首页渲染，窗口一亮就有图
        self.rd.get_async(1, lambda _n, _img: None)

    # ----- sidecar 缓存：把"提取/重建 PDF"的结果落到 <源文件>.cajviewer.pdf -----
    def _cache_path(self):
        """sidecar 路径：源文件同目录下 <name>.cajviewer.pdf。
        源文件不可写时（只读介质）会落到 ~/.cache/caj-viewer/。"""
        try:
            p = self.path + ".cajviewer.pdf"
            # 检查同目录是否可写
            probe = os.path.join(os.path.dirname(p), ".cajviewer.write_probe")
            with open(probe, "wb") as f:
                f.write(b"x")
            os.remove(probe)
            return p
        except (OSError, IOError):
            import hashlib
            h = hashlib.sha1(os.path.abspath(self.path).encode()).hexdigest()[:16]
            d = os.path.expanduser("~/.cache/caj-viewer")
            try:
                os.makedirs(d, exist_ok=True)
            except OSError:
                return None
            return os.path.join(d, "%s.cajviewer.pdf" % h)

    def _try_load_cache(self):
        """缓存命中条件：源文件 mtime+size 与缓存头一致；读头 32 字节校验。"""
        cp = self._cache_path()
        if cp is None or not os.path.exists(cp):
            return None
        try:
            st_src = os.stat(self.path)
            with open(cp, "rb") as f:
                head = f.read(64)
            # 缓存头：源 mtime(ns)|源 size|fmt
            src_mtime = struct.pack("q", int(st_src.st_mtime * 1e9))
            src_size = struct.pack("q", st_src.st_size)
            fmt = self.cap.fmt.encode() if self.cap.fmt else b"?"
            if not head.startswith(src_mtime + src_size + fmt):
                return None
            with open(cp, "rb") as f:
                f.seek(64)
                return f.read()
        except (OSError, IOError, struct.error):
            return None

    def _save_cache(self, pdf_bytes):
        cp = self._cache_path()
        if cp is None:
            return
        try:
            st_src = os.stat(self.path)
            head = (struct.pack("q", int(st_src.st_mtime * 1e9)) +
                    struct.pack("q", st_src.st_size) +
                    (self.cap.fmt or "?").encode())
            tmp = cp + ".tmp"
            with open(tmp, "wb") as f:
                f.write(head)
                f.write(pdf_bytes)
            os.replace(tmp, cp)
        except (OSError, IOError):
            try:
                os.remove(tmp)
            except OSError:
                pass

    def _set_icon(self):
        """设置窗口图标：X11 标题栏 + Wayland 任务栏。
        Tk 内置的 PhotoImage 在某些发行版不包含 PNG handler（取决于编译选项），
        所以用 PIL 的 ImageTk.PhotoImage 加载，跨平台都可靠。"""
        from PIL import Image, ImageTk
        candidates = [
            os.path.join(os.path.dirname(os.path.abspath(__file__)), "appicon.png"),
            "/usr/share/icons/hicolor/256x256/apps/caj-viewer.png",
            os.path.expanduser("~/.local/usr/share/icons/hicolor/256x256/apps/caj-viewer.png"),
            os.path.expanduser("~/.local/share/icons/hicolor/256x256/apps/caj-viewer.png"),
        ]
        for p in candidates:
            if not os.path.exists(p):
                continue
            try:
                icon = ImageTk.PhotoImage(Image.open(p))
                # True = 同样应用到后续所有 Toplevel（如格式分析对话框）
                self.root.iconphoto(True, icon)
                self._icon_ref = icon          # 保引用，防 GC
                return
            except Exception as e:
                print("[caj-viewer] 加载图标失败: %s: %s" % (p, e), file=sys.stderr)
                continue
        print("[caj-viewer] 警告：未找到可用图标 (候选: %s)" % candidates, file=sys.stderr)

    def _setup_style(self):
        """ttk 主题：以 clam 为基础，统一暗色面板 + 黄铜强调。"""
        self.root.configure(bg=PAL["desk"])
        try:
            s = ttk.Style(self.root)
            try:
                s.theme_use("clam")
            except tk.TclError:
                pass
            s.configure(".", background=PAL["desk"], foreground=PAL["paper"])
            s.configure("Toolbar.TFrame", background=PAL["panel"])
            s.configure("Side.TFrame", background=PAL["panel"])
            s.configure("TButton",
                        background=PAL["panel"], foreground=PAL["paper"],
                        bordercolor=PAL["panel_edge"], lightcolor=PAL["panel_edge"],
                        darkcolor=PAL["panel_edge"], focusthickness=0,
                        padding=(12, 5), font=_font("ui", 10))
            s.map("TButton",
                  background=[("active", PAL["panel_edge"]), ("pressed", PAL["brass"])],
                  foreground=[("active", PAL["paper"]), ("pressed", PAL["ink"])])
            s.configure("Page.TLabel", background=PAL["brass"], foreground=PAL["ink"],
                        font=_font("mono", 11, "bold"), padding=(12, 3))
            s.configure("Title.TLabel", background=PAL["panel"], foreground=PAL["muted"],
                        font=_font("ui", 10))
            s.configure("TCheckbutton", background=PAL["panel"], foreground=PAL["muted"],
                        font=_font("ui", 10))
            s.map("TCheckbutton",
                  background=[("active", PAL["panel"])],
                  foreground=[("active", PAL["paper"])])
            s.configure("TScrollbar", background=PAL["panel_edge"], troughcolor=PAL["panel"],
                        arrowcolor=PAL["muted"], bordercolor=PAL["panel"])
        except Exception:
            pass

    def _build_toolbar(self):
        bar = ttk.Frame(self.root, style="Toolbar.TFrame")
        bar.pack(side=tk.TOP, fill=tk.X)
        ttk.Label(bar, text=os.path.basename(self.path)[:36],
                  style="Title.TLabel").pack(side=tk.LEFT, padx=(12, 6), pady=4)
        ttk.Separator(bar, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=4, pady=4)
        for txt, cmd in (("首页", self.first), ("上一页", self.prev),
                         ("下一页", self.next), ("末页", self.last)):
            ttk.Button(bar, text=txt, command=cmd).pack(side=tk.LEFT, padx=2, pady=3)
        ttk.Separator(bar, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=4, pady=4)
        for txt, cmd in (("放大", self.zoom_in), ("缩小", self.zoom_out),
                         ("格式信息", self.show_info)):
            ttk.Button(bar, text=txt, command=cmd).pack(side=tk.LEFT, padx=2, pady=3)
        # 黄铜“书签”页码
        self.page_lbl = ttk.Label(bar, text="1 / %d" % self.N, style="Page.TLabel")
        self.page_lbl.pack(side=tk.RIGHT, padx=12, pady=3)

    def _build_toc(self):
        self.toc_on = tk.BooleanVar(value=True)
        self._toc_frame = ttk.Frame(self.root, style="Side.TFrame")
        self._toc_frame.pack(side=tk.LEFT, fill=tk.Y)
        head = ttk.Frame(self._toc_frame, style="Side.TFrame")
        head.pack(fill=tk.X, padx=8, pady=(8, 4))
        ttk.Checkbutton(head, text="目录", var=self.toc_on,
                        command=lambda: self._toggle_toc()).pack(anchor=tk.W)
        body = ttk.Frame(self._toc_frame, style="Side.TFrame")
        body.pack(fill=tk.BOTH, expand=True, padx=(8, 2), pady=(0, 8))
        self.toc_box = tk.Listbox(body, width=30, font=_font("ui", 9),
                                  bg=PAL["panel"], fg=PAL["paper"],
                                  selectbackground=PAL["brass"], selectforeground=PAL["ink"],
                                  highlightthickness=0, borderwidth=0, activestyle="none",
                                  relief="flat", exportselection=False)
        self.toc_box.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        sb = ttk.Scrollbar(body, orient=tk.VERTICAL, command=self.toc_box.yview)
        sb.pack(side=tk.RIGHT, fill=tk.Y)
        self.toc_box.configure(yscrollcommand=sb.set)
        self.toc_box.bind("<<ListboxSelect>>", self._toc_jump)

    def _build_canvas(self):
        cont = ttk.Frame(self.root, style="Side.TFrame")
        cont.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        self.canvas = tk.Canvas(cont, bg=PAL["desk"], highlightthickness=0, borderwidth=0)
        self.canvas.pack(fill=tk.BOTH, expand=True)
        self.photo = None
        self.img_ref = None

    def _toggle_toc(self):
        if self.toc_on.get():
            self._toc_show()
        else:
            self._toc_hide()
        self._draw()

    def _toc_show(self):
        try:
            self._toc_frame.pack(side=tk.LEFT, fill=tk.Y)
        except tk.TclError:
            pass

    def _toc_hide(self):
        try:
            self._toc_frame.pack_forget()
        except tk.TclError:
            pass

    def _set_page(self, n, at_bottom=False):
        self.cur = max(1, min(self.N, n))
        self.page_lbl.config(text="%d / %d" % (self.cur, self.N))
        if self.toc_box.size() == 0:
            self._load_toc()
        # 翻页默认定位到页首；at_bottom 时先给一个超大偏移，
        # 由 _draw 按该页实际高度钳制到页底（连续滚动时的“回到上一页底部”）。
        self.scroll = 10 ** 9 if at_bottom else 0
        self._draw()

    def _load_toc(self):
        try:
            for lv, title, page in self.cap.toc():
                self.toc_box.insert(tk.END, "%s%s  · p%s" % ("   " * (lv - 1), title, page))
        except Exception:
            pass

    def _toc_jump(self, ev):
        sel = self.toc_box.curselection()
        if not sel:
            return
        lv, title, page = self.cap.toc()[sel[0]]
        if pages := [p for _, _, p in self.cap.toc()]:
            self._set_page(max(1, min(page, self.N)))

    def _display_scale(self, img_w):
        # 以“宽度适配”为基准：页面宽度铺满视口宽度，页面纵向通常高于
        # 视口，从而支持页内滚动；zoom 作为额外倍率。
        cw = max(self.canvas.winfo_width(), 50)
        return (cw / img_w) * self.zoom

    def _draw(self):
        """页面/缩放/尺寸变化时重绘。未缓存则先显示占位并异步渲染，不阻塞。"""
        img = self.rd.get(self.cur)
        if img is None:
            self._show_placeholder()
            self.rd.get_async(self.cur, self._on_page_ready)
            return
        self._paint(img)
        self._prefetch(self.cur)

    def _paint(self, img):
        sc = self._display_scale(img.width)
        dw = max(1, int(img.width * sc))
        dh = max(1, int(img.height * sc))
        cw = max(self.canvas.winfo_width(), 50)
        ch = max(self.canvas.winfo_height(), 50)
        ch_eff = ch - self._foot_h                       # 扣除页脚后的可视高度
        self._max_scroll = max(0, dh - ch_eff)           # 纵向可滚动量
        self.scroll = max(0, min(self.scroll, self._max_scroll))
        small = img.resize((dw, dh), Image.LANCZOS)
        self.photo = ImageTk.PhotoImage(small)
        self.canvas.delete("all")
        pad = 10                                    # 纸卡内衬边距
        cx = cw / 2
        top_y = (ch_eff - dh) / 2 if dh <= ch_eff else -self.scroll
        # 阴影（两层，营造纸卡浮于桌面）
        sh1 = _rr(self.canvas, cx - dw / 2 - pad + 3, top_y - pad + 4,
                  cx + dw / 2 + pad + 3, top_y + dh + pad + 4, 8,
                  fill=PAL["paper_shadow"], outline="")
        sh2 = _rr(self.canvas, cx - dw / 2 - pad + 5, top_y - pad + 6,
                  cx + dw / 2 + pad + 5, top_y + dh + pad + 6, 8,
                  fill=PAL["paper_shadow"], outline="")
        # 纸卡
        paper = _rr(self.canvas, cx - dw / 2 - pad, top_y - pad,
                    cx + dw / 2 + pad, top_y + dh + pad, 8,
                    fill=PAL["page_bg"], outline=PAL["paper_shadow"])
        # 页面图像
        img_id = self.canvas.create_image(cx, top_y, anchor="n", image=self.photo)
        self._card_ids = [sh1, sh2, paper, img_id]
        self._img_id = img_id
        self._disp = (dw, dh)
        self._draw_footer(cw, ch)

    def _move_page(self):
        """页内滚动：仅平移页卡（阴影/纸/图像），不重新缩放，保证流畅。"""
        if not self._card_ids or self._disp is None:
            return
        dw, dh = self._disp
        cw = max(self.canvas.winfo_width(), 50)
        ch = max(self.canvas.winfo_height(), 50)
        ch_eff = ch - self._foot_h
        self._max_scroll = max(0, dh - ch_eff)
        self.scroll = max(0, min(self.scroll, self._max_scroll))
        top_y = (ch_eff - dh) / 2 if dh <= ch_eff else -self.scroll
        pad = 10
        cx = cw / 2
        # 阴影1 / 阴影2 / 纸卡 / 图像
        self.canvas.moveto(self._card_ids[0], cx - dw / 2 - pad + 3, top_y - pad + 4)
        self.canvas.moveto(self._card_ids[1], cx - dw / 2 - pad + 5, top_y - pad + 6)
        self.canvas.moveto(self._card_ids[2], cx - dw / 2 - pad, top_y - pad)
        self.canvas.moveto(self._card_ids[3], cx - dw / 2, top_y)

    def _draw_footer(self, cw, ch):
        """签名元素：底部黄铜“页垛”阅读进度条。"""
        fh = 44
        self.canvas.create_rectangle(0, ch - fh, cw, ch, fill=PAL["panel"], outline="")
        self.canvas.create_line(0, ch - fh, cw, ch - fh, fill=PAL["brass"], width=1)
        self.canvas.create_text(cw / 2, ch - fh + 12, anchor="n",
                                text="第 %d 页 · 共 %d 页" % (self.cur, self.N),
                                fill=PAL["muted"], font=_font("serif", 11))
        w = min(240, max(120, cw - 160))
        x1 = (cw - w) / 2
        y1 = ch - fh + 28
        self.canvas.create_rectangle(x1, y1, x1 + w, y1 + 5, fill=PAL["panel_edge"], outline="")
        fw = max(5, int(w * self.cur / self.N))
        self.canvas.create_rectangle(x1, y1, x1 + fw, y1 + 5, fill=PAL["brass"], outline="")

    def _show_placeholder(self):
        """页面渲染未就绪时的轻量占位：桌面上的暖纸卡。"""
        cw = max(self.canvas.winfo_width(), 50)
        ch = max(self.canvas.winfo_height(), 50)
        self._disp = None
        self._img_id = None
        self._card_ids = []
        self.canvas.delete("all")
        pw, ph = 420, 150
        cx, cy = cw / 2, ch / 2
        for off in ((3, 4), (5, 6)):
            self.canvas.create_rectangle(cx - pw / 2 + off[0], cy - ph / 2 + off[1],
                                         cx + pw / 2 + off[0], cy + ph / 2 + off[1],
                                         fill=PAL["paper_shadow"], outline="")
        self.canvas.create_rectangle(cx - pw / 2, cy - ph / 2,
                                     cx + pw / 2, cy + ph / 2,
                                     fill=PAL["page_bg"], outline=PAL["paper_shadow"])
        self.canvas.create_text(cx, cy - 12, text="正在渲染第 %d 页…" % self.cur,
                                fill=PAL["ink"], font=_font("serif", 15))
        self.canvas.create_line(cx - 70, cy + 34, cx + 70, cy + 34,
                                fill=PAL["brass"], width=2)
        self._draw_footer(cw, ch)

    def _on_page_ready(self, n, img):
        if img is None or n != self.cur:
            return
        try:
            if not self.root.winfo_exists():
                return
        except tk.TclError:
            return
        self._paint(img)
        self._prefetch(n)

    def _prefetch(self, n):
        """后台预取相邻页，使连续翻页几乎即时。"""
        for m in (n - 1, n + 1):
            if 1 <= m <= self.N and self.rd.get(m) is None:
                self.rd.get_async(m, lambda _n, _img: None)

    def _poll_render(self):
        """定期取回后台渲染结果（主线程）。"""
        try:
            if not self.root.winfo_exists():
                return
        except tk.TclError:
            return
        try:
            for cb, n, img in self.rd.drain():
                try:
                    cb(n, img)
                except Exception:
                    pass
        except Exception:
            pass
        self.root.after(60, self._poll_render)

    def _on_configure(self, e):
        # 窗口尺寸变化频繁触发，防抖后再重绘
        if self._resize_job:
            self.root.after_cancel(self._resize_job)
        self._resize_job = self.root.after(80, self._draw)

    def first(self): self._set_page(1)
    def last(self): self._set_page(self.N)
    def prev(self): self._set_page(self.cur - 1)
    def next(self): self._set_page(self.cur + 1)
    def zoom_in(self):
        self.zoom = min(3.0, self.zoom * 1.25); self._draw()
    def zoom_out(self):
        self.zoom = max(0.3, self.zoom / 1.25); self._draw()

    def _on_mousewheel(self, ev):
        """Windows/macOS 的滚轮事件：delta>0 上滑 -> step -1。"""
        self._wheel_page(-1 if ev.delta > 0 else 1)

    def _wheel_page(self, step):
        """连续滚动：先在当前页内纵向移动；到达页底才翻下一页，页首继续上滚翻上一页。"""
        # 翻页后短暂保护窗口内忽略滚轮，避免高精度滚轮/触摸板惯性的“
        # 尾随事件”把刚定位到页首（或页底）的页面立刻推走。
        if time.monotonic() < self._flip_until:
            return
        self._real_wheel(step)

    def _real_wheel(self, step):
        if self._disp is None:                          # 页面尚未渲染就绪
            return
        _, dh = self._disp                               # 页面显示高度
        ch = max(self.canvas.winfo_height(), 50)        # 视口高度
        max_scroll = max(0, dh - (ch - self._foot_h))   # 扣除页脚后的可滚动量
        step_px = max(30, dh // 12)                     # 每格滚动的像素量
        if step < 0:                                    # 上滑
            if self.scroll > 0:                         # 页内还有上方内容：上移
                self.scroll = max(0, self.scroll - step_px)
                self._move_page()
            elif self.cur > 1:                          # 已在页首：翻上一页并定位到其底部
                self._set_page(self.cur - 1, at_bottom=True)
                self._flip_until = time.monotonic() + 0.15
        else:                                           # 下滑
            if self.scroll < max_scroll:                # 未越过页底：下移
                self.scroll = min(max_scroll, self.scroll + step_px)
                self._move_page()
            elif self.cur < self.N:                     # 已越过页底：翻下一页（定位页首）
                self._set_page(self.cur + 1)
                self._flip_until = time.monotonic() + 0.15
            else:
                self._move_page()

    def show_info(self):
        txt = analyze_text(self.cap)
        top = tk.Toplevel(self.root)
        top.title("格式分析")
        top.configure(bg=PAL["desk"])
        txtw = tk.Text(top, width=64, height=28, font=_font("mono", 10),
                       bg=PAL["desk"], fg=PAL["paper"], insertbackground=PAL["paper"],
                       selectbackground=PAL["brass"], selectforeground=PAL["ink"],
                       relief="flat", padx=14, pady=10)
        txtw.pack(fill=tk.BOTH, expand=True)
        txtw.insert(tk.END, txt)
        txtw.config(state=tk.DISABLED)

    def run(self):
        self.root.mainloop()
        shutil.rmtree(self.tmp, ignore_errors=True)

# ---------------- 格式分析文本 ----------------
def analyze_text(cap):
    L = []
    L.append("=" * 56)
    L.append(" CAJ 文件格式分析：%s" % os.path.basename(cap.path))
    L.append("=" * 56)
    L.append("文件头(前4字节): %s" % " ".join("%02X" % b for b in cap.magic))
    L.append("标识文本: %r" % cap.magic.rstrip(b"\x00").decode("latin-1"))
    L.append("格式类别: %s" % cap.fmt)
    if cap.page_off:
        L.append("页面数:   %d" % cap.page_num)
    if cap.toc_off:
        toc = cap.toc()
        L.append("大纲项数: %d (起始偏移 0x%X)" % (len(toc), cap.toc_off))
        L.append("— 大纲预览 —")
        for lv, title, page in toc[:20]:
            L.append("  %s%s  · 第 %s 页" % ("   " * (lv - 1), title, page))
        if len(toc) > 20:
            L.append("  ... 共 %d 项" % len(toc))
    L.append("-" * 56)
    if cap.fmt == "CAJ":
        L.append("结构说明：CAJ 是知网私有容器，文件头 'CAJ' 后按特定偏移")
        L.append("存放信息。0x10=页数，0x110=大纲表；0x14 处为指向内嵌")
        L.append("PDF 对象流的指针。读取其中的 PDF 对象并重建目录/xref")
        L.append("即可得到可渲染的标准 PDF——本程序正是采用此方案。")
    elif cap.fmt == "%PDF":
        L.append("该文件本质是标准 PDF，直接改后缀为 .pdf 即可阅读。")
    elif cap.fmt == "HN":
        L.append("HN 为另一类专有格式，无内嵌 PDF 流。正文页面多为")
        L.append("CAJViewer 专有 JBig 编码图像（无法直接解码），少量为")
        L.append("标准 JPEG。本程序解析页信息表，将 JPEG 页直接嵌入")
        L.append("PDF，JBig 页以占位白纸代替（可正常翻页/大纲跳转）。")
    else:
        L.append("此格式变体暂不完整支持。")
    L.append("=" * 56)
    return "\n".join(L)

def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    path = sys.argv[1]
    if not os.path.exists(path):
        sys.exit("文件不存在: %s" % path)
    cap = CAJParser(path)
    if cap.fmt is None:
        sys.exit("无法识别的文件类型（未知魔数）。")
    print(analyze_text(cap))
    if "--info" in sys.argv:
        return
    try:
        v = Viewer(path)
    except SystemExit:
        raise
    except Exception as e:
        import traceback
        traceback.print_exc()
        sys.exit("打开查看器失败: %s" % e)
    v.run()

if __name__ == "__main__":
    main()