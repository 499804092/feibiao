#include "threadpool/ThreadPool.hpp"
#include "lockfree/LockFree.hpp"

// ============================================================================
// 相机后端：UVC 免驱摄像头
// ----------------------------------------------------------------------------
// 「亚博智能 USB 摄像头 1080P」这类标准 UVC 摄像头，走内核 uvcvideo 驱动
// + OpenCV 的 V4L2 后端，读 /dev/videoN 即可，不需要任何厂商 SDK。
// 取图实现在 src/sensors/src/uvc/UvcCamera.cpp。
// ============================================================================
#include "uvc/UvcCamera.hpp"

#include "Detect.hpp"
#include "Track.hpp"
#include "DetectView.hpp"
// 【改动】原来这里是 #include "Mouse.hpp"。
//   Mouse 是原版比赛代码留下的"用鼠标手动画裁剪框"的调试工具，和识别无关，
//   而且它把识别结果丢掉了，导致运行 Dart 时看不到任何识别框。
//   现在换成 DetectView：真正把识别结果画到窗口上。

#include "global/Config.hpp"
// 【新增】为了读 tuning.yaml 里的 view.enable（要不要启动识别窗口线程）

#include "function/function.hpp"

// ============================================================================
// 【本任务改动】录像模块改成可选
// ----------------------------------------------------------------------------
// 原来这里无条件 #define CAPTURE，导致必须用到 Capture.hpp → mcap → FlatBuffers，
// 而 FlatBuffers/lz4/zstd 在目标机器上不一定齐。现在与 CMake 的
// ENABLE_CAPTURE 选项联动：
//   cmake .. -DENABLE_CAPTURE=OFF    （默认）→ 不编译录像线程，也不需要 FlatBuffers
//   cmake .. -DENABLE_CAPTURE=ON            → 编译并定义 ENABLE_CAPTURE，启用录像
// 相机取图 / 目标检测 / 跟踪 / 解算 / 串口下发 这些主线功能与它无关。
// ============================================================================
#ifdef ENABLE_CAPTURE
#ifndef MCAP_IMPLEMENTATION
#define MCAP_IMPLEMENTATION
#endif
#include "Capture.hpp"
#define CAPTURE
#endif

//#define DUMP
#ifdef DUMP
#include "dump/dump.hpp"
#endif


