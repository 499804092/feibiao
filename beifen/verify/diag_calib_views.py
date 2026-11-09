# -*- coding: utf-8 -*-
# ============================================================================
# diag_calib_views.py —— 逐张诊断标定图：清晰度 / 板子位置大小 / 重投影误差
# ----------------------------------------------------------------------------
# 【为什么需要】
#   标定结果不好时，calib_camera_usb 只告诉你"误差最大的 5 张"。
#   但要决定"删掉重来"还是"全部重拍"，还需要知道：
#     · 每张图糊不糊（清晰度）
#     · 板子在画面里多大、在什么位置（决定 fx/fy 和 cx/cy 的约束强不强）
#     · 每张图各自的误差（不只是最差的 5 张）
#
# 【它怎么算】
#   清晰度  = 拉普拉斯方差，越大越清晰。糊图一般 < 50，清楚的一般 > 100
#   板子尺寸= 88 个角点的外接框（像素），反映距离远近——远近要有变化，fx/fy 才解得准
#   板心    = 88 个角点的重心（像素），必须铺满画面【包括四个角】，cx/cy 才解得准
#   误差    = 用标定结果把 3D 点重投影回去，和检测到的角点比，单位像素
#
# 【用法】
#   python3 diag_calib_views.py [图片目录]
# ============================================================================

import glob
import os
import sys

import cv2
import numpy as np

sys.stdout.reconfigure(encoding="utf-8")

COLS, ROWS = 11, 8          # 大凡 Q12-340-25
S = 25.0
FOLDER = sys.argv[1] if len(sys.argv) > 1 else "calib_out"


def natural(path):
    stem = os.path.splitext(os.path.basename(path))[0]
    digits = "".join(ch for ch in stem if ch.isdigit())
    return int(digits) if digits else 0


files = sorted(glob.glob(os.path.join(FOLDER, "*.jpg")), key=natural)
if not files:
    print("目录 %s 里没有 jpg" % FOLDER)
    sys.exit(1)

objp = np.zeros((ROWS * COLS, 3), np.float32)
objp[:, :2] = np.mgrid[0:COLS, 0:ROWS].T.reshape(-1, 2)
objp *= S

rows = []
img_size = None
obj_points, img_points = [], []

for f in files:
    img = cv2.imread(f)
    if img is None:
        rows.append((natural(f), os.path.basename(f), -1, None, None, None, None))
        continue
    if img_size is None:
        img_size = (img.shape[1], img.shape[0])
    gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    sharp = float(cv2.Laplacian(gray, cv2.CV_64F).var())

    ok, corners = cv2.findChessboardCorners(
        gray, (COLS, ROWS),
        flags=cv2.CALIB_CB_ADAPTIVE_THRESH + cv2.CALIB_CB_NORMALIZE_IMAGE)
    if not ok:
        rows.append((natural(f), os.path.basename(f), sharp, None, None, None, "没检出"))
        continue
    corners = cv2.cornerSubPix(
        gray, corners, (11, 11), (-1, -1),
        (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_COUNT, 30, 0.01))
    pts = corners.reshape(-1, 2)
    w = float(pts[:, 0].max() - pts[:, 0].min())
    h = float(pts[:, 1].max() - pts[:, 1].min())
    ctr = (float(pts[:, 0].mean()), float(pts[:, 1].mean()))
    obj_points.append(objp)
    img_points.append(corners)
    rows.append((natural(f), os.path.basename(f), sharp, (w, h), ctr, None, None))

if len(obj_points) < 6:
    print("有效图不足 6 张，标不了")
    sys.exit(1)

rms, K, D, rvecs, tvecs = cv2.calibrateCamera(
    obj_points, img_points, img_size, None, None,
    flags=cv2.CALIB_FIX_K3,
    criteria=(cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_COUNT, 100, np.finfo(float).eps))

errs = []
for i in range(len(obj_points)):
    proj, _ = cv2.projectPoints(obj_points[i], rvecs[i], tvecs[i], K, D)
    errs.append(float(np.mean(np.linalg.norm(img_points[i] - proj, axis=2))))

# 把误差填回表里：按文件自然序对齐有效图
ei = 0
fixed = []
for r in rows:
    if r[6] is None:                       # 这张有效
        fixed.append((r[0], r[1], r[2], r[3], r[4], errs[ei], None))
        ei += 1
    else:
        fixed.append((r[0], r[1], r[2], r[3], r[4], None, r[6]))

print("============ 逐张诊断  目录 %s  %d 张 ============" % (FOLDER, len(files)))
print("尺寸 %dx%d   标定 RMS %.4f px   fx=%.1f fy=%.1f cx=%.1f cy=%.1f"
      % (img_size[0], img_size[1], rms, K[0, 0], K[1, 1], K[0, 2], K[1, 2]))
print()
print("%-5s %-10s %9s %12s %14s %9s %s" %
      ("序", "文件", "清晰度", "板子宽x高", "板心(x,y)", "误差px", "备注"))
print("-" * 82)

order = sorted(fixed, key=lambda r: (-(r[5] if r[5] is not None else -1)))
for r in order:
    idx, name, sharp, size, ctr, err, note = r
    if note:
        print("%-5d %-10s %9.0f %12s %14s %9s %s" % (idx, name, sharp, "-", "-", "-", note))
    else:
        flag = ""
        if err is not None and err > 1.0:
            flag = "★误差大"
        if sharp < 50:
            flag += " ★很糊"
        print("%-5d %-10s %9.0f %5.0fx%-5.0f (%5.0f,%5.0f) %9.3f %s"
              % (idx, name, sharp, size[0], size[1], ctr[0], ctr[1], err, flag))

print()
good = [r for r in fixed if r[5] is not None]
bad = [r for r in good if r[5] > 1.0]
print("统计：有效 %d 张，其中误差 >1px 的 %d 张" % (len(good), len(bad)))
if good:
    sharp_vals = [r[2] for r in good]
    sizes = [max(r[3][0], r[3][1]) for r in good]
    print("      清晰度  最小 %.0f  最大 %.0f  中位 %.0f" %
          (min(sharp_vals), max(sharp_vals), float(np.median(sharp_vals))))
    print("      板子大小 最小 %.0fpx  最大 %.0fpx（差 %d 倍才算远近有变化）"
          % (min(sizes), max(sizes), 1))
    print()
    print("      九宫格覆盖（板心落在哪几格）:")
    grid = set()
    for r in good:
        gx = min(2, int(r[4][0] / (img_size[0] / 3.0)))
        gy = min(2, int(r[4][1] / (img_size[1] / 3.0)))
        grid.add((gy, gx))
    print("        %s   → %d/9 格" % (sorted(grid), len(grid)))

print()
print("怎么改：")
print("  · 误差 >1px 或 清晰度 <50 的图，删掉（rm 对应文件）再重标")
print("  · 板子大小只差 1 倍多 → 远近变化不够，fx/fy 会解不准，要补远近距离的图")
print("  · 九宫格覆盖 <6 格 → cx/cy 会解不准，要把板子挪到画面四角补拍")
