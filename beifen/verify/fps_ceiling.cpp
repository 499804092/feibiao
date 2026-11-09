// ============================================================================
// fps_ceiling.cpp —— 把「相机真实上限」和「OpenCV 解码开销」拆开量
// ----------------------------------------------------------------------------
// 【为什么必须拆】
//   相机参数表写的是 1280x720 MJPG = 250fps，可我们用 OpenCV 实测只有 67fps。
//   差这么多只有两种可能：
//     (a) 相机根本给不到那个数（USB 带宽/固件虚标）
//     (b) 相机给到了，但 OpenCV 把每一帧 MJPG【解成 BGR】太慢，成了瓶颈
//   这两种情况的改法完全相反，必须先分开量。
//
// 【怎么拆】
//   cv::CAP_PROP_CONVERT_RGB = 0 让 OpenCV【不要解码】，直接把 MJPG 原始数据给你。
//   这样：
//     · 正常模式 的 fps = 相机 → 解码 → 你的处理  的端到端速度
//     · 原始模式 的 fps = 相机 + 内存搬运的真实上限
//   两者的差就是「解码」吃掉的部分。再用 cv::imdecode 单独量一帧解码多少毫秒，
//   数字就能对上。
//
// 【安全】只打开/配置/读帧，读完就 release；不写曝光/增益等控制项。
//   每个阶段都有出流看门狗。整个程序跑完自动退出，不会占着相机。
//
// 【编译】
//   g++ -std=c++17 -O2 fps_ceiling.cpp -o fps_ceiling $(pkg-config --cflags --libs opencv4)
// 【用法】
//   ./fps_ceiling --seconds 3
// ============================================================================

#include <opencv2/opencv.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

static std::atomic<bool> g_got{false};

static void watchdog(int dev, int timeout_s) {
    for (int i = 0; i < timeout_s * 10; ++i) {
        if (g_got.load()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    printf("\n✗ /dev/video%d %d 秒内一帧都没读到\n", dev, timeout_s);
    fflush(stdout);
    _exit(1);
}

struct Res {
    double fps = -1;
    int realW = 0, realH = 0;
    double drvFps = 0;
    size_t bytes = 0;      // 原始模式下每帧多少字节（估计 USB 占用）
};

// raw=true  → 让 OpenCV 不做 JPEG 解码，直接给 MJPG 原始数据
static Res run(int dev, int W, int H, int FPS, int secs, bool raw, int quiet) {
    Res r;
    cv::VideoCapture cap(dev, cv::CAP_V4L2);
    if (!cap.isOpened()) return r;

    cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
    cap.set(cv::CAP_PROP_FRAME_WIDTH, W);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, H);
    cap.set(cv::CAP_PROP_FPS, FPS);
    cap.set(cv::CAP_PROP_BUFFERSIZE, 1);
    if (raw) cap.set(cv::CAP_PROP_CONVERT_RGB, 0);      // ★关键：不解码

    r.realW = (int)cap.get(cv::CAP_PROP_FRAME_WIDTH);
    r.realH = (int)cap.get(cv::CAP_PROP_FRAME_HEIGHT);
    r.drvFps = cap.get(cv::CAP_PROP_FPS);

    long n = 0;
    size_t bytesum = 0;
    double t0 = (double)cv::getTickCount();
    double el = 0;
    while (true) {
        cv::Mat f;
        if (!cap.read(f) || f.empty()) break;
        g_got.store(true);
        ++n;
        bytesum += f.total() * f.elemSize();
        el = ((double)cv::getTickCount() - t0) / cv::getTickFrequency();
        if (el >= secs) break;
    }
    cap.release();
    r.fps = el > 0 ? n / el : -1;
    r.bytes = n > 0 ? bytesum / n : 0;
    if (!quiet) { /* 占位 */ }
    return r;
}

int main(int argc, char ** argv) {
    int dev = 0, secs = 3;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--device") && i + 1 < argc) dev = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) secs = atoi(argv[++i]);
    }

    printf("============ 相机上限 / OpenCV 解码开销 分离测量 ============\n");
    printf("每阶段 %d 秒；原始模式 = CAP_PROP_CONVERT_RGB=0（不解码）\n\n", secs);

    std::thread(watchdog, dev, 15).detach();

    struct Mode { int w, h; const char * note; };
    const Mode modes[] = {
        {1280, 1024, "参数表 180fps"},
        {1280, 960,  "参数表 180fps"},
        {1280, 720,  "参数表 250fps"},
        {1024, 768,  "参数表 180fps"},
        {800,  600,  "参数表 180fps"},
        {640,  480,  "参数表 350fps"},
    };

    printf("%-12s %-14s | %-22s | %-22s | %s\n",
           "分辨率", "备注", "正常(含解码)", "原始(不解码)", "每帧KB/解码ms");
    printf("-----------------------------------------------------------------------------------------------\n");

    for (const auto & m : modes) {
        Res dec = run(dev, m.w, m.h, 400, secs, false, 1);
        Res raw = run(dev, m.w, m.h, 400, secs, true, 1);

        // 解码一帧要多少毫秒：先抓一张原始 MJPG，再单独 imdecode
        double dec_ms = 0;
        {
            cv::VideoCapture cap(dev, cv::CAP_V4L2);
            if (cap.isOpened()) {
                cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
                cap.set(cv::CAP_PROP_FRAME_WIDTH, m.w);
                cap.set(cv::CAP_PROP_FRAME_HEIGHT, m.h);
                cap.set(cv::CAP_PROP_FPS, 400);
                cap.set(cv::CAP_PROP_BUFFERSIZE, 1);
                cap.set(cv::CAP_PROP_CONVERT_RGB, 0);
                cv::Mat buf;
                for (int i = 0; i < 5; ++i) cap.read(buf);   // 丢掉前几帧
                if (!buf.empty()) {
                    double t0 = (double)cv::getTickCount();
                    int iters = 5;
                    for (int i = 0; i < iters; ++i) {
                        cv::Mat out = cv::imdecode(buf, cv::IMREAD_COLOR);
                    }
                    dec_ms = ((double)cv::getTickCount() - t0) /
                             cv::getTickFrequency() * 1000.0 / iters;
                }
                cap.release();
            }
        }

        printf("%-12s %-14s | %7.1f fps  %dx%d | %7.1f fps  %dx%d | %.0fKB / %.1fms\n",
               (std::to_string(m.w) + "x" + std::to_string(m.h)).c_str(), m.note,
               dec.fps, dec.realW, dec.realH,
               raw.fps, raw.realW, raw.realH,
               raw.bytes / 1024.0, dec_ms);
        fflush(stdout);
    }

    printf("\n判读方法：\n");
    printf("  · 「原始」比「正常」快很多 → 瓶颈是 OpenCV 的 MJPG 解码，不是相机\n");
    printf("  · 「原始」也上不去       → 瓶颈在相机/驱动/USB 带宽\n");
    printf("  · 「每帧KB」× 原始fps ×8 = 实际 USB 占用(Mbps)，USB2.0 上限约 280~320Mbps\n");
    return 0;
}