int main() {

    // 【新增】让 stdout 变成"按行刷新"
    //   为什么必须加：把程序输出重定向到文件或管道时（例如 ./Dart > log.txt），
    //   stdout 会变成【全缓冲】（攒满 4KB 才写一次）。程序被 Ctrl+C / timeout
    //   结束时，缓冲区里没刷出来的日志就【直接丢了】—— 排查时看到的现象就是
    //   "日志打到一半就没了，像是卡住了"，其实程序还在正常跑。
    //   加这一句后每行都会立刻落盘，排查不会再被误导。
    setvbuf(stdout, nullptr, _IOLBF, 0);

    ThreadPool t_p;
    LfStack<FromData> fd_msgs;
#ifndef DUMP
    LfStack<ImageInfo> image_info_stack;                             // 识别线程 → 跟踪线程
    LfStack<ImageInfo> view_info_stack;                              // 识别线程 → 显示线程（原来这条是给 mouse 的）
    SerialPort sp;

    // ========================================================================
    // 【新增】串口是给飞控用的：没打开就明确报错退出，别带着坏句柄一路跑下去
    //   原来 SerialPort::init() 失败时静默返回，后面 receive() 只会刷
    //   "ioctl: Bad file descriptor"，让人完全看不懂问题在哪。
    //   例外：配置里 serialport.optional = 1（无飞控测试模式）时允许继续，
    //        此时取图线程会用默认指令，视觉识别链路可以单独测。
    //   注意这里还在主线程、线程池还没启动，所以直接 return 是安全的。
    // ========================================================================
    if (!sp.isOpen() && !sp.isOptional()) {
        printf("\n[main] 串口没打开，程序退出。\n");
        printf("[main] 两个选择：\n");
        printf("  1) 接上飞控（STM32），确认 tuning.yaml 里 serialport.port 写的是实际设备名；\n");
        printf("  2) 只想单独测视觉识别，就把 tuning.yaml 的 serialport.optional 改成 1。\n");
        fflush(stdout);
        return 1;
    }

#ifdef CAPTURE
    LfStack<CaptureInfo> capture_stack;
#endif
    auto receive = [&]() -> void {
        // 取图线程：用 UVC 免驱摄像头，读 /dev/videoN（设备号在 tuning.yaml 的 uvc.device）
        // 下游（检测/跟踪/解算/串口下发）与相机后端无关，完全不受影响。
        UvcCamera cam(sp);

        // 相机打不开时 cameraInit() 会抛异常。这里必须拦住并让整个程序退出，
        // 否则会带着空图像一路跑下去，最后崩在 cv::resize 里（报错信息完全看不懂）。
        //
        // 【注意】这里用的是 _exit() 而不是 exit()：
        //   这是在 ThreadPool 的工作线程里，此时其它线程（检测/跟踪/鼠标）还在跑。
        //   用 exit() 会触发静态对象析构（ReJson 的 inline 全局变量等），
        //   与其它线程抢资源 → 直接段错误。_exit() 立即结束进程、不做清理，最安全。
        //   这里本来就是要"立刻退出"，不需要任何清理。
        try {
            cam.cameraInit();                   // 打开设备 + 配置 MJPG/分辨率/帧率
        } catch (const std::exception &e) {
            printf("\n[main] 取图线程启动失败：%s\n", e.what());
            printf("[main] 请按上面的排查步骤接好摄像头后重新运行。程序退出。\n");
            fflush(stdout);
            _exit(1);                           // 立即退出，不走静态析构
        }

        while (1) {
            cam.refreshImage(fd_msgs);          // 取一帧 → 连同电控指令推给检测线程
        }
    };

    auto detect = [&]() -> void {
        Detect detect("debug");

        while (1) {
            detect.detect(
                    fd_msgs,
                    image_info_stack);                               // 【改动】去掉了 view_info_stack 这个参数
        }                                                            //   显示线程现在由下面 track 线程负责喂
    };


    auto track = [&]() -> void {
        Track tracker(sp, "debug");
        tracker.setViewStack(view_info_stack);                       // 【新增】把显示队列交给 Track，
                                                                     //   Track 每帧算完角度后会推一帧给显示线程
        while (1) {
#ifdef CAPTURE
            tracker.update(image_info_stack, capture_stack);
#else
            tracker.update(image_info_stack);
#endif
        }
    };


    // ========================================================================
    // 显示线程：把识别结果显示出来（识别框 + 解算角度 + 状态栏 + 掩膜）
    // ------------------------------------------------------------------------
    // 【为什么必须单独开一个线程，而不是在识别线程或跟踪线程里直接 imshow】
    //   1) 线程之间用的 LfStack 是【无界队列】：LockFree.inl 里 push() 每次都
    //      new 一个节点，队列满了不阻塞、只增长。如果在识别线程里做 imshow，
    //      识别会变慢，而取图线程照旧全速 push，积压的帧（800x600x3 ≈ 1.4MB/帧）
    //      会把内存吃光。
    //   2) 更要命的是 OpenCV 的 HighGUI（imshow/waitKey）【不是线程安全的】。
    //      以前 Track 和 mouse 各自开一个窗口、各自调 imshow，是踩在雷上跑。
    //      现在全工程只有这一个线程碰界面，其余线程只往队列里丢数据。
    //
    // 【数据从哪来】Track 每帧算完 yaw/pitch/距离后，
    //   把「原图 + 原始识别结果 + 电控指令 + 解算结果」一起推给这个队列。
    //
    // 【开关】config/tuning.yaml 的 view.enable
    //   1 = 开（调试用，弹窗口）
    //   0 = 关（比赛用，一个窗口都不弹，也没有任何显示开销）
    // ========================================================================
    auto view = [&]() -> void {
        DetectView v;                                                // 建显示对象（构造里会弹窗口）
        while (1) {
            v.run(view_info_stack);                                  // 循环：取一帧结果 → 画 → 显示 → 处理按键
        }
    };

#ifdef CAPTURE
    auto capture_func = [&]() -> void {
        std::cout << "capture thread start." << std::endl;
        Capture cap("/Dart",
                    "Vision",
                    "foxglove.CaptureInformation",
                    "../config/autogenerated_flatbuffers/CaptureInformation.bfbs",
                    "8UC3");
        cap.setMacpPath(function::getBagPath());
        cap.init();
        int frame_count;
        J_CAPTURE.config_["frame_cnt"] >> frame_count;
        while(true) {
            std::optional<CaptureInfo> opt_value = capture_stack.pop();

            if (!opt_value.has_value() || opt_value.value().cd.dune == 1 || !opt_value.value().cd.mode) {
                continue;
            }

            cap.update(opt_value.value());

            if (!(--frame_count)) {
                cap.finish();
                cap.setMacpPath(function::getBagPath());
                cap.init();
                J_CAPTURE.config_["frame_cnt"] >> frame_count;
            }
        }
    };

#endif

    t_p.addTask(receive);
    t_p.addTask(detect);
    t_p.addTask(track);

    // 显示线程按配置决定开不开。
    //   它现在负责两路输出，任意一路打开就需要这个线程：
    //     ① view.enable —— 本地窗口（有桌面的机器上用，比如虚拟机）
    //     ② web.enable  —— 网页推流（板子上只有终端时用，浏览器看画面）
    //   ★板子上的推荐配置★：view.enable=0（没桌面，imshow 会报错）+ web.enable=1
    int view_enable = 1;                                             // 本地窗口开关，默认开（方便调试）
    if (!J_DETECT.config_["view"]["enable"].empty()) {               // 配置里写了就用配置里的值
        J_DETECT.config_["view"]["enable"] >> view_enable;           // 读出来
    }                                                                // if 结束

    int web_enable = 0;                                              // 网页推流开关，默认关
    if (!J_DETECT.config_["web"]["enable"].empty()) {                // 配置里写了就用配置里的值
        J_DETECT.config_["web"]["enable"] >> web_enable;             // 读出来
    }                                                                // if 结束

    if (view_enable || web_enable) {                                 // 任意一路开着就启动显示线程
        t_p.addTask(view);
    } else {                                                         // 两路都关着
        printf("[main] view.enable 和 web.enable 都是 0，不启动显示线程\n");
    }                                                                // if 结束
#ifdef CAPTURE
    t_p.addTask(capture_func);
#endif
#else
    auto dump = [&]() -> void {
        std::string directory;
        std::string name;
        std::string type;
        J_CAPTURE.config_["mcap"]["directory"] >> directory;
        J_CAPTURE.config_["mcap"]["name"] >> name;
        J_CAPTURE.config_["mcap"]["type"] >> type;
        std::cout << directory + name + type << std::endl;
        std::ifstream input(directory + name + type, std::ios::binary);
        mcap::FileStreamReader dataSource{input};
        DumpMessages(name, dataSource, fd_msgs);
    };
    t_p.addTask(dump);
#endif
    t_p.run();

    return 0;
}