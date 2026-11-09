// ============================================================================
// UvcCamera.cpp —— UVC 免驱摄像头取图实现（逐行中文注释）
// ----------------------------------------------------------------------------
// 【取图方式】
//   cv::VideoCapture(index, cv::CAP_V4L2)  打开 /dev/videoN
//   cap_.read(frame)                       取一帧（OpenCV 内部完成 MJPG 解码 + 色彩转换）
//   不需要厂商 SDK，也不需要手动管理图像缓冲（OpenCV 负责）
//
// 【关键顺序】先设 FOURCC 再设分辨率 —— 顺序写反的话帧率经常协商不上
// 【关键参数】BUFFERSIZE 要设 3（★原来写 1 是错的，直接把帧率砍一半★）
//   实测（DF-10-200W，1280x720 MJPG，同一台机）：
//     BUFFERSIZE=1 →  88 fps      BUFFERSIZE=2 → 172 fps
//     BUFFERSIZE=3 → 173 fps      BUFFERSIZE=4 → 173 fps
//   原因：只有 1 个缓冲时，驱动必须等程序把缓冲还回去才能抓下一帧，
//   这个空档里到达的帧全部被丢掉，帧率正好掉一半。
//   延迟不用担心：识别只要 0.6~1.3ms，而一帧间隔 5.8ms，
//   程序总能把队列清空，实际延迟还是一帧左右（选 3 不选 4 就是为了留余量又不太深）。
// 【曝光/增益】见 config/tuning.yaml 的 uvc.exposure：
//   enable=0（默认）→ 完全不碰相机，和以前行为一模一样；
//   enable=1        → 开机按配置下发一次，也可运行时用 setExposureParams() 调。
//   实现走 V4L2 ioctl（与 v4l2-ctl 同一套机制），原因见 V4l2Ctrl.hpp 顶部说明。
// ============================================================================

#include "uvc/UvcCamera.hpp"
#include "uvc/V4l2Ctrl.hpp"     // 【新增】V4L2 控制项读写（纯头文件）

#include <opencv2/opencv.hpp>
#include <thread>
#include <atomic>               // 【新增】出流看门狗用的标志位
#include <chrono>
#include <cstdio>
#include <cstdlib>              // _exit()
#include <unistd.h>             // _exit()：从工作线程里立即结束进程（不做静态析构）
#include <stdexcept>            // std::runtime_error：相机打不开时抛出，让 main 明确报错

#include "function/function.hpp"
#include "global/Config.hpp"

// ============================================================================
// 小工具：安全读取配置项
//   cv::FileStorage 在键不存在时直接 >> 会抛异常，所以统一用这个包一层：
//   节点为空（键不存在）就保持原来的默认值不动。
// ============================================================================
static void readCfgIfPresent(const cv::FileNode &parent, const char *key, int &out) {
    cv::FileNode n = parent[key];
    if (!n.empty()) {
        n >> out;
    }
}

