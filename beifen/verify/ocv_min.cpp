// ============================================================================
// ocv_min.cpp —— 最小化 OpenCV 取流测试：逐个设置项隔离，找出哪一步把流搞停
// ----------------------------------------------------------------------------
// 【为什么需要它】
//   现象：ffmpeg 能从同一个 /dev/video0 抓 MJPG 640x480，但工程的 OpenCV
//   取流一直报 select() timeout（像卡死）。差别只可能在 OpenCV 调的那几个
//   cap.set() 上。这个程序每次只做「部分设置」，从而定位到具体是哪一句。
//
// 【用法】 一次只跑一种模式（每种模式必须单独一个进程，避免状态互相污染）
//   ./ocv_min 1     # 只 open，什么都不设
//   ./ocv_min 2     # + FOURCC=MJPG
//   ./ocv_min 3     # + 640x480
//   ./ocv_min 4     # + FPS=120      ← 工程当前的做法
//   ./ocv_min 5     # + BUFFERSIZE=1 ← 工程当前的做法
//   ./ocv_min 30    # 3 步 + FPS=30  （测试是不是帧率请求的问题）
//   ./ocv_min 60    # 3 步 + FPS=60
// 必须配 timeout 使用，因为 cap.read() 在没有帧时会阻塞：
//   timeout 20 ./ocv_min 4
// ============================================================================

#include <opencv2/opencv.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("用法: ./ocv_min <模式>   模式: 1,2,3,4,5,30,60\n");
        return 2;
    }
    const int mode = atoi(argv[1]);

    printf("[模式 %d] ", mode);
    cv::VideoCapture cap(0, cv::CAP_V4L2);
    if (!cap.isOpened()) {
        printf("❌ 打不开设备\n");
        return 1;
    }

    // ---- 按模式逐步加设置 ----
    if (mode == 1) {
        printf("只 open，不做任何 set ... ");
    } else {
        cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
        if (mode == 2) {
            printf("open + FOURCC=MJPG ... ");
        } else {
            cap.set(cv::CAP_PROP_FRAME_WIDTH, 640);
            cap.set(cv::CAP_PROP_FRAME_HEIGHT, 480);
            if (mode == 3) {
                printf("open + FOURCC + 640x480 ... ");
            } else if (mode == 4) {
                cap.set(cv::CAP_PROP_FPS, 120);
                printf("open + FOURCC + 640x480 + FPS=120 ... ");
            } else if (mode == 5) {
                cap.set(cv::CAP_PROP_FPS, 120);
                cap.set(cv::CAP_PROP_BUFFERSIZE, 1);
                printf("open + FOURCC + 640x480 + FPS=120 + BUFFERSIZE=1 (工程当前做法) ... ");
            } else {
                cap.set(cv::CAP_PROP_FPS, (double)mode);
                printf("open + FOURCC + 640x480 + FPS=%d ... ", mode);
            }
        }
    }
    fflush(stdout);

    printf("回读 %dx%d @%.0f | ", (int)cap.get(cv::CAP_PROP_FRAME_WIDTH),
           (int)cap.get(cv::CAP_PROP_FRAME_HEIGHT), cap.get(cv::CAP_PROP_FPS));
    fflush(stdout);

    // ---- 尝试读 5 帧 ----
    cv::Mat f;
    int ok = 0;
    double t0 = (double)cv::getTickCount();
    for (int i = 0; i < 5; ++i) {
        if (!cap.read(f) || f.empty()) break;
        ++ok;
    }
    double dt = ((double)cv::getTickCount() - t0) / cv::getTickFrequency();

    if (ok > 0) {
        printf("✅ 读到 %d 帧，用时 %.2fs (约 %.1f fps)\n", ok, dt, ok / (dt > 0 ? dt : 1));
        cap.release();
        return 0;
    }
    printf("❌ 一帧都读不到（select timeout）\n");
    cap.release();
    return 1;
}
