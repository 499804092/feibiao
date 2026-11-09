// ============================================================================
// uvc_color_view.cpp —— 开相机看画面 + 实时调 HSV 阈值
// ----------------------------------------------------------------------------
// 【它干什么】
//   打开 UVC 相机，用和工程**完全同一套** ColorDetector 找绿色引导灯，
//   把框和质心画在画面上；再开一个小窗口显示二值掩膜（调阈值时看的就是它）。
//
// 【为什么需要它】
//   HSV 阈值跟相机、光照、白平衡都有关，纸面上算不出来，必须看着画面调。
//   调好之后按 g 打印参数，直接粘进 config/tuning.yaml 就固化了。
//
// 【编译】（不需要 OpenVINO，只要 OpenCV）—— 下面是一条完整命令，复制整行执行
//   cd ~/feibiao
//   g++ -std=c++17 -O2 uvc_color_view.cpp Vision_Code/src/detect/src/color/ColorDetector.cpp -I Vision_Code/src/detect/inc -o uvc_color_view $(pkg-config --cflags --libs opencv4)
//
// 【用法】
//   ./uvc_color_view                          # 默认 640x480 @120
//   ./uvc_color_view --width 640 --height 480 --fps 120
//   ./uvc_color_view --headless --seconds 10  # 无窗口，只打印统计
//   ./uvc_color_view --image yindaodeng.jpg --orientation 0
//                                             # 离线识别一张图片，完全不碰相机
//                                             # （照片是正的就要 --orientation 0，
//                                             #   不加会按 sensors.json 里的 2 转 180°）
//
// 【按键】
//   [ ]      选择要调的参数（Hmin→Hmax→Smin→Smax→Vmin→Vmax→长宽比→圆度→形态学）
//   - =      把选中的参数减小 / 增大
//   o        切换画面朝向（0不转 → 1顺90° → 2转180° → 3逆90° → 4水平镜像 → 5垂直镜像）
//            画面倒过来就按到 2；左右反了就按到 4
//   m        显示 / 隐藏 掩膜窗口
//   g        打印当前参数（YAML 格式，可直接粘进 config/tuning.yaml）
//   s        存一张带标注的图
//   r        恢复默认参数
//   q / ESC  退出
//
// 【画面朝向】
//   默认从 config/tuning.yaml 的 uvc.orientation 读（和工程 Dart 用同一个配置项），
//   所以调试窗口看到的画面和 Dart 里识别用的画面是一致的。
//   命令行也可以用 --orientation N 直接指定。
//
// 【安全设计】cv::read() 在相机不出流时会无限阻塞，所以加了看门狗：
//   超过 --stream-timeout 秒（默认10）读不到帧，就打印排查步骤并干净退出。
// ============================================================================

#include <opencv2/opencv.hpp>                                        // OpenCV：相机、画图、窗口
#include <atomic>                                                    // std::atomic：看门狗的标志位（多线程安全）
#include <chrono>                                                    // 时间相关（这里主要给 sleep 用）
#include <cstdio>                                                    // printf
#include <cstring>                                                   // strcmp：比较命令行参数
#include <cstdlib>                                                   // atoi：字符串转整数
#include <string>                                                    // std::string
#include <thread>                                                    // std::thread：看门狗线程
#include <unistd.h>                                                  // _exit()：从线程里立即结束进程

#include "color/ColorDetector.hpp"                                   // 复用工程里的识别器（保证调参和工程行为一致）

// ---------------------------------------------------------------------------
// 出流看门狗：相机不出流时不让程序假死
//   cv::read() 在相机不出流时会无限阻塞，没有这个的话终端就没输出了，
//   看起来像卡死，实际是在等一帧永远不会来的画面。
// ---------------------------------------------------------------------------
static std::atomic<bool> g_got_frame{false};                         // 全局标志：有没有成功读到过帧