// ============================================================================
// 构造函数：只保存引用，不做耗时操作
// ============================================================================
UvcCamera::UvcCamera(SerialPort &sp) : sensors::BaseCamera(sp) {
    // 从 tuning.yaml 读配置（与海康版本共用同一个配置文件，方便切换）
    //   注意：cv::FileStorage 取值时若键不存在会抛异常，所以这里逐个兜底
    if (!J_SENSORS.config_["uvc"].empty()) {
        J_SENSORS.config_["uvc"]["device"]      >> this->device_index_;   // /dev/videoN
        J_SENSORS.config_["uvc"]["width"]       >> this->req_width_;      // 请求宽
        J_SENSORS.config_["uvc"]["height"]      >> this->req_height_;     // 请求高
        J_SENSORS.config_["uvc"]["fps"]         >> this->req_fps_;        // 请求帧率
        J_SENSORS.config_["uvc"]["orientation"] >> this->orientation_;    // 朝向修正
        int mjpg = 1;
        J_SENSORS.config_["uvc"]["mjpg"]        >> mjpg;
        this->use_mjpg_ = (mjpg != 0);

        // ---- 【新增】读曝光配置 ----
        //   整个 exposure 节点不存在时，下面几行全部跳过，
        //   exp_enable_ 保持 0 → 相机行为和加这个功能之前完全一样。
        cv::FileNode exp = J_SENSORS.config_["uvc"]["exposure"];
        if (!exp.empty()) {
            readCfgIfPresent(exp, "enable",     this->exp_enable_);
            readCfgIfPresent(exp, "auto",       this->exp_auto_);
            readCfgIfPresent(exp, "value",      this->exp_value_);
            readCfgIfPresent(exp, "gain",       this->exp_gain_);
            readCfgIfPresent(exp, "brightness", this->exp_brightness_);
            readCfgIfPresent(exp, "backlight",  this->exp_backlight_);
            printf("[UvcCamera] 读到曝光配置: enable=%d auto=%d value=%d gain=%d brightness=%d backlight=%d\n",
                   this->exp_enable_, this->exp_auto_, this->exp_value_,
                   this->exp_gain_, this->exp_brightness_, this->exp_backlight_);
        } else {
            printf("[UvcCamera] tuning.yaml 里没有 uvc.exposure 节点 → 不调整曝光\n");
        }
    }
}

UvcCamera::~UvcCamera() {
    if (this->opened_) {
        this->cap_.release();       // 释放摄像头，否则别的程序打开会失败
    }
    // 【新增】关掉控制通道句柄，别留着占用设备
    if (this->ctrl_fd_ >= 0) {
        v4l2ctrl::closeDevice(this->ctrl_fd_);
        this->ctrl_fd_ = -1;
    }
}

// ============================================================================
// cameraInit()：打开设备 + 配置格式/分辨率/帧率
// ============================================================================
void UvcCamera::cameraInit() {
    // ① 打开 /dev/videoN。
    //    第二个参数一定要写 cv::CAP_V4L2，否则 OpenCV 可能去试 GStreamer 后端，
    //    报一些莫名其妙的错。
    this->cap_.open(this->device_index_, cv::CAP_V4L2);
    if (!this->cap_.isOpened()) {
        // 【重要】这里必须抛异常，不能只是 return。
        //   原来的写法是"打印一行错误就返回"，程序会继续往下跑，取图拿到空 Mat，
        //   最后在 letterBox 的 cv::resize 里对一个空图崩溃：
        //       OpenCV error: (-215:Assertion failed) !ssize.empty() in function 'resize'
        //   抛异常让 main 明确报错退出，用户一眼就知道是相机没接好。
        printf("\n[UvcCamera] ❌ 打不开 /dev/video%d\n", this->device_index_);
        printf("  排查步骤：\n");
        printf("    1) 摄像头是否已在 VMware 里挂给虚拟机？\n");
        printf("       VMware 菜单 → 虚拟机 → 可移动设备 → 找到摄像头 → 连接(断开与主机连接)\n");
        printf("    2) 设备号对不对？  执行： v4l2-ctl --list-devices\n");
        printf("       如果列出的不是 video%d，改 config/tuning.yaml 里的 uvc.device\n", this->device_index_);
        printf("    3) 有没有被别的程序占用？  同一个摄像头只能被一个程序打开\n");
        printf("    4) 权限问题：  ls -l /dev/video*   必要时 sudo chmod 666 /dev/video*\n");
        throw std::runtime_error("UvcCamera: 无法打开摄像头设备");
    }

    // ② 先设格式再设分辨率 —— 顺序很重要！
    //    1080p 想跑到 60/120 帧，UVC 必须用 MJPG（YUYV 未压缩带宽根本不够）。
    //    FOURCC 用 MJPG，OpenCV 会请摄像头内部做 JPEG 解码。
    if (this->use_mjpg_) {
        this->cap_.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
    } else {
        this->cap_.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('Y', 'U', 'Y', 'V'));
    }

    // ③ 设置期望的分辨率和帧率
    //    ★ 建议不要设 1920x1080：一是 UVC 在 1080p 下实际大多只有 30fps，
    //      二是把整张 1080p 图送进网络，letterBox 的缩放比例和训练时差太多，
    //      检测精度会掉。这台机器上先用 640x480 起步。
    this->cap_.set(cv::CAP_PROP_FRAME_WIDTH,  this->req_width_);
    this->cap_.set(cv::CAP_PROP_FRAME_HEIGHT, this->req_height_);
    this->cap_.set(cv::CAP_PROP_FPS,          this->req_fps_);

    // ④ 驱动内部缓冲个数 —— ★这里是帧率的关键，别改回 1★
    //    原来写 1（注释说"降延迟"），实测那会把帧率从 173fps 砍到 88fps：
    //    单缓冲时驱动要等程序还回缓冲才能抓下一帧，空档里的帧全丢。
    //    设 3 实测和 2/4/8 一样快，且比 4 浅一点，所以取 3。
    this->cap_.set(cv::CAP_PROP_BUFFERSIZE, 3);

    // ⑤ 回读实际生效的值 —— 摄像头不一定接受你请求的参数！
    //    这一步必须打印出来看，否则会出现"我设了 120fps 怎么只有 30"的困惑。
    int   real_w   = (int)this->cap_.get(cv::CAP_PROP_FRAME_WIDTH);
    int   real_h   = (int)this->cap_.get(cv::CAP_PROP_FRAME_HEIGHT);
    double real_fps = this->cap_.get(cv::CAP_PROP_FPS);
    printf("[UvcCamera] 打开 /dev/video%d 成功\n", this->device_index_);
    printf("[UvcCamera] 请求 %dx%d @%.0f fps，实际生效 %dx%d @%.1f fps\n",
           this->req_width_, this->req_height_, (double)this->req_fps_,
           real_w, real_h, real_fps);

    // ⑥ 下发曝光/增益配置
    //    放在最后做，因为部分驱动在「改分辨率/格式」时会把控制项复位，
    //    先设曝光再改格式的话会被冲掉。
    //    ★ 只在这里设一次（开流之前）。视频流跑起来之后不再碰控制项 ——
    //      流运行中改曝光可能逼 uvcvideo 停流重开，重启失败流就死了。
    this->applyExposureConfig();

    this->opened_ = true;

    // ⑦ 【新增】校验「真的能读到帧」
    //    open() 成功 != 能出帧。不做这一步的话，相机不出流时程序会一路带着
    //    空图像往下跑，最后崩在别处，报错完全看不懂。
    this->waitFirstFrame();
}

