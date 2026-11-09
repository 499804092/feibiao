// ============================================================================
// diag_color.cpp —— 颜色识别诊断工具：把筛选过程每一步都打印出来
// ----------------------------------------------------------------------------
// 【为什么要它】
//   "识别不到目标"可能是很多原因：HSV 阈值不对 / 长宽比筛掉了 / 圆度筛掉了 /
//   面积筛掉了。光看代码猜不出来，必须把每一层的数据打出来看。
//
// 【它做什么】
//   1) 抓一帧
//   2) 统计全图里"高饱和"像素的 H 直方图 → 看绿色实际落在哪个 H 区间
//   3) 按当前阈值生成掩膜，统计白色像素数
//   4) 【关键】列出所有轮廓，并逐个说明被哪一条筛选规则刷掉
//   5) 把原图和掩膜存成 png，方便肉眼看
//
// 【编译】
//   cd ~/feibiao
//   g++ -std=c++17 -O2 diag_color.cpp Vision_Code/src/detect/src/color/ColorDetector.cpp \
//       -I Vision_Code/src/detect/inc -o diag_color $(pkg-config --cflags --libs opencv4)
//
// 【用法】
//   ./diag_color                          # 默认 800x600@60
//   ./diag_color --width 800 --height 600 --fps 60
// ============================================================================

#include <opencv2/opencv.hpp>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>

#include "color/ColorDetector.hpp"

