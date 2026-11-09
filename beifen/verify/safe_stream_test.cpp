// ============================================================================
// safe_stream_test.cpp —— 安全取流测试（可指定 分辨率/帧率/格式）
// ----------------------------------------------------------------------------
// 【为什么单独写一个】
//   feibiao/uvc_detect_view 不能指定分辨率，而换分辨率测 MJPG 是现在最有信息量的
//   实验。这个工具就是为它写的，同时把安全设计做足。
//
// 【安全设计（重要）】
//   ① 完全不写任何 V4L2 控制项 —— 只设 FOURCC/宽/高/帧率/缓冲数（取流参数）
//      （之前把相机搞卡的最可疑操作就是「流运行中写曝光控制项」）
//   ② 只开一次设备、只释放一次，绝不重试、绝不重开
//   ③ 超时靠【程序自己的看门狗】干净退出（_exit），不依赖外部 kill
//      （「流运行中被强杀」才是危险操作）
//
// 【用法】
//   ./safe_stream_test <宽> <高> <帧率> <MJPG|YUYV> [读几帧] [看门狗秒数]
//   ./safe_stream_test 1280 720 60 MJPG 30 15
// ============================================================================

#include <opencv2/opencv.hpp>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unistd.h>

static std::atomic<bool> g_got_frame{false};

int main(int argc, char **argv) {
    const int   W    = (argc > 1) ? atoi(argv[1]) : 640;
    const int   H    = (argc > 2) ? atoi(argv[2]) : 480;
    const int   FPS  = (argc > 3) ? atoi(argv[3]) : 120;
    const char *FMT  = (argc > 4) ? argv[4] : "MJPG";
    const int   want = (argc > 5) ? atoi(argv[5]) : 30;
    const int   tmo  = (argc > 6) ? atoi(argv[6]) : 15;

    if (strlen(FMT) < 4) { printf("格式必须是 4 个字符，如 MJPG / YUYV\n"); return 2; }

    printf("================================================================\n");
    printf("安全取流测试：%s %dx%d @%d   目标 %d 帧   看门狗 %d 秒\n", FMT, W, H, FPS, want, tmo);
    printf("安全设计：不写任何控制项 / 只开一次 / 不重试 / 看门狗自己退出\n");
    printf("================================================================\n\n");
    fflush(stdout);

    // ---- 看门狗：tmo 秒内一帧都没有，就自己干净退出（不是被外部杀）----
    std::thread([tmo]() {
        for (int i = 0; i < tmo * 10; ++i) {
            if (g_got_frame.load()) return;                 // 已经出流，正常退场
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        printf("\n❌ 看门狗：%d 秒内一帧都读不到 —— 这个配置下相机不出流\n", tmo);
        printf("   （程序自己退出，没有被外部杀进程）\n");
        fflush(stdout);
        _exit(1);
    }).detach();

    cv::VideoCapture cap(0, cv::CAP_V4L2);
    if (!cap.isOpened()) {
        printf("❌ 打不开 /dev/video0\n");
        return 1;
    }

    cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc(FMT[0], FMT[1], FMT[2], FMT[3]));
    cap.set(cv::CAP_PROP_FRAME_WIDTH,  W);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, H);
    cap.set(cv::CAP_PROP_FPS,          FPS);
    cap.set(cv::CAP_PROP_BUFFERSIZE,   1);

    printf("回读实际生效：%dx%d @%.0f\n",
           (int)cap.get(cv::CAP_PROP_FRAME_WIDTH),
           (int)cap.get(cv::CAP_PROP_FRAME_HEIGHT),
           cap.get(cv::CAP_PROP_FPS));
    fflush(stdout);

    cv::Mat f;
    int got = 0;
    double t0 = (double)cv::getTickCount();
    for (int i = 0; i < want; ++i) {
        if (!cap.read(f) || f.empty()) break;
        ++got;
        g_got_frame.store(true);        // 只要读到过帧，看门狗就不再报警
    }
    double dt = ((double)cv::getTickCount() - t0) / cv::getTickFrequency();

    printf("\n读到 %d/%d 帧，用时 %.2f 秒，实际 %.1f fps\n",
           got, want, dt, got / (dt > 0 ? dt : 1));
    if (got >= want)      printf("⇒ ✅ 稳定出流 —— 这个配置可用\n");
    else if (got > 0)     printf("⇒ ⚠️ 出流不稳（只读到 %d 帧就断了）\n", got);
    else                  printf("⇒ ❌ 一帧都读不到\n");

    cap.release();
    return (got > 0) ? 0 : 1;
}
