# -*- coding: utf-8 -*-
# ============================================================================
# gen_synth_board.py —— 生成【合成棋盘格标定图】，用来验证标定程序对不对
# ----------------------------------------------------------------------------
# 【它干什么】
#   不用相机、不用打印标定板，用一组已知的相机内参 K / 畸变 D，
#   把棋盘格纹理按不同的姿态和距离"投影"到画面里，存成一堆 jpg。
#   然后拿 calib_camera_usb 去标定，看解出来的 fx/fy/cx/cy 能不能回到已知值。
#
#   这是标定程序唯一靠谱的自检方式：
#   拿真相机标出来的数，你没法判断是标定程序错了还是相机就那样；
#   而合成图的正确答案是已知的，误差超过 1% 就说明程序有问题。
#
# 【关键设计：为什么要"分层挑选"视角】
#   标定里 cx/cy 和畸变系数 k1 是【弱约束】参数 —— 只有当标定板出现在画面
#   边角、并且几乎占满画面时，才解得准。如果所有照片里板子都小小地待在画面
#   中央（很容易发生，就是"举着板子站在相机正前方"），那 fx/fy 还能标对，
#   cx/cy/k1 会偏得离谱，但重投影误差依然很小 —— 这种"看着没问题其实是错的"
#   结果最容易坑人。
#   所以这里把画面切成 3x3 九宫格，每格轮流挑一张，强制覆盖到边角。
#   下面 --center 开关可以复现"板子全在中央"的坏数据集，用来对比。
#
# 【用法】（Windows 上用 miniconda 的 python 跑）
#   python gen_synth_board.py <输出目录> [张数] [center]
#   python gen_synth_board.py _synth_board 25           # 覆盖好，推荐
#   python gen_synth_board.py _synth_bad 25 center      # 故意都放中央，看能错多少
#
# 【已知参数（脚本里改）】
#   W, H      画面尺寸          800x600
#   fx, fy    焦距(像素)        1230.0 / 1225.0
#   cx, cy    光心(像素)        400.0 / 300.0
#   D         畸变系数          k1=-0.15 k2=0.05 p1=0.001 p2=-0.001 k3=0
#   COLS/ROWS 内角点            11 x 8（大凡 Q12-340-25）
#   S         方格边长(mm)      25.0
# ============================================================================

import os
import sys

import cv2
import numpy as np

# 中文输出不乱码
sys.stdout.reconfigure(encoding="utf-8")

# ---- 已知的"真值" ----
W, H = 800, 600
FX, FY = 1230.0, 1225.0
CX, CY = 400.0, 300.0
D = np.array([-0.15, 0.05, 0.001, -0.001, 0.0])          # k1 k2 p1 p2 k3
K = np.array([[FX, 0.0, CX],
              [0.0, FY, CY],
              [0.0, 0.0, 1.0]])

COLS, ROWS = 11, 8                                       # 内角点：11 列 8 行（= 12x9 个方格）
S = 25.0                                                 # 方格边长 mm
BW = (COLS + 1) * S                                      # 板子宽度 300mm
BH = (ROWS + 1) * S                                      # 板子高度 225mm

SQ = 100                                                 # 纹理里一个方格多少像素
MARGIN = 80                                              # 纹理四周白边

EDGE = 25                                                # 板子四角离画面边界至少留多少像素


def make_texture():
    """画一张棋盘格纹理：(COLS+1) x (ROWS+1) 个方格，四周留白边"""
    tw = (COLS + 1) * SQ + 2 * MARGIN
    th = (ROWS + 1) * SQ + 2 * MARGIN
    tex = np.full((th, tw), 255, np.uint8)               # 先铺白
    for i in range(ROWS + 1):                            # 行
        for j in range(COLS + 1):                        # 列
            if (i + j) % 2 == 0:                         # 棋盘格相间涂黑
                y0 = MARGIN + i * SQ
                x0 = MARGIN + j * SQ
                tex[y0:y0 + SQ, x0:x0 + SQ] = 0
    return tex