int main(int argc, char **argv) {
    int dev = 0, W = 800, H = 600, FPS = 60, orient = 2;
    std::string cfg = "Vision_Code/config/json/detect.json";
    int ov_hmin = -1, ov_hmax = -1, ov_smin = -1, ov_vmin = -1;   // 命令行临时覆盖阈值

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--device") && i + 1 < argc) dev = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--width") && i + 1 < argc) W = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--height") && i + 1 < argc) H = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc) FPS = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--orientation") && i + 1 < argc) orient = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--config") && i + 1 < argc) cfg = argv[++i];
        else if (!strcmp(argv[i], "--h_min") && i + 1 < argc) ov_hmin = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--h_max") && i + 1 < argc) ov_hmax = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--s_min") && i + 1 < argc) ov_smin = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--v_min") && i + 1 < argc) ov_vmin = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-outline")) ;   // 保留参数位
    }

    // ---- 读参数 ----
    ColorDetector::Params p;
    {
        cv::FileStorage fs(cfg, cv::FileStorage::READ);
        if (fs.isOpened()) p = ColorDetector::Params::fromJson(fs["color"]);
    }
    if (ov_hmin >= 0) p.h_min = ov_hmin;    // 命令行覆盖优先
    if (ov_hmax >= 0) p.h_max = ov_hmax;
    if (ov_smin >= 0) p.s_min = ov_smin;
    if (ov_vmin >= 0) p.v_min = ov_vmin;
    printf("============ 颜色识别诊断 ============\n");
    p.print();
    printf("画面朝向 = %d\n\n", orient);

    // ---- 开相机 ----
    cv::VideoCapture cap(dev, cv::CAP_V4L2);
    if (!cap.isOpened()) { printf("打不开相机\n"); return 1; }
    cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
    cap.set(cv::CAP_PROP_FRAME_WIDTH, W);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, H);
    cap.set(cv::CAP_PROP_FPS, FPS);
    cap.set(cv::CAP_PROP_BUFFERSIZE, 1);

    cv::Mat raw;
    for (int i = 0; i < 10; ++i) cap.read(raw);      // 丢掉开头几帧（曝光/白平衡还没稳）
    if (!cap.read(raw) || raw.empty()) { printf("读不到帧\n"); return 1; }
    cap.release();

    // 朝向修正
    cv::Mat frame;
    switch (orient) {
        case 1: cv::rotate(raw, frame, cv::ROTATE_90_CLOCKWISE);        break;
        case 2: cv::rotate(raw, frame, cv::ROTATE_180);                 break;
        case 3: cv::rotate(raw, frame, cv::ROTATE_90_COUNTERCLOCKWISE); break;
        case 4: cv::flip(raw, frame, 1);                                break;
        case 5: cv::flip(raw, frame, 0);                                break;
        default: frame = raw.clone();                                   break;
    }

    printf("========== 1) 图像 ==========\n");
    printf("尺寸 %dx%d，已存 frame.png\n", frame.cols, frame.rows);
    cv::imwrite("frame.png", frame);

    // ---- 2) 全图高饱和像素的 H 直方图 ----
    printf("\n========== 2) 全图 HSV 统计 ==========\n");
    cv::Mat hsv;
    cv::cvtColor(frame, hsv, cv::COLOR_BGR2HSV);

    int hist[18] = {0};          // H 0~179，每 10 度一档
    long sat_px = 0;             // S>=60 且 V>=60 的像素数
    long green_like = 0;         // H 在 30~100 之间的像素数（广义"绿"）
    for (int y = 0; y < hsv.rows; ++y) {
        const cv::Vec3b *row = hsv.ptr<cv::Vec3b>(y);
        for (int x = 0; x < hsv.cols; ++x) {
            int H_ = row[x][0], S_ = row[x][1], V_ = row[x][2];
            if (S_ >= p.s_min && V_ >= p.v_min) {
                ++sat_px;
                hist[H_ / 10]++;
                if (H_ >= 30 && H_ <= 100) ++green_like;
            }
        }
    }
    printf("满足 S>=%d 且 V>=%d 的像素: %ld 个（占画面 %.3f%%）\n",
           p.s_min, p.v_min, sat_px, 100.0 * sat_px / (frame.cols * frame.rows));
    printf("其中 H 在 30~100（广义绿色）的: %ld 个\n", green_like);
    printf("H 直方图（每 10 度一档，只统计高饱和像素）:\n");
    for (int i = 0; i < 18; ++i) {
        if (hist[i] == 0) continue;
        printf("   H[%3d-%3d] : %8d  %s\n", i * 10, i * 10 + 9, hist[i],
               (i * 10 >= p.h_min && i * 10 <= p.h_max) ? "  <- 落在当前 H 阈值内" : "");
    }

    // ---- 3) 当前阈值下的掩膜 ----
    printf("\n========== 3) 当前阈值下的掩膜 ==========\n");
    ColorDetector detector(p);
    cv::Mat mask = detector.makeMask(frame);
    int white = cv::countNonZero(mask);
    printf("H[%d,%d] S[%d,%d] V[%d,%d] → 白色像素 %d 个（占画面 %.3f%%）\n",
           p.h_min, p.h_max, p.s_min, p.s_max, p.v_min, p.v_max,
           white, 100.0 * white / (frame.cols * frame.rows));
    cv::imwrite("mask.png", mask);
    printf("掩膜已存 mask.png（白色=通过颜色筛选的区域）\n");

    // ---- 4) 所有轮廓 + 逐条筛选说明 ----
    printf("\n========== 4) 所有轮廓的筛选过程 ==========\n");
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    printf("找到 %zu 个外轮廓\n", contours.size());
    if (contours.empty()) {
        printf("  ⚠️ 掩膜里一个轮廓都没有 → 颜色阈值不对，绿色区域没被分出来\n");
    }

    int pass_color_but_fail_shape = 0;
    for (size_t i = 0; i < contours.size(); ++i) {
        const auto &c = contours[i];
        cv::Rect box = cv::boundingRect(c);
        double area = cv::contourArea(c);
        double per = cv::arcLength(c, true);
        double w = box.width, h = box.height;
        double aspect = (w > h) ? (w / h) : (h / w);
        double circ = (per > 1e-6) ? (4.0 * CV_PI * area / (per * per)) : 0.0;

        printf("\n  轮廓 #%zu: 外接框 %dx%d @(%d,%d)  面积=%.1f  周长=%.1f\n",
               i + 1, box.width, box.height, box.x, box.y, area, per);
        printf("     长宽比 = %.3f  （上限 %.2f）→ %s\n", aspect, p.aspect_max,
               aspect <= p.aspect_max ? "✓ 通过" : "✗ 被长宽比筛掉");
        printf("     圆度   = %.3f  （下限 %.2f）→ %s\n", circ, p.circularity_min,
               (p.circularity_min <= 0.0 || circ >= p.circularity_min) ? "✓ 通过" : "✗ 被圆度筛掉");
        printf("     面积   = %.1f  （下限 %.0f）→ %s\n", area, p.min_area,
               area >= p.min_area ? "✓ 通过" : "✗ 被面积筛掉");

        bool ok = (aspect <= p.aspect_max) &&
                  (p.circularity_min <= 0.0 || circ >= p.circularity_min) &&
                  (area >= p.min_area);
        // 颜色过了、但形状/面积没过 → 说明阈值没问题，是筛选太严
        if (!ok) ++pass_color_but_fail_shape;
        printf("     → %s\n", ok ? "✅ 最终通过，会作为目标输出" : "❌ 被筛掉");
    }

    // ---- 5) 结论 ----
    printf("\n========== 5) 结论 ==========\n");
    if (white == 0) {
        printf("  颜色阈值不对：掩膜全黑，说明画面里没有落在 H[%d,%d] S>=%d V>=%d 的像素。\n",
               p.h_min, p.h_max, p.s_min, p.v_min);
        printf("  → 看上面第 2 节的 H 直方图，找到绿色实际落在哪个区间，然后把 detect.json 的 h_min/h_max 改成那个区间。\n");
        printf("  → 也可能是 s_min/v_min 太高把绿色滤掉了，试着调低。\n");
    } else if (contours.empty()) {
        printf("  掩膜有白色像素但没有轮廓 → 白色区域太碎（都是孤立小点），把 morph_size 调大试试。\n");
    } else if (pass_color_but_fail_shape == (int)contours.size()) {
        printf("  颜色分出来了，但【所有轮廓都被形状/面积筛掉】→ 说明是筛选条件太严，不是阈值问题。\n");
        printf("  从上面的输出看是哪一条卡住：\n");
        printf("    · 被长宽比筛 → 引导灯不是方的，把 aspect_max 调大（比如 2.0 甚至 10）\n");
        printf("    · 被圆度筛   → 引导灯不是圆的/太小，把 circularity_min 调低或设 0\n");
        printf("    · 被面积筛   → 目标太小，把 min_area 调小\n");
    } else {
        printf("  有轮廓通过了筛选，识别应该能出框。如果实际没出框，检查是不是朝向/坐标问题。\n");
    }
    printf("\n原图 frame.png / 掩膜 mask.png 已保存，可以直接打开看。\n");
    return 0;
}
