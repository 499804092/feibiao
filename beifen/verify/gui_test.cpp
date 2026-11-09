// ============================================================================
// gui_test.cpp —— 最小窗口测试：确认 OpenCV 窗口能不能显示在你的桌面上
// ----------------------------------------------------------------------------
// 只做一件事：弹一个窗口，显示 10 秒倒计时，然后自己关闭。
// 用来排除"窗口根本没创建"和"相机/模型的问题"。
//
// 用法：
//   ./gui_test              # 用当前 DISPLAY
//   DISPLAY=:0 ./gui_test   # 指定 display
// ============================================================================

#include <opencv2/opencv.hpp>
#include <cstdio>
#include <cstdlib>

int main(int argc, char **argv) {
    int seconds = (argc > 1) ? atoi(argv[1]) : 10;

    printf("=== OpenCV 窗口测试 ===\n");
    printf("DISPLAY = %s\n", getenv("DISPLAY") ? getenv("DISPLAY") : "(未设置!)");

    const char *win = "GUI-TEST 如果你看到这个窗口就说明能显示";
    cv::namedWindow(win, cv::WINDOW_NORMAL);
    cv::resizeWindow(win, 640, 300);

    printf("窗口已创建，开始显示 %d 秒...\n", seconds);
    fflush(stdout);

    cv::Mat img(300, 640, CV_8UC3, cv::Scalar(40, 40, 40));
    for (int i = seconds; i > 0; --i) {
        img.setTo(cv::Scalar(40, 40, 40));
        cv::putText(img, "GUI OK", cv::Point(40, 100),
                    cv::FONT_HERSHEY_SIMPLEX, 2.0, cv::Scalar(0, 255, 0), 4);
        char buf[64];
        snprintf(buf, sizeof(buf), "close in %d s", i);
        cv::putText(img, buf, cv::Point(40, 200),
                    cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(255, 255, 255), 2);
        cv::imshow(win, img);
        int key = cv::waitKey(1000) & 0xFF;      // 每秒刷新一次
        if (key == 'q' || key == 27) break;
    }

    cv::destroyAllWindows();
    printf("窗口测试结束（能走到这里说明 GUI 正常）\n");
    return 0;
}
