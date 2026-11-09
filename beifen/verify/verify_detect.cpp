// ============================================================================
// verify_detect.cpp —— 识别链路坐标换算 离线校验（自洽定标版）
// ----------------------------------------------------------------------------
// 【方法】不靠人肉推公式，而是"先正向埋点、再反向还原"，让程序自己验证一致性：
//   ① 在 ori_mat 上挑若干"真值"靶心位置
//   ② 用与 infer() 完全相同的 letterBox 把它们映射到画布（= 模型会输出的坐标）
//   ③ 再用与 infer() 完全相同的 fitRec 把它们还原
//   ④ 比较还原值与真值 —— 误差必须接近 0，否则说明公式不自洽
//
// 【实测尺寸链路】
//   相机 ROI          640 列 x 384 行
//   transpose         384 列 x 640 行
//   flip(-1),flip(1)  384 列 x 640 行     ← ori_mat
//   letterBox(dst = cv::Size(640,384)):
//       r = min(640/384, 384/640) = 0.6   → 内图 230 列 x 384 行
//       画布 640 列 x 384 行（左右灰边各 205 列）= 网络输入张量 1x3x384x640
//   正向：画布 = ori_mat * 0.6 + (205, 0)
//   反向：ori_mat = (画布 - 画布中心) * 1.6667 + ori_mat 中心      （fitRec，scale=1/0.6）
//
// 编译运行：
//   g++ -std=c++17 -O2 -Wall verify_detect.cpp -o verify_detect $(pkg-config --cflags --libs opencv4)
//   ./verify_detect
// ============================================================================

#include <opencv2/opencv.hpp>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <vector>

// ---------------- 与 OpenvinoInfer::letterBox 逐字一致 ----------------
struct Resize { cv::Mat resized_image; int dx, dy; };

static Resize letterBox(cv::Mat &src, const cv::Size2d &dst_size) {
    int w = src.cols, h = src.rows;
    double r_w = (double)dst_size.width / w;
    double r_h = (double)dst_size.height / h;
    double r = std::min(r_w, r_h);

    int new_w = (int)(w * r), new_h = (int)(h * r);

    cv::Mat resized_image;
    cv::resize(src, resized_image, cv::Size(new_w, new_h), cv::INTER_CUBIC);

    cv::Mat canvas(dst_size, CV_8UC3, cv::Scalar(128, 128, 128));
    int dx = (int)((dst_size.width - new_w) / 2.);
    int dy = (int)((dst_size.height - new_h) / 2.);
    resized_image.copyTo(canvas(cv::Rect(dx, dy, new_w, new_h)));

    return {canvas, dx, dy};
}

// ---------------- 与 OpenvinoInfer::fitRec 逐字一致 ----------------
// 注意 fitRec 变换的是【左上角坐标 + 宽高】，所以埋点时要先把中心点转成左上角
static void fitRec(std::vector<cv::Rect2d> &boxes,
                   std::vector<cv::Point2d> &centers,
                   cv::Size2d ori_size, cv::Size2d now_size) {
    if (now_size.width <= 1. || now_size.height <= 1. ||
        ori_size.width <= 1. || ori_size.height <= 1.) {
        return;
    }
    double scale = std::max((double)ori_size.width / now_size.width,
                            (double)ori_size.height / now_size.height);
    for (size_t i = 0; i < boxes.size(); ++i) {
        auto &box = boxes[i];
        box.x = (box.x - now_size.width / 2) * scale + ori_size.width / 2;
        box.y = (box.y - now_size.height / 2) * scale + ori_size.height / 2;
        box.width  *= scale;
        box.height *= scale;
        centers[i].x = (centers[i].x - now_size.width / 2) * scale + ori_size.width / 2;
        centers[i].y = (centers[i].y - now_size.height / 2) * scale + ori_size.height / 2;
    }
}

static int g_fail = 0;
static void check(bool ok, const char *what) {
    printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) g_fail++;
}

