// ============================================================================
// uvc_view.cpp —— 独立取图程序：打开 UVC 摄像头并实时显示画面
// ----------------------------------------------------------------------------
// 【为什么单独写这个】
//   工程里的 UvcCamera.cpp 依赖 sensors::BaseCamera、Config.hpp、SerialPort 等一堆东西，
//   没搭好整个工程就跑不起来。这个程序**没有任何工程依赖**，只要 OpenCV 就能编译，
//   用来快速确认"相机能不能打开、画面正不正常、朝向对不对"。
//
//   它里面的打开流程与 UvcCamera.cpp 的 cameraInit()/getImage() **完全一致**
//   （先设 FOURCC 再设分辨率、BUFFERSIZE=1、读帧、朝向修正），
//   所以这里能看到画面，工程里就一定能看到。
//
// 编译：
//   g++ -std=c++17 -O2 uvc_view.cpp -o uvc_view $(pkg-config --cflags --libs opencv4)
//
// 运行：
//   ./uvc_view                      # 默认 /dev/video0, 640x480@120 MJPG, 朝向 0
//   ./uvc_view 0 640 480 120 1      # 参数: 设备号 宽 高 帧率 朝向
//   ./uvc_view --headless           # 不开窗口，只统计帧率并存图（无桌面/SSH 环境用）
//
// 键盘操作：
//   q / ESC : 退出
//   s       : 存一张图 view_shot.png
//   o       : 循环切换朝向 0→1→2→3→4→5→0（实时看哪个对）
//   i       : 打印当前实测帧率
// ============================================================================

#include <opencv2/opencv.hpp>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>

// ---- 与 UvcCamera.cpp 里的 switch 完全一致 ----
static cv::Mat applyOrientation(const cv::Mat &src, int mode) {
    cv::Mat out;
    switch (mode) {
        case 0: out = src.clone();                                    break;  // 原样
        case 1: cv::rotate(src, out, cv::ROTATE_90_CLOCKWISE);        break;  // 顺时针 90°
        case 2: cv::rotate(src, out, cv::ROTATE_180);                 break;  // 180°
        case 3: cv::rotate(src, out, cv::ROTATE_90_COUNTERCLOCKWISE); break;  // 逆时针 90°
        case 4: cv::flip(src, out, 1);                                break;  // 水平镜像
        case 5: cv::flip(src, out, 0);                                break;  // 垂直镜像
        default: out = src.clone();                                   break;
    }
    return out;
}

static const char *orientName(int m) {
    switch (m) {
        case 0: return "0 as-is";
        case 1: return "1 CW90";
        case 2: return "2 R180";
        case 3: return "3 CCW90";
        case 4: return "4 flipH";
        case 5: return "5 flipV";
        default: return "?";
    }
}