static void streamWatchdog(int dev, int timeout_s) {                 // 看门狗函数（跑在独立线程里）
    for (int i = 0; i < timeout_s * 10; ++i) {                       // 每 100ms 检查一次，共检查 timeout_s 秒
        if (g_got_frame.load()) {                                    // 已经读到过帧了
            return;                                                  // 看门狗正常退场，不再管
        }                                                            // if 结束
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // 睡 100ms 再检查
    }                                                                // for 结束
    printf("\n");                                                    // 换行，让提示好看点
    printf("==================================================================\n");    // 分隔线
    printf("相机 /dev/video%d 打开了，但 %d 秒内一帧都读不到 —— 没有出流\n", dev, timeout_s);  // 明确说清楚问题
    printf("==================================================================\n");    // 分隔线
    printf("按顺序试：\n");                                          // 下面是排查步骤
    printf("  1) 换一种分辨率/帧率试试（本相机 640x480 只有 120fps 一档，时好时坏）\n");        // 建议 1
    printf("     例： ./uvc_color_view --width 800 --height 600 --fps 60\n");                   // 具体命令
    printf("  2) 把相机拔下来等 10 秒再插上（真断电，重启虚拟机不算）\n");                        // 建议 2
    printf("  3) 确认没有别的程序占着相机：  pgrep -a -f uvc_\n");                               // 建议 3
    printf("==================================================================\n");    // 分隔线
    fflush(stdout);                                                  // 把上面这些立刻刷出去（不然可能看不到）
    _exit(1);                                                        // 直接结束整个进程（用 _exit 避免静态析构出问题）
}                                                                    // 看门狗结束

// ---------------------------------------------------------------------------
// drawResult() —— 把识别结果画到画面上
// ---------------------------------------------------------------------------
static void drawResult(cv::Mat &img,                                 // 要画的图（会被直接修改）
                       const std::vector<ColorDetector::Light> &lights) {  // 识别出来的目标列表
    for (size_t i = 0; i < lights.size(); ++i) {                     // 逐个目标画
        const auto &l = lights[i];                                   // 取第 i 个目标（引用，不拷贝）
        cv::Scalar color = (i == 0) ? cv::Scalar(0, 0, 255)          // 第 1 个是我们要的（面积最大）→ 红色
                                    : cv::Scalar(0, 255, 255);       // 其余备选 → 黄色（BGR 顺序：0,255,255 是黄）

        cv::rectangle(img, l.box, color, 2);                         // 画外接矩形，线宽 2
        cv::circle(img, l.center_point, 5, color, -1);               // 画质心：实心小圆（-1 = 填充）
        cv::circle(img, l.center_point, 8, cv::Scalar(255, 255, 255), 1);  // 质心外面再套一个白圈，更醒目

        char txt[160];                                               // 文本缓冲区
        snprintf(txt, sizeof(txt), "#%zu area=%.0f (%.0f,%.0f)",     // 拼字符串：序号 / 面积 / 质心坐标
                 i + 1, l.area, l.center_point.x, l.center_point.y); // 填进缓冲区
        int ty = (int)l.box.y - 8;                                   // 文字默认画在框上方 8 像素
        if (ty < 20) ty = (int)(l.box.y + l.box.height) + 22;        // 框太靠上就改画到框下面，别出画面
        cv::putText(img, txt, cv::Point((int)l.box.x, ty),           // 把文字画上去
                    cv::FONT_HERSHEY_SIMPLEX, 0.55, color, 2);       // 字体 / 大小 / 颜色 / 线宽
    }                                                                // for 结束

    cv::line(img, cv::Point(img.cols / 2, img.rows / 2 - 15),        // 画面中心的十字准星：竖线上半段
             cv::Point(img.cols / 2, img.rows / 2 + 15),             // 竖线下半段
             cv::Scalar(255, 255, 255), 1);                          // 白色，线宽 1
    cv::line(img, cv::Point(img.cols / 2 - 15, img.rows / 2),        // 横线左半段
             cv::Point(img.cols / 2 + 15, img.rows / 2),             // 横线右半段
             cv::Scalar(255, 255, 255), 1);                          // 白色，线宽 1
}                                                                    // drawResult 结束

// ---------------------------------------------------------------------------
// applyOrientation() —— 画面朝向修正（旋转 / 镜像）
//   相机装反了画面就是倒的；这里按和工程 UvcCamera 完全一样的规则修正，
//   保证调试窗口看到的画面 == Dart 里识别用的画面
//     0 = 不动
//     1 = 顺时针转 90°
//     2 = 转 180°（上下颠倒时用这个）
//     3 = 逆时针转 90°（= 顺时针 270°）
//     4 = 水平镜像
//     5 = 垂直镜像
// ---------------------------------------------------------------------------
static cv::Mat applyOrientation(const cv::Mat &src, int mode) {      // 入参：原图 + 模式号；返回：修正后的图
    cv::Mat out;                                                     // 存放结果
    switch (mode) {                                                  // 按模式号分支
        case 1: cv::rotate(src, out, cv::ROTATE_90_CLOCKWISE);         break;  // 顺时针 90°
        case 2: cv::rotate(src, out, cv::ROTATE_180);                  break;  // 转 180°
        case 3: cv::rotate(src, out, cv::ROTATE_90_COUNTERCLOCKWISE);  break;  // 逆时针 90°
        case 4: cv::flip(src, out, 1);                                 break;  // 水平镜像（左右翻）
        case 5: cv::flip(src, out, 0);                                 break;  // 垂直镜像（上下翻）
        default: out = src.clone();                                    break;  // 0 或非法值：保持原样
    }                                                                // switch 结束
    return out;                                                      // 返回修正后的图
}                                                                    // applyOrientation 结束

// ---------------------------------------------------------------------------
// printParamsYaml() —— 把当前调好的参数打印成能直接粘进 tuning.yaml 的片段
//   为什么输出 YAML 而不是 JSON：现在唯一的配置文件是 config/tuning.yaml，
//   打印成 JSON 还得手工翻译一遍才能用。
// ---------------------------------------------------------------------------
static void printParamsYaml(const ColorDetector::Params &p) {        // 入参：当前参数
    printf("\n--------- 当前参数（下面这段直接粘进 config/tuning.yaml 覆盖 color 节点）---------\n");
    printf("color:\n");                                              // YAML 里 color 是一个映射节点
    printf("  h_min: %d          # 色调下限\n", p.h_min);             // 色调下限
    printf("  h_max: %d          # 色调上限\n", p.h_max);             // 色调上限
    printf("  s_min: %d          # 饱和度下限\n", p.s_min);           // 饱和度下限
    printf("  s_max: %d          # 饱和度上限\n", p.s_max);           // 饱和度上限
    printf("  v_min: %d          # 亮度下限\n", p.v_min);             // 亮度下限
    printf("  v_max: %d          # 亮度上限\n", p.v_max);             // 亮度上限
    printf("  aspect_max: %.2f   # 长宽比上限\n", p.aspect_max);       // 长宽比上限
    printf("  extent_min: %.2f   # 填充率下限（0=关闭）\n", p.extent_min);        // 填充率下限
    printf("  circularity_min: %.2f   # 圆度下限（0=关闭）\n", p.circularity_min); // 圆度下限
    printf("  min_area: %.0f      # 最小轮廓面积\n", p.min_area);      // 最小面积
    printf("  ref_area: %.0f      # 面积归一化基准\n", p.ref_area);    // 归一化基准
    printf("  max_targets: %d      # 最多返回几个目标\n", p.max_targets);  // 最多几个
    printf("  morph_size: %d       # 形态学核大小（0=关闭）\n", p.morph_size);  // 形态学核
    printf("  roi_top: %.2f        # ROI 上边界\n", p.roi_top);        // ROI 上边界
    printf("  roi_bottom: %.2f     # ROI 下边界\n", p.roi_bottom);     // ROI 下边界
    printf("--------------------------------------------------------------------------------\n");
    fflush(stdout);                                                  // 立刻刷出
}                                                                    // printParamsJson 结束

// ---------------------------------------------------------------------------
// main —— 主流程
// ---------------------------------------------------------------------------
int main(int argc, char **argv) {                                    // argv[0] 是程序名，参数从 argv[1] 开始
    int  dev = 0;                                                    // 相机设备号：/dev/video0
    int  W = 640;                                                    // 请求的画面宽度
    int  H = 480;                                                    // 请求的画面高度
    int  FPS = 120;                                                  // 请求的帧率
    int  seconds = 15;                                               // headless 模式跑多少秒
    int  stream_timeout = 10;                                        // 等出流最多等多少秒
    int  orient = -1;                                                // 画面朝向：-1 = 还没定，稍后从 sensors.json 读
    bool headless = false;                                           // 是否无窗口模式
    bool show_mask = false;                                          // 是否显示掩膜窗口
    std::string cfg = "Vision_Code/config/tuning.yaml";               // 默认配置文件（★全部调参都在这一个文件里★）
    std::string cfg_sensors = "Vision_Code/config/tuning.yaml";       // 相机朝向也在同一个文件里（uvc.orientation）
    std::string image_path;                                          // --image 指定的图片；非空 = 离线模式，不开相机

    for (int i = 1; i < argc; ++i) {                                 // 遍历所有命令行参数
        if (!strcmp(argv[i], "--device") && i + 1 < argc)         dev = atoi(argv[++i]);          // --device 0
        else if (!strcmp(argv[i], "--width") && i + 1 < argc)     W = atoi(argv[++i]);            // --width 640
        else if (!strcmp(argv[i], "--height") && i + 1 < argc)    H = atoi(argv[++i]);            // --height 480
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc)       FPS = atoi(argv[++i]);          // --fps 120
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc)   seconds = atoi(argv[++i]);      // --seconds 10
        else if (!strcmp(argv[i], "--headless"))                  headless = true;                // --headless
        else if (!strcmp(argv[i], "--mask"))                      show_mask = true;               // --mask 一开始就显示掩膜
        else if (!strcmp(argv[i], "--config") && i + 1 < argc)    cfg = argv[++i];                // --config 识别参数文件
        else if (!strcmp(argv[i], "--sensors") && i + 1 < argc)   cfg_sensors = argv[++i];        // --sensors 相机参数文件
        else if (!strcmp(argv[i], "--orientation") && i + 1 < argc) orient = atoi(argv[++i]);     // --orientation 0~5 画面朝向
        else if (!strcmp(argv[i], "--image") && i + 1 < argc)     image_path = argv[++i];         // --image 离线识别一张图片（不开相机）
        else if (!strcmp(argv[i], "--stream-timeout") && i + 1 < argc) stream_timeout = atoi(argv[++i]);  // 看门狗秒数
    }                                                                // for 结束

    printf("============ 传统视觉：HSV 找绿色引导灯 ============\n");  // 打印标题

    // ---- 画面朝向：命令行没指定就从 sensors.json 的 uvc.orientation 读 ----
    //   和工程 UvcCamera 用同一个配置项，保证调试窗口和 Dart 看到的画面一致
    if (orient < 0) {                                                // 命令行没给 --orientation
        cv::FileStorage fs2(cfg_sensors, cv::FileStorage::READ);      // 打开 sensors.json（只读）
        if (fs2.isOpened() && !fs2["uvc"]["orientation"].empty()) {   // 文件打开成功且节点存在
            fs2["uvc"]["orientation"] >> orient;                      // 读出来
            printf("画面朝向 = %d（来自 %s 的 uvc.orientation）\n", orient, cfg_sensors.c_str());  // 提示来源
        } else {                                                      // 读不到
            orient = 0;                                               // 默认不转
            printf("读不到 %s 的 uvc.orientation，画面朝向默认 0（不旋转）\n", cfg_sensors.c_str());  // 提示
        }                                                             // if 结束
    } else {                                                         // 命令行给了 --orientation
        printf("画面朝向 = %d（命令行指定）\n", orient);              // 提示来源
    }                                                                // if 结束

    ColorDetector::Params params;                                    // 准备参数对象
    {                                                                // 用花括号限定 fs 的作用域
        cv::FileStorage fs(cfg, cv::FileStorage::READ);              // 打开配置文件（只读）
        if (fs.isOpened()) {                                         // 打开成功
            params = ColorDetector::Params::fromJson(fs["color"]);   // 从 color 节点读参数
            printf("参数来自: %s\n", cfg.c_str());                   // 告诉用户参数是从哪来的
        } else {                                                     // 打开失败
            printf("读不到 %s，用默认参数（和 detect.json 里那套一样）\n", cfg.c_str());  // 提示用默认值
        }                                                            // if 结束
    }                                                                // 作用域结束，fs 自动关闭
    params.print();                                                  // 打印参数，确认生效值
    ColorDetector detector(params);                                  // 用这份参数创建识别器

    // ========================================================================
    // 离线模式：直接读一张图片来识别，全程不打开相机
    //   用途：相机不在手边、或者还没和飞控联调时，单独验证识别逻辑对不对。
    //   输出：终端打印检出结果，并保存 light_result.png（带框）和 light_mask.png（掩膜）
    // ========================================================================
    if (!image_path.empty()) {                                       // 命令行给了 --image 就走这条路
        printf("\n>>> 离线识别图片: %s\n", image_path.c_str());       // 提示正在处理哪张图
        cv::Mat img = cv::imread(image_path);                        // 读图（默认读成 BGR 三通道）
        if (img.empty()) {                                           // 读不到（路径错 / 格式不支持）
            printf("读不到图片 %s\n", image_path.c_str());            // 报错
            return 1;                                                // 直接退出
        }                                                            // if 结束

        cv::Mat view = applyOrientation(img, orient);                // 先做朝向修正（和相机模式用同一套规则）
        cv::Mat mask = detector.makeMask(view);                      // 取出二值掩膜 —— 能看到"填洞"之后的效果
        auto lights = detector.detect(view);                         // 跑识别（和 Dart 用的是同一份代码）

        printf("图片尺寸 %dx%d，检出 %zu 个目标\n",                   // 先报个总览
               view.cols, view.rows, lights.size());                 // 填尺寸和检出个数
        for (size_t i = 0; i < lights.size(); ++i) {                 // 逐个目标打印细节
            printf("  #%zu  面积=%.1f  中心=(%.1f,%.1f)  外接框=%.0fx%.0f  归一化得分=%.3f\n",  // 打印格式
                   i + 1,                                            // 序号（#1 就是面积最大、最终会被采用的那个）
                   lights[i].area,                                   // 轮廓面积
                   lights[i].center_point.x, lights[i].center_point.y,  // 质心坐标
                   lights[i].box.width, lights[i].box.height,        // 外接框宽高
                   lights[i].score);                                 // 面积归一化得分
        }                                                            // for 结束
        if (lights.empty()) {                                        // 一个都没识别出来
            printf("  ✗ 没识别到引导灯。下一步：\n");                 // 明确告知失败
            printf("    1) 加 --mask 弹窗看一眼掩膜，确认绿色有没有被框住\n");  // 排查建议 1
            printf("    2) 掩膜里没东西 → 调 HSV 阈值（--config 指向你自己的配置）\n");  // 排查建议 2
            printf("    3) 掩膜里有东西但没检出 → 放宽 extent_min 或 aspect_max\n");  // 排查建议 3
        } else {                                                     // 识别成功
            printf("  ✓ 识别成功。最终采用 #1：中心 (%.1f, %.1f)\n",  // 告诉用户最终用哪个
                   lights[0].center_point.x, lights[0].center_point.y);  // 填坐标
        }                                                            // if 结束

        cv::Mat shown = view.clone();                                // 复制一份来画（不动识别用的原图）
        drawResult(shown, lights);                                   // 把外接框、质心、十字准星画上去
        cv::imwrite("light_result.png", shown);                      // 存带标注的结果图
        cv::imwrite("light_mask.png", mask);                         // 存掩膜图（黑白，调阈值就看它）
        printf("已保存 light_result.png（带框）和 light_mask.png（掩膜）\n");  // 提示存到哪了

        if (show_mask) {                                             // 加了 --mask 就顺便弹窗看一眼
            cv::imshow("mask (HSV green)", mask);                    // 显示掩膜
            cv::imshow("light result", shown);                       // 显示结果图
            printf("按任意键关闭窗口...\n");                          // 提示怎么关
            cv::waitKey(0);                                          // 等按键（0 = 一直等）
            cv::destroyAllWindows();                                 // 关掉所有窗口
        }                                                            // if 结束

        return 0;                                                    // 离线模式到此结束，后面开相机的代码不执行
    }                                                                // if 结束

    printf("\n>>> 打开相机 /dev/video%d  %dx%d @%d ...\n", dev, W, H, FPS);  // 提示正在开相机
    fflush(stdout);                                                  // 立刻刷出

    cv::VideoCapture cap(dev, cv::CAP_V4L2);                         // 打开相机（强制走 Linux V4L2 后端）
    if (!cap.isOpened()) {                                           // 打不开
        printf("打不开 /dev/video%d\n", dev);                        // 报错
        return 1;                                                    // 退出
    }                                                                // if 结束
    cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));  // 设像素格式为 MJPG（高帧率必须）
    cap.set(cv::CAP_PROP_FRAME_WIDTH,  W);                           // 设宽度
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, H);                           // 设高度
    cap.set(cv::CAP_PROP_FPS,          FPS);                         // 设帧率
    cap.set(cv::CAP_PROP_BUFFERSIZE,   1);                           // 驱动缓冲降到 1 帧，减少画面延迟
    printf("实际生效 %dx%d @%.0f\n",                                 // 回读实际生效的参数
           (int)cap.get(cv::CAP_PROP_FRAME_WIDTH),                   // 实际宽度
           (int)cap.get(cv::CAP_PROP_FRAME_HEIGHT),                  // 实际高度
           cap.get(cv::CAP_PROP_FPS));                               // 实际帧率

    std::thread(streamWatchdog, dev, stream_timeout).detach();       // 启动看门狗线程（detach = 不用 join，自己跑）

    cv::Mat frame;                                                   // 存放每帧图像
    long frames = 0;                                                 // 累计读到的帧数（算帧率用）
    double t0 = (double)cv::getTickCount();                          // 起始时刻（cv 的计时器，单位是"滴答"）
    int  param_idx = 0;                                              // 当前选中的参数序号
    // 注意：这些名字会画到【图像】上，而 cv::putText 的 Hershey 字体不支持中文
    //       （画中文会变成问号），所以这里必须用英文
    const char *param_names[] = {"H_min", "H_max", "S_min", "S_max", "V_min", "V_max",   // 可调参数的名字表
                                 "aspect_max", "extent_min", "circularity", "morph_size"};  // 后四个
    const int  param_cnt = 10;                                       // 参数个数

    if (!headless) {                                                 // 有窗口模式才打印按键说明
        printf("\n按键: [ ] 选参数 | - = 调值 | o 切画面朝向 | m 掩膜 | g 打印JSON | s 存图 | r 重置 | q 退出\n");
        printf("当前选中: %s    画面朝向: %d\n", param_names[param_idx], orient);  // 显示当前选中参数和朝向
    }                                                                // if 结束

    while (true) {                                                   // 主循环：一直跑
        if (!cap.read(frame) || frame.empty()) {                     // 读一帧；失败或空图
            printf("读帧失败\n");                                    // 报一下
            break;                                                   // 跳出循环结束程序
        }                                                            // if 结束
        g_got_frame.store(true);                                     // 标记"读到过帧了"，看门狗就不会再报警
        ++frames;                                                    // 帧计数 +1

        cv::Mat view = applyOrientation(frame, orient);              // ★先做朝向修正（相机装反了画面才是正的）
        auto lights = detector.detect(view);                         // ★再用识别器找绿色引导灯（识别的是修正后的图）
        cv::Mat shown = view.clone();                                // 复制一份来画（不污染识别用的图）
        drawResult(shown, lights);                                   // 把框和质心画上去

        double el = ((double)cv::getTickCount() - t0) / cv::getTickFrequency();  // 已运行秒数
        double fps = el > 0.5 ? frames / el : 0.0;                   // 实际帧率（跑够 0.5 秒才算，避免开场抖动）
        char bar[300];                                               // 状态栏文字缓冲
        snprintf(bar, sizeof(bar),                                   // 拼状态栏文字
                 "%.1ffps  %dx%d  det=%zu  orient=%d  param=%s",     // 格式（全英文，因为画在图像上，中文会变问号）
                 fps, view.cols, view.rows, lights.size(), orient, param_names[param_idx]);  // 填值
        cv::rectangle(shown, cv::Rect(0, 0, shown.cols, 34), cv::Scalar(0, 0, 0), -1);  // 顶部画一条黑底
        cv::putText(shown, bar, cv::Point(8, 25), cv::FONT_HERSHEY_SIMPLEX, 0.6,       // 把状态栏文字画上去
                    cv::Scalar(0, 255, 255), 2);                     // 黄色

        if (headless) {                                              // -------- 无窗口模式 --------
            if ((int)el % 2 == 0 && el - (int)el < 0.05) {           // 大约每 2 秒打印一次
                printf("[%5.1fs] %.1ffps 检出%zu个", el, fps, lights.size());  // 打印时间/帧率/检出数
                if (!lights.empty()) {                               // 有目标就顺便打印目标信息
                    printf("  最大: area=%.0f 中心(%.0f,%.0f) 框%.0fx%.0f",     // 面积/中心/框尺寸
                           lights[0].area, lights[0].center_point.x, lights[0].center_point.y,  // 填值
                           lights[0].box.width, lights[0].box.height);  // 框宽高
                }                                                    // if 结束
                printf("\n");                                        // 换行
                fflush(stdout);                                      // 立刻刷出
            }                                                        // if 结束
            if (el >= seconds) break;                                // 跑够时间就退出
            continue;                                                // 无窗口没有按键处理，直接下一轮
        }                                                            // if 结束

        cv::imshow("green detect (q=quit  [ ]=param  -/==value  o=orient  g=json)", shown);  // 显示主画面
        if (show_mask) {                                             // 如果开了掩膜显示
            cv::imshow("mask (HSV green)", detector.makeMask(view)); // 显示黑白掩膜（调阈值就看它；用的是修正后的图）
        }                                                            // if 结束

        int key = cv::waitKey(1) & 0xFF;                             // 取按键（等 1ms，&0xFF 只留低 8 位）
        if (key == 'q' || key == 27) {                               // 按了 q 或 ESC
            break;                                                   // 退出主循环
        } else if (key == '[') {                                     // 按了 [ ：上一个参数
            param_idx = (param_idx + param_cnt - 1) % param_cnt;     // 环形后退（+cnt-1 再取模，避免负数）
            printf("选中参数 -> %s\n", param_names[param_idx]);      // 提示当前选中
        } else if (key == ']') {                                     // 按了 ] ：下一个参数
            param_idx = (param_idx + 1) % param_cnt;                 // 环形前进
            printf("选中参数 -> %s\n", param_names[param_idx]);      // 提示当前选中
        } else if (key == '-' || key == '=') {                       // 按了 - 或 = ：调当前参数的值
            int dir = (key == '=') ? 1 : -1;                         // = 是加，- 是减
            ColorDetector::Params &p = detector.params();            // 取参数引用（可写）
            switch (param_idx) {                                     // 按选中的是哪一项分别处理
                case 0: p.h_min = std::max(0,   std::min(179, p.h_min + dir * 2));  break;   // H_min，步进 2，范围 0~179
                case 1: p.h_max = std::max(0,   std::min(179, p.h_max + dir * 2));  break;   // H_max
                case 2: p.s_min = std::max(0,   std::min(255, p.s_min + dir * 10)); break;   // S_min，步进 10
                case 3: p.s_max = std::max(0,   std::min(255, p.s_max + dir * 10)); break;   // S_max
                case 4: p.v_min = std::max(0,   std::min(255, p.v_min + dir * 10)); break;   // V_min
                case 5: p.v_max = std::max(0,   std::min(255, p.v_max + dir * 10)); break;   // V_max
                case 6: p.aspect_max = std::max(1.0, std::min(3.0, p.aspect_max + dir * 0.05)); break;         // 长宽比，步进 0.05
                case 7: p.extent_min = std::max(0.0, std::min(1.0, p.extent_min + dir * 0.05)); break;        // 填充率，步进 0.05
                case 8: p.circularity_min = std::max(0.0, std::min(1.0, p.circularity_min + dir * 0.05)); break;  // 圆度，步进 0.05
                case 9: p.morph_size = std::max(0, std::min(21, p.morph_size + dir * 2)); break;              // 形态学核，步进 2
            }                                                        // switch 结束
            p.print();                                               // 改完打印一遍，确认新值
        } else if (key == 'm') {                                     // 按了 m ：开关掩膜窗口
            show_mask = !show_mask;                                  // 取反
            if (!show_mask) cv::destroyWindow("mask (HSV green)");   // 关掉时把窗口销毁，别留个空壳
            printf("掩膜窗口: %s\n", show_mask ? "开" : "关");        // 提示状态
        } else if (key == 'o') {                                     // 按了 o ：切换画面朝向
            orient = (orient + 1) % 6;                               // 0→1→2→3→4→5→0 循环
            const char *names[] = {"不转", "顺时针90°", "转180°", "逆时针90°", "水平镜像", "垂直镜像"};  // 模式名
            printf("画面朝向 -> %d (%s)   倒过来就按到 2，左右反就按到 4\n", orient, names[orient]);  // 提示
        } else if (key == 'g') {                                     // 按了 g ：打印 JSON 参数
            printParamsYaml(detector.params());                      // 打印出来，直接粘进 config/tuning.yaml
        } else if (key == 's') {                                     // 按了 s ：存图
            cv::imwrite("green_shot.png", shown);                    // 把带标注的图存下来
            printf("已存 green_shot.png\n");                         // 提示
        } else if (key == 'r') {                                     // 按了 r ：恢复默认参数
            detector.params() = ColorDetector::Params();             // 直接赋一个默认构造的参数对象
            printf("参数已重置\n");                                  // 提示
        }                                                            // if 结束
    }                                                                // while 主循环结束

    cap.release();                                                   // 释放相机（不然别的程序打不开）
    if (!headless) cv::destroyAllWindows();                          // 关掉所有窗口
    printf("\n结束：共 %ld 帧\n", frames);                           // 打印总帧数
    return 0;                                                        // 正常退出
}                                                                    // main 结束