int main() {
    printf("========= 识别链路坐标换算 离线校验（自洽定标版） =========\n\n");

    // cv::Mat 构造是 (行, 列) = (高, 宽)：ori_mat = 384 列 x 640 行 → Mat(640, 384)
    cv::Size2d ori_size(384, 640);
    cv::Size2d net_size(640, 384);
    cv::Mat src((int)ori_size.height, (int)ori_size.width, CV_8UC3, cv::Scalar(0));

    Resize rz = letterBox(src, net_size);

    // 【重点】letterBox 返回的 resized_image 就是【画布】(= 网络输入)，里面含灰边；
    //   内图尺寸要从 (画布宽 - 2*dx) 反推出来。之前把它当内图看，导致判断错误。
    int canvas_w = rz.resized_image.cols;
    int canvas_h = rz.resized_image.rows;
    int inner_w  = canvas_w - 2 * rz.dx;
    int inner_h  = canvas_h - 2 * rz.dy;
    double r = (double)inner_w / ori_size.width;      // 正向缩放比 = 内图宽 / 原图宽

    printf("---- 1) 尺寸链路（实测） ----\n");
    printf("ori_mat            : %d 列 x %d 行\n", src.cols, src.rows);
    printf("等比缩放内图       : %d 列 x %d 行   (正向缩放比 %.4f)\n", inner_w, inner_h, r);
    printf("画布 = 网络输入    : %d 列 x %d 行   (灰边 dx=%d dy=%d)\n", canvas_w, canvas_h, rz.dx, rz.dy);
    printf("正向: 画布 = ori_mat x %.4f + (%d, %d)\n", r, rz.dx, rz.dy);
    double fs = std::max(ori_size.width / net_size.width, ori_size.height / net_size.height);
    printf("反向: fitRec 的 scale = max(%.0f/%.0f, %.0f/%.0f) = %.4f，1/r = %.4f  → %s\n\n",
           ori_size.width, net_size.width, ori_size.height, net_size.height,
           fs, 1 / r, (std::fabs(fs - 1 / r) < 1e-9) ? "相等 ✓（正反互为逆运算）" : "不相等 ✗");

    check(canvas_w == 640 && canvas_h == 384, "letterBox 返回的画布 = 640 x 384（= 网络输入）");
    check(inner_w == 230 && inner_h == 384, "等比缩放内图 = 230 x 384");
    check(std::fabs(r - 0.6) < 0.01, "正向缩放比 ≈ 0.6（384x640 → 230x384，整数取整带来 0.17% 偏差）");
    check(std::fabs(fs - 1 / r) / fs < 0.005, "fitRec 的 scale ≈ 1/r（差 0.17%，来自内图宽度取整）");

    // 顺手验证返回的确实是画布（左边缘应是灰边 128，中心应是原图内容 0）
    int left_px = rz.resized_image.at<cv::Vec3b>(192, 5)[0];
    check(left_px == 128, "画布左边缘是灰边(128)，证明返回的是画布而不是内图");

    // ---- 2) 自洽定标：埋点 → 正向 → 反向 → 比对 ----
    struct Truth { double x, y, w, h; const char *note; };
    Truth truths[] = {
        {192, 320, 120, 120, "画面正中"},
        { 60, 100,  80,  80, "左上区域"},
        {300, 600, 100, 100, "右下区域"},
        { 20,  20,  60,  60, "贴左上角"},
        {350, 600,  60,  70, "贴右下角"},
    };

    std::vector<cv::Point2d> centers;
    std::vector<cv::Rect2d>  boxes;
    std::vector<cv::Point2d> canvas_centers;

    printf("---- 2) 正向：真值(ori_mat) → 画布坐标（这就是模型会输出的坐标） ----\n");
    printf("%-9s %-18s %-18s %-18s\n", "位置", "真值中心", "真值框(左上+宽高)", "画布中心");
    for (const auto &t : truths) {
        double ltx = t.x - t.w / 2., lty = t.y - t.h / 2.;      // 中心 → 左上角
        double clx = ltx * r + rz.dx;                            // 左上角正向映射
        double cly = lty * r + rz.dy;
        double cw  = t.w * r, ch = t.h * r;                      // 宽高正向映射
        double ccx = clx + cw / 2., ccy = cly + ch / 2.;         // 画布上的中心点

        canvas_centers.push_back(cv::Point2d(ccx, ccy));
        centers.push_back(cv::Point2d(ccx, ccy));
        boxes.push_back(cv::Rect2d(clx, cly, cw, ch));

        printf("%-9s (%6.1f,%6.1f)   (%6.1f,%6.1f,%5.1f,%5.1f)  (%7.1f,%7.1f)\n",
               t.note, t.x, t.y, ltx, lty, t.w, t.h, ccx, ccy);
    }

    printf("\n---- 3) 路线A：原代码（从不调用 fitRec，直接把画布坐标当 ori_mat 坐标） ----\n");
    printf("%-9s %-18s %-20s %-20s\n", "位置", "真值中心", "画布中心", "误差(px)");
    double maxA = 0.;
    for (size_t i = 0; i < sizeof(truths) / sizeof(truths[0]); ++i) {
        double ex = canvas_centers[i].x - truths[i].x;
        double ey = canvas_centers[i].y - truths[i].y;
        maxA = std::max(maxA, std::max(std::fabs(ex), std::fabs(ey)));
        printf("%-9s (%6.1f,%6.1f)   (%7.1f,%7.1f)      (%7.1f,%7.1f)\n",
               truths[i].note, truths[i].x, truths[i].y,
               canvas_centers[i].x, canvas_centers[i].y, ex, ey);
    }
    printf("→ 最大误差 %.1f px（ori_mat 只有 384x640，这个量级等于完全瞄错）\n\n", maxA);
    check(maxA > 50., "【已确认】不调用 fitRec 时偏差远超可接受范围");

    printf("---- 4) 路线B：修正版（调用 fitRec(result, src.size(), dst_size)） ----\n");
    fitRec(boxes, centers, src.size(), net_size);   // ★与 infer() 里的调用完全一致

    printf("%-9s %-18s %-20s %-16s %-16s\n", "位置", "真值中心", "还原后中心", "误差(px)", "还原后框宽高");
    double maxB = 0.;
    for (size_t i = 0; i < sizeof(truths) / sizeof(truths[0]); ++i) {
        double ex = centers[i].x - truths[i].x;
        double ey = centers[i].y - truths[i].y;
        double err = std::max(std::fabs(ex), std::fabs(ey));
        maxB = std::max(maxB, err);
        printf("%-9s (%6.1f,%6.1f)   (%8.2f,%8.2f)   (%5.3f,%5.3f)   %.1f x %.1f\n",
               truths[i].note, truths[i].x, truths[i].y,
               centers[i].x, centers[i].y, ex, ey, boxes[i].width, boxes[i].height);
    }
    printf("→ 最大误差 %.3f px（残差来自灰边取整 dx=205 与内图宽度取整）\n\n", maxB);
    check(maxB < 2.0, "【已确认】调用 fitRec 后误差 < 2 像素（残差来自画布/内图取整）");

    // ---- 5) 框尺寸还原对单目测距的影响 ----
    printf("---- 5) 框尺寸还原对单目测距的影响（Z = f*W/w） ----\n");
    const double fx = 1706.0, W = 0.15;
    double true_w_px = 120.;
    double net_w_px  = true_w_px * r;
    double z_true  = fx * W / true_w_px;
    double z_wrong = fx * W / net_w_px;
    printf("真值    : ori_mat 框宽 %.0f px → 距离 %.3f m\n", true_w_px, z_true);
    printf("不还原  : 画布框宽 %.0f px → 距离 %.3f m（偏大 %.2f 倍，%.2f m 报成 %.2f m）\n",
           net_w_px, z_wrong, z_wrong / z_true, z_true, z_wrong);
    printf("还原后  : 框宽回到 %.0f px → 距离 %.3f m ✓\n\n", net_w_px / r, fx * W / (net_w_px / r));
    check(std::fabs(z_wrong / z_true - 1 / r) < 1e-6, "【已确认】不还原时测距偏大 1.667 倍");
    check(std::fabs(fx * W / (net_w_px / r) - z_true) < 1e-9, "框宽还原后测距回到正确值");

    // ---- 6) 裁剪 / 退化框过滤 ----
    printf("---- 6) 框裁剪与退化框过滤（修正版 infer() 第 ⑨ 步） ----\n");
    struct Case { double x, y, w, h; const char *note; };
    Case cases[] = {
        {-30, 100, 100, 100, "左边超出画面"},
        {350, 600,  60,  70, "右下超出画面"},
        {192, 320, 0.5, 0.5, "退化框(<2px)"},
        {100, 100, 120, 120, "完全在画面内"},
    };
    for (const auto &c : cases) {
        double x1 = std::max(0., c.x), y1 = std::max(0., c.y);
        double x2 = std::min((double)src.cols, c.x + c.w);
        double y2 = std::min((double)src.rows, c.y + c.h);
        bool drop = (x2 - x1 < 2. || y2 - y1 < 2.);
        printf("%-14s 原始(%6.1f,%6.1f,%5.1f,%5.1f) → %s", c.note, c.x, c.y, c.w, c.h,
               drop ? "丢弃（退化）" : "保留");
        if (!drop) printf("  裁剪后 %.1f x %.1f 中心(%.1f,%.1f)", x2 - x1, y2 - y1, (x1 + x2) / 2, (y1 + y2) / 2);
        printf("\n");
    }
    {
        double x1 = std::max(0., -30.), x2 = std::min((double)src.cols, -30. + 100.);
        check(std::fabs((x2 - x1) - 70.) < 1e-6, "左超界框裁到边界（宽 70）");
        double f1 = std::max(0., 192.), f2 = std::min((double)src.cols, 192. + 0.5);
        check((f2 - f1) < 2., "退化框被丢弃（不污染测距）");
    }

    printf("\n========= 校验结束: %s, 失败 %d 项 =========\n",
           g_fail == 0 ? "全部通过" : "存在失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