int main(int argc, char **argv) {
    // --headless：不开窗口，只取帧 + 统计帧率 + 存图。
    // 用于 SSH / 无桌面环境，或者只想确认"相机到底能不能出图"的时候。
    // 参数解析：先挑出所有纯数字参数，再按顺序当作 设备号 宽 高 帧率 朝向。
    // （这样 --headless 放哪个位置都不影响，避免位置错乱）
    bool headless = false;
    std::vector<int> nums;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--headless") == 0) {
            headless = true;
        } else {
            char *endp = nullptr;
            long v = strtol(argv[i], &endp, 10);
            if (endp && *endp == '\0') nums.push_back((int)v);   // 整个参数都是数字才算
        }
    }

    const int dev = (nums.size() > 0) ? nums[0] : 0;
    const int W   = (nums.size() > 1) ? nums[1] : 640;
    const int H   = (nums.size() > 2) ? nums[2] : 480;
    const int FPS = (nums.size() > 3) ? nums[3] : 120;
    int orient    = (nums.size() > 4) ? nums[4] : 0;

    printf("=== UVC 取图测试%s ===\n", headless ? "（headless 模式）" : "");
    printf("设备 /dev/video%d，请求 %dx%d @%d fps MJPG，朝向 %d\n", dev, W, H, FPS, orient);

    // ① 打开设备。第二个参数一定要写 cv::CAP_V4L2，
    //    否则 OpenCV 可能去试 GStreamer 后端，报一些看不懂的错。
    cv::VideoCapture cap(dev, cv::CAP_V4L2);
    if (!cap.isOpened()) {
        printf("❌ 打不开 /dev/video%d\n", dev);
        printf("   排查：1) v4l2-ctl --list-devices 看设备号\n");
        printf("         2) ls -l /dev/video* 看权限（必要时 sudo chmod 666 /dev/video*）\n");
        printf("         3) 有没有别的程序占着（比如另一个预览窗口没关）\n");
        return 1;
    }

    // ② 先设格式，再设分辨率，最后设帧率 —— 顺序很重要，写反了帧率经常协商不上。
    //    高帧率必须用 MJPG（YUYV 未压缩带宽不够）。
    cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
    cap.set(cv::CAP_PROP_FRAME_WIDTH,  W);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, H);
    cap.set(cv::CAP_PROP_FPS,          FPS);

    // ③ 驱动默认可能缓冲 4 帧 → 画面延迟 4 帧，打飞镖是致命的。降到 1。
    cap.set(cv::CAP_PROP_BUFFERSIZE, 1);

    // ④ 回读实际生效的参数 —— 摄像头不一定接受你请求的值，必须打印出来看
    int    rw = (int)cap.get(cv::CAP_PROP_FRAME_WIDTH);
    int    rh = (int)cap.get(cv::CAP_PROP_FRAME_HEIGHT);
    double rf = cap.get(cv::CAP_PROP_FPS);
    int    fcc = (int)cap.get(cv::CAP_PROP_FOURCC);
    char   fccs[5] = { (char)(fcc & 0xFF), (char)((fcc >> 8) & 0xFF),
                       (char)((fcc >> 16) & 0xFF), (char)((fcc >> 24) & 0xFF), 0 };
    printf("实际生效：%d x %d @ %.1f fps，FOURCC=%s\n", rw, rh, rf, fccs);

    // ⑤ 先空读几帧：自动曝光/白平衡需要十几帧才收敛，开头几帧偏暗偏色是正常的
    cv::Mat frame;
    for (int i = 0; i < 10; ++i) cap.read(frame);

    printf("\n按键说明：q/ESC 退出 | s 存图 | o 切换朝向 | i 打印帧率\n");
    printf("当前朝向：%s\n", orientName(orient));

    // ⑥ 主循环
    long   frames = 0;
    double t0 = (double)cv::getTickCount();     // 帧率统计起点

    // ---------- headless：不开窗口，跑固定时长后存图退出 ----------
    if (headless) {
        const double SECONDS = 5.0;
        double last_report = 0.0;
        cv::Mat last;
        while (true) {
            if (!cap.read(frame) || frame.empty()) {
                printf("❌ 读帧失败（相机掉线或被占用）\n");
                break;
            }
            ++frames;
            last = applyOrientation(frame, orient);

            double el = ((double)cv::getTickCount() - t0) / cv::getTickFrequency();
            if (el - last_report >= 1.0) {                 // 每秒报一次进度
                last_report = el;
                printf("  %4.1f s : 已取 %ld 帧，当前 %.1f fps\n", el, frames, frames / el);
            }
            if (el >= SECONDS) break;
        }
        if (!last.empty()) {
            cv::imwrite("view_shot.png", last);
            // 判断画面是不是全黑/全白（说明镜头盖没开、曝光参数错、或者相机没真的出图）
            cv::Scalar mean, stddev;
            cv::meanStdDev(last, mean, stddev);
            printf("\n画面统计: 均值 B=%.1f G=%.1f R=%.1f，标准差=%.1f\n",
                   mean[0], mean[1], mean[2], stddev[0]);
            if (stddev[0] < 2.0) {
                printf("⚠️ 标准差接近 0 → 画面几乎是纯色！检查：镜头盖、曝光参数、USB 带宽\n");
            } else {
                printf("✓ 画面有内容（标准差足够大）\n");
            }
            printf("已保存 view_shot.png（%d 列 x %d 行），可以拉到本机看朝向\n",
                   last.cols, last.rows);
        }
        double total = ((double)cv::getTickCount() - t0) / cv::getTickFrequency();
        printf("headless 结束：共 %ld 帧 / %.2f 秒 = 平均 %.1f fps\n",
               frames, total, total > 0 ? frames / total : 0.0);
        cap.release();
        return 0;
    }

    while (true) {
        if (!cap.read(frame) || frame.empty()) {
            printf("❌ 读帧失败（相机掉线或被占用），退出\n");
            break;
        }
        ++frames;

        // 应用朝向修正
        cv::Mat shown = applyOrientation(frame, orient);

        // 叠加提示信息（画在图上，方便你看截图）
        cv::rectangle(shown, cv::Rect(0, 0, shown.cols, 36), cv::Scalar(0, 0, 0), -1);
        char txt[256];
        double elapsed = ((double)cv::getTickCount() - t0) / cv::getTickFrequency();
        double cur_fps = (elapsed > 0.5) ? frames / elapsed : 0.0;
        snprintf(txt, sizeof(txt), "%dx%d  %.1f fps  orient=%s",
                 shown.cols, shown.rows, cur_fps, orientName(orient));
        cv::putText(shown, txt, cv::Point(8, 26),
                    cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 255, 255), 2);

        // 画面中心画个红十字，方便判断"上下方向"
        cv::line(shown, cv::Point(shown.cols / 2, 0), cv::Point(shown.cols / 2, shown.rows),
                 cv::Scalar(0, 0, 255), 1);
        cv::line(shown, cv::Point(0, shown.rows / 2), cv::Point(shown.cols, shown.rows / 2),
                 cv::Scalar(0, 0, 255), 1);

        cv::imshow("UVC view (q=quit s=save o=orient i=fps)", shown);

        int key = cv::waitKey(1) & 0xFF;
        if (key == 'q' || key == 27) {
            break;
        } else if (key == 's') {
            cv::imwrite("view_shot.png", shown);
            printf("已保存 view_shot.png\n");
        } else if (key == 'o') {
            orient = (orient + 1) % 6;              // 循环切换朝向
            printf("切换到朝向 %s\n", orientName(orient));
        } else if (key == 'i') {
            printf("当前实测帧率：%.1f fps（累计 %ld 帧 / %.1f 秒）\n",
                   cur_fps, frames, elapsed);
        }
    }

    cap.release();
    cv::destroyAllWindows();

    double total = ((double)cv::getTickCount() - t0) / cv::getTickFrequency();
    printf("\n退出。共 %ld 帧 / %.1f 秒 = 平均 %.1f fps\n", frames, total,
           total > 0 ? frames / total : 0.0);
    return 0;
}