// ============================================================================
// getImage()：取一帧，组装成 FromData 推给检测线程
//   流程：收电控指令 → 读一帧 → 朝向修正 → 填入 message_
// ============================================================================
void UvcCamera::getImage() {
    if (!this->opened_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return;
    }

    // ① 先收电控下发的指令（与海康版一致：把最新指令挂到这一帧上）
    //    原海康版是 while 阻塞直到收到；这里保持同样语义。
    //    【核对过，这个位置没问题】：OpenCV 的 V4L2 后端是「第一次 read() 时才
    //    STREAMON」（惰性开流），所以在这里等串口并不会让相机空转灌帧。
    //
    //    【新增】无飞控测试模式：串口没打开、且配置里 serialport.optional=1 时，
    //    不再死等飞控，直接用一个默认指令继续跑 —— 这样能单独测视觉识别链路。
    //    默认指令取 mode=1（开自瞄）、number=1（第1发），
    //    特意避开 number<1 && mode==0 那种组合，因为 Track 遇到它回去写标定文件。
    CarData car_data;
    if (this->sp_.isOpen()) {
        // 有飞控：保持原逻辑 —— 阻塞等飞控下发指令
        while (!this->sp_.receive(car_data)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    } else {
        // 串口没打开：能走到这里说明是测试模式（否则 main 已经报错退出了）
        car_data.header = 0xA5;     // 帧头
        car_data.mode   = 1;        // 1 = 开自瞄
        car_data.status = 0;        // 状态：0
        car_data.number = 1;        // 第 1 发飞镖
        car_data.dune   = 0;        // 0 = 完全展开
        car_data.CRC8   = 0;        // 无飞控就没有 CRC（只有 receive 才校验，这里用不到）
        car_data.CRC16  = 0;
    }
    this->message_.cd = car_data;

    // ② 读一帧。read() 返回的是 BGR 三通道（OpenCV 已帮我们做完解码+色彩转换）
    cv::Mat frame;
    if (!this->cap_.read(frame) || frame.empty()) {
        // ====================================================================
        // 掉线自恢复 —— 【已改成带刹车的版本】
        // --------------------------------------------------------------------
        // 原来的写法是「一失败就立刻 release()+open() 重开设备」，而且失败路径上
        // 一句 sleep 都没有。相机出流本来就不稳时会变成：
        //     读失败 → 重开 → 又失败 → 再重开 → ...
        // 持续反复地对 USB 设备做 STREAMOFF/STREAMON，反而可能把相机搞进坏状态。
        // 现在改成三级处理：
        //   1) 少量失败先容忍（UVC 偶发丢帧是正常的，不该动设备）
        //   2) 连续失败够多才重连，重连前先退避 500ms，别冲击设备
        //   3) 连续重连都救不回来 → 明确报错退出，而不是无限重试
        // ====================================================================
        this->read_fail_cnt_++;

        if (this->read_fail_cnt_ < READ_FAIL_TOLERANCE) {
            // 第 1 级：先容忍，短暂让出 CPU，不做任何设备操作
            if (this->read_fail_cnt_ == 1) {
                printf("[UvcCamera] ⚠️ 读帧失败，先容忍几次看看能不能自愈\n");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            return;
        }

        // 第 2 级：连续失败太多，认为掉线了，重连一次
        //   注意计数放在重连【之前】：只要「重连了却仍然读不到帧」，计数就往上走，
        //   读到帧才清零（见本函数末尾）。这样即使 open() 每次都能成功、
        //   但就是不出帧，也不会无限重连下去。
        if (++this->reconnect_fail_cnt_ > RECONNECT_LIMIT) {
            printf("\n[UvcCamera] ❌ 连续重连 %d 次都读不到帧，放弃。\n",
                   this->reconnect_fail_cnt_ - 1);
            this->printStreamTroubleshooting();
            fflush(stdout);
            _exit(1);       // 取图线程里必须用 _exit（见 main.cpp 的说明）
        }
        printf("[UvcCamera] ❌ 连续 %d 次读不到帧，尝试重连（第 %d/%d 次）\n",
               this->read_fail_cnt_, this->reconnect_fail_cnt_, RECONNECT_LIMIT);

        this->cap_.release();
        this->opened_ = false;
        this->read_fail_cnt_ = 0;

        // 退避：给 USB/驱动时间清理上一次的流，别紧接着就重开
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        this->cap_.open(this->device_index_, cv::CAP_V4L2);
        if (this->cap_.isOpened()) {
            if (this->use_mjpg_) {
                this->cap_.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
            } else {
                this->cap_.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('Y', 'U', 'Y', 'V'));
            }
            this->cap_.set(cv::CAP_PROP_FRAME_WIDTH,  this->req_width_);
            this->cap_.set(cv::CAP_PROP_FRAME_HEIGHT, this->req_height_);
            this->cap_.set(cv::CAP_PROP_FPS,          this->req_fps_);
            this->cap_.set(cv::CAP_PROP_BUFFERSIZE,   3);   // ★和 cameraInit 保持一致，别写回 1★

            // ★ 这里【故意不】重新下发曝光设置 ★
            //   原因：uvcvideo 在「视频流正在跑」的时候修改曝光相关控制，
            //   可能需要停止并重启视频流（帧间隔要重算）。重启一旦失败，流就死了，
            //   表现就是「重连完还是读不到帧」。所以曝光只在 cameraInit() 里、
            //   开流之前设置一次；重连后如果发现曝光变了，宁可让它保持驱动默认。
            this->opened_ = true;
            printf("[UvcCamera] 已重新打开设备（曝光设置不回写，避免打断视频流）\n");
        } else {
            printf("[UvcCamera] 重新打开设备失败\n");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        return;
    }

    // 读到帧了：把两个失败计数清零
    this->read_fail_cnt_      = 0;
    this->reconnect_fail_cnt_ = 0;

    // ③ 时间戳取"取到帧"的当前时刻
    //    （UVC 驱动不给硬件曝光时间，只能用读取时刻近似，与海康版的处理一致）
    this->message_.timestamp = function::getNowTimestamp();

    // ④ 【朝向修正】—— 这一步必须按你的实际安装方式定，不能照抄
    //    海康版原来是：transpose + flip(-1) + flip(1)，实测等价于"旋转90°+镜像"，
    //    那是针对它当时的机械安装角度做的。换成 UVC 摄像头后，安装角度变了，
    //    所以这里用 orientation_ 参数控制，你在调试窗口里看哪个方向对就填哪个：
    //       0 = 不动
    //       1 = 旋转 90°  顺时针   （等价原海康那三步的效果，先试这个）
    //       2 = 旋转 180°
    //       3 = 旋转 270° 顺时针
    //       4 = 只做水平镜像
    //       5 = 只做垂直镜像
    switch (this->orientation_) {
        case 1:
            cv::rotate(frame, frame, cv::ROTATE_90_CLOCKWISE);
            break;
        case 2:
            cv::rotate(frame, frame, cv::ROTATE_180);
            break;
        case 3:
            cv::rotate(frame, frame, cv::ROTATE_90_COUNTERCLOCKWISE);
            break;
        case 4:
            cv::flip(frame, frame, 1);      // 1 = 水平镜像
            break;
        case 5:
            cv::flip(frame, frame, 0);      // 0 = 垂直镜像
            break;
        default:
            break;                          // 0 = 保持原样
    }

    // ⑤ 存进 message_（clone 一下，避免和 VideoCapture 的内部缓冲共享内存）
    this->message_.img = frame.clone();
}

// ============================================================================
// 【新增】出流看门狗用的标志位（文件作用域，只有一个相机实例）
// ============================================================================
static std::atomic<bool> g_uvc_got_frame{false};

// ============================================================================
// 【新增】printStreamTroubleshooting()：相机出不了流时的统一排查提示
//   多处共用，避免文案不一致
// ============================================================================
void UvcCamera::printStreamTroubleshooting() {
    printf("\n");
    printf("==================================================================\n");
    printf("相机打开了，但读不到画面 —— 相机没有出流\n");
    printf("==================================================================\n");
    printf("按顺序试，通常第 1 步就好：\n\n");
    printf(" 1) 自动复位 USB 相机（不用重启虚拟机）：\n");
    printf("      cd ~/Desktop/feibiao/beifen/verify\n");
    printf("      SUDOPW=你的密码 ./recover_camera.sh\n\n");
    printf(" 2) 不行就在 VMware 里手动重连摄像头：\n");
    printf("      虚拟机 → 可移动设备 → USB 2.0 Camera → 断开 → 等3秒 → 再连接\n\n");
    printf(" 3) 还不行：把相机从电脑上【物理拔下来】等 10 秒再插回去\n");
    printf("      （重启虚拟机 != 相机断电；相机内部的坏状态只有断电才能清）\n\n");
    printf(" 4) 最彻底：重启虚拟机\n\n");
    printf("【本相机已知情况】\n");
    printf("  - 640x480 要 120fps 只能用 MJPG（YUYV 最高 30fps）\n");
    printf("  - 若 MJPG 完全读不到、而 YUYV 正常，多为相机 MJPG 通路故障\n");
    printf("  - 当前配置：/dev/video%d  %dx%d @%d  %s\n",
           this->device_index_, this->req_width_, this->req_height_, this->req_fps_,
           this->use_mjpg_ ? "MJPG" : "YUYV");
    printf("==================================================================\n");
}

// ============================================================================
// 【新增】waitFirstFrame()：启动时校验「相机真的能出帧」
// ----------------------------------------------------------------------------
// 为什么必须做：
//   ① OpenCV 的 cap_.read() 在相机不出流时会【无限阻塞】（内部 select 没有超时）
//   ② open() 成功完全不代表能出帧
//   原来 cameraInit() 只检查 open()，结果相机不出流时程序会一路带着空图像往下跑，
//   最后崩在别的地方，报错信息完全看不懂。
//
// 做法：起一个看门狗线程兜底 —— 超过 STREAM_CHECK_SEC 秒还没第一帧，就打印
// 排查步骤并退出。用 _exit() 而不是 exit()（取图线程里 exit 会触发静态析构抢资源）。
// ============================================================================
bool UvcCamera::waitFirstFrame() {
    g_uvc_got_frame.store(false);

    std::thread watchdog([this]() {
        for (int i = 0; i < STREAM_CHECK_SEC * 10; ++i) {
            if (g_uvc_got_frame.load()) {
                return;                     // 已经出流，看门狗正常退场
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        printf("\n[UvcCamera] ❌ 打开相机后 %d 秒内一帧都读不到\n", STREAM_CHECK_SEC);
        this->printStreamTroubleshooting();
        fflush(stdout);
        _exit(1);
    });
    watchdog.detach();

    printf("[UvcCamera] 正在等待相机出流（最多 %d 秒）...\n", STREAM_CHECK_SEC);
    fflush(stdout);

    cv::Mat probe;
    for (int i = 0; i < 30; ++i) {
        if (this->cap_.read(probe) && !probe.empty()) {
            g_uvc_got_frame.store(true);
            printf("[UvcCamera] ✅ 相机出流正常，首帧 %dx%d\n", probe.cols, probe.rows);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    printf("\n[UvcCamera] ❌ 连续 30 次都读不到帧\n");
    this->printStreamTroubleshooting();
    fflush(stdout);
    _exit(1);
    return false;       // 走不到这里
}

// ============================================================================
// 【新增】ensureCtrlFd()：惰性打开「控制通道」
//   返回 fd；失败返回 -1 并打印排查提示。
//   为什么要单独一个 fd：控制项（曝光/增益）走 V4L2 ioctl，而取流是 OpenCV
//   在管。V4L2 支持同一个设备被多次 open，所以再开一个句柄专门发控制命令，
//   两互不干扰（v4l2-ctl 一边预览一边调曝光就是这么做的）。
// ============================================================================
int UvcCamera::ensureCtrlFd() {
    if (this->ctrl_fd_ >= 0) {
        return this->ctrl_fd_;                  // 已经开好了，直接用
    }
    this->ctrl_fd_ = v4l2ctrl::openDevice(this->device_index_);
    if (this->ctrl_fd_ < 0) {
        printf("[UvcCamera] ⚠️ 打不开 /dev/video%d 的控制通道，曝光设置跳过\n",
               this->device_index_);
        printf("   可以退而用命令行调（同样效果，只是不写进配置）：\n");
        printf("     v4l2-ctl -d /dev/video%d --list-ctrls\n", this->device_index_);
        printf("     v4l2-ctl -d /dev/video%d --set-ctrl=auto_exposure=1\n", this->device_index_);
        printf("     v4l2-ctl -d /dev/video%d --set-ctrl=exposure_time_absolute=100\n", this->device_index_);
    }
    return this->ctrl_fd_;
}

// ============================================================================
// 【新增】applyExposureConfig()：按 tuning.yaml 的配置下发一次曝光/增益
//   enable = 0 时直接返回，一个字节都不写 —— 保证「不开启就绝不影响现状」
// ============================================================================
void UvcCamera::applyExposureConfig() {
    if (this->exp_enable_ == 0) {
        printf("[UvcCamera] 曝光调整未启用（tuning.yaml 的 uvc.exposure.enable=0）→ 保持相机默认\n");
        return;
    }

    int fd = this->ensureCtrlFd();
    if (fd < 0) {
        return;
    }

    printf("\n[UvcCamera] ===== 开始设置曝光（uvc.exposure.enable=1）=====\n");
    printf("  设置之前，这台相机支持的项与范围：\n");
    v4l2ctrl::printAllRelevant(fd);

    // ① 先切自动/手动模式，再写曝光值（顺序很重要，见 V4l2Ctrl.hpp 说明）
    //   ★★ 绝对不要动 V4L2_CID_EXPOSURE_AUTO_PRIORITY！★★
    //   实测教训：把它设成 0 之后，本相机 640x480 MJPG 取流会彻底起不来
    //   （该分辨率只支持 0.008s 这一档帧间隔，驱动按曝光值反推出来的间隔不在
    //    支持列表里 → 设置帧率失败 → 一帧都读不到，程序看起来像卡死，而且会
    //    "粘住"，之后什么曝光值都出不了流，必须重连 USB 相机才能恢复）。
    if (this->exp_auto_ >= 0) {
        if (v4l2ctrl::set(fd, V4L2_CID_EXPOSURE_AUTO, this->exp_auto_)) {
            printf("  ✓ auto_exposure -> %d  (%s)\n", this->exp_auto_,
                   this->exp_auto_ == V4L2_EXPOSURE_MANUAL ? "手动" : "自动");
        } else {
            printf("  ✗ auto_exposure 写入失败（相机不支持？）\n");
        }
    }

    // ② 曝光绝对值。只有在「手动模式」下才会被驱动采纳，所以这里判一下。
    if (this->exp_value_ >= 0) {
        if (this->exp_auto_ == V4L2_EXPOSURE_MANUAL) {
            // 【已移除】原来这里有个"低于 157 就拒绝写入"的保护。
            //   那是基于一次被污染的实验加的（当时 120fps 那一档本来就是坏的，
            //   误以为是曝光值低导致的）。后来实测证明：曝光 100 在 800x600@60 下
            //   跑 8 秒 480 帧完全正常，所以这条保护是错的，会挡住正常调参。
            //   现在直接写，写不进去打印提示即可。
            if (v4l2ctrl::set(fd, V4L2_CID_EXPOSURE_ABSOLUTE, this->exp_value_)) {
                printf("  ✓ exposure_time_absolute -> %d\n", this->exp_value_);
            } else {
                printf("  ✗ exposure_time_absolute 写入失败\n");
            }
        } else {
            printf("  ⚠️ 配置里 auto=%d 不是手动模式(1)，驱动会忽略曝光值 %d\n",
                   this->exp_auto_, this->exp_value_);
            printf("     想用手动曝光请把 tuning.yaml 的 uvc.exposure.auto 改成 1\n");
        }
    }

    // ③ 增益 / 亮度 / 背光补偿（<0 表示不设置）
    if (this->exp_gain_ >= 0) {
        printf("  %s gain -> %d\n",
               v4l2ctrl::set(fd, V4L2_CID_GAIN, this->exp_gain_) ? "✓" : "✗", this->exp_gain_);
    }
    if (this->exp_brightness_ >= 0) {
        printf("  %s brightness -> %d\n",
               v4l2ctrl::set(fd, V4L2_CID_BRIGHTNESS, this->exp_brightness_) ? "✓" : "✗",
               this->exp_brightness_);
    }
    if (this->exp_backlight_ >= 0) {
        printf("  %s backlight_compensation -> %d\n",
               v4l2ctrl::set(fd, V4L2_CID_BACKLIGHT_COMPENSATION, this->exp_backlight_) ? "✓" : "✗",
               this->exp_backlight_);
    }

    // ④ 读回真实生效的值 —— 必须确认，因为驱动可能把你的值夹到范围内
    int a = -1, v = -1, g = -1;
    this->readExposureParams(a, v, g);
    printf("  读回实际生效: auto_exposure=%d  exposure_time_absolute=%d  gain=%d\n", a, v, g);
    printf("[UvcCamera] ===== 曝光设置结束 =====\n\n");
}

// ============================================================================
// 【新增】setExposureParams()：运行时改曝光/增益（参数给 <0 表示该项不动）
//   典型用法（调试窗口里按方向键实时调，立刻能看到画面明暗变化）：
//       cam.setExposureParams(1, 80, -1);   // 切手动 + 曝光 80，增益不动
// ============================================================================
bool UvcCamera::setExposureParams(int auto_mode, int value, int gain) {
    int fd = this->ensureCtrlFd();
    if (fd < 0) {
        return false;
    }

    bool done = false;

    // ① 先切模式：必须写成功，否则下面的曝光值一定被驱动忽略
    //   ★ 不碰 EXPOSURE_AUTO_PRIORITY —— 设成 0 会让 640x480 MJPG 取流起不来，
    //     详见本文件 applyExposureConfig() 里的说明。
    if (auto_mode >= 0) {
        done |= v4l2ctrl::set(fd, V4L2_CID_EXPOSURE_AUTO, auto_mode);
    }

    // ② 再写曝光绝对值（只在手动模式下有意义）
    //   【已移除】原来这里也有"低于 157 就拒绝"，同样是被污染实验导致的误判，去掉了
    if (value >= 0) {
        done |= v4l2ctrl::set(fd, V4L2_CID_EXPOSURE_ABSOLUTE, value);
    }

    // ③ 增益
    if (gain >= 0) {
        done |= v4l2ctrl::set(fd, V4L2_CID_GAIN, gain);
    }

    return done;
}

// ============================================================================
// 【新增】readExposureParams()：读回相机当前真实值
//   调参时用它确认「我以为设上去了」和「实际生效了」是不是一回事。
// ============================================================================
void UvcCamera::readExposureParams(int &auto_mode, int &value, int &gain) {
    auto_mode = -1;
    value     = -1;
    gain      = -1;

    int fd = this->ensureCtrlFd();
    if (fd < 0) {
        return;
    }
    v4l2ctrl::get(fd, V4L2_CID_EXPOSURE_AUTO,     auto_mode);
    v4l2ctrl::get(fd, V4L2_CID_EXPOSURE_ABSOLUTE, value);
    v4l2ctrl::get(fd, V4L2_CID_GAIN,              gain);
}

// ============================================================================
// 【新增】printCameraControls()：打印相机支持的全部相关控制项和范围
// ============================================================================
void UvcCamera::printCameraControls() {
    int fd = this->ensureCtrlFd();
    if (fd < 0) {
        return;
    }
    printf("\n[UvcCamera] /dev/video%d 可调项一览（手动调参就照这个填）：\n",
           this->device_index_);
    v4l2ctrl::printAllRelevant(fd);
    printf("\n");
}

// ============================================================================
// 以下两个是 BaseCamera 要求的纯虚接口，UVC 用不上，留空实现
// ============================================================================
void UvcCamera::cameraModeChange(int now_mode, int my_color) {
    // 海康版用它在红/蓝方之间切换曝光；UVC 摄像头没有这个接口。
    // 【新增】现在改成读 tuning.yaml 的 uvc.exposure 配置，
    //   要现场临时调可以调 setExposureParams()（见本文件上面的实现）。
    (void)now_mode;
    (void)my_color;
}

void UvcCamera::assertSuccess(uint32_t status) {
    (void)status;
}

// ============================================================================
// 【附】曝光调参速查
// ----------------------------------------------------------------------------
// 1) 想看这台相机到底能调什么、范围多少：
//      ./Dart 启动时会自动打印（需要 uvc.exposure.enable=1）
//      或直接命令行： v4l2-ctl -d /dev/video0 --list-ctrls
//
// 2) 想现场试不同曝光（不用改配置、不用重编译）：
//      v4l2-ctl -d /dev/video0 --set-ctrl=auto_exposure=1
//      v4l2-ctl -d /dev/video0 --set-ctrl=exposure_time_absolute=80
//
// 3) 试出满意的值后再写进 config/tuning.yaml：
//      "exposure": { "enable": 1, "auto": 1, "value": 80, "gain": 10 }
//    ★ auto 必须写 1（手动），否则 exposure.value 会被驱动忽略。
//    ★ exposure_time_absolute 单位是 100 微秒，值越小画面越暗、噪声越少。
//
// 4) 调完想恢复相机默认（自动曝光）：
//      v4l2-ctl -d /dev/video0 --set-ctrl=auto_exposure=3
// ============================================================================
