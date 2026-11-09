// ============================================================================
// ocv_fmt.cpp —— 用 OpenCV 测试指定像素格式/帧率下能不能稳定出流
// ----------------------------------------------------------------------------
// 【用法】 ./ocv_fmt <MJPG|YUYV> <帧率> [读多少帧]
//   ./ocv_fmt YUYV 30 30      # YUYV 640x480 @30，读 30 帧
//   ./ocv_fmt MJPG 120 30     # 工程当前配置
// 必须配 timeout 用：相机不出流时 cap.read() 会无限阻塞。
//
// 输出：实际读到的帧数 + 耗时 + 实际帧率；读不满就说明出流不稳。
// ============================================================================

#include <opencv2/opencv.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char **argv) {
    const char *fmt  = (argc > 1) ? argv[1] : "YUYV";
    const int   fps  = (argc > 2) ? atoi(argv[2]) : 30;
    const int   want = (argc > 3) ? atoi(argv[3]) : 30;

    printf("[%s %dx%d @%d] ", fmt, 640, 480, fps);
    fflush(stdout);

    cv::VideoCapture cap(0, cv::CAP_V4L2);
    if (!cap.isOpened()) { printf("❌ 打不开设备\n"); return 1; }

    if (!strcmp(fmt, "MJPG")) {
        cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
    } else {
        cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('Y', 'U', 'Y', 'V'));
    }
    cap.set(cv::CAP_PROP_FRAME_WIDTH, 640);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, 480);
    cap.set(cv::CAP_PROP_FPS, fps);
    cap.set(cv::CAP_PROP_BUFFERSIZE, 1);

    printf("回读 %dx%d @%.0f | ",
           (int)cap.get(cv::CAP_PROP_FRAME_WIDTH),
           (int)cap.get(cv::CAP_PROP_FRAME_HEIGHT),
           cap.get(cv::CAP_PROP_FPS));
    fflush(stdout);

    cv::Mat f;
    int ok = 0;
    double t0 = (double)cv::getTickCount();
    for (int i = 0; i < want; ++i) {
        if (!cap.read(f) || f.empty()) break;
        ++ok;
    }
    double dt = ((double)cv::getTickCount() - t0) / cv::getTickFrequency();

    printf("读到 %d/%d 帧，用时 %.2fs，实际 %.1f fps", ok, want, dt, ok / (dt > 0 ? dt : 1));
    if (ok >= want) {
        printf("  ✅ 稳定出流\n");
        cap.release();
        return 0;
    }
    printf("  ❌ 出流不稳\n");
    cap.release();
    return 1;
}
