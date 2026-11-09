// ============================================================================
// imshow_bench.cpp —— 只量【显示】要花多少毫秒，不碰相机
// ----------------------------------------------------------------------------
// 【为什么要它】
//   calib_capture_usb 的循环里，除了取帧和找棋盘格，还有 cv::imshow + cv::waitKey(1)。
//   找棋盘格实测只要 6ms，取帧是相机的事，那么剩下的嫌疑就只有显示这条路。
//   在 VMware + GNOME/Wayland（OpenCV 走 XWayland）上，imshow 有可能慢得离谱。
//   这个程序把 imshow / waitKey 单独拿出来量，直接给出每帧毫秒数。
//
// 【编译】
//   g++ -std=c++17 -O2 imshow_bench.cpp -o imshow_bench $(pkg-config --cflags --libs opencv4)
// 【用法】（要有图形界面）
//   DISPLAY=:0 ./imshow_bench --iters 60
//   DISPLAY=:0 ./imshow_bench --iters 60 --width 320 --height 240   # 小窗口对比
// ============================================================================

#include <opencv2/opencv.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char ** argv) {
    int iters = 60, W = 800, H = 600;
    std::string win = "imshow_bench";
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--iters") && i + 1 < argc)  iters = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--width") && i + 1 < argc)  W = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--height") && i + 1 < argc) H = atoi(argv[++i]);
    }

    printf("============ 显示耗时基准 ============\n");
    printf("DISPLAY=%s   窗口 %dx%d   迭代 %d\n\n",
           getenv("DISPLAY") ? getenv("DISPLAY") : "(空)", W, H, iters);
    if (!getenv("DISPLAY") || !getenv("DISPLAY")[0]) {
        printf("没有 DISPLAY，测不了\n");
        return 1;
    }

    // 造一张和相机帧同样大小的图（有内容，不然压缩/优化会失真）
    cv::Mat img(H, W, CV_8UC3);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            img.at<cv::Vec3b>(y, x) = cv::Vec3b((uchar)(x % 256), (uchar)(y % 256),
                                                (uchar)((x + y) % 256));

    const double freq = cv::getTickFrequency();

    // 先开窗、画一次，避免把首次建窗的开销算进循环
    cv::namedWindow(win, cv::WINDOW_AUTOSIZE);
    cv::imshow(win, img);
    cv::waitKey(50);

    // ---- 阶段A：imshow + waitKey(1)，就是 calib_capture_usb 每帧干的事 ----
    double t0 = (double)cv::getTickCount();
    for (int i = 0; i < iters; ++i) {
        cv::imshow(win, img);
        cv::waitKey(1);
    }
    double msA = ((double)cv::getTickCount() - t0) / freq * 1000.0 / iters;

    // ---- 阶段B：只 waitKey(1)，看它自己占多少 ----
    t0 = (double)cv::getTickCount();
    for (int i = 0; i < iters; ++i) cv::waitKey(1);
    double msB = ((double)cv::getTickCount() - t0) / freq * 1000.0 / iters;

    // ---- 阶段C：只 imshow，不 waitKey（不一定会真的绘到屏幕上）----
    t0 = (double)cv::getTickCount();
    for (int i = 0; i < iters; ++i) cv::imshow(win, img);
    double msC = ((double)cv::getTickCount() - t0) / freq * 1000.0 / iters;

    // ---- 阶段D：waitKey(1) 之后有没有把事件排空 ----
    t0 = (double)cv::getTickCount();
    for (int i = 0; i < iters; ++i) { cv::imshow(win, img); cv::waitKey(0 + 1); }
    double msD = ((double)cv::getTickCount() - t0) / freq * 1000.0 / iters;

    printf("  阶段A  imshow + waitKey(1)     : %7.2f ms/帧  → %6.1f fps   ← 程序里每帧都在做\n", msA, 1000.0 / msA);
    printf("  阶段B  只 waitKey(1)           : %7.2f ms/次\n", msB);
    printf("  阶段C  只 imshow（不刷屏）     : %7.2f ms/次\n", msC);
    printf("  阶段D  imshow + waitKey(1) 重跑: %7.2f ms/帧  → %6.1f fps\n", msD, 1000.0 / msD);

    cv::destroyWindow(win);
    cv::waitKey(20);

    printf("\n判读：\n");
    if (msA > 100) {
        printf("  ★★★ 光是显示就 %.0f ms/帧 → 上限定死在 %.1f fps。\n", msA, 1000.0 / msA);
        printf("      这就是 calib_capture_usb 只有 2fps 的原因：不是相机，是 imshow 走 XWayland 太慢。\n");
        printf("      对策：① 去掉本地窗口，加 --headless（板子上就是这么干的）\n");
        printf("            ② 或者切到 Ubuntu on Xorg 会话登录（不用 Wayland）\n");
        printf("            ③ 或者开网页推流看画面\n");
    } else if (msA > 30) {
        printf("  显示要 %.0f ms/帧（约 %.0f fps），偏慢但不是 2fps 那种程度。\n", msA, 1000.0 / msA);
    } else {
        printf("  显示很快（%.1f ms/帧），不是瓶颈。\n", msA);
    }
    return 0;
}
