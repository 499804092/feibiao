// ============================================================================
// calib_capture_usb.cpp —— 用【普通 USB 摄像头】采集相机标定图片
// ----------------------------------------------------------------------------
// 【它干什么】
//   打开 UVC 免驱摄像头（就是工程用的那个），实时找棋盘格标定板，
//   按 s 存一张图到输出文件夹；凑够十几张不同角度/距离的照片之后，
//   换 calib_camera_usb 去算相机内参。
//
// 【为什么要单独写一个】
//   工程原来那套标定程序（spr_vision_26/calibration/capture.cpp）绑死了两样东西：
//     ① 海康工业相机（要 /opt/MVS 下的 MVS SDK）
//     ② can0 上的云台四元数（io::Gimbal）
//   普通 USB 摄像头在这套代码里根本打不开（io/camera.cpp 只认 hikrobot / mindvision），
//   虚拟机上也没有 CAN。这个程序把这两个依赖全部去掉，只留"相机 + 标定板"，
//   所以在虚拟机、笔记本上就能标。
//
// 【和原版流程的对应关系】
//   原版：  capture                  →  calibrate_camera
//   这里：  calib_capture_usb        →  calib_camera_usb
//   棋盘格规格、求解器参数（CALIB_FIX_K3、迭代 100 次）都跟原版一致，
//   差别只有两点：① 加了亚像素角点细化（标定精度明显更好）
//                 ② 结果直接打印成能粘进 Vision_Code/config/tuning.yaml 的片段
//
// 【编译】见同目录 build_calib.sh，或者手敲：
//   g++ -std=c++17 -O2 calib_capture_usb.cpp -o calib_capture_usb $(pkg-config --cflags --libs opencv4)
//
// 【用法】
//   ./calib_capture_usb                        # 相机参数默认从 tuning.yaml 读
//   ./calib_capture_usb --auto                 # 自动采集：拿着板子慢慢晃，摆得够开就自动存
//   ./calib_capture_usb --cols 11 --rows 8 --square 25   # 就是下面那块板子的规格（默认值）
//   ./calib_capture_usb --out mydata --width 800 --height 600 --fps 60
//
// 【按键】
//   s        手动存一张（不检查角度是否重复，按了就存）
//   u        撤销：删掉最后存的那张
//   a        开关自动采集
//   q / ESC  退出
//
// 【标定板长什么样】
//   默认按【大凡视觉 Q12-340-25】这块板子设置：内角点 11 列 × 8 行，方格 25mm。
//     ⚠️ 数的是"内角点"不是方格。Q12-340-25 的规格：
//        外形 340×260mm，图案区 300×225mm，方格边长 25mm
//        横向 300/25 = 12 个方格，纵向 225/25 = 9 个方格
//        内角点 = (12-1) × (9-1) = 11 × 8 = 88 个
//   换别的板子就用 --cols/--rows/--square 告诉程序实际规格。
//   ★ 只有 cols/rows 必须填对，填错会【一张都检测不到】且不报错；
//     square 填错【不影响内参】—— 3D 点整体缩放等价于坐标系缩放，fx/fy/cx/cy 不变。
//
// 【拍照要求（决定了标定准不准）】
//   · 板子占画面 1/3 以上，四边都要留白，别贴边、别出画
//   · 正对 1~2 张，然后左倾、右倾、上仰、下俯、近、远各来几张
//   · 15~25 张最合适。全是同一个角度拍的，拍 100 张也没用
//   · 手别抖，晃动时画面糊了角点就找不到
//
// 【安全设计】
//   1) 出流看门狗：和 uvc_color_view 一样，超时读不到帧就打印排查步骤并退出，
//      不会假死在 cv::read() 里。
//   2) 全程【不写 V4L2 控制寄存器】（不碰曝光/增益/亮度）——
//      实测写这些会打断正在跑的取流，而标定用自动曝光完全够用，
//      所以这里故意不提供曝光选项。要压曝光请用主程序 Dart 或改 tuning.yaml。
//   3) 退出时一定 cap.release()，不会把相机占住。
// ============================================================================

#include <opencv2/opencv.hpp>   // OpenCV：相机、找棋盘格、画图、窗口

