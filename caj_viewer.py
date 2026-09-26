#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
caj_viewer.py —— CAJ 文档阅读器

功能
----
1. 分析 .caj 文件的格式（魔数、格式类别、版本、页数、大纲等）。
2. 提取并重建 CAJ 容器内嵌的 PDF 流（纯 Python 实现，无需 mutool）；
   "KDH 2.00" 新版加密格式按频率分析自动恢复 XOR 密钥并解密。
3. 用 poppler 的 pdftoppm/pdftohtml 渲染页面并用 tkinter 以 GUI 形式阅读：
   文本页提取真实文本版面（可任意缩放仍清晰），扫描图片页渲染位图。

用法
----
    python3 caj_viewer.py 文献.caj            # 打印格式分析并打开阅读窗口
    python3 caj_viewer.py 文献.caj --info     # 仅打印格式分析，不打开窗口
    python3 caj_viewer.py                     # 不带参数时弹出文件选择对话框

依赖
----
    python3 + tkinter + Pillow + poppler-utils(pdftoppm/pdftohtml/pdffonts)
"""
import os
import re
import io
import sys
import time
import glob
import json
import ctypes
import hashlib
import uuid
import queue
import shutil
import struct
import tempfile
import threading
import subprocess
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from collections import OrderedDict, Counter

try:
    from PIL import Image, ImageDraw, ImageTk
    import tkinter as tk
    from tkinter import ttk, filedialog, messagebox
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
#   KDH   -> "KDH 2.00" 新版知网下载格式（XOR 加密 PDF，程序自动解密）
#   TEB   -> 其他变体
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
            # 魔数后常跟版本/空格/NUL（如 "KDH 2.00 ..."、b"CAJ\x00"），
            # 按 NUL+空格剥离后再精确匹配已知格式名
            fmt = head.rstrip(b"\x00 ").decode("latin-1")
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

# ---------------- KDH 2.00：加密容器解密 ----------------
# 新版知网下载的文献为 "KDH 2.00 Copyright(C) 2000 CAJCD" 头：
#   0x3C  int32 -> 尾部明文 XML 元数据（<FileProperty>...）的起点
#   0xFC  起到该点为止为 XOR 加密体（解密后即为内嵌 PDF）
# 密钥为 N 字节循环 XOR（实测 6 字节），文件内无明文存放，但 PDF 明文的
# 字节分布会泄露它：按相位取密文最高频字节 = 密钥字节 ^ 明文最高频字节
# （PDF 中最高频字节几乎必为 '0'），再用 "endobj" 相邻差分模式投票确定
# 全局常数即可恢复整把密钥——无需任何固定密钥，任意 KDH 文件通用。
def _xor_rep(data, key):
    """data 与循环 key 逐字节异或（借大整数一次异或，C 速度）。"""
    n = (len(data) + len(key) - 1) // len(key) * len(key)
    padded = data + b"\x00" * (n - len(data))
    return (int.from_bytes(padded, "big") ^
            int.from_bytes(key * (n // len(key)), "big")).to_bytes(n, "big")[:len(data)]


def _kdh_pdf(body, key):
    """用恢复的密钥解出 PDF；无效（无 %PDF 头/无对象）返回 None。"""
    plain = _xor_rep(body, key)
    off = plain.find(b"%PDF-")
    if off < 0:
        return None
    pdf = plain[off:]
    e = pdf.rfind(b"%%EOF")              # 截去 %%EOF 之后的尾部杂讯
    if e >= 0:
        pdf = pdf[:e + 5]
    if not (pdf.count(b"endobj") or pdf.count(b" obj")):
        return None
    return pdf


def _kdh_try(body, L):
    """按密钥长度 L 恢复密钥并解密；失败返回 None。"""
    tops = [Counter(body[i::L]).most_common(1)[0][0] for i in range(L)]
    cand = _xor_rep(body, bytes(tops))
    # 若密钥正确，cand = 明文 ^ 常数，其相邻差分序列含 "endobj" 的
    # 相邻 XOR 模式（与常数无关）；每个命中即投一票确定该常数
    crib = b"endobj"
    dpat = bytes(a ^ b for a, b in zip(crib, crib[1:]))
    z = _xor_rep(cand[:-1], cand[1:])
    votes = Counter()
    i = z.find(dpat)
    while i >= 0:
        votes[cand[i] ^ crib[0]] += 1
        i = z.find(dpat, i + 1)
    if votes:
        m0, n = votes.most_common(1)[0]
        if n >= 3:                       # 票数集中：密钥可信
            return _kdh_pdf(body, bytes(t ^ m0 for t in tops))
    for m0 in (0x30, 0x00, 0x0D, 0x20):  # 投票不足：按 PDF 常见最高频字节验证
        pdf = _kdh_pdf(body, bytes(t ^ m0 for t in tops))
        if pdf is not None:
            return pdf
    return None


def _kdh_decrypt(d):
    """解密 KDH 2.00 容器 -> PDF 字节；失败抛 ValueError。"""
    if len(d) < 0x100:
        raise ValueError("KDH 文件过小")
    end = struct.unpack("i", d[0x3C:0x40])[0]
    if not (0xFC < end <= len(d)):       # 头部异常时兜底：解到文件尾
        end = len(d)
    body = d[0xFC:end]
    if len(body) < 0x200:
        raise ValueError("KDH 加密体过小")
    for L in (6, 4, 8, 12, 16, 32, 64):  # 实测为 6，其余为兼容变体
        pdf = _kdh_try(body, L)
        if pdf is not None:
            return pdf
    raise ValueError("KDH 解密失败：无法恢复 XOR 密钥")


# ---------------- 页面版面模型（文本页） ----------------
@dataclass
class TextRun:
    """一段同字体的连续文本，坐标为 PDF 磅、左上原点。"""
    x: float; y: float; w: float; h: float
    text: str
    font: int                       # PageLayout.fonts 的 id
    bold: bool = False
    italic: bool = False

@dataclass
class ImgRun:
    x: float; y: float; w: float; h: float
    img: object                     # PIL Image（后台线程已加载）

@dataclass
class PageLayout:
    """一页的真实版面：文字 + 插图（坐标单位 PDF 磅，左上原点）。
    渲染成画布文本项而非位图，缩放任意倍仍清晰锐利。"""
    w: float
    h: float
    fonts: dict = field(default_factory=dict)   # id -> (size_pt, family, color)
    texts: list = field(default_factory=list)   # [TextRun]
    imgs: list = field(default_factory=list)    # [ImgRun]

# ---------------- 渲染器 ----------------
class Renderer:
    """页面渲染器：后台线程执行，主线程绝不阻塞。
    文本页用 pdftohtml 提取版面（PageLayout），无文本的页（纯扫描图）
    回退 pdftoppm 位图。结果放入队列，由 GUI 定时轮询取回。"""
    def __init__(self, pdf_bytes, tmpdir):
        self.pdf = os.path.join(tmpdir, "doc.pdf")
        self.tmpdir = tmpdir
        with open(self.pdf, "wb") as f:
            f.write(pdf_bytes)
        self.cache = OrderedDict()
        self._lock = threading.Lock()
        self._inflight = {}                # 页号 -> [回调列表]，同一页多次请求可复用
        self._done = queue.Queue()         # (cb, page, obj) 结果队列
        self.max = 10                      # LRU 缓存页数（含预取相邻页）
        self.dpi = 120
        # 整文档无字体（如 HN/C8 图像 PDF、纯扫描件）则完全跳过文本提取
        self.text_mode = self._probe_fonts()

    def _probe_fonts(self):
        """pdffonts 列出至少一种字体 -> 可能存在文本层。"""
        try:
            out = subprocess.run(["pdffonts", self.pdf], capture_output=True,
                                 text=True, timeout=20).stdout
            rows = [ln for ln in out.splitlines()[2:] if ln.strip()]
            return bool(rows)
        except Exception:
            return True                    # 探测失败则逐页尝试，失败自动回退位图

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
        """优先文本版面；失败或无文本（纯扫描页）回退位图。"""
        if self.text_mode:
            try:
                lay = self._render_layout(n)
            except Exception:
                lay = None
            if lay is not None:
                return lay
        return self._render_bitmap(n)

    def _render_bitmap(self, n):
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

    def _render_layout(self, n):
        """pdftohtml -xml 提取第 n 页版面（坐标/字号均为 PDF 磅）。
        插图以 <pdf基名>-<页>_<序>.png 落在 PDF 同目录，一并加载。
        返回 None 表示该页应按位图渲染（无文本 / 整页扫描图 / 提取失败）。"""
        r = subprocess.run(
            ["pdftohtml", "-xml", "-q", "-noroundcoord", "-zoom", "1",
             "-f", str(n), "-l", str(n), "-stdout", "-fmt", "png", self.pdf],
            capture_output=True, timeout=60,
            cwd=os.path.dirname(self.pdf))
        if r.returncode != 0 or not r.stdout:
            return None
        return self._parse_layout(r.stdout)

    def _parse_layout(self, xml_bytes):
        root = ET.fromstring(xml_bytes)      # 传 bytes：输出带 encoding 声明
        pg = root.find("page")
        if pg is None:
            return None
        W, H = float(pg.get("width", 0)), float(pg.get("height", 0))
        if W <= 0 or H <= 0:
            return None
        fonts = {}
        for fs in pg.findall("fontspec"):
            fonts[int(fs.get("id"))] = (float(fs.get("size", 12)),
                                        fs.get("family", ""),
                                        fs.get("color", "#000000"))
        texts, imgs = [], []
        page_area = W * H
        for el in pg:
            if el.tag == "text":
                t = "".join(el.itertext())
                if not t.strip():
                    continue                 # 纯空白占位段（下划线空格等）
                style = {s.tag for s in el}
                try:
                    fid = int(el.get("font", -1))
                except ValueError:
                    fid = -1
                texts.append(TextRun(
                    float(el.get("left", 0)), float(el.get("top", 0)),
                    float(el.get("width", 0)), float(el.get("height", 0)),
                    t, fid, "b" in style, "i" in style))
            elif el.tag == "image":
                iw = float(el.get("width", 0) or 0)
                ih = float(el.get("height", 0) or 0)
                if iw * ih >= 0.9 * page_area:
                    return None              # 整页扫描图（含可见 OCR 层）：按位图渲染
                src = el.get("src") or ""
                path = os.path.join(self.tmpdir, src)
                if not src or not os.path.isfile(path):
                    continue
                try:
                    im = Image.open(path); im.load()
                    if im.mode not in ("RGB", "RGBA"):
                        im = im.convert("RGB")
                except Exception:
                    continue
                imgs.append(ImgRun(float(el.get("left", 0) or 0),
                                   float(el.get("top", 0) or 0), iw, ih, im))
        if not texts:
            return None                      # 纯图页/空页 -> 位图渲染
        return PageLayout(W, H, fonts, texts, imgs)

    def get_async(self, n, cb):
        """异步渲染第 n 页，完成后回调 cb(n, obj)（obj 可能为 None）。
        obj 为 PageLayout（文本页）或 PIL Image（位图页）。
        已在缓存则直接回调；渲染中的页可追加回调，随渲染完成一并触发。"""
        obj = self.get(n)
        if obj is not None:
            cb(n, obj)
            return
        with self._lock:
            cbs = self._inflight.setdefault(n, [])
            cbs.append(cb)
            first = len(cbs) == 1
        if not first:
            return
        def worker():
            try:
                res = self._render(n)
            except Exception:
                res = None
            with self._lock:
                cbs = self._inflight.pop(n, [])
            if res is not None:
                self._store(n, res)
            for c in cbs:
                self._done.put((c, n, res))
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

# ---------------- 内容标记 ----------------
MARK_COLORS = {"黄": "#F2C94C", "绿": "#7BC96F", "蓝": "#6FB7E8", "红": "#E86F6F"}
SEL_FILL = "#BBD6F0"                   # 文本选区底色（淡蓝，画在文字下层）

@dataclass
class Annotation:
    """一个矩形高亮标记：rect 为归一化页面坐标 (0..1)，缩放/翻页不失效。"""
    page: int                       # 1-based 页号
    rect: list                      # [x0, y0, x1, y1]，x0<x1, y0<y1
    color: str = "#F2C94C"
    id: str = ""
    created: float = 0.0

    def __post_init__(self):
        if not self.id:
            self.id = uuid.uuid4().hex[:12]
        if not self.created:
            self.created = time.time()

def _hex_rgb(c):
    """'#RRGGBB' -> (r, g, b)，非法颜色回退默认黄。"""
    try:
        return tuple(int(c[i:i + 2], 16) for i in (1, 3, 5))
    except Exception:
        return (0xF2, 0xC9, 0x4C)

def overlay_marks(img, marks):
    """把高亮以半透明形式烙进页面位图副本（不动 Renderer 缓存原图）。
    Canvas 无真透明，PIL 合成使标记随图像项平移，滚动/翻页零额外处理。
    注意：须在 RGB 图上以 RGBA 模式绘制才会做 alpha 混合；若先转成
    RGBA 图再画，PIL 会整像素替换（alpha 直接写入），正文会被遮没。"""
    dr = ImageDraw.Draw(img, "RGBA")
    w, h = img.size
    for a in marks:
        x0, y0, x1, y1 = a.rect
        rgb = _hex_rgb(a.color)
        dr.rectangle([x0 * w, y0 * h, x1 * w, y1 * h],
                     fill=rgb + (70,), outline=rgb + (230,), width=1)
    return img

# ---------------- sidecar 缓存（模块级） ----------------
# 把"提取/重建 PDF"的结果落到 <源文件>.cajviewer.pdf，同一文件二次打开近瞬时。
def _cache_path_for(path):
    """sidecar 路径：源文件同目录下 <name>.cajviewer.pdf。
    源文件不可写时（只读介质）会落到 ~/.cache/caj-viewer/。"""
    try:
        p = path + ".cajviewer.pdf"
        # 检查同目录是否可写
        probe = os.path.join(os.path.dirname(p), ".cajviewer.write_probe")
        with open(probe, "wb") as f:
            f.write(b"x")
        os.remove(probe)
        return p
    except (OSError, IOError):
        h = hashlib.sha1(os.path.abspath(path).encode()).hexdigest()[:16]
        d = os.path.expanduser("~/.cache/caj-viewer")
        try:
            os.makedirs(d, exist_ok=True)
        except OSError:
            return None
        return os.path.join(d, "%s.cajviewer.pdf" % h)


def _try_load_cache(path, fmt):
    """缓存命中条件：源文件 mtime+size 与缓存头一致；读头 32 字节校验。"""
    cp = _cache_path_for(path)
    if cp is None or not os.path.exists(cp):
        return None
    try:
        st_src = os.stat(path)
        with open(cp, "rb") as f:
            data = f.read()
        # 缓存头：源 mtime(ns)|源 size|fmt（旧版缓存头实际长 8+8+len(fmt)，
        # 与本函数旧的 seek(64) 不一致曾导致 PDF 头部被截去 45 字节）
        src_mtime = struct.pack("q", int(st_src.st_mtime * 1e9))
        src_size = struct.pack("q", st_src.st_size)
        fb = fmt.encode() if fmt else b"?"
        if not data.startswith(src_mtime + src_size + fb):
            return None
        # 按内容定位 PDF 起点，兼容头部未补齐 64 字节的新旧缓存
        # （注意 fmt 本身可能是 %PDF，须带连字符搜索以免错位）
        off = data.find(b"%PDF-")
        return data[off:] if 0 <= off else None
    except (OSError, IOError, struct.error):
        return None


def _save_cache(path, fmt, pdf_bytes):
    cp = _cache_path_for(path)
    if cp is None:
        return
    try:
        st_src = os.stat(path)
        head = (struct.pack("q", int(st_src.st_mtime * 1e9)) +
                struct.pack("q", st_src.st_size) +
                (fmt or "?").encode())
        head = head.ljust(64, b"\x00")     # 头补齐 64B，读取端按 %PDF 定位
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

# ---------------- 文档装载 ----------------
def _prepare_doc(path):
    """解析并提取文档 -> (CAJParser, pdf_bytes)。
    优先读 sidecar 缓存；未命中则按格式提取（一次 read 复用给所有需要
    整文件的分支），并异步写缓存（避免阻塞主线程）。
    供 Viewer 初始化与"打开文件"复用；失败向上抛异常。"""
    cap = CAJParser(path)
    if cap.fmt is None:
        raise ValueError("无法识别的文件类型（未知魔数）。")
    pdf = _try_load_cache(path, cap.fmt)
    if pdf is not None:
        return cap, pdf
    with open(path, "rb") as f:
        raw = f.read()
    if cap.fmt == "HN":
        pdf, _ = _build_hn_pdf(raw)
    elif cap.fmt == "%PDF":
        pdf = raw                           # 本质是标准 PDF，直接使用
    elif cap.fmt == "KDH":
        pdf = _kdh_decrypt(raw)             # 新版知网加密格式，自动解密
    else:
        pdf, _ = extract_pdf(cap, blob=raw)
    threading.Thread(target=_save_cache, args=(path, cap.fmt, pdf),
                     daemon=True).start()
    return cap, pdf

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

# ---- PDF 字体名 -> tkinter 字体（文本页渲染用）----
_PDF_FONT_CATS = {
    # 类别 -> 候选字体族（按优先级，运行时与 tkfont.families() 求交集）
    "cjk_serif": ["Noto Serif CJK SC", "Source Han Serif SC", "Songti SC",
                  "STSong", "SimSun", "NSimSun", "AR PL UMing CN",
                  "Droid Sans Fallback", "DejaVu Serif"],
    "cjk_sans":  ["Noto Sans CJK SC", "Source Han Sans SC", "Microsoft YaHei",
                  "SimHei", "WenQuanYi Micro Hei", "Droid Sans Fallback",
                  "DejaVu Sans"],
    "cjk_kai":   ["KaiTi", "楷体_GB2312", "STKaiti", "AR PL UKai CN",
                  "AR PL UMing CN", "Droid Sans Fallback"],
    "lat_serif": ["Times New Roman", "Liberation Serif", "DejaVu Serif",
                  "Nimbus Roman", "FreeSerif"],
    "lat_sans":  ["Arial", "Liberation Sans", "DejaVu Sans", "Nimbus Sans",
                  "FreeSans"],
    "mono":      ["Courier New", "DejaVu Sans Mono", "Liberation Mono",
                  "Noto Mono", "FreeMono"],
}
_PDF_CAT_KEYS = (                       # 依序匹配（CJK 判定必须先于拉丁）
    ("cjk_sans",  ("hei", "yahei", "gothic", "heiti", "wentai", "zhonghei")),
    ("cjk_kai",   ("kai", "xingkai")),
    ("cjk_serif", ("song", "sun", "fang", "ming", "mingli", "zhengsong",
                   "xiaobiaosong")),
    ("lat_serif", ("times", "georgia", "roman", "garamond", "book")),
    ("lat_sans",  ("arial", "helvetica", "verdana", "calibri", "tahoma",
                   "sans", "segoe")),
    ("mono",      ("courier", "mono", "consol")),
)
_TK_FAMS = None
_PDF_FAM_CACHE = {}
_LAYOUT_FONT_CACHE = {}

def _pdf_font_family(raw):
    """PDF 字体名（含子集前缀如 'BGFNPF+SimSun'）-> 本机可用的 tk 字体族。"""
    if raw in _PDF_FAM_CACHE:
        return _PDF_FAM_CACHE[raw]
    global _TK_FAMS
    if _TK_FAMS is None:
        try:
            _TK_FAMS = set(tkfont.families())
        except Exception:
            _TK_FAMS = set()
    name = (raw.split("+")[-1] or raw).strip()
    fam = name if name in _TK_FAMS else ""
    if not fam:                           # 名称不可用：按关键词归类后选候选
        low = name.lower().replace(" ", "").replace("-", "")
        cat = "cjk_serif"                 # 默认按中文衬线（CAJ 文献以中文为主）
        for c, keys in _PDF_CAT_KEYS:
            if any(k in low for k in keys):
                cat = c
                break
        for cand in _PDF_FONT_CATS[cat]:
            if cand in _TK_FAMS:
                fam = cand
                break
    if not fam:
        fam = "TkDefaultFont"
    _PDF_FAM_CACHE[raw] = fam
    return fam

def _layout_font(raw_family, size_pt, scale, bold=False, italic=False):
    """文本运行 -> tk 字体元组。size 用负数（像素），与屏幕 DPI 无关。"""
    px = max(1, round(size_pt * scale))
    fam = _pdf_font_family(raw_family)
    low = (raw_family or "").lower()
    if "bold" in low or "black" in low:
        bold = True
    if "italic" in low or "oblique" in low:
        italic = True
    key = (fam, px, bold, italic)
    if key not in _LAYOUT_FONT_CACHE:
        t = (fam, -px)
        if bold and italic:
            t = (fam, -px, "bold", "italic")
        elif bold:
            t = (fam, -px, "bold")
        elif italic:
            t = (fam, -px, "italic")
        _LAYOUT_FONT_CACHE[key] = t
    return _LAYOUT_FONT_CACHE[key]

_CHAROFF_CACHE = {}

def _char_offsets(text, ft_tup):
    """文本在给定 tk 字体下的逐字符累计像素宽（offs[0]=0，长 len(text)+1）。
    按（字体, 文本）缓存，选区重绘时免重复测量。"""
    key = (ft_tup, text)
    offs = _CHAROFF_CACHE.get(key)
    if offs is None:
        f = tkfont.Font(font=ft_tup)
        offs, x = [0], 0
        for ch in text:
            x += f.measure(ch)
            offs.append(x)
        if len(_CHAROFF_CACHE) > 4096:
            _CHAROFF_CACHE.clear()
        _CHAROFF_CACHE[key] = offs
    return offs

_CJK_SP_RE = re.compile(
    r"(?<=[\u4e00-\u9fff\u3000-\u303f\uff00-\uffef])\s+"
    r"(?=[\u4e00-\u9fff\u3000-\u303f\uff00-\uffef])")

def _cjk_tight(s):
    """去掉夹在两个 CJK 字符（含全角标点）之间的空格。pdftohtml 逐字形
    输出常用空格分隔相邻汉字，那是版面伪迹——中文词语间本无空格。"""
    return _CJK_SP_RE.sub("", s)

def _rr(c, x1, y1, x2, y2, r, **kw):
    """圆角矩形（smooth 多边形近似）。"""
    pts = [x1 + r, y1, x2 - r, y1, x2, y1, x2, y1 + r,
           x2, y2 - r, x2, y2, x2 - r, y2, x1 + r, y2,
           x1, y2, x1, y2 - r, x1, y1 + r, x1, y1]
    return c.create_polygon(pts, smooth=True, **kw)

def _try_cursor(widget, name):
    """尝试设置光标名，失败（平台不支持）返回 False。X11 的 I 型光标
    叫 xterm，Windows/macOS 叫 ibeam。"""
    try:
        widget.config(cursor=name)
        return True
    except tk.TclError:
        return False

# 文件选择对话框的类型过滤（一项可含空格分隔的多个通配符）
OPEN_FILETYPES = [
    ("CAJ/知网文档", "*.caj *.kdh *.hn *.nh"),
    ("PDF 文档", "*.pdf"),
    ("所有文件", "*"),
]

class Viewer:
    def __init__(self, path):
        self.path = path
        self.tmp = tempfile.mkdtemp(prefix="cajview_")
        t0 = time.monotonic()
        # 解析 + 提取（sidecar 缓存优先，见 _prepare_doc）；失败直接终止
        try:
            self.cap, self.pdf = _prepare_doc(path)
        except Exception as e:
            raise SystemExit("打开文档失败: %s" % e)
        self._t_extract = time.monotonic() - t0
        self.rd = Renderer(self.pdf, self.tmp)
        # KDH 等解密格式头部无页数，回退从解出的 PDF 统计
        self.N = self.cap.page_num or _pdf_page_count(self.rd.pdf)
        self.cur = 1
        self.zoom = 1.0
        self.scroll = 0                  # 当前页内纵向滚动偏移（显示像素）
        self._max_scroll = 0             # 当前页可滚动上限（页面高度 - 视口高度）
        self._flip_until = 0.0           # 翻页后的“落位保护”截止时间
        self._disp = None                # 当前已绘制的显示尺寸 (dw, dh)
        self._img_id = None              # 画布上页面图像项的 id
        self._top_y = 0.0                # 当前页顶在画布上的 y（滚动平移基准）
        self._photos = []                # 文本页插图 PhotoImage 引用（防 GC）
        self._resize_job = None          # 窗口尺寸变化的防抖句柄
        self._foot_h = 44                # 底部页垛页脚高度（滚动可视区须扣除）
        self.annos = []                  # 全文档标记（Annotation 列表）
        self._anno_sel = None            # 拖选中：[(sx, sy), preview_id]，None=未拖选
        # （注意：不能叫 _anno_drag，会与同名 B1-Motion 处理方法互相遮蔽）
        self.text_sel = None             # 文本选区：(page, x0, y0, x1, y1) 归一化坐标
        self._selpress = None            # 拖选文字起点（画布坐标），None=未拖选
        self._toast_tok = 0              # 轻提示去重令牌
        self._load_annotations()

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
        # 内容标记：B1 拖选创建高亮（仅标记模式），点击已有标记弹换色/删除菜单。
        # 非标记模式下 B1 拖选 = 文字选区（松开后 Ctrl+C / 右键复制）。
        # 滚轮翻页绑定不受影响，两者无冲突。
        self.canvas.bind("<Button-1>", self._anno_press)
        self.canvas.bind("<B1-Motion>", self._anno_drag)
        self.canvas.bind("<ButtonRelease-1>", self._anno_release)
        self.canvas.bind("<Button-3>", self._sel_menu)
        # 打开文件：Ctrl+O（主窗口级绑定，随 bindtags 冒泡触发）
        self.root.bind("<Control-o>", self.open_file)
        self.root.bind("<Control-O>", self.open_file)
        # 复制选中文本：主窗口级绑定（子部件获得焦点时经由 bindtags 冒泡触发）
        self.root.bind("<Control-c>", self._copy_selection)
        self.root.bind("<Control-C>", self._copy_selection)
        self.root.bind("<Escape>", lambda e: self._clear_selection())
        self._set_page(1)
        # 4) 预热：在 mainloop 启动前就触发首页渲染，窗口一亮就有图
        self.rd.get_async(1, lambda _n, _obj: None)

    # ----- 打开文件：在当前窗口切换文档 -----
    def open_file(self, ev=None):
        """工具栏“打开”/Ctrl+O：弹出文件选择对话框（tk 原生，深色主题外）。"""
        path = filedialog.askopenfilename(
            parent=self.root, title="打开文档", filetypes=OPEN_FILETYPES)
        if path:
            self.open_path(path)

    def open_path(self, path):
        """载入新文档。解析/渲染器全部就绪后才切换状态：失败弹窗提示并
        保留原文档；成功则重置阅读状态并刷新标题/大纲/页码，窗口不重建。"""
        tmp = None
        try:
            cap, pdf = _prepare_doc(path)
            tmp = tempfile.mkdtemp(prefix="cajview_")
            rd = Renderer(pdf, tmp)
        except Exception as e:
            if tmp:
                shutil.rmtree(tmp, ignore_errors=True)
            messagebox.showerror("打开失败", "无法打开 %s：\n%s"
                                 % (os.path.basename(path), e),
                                 parent=self.root)
            return
        old_tmp = self.tmp
        self.path, self.cap, self.pdf = path, cap, pdf
        self.tmp, self.rd = tmp, rd
        self.N = cap.page_num or _pdf_page_count(rd.pdf)   # KDH 无头部页数
        self.cur = 1
        self.zoom = 1.0
        self.scroll = 0
        self._max_scroll = 0
        self._flip_until = 0.0
        self._disp = None
        self._img_id = None
        self._top_y = 0.0
        self._photos = []
        self.text_sel = None
        self._anno_cancel()
        self._load_annotations()          # 新文档的标记（sidecar）
        self.root.title("CAJ 阅读器 - %s" % os.path.basename(path))
        self.title_lbl.config(text=os.path.basename(path)[:36])
        self.toc_box.delete(0, tk.END)    # 清空后由 _set_page 重新装载
        self._set_page(1)
        self.rd.get_async(1, lambda _n, _obj: None)   # 预热首页
        shutil.rmtree(old_tmp, ignore_errors=True)    # 清理旧文档临时目录

    # ----- 标记持久化：源文件旁 <name>.annotations.json -----
    def _anno_path(self):
        """标记文件路径：优先源文件同目录；不可写时回退
        ~/.local/share/caj-viewer/annotations/<sha1前16>.json（模式仿 _cache_path）。"""
        try:
            p = self.path + ".annotations.json"
            probe = os.path.join(os.path.dirname(p), ".cajviewer.write_probe")
            with open(probe, "wb") as f:
                f.write(b"x")
            os.remove(probe)
            return p
        except (OSError, IOError):
            h = hashlib.sha1(os.path.abspath(self.path).encode()).hexdigest()[:16]
            d = os.path.expanduser("~/.local/share/caj-viewer/annotations")
            try:
                os.makedirs(d, exist_ok=True)
            except OSError:
                return None
            return os.path.join(d, "%s.annotations.json" % h)

    def _load_annotations(self):
        """读取标记文件；损坏/版本不符则忽略整文件（自动重建）。"""
        self.annos = []
        cp = self._anno_path()
        if cp is None or not os.path.exists(cp):
            return
        try:
            with open(cp, "r", encoding="utf-8") as f:
                data = json.load(f)
            if data.get("version") != 1:
                return
            for d in data.get("annotations", []):
                r = d.get("rect")
                if (isinstance(r, list) and len(r) == 4 and
                        all(isinstance(v, (int, float)) for v in r)):
                    self.annos.append(Annotation(
                        page=int(d.get("page", 1)), rect=r,
                        color=d.get("color", "#F2C94C"),
                        id=str(d.get("id", "")),
                        created=float(d.get("created", 0.0))))
        except (OSError, IOError, ValueError, TypeError, AttributeError):
            self.annos = []

    def _save_annotations(self):
        """立即原子写盘（.tmp + os.replace）。"""
        cp = self._anno_path()
        if cp is None:
            return
        try:
            st_src = os.stat(self.path)
            data = {
                "version": 1,
                "source": {"size": st_src.st_size,
                           "mtime_ns": int(st_src.st_mtime * 1e9)},
                "annotations": [{"id": a.id, "page": a.page, "rect": a.rect,
                                 "color": a.color, "created": a.created}
                                for a in self.annos],
            }
            tmp = cp + ".tmp"
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(data, f, ensure_ascii=False, indent=1)
            os.replace(tmp, cp)
        except (OSError, IOError):
            try:
                os.remove(cp + ".tmp")
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
        self.title_lbl = ttk.Label(bar, text=os.path.basename(self.path)[:36],
                                    style="Title.TLabel")
        self.title_lbl.pack(side=tk.LEFT, padx=(12, 6), pady=4)
        ttk.Separator(bar, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=4, pady=4)
        ttk.Button(bar, text="打开", command=self.open_file).pack(side=tk.LEFT, padx=2, pady=3)
        for txt, cmd in (("首页", self.first), ("上一页", self.prev),
                         ("下一页", self.next), ("末页", self.last)):
            ttk.Button(bar, text=txt, command=cmd).pack(side=tk.LEFT, padx=2, pady=3)
        ttk.Separator(bar, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=4, pady=4)
        for txt, cmd in (("放大", self.zoom_in), ("缩小", self.zoom_out),
                         ("格式信息", self.show_info)):
            ttk.Button(bar, text=txt, command=cmd).pack(side=tk.LEFT, padx=2, pady=3)
        # 内容标记：模式开关 + 当前颜色选择器（底色实时跟随所选颜色）
        ttk.Separator(bar, orient=tk.VERTICAL).pack(side=tk.LEFT, fill=tk.Y, padx=4, pady=4)
        self.mark_on = tk.BooleanVar(value=False)
        self.mark_color = tk.StringVar(value=MARK_COLORS["黄"])
        ttk.Checkbutton(bar, text="标记模式", var=self.mark_on,
                        command=self._toggle_mark_mode).pack(side=tk.LEFT, padx=2, pady=3)
        self.mark_btn = tk.Menubutton(bar, text="色", width=2, relief="flat",
                                      borderwidth=0, cursor="hand2",
                                      bg=self.mark_color.get(), fg=PAL["ink"],
                                      activebackground=self.mark_color.get(),
                                      font=_font("ui", 10, "bold"))
        mk = tk.Menu(self.mark_btn, tearoff=0, bg=PAL["panel"], fg=PAL["paper"],
                     activebackground=PAL["brass"], activeforeground=PAL["ink"])
        for _name, _hexv in MARK_COLORS.items():
            mk.add_command(label=_name, foreground=_hexv,
                           command=lambda v=_hexv: self._set_mark_color(v))
        self.mark_btn["menu"] = mk
        self.mark_btn.pack(side=tk.LEFT, padx=(2, 8), pady=3)
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
        self.canvas = tk.Canvas(cont, bg=PAL["desk"], highlightthickness=0,
                                borderwidth=0, takefocus=True)  # 页面可拖选文字
        self.canvas.pack(fill=tk.BOTH, expand=True)
        self._ibeam = "ibeam" if _try_cursor(self.canvas, "ibeam") else \
            ("xterm" if _try_cursor(self.canvas, "xterm") else "")
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
        self._anno_cancel()          # 键盘/按钮翻页打断拖选时清预览项
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
        obj = self.rd.get(self.cur)
        if obj is None:
            self._show_placeholder()
            self.rd.get_async(self.cur, self._on_page_ready)
            return
        self._paint_obj(obj)
        self._prefetch(self.cur)

    def _paint_obj(self, obj):
        """obj 为 PageLayout（文本页）或 PIL Image（位图页）。"""
        if isinstance(obj, PageLayout):
            self._paint_layout(obj)
        else:
            self._paint(obj)

    def _page_geom(self, page_w, page_h):
        """按“宽度适配 + zoom”计算显示尺寸与滚动几何。"""
        sc = self._display_scale(page_w)
        dw = max(1, int(page_w * sc))
        dh = max(1, int(page_h * sc))
        cw = max(self.canvas.winfo_width(), 50)
        ch = max(self.canvas.winfo_height(), 50)
        ch_eff = ch - self._foot_h                       # 扣除页脚后的可视高度
        self._max_scroll = max(0, dh - ch_eff)           # 纵向可滚动量
        self.scroll = max(0, min(self.scroll, self._max_scroll))
        top_y = (ch_eff - dh) / 2 if dh <= ch_eff else -self.scroll
        return sc, dw, dh, cw, ch, ch_eff, top_y

    def _draw_card(self, cw, dw, dh, top_y):
        """纸卡（阴影 + 卡面）。页内会随滚动平移的项统一打 page 标签。"""
        pad = 10                                    # 纸卡内衬边距
        cx = cw / 2
        # 阴影（两层，营造纸卡浮于桌面）
        _rr(self.canvas, cx - dw / 2 - pad + 3, top_y - pad + 4,
            cx + dw / 2 + pad + 3, top_y + dh + pad + 4, 8,
            fill=PAL["paper_shadow"], outline="", tags=("page",))
        _rr(self.canvas, cx - dw / 2 - pad + 5, top_y - pad + 6,
            cx + dw / 2 + pad + 5, top_y + dh + pad + 6, 8,
            fill=PAL["paper_shadow"], outline="", tags=("page",))
        # 纸卡
        _rr(self.canvas, cx - dw / 2 - pad, top_y - pad,
            cx + dw / 2 + pad, top_y + dh + pad, 8,
            fill=PAL["page_bg"], outline=PAL["paper_shadow"], tags=("page",))

    def _paint(self, img):
        """位图页（扫描件/插图页）：pdftoppm 位图贴图。"""
        sc, dw, dh, cw, ch, ch_eff, top_y = self._page_geom(img.width, img.height)
        small = img.resize((dw, dh), Image.LANCZOS)
        marks = [a for a in self.annos if a.page == self.cur]
        if marks:                                   # 当前页有标记：合成高亮层
            small = overlay_marks(small, marks)
        self.photo = ImageTk.PhotoImage(small)
        self.canvas.delete("all")
        self._draw_card(cw, dw, dh, top_y)
        # 页面图像
        self._img_id = self.canvas.create_image(cw / 2, top_y, anchor="n",
                                                image=self.photo, tags=("page",))
        self._photos = []
        self._disp = (dw, dh)
        self._top_y = top_y
        self._draw_footer(cw, ch)

    def _paint_layout(self, lay):
        """文本页：按版面逐段绘制真实文本项与插图，非位图贴图。
        缩放/放大重绘时字体按新尺寸重建，任意倍率下都清晰。"""
        sc, dw, dh, cw, ch, ch_eff, top_y = self._page_geom(lay.w, lay.h)
        self.canvas.delete("all")
        self._draw_card(cw, dw, dh, top_y)
        left = cw / 2 - dw / 2
        # 高亮标记：矩形画在文字下层（stipple 网点近似半透明，不遮字）
        for a in [a for a in self.annos if a.page == self.cur]:
            x0, y0, x1, y1 = a.rect
            self.canvas.create_rectangle(left + x0 * dw, top_y + y0 * dh,
                                         left + x1 * dw, top_y + y1 * dh,
                                         fill=a.color, stipple="gray50",
                                         outline=a.color, tags=("page",))
        # 文本选区：按行画淡蓝底，横向边界精确到字符切片
        # （同样在文字下层；随 page 标签滚动平移）
        if self.text_sel and self.text_sel[0] == self.cur:
            for ln in self._sel_spans(lay):
                rx0 = rx1 = ry0 = ry1 = None
                for tr, i0, i1 in ln:
                    size_pt, fam, _ = lay.fonts.get(tr.font, (12, "", "#000000"))
                    ft = _layout_font(fam, size_pt, sc, tr.bold, tr.italic)
                    offs = _char_offsets(tr.text, ft)
                    a = tr.x * sc + offs[i0]
                    b = tr.x * sc + offs[i1]
                    y0s, y1s = tr.y * sc, (tr.y + tr.h) * sc
                    rx0 = a if rx0 is None else min(rx0, a)
                    rx1 = b if rx1 is None else max(rx1, b)
                    ry0 = y0s if ry0 is None else min(ry0, y0s)
                    ry1 = y1s if ry1 is None else max(ry1, y1s)
                self.canvas.create_rectangle(left + rx0, top_y + ry0,
                                             left + rx1, top_y + ry1,
                                             fill=SEL_FILL, outline="", tags=("page",))
        # 插图
        self._photos = []
        for ir in lay.imgs:
            try:
                iw = max(1, int(ir.w * sc))
                ih = max(1, int(ir.h * sc))
                ph = ImageTk.PhotoImage(ir.img.resize((iw, ih), Image.LANCZOS))
                self._photos.append(ph)             # 保引用防 GC
                self.canvas.create_image(left + ir.x * sc, top_y + ir.y * sc,
                                         anchor="nw", image=ph, tags=("page",))
            except Exception:
                pass
        # 文字：锚点取各段纵向中点，单行多段（中英文混排拆段）也能对齐
        for tr in lay.texts:
            size_pt, fam, color = lay.fonts.get(tr.font, (12, "", "#000000"))
            ft = _layout_font(fam, size_pt, sc, tr.bold, tr.italic)
            self.canvas.create_text(left + tr.x * sc, top_y + (tr.y + tr.h / 2) * sc,
                                    anchor="w", text=tr.text, fill=color, font=ft,
                                    tags=("page",))
        self._img_id = None
        self._disp = (dw, dh)
        self._top_y = top_y
        self._draw_footer(cw, ch)

    def _move_page(self):
        """页内滚动：按 page 标签整体平移页卡（含文本项），不重新缩放。"""
        if self._disp is None:
            return
        dw, dh = self._disp
        cw = max(self.canvas.winfo_width(), 50)
        ch = max(self.canvas.winfo_height(), 50)
        ch_eff = ch - self._foot_h
        self._max_scroll = max(0, dh - ch_eff)
        self.scroll = max(0, min(self.scroll, self._max_scroll))
        top_y = (ch_eff - dh) / 2 if dh <= ch_eff else -self.scroll
        dy = top_y - self._top_y
        if dy:
            self.canvas.move("page", 0, dy)
            self._top_y = top_y

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
        self._photos = []
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

    def _on_page_ready(self, n, obj):
        if obj is None or n != self.cur:
            return
        try:
            if not self.root.winfo_exists():
                return
        except tk.TclError:
            return
        self._paint_obj(obj)
        self._prefetch(n)

    def _prefetch(self, n):
        """后台预取相邻页，使连续翻页几乎即时。"""
        for m in (n - 1, n + 1):
            if 1 <= m <= self.N and self.rd.get(m) is None:
                self.rd.get_async(m, lambda _n, _obj: None)

    def _poll_render(self):
        """定期取回后台渲染结果（主线程）。"""
        try:
            if not self.root.winfo_exists():
                return
        except tk.TclError:
            return
        try:
            for cb, n, obj in self.rd.drain():
                try:
                    cb(n, obj)
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

    # ----- 内容标记交互 -----
    def _toggle_mark_mode(self):
        """标记模式开关：开启为十字光标拖选；关闭为 I 型光标（可拖选文字）。"""
        self._anno_cancel()
        if self.mark_on.get():
            self.canvas.config(cursor="crosshair")
        else:
            self.canvas.config(cursor=self._ibeam or "")

    def _set_mark_color(self, hexv):
        self.mark_color.set(hexv)
        try:
            self.mark_btn.config(bg=hexv, activebackground=hexv)
        except tk.TclError:
            pass

    def _page_rect(self):
        """当前页面图像在画布上的位置 (left, top_y, dw, dh)。"""
        dw, dh = self._disp
        cw = max(self.canvas.winfo_width(), 50)
        ch = max(self.canvas.winfo_height(), 50)
        ch_eff = ch - self._foot_h
        left = cw / 2 - dw / 2
        top_y = (ch_eff - dh) / 2 if dh <= ch_eff else -self.scroll
        return left, top_y, dw, dh

    def _norm_from_canvas(self, x, y):
        """画布坐标 -> 归一化页面坐标（钳制 0..1，页外拖选自动裁边）。"""
        left, top_y, dw, dh = self._page_rect()
        nx = (x - left) / dw if dw else 0.0
        ny = (y - top_y) / dh if dh else 0.0
        return max(0.0, min(1.0, nx)), max(0.0, min(1.0, ny))

    def _anno_at(self, x, y):
        """命中测试：返回包含画布点 (x, y) 的当前页标记（后画的优先）。"""
        if self._disp is None:
            return None
        nx, ny = self._norm_from_canvas(x, y)
        for a in reversed([a for a in self.annos if a.page == self.cur]):
            x0, y0, x1, y1 = a.rect
            if x0 <= nx <= x1 and y0 <= ny <= y1:
                return a
        return None

    def _anno_press(self, ev):
        if self.mark_on.get():                 # 标记模式：拖画高亮
            if self._disp is None:
                self._anno_sel = None
                return
            self._selpress = None
            self._anno_sel = [(ev.x, ev.y), None]
            return
        # 普通模式：拖选文字（点一下空白处=清除选区，见 _anno_release）
        self._anno_sel = None
        self._selpress = (ev.x, ev.y) if self._disp is not None else None

    def _anno_drag(self, ev):
        if self.mark_on.get():
            if not self._anno_sel:
                return
            (sx, sy), _ = self._anno_sel
            self.canvas.delete("anno_preview")
            pid = self.canvas.create_rectangle(sx, sy, ev.x, ev.y,
                                               outline=self.mark_color.get(),
                                               width=2, dash=(4, 3),
                                               tags="anno_preview")
            self._anno_sel[1] = pid
            return
        if self._selpress:                     # 文字选区拖选预览
            sx, sy = self._selpress
            self.canvas.delete("sel_preview")
            self.canvas.create_rectangle(sx, sy, ev.x, ev.y,
                                         outline=PAL["brass"], width=1,
                                         dash=(3, 3), tags="sel_preview")

    def _anno_release(self, ev):
        if self.mark_on.get():
            if not self._anno_sel:
                return
            (sx, sy), _ = self._anno_sel
            self._anno_sel = None
            self.canvas.delete("anno_preview")
            if self._disp is None:
                return
            # 位移过小视为点击：命中已有标记则弹出换色/删除菜单
            if abs(ev.x - sx) < 6 and abs(ev.y - sy) < 6:
                hit = self._anno_at(ev.x, ev.y)
                if hit:
                    self._anno_menu(hit, ev.x_root, ev.y_root)
                return
            x0, y0 = self._norm_from_canvas(sx, sy)
            x1, y1 = self._norm_from_canvas(ev.x, ev.y)
            nx0, nx1 = min(x0, x1), max(x0, x1)
            ny0, ny1 = min(y0, y1), max(y0, y1)
            if (nx1 - nx0) * (ny1 - ny0) < 0.0005:      # 过滤误触
                return
            a = Annotation(page=self.cur, rect=[nx0, ny0, nx1, ny1],
                           color=self.mark_color.get())
            self.annos.append(a)
            self._save_annotations()
            self._draw()
            return
        # 普通模式：落定文字选区
        if self._selpress is None:
            return
        sx, sy = self._selpress
        self._selpress = None
        self.canvas.delete("sel_preview")
        if self._disp is None:
            return
        if abs(ev.x - sx) < 5 and abs(ev.y - sy) < 5:   # 单击：清除选区
            self._clear_selection()
            return
        if not isinstance(self.rd.get(self.cur), PageLayout):
            self._toast("此页为扫描图像，无可选文字")
            return
        x0, y0 = self._norm_from_canvas(sx, sy)
        x1, y1 = self._norm_from_canvas(ev.x, ev.y)
        self.text_sel = (self.cur, min(x0, x1), min(y0, y1),
                         max(x0, x1), max(y0, y1))
        self._draw()

    def _anno_cancel(self):
        """取消进行中的拖选预览（翻页/关闭模式时）。"""
        self._anno_sel = None
        self._selpress = None
        try:
            self.canvas.delete("anno_preview")
            self.canvas.delete("sel_preview")
        except tk.TclError:
            pass

    # ----- 文本选区与复制 -----
    def _sel_spans(self, lay):
        """选区覆盖的字符片段 -> 按阅读顺序分行的 [[(TextRun, i0, i1)]]。
        i0/i1 为 TextRun 内的字符切片下标：纵向要求段中线落在选区带内
        （或盖过半高），横向用 tk 字体逐字测量把选区边界映射到字符级，
        矩形拖选精确到单个字。"""
        if not self.text_sel or self.text_sel[0] != self.cur:
            return []
        _, nx0, ny0, nx1, ny1 = self.text_sel
        sc = self._display_scale(lay.w)
        x0, x1 = nx0 * lay.w * sc, nx1 * lay.w * sc
        y0, y1 = ny0 * lay.h * sc, ny1 * lay.h * sc
        spans = []
        for tr in lay.texts:
            ty0, ty1 = tr.y * sc, (tr.y + tr.h) * sc
            if not (y0 <= ty0 + tr.h * sc / 2 <= y1
                    or min(y1, ty1) - max(y0, ty0) >= 0.5 * tr.h * sc):
                continue
            size_pt, fam, _ = lay.fonts.get(tr.font, (12, "", "#000000"))
            ft = _layout_font(fam, size_pt, sc, tr.bold, tr.italic)
            offs = _char_offsets(tr.text, ft)
            i0 = i1 = None
            for i in range(len(tr.text)):
                cx = tr.x * sc + (offs[i] + offs[i + 1]) / 2
                if x0 <= cx <= x1:
                    if i0 is None:
                        i0 = i
                    i1 = i + 1
            if i0 is not None:
                spans.append((tr, i0, i1))
        spans.sort(key=lambda s: (s[0].y, s[0].x))
        lines, cur = [], None
        for s in spans:
            tr = s[0]
            if (cur and abs((tr.y + tr.h / 2) - (cur[0][0].y + cur[0][0].h / 2))
                    <= 0.5 * max(tr.h, cur[0][0].h)):
                cur.append(s)
            else:
                cur = [s]
                lines.append(cur)
        for ln in lines:
            ln.sort(key=lambda s: s[0].x)
        return lines

    def _sel_text(self):
        """选中文本 -> 字符串（切片拼接；段间隙明显大于字距时补空格，
        CJK 段内伪迹空格收紧，行间换行）。"""
        obj = self.rd.get(self.cur)
        if not isinstance(obj, PageLayout) or not self.text_sel:
            return ""
        parts = []
        for ln in self._sel_spans(obj):
            buf, prev = "", None
            for tr, i0, i1 in ln:
                if prev is not None:
                    ptr, pi0, pi1 = prev
                    if pi1 >= len(ptr.text) and i0 == 0:
                        gap = tr.x - (ptr.x + ptr.w)
                        size = obj.fonts.get(tr.font, (12,))[0] or 12
                        if (gap > 0.6 * size and not buf.endswith(" ")
                                and not tr.text.startswith(" ")):
                            buf += " "
                buf += _cjk_tight(tr.text[i0:i1])
                prev = (tr, i0, i1)
            buf = buf.strip()
            if buf:
                parts.append(buf)
        return "\n".join(parts)

    def _copy_selection(self, ev=None):
        txt = self._sel_text()
        if not txt:
            self._toast("未选中文本")
            return
        self.root.clipboard_clear()
        self.root.clipboard_append(txt)
        self._toast("已复制 %d 字" % len(txt.replace("\n", "")))

    def _clear_selection(self):
        if self.text_sel:
            self.text_sel = None
            self._draw()

    def _sel_menu(self, ev):
        """右键：有选区时弹复制/清除菜单。"""
        if not self.text_sel or self.text_sel[0] != self.cur or not self._sel_text():
            return
        m = tk.Menu(self.canvas, tearoff=0, bg=PAL["panel"], fg=PAL["paper"],
                    activebackground=PAL["brass"], activeforeground=PAL["ink"])
        m.add_command(label="复制选中文本（Ctrl+C）", command=self._copy_selection)
        m.add_command(label="清除选区（Esc）", command=self._clear_selection)
        try:
            m.tk_popup(ev.x_root, ev.y_root)
        finally:
            m.grab_release()

    def _toast(self, msg):
        """画布顶部轻提示（1.8 秒自动消失，不打断阅读）。"""
        try:
            self.canvas.delete("toast")
        except tk.TclError:
            pass
        cw = max(self.canvas.winfo_width(), 50)
        tid = _rr(self.canvas, cw / 2 - 90, 12, cw / 2 + 90, 40, 8,
                  fill=PAL["panel"], outline=PAL["brass_dim"], tags=("toast",))
        self.canvas.create_text(cw / 2, 26, text=msg, fill=PAL["paper"],
                                font=_font("ui", 10), tags=("toast",))
        self._toast_tok += 1
        tok = self._toast_tok
        def _rm():
            if self._toast_tok == tok:
                try:
                    self.canvas.delete("toast")
                except tk.TclError:
                    pass
        self.root.after(1800, _rm)
        return tid

    def _anno_menu(self, anno, x, y):
        """点击已有标记：弹出换色（四色）+ 删除菜单。"""
        m = tk.Menu(self.canvas, tearoff=0, bg=PAL["panel"], fg=PAL["paper"],
                    activebackground=PAL["brass"], activeforeground=PAL["ink"])
        for _name, _hexv in MARK_COLORS.items():
            m.add_command(label="改色 · " + _name, foreground=_hexv,
                          command=lambda a=anno, v=_hexv: self._anno_recolor(a, v))
        m.add_separator()
        m.add_command(label="删除标记",
                      command=lambda a=anno: self._anno_delete(a))
        try:
            m.tk_popup(x, y)
        finally:
            m.grab_release()

    def _anno_recolor(self, anno, hexv):
        anno.color = hexv
        self._save_annotations()
        self._draw()

    def _anno_delete(self, anno):
        if anno in self.annos:
            self.annos.remove(anno)
        self._save_annotations()
        self._draw()

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
    elif cap.fmt == "KDH":
        L.append("KDH 2.00 为新版知网下载的加密格式：头部 0x3C 处指向")
        L.append("尾部明文 XML 元数据，其间为多字节循环 XOR 加密的 PDF。")
        L.append("本程序按字节频率分析自动恢复密钥并解出内嵌 PDF，")
        L.append("同一文件第二次打开走 sidecar 缓存，近瞬时。")
    else:
        L.append("此格式变体暂不完整支持。")
    L.append("=" * 56)
    return "\n".join(L)

def main():
    if len(sys.argv) < 2:
        # 无参数启动（应用菜单/桌面图标）：弹出文件选择对话框。
        # 无 GUI 环境（缺 tkinter/无显示器）则退回打印用法。
        try:
            root = tk.Tk()
            root.withdraw()
            path = filedialog.askopenfilename(title="打开文档",
                                              filetypes=OPEN_FILETYPES)
            root.destroy()
        except Exception:
            print(__doc__)
            sys.exit(1)
        if not path:                        # 用户取消：静默退出
            return
    else:
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