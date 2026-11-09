// ============================================================================
// detect_bench.cpp —— 只量「找棋盘格」这一步要花多少毫秒，完全不碰相机
// ----------------------------------------------------------------------------
// 【为什么要它】
//   calib_capture_usb 界面上显示 2fps，可能是两种完全不同的原因：
//     (a) 相机只给 2fps              → 改相机/驱动/USB
//     (b) 相机给 60fps，但每帧做一次 findChessboardCorners 要 500ms → 改算法
//   这个程序在一张静态棋盘格图上把这一步跑 N 遍，直接给出每帧毫秒数，
//   于是 (b) 到底成不成立就一目了然，而且全程不打开相机、没有把相机搞挂的风险。
//
// 【编译】
//   g++ -std=c++17 -O2 detect_bench.cpp -o detect_bench $(pkg-config --cflags --libs opencv4)
//
// 【用法】
//   ./detect_bench --image _bench/1.jpg --iters 10
// ============================================================================

#include <opencv2/opencv.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// 把某个变体跑 iters 遍，返回每遍平均毫秒数
static double bench(
    const cv::Mat & gray, const cv::Size & board, int variant, int iters, bool & ok) {
    std::vector<cv::Point2f> c;
    c.reserve(board.width * board.height);

    // 先热一次，避免把首次调用的惰性初始化算进去
    c.clear();
    if (variant == 0)
        ok = cv::findChessboardCorners(
            gray, board, c, cv::CALIB_CB_ADAPTIVE_THRESH + cv::CALIB_CB_NORMALIZE_IMAGE);
    else if (variant == 1)
        ok = cv::findChessboardCorners(
            gray, board, c,
            cv::CALIB_CB_ADAPTIVE_THRESH + cv::CALIB_CB_NORMALIZE_IMAGE + cv::CALIB_CB_FAST_CHECK);
    else if (variant == 2)
        ok = cv::findChessboardCornersSB(gray, board, c);
    else if (variant == 3)
        ok = cv::findChessboardCorners(gray, board, c, cv::CALIB_CB_FAST_CHECK);

    double t0 = (double)cv::getTickCount();
    for (int i = 0; i < iters; ++i) {
        c.clear();
        if (variant == 0)
            ok = cv::findChessboardCorners(
                gray, board, c, cv::CALIB_CB_ADAPTIVE_THRESH + cv::CALIB_CB_NORMALIZE_IMAGE);
        else if (variant == 1)
            ok = cv::findChessboardCorners(
                gray, board, c,
                cv::CALIB_CB_ADAPTIVE_THRESH + cv::CALIB_CB_NORMALIZE_IMAGE +
                    cv::CALIB_CB_FAST_CHECK);
        else if (variant == 2)
            ok = cv::findChessboardCornersSB(gray, board, c);
        else if (variant == 3)
            ok = cv::findChessboardCorners(gray, board, c, cv::CALIB_CB_FAST_CHECK);
    }
    double el = ((double)cv::getTickCount() - t0) / cv::getTickFrequency();
    return el * 1000.0 / iters;
}

int main(int argc, char ** argv) {
    std::string path = "_bench/1.jpg";
    int iters = 10, cols = 8, rows = 11;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--image") && i + 1 < argc)   path = argv[++i];
        else if (!strcmp(argv[i], "--iters") && i + 1 < argc) iters = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cols") && i + 1 < argc)  cols = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rows") && i + 1 < argc)  rows = atoi(argv[++i]);
    }

    cv::Mat img = cv::imread(path, cv::IMREAD_COLOR);
    if (img.empty()) {
        printf("读不到 %s\n", path.c_str());
        return 1;
    }
    printf("============ 找棋盘格耗时基准（不碰相机）============\n");
    printf("图片 %s  %dx%d   迭代 %d 次\n\n", path.c_str(), img.cols, img.rows, iters);

    cv::Mat gray;
    cv::cvtColor(img, gray, cv::COLOR_BGR2GRAY);
    const cv::Size board(cols, rows);

    // 顺带量一下纯拷贝和灰度转换的耗时，好知道"除了识别还有多少固定开销"
    double t0 = (double)cv::getTickCount();
    for (int i = 0; i < iters; ++i) {
        cv::Mat g;
        cv::cvtColor(img, g, cv::COLOR_BGR2GRAY);
    }
    double t_gray = ((double)cv::getTickCount() - t0) / cv::getTickFrequency() * 1000.0 / iters;

    t0 = (double)cv::getTickCount();
    for (int i = 0; i < iters; ++i) {
        cv::Mat c = img.clone();
    }
    double t_clone = ((double)cv::getTickCount() - t0) / cv::getTickFrequency() * 1000.0 / iters;

    bool ok0 = false, ok1 = false, ok2 = false, ok3 = false;
    double m0 = bench(gray, board, 0, iters, ok0);
    double m1 = bench(gray, board, 1, iters, ok1);
    double m2 = bench(gray, board, 2, iters, ok2);
    double m3 = bench(gray, board, 3, iters, ok3);

    printf("  cvtColor 灰度转换                    : %6.2f ms    → %5.1f fps\n", t_gray, 1000.0 / t_gray);
    printf("  Mat::clone 拷贝一份(画图用)          : %6.2f ms    → %5.1f fps\n", t_clone, 1000.0 / t_clone);
    printf("\n");
    printf("  findChessboardCorners 原版flags      : %6.2f ms    → %5.1f fps   检出=%d\n",
           m0, 1000.0 / m0, (int)ok0);
    printf("  findChessboardCorners +FAST_CHECK    : %6.2f ms    → %5.1f fps   检出=%d\n",
           m1, 1000.0 / m1, (int)ok1);
    printf("  findChessboardCorners 只FAST_CHECK   : %6.2f ms    → %5.1f fps   检出=%d\n",
           m3, 1000.0 / m3, (int)ok3);
    printf("  findChessboardCornersSB (--sb 1)     : %6.2f ms    → %5.1f fps   检出=%d\n",
           m2, 1000.0 / m2, (int)ok2);

    printf("\n判读：\n");
    if (m1 > 100) {
        printf("  ★ 每帧光识别就要 %.0f ms，加上取帧/画图/显示，2fps 完全对得上。\n", m1);
        printf("    这就是 calib_capture_usb 慢的原因 —— 不是相机的问题。\n");
    } else {
        printf("  识别只有 %.0f ms，撑不起『2fps』这种程度的卡顿，问题多半在别处（显示/相机）。\n", m1);
    }
    return 0;
}
