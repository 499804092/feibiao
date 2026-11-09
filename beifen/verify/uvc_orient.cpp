// ============================================================================
// uvc_orient.cpp —— 画面朝向一键选定工具
// ----------------------------------------------------------------------------
// 换摄像头后最容易踩的坑就是"画面方向不对"，方向错了会导致后续 pitch/yaw
// 的正负号全反，飞控往反方向转。这个工具把 6 种候选朝向**同屏**显示出来，
// 你看哪个里面靶子是正立的，就把那个编号填进 sensors.json 的 orientation。
//
// 编译：
//   g++ -std=c++17 -O2 uvc_orient.cpp -o uvc_orient $(pkg-config --cflags --libs opencv4)
// 运行：
//   ./uvc_orient                # 默认 /dev/video0, 640x480@120 MJPG
//   ./uvc_orient 0 1280 720 60
//
// 操作：
//   按数字键 0~5  → 把当前画面按该朝向存成 orient_<n>.png（方便你截图发人）
//   按 q 或 ESC   → 退出
//
// ★拿摄像头对准靶子（或任何有明显上下方向的物体）再跑，效果最直观★
// ============================================================================

#include <opencv2/opencv.hpp>
#include <cstdio>
#include <string>
#include <vector>

// 应用某种朝向修正。与 UvcCamera.cpp 里的 switch 完全对应。
static cv::Mat applyOrientation(const cv::Mat &src, int mode) {
    cv::Mat out;
    switch (mode) {
        case 0: out = src.clone();                                        break;  // 原样
        case 1: cv::rotate(src, out, cv::ROTATE_90_CLOCKWISE);            break;  // 顺90
        case 2: cv::rotate(src, out, cv::ROTATE_180);                     break;  // 180
        case 3: cv::rotate(src, out, cv::ROTATE_90_COUNTERCLOCKWISE);     break;  // 逆90
        case 4: cv::flip(src, out, 1);                                    break;  // 水平镜像
        case 5: cv::flip(src, out, 0);                                    break;  // 垂直镜像
        default: out = src.clone();                                       break;
    }
    return out;
}

int main(int argc, char **argv) {
    int dev = (argc > 1) ? atoi(argv[1]) : 0;
    int W   = (argc > 2) ? atoi(argv[2]) : 640;
    int H   = (argc > 3) ? atoi(argv[3]) : 480;
    int FPS = (argc > 4) ? atoi(argv[4]) : 120;

    cv::VideoCapture cap(dev, cv::CAP_V4L2);
    if (!cap.isOpened()) { printf("打不开 /dev/video%d\n", dev); return 1; }
    cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
    cap.set(cv::CAP_PROP_FRAME_WIDTH,  W);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, H);
    cap.set(cv::CAP_PROP_FPS,          FPS);
    cap.set(cv::CAP_PROP_BUFFERSIZE,   1);

    printf("已打开 /dev/video%d  %dx%d  FOURCC=MJPG\n", dev, W, H);
    printf("把靶子放到镜头前，看哪个格子里的靶子是正的。\n");
    printf("按 0~5 保存对应图片，按 q 退出。\n");

    const char *names[6] = {
        "0: 原样 as-is",
        "1: 顺时针90 CW90",
        "2: 旋转180 R180",
        "3: 逆时针90 CCW90",
        "4: 水平镜像 flipH",
        "5: 垂直镜像 flipV"
    };

    const int THUMB_W = 400;          // 每格缩略图宽度
    cv::Mat frame, canvas;
    bool running = true;

    while (running) {
        if (!cap.read(frame) || frame.empty()) { printf("读帧失败\n"); break; }

        // 六格拼成 2 行 x 3 列的对比图
        std::vector<cv::Mat> tiles;
        for (int m = 0; m < 6; ++m) {
            cv::Mat t = applyOrientation(frame, m);
            // 统一缩放到同一宽度，方便并排比较
            double s = (double)THUMB_W / t.cols;
            cv::resize(t, t, cv::Size(THUMB_W, (int)(t.rows * s)));

            // 左上角标注编号和名称（黑底白字，加粗看得清）
            cv::rectangle(t, cv::Rect(0, 0, THUMB_W, 34), cv::Scalar(0, 0, 0), -1);
            cv::putText(t, names[m], cv::Point(8, 25),
                        cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(0, 255, 255), 2);

            // 画面中央画十字，方便看"上/下"方向对不对
            cv::line(t, cv::Point(t.cols / 2, 0), cv::Point(t.cols / 2, t.rows),
                     cv::Scalar(0, 0, 255), 1);
            cv::line(t, cv::Point(0, t.rows / 2), cv::Point(t.cols, t.rows / 2),
                     cv::Scalar(0, 0, 255), 1);
            tiles.push_back(t);
        }

        // 三列两行拼接（列宽相同，直接 hconcat/vconcat）
        cv::Mat row1, row2;
        cv::hconcat(std::vector<cv::Mat>{tiles[0], tiles[1], tiles[2]}, row1);
        cv::hconcat(std::vector<cv::Mat>{tiles[3], tiles[4], tiles[5]}, row2);
        // 两行高度可能差 1 像素，统一一下
        if (row1.rows != row2.rows) {
            int hh = std::min(row1.rows, row2.rows);
            row1 = row1(cv::Rect(0, 0, row1.cols, hh));
            row2 = row2(cv::Rect(0, 0, row2.cols, hh));
        }
        cv::vconcat(row1, row2, canvas);

        cv::imshow("choose orientation", canvas);
        int key = cv::waitKey(1) & 0xFF;
        if (key == 'q' || key == 27) {                       // q 或 ESC 退出
            running = false;
        } else if (key >= '0' && key <= '5') {               // 保存对应朝向的整帧
            int m = key - '0';
            std::string fn = "orient_" + std::to_string(m) + ".png";
            cv::imwrite(fn, applyOrientation(frame, m));
            printf("已保存 %s （orientation = %d）\n", fn.c_str(), m);
        }
    }

    cap.release();
    cv::destroyAllWindows();
    printf("\n选好之后，把编号填进：\n");
    printf("  feibiao/Vision_Code/config/json/sensors.json  →  \"orientation\": <编号>\n");
    return 0;
}
