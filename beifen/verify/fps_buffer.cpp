// ============================================================================
// fps_buffer.cpp —— 测「V4L2 缓冲个数」对 OpenCV 取帧速度的影响
// ----------------------------------------------------------------------------
// 【背景】
//   同一台相机、同一个 1280x720 MJPG：
//     v4l2-ctl --stream-mmap=4   → 167 fps
//     OpenCV  cap.read()         →  88 fps（就算关掉解码也只有 88）
//   两者差的只有「缓冲个数」。工程里 UvcCamera.cpp 为了降延迟写了
//     cap.set(cv::CAP_PROP_BUFFERSIZE, 1);
//   只有一个缓冲时，驱动必须等程序把缓冲还回去才能再抓下一帧，
//   中间的空档会让帧被丢掉 —— 这很可能就是把 167 砍成 88 的原因。
//
// 【它干什么】
//   把缓冲个数从 1 试到 16，每个都跑 N 秒，直接看 fps 怎么变。
//   全程用 CAP_PROP_CONVERT_RGB=0（不解码），排除解码干扰。
//
// 【安全】只读帧；每轮独立开/关设备；有出流看门狗。
//
// 【编译】
//   g++ -std=c++17 -O2 fps_buffer.cpp -o fps_buffer $(pkg-config --cflags --libs opencv4)
// 【用法】
//   ./fps_buffer --width 1280 --height 720 --fps 250 --seconds 3
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

static std::atomic<bool> g_got{false};

static void watchdog(int dev, int timeout_s) {
    for (int i = 0; i < timeout_s * 10; ++i) {
        if (g_got.load()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    printf("\n✗ %d 秒没出流\n", timeout_s);
    fflush(stdout);
    _exit(1);
}

// buffers<0 表示「不设置 BUFFERSIZE」，用驱动默认值
static double run(int dev, int W, int H, int FPS, int secs, int buffers, bool decode) {
    cv::VideoCapture cap(dev, cv::CAP_V4L2);
    if (!cap.isOpened()) return -1;

    cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
    cap.set(cv::CAP_PROP_FRAME_WIDTH, W);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, H);
    cap.set(cv::CAP_PROP_FPS, FPS);
    if (buffers > 0) cap.set(cv::CAP_PROP_BUFFERSIZE, buffers);
    if (!decode) cap.set(cv::CAP_PROP_CONVERT_RGB, 0);

    long n = 0;
    double t0 = (double)cv::getTickCount();
    double el = 0;
    while (true) {
        cv::Mat f;
        if (!cap.read(f) || f.empty()) break;
        g_got.store(true);
        ++n;
        el = ((double)cv::getTickCount() - t0) / cv::getTickFrequency();
        if (el >= secs) break;
    }
    cap.release();
    return el > 0 ? n / el : -1;
}

int main(int argc, char ** argv) {
    int dev = 0, W = 1280, H = 720, FPS = 250, secs = 3;
    bool decode = false;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--device") && i + 1 < argc) dev = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--width") && i + 1 < argc) W = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--height") && i + 1 < argc) H = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc) FPS = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) secs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--decode")) decode = true;
    }

    printf("============ V4L2 缓冲个数 vs OpenCV 取帧速度 ============\n");
    printf("分辨率 %dx%d  MJPG  请求 %d fps  每轮 %d 秒  %s\n\n",
           W, H, FPS, secs, decode ? "含解码" : "不解码(CONVERT_RGB=0)");

    std::thread(watchdog, dev, 15).detach();

    const int counts[] = {-1, 1, 2, 3, 4, 6, 8, 16};
    printf("%-14s %s\n", "BUFFERSIZE", "实测 fps");
    printf("----------------------------------------\n");
    for (int c : counts) {
        double f = run(dev, W, H, FPS, secs, c, decode);
        char label[32];
        if (c < 0) snprintf(label, sizeof(label), "不设置");
        else       snprintf(label, sizeof(label), "%d", c);
        printf("%-14s %.1f fps\n", label, f);
        fflush(stdout);
    }

    printf("\n判读：\n");
    printf("  · 如果 BUFFERSIZE=1 明显最慢、调大就快 → 就是缓冲个数卡的，\n");
    printf("    工程里那句 cap.set(CAP_PROP_BUFFERSIZE, 1) 要放宽。\n");
    printf("  · 如果全都上不去 → 瓶颈在别处。\n");
    return 0;
}
