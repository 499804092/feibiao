// ============================================================================
// fps_isolate.cpp —— 把「相机出流慢」和「算法慢」分开量
// ----------------------------------------------------------------------------
// 【为什么要单独量】
//   calib_capture_usb 每帧要做 找棋盘格(8x11) 这件事，这一步可能很慢。
//   如果只看界面上的 fps，分不清到底是相机只给 2fps，还是相机给 60fps
//   但算法每帧花 500ms。这两件事的修法完全相反，所以必须先拆开。
//
// 【它怎么拆】
//   分四个阶段量同一台相机，每阶段跑 N 秒，看 fps 掉在哪一步：
//     阶段0  只 read()                    → 相机本身的出流上限
//     阶段1  + 灰度 + findChessboardCorners（原版 spr 的 flags）
//     阶段2  + FAST_CHECK（calib_capture_usb 用的 flags）
//     阶段3  + findChessboardCornersSB（--sb 1 用的）
//
// 【安全】只读帧，不写任何 V4L2 控制寄存器（不碰曝光/增益/亮度）。
//   有出流看门狗；跑完一定 release()。
//
// 【编译】
//   g++ -std=c++17 -O2 fps_isolate.cpp -o fps_isolate $(pkg-config --cflags --libs opencv4)
// 【用法】
//   ./fps_isolate --seconds 3
//   ./fps_isolate --width 640 --height 480 --fps 30
// ============================================================================

#include <opencv2/opencv.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unistd.h>
#include <vector>

static std::atomic<bool> g_got{false};                     // 有没有读到过帧

static void watchdog(int dev, int timeout_s) {
    for (int i = 0; i < timeout_s * 10; ++i) {
        if (g_got.load()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    printf("\n✗ %d 秒内一帧都没读到（相机没出流）\n", timeout_s);
    fflush(stdout);
    _exit(1);
}

// 跑 secs 秒，返回实测 fps；found 表示最后一帧有没有找到棋盘格
static double measure(
    cv::VideoCapture & cap, double secs, int phase, const cv::Size & board, bool & found) {
    long n = 0;
    double t0 = (double)cv::getTickCount();
    const double freq = cv::getTickFrequency();
    double el = 0;
    found = false;
    while (true) {
        cv::Mat f;
        if (!cap.read(f) || f.empty()) break;              // 读一帧
        g_got.store(true);
        ++n;

        if (phase >= 1) {                                  // 阶段1 起要做识别
            cv::Mat g;
            cv::cvtColor(f, g, cv::COLOR_BGR2GRAY);
            std::vector<cv::Point2f> c;
            bool ok = false;
            if (phase == 1) {                              // 原版 spr 的 flags（无 FAST_CHECK）
                ok = cv::findChessboardCorners(
                    g, board, c,
                    cv::CALIB_CB_ADAPTIVE_THRESH + cv::CALIB_CB_NORMALIZE_IMAGE);
            } else if (phase == 2) {                       // calib_capture_usb 用的 flags
                ok = cv::findChessboardCorners(
                    g, board, c,
                    cv::CALIB_CB_ADAPTIVE_THRESH + cv::CALIB_CB_NORMALIZE_IMAGE +
                        cv::CALIB_CB_FAST_CHECK);
            } else if (phase == 3) {                       // --sb 1 用的
                ok = cv::findChessboardCornersSB(g, board, c);
            }
            found = ok;
        }
        el = ((double)cv::getTickCount() - t0) / freq;
        if (el >= secs) break;
    }
    return el > 0 ? n / el : 0.0;
}

int main(int argc, char ** argv) {
    int dev = 0, W = 800, H = 600, FPS = 60, secs = 3;
    int cols = 8, rows = 11;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--device") && i + 1 < argc)       dev = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--width") && i + 1 < argc)   W = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--height") && i + 1 < argc)  H = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc)     FPS = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) secs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cols") && i + 1 < argc)    cols = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rows") && i + 1 < argc)    rows = atoi(argv[++i]);
    }

    printf("============ 相机出流 / 算法耗时 分离测量 ============\n");
    printf("每个阶段跑 %d 秒\n\n", secs);

    cv::VideoCapture cap(dev, cv::CAP_V4L2);               // 和 calib_capture_usb 一样的开法
    if (!cap.isOpened()) {
        printf("打不开 /dev/video%d\n", dev);
        return 1;
    }
    cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
    cap.set(cv::CAP_PROP_FRAME_WIDTH, W);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, H);
    cap.set(cv::CAP_PROP_FPS, FPS);
    cap.set(cv::CAP_PROP_BUFFERSIZE, 1);

    int fourcc = (int)cap.get(cv::CAP_PROP_FOURCC);
    char fcc[5] = {char(fourcc & 0xFF), char((fourcc >> 8) & 0xFF),
                   char((fourcc >> 16) & 0xFF), char((fourcc >> 24) & 0xFF), 0};
    printf("驱动报告：%dx%d @%.0f  格式=%s\n",
           (int)cap.get(cv::CAP_PROP_FRAME_WIDTH),
           (int)cap.get(cv::CAP_PROP_FRAME_HEIGHT),
           cap.get(cv::CAP_PROP_FPS), fcc);

    std::thread(watchdog, dev, 10).detach();

    const cv::Size board(cols, rows);
    bool found = false;
    double f0 = measure(cap, secs, 0, board, found);
    double f1 = measure(cap, secs, 1, board, found);
    bool f1_found = found;
    double f2 = measure(cap, secs, 2, board, found);
    bool f2_found = found;
    double f3 = measure(cap, secs, 3, board, found);
    bool f3_found = found;

    cap.release();

    printf("\n结果：\n");
    printf("  阶段0  只取帧，不做任何处理      : %6.1f fps\n", f0);
    printf("  阶段1  + 找棋盘格(原版flags)     : %6.1f fps   每帧多花 %6.1f ms   板子检出=%d\n",
           f1, f1 > 0 ? (1000.0 / f1 - 1000.0 / f0) : 0, (int)f1_found);
    printf("  阶段2  + 找棋盘格(FAST_CHECK)    : %6.1f fps   每帧多花 %6.1f ms   板子检出=%d\n",
           f2, f2 > 0 ? (1000.0 / f2 - 1000.0 / f0) : 0, (int)f2_found);
    printf("  阶段3  + 找棋盘格(SB 检测器)     : %6.1f fps   每帧多花 %6.1f ms   板子检出=%d\n",
           f3, f3 > 0 ? (1000.0 / f3 - 1000.0 / f0) : 0, (int)f3_found);

    printf("\n判读：\n");
    if (f0 < 15) {
        printf("  ★ 阶段0 就已经很慢 → 问题在【相机/驱动/USB】，不是算法。\n");
    } else if (f2 < f0 * 0.6) {
        printf("  ★ 相机本身够快(%.1ffps)，是【找棋盘格】把它拖到了 %.1ffps。\n", f0, f2);
        printf("    对策：用 --sb 1（SB 检测器）看阶段3 快不快；\n");
        printf("          或者采集时不要每帧都识别（隔几帧识别一次）。\n");
    } else {
        printf("  ★ 识别开销不大，相机和算法都正常。\n");
    }
    return 0;
}