#include <algorithm>            // std::sort
#include <atomic>               // std::atomic：看门狗标志位
#include <chrono>               // sleep_for
#include <cmath>                // sqrt / fabs
#include <cstdio>               // printf / snprintf
#include <cstdlib>              // atoi / atof
#include <cstring>              // strcmp
#include <string>               // std::string
#include <sys/stat.h>           // mkdir / stat：建输出文件夹
#include <thread>               // std::thread：看门狗线程
#include <unistd.h>             // _exit：从看门狗线程里立刻结束进程
#include <vector>               // std::vector

// ---------------------------------------------------------------------------
// 出流看门狗：相机不出流时不让程序假死
//   cv::read() 在相机不出流时会无限阻塞，没有这个的话终端一直没输出，看起来像卡死。
// ---------------------------------------------------------------------------
static std::atomic<bool> g_got_frame{false};                          // 有没有成功读到过帧

static void streamWatchdog(int dev, int timeout_s) {                  // 跑在独立线程里
    for (int i = 0; i < timeout_s * 10; ++i) {                        // 每 100ms 查一次
        if (g_got_frame.load()) return;                               // 读到过帧了，看门狗退场
        std::this_thread::sleep_for(std::chrono::milliseconds(100));  // 睡 100ms
    }
    printf("\n");                                                     // 换行
    printf("==================================================================\n");
    printf("相机 /dev/video%d 打开了，但 %d 秒内一帧都没读到 —— 没有出流\n", dev, timeout_s);
    printf("==================================================================\n");
    printf("按顺序试：\n");
    printf("  1) 换一种分辨率/帧率（本相机 640x480 只有 120fps 一档，时好时坏）\n");
    printf("     例： ./calib_capture_usb --width 800 --height 600 --fps 60\n");
    printf("  2) 把相机拔下来等 10 秒再插上（真断电，重启虚拟机不算）\n");
    printf("  3) 确认没有别的程序占着相机：  pgrep -a -f calib_\n");
    printf("==================================================================\n");
    fflush(stdout);
    _exit(1);                                                         // 立刻结束进程
}

// ---------------------------------------------------------------------------
// ensureDir() —— 建输出文件夹（存在就直接返回）
// ---------------------------------------------------------------------------
static bool ensureDir(const std::string & path) {
    struct stat st;                                                   // 用来查这个路径在不在
    if (stat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode);     // 已存在，且确实是目录
    return mkdir(path.c_str(), 0777) == 0;                            // 不存在就建
}

// ---------------------------------------------------------------------------
// applyOrientation() —— 画面朝向修正（和工程 UvcCamera / uvc_color_view 完全同一套规则）
//   相机装反了画面就是倒的。这里先修正再存图，保证【标定用的图】和
//   【Dart 里识别用的图】是同一个朝向，否则内参会和实际用的画面不匹配。
//     0 = 不动   1 = 顺时针 90°   2 = 转 180°
//     3 = 逆时针 90°   4 = 水平镜像   5 = 垂直镜像
// ---------------------------------------------------------------------------
static cv::Mat applyOrientation(const cv::Mat & src, int mode) {
    cv::Mat out;                                                      // 存放结果
    switch (mode) {
        case 1: cv::rotate(src, out, cv::ROTATE_90_CLOCKWISE);         break;  // 顺时针 90°
        case 2: cv::rotate(src, out, cv::ROTATE_180);                  break;  // 转 180°
        case 3: cv::rotate(src, out, cv::ROTATE_90_COUNTERCLOCKWISE);  break;  // 逆时针 90°
        case 4: cv::flip(src, out, 1);                                 break;  // 水平镜像
        case 5: cv::flip(src, out, 0);                                 break;  // 垂直镜像
        default: out = src.clone();                                    break;  // 0 或非法值：原样
    }
    return out;
}

