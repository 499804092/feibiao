// ============================================================================
// pipeline_fps.cpp —— 用【飞镖工程真正的识别代码】量端到端帧率
// ----------------------------------------------------------------------------
// 【它干什么】
//   直接把工程里的 ColorDetector 拿过来（同一个 .cpp，不是仿写的），
//   接上真相机跑：取帧 → MJPG 解码 → HSV 识别，数一秒能跑多少帧。
//   这才是"用飞镖识别代码测相机最高帧率"的准确答案。
//
// 【为什么要这个】
//   fps_ceiling 量的是"取帧"的上限（172fps），fps_buffer 量的是缓冲个数的影响。
//   但真正决定能用多少帧的是【解码 + 识别】还要花多少时间：
//     实际帧率 ≈ 1 / (取一帧的时间 + 解码时间 + 识别时间)
//   这个程序一次性把这三块合起来量。
//
// 【安全】只读帧，不写任何相机控制项；每轮独立开关设备；有出流看门狗。
//
// 【编译】（要连工程里的 ColorDetector.cpp 一起编）
//   g++ -std=c++17 -O2 pipeline_fps.cpp \
//       /home/rsve/Desktop/feibiao/Vision_Code/src/detect/src/color/ColorDetector.cpp \
//       -I /home/rsve/Desktop/feibiao/Vision_Code/src/detect/inc \
//       -o pipeline_fps $(pkg-config --cflags --libs opencv4)
// 【用法】
//   ./pipeline_fps --seconds 3 --buffers 2
// ============================================================================

#include <opencv2/opencv.hpp>

#include "color/ColorDetector.hpp"

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

struct Out {
    double fps = -1;        // 取帧+解码+识别 的总帧率
    double read_ms = 0;     // 平均取一帧(含解码)多少毫秒
    double detect_ms = 0;   // 平均识别多少毫秒
    int det = 0;            // 最后一帧识别到几个目标
};

static Out run(int dev, int W, int H, int FPS, int buffers, int secs, ColorDetector & detector) {
    Out o;
    cv::VideoCapture cap(dev, cv::CAP_V4L2);
    if (!cap.isOpened()) return o;

    cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
    cap.set(cv::CAP_PROP_FRAME_WIDTH, W);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, H);
    cap.set(cv::CAP_PROP_FPS, FPS);
    if (buffers > 0) cap.set(cv::CAP_PROP_BUFFERSIZE, buffers);

    long n = 0;
    double tread = 0, tdet = 0;
    double t0 = (double)cv::getTickCount();
    const double freq = cv::getTickFrequency();
    double el = 0;
    while (true) {
        double tA = (double)cv::getTickCount();
        cv::Mat f;
        if (!cap.read(f) || f.empty()) break;
        double tB = (double)cv::getTickCount();
        g_got.store(true);

        auto lights = detector.detect(f);          // ★工程里真正的识别代码
        double tC = (double)cv::getTickCount();

        ++n;
        tread += (tB - tA) / freq;
        tdet  += (tC - tB) / freq;
        o.det = (int)lights.size();

        el = ((double)cv::getTickCount() - t0) / freq;
        if (el >= secs) break;
    }
    cap.release();
    if (n > 0) {
        o.fps = n / el;
        o.read_ms = tread / n * 1000.0;
        o.detect_ms = tdet / n * 1000.0;
    }
    return o;
}

int main(int argc, char ** argv) {
    int dev = 0, secs = 3, buffers = 2;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--device") && i + 1 < argc) dev = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) secs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--buffers") && i + 1 < argc) buffers = atoi(argv[++i]);
    }

    printf("============ 飞镖识别代码 端到端帧率 ============\n");
    printf("用工程真正的 ColorDetector；每档 %d 秒；BUFFERSIZE=%d\n\n", secs, buffers);

    ColorDetector::Params params;              // 用默认参数（和 tuning.yaml 里那套一致）
    ColorDetector detector(params);

    std::thread(watchdog, dev, 15).detach();

    struct Mode { int w, h, req; const char * note; };
    const Mode modes[] = {
        {1280, 1024, 180, "参数表 180fps"},
        {1280, 720,  250, "参数表 250fps"},
        {800,  600,  180, "参数表 180fps"},
        {640,  480,  350, "参数表 350fps"},
    };

    printf("%-12s %-14s | %8s | %10s | %10s | %s\n",
           "分辨率", "备注", "端到端fps", "取帧+解码", "识别", "检出");
    printf("---------------------------------------------------------------------------------\n");
    for (const auto & m : modes) {
        Out o = run(dev, m.w, m.h, m.req, buffers, secs, detector);
        char res[32];
        snprintf(res, sizeof(res), "%dx%d", m.w, m.h);
        printf("%-12s %-14s | %6.1f   | %6.2f ms  | %6.2f ms  | %d\n",
               res, m.note, o.fps, o.read_ms, o.detect_ms, o.det);
        fflush(stdout);
    }

    printf("\n判读：把「取帧+解码」和「识别」加起来取倒数，就是端到端帧率的上限。\n");
    printf("      要冲 180fps，这两项加起来必须 < 5.6ms。\n");
    return 0;
}
