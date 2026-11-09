// ============================================================================
// uvc_test.cpp —— 实测 UVC 摄像头在"目标参数"下到底能跑多少帧
// ----------------------------------------------------------------------------
// 目的：v4l2-ctl 列出的只是"摄像头声称支持的"，实际能不能协商上、跑起来多少帧，
//       必须自己测。这里按 MJPG + 指定分辨率 + 指定帧率打开，实测 N 秒统计真实帧率。
//
// 用法：
//   g++ -std=c++17 -O2 uvc_test.cpp -o uvc_test $(pkg-config --cflags --libs opencv4)
//   ./uvc_test              # 默认按 640x480 @120 MJPG 测
//   ./uvc_test 0 1280 720 60
//   ./uvc_test <设备号> <宽> <高> <帧率>
//
// 同时会把一帧存成 uvc_frame.png，方便你看画面朝向对不对。
// ============================================================================

#include <opencv2/opencv.hpp>
#include <cstdio>
#include <chrono>

int main(int argc, char **argv) {
    int dev = (argc > 1) ? atoi(argv[1]) : 0;
    int W   = (argc > 2) ? atoi(argv[2]) : 640;
    int H   = (argc > 3) ? atoi(argv[3]) : 480;
    int FPS = (argc > 4) ? atoi(argv[4]) : 120;

    printf("================ UVC 实测 ================\n");
    printf("打开 /dev/video%d，请求 %dx%d @%d fps，MJPG\n", dev, W, H, FPS);

    cv::VideoCapture cap(dev, cv::CAP_V4L2);
    if (!cap.isOpened()) {
        printf("打不开 /dev/video%d\n", dev);
        return 1;
    }

    // 顺序：先 MJPG，再分辨率，最后帧率（顺序错了帧率常常协商不上）
    cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
    cap.set(cv::CAP_PROP_FRAME_WIDTH,  W);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, H);
    cap.set(cv::CAP_PROP_FPS,          FPS);
    cap.set(cv::CAP_PROP_BUFFERSIZE,   1);      // 缓冲降 1，减小延迟

    int    real_w = (int)cap.get(cv::CAP_PROP_FRAME_WIDTH);
    int    real_h = (int)cap.get(cv::CAP_PROP_FRAME_HEIGHT);
    double real_f = cap.get(cv::CAP_PROP_FPS);
    int    fourcc = (int)cap.get(cv::CAP_PROP_FOURCC);
    char   fcc[5] = { (char)(fourcc & 0xFF), (char)((fourcc >> 8) & 0xFF),
                      (char)((fourcc >> 16) & 0xFF), (char)((fourcc >> 24) & 0xFF), 0 };
    printf("实际生效：%d x %d @ %.1f fps，FOURCC=%s\n", real_w, real_h, real_f, fcc);

    // 丢掉前 30 帧（自动曝光/白平衡收敛需要时间）
    cv::Mat frame;
    for (int i = 0; i < 30; ++i) cap.read(frame);

    // 实测 5 秒的真实帧率
    const int SECONDS = 5;
    int count = 0;
    double t0 = (double)cv::getTickCount();
    while (true) {
        if (!cap.read(frame) || frame.empty()) break;
        ++count;
        double el = ((double)cv::getTickCount() - t0) / cv::getTickFrequency();
        if (el >= SECONDS) break;
    }
    double elapsed = ((double)cv::getTickCount() - t0) / cv::getTickFrequency();
    printf("实测：%d 帧 / %.2f 秒  =  %.1f fps\n", count, elapsed, count / elapsed);

    // 存一帧看朝向
    if (!frame.empty()) {
        cv::imwrite("uvc_frame.png", frame);
        printf("已保存一帧到 uvc_frame.png（%d 列 x %d 行），用来看画面朝向\n",
               frame.cols, frame.rows);
    }
    cap.release();
    printf("==========================================\n");
    return 0;
}