def make_background():
    """灰色带一点渐变的背景，模拟桌面/地面（纯白背景会让板子边界糊掉）"""
    bg = np.zeros((H, W), np.uint8)
    for y in range(H):
        bg[y, :] = 95 + int(35.0 * y / H)
    return bg


def euler_rvec(rx, ry, rz):
    """三个欧拉角（度）转成 Rodrigues 旋转向量"""
    rx, ry, rz = np.deg2rad([rx, ry, rz])
    Rx = np.array([[1, 0, 0], [0, np.cos(rx), -np.sin(rx)], [0, np.sin(rx), np.cos(rx)]])
    Ry = np.array([[np.cos(ry), 0, np.sin(ry)], [0, 1, 0], [-np.sin(ry), 0, np.cos(ry)]])
    Rz = np.array([[np.cos(rz), -np.sin(rz), 0], [np.sin(rz), np.cos(rz), 0], [0, 0, 1]])
    R = Rz @ Ry @ Rx
    return cv2.Rodrigues(R)[0]


def candidates():
    """枚举候选姿态：近处占满画面、远处小目标，再加上大范围平移把板子推到画面边角"""
    out = []
    # 距离：从"几乎占满画面"到"只占一小块"都要有
    for d in (560.0, 640.0, 740.0, 860.0, 1000.0, 1200.0, 1450.0):
        for rx in (-35, -22, -10, 0, 10, 22, 35):
            for ry in (-35, -22, -10, 0, 10, 22, 35):
                for rz in (-12, 0, 12):
                    # 平移范围随距离放大，保证近处的板子也能被推到画面边上
                    for tx in (-0.28, -0.16, -0.06, 0.0, 0.06, 0.16, 0.28):
                        for ty in (-0.22, -0.12, 0.0, 0.12, 0.22):
                            out.append((d, rx, ry, rz, tx * d, ty * d))
    return out


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "_synth_board"
    want = int(sys.argv[2]) if len(sys.argv) > 2 else 25
    center_only = len(sys.argv) > 3 and sys.argv[3] == "center"
    os.makedirs(out_dir, exist_ok=True)

    np.random.seed(0)
    tex = make_texture()
    bg = make_background()

    # 板子四个外角在板面坐标系里的位置（Z=0，原点在左上外角，单位 mm）
    obj_quad = np.array([[0, 0, 0], [BW, 0, 0], [BW, BH, 0], [0, BH, 0]], np.float64)
    # 上面四个角在纹理图里对应的像素位置
    tex_quad = np.array([[MARGIN, MARGIN],
                         [MARGIN + (COLS + 1) * SQ, MARGIN],
                         [MARGIN + (COLS + 1) * SQ, MARGIN + (ROWS + 1) * SQ],
                         [MARGIN, MARGIN + (ROWS + 1) * SQ]], np.float32)

    cands = candidates()
    np.random.shuffle(cands)                             # 打乱，让每个格子里挑到的姿态也是随机的

    # ---- 第一轮：几何过滤，并按"板心落在九宫格哪一格"分类 ----
    cells = {}                                           # cell -> [候选...]
    tried = 0
    for (d, rx, ry, rz, tx, ty) in cands:
        tried += 1
        rvec = euler_rvec(rx, ry, rz)
        tvec = np.array([[tx], [ty], [d]], np.float64)

        proj, _ = cv2.projectPoints(obj_quad, rvec, tvec, K, D)   # 四个外角投到图像上
        proj = proj.reshape(-1, 2).astype(np.float32)

        # 1) 四个角要完整落在画面里
        if proj[:, 0].min() < EDGE or proj[:, 0].max() > W - EDGE:
            continue
        if proj[:, 1].min() < EDGE or proj[:, 1].max() > H - EDGE:
            continue
        # 2) 四边形不能太扁（面积太小说明几乎侧对着看）
        if abs(cv2.contourArea(proj)) < 12000:
            continue

        ccx, ccy = float(proj[:, 0].mean()), float(proj[:, 1].mean())
        if center_only:                                  # 坏数据集：只保留最中央那一格
            cell = (1, 1)
            if abs(ccx - W / 2.0) > W / 6.0 or abs(ccy - H / 2.0) > H / 6.0:
                continue
        else:                                            # 好数据集：九宫格分层
            cell = (min(2, int(ccy / (H / 3.0))), min(2, int(ccx / (W / 3.0))))
        cells.setdefault(cell, []).append((d, rx, ry, rz, tx, ty, proj, ccx, ccy))

    if not cells:
        print("✗ 一个合格候选都没有，检查参数")
        return 1

    # ---- 第二轮：九宫格轮流挑，强制覆盖画面每个区域 ----
    picked = []
    order = sorted(cells.keys())
    while len(picked) < want and any(cells[c] for c in order):
        for c in order:
            if cells[c] and len(picked) < want:
                picked.append(cells[c].pop(0))

    # ---- 渲染并保存 ----
    board_size = (COLS, ROWS)                            # findChessboardCorners 要 (列, 行)
    saved = 0
    for (d, rx, ry, rz, tx, ty, proj, ccx, ccy) in picked:
        M = cv2.getPerspectiveTransform(tex_quad, proj)           # 纹理 -> 图像 的单应
        board_img = cv2.warpPerspective(tex, M, (W, H), flags=cv2.INTER_LINEAR)
        mask = cv2.warpPerspective(np.full_like(tex, 255), M, (W, H), flags=cv2.INTER_NEAREST)
        frame = np.where(mask > 0, board_img, bg).astype(np.uint8)  # 把板子贴到背景上

        frame = cv2.GaussianBlur(frame, (0, 0), 0.8)              # 轻微模糊，模拟镜头
        noise = np.random.normal(0.0, 2.5, frame.shape)           # 加噪声，模拟传感器
        frame = np.clip(frame.astype(np.float32) + noise, 0, 255).astype(np.uint8)

        # 自检：这张图必须真的能被检出 COLSxROWS 个内角点，否则不算数
        found, _ = cv2.findChessboardCorners(
            frame, board_size,
            flags=cv2.CALIB_CB_ADAPTIVE_THRESH + cv2.CALIB_CB_NORMALIZE_IMAGE)

        saved += 1
        path = os.path.join(out_dir, "%d.jpg" % saved)
        cv2.imwrite(path, frame, [cv2.IMWRITE_JPEG_QUALITY, 92])
        print("  [%2d] d=%4.0fmm rx=%4d ry=%4d  板心(%4.0f,%4.0f)  %s"
              % (saved, d, rx, ry, ccx, ccy, "检出" if found else "★ 程序没检出 ★"))

    # 覆盖情况汇总
    cover = sorted(set((min(2, int(p[8] / (H / 3.0))), min(2, int(p[7] / (W / 3.0)))) for p in picked))
    print()
    print("生成 %d 张 -> %s" % (saved, out_dir))
    print("画面九宫格覆盖：%d/9 格  %s" % (len(cover), sorted(cover)))
    if len(cover) <= 2:
        print("⚠ 只覆盖了很少的格子，cx/cy 和畸变会标不准（这正是 center 模式要演示的）")
    print()
    print("真值（calib_camera_usb 应该能标回这几个数）：")
    print("  fx=%.1f  fy=%.1f  cx=%.1f  cy=%.1f" % (FX, FY, CX, CY))
    print("  k1=%.4f  k2=%.4f  p1=%.5f  p2=%.5f" % tuple(D[:4]))
    print("  尺寸 %dx%d，内角点 %dx%d，方格 %.1fmm" % (W, H, COLS, ROWS, S))
    return 0


if __name__ == "__main__":
    sys.exit(main())
