// ============================================================================
// uvc_probe.cpp —— 检查这台机器能不能直接用 OpenCV 打开 UVC 摄像头
// ----------------------------------------------------------------------------
// 亚博智能那类"USB摄像头 1080P 120帧"是标准 UVC 免驱摄像头，
// 不需要厂商 SDK，走内核 uvcvideo 驱动 → /dev/video* → OpenCV VideoCapture。
// 这个程序做三件事：
//   ① 打印 OpenCV 版本和它支持的 videoio 后端（关键看有没有 V4L/V4L2）
//   ② 扫描 /dev/video0..9，报告每个节点能不能打开
//   ③ 如果能打开，设置 1080p 并尝试读一帧，报告真实分辨率/帧率
// 编译：
//   g++ -std=c++17 -O2 uvc_probe.cpp -o uvc_probe $(pkg-config --cflags --libs opencv4)
// 运行：
//   ./uvc_probe
// ============================================================================

#include <opencv2/opencv.hpp>
#include <cstdio>
#include <string>
#include <unistd.h>      // access()：判断设备节点是否存在

int main() {
    printf("================ UVC 摄像头环境自检 ================\n\n");

    // ---------- ① OpenCV 版本与后端 ----------
    printf("---- ① OpenCV 环境 ----\n");
    printf("OpenCV 版本 : %s\n", CV_VERSION);

    std::string info = cv::getBuildInformation();
    // 找 "Video I/O:" 这一段，看有没有 V4L/V4L2。
    // 【注意】OpenCV 打印出来的是小写 "v4l/v4l2:"，用大写 "V4L" 去 find 会误判成 NO，
    //         所以这里同时匹配 "v4l" 和 "V4L"。
    size_t p = info.find("Video I/O:");
    if (p != std::string::npos) {
        std::string seg = info.substr(p, 700);
        size_t end = seg.find("Parallel framework");
        if (end != std::string::npos) seg = seg.substr(0, end);
        printf("Video I/O 后端:\n%s\n", seg.c_str());
        if (seg.find("v4l") != std::string::npos || seg.find("V4L") != std::string::npos) {
            printf(">>> 结论: OpenCV 编译时带了 V4L/V4L2 支持，可以直接读 UVC 摄像头 ✓\n\n");
        } else {
            printf(">>> 警告: OpenCV 没有 V4L 支持，需要重装 OpenCV 或改用 python cv2\n\n");
        }
    }

    // ---------- ② 扫描 /dev/video* ----------
    printf("---- ② 扫描 /dev/video0..9 ----\n");
    bool any_ok = false;
    for (int i = 0; i < 10; ++i) {
        std::string dev = "/dev/video" + std::to_string(i);
        if (access(dev.c_str(), F_OK) != 0) {       // 节点不存在就直接跳过
            continue;
        }
        cv::VideoCapture cap(dev, cv::CAP_V4L2);
        if (!cap.isOpened()) {
            printf("%-14s 存在但打不开（可能没权限，或已被别的程序占用）\n", dev.c_str());
            continue;
        }
        any_ok = true;
        printf("%-14s 可以打开 ✓\n", dev.c_str());

        // ---------- ③ 尝试设 1080p 并读一帧 ----------
        cap.set(cv::CAP_PROP_FRAME_WIDTH, 1920);
        cap.set(cv::CAP_PROP_FRAME_HEIGHT, 1080);
        cap.set(cv::CAP_PROP_FPS, 60);

        double w = cap.get(cv::CAP_PROP_FRAME_WIDTH);
        double h = cap.get(cv::CAP_PROP_FRAME_HEIGHT);
        double fps = cap.get(cv::CAP_PROP_FPS);
        printf("      设置 1920x1080@60 后，实际生效的是 %.0f x %.0f @ %.1f fps\n", w, h, fps);

        cv::Mat frame;
        if (cap.read(frame) && !frame.empty()) {
            printf("      成功读到一帧: %d 列 x %d 行, %d 通道\n", frame.cols, frame.rows, frame.channels());
        } else {
            printf("      读帧失败（可能被占用或驱动异常）\n");
        }
        cap.release();
    }

    if (!any_ok) {
        printf("\n没有可用的 /dev/video* 节点。常见原因：\n");
        printf("  1) 摄像头没插，或没在 VMware 里挂载给虚拟机\n");
        printf("     （VMware: 虚拟机 → 可移动设备 → 找到摄像头 → 连接/断开连接）\n");
        printf("  2) 内核 uvcvideo 模块没加载：sudo modprobe uvcvideo\n");
        printf("  3) 权限不足：把用户加进 video 组并重新登录\n");
    }

    printf("\n================ 自检结束 ================\n");
    return 0;
}
