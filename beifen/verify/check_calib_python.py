# -*- coding: utf-8 -*-
# ============================================================================
# check_calib_python.py —— 用 Python 版 OpenCV 复算一遍，给 calib_camera_usb 当参照
# ----------------------------------------------------------------------------
# 【它干什么】三件事：
#   ① 用和 calib_camera_usb 完全一样的算法（找角点 + 亚像素 + calibrateCamera）
#      在 Python 里再标一次。两边结果应当几乎一致 —— 不一致就是我 C++ 写错了。
#   ② 把结果和合成图的真值对比，看 fx/fy/cx/cy/k1 各偏多少。
#   ③ 【最关键】用真值 K/D 重新求每张图的外参，算出"真值能达到的重投影误差"。
#      如果真值的重投影误差和标定结果差不多，说明这组数据本来就分不出这两组参数
#      （纯平面标定板的固有毛病），不是程序算错。
#
# 【用法】
#   python check_calib_python.py <图片目录>
#   python check_calib_python.py _synth_wide
# ============================================================================

import glob
import os
import sys

import cv2
import numpy as np

sys.stdout.reconfigure(encoding="utf-8")

# 和 gen_synth_board.py 里保持一致的"真值"
FX, FY = 1230.0, 1225.0
CX, CY = 400.0, 300.0
D_TRUE = np.array([-0.15, 0.05, 0.001, -0.001, 0.0])
K_TRUE = np.array([[FX, 0.0, CX], [0.0, FY, CY], [0.0, 0.0, 1.0]])

COLS, ROWS = 11, 8          # 大凡 Q12-340-25：12x9 个方格 → 11x8 个内角点
S = 25.0
BOARD = (COLS, ROWS)


def natural_key(path):
    """按文件名里的数字排序，避免 1,10,11,2 这种顺序"""
    stem = os.path.splitext(os.path.basename(path))[0]
    digits = ""
    for ch in reversed(stem):
        if ch.isdigit():
            digits = ch + digits
        else:
            break
    return (0, int(digits), stem) if digits else (1, 0, stem)


def main():
    folder = sys.argv[1] if len(sys.argv) > 1 else "_synth_wide"
    files = sorted(glob.glob(os.path.join(folder, "*.jpg")), key=natural_key)
    if not files:
        print("✗ %s 里没有 jpg" % folder)
        return 1
    print("参照标定（Python / OpenCV %s）  目录 %s  共 %d 张" %
          (cv2.__version__, folder, len(files)))

    objp = np.zeros((ROWS * COLS, 3), np.float32)
    objp[:, :2] = np.mgrid[0:COLS, 0:ROWS].T.reshape(-1, 2)
    objp *= S                                        # 单位 mm，和 C++ 版一致

    obj_points, img_points = [], []
    img_size = None
    for f in files:
        img = cv2.imread(f, cv2.IMREAD_COLOR)
        if img is None:
            continue
        if img_size is None:
            img_size = (img.shape[1], img.shape[0])
        gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
        ok, corners = cv2.findChessboardCorners(
            gray, BOARD,
            flags=cv2.CALIB_CB_ADAPTIVE_THRESH + cv2.CALIB_CB_NORMALIZE_IMAGE)
        if not ok:
            print("  %s 没检出，跳过" % os.path.basename(f))
            continue
        corners = cv2.cornerSubPix(                             # 亚像素细化，和 C++ 版一致
            gray, corners, (11, 11), (-1, -1),
            (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_COUNT, 30, 0.01))
        obj_points.append(objp)
        img_points.append(corners)

    print("有效 %d 张，尺寸 %dx%d" % (len(obj_points), img_size[0], img_size[1]))
    if len(obj_points) < 6:
        print("✗ 太少，标不了")
        return 1

    # ---- ① 和 C++ 版同参数标定 ----
    criteria = (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_COUNT, 100, np.finfo(float).eps)
    rms, K, D, rvecs, tvecs = cv2.calibrateCamera(
        obj_points, img_points, img_size, None, None,
        flags=cv2.CALIB_FIX_K3, criteria=criteria)

    fx, fy = K[0, 0], K[1, 1]
    cx, cy = K[0, 2], K[1, 2]
    print()
    print("---- Python 标定结果 ----")
    print("  RMS 重投影误差 : %.4f px" % rms)
    print("  fx=%.2f  fy=%.2f  cx=%.2f  cy=%.2f" % (fx, fy, cx, cy))
    print("  D = %s" % np.array2string(D.ravel(), precision=6))

    # ---- ② 和真值比 ----
    print()
    print("---- 和真值的偏差 ----")
    print("  fx %+8.2f (%+.2f%%)" % (fx - FX, 100.0 * (fx - FX) / FX))
    print("  fy %+8.2f (%+.2f%%)" % (fy - FY, 100.0 * (fy - FY) / FY))
    print("  cx %+8.2f px" % (cx - CX))
    print("  cy %+8.2f px" % (cy - CY))
    print("  k1 %+8.5f   (真值 %+.5f)" % (D.ravel()[0], D_TRUE[0]))
    print("  k2 %+8.5f   (真值 %+.5f)" % (D.ravel()[1], D_TRUE[1]))

    # ---- ③ 真值能达到的重投影误差（判断这组数据能不能分辨这两组参数）----
    err_true_sum, n_pts = 0.0, 0
    for i in range(len(obj_points)):
        okp, rv, tv = cv2.solvePnP(obj_points[i], img_points[i], K_TRUE, D_TRUE)
        proj, _ = cv2.projectPoints(obj_points[i], rv, tv, K_TRUE, D_TRUE)
        err_true_sum += float(np.sum(np.linalg.norm(img_points[i] - proj, axis=2)))
        n_pts += len(obj_points[i])
    err_true = err_true_sum / n_pts

    # 标定结果自身的平均重投影误差
    err_cal_sum = 0.0
    for i in range(len(obj_points)):
        proj, _ = cv2.projectPoints(obj_points[i], rvecs[i], tvecs[i], K, D)
        err_cal_sum += float(np.sum(np.linalg.norm(img_points[i] - proj, axis=2)))
    err_cal = err_cal_sum / n_pts

    print()
    print("---- 关键判据：这组数据能不能分辨出真值 ----")
    print("  用【真值 K/D】重投的误差         : %.4f px" % err_true)
    print("  用【标定结果 K/D】重投的误差     : %.4f px" % err_cal)
    if err_cal <= err_true * 1.25 + 0.05:
        print("  → 两者几乎一样。说明这组数据本身就分不清这两组参数，")
        print("    标定程序没有算错；cx/cy 的偏差是【纯平面标定板】的固有毛病，")
        print("    要标准必须让板子出现在画面【边角】并尽量占满画面。")
    else:
        print("  → 标定结果明显比真值差，这才是程序有问题，要查。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
