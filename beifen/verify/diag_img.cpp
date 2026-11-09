// ============================================================================
// diag_img.cpp —— 对【静态图片】跑真实的 ColorDetector，把每层筛选都打出来
//   用法:
//     ./diag_img --img yindaodeng.jpg --out /tmp/dg
//     ./diag_img --img yindaodeng.jpg --sweep
// ============================================================================
#include <opencv2/opencv.hpp>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
#include "color/ColorDetector.hpp"

static const char *judge(double aspect, double area, double extent, double circ,
                         const ColorDetector::Params &p) {
    if (aspect > p.aspect_max)                     return "REJ 长宽比>";
    if (area < p.min_area)                         return "REJ 面积小";
    if (p.extent_min > 0.0 && extent < p.extent_min)  return "REJ 填充率低";
    if (p.circularity_min > 0.0 && circ < p.circularity_min) return "REJ 圆度低";
    return "OK  通过";
}

static void dump(const cv::Mat &mask, const ColorDetector::Params &p, const char *tag) {
    std::vector<std::vector<cv::Point> > cs;
    cv::findContours(mask.clone(), cs, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    printf("---- [%s] 轮廓数 = %zu ----\n", tag, cs.size());
    std::vector<int> idx(cs.size());
    for (size_t i = 0; i < cs.size(); ++i) idx[i] = (int)i;
    std::sort(idx.begin(), idx.end(),
              [&](int a, int b) { return cv::contourArea(cs[a]) > cv::contourArea(cs[b]); });
    for (size_t k = 0; k < idx.size() && k < 10; ++k) {
        int i = idx[k];
        cv::Rect b = cv::boundingRect(cs[i]);
        double w = b.width, h = b.height;
        double area = cv::contourArea(cs[i]);
        double aspect = (w > h) ? w / std::max(1.0, h) : h / std::max(1.0, w);
        double extent = area / std::max(1.0, w * h);
        double per = cv::arcLength(cs[i], true);
        double circ = (per > 1e-6) ? 4.0 * CV_PI * area / (per * per) : 0.0;
        printf("  #%zu box=(%4d,%4d,%4d,%4d) area=%9.1f aspect=%.2f extent=%.3f circ=%.3f  %s\n",
               k, b.x, b.y, b.width, b.height, area, aspect, extent, circ,
               judge(aspect, area, extent, circ, p));
    }
}

static void scanline(const cv::Mat &bgr, const cv::Mat &rawmask) {
    std::vector<std::vector<cv::Point> > cs;
    cv::findContours(rawmask.clone(), cs, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    if (cs.empty()) { printf("(原掩膜没有轮廓，无法取扫描线)\n"); return; }
    size_t best = 0; double ba = -1;
    for (size_t i = 0; i < cs.size(); ++i) {
        double a = cv::contourArea(cs[i]);
        if (a > ba) { ba = a; best = i; }
    }
    cv::Moments m = cv::moments(cs[best]);
    int cx = (m.m00 > 1e-6) ? (int)(m.m10 / m.m00) : 0;
    int cy = (m.m01 > 1e-6) ? (int)(m.m01 / m.m00) : 0;
    printf("---- 过灯芯水平扫描线 y=%d, x=%d..%d ----\n", cy, cx - 120, cx + 120);
    cv::Mat hsv; cv::cvtColor(bgr, hsv, cv::COLOR_BGR2HSV);
    printf("   x    H   S   V   B   G   R\n");
    for (int x = cx - 120; x <= cx + 120; x += 4) {
        if (x < 0 || x >= bgr.cols || cy < 0 || cy >= bgr.rows) continue;
        cv::Vec3b hsvp = hsv.at<cv::Vec3b>(cy, x);
        cv::Vec3b bgrp = bgr.at<cv::Vec3b>(cy, x);
        printf(" %4d %4d %4d %4d %4d %4d %4d\n", x, hsvp[0], hsvp[1], hsvp[2],
               bgrp[0], bgrp[1], bgrp[2]);
    }
}

int main(int argc, char **argv) {
    std::string img, out = "/tmp/dg", cfg = "config/tuning.yaml";
    bool doSweep = false, doScan = false;
    int ohmin = -1, ohmax = -1, osmin = -1, ovmin = -1;
    double oaspect = -1, oextent = -1;

    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i], "--img")    && i + 1 < argc) img  = argv[++i];
        else if (!strcmp(argv[i], "--out")    && i + 1 < argc) out  = argv[++i];
        else if (!strcmp(argv[i], "--config") && i + 1 < argc) cfg  = argv[++i];
        else if (!strcmp(argv[i], "--h_min")  && i + 1 < argc) ohmin = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--h_max")  && i + 1 < argc) ohmax = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--s_min")  && i + 1 < argc) osmin = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--v_min")  && i + 1 < argc) ovmin = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--aspect_max") && i + 1 < argc) oaspect = atof(argv[++i]);
        else if (!strcmp(argv[i], "--extent_min") && i + 1 < argc) oextent = atof(argv[++i]);
        else if (!strcmp(argv[i], "--sweep")) doSweep = true;
        else if (!strcmp(argv[i], "--scan"))  doScan  = true;
    }
    if (img.empty()) { printf("用法: ./diag_img --img <图片> [--out 前缀] [--sweep] [--scan]\n"); return 1; }

    cv::Mat bgr = cv::imread(img, cv::IMREAD_COLOR);
    if (bgr.empty()) { printf("读不到图片: %s\n", img.c_str()); return 1; }
    printf("图片 %s  宽=%d 高=%d\n", img.c_str(), bgr.cols, bgr.rows);

    ColorDetector::Params p;
    {
        cv::FileStorage fs(cfg, cv::FileStorage::READ);
        if (fs.isOpened()) { p = ColorDetector::Params::fromJson(fs["color"]); printf("配置来自 %s\n", cfg.c_str()); }
        else               { printf("!! 打不开 %s，用代码里的默认值\n", cfg.c_str()); }
    }
    if (ohmin >= 0) p.h_min = ohmin;
    if (ohmax >= 0) p.h_max = ohmax;
    if (osmin >= 0) p.s_min = osmin;
    if (ovmin >= 0) p.v_min = ovmin;
    if (oaspect > 0) p.aspect_max = oaspect;
    if (oextent >= 0) p.extent_min = oextent;
    p.sanitize();

    if (doSweep) {
        printf("\n================ 阈值扫描 ================\n");
        printf(" h[%d,%d]  形态学=%d  aspect<=%.2f extent>=%.2f min_area=%.0f\n",
               p.h_min, p.h_max, p.morph_size, p.aspect_max, p.extent_min, p.min_area);
        int svals[] = {15, 20, 30, 40, 60, 80, 100};
        int vvals[] = {30, 50, 60, 80, 100, 130, 160};
        for (int hi = 0; hi < 3; ++hi) {
            int hlo = 35 + hi * 5, hhi = 80;
            for (unsigned a = 0; a < sizeof(svals) / sizeof(int); ++a) {
                for (unsigned b = 0; b < sizeof(vvals) / sizeof(int); ++b) {
                    ColorDetector::Params q = p;
                    q.h_min = hlo; q.h_max = hhi;
                    q.s_min = svals[a]; q.v_min = vvals[b];
                    q.sanitize();
                    ColorDetector cd(q);
                    cv::Mat raw, hsv;
                    cv::cvtColor(bgr, hsv, cv::COLOR_BGR2HSV);
                    cv::inRange(hsv, cv::Scalar(q.h_min, q.s_min, q.v_min),
                                cv::Scalar(q.h_max, q.s_max, q.v_max), raw);
                    int white = cv::countNonZero(raw);
                    std::vector<std::vector<cv::Point> > cs;
                    cv::findContours(raw.clone(), cs, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
                    double bestA = -1; cv::Rect bestB; double bestE = 0, bestAsp = 0;
                    for (size_t i = 0; i < cs.size(); ++i) {
                        double ar = cv::contourArea(cs[i]);
                        if (ar > bestA) {
                            bestA = ar; bestB = cv::boundingRect(cs[i]);
                            double w = bestB.width, h = bestB.height;
                            bestAsp = (w > h) ? w / std::max(1.0, h) : h / std::max(1.0, w);
                            bestE = ar / std::max(1.0, w * h);
                        }
                    }
                    std::vector<ColorDetector::Light> L = cd.detect(bgr);
                    ColorDetector::Light Lf;
                    cv::Mat fm = cd.makeMask(bgr);
                    std::vector<std::vector<cv::Point> > fc;
                    cv::findContours(fm.clone(), fc, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
                    double fA = -1; cv::Rect fB;
                    for (size_t i = 0; i < fc.size(); ++i) {
                        double ar = cv::contourArea(fc[i]);
                        if (ar > fA) { fA = ar; fB = cv::boundingRect(fc[i]); }
                    }
                    printf("H[%d,%d] S>=%3d V>=%3d | 原白=%7d 轮廓=%3zu 最大(原)=%4dx%-4d asp=%.2f ext=%.2f"
                           " | 填洞后最大框=%4dx%-4d | 检出=%zu %s\n",
                           hlo, hhi, svals[a], vvals[b], white, cs.size(),
                           bestB.width, bestB.height, bestAsp, bestE,
                           fB.width, fB.height, L.size(),
                           L.empty() ? "✗" : "✓");
                }
            }
        }
        printf("==========================================\n");
        return 0;
    }

    printf("\n生效参数: "); p.print();

    cv::Mat hsv, raw;
    cv::cvtColor(bgr, hsv, cv::COLOR_BGR2HSV);
    cv::inRange(hsv, cv::Scalar(p.h_min, p.s_min, p.v_min),
                cv::Scalar(p.h_max, p.s_max, p.v_max), raw);
    printf("inRange 原始白像素 = %d\n", cv::countNonZero(raw));

    ColorDetector cd(p);
    cv::Mat fin = cd.makeMask(bgr);

    if (doScan) scanline(bgr, raw);

    printf("\n");
    dump(raw, p, "填洞之前 inRange 原掩膜");
    printf("\n");
    dump(fin, p, "填洞之后 makeMask 最终掩膜");

    std::vector<ColorDetector::Light> L = cd.detect(bgr);
    printf("\n---- detect() 结果: %zu 个 ----\n", L.size());
    for (size_t i = 0; i < L.size(); ++i) {
        printf("  #%zu box=(%.0f,%.0f,%.0f,%.0f) center=(%.1f,%.1f) area=%.1f score=%.3f\n",
               i, L[i].box.x, L[i].box.y, L[i].box.width, L[i].box.height,
               L[i].center_point.x, L[i].center_point.y, L[i].area, L[i].score);
    }

    cv::imwrite(out + "_raw.png", raw);
    cv::imwrite(out + "_final.png", fin);
    cv::Mat vis = bgr.clone();
    for (size_t i = 0; i < L.size(); ++i) {
        cv::rectangle(vis, L[i].box, cv::Scalar(0, 0, 255), 3);
        cv::circle(vis, L[i].center_point, 6, cv::Scalar(255, 0, 255), -1);
    }
    cv::imwrite(out + "_result.png", vis);
    printf("已保存: %s_raw.png  %s_final.png  %s_result.png\n", out.c_str(), out.c_str(), out.c_str());
    return 0;
}