// ---------------------------------------------------------------------------
// loadUvcDefaults() —— 从 Vision_Code/config/tuning.yaml 读相机参数
//   和主程序 Dart 用同一个配置项，所以标定的分辨率就是实际跑的分辨率。
//   ⚠️ 内参和分辨率绑死：800x600 标出来的 fx 不能直接用在 640x480 上。
// ---------------------------------------------------------------------------
static void loadUvcDefaults(
    const std::string & cfg, int & dev, int & W, int & H, int & FPS, bool & mjpg, int & orient) {
    cv::FileStorage fs(cfg, cv::FileStorage::READ);                   // 只读打开
    if (!fs.isOpened()) {                                             // 打不开就用命令行/内置默认值
        printf("读不到 %s，相机参数用默认值（可用命令行覆盖）\n", cfg.c_str());
        return;
    }
    cv::FileNode node = fs["uvc"];                                    // 相机参数都在 uvc 节点下
    if (node.empty()) {                                               // 没有 uvc 节点
        printf("%s 里没有 uvc 节点，相机参数用默认值\n", cfg.c_str());
        return;
    }
    if (!node["device"].empty())      node["device"]      >> dev;      // /dev/videoN 的 N
    if (!node["width"].empty())       node["width"]       >> W;        // 采集宽度
    if (!node["height"].empty())      node["height"]      >> H;        // 采集高度
    if (!node["fps"].empty())         node["fps"]         >> FPS;      // 请求帧率
    if (!node["orientation"].empty()) node["orientation"] >> orient;   // 画面朝向
    if (!node["mjpg"].empty()) {                                      // 1=MJPG 0=YUYV
        int m = 1;
        node["mjpg"] >> m;
        mjpg = (m != 0);
    }
    printf("相机参数来自 %s 的 uvc 节点\n", cfg.c_str());
}

// ---------------------------------------------------------------------------
// findBoard() —— 找棋盘格
//   返回 true 表示找齐了 board.width × board.height 个内角点。
//   use_sb = 用 findChessboardCornersSB（OpenCV 4 自带的更稳的检测器），
//            默认用经典检测器，和原版标定程序一致；识别不稳时再加 --sb 1。
// ---------------------------------------------------------------------------
static bool findBoard(
    const cv::Mat & gray, const cv::Size & board, bool use_sb,
    std::vector<cv::Point2f> & corners) {
    corners.clear();                                                  // 先清空
    bool ok = false;
#if CV_VERSION_MAJOR >= 4                                                // SB 检测器是 OpenCV 4 才有的
    if (use_sb) {
        ok = cv::findChessboardCornersSB(gray, board, corners);       // 更强的检测器
    } else
#endif
    {
        // FAST_CHECK 让实时预览快很多；ADAPTIVE_THRESH + NORMALIZE_IMAGE 和原版一致
        ok = cv::findChessboardCorners(
            gray, board, corners,
            cv::CALIB_CB_ADAPTIVE_THRESH + cv::CALIB_CB_NORMALIZE_IMAGE + cv::CALIB_CB_FAST_CHECK);
    }
    return ok && static_cast<int>(corners.size()) == board.width * board.height;
}

// ---------------------------------------------------------------------------
// 描述"这一张拍的角度"的特征量，自动采集时用来判断是不是和之前存过的太像
//   cx, cy  板子中心在画面上的位置（像素）
//   scale   板子的"半径"（角点到中心的平均距离，像素）→ 反映远近
//   tilt    板子的长宽比 → 反映倾斜角度
// ---------------------------------------------------------------------------
struct ViewFeat {
    double cx = 0, cy = 0, scale = 0, tilt = 0;
};

static ViewFeat viewFeat(const std::vector<cv::Point2f> & c, const cv::Size & board) {
    ViewFeat f;                                                       // 结果
    cv::Point2f ctr(0, 0);                                            // 角点重心
    for (const auto & p : c) ctr += p;                                // 累加
    ctr *= 1.0f / static_cast<float>(c.size());                       // 取平均
    double m = 0;                                                     // 平均半径
    for (const auto & p : c) m += cv::norm(p - ctr);                  // 累加距离
    m /= static_cast<double>(c.size());                               // 取平均
    f.cx = ctr.x;
    f.cy = ctr.y;
    f.scale = m;
    // 用"上边长度 / 左边长度"当倾斜代理量：正对时约等于 1，斜看时偏离 1
    double top = cv::norm(c[board.width - 1] - c[0]);
    double left = cv::norm(c[(board.height - 1) * board.width] - c[0]);
    f.tilt = (top > 1e-6) ? (left / top) : 1.0;
    return f;
}

// 两个视角差多少：位置差 + 远近差 + 倾斜差，都是归一化后相加，>= min_diff 就算"够不一样"
static double featDiff(const ViewFeat & a, const ViewFeat & b, const cv::Size & img) {
    double dx = (a.cx - b.cx) / img.width;                            // 横向位移（按宽度归一化）
    double dy = (a.cy - b.cy) / img.height;                           // 纵向位移（按高度归一化）
    double diag = std::sqrt(static_cast<double>(img.width) * img.width +
                            static_cast<double>(img.height) * img.height);
    double ds = (a.scale - b.scale) / diag;                           // 远近变化（按对角线归一化）
    double dt = a.tilt - b.tilt;                                      // 倾斜变化
    return std::sqrt(dx * dx + dy * dy) + std::fabs(ds) * 2.0 + std::fabs(dt) * 0.6;
}

// ---------------------------------------------------------------------------
// saveView() —— 存一张图（文件名就是序号，calib_camera_usb 按序号读）
// ---------------------------------------------------------------------------
static bool saveView(const std::string & dir, int idx, const cv::Mat & img, int jpeg_q) {
    char name[1024];                                                  // 拼路径用的缓冲
    snprintf(name, sizeof(name), "%s/%d.jpg", dir.c_str(), idx);
    std::vector<int> params{cv::IMWRITE_JPEG_QUALITY, jpeg_q};        // 高质量，减少压缩伪影
    return cv::imwrite(name, img, params);
}

// ---------------------------------------------------------------------------
// removeView() —— 撤销：把最后存的那张删掉
// ---------------------------------------------------------------------------
static void removeView(const std::string & dir, int idx) {
    char name[1024];
    snprintf(name, sizeof(name), "%s/%d.jpg", dir.c_str(), idx);
    remove(name);                                                     // 删掉，删不掉也无所谓
}

// ---------------------------------------------------------------------------
// resolveConfig() —— 找 tuning.yaml 的真实位置
//   默认路径写的是 "Vision_Code/config/tuning.yaml"，那是【在 feibiao/ 下跑】时的
//   相对路径。但编译出来的可执行文件在 feibiao/calib_usb/ 里，大家自然会在那里面
//   ./calib_capture_usb，这时相对路径就对不上 —— 而读不到配置【不会报错】，
//   只会静默退回内置默认值，万一分辨率不一样就会把内参标到错的分辨率上。
//   所以这里按几个常见的相对位置挨个试。
// ---------------------------------------------------------------------------
static std::string resolveConfig(const std::string & given, bool from_cli) {
    if (from_cli) return given;                                       // 命令行明确给了就用它，不猜
    const char * cands[] = {
        "Vision_Code/config/tuning.yaml",                             // 在 feibiao/ 下跑
        "../Vision_Code/config/tuning.yaml",                          // 在 feibiao/calib_usb/ 下跑（最常见）
        "../../Vision_Code/config/tuning.yaml",                       // 在更深的子目录里跑
        "config/tuning.yaml",                                         // 在 Vision_Code/ 下跑
    };
    for (const char * c : cands) {                                    // 挨个试能不能打开
        FILE * f = fopen(c, "r");
        if (f) {
            fclose(f);
            return std::string(c);                                    // 找到就用这个
        }
    }
    return given;                                                     // 都没找到，原样返回（后面会提示读不到）
}

// ---------------------------------------------------------------------------
// 用法说明
// ---------------------------------------------------------------------------
static void printUsage() {
    printf("用法: ./calib_capture_usb [选项]\n");
    printf("  --out <目录>        存图的目录（默认 calib_out）\n");
    printf("  --cols <n>          棋盘格内角点列数（默认 11 = Q12-340-25 横着拿）\n");
    printf("  --rows <n>          棋盘格内角点行数（默认 8）\n");
    printf("  --square <mm>       方格边长 mm（默认 25，只影响外参尺度）\n");
    printf("  --device <n>        相机设备号 /dev/videoN（默认从 tuning.yaml 读）\n");
    printf("  --width/--height    采集分辨率（默认从 tuning.yaml 读）\n");
    printf("  --fps <n>           请求帧率（默认从 tuning.yaml 读）\n");
    printf("  --orientation <0-5> 画面朝向（默认从 tuning.yaml 读）\n");
    printf("  --config <路径>     tuning.yaml 路径（默认自动在几个常见位置找）\n");
    printf("  --auto              启动就打开自动采集\n");
    printf("  --max <n>           自动采集最多存几张（默认 30）\n");
    printf("  --min-diff <x>      自动采集的『角度要够不一样』阈值（默认 0.12）\n");
    printf("  --interval <秒>     自动采集两张之间的最小间隔（默认 1.0）\n");
    printf("  --quality <1-100>   存图 JPEG 质量（默认 95）\n");
    printf("  --stream-timeout <秒> 等出流最多等多少秒（默认 10）\n");
    printf("  --sb 1              用 SB 检测器（识别不稳时加）\n");
    printf("  --help              显示这段说明\n");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char ** argv) {
    // ---- 默认值 ----
    std::string out_dir = "calib_out";                                // 存图目录
    std::string cfg = "Vision_Code/config/tuning.yaml";               // 相机参数来源
    int  dev = 0, W = 800, H = 600, FPS = 60;                         // 相机参数
    bool mjpg = true;                                                 // 是否用 MJPG
    int  orient = 2;                                                  // 画面朝向
    int  cols = 11, rows = 8;                                         // 棋盘格内角点数（大凡 Q12-340-25）
    double square = 25.0;                                             // 方格边长 mm
    bool auto_mode = false;                                           // 自动采集
    int  max_saves = 30;                                              // 自动采集上限
    double min_diff = 0.12;                                           // "够不一样"阈值
    double interval = 1.0;                                            // 自动采集间隔（秒）
    int  quality = 95;                                                // 存图质量
    int  stream_timeout = 10;                                         // 出流看门狗秒数
    bool use_sb = false;                                              // 是否用 SB 检测器
    bool cfg_from_cli = false;                                        // 命令行是否指定过分辨率/设备
    bool cfg_path_cli = false;                                        // 命令行是否明确指定了 --config

    // ---- 解析命令行 ----
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char * def) -> std::string {             // 取下一个参数（越界就用默认值）
            return (i + 1 < argc) ? std::string(argv[++i]) : std::string(def);
        };
        if (a == "--out")                 out_dir = next("calib_out");
        else if (a == "--config")       { cfg = next("Vision_Code/config/tuning.yaml"); cfg_path_cli = true; }
        else if (a == "--cols")           cols = atoi(next("11").c_str());
        else if (a == "--rows")           rows = atoi(next("8").c_str());
        else if (a == "--square")         square = atof(next("25").c_str());
        else if (a == "--device")       { dev = atoi(next("0").c_str());   cfg_from_cli = true; }
        else if (a == "--width")        { W = atoi(next("800").c_str());   cfg_from_cli = true; }
        else if (a == "--height")       { H = atoi(next("600").c_str());   cfg_from_cli = true; }
        else if (a == "--fps")          { FPS = atoi(next("60").c_str());  cfg_from_cli = true; }
        else if (a == "--orientation")  { orient = atoi(next("2").c_str()); }
        else if (a == "--auto")           auto_mode = true;
        else if (a == "--max")            max_saves = atoi(next("30").c_str());
        else if (a == "--min-diff")       min_diff = atof(next("0.12").c_str());
        else if (a == "--interval")       interval = atof(next("1.0").c_str());
        else if (a == "--quality")        quality = atoi(next("95").c_str());
        else if (a == "--stream-timeout") stream_timeout = atoi(next("10").c_str());
        else if (a == "--sb")             use_sb = (atoi(next("1").c_str()) != 0);
        else if (a == "--help" || a == "-h") { printUsage(); return 0; }
        else { printf("不认识的参数: %s（用 --help 看说明）\n", a.c_str()); return 1; }
    }

    printf("============ USB 相机标定图片采集 ============\n");

    // ---- 找出 tuning.yaml 的真实位置（在不同目录下启动相对路径不一样）----
    cfg = resolveConfig(cfg, cfg_path_cli);

    // ---- 命令行没指定相机参数时，从 tuning.yaml 读，保证和实际跑的分辨率一致 ----
    if (!cfg_from_cli) loadUvcDefaults(cfg, dev, W, H, FPS, mjpg, orient);

    // ---- 桌面检查：没有 DISPLAY 就没有窗口，这个程序必须看着画面拍 ----
    if (getenv("DISPLAY") == nullptr || getenv("DISPLAY")[0] == '\0') {
        printf("✗ 当前环境没有图形界面（DISPLAY 为空），拍不了标定图。\n");
        printf("  这个程序要一边看画面一边摆板子，必须在有桌面的机器上跑：\n");
        printf("    · 虚拟机桌面里开终端，或者用 VS Code Remote-SSH 的图形转发\n");
        printf("    · 纯 SSH 命令行不行\n");
        return 1;
    }

    // ---- 检查标定板规格 ----
    if (cols < 3 || rows < 3 || cols * rows > 2000) {
        printf("✗ 棋盘格规格不合理：%d 列 × %d 行内角点\n", cols, rows);
        return 1;
    }

    // ---- 建输出目录 ----
    if (!ensureDir(out_dir)) {
        printf("✗ 建不了输出目录 %s\n", out_dir.c_str());
        return 1;
    }

    printf("标定板：内角点 %d 列 × %d 行（= %d × %d 个方格），方格边长 %.1f mm\n",
           cols, rows, cols + 1, rows + 1, square);
    printf("输出目录：%s\n", out_dir.c_str());
    printf("画面朝向：%d\n", orient);

    // ---- 开相机（和 uvc_color_view 完全一样的开法）----
    printf("\n>>> 打开相机 /dev/video%d  %dx%d @%d ...\n", dev, W, H, FPS);
    fflush(stdout);

    cv::VideoCapture cap(dev, cv::CAP_V4L2);                          // 强制走 V4L2 后端
    if (!cap.isOpened()) {
        printf("打不开 /dev/video%d\n", dev);
        return 1;
    }
    if (mjpg) cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));  // 高帧率必须 MJPG
    cap.set(cv::CAP_PROP_FRAME_WIDTH,  W);                            // 宽
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, H);                            // 高
    cap.set(cv::CAP_PROP_FPS,          FPS);                          // 帧率
    // 缓冲个数用 3 —— ★写 1 会把帧率砍一半，和 UvcCamera.cpp 是同一个坑★
    //   单缓冲时驱动必须等程序把缓冲还回去才能抓下一帧，空档里的帧全被丢掉。
    //   实测 1280x720 MJPG：BUFFERSIZE=1 → 88fps，=3 → 173fps。
    //   延迟不用担心：找一次棋盘格只要 1.3ms，而一帧间隔 5.8ms，队列始终是空的。
    cap.set(cv::CAP_PROP_BUFFERSIZE,   3);
    printf("实际生效 %dx%d @%.0f\n",
           (int)cap.get(cv::CAP_PROP_FRAME_WIDTH),
           (int)cap.get(cv::CAP_PROP_FRAME_HEIGHT),
           cap.get(cv::CAP_PROP_FPS));

    std::thread(streamWatchdog, dev, stream_timeout).detach();        // 启动看门狗

    // ---- 主循环 ----
    const cv::Size board(cols, rows);                                 // findChessboardCorners 要的是(列,行)
    std::vector<ViewFeat> saved_feats;                                // 已存图的视角特征
    int saved = 0;                                                    // 已存张数
    long frames = 0;                                                  // 总帧数（算帧率）
    double t0 = (double)cv::getTickCount();                           // 起始时刻
    double last_save_t = -1e9;                                        // 上次存图时刻
    cv::Mat frame, shown, gray;                                       // 复用缓冲

    printf("\n按键: s=存一张   u=撤销   a=%s自动采集   q=退出\n",
           auto_mode ? "关掉" : "打开");
    printf("★ 建议按 a 打开自动采集：拿着板子慢慢换角度，程序自己挑不同视角存 ★\n\n");

    while (true) {
        if (!cap.read(frame) || frame.empty()) {                     // 读一帧
            printf("读帧失败\n");
            break;
        }
        g_got_frame.store(true);                                     // 告诉看门狗"出流了"
        ++frames;

        cv::Mat view = applyOrientation(frame, orient);              // 朝向修正（存的就是修正后的图）
        cv::cvtColor(view, gray, cv::COLOR_BGR2GRAY);                // 找角点用灰度

        std::vector<cv::Point2f> corners;                            // 角点
        bool found = findBoard(gray, board, use_sb, corners);        // 找棋盘格

        // ---- 自动采集 ----
        double now = ((double)cv::getTickCount() - t0) / cv::getTickFrequency();
        if (auto_mode && found && saved < max_saves && now - last_save_t >= interval) {
            ViewFeat f = viewFeat(corners, board);                   // 算这一帧的视角特征
            bool diverse = true;                                     // 默认认为够不一样
            for (const auto & prev : saved_feats) {                  // 和已存的每一张比
                if (featDiff(f, prev, view.size()) < min_diff) {     // 太像了
                    diverse = false;
                    break;
                }
            }
            if (diverse) {                                           // 够不一样就存
                ++saved;
                if (saveView(out_dir, saved, view, quality)) {
                    saved_feats.push_back(f);                        // 记下这一张的视角
                    last_save_t = now;
                    printf("自动存 #%d/%d   板心(%.0f,%.0f) 半径%.0f 倾斜比%.2f\n",
                           saved, max_saves, f.cx, f.cy, f.scale, f.tilt);
                } else {
                    --saved;                                         // 存失败就回退计数
                    printf("存图失败（目录 %s 可写吗？）\n", out_dir.c_str());
                }
            }
        }

        // ---- 画到窗口上 ----
        shown = view.clone();
        if (found) {
            cv::drawChessboardCorners(shown, board, corners, true);   // 把角点画出来
            cv::putText(shown, "BOARD OK", cv::Point(10, 30),
                        cv::FONT_HERSHEY_SIMPLEX, 0.9, cv::Scalar(0, 255, 0), 2);
        }
        if (saved > 0 && !saved_feats.empty()) {                      // 把已存图的板心点出来
            for (const auto & f : saved_feats) {
                cv::circle(shown, cv::Point((int)f.cx, (int)f.cy), 4,
                           cv::Scalar(0, 200, 255), -1);
            }
        }

        double el = ((double)cv::getTickCount() - t0) / cv::getTickFrequency();
        double fps = el > 0.5 ? frames / el : 0.0;                    // 实际帧率
        char bar[256];
        snprintf(bar, sizeof(bar), "saved=%d/%d  auto=%s  %.1ffps  %dx%d",
                 saved, auto_mode ? max_saves : 0, auto_mode ? "ON" : "OFF",
                 fps, view.cols, view.rows);
        cv::rectangle(shown, cv::Rect(0, 0, shown.cols, 34), cv::Scalar(0, 0, 0), -1);  // 黑底
        cv::putText(shown, bar, cv::Point(8, 25),
                    cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 255, 255), 2);

        if (saved >= max_saves && auto_mode) {                        // 存够了就提示
            cv::putText(shown, "ENOUGH - press q", cv::Point(10, shown.rows - 20),
                        cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(0, 255, 255), 2);
        }

        cv::imshow("calib capture (s=save u=undo a=auto q=quit)", shown);

        int key = cv::waitKey(1) & 0xFF;                             // 取按键
        if (key == 'q' || key == 27) break;                          // 退出
        if (key == 's') {                                            // 手动存
            ++saved;
            if (saveView(out_dir, saved, view, quality)) {
                saved_feats.push_back(found ? viewFeat(corners, board) : ViewFeat());
                printf("手动存 #%d   %s\n", saved, found ? "（板子已识别）" : "⚠ 这帧没识别到板子！");
            } else {
                --saved;
                printf("存图失败\n");
            }
        } else if (key == 'u') {                                     // 撤销
            if (saved > 0) {
                removeView(out_dir, saved);                          // 删文件
                --saved;
                if (!saved_feats.empty()) saved_feats.pop_back();    // 同步特征表
                printf("已撤销，剩 %d 张\n", saved);
            } else {
                printf("没有可撤销的\n");
            }
        } else if (key == 'a') {                                     // 开关自动采集
            auto_mode = !auto_mode;
            printf("自动采集 -> %s\n", auto_mode ? "开" : "关");
        }
    }

    cap.release();                                                   // 释放相机，别占着
    cv::destroyAllWindows();

    // ---- 结束统计 ----
    printf("\n结束：共 %ld 帧，存了 %d 张到 %s\n", frames, saved, out_dir.c_str());
    if (saved < 8) {
        printf("⚠ 只有 %d 张，太少了。标定至少要 8 张，建议 15~25 张。\n", saved);
    } else {
        printf("→ 下一步：  ./calib_camera_usb --in %s --cols %d --rows %d --square %.1f\n",
               out_dir.c_str(), cols, rows, square);
    }
    return 0;
}
