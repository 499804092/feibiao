// ============================================================================
// DetectView.cpp —— 识别结果显示窗口（实现在头文件里已经说明用途）
// ============================================================================

#include "DetectView.hpp"                                            // 自己的头文件

#include <chrono>                                                    // std::chrono::milliseconds（不开窗口时的休眠）
#include <cmath>                                                     // std::lround：算十字准星位置时四舍五入
#include <cstdio>                                                    // printf / snprintf：打印和拼状态栏文字
#include <cstdlib>                                                   // ::getenv：判断有没有图形界面（开发板适配）
#include <cstring>                                                   // strcmp 之类的字符串处理
#include <sys/stat.h>                                                // mkdir：建 snapshots 目录
#include <thread>                                                    // std::this_thread::sleep_for
#include <unistd.h>                                                  // _exit：从工作线程里立即退出进程

#include "global/Config.hpp"                                         // J_DETECT：读 config/tuning.yaml

// ============================================================================
// 构造函数 —— 读配置、准备掩膜识别器、建窗口
// ============================================================================
DetectView::DetectView() {                                           // 无参构造
    // ---------- 读 view 节点 ----------
    //   view.enable = 要不要开【本地窗口】（板子上没桌面，必须设 0）
    //   view.mask   = 启动时掩膜窗口是否已经打开
    //   ★注意★ "要不要启动这个线程"是在 main.cpp 里判断的（view.enable 或 web.enable
    //     任意一个打开就要启动），这里只管"线程起来之后具体开哪些输出"。
    if (!J_DETECT.config_["view"]["enable"].empty()) {               // 配置里有 enable 就按它来
        int en = 1;                                                  // 默认开
        J_DETECT.config_["view"]["enable"] >> en;                    // 读出来
        this->show_window_ = (en != 0);                              // 非 0 = 开窗口
    }                                                                // if 结束
    if (!J_DETECT.config_["view"]["mask"].empty()) {                 // 配置里有这个节点才读（没有就用默认值）
        int mask = 0;                                                // 临时变量
        J_DETECT.config_["view"]["mask"] >> mask;                    // 读出来
        this->show_mask_ = (mask != 0);                              // 非 0 就是开
    }                                                                // if 结束
    // ---------- 【开发板适配①】自动判断这台机器有没有图形界面 ----------
    //   开发板（Radxa ZERO 3W 等）平时在 SSH 里操作，没有桌面 —— 这时候
    //   cv::namedWindow() 会抛 cv::Exception。而这个构造函数是在【显示线程】里跑的，
    //   异常没人接就会 std::terminate，【整个飞镖程序当场死掉】。
    //   也就是说：在板子上忘了把 view.enable 改成 0，程序直接崩。
    //
    //   所以这里主动判断一次，把"有没有桌面"这件事自动化：
    //     · DISPLAY 环境变量为空（SSH 登录就是这种情况）→ 自动不开窗口
    //     · 下面建窗口那一步还额外用 try/catch 兜底（双保险）
    //   这样同一个二进制在虚拟机（有桌面）和板子（没桌面）上都能直接跑，不用改配置。
    if (this->show_window_) {                                        // 配置说想开窗口
        const char *disp = ::getenv("DISPLAY");                      // 查图形界面环境变量
        if (disp == nullptr || disp[0] == '\0') {                    // 空 = 这台机器没有图形界面
            this->show_window_ = false;                              //   自动关掉本地窗口
            printf("[DetectView] 这台机器没有图形界面（DISPLAY 为空）→ 自动关闭本地窗口\n");
            printf("[DetectView]    这是开发板的正常情况，不是错误。\n");
            printf("[DetectView]    要看画面：用浏览器打开下面 [Web] 打印的网址\n");
            fflush(stdout);                                          // 立刻刷出
        }                                                            // if 结束
    }                                                                // if 结束

    if (!this->show_window_) {                                       // 不开窗口
        this->show_mask_ = false;                                    //   掩膜窗口也开不了，一并关掉
    }                                                                // if 结束

    // ---------- 读 web 节点，需要的话起 MJPEG 推流服务器 ----------
    //   板子上只有终端、没有桌面时，就靠它在电脑浏览器上看画面。
    //   它和本地窗口【互不影响】，可以只开推流不开窗口。
    int  web_enable = 0;                                             // 推流开关
    int  web_port   = 8080;                                          // 端口
    int  web_fps    = 15;                                            // 推流帧率上限
    int  web_quality= 70;                                            // JPEG 质量
    int  web_width  = 0;                                             // 输出宽度（0=不缩放）
    cv::FileNode wn = J_DETECT.config_["web"];                       // 取 web 节点
    if (!wn.empty()) {                                               // 配置里有这个节点
        if (!wn["enable"].empty())  wn["enable"]  >> web_enable;     // 读开关
        if (!wn["port"].empty())    wn["port"]    >> web_port;       // 读端口
        if (!wn["fps"].empty())     wn["fps"]     >> web_fps;        // 读帧率
        if (!wn["quality"].empty()) wn["quality"] >> web_quality;    // 读质量
        if (!wn["width"].empty())   wn["width"]   >> web_width;      // 读输出宽度
    }                                                                // if 结束

    if (web_enable != 0) {                                           // 打开了推流
        this->web_ = new MjpegServer(web_port, web_quality,          // 建服务器对象
                                     (double)web_fps, web_width);    //   传入帧率上限和输出宽度
        if (!this->web_->start()) {                                  // 启动失败（端口被占等）
            printf("[Web] ⚠️ 推流启动失败，程序继续跑（只是看不到网页画面）\n");  // 提示但不影响识别
            fflush(stdout);                                          // 立刻刷出
            delete this->web_;                                       // 释放
            this->web_ = nullptr;                                    // 置空，后面会跳过推流
        }                                                            // if 结束
    }                                                                // if 结束

    // ---------- 准备一个"只用来算掩膜"的识别器 ----------
    //   为什么要单独建一个：掩膜窗口展示的是"绿色区域分割结果"，
    //   必须和识别线程用【完全一样】的参数（都从 tuning.yaml 的 color 节点读），
    //   否则窗口里看到的掩膜和实际参与识别的掩膜对不上，调阈值就是白调。
    //   注意：这里只多花一次 HSV 转换的开销，而且是在显示线程里，
    //         不会拖慢识别线程。
    ColorDetector::Params params =                                   // 准备参数
            ColorDetector::Params::fromJson(J_DETECT.config_["color"]);  // 从 color 节点读（和 Detect.cpp 同一个来源）
    this->mask_detector_ = new ColorDetector(params);                // 建识别器（析构里 delete）

    // ---------- 做一张"正在等待相机"的底图 ----------
    //   窗口尺寸按 tuning.yaml 里配置的相机分辨率来，这样第一帧到达时窗口不会跳尺寸
    int vw = 800, vh = 600;                                          // 兜底尺寸（读不到配置时用）
    if (!J_SENSORS.config_["uvc"]["width"].empty())  J_SENSORS.config_["uvc"]["width"]  >> vw;  // 读配置里的宽
    if (!J_SENSORS.config_["uvc"]["height"].empty()) J_SENSORS.config_["uvc"]["height"] >> vh;  // 读配置里的高
    if (vw < 1 || vh < 1) { vw = 800; vh = 600; }                    // 配置写坏了就退回兜底值
    this->placeholder_ = cv::Mat::zeros(vh, vw, CV_8UC3);            // 全黑底图，等会儿在上面写字

    // ---------- 十字准星画在哪：用【标定出来的光心】cx/cy ----------
    //   相机装在枪口正中心 → 枪口轴线 ≈ 相机光轴 → 枪口会打中的那个点
    //   在画面上就是【光心】的位置，不是画面正中间。
    //   两者不一样：标定出来 cx=641.27 / cy=327.77，
    //   而 1280x720 的几何中心是 640 / 360 —— 竖直差 32 像素。
    //   读不到配置就退回几何中心（至少不会崩）。
    this->cross_x_ = vw * 0.5;                                       // 兜底：画面几何中心
    this->cross_y_ = vh * 0.5;
    if (!J_DETECT.config_["camera"]["cx"].empty())                   // 配置里有 cx 就用标定值
        J_DETECT.config_["camera"]["cx"] >> this->cross_x_;
    if (!J_DETECT.config_["camera"]["cy"].empty())                   // 配置里有 cy 就用标定值
        J_DETECT.config_["camera"]["cy"] >> this->cross_y_;
    printf("[DetectView] 准星画在光心 (%.2f, %.2f)（画面几何中心是 (%.1f, %.1f)）\n",
           this->cross_x_, this->cross_y_, vw * 0.5, vh * 0.5);

    // ---------- 建主窗口（只有开了本地窗口才建）----------
    //   标题必须是纯英文：窗口标题栏底层同样是 Hershey 字体，中文会渲染成问号
    if (this->show_window_) {                                        // 要开窗口
        // 【开发板适配②】建窗口失败【绝不能】把进程带崩 ——
        //   这里是显示线程，抛出的 cv::Exception 没人接就是 std::terminate。
        //   失败就降级成"只推流不弹窗"，识别和网页推流完全不受影响。
        try {
            cv::namedWindow(kWinMain, cv::WINDOW_NORMAL);            //   可缩放的窗口
            cv::resizeWindow(kWinMain, cv::Size(960, 720));          //   给个合适的初始大小（4:3，和相机一致）
        } catch (const cv::Exception &e) {                           // 建窗口失败（后端不支持等）
            printf("[DetectView] ⚠️ 建窗口失败：%s\n", e.what());     //   把原因打出来
            printf("[DetectView]    → 自动降级为「只推流不弹窗」，识别和网页推流不受影响\n");
            fflush(stdout);                                          //   立刻刷出
            this->show_window_ = false;                              //   关掉窗口这条路
            this->show_mask_ = false;                                //   掩膜窗口也一起关
        }                                                            // try/catch 结束
    }                                                                // if 结束

    this->t0_ = (double)cv::getTickCount();                          // 记录起始时刻，后面算帧率用

    // 启动提示：说清楚这一路都开了什么，免得用户找不到画面
    printf("[DetectView] 启动：本地窗口=%s  网页推流=%s\n",           // 打印两个输出的开关状态
           this->show_window_ ? "开" : "关",                         // 窗口状态
           this->web_ ? "开" : "关");                                // 推流状态
    if (this->show_window_) {                                        // 开了窗口才提示按键
        printf("[DetectView] 按键: m 切掩膜 / s 存图 / q 退出\n");     // 按键说明
    }                                                                // if 结束

    // ---------- 【开发板适配③】两个输出都没开 → 你必须知道你看不到画面 ----------
    //   最容易踩的场景：在板子上（没桌面→窗口被自动关掉）而 web.enable 又忘了开，
    //   结果程序看着一切正常，实际上一个画面都没有。这里直接喊出来。
    if (!this->show_window_ && this->web_ == nullptr) {              // 两条路都没走通
        printf("\n[DetectView] ⚠️⚠️ 本地窗口和网页推流都没开 —— 你【看不到任何画面】！\n");
        printf("[DetectView]      想让画面能看：把 config/tuning.yaml 里的 web.enable 改成 1，重启程序\n");
        printf("[DetectView]      然后浏览器打开  http://<本机IP>:8080/\n\n");
    }                                                                // if 结束
    fflush(stdout);                                                  // 立刻刷出（stdout 被重定向时也能马上看到）
}                                                                    // 构造函数结束

// ============================================================================
// 析构函数
// ============================================================================
DetectView::~DetectView() {                                          // 析构
    delete this->mask_detector_;                                     // 释放掩膜识别器
    this->mask_detector_ = nullptr;                                  // 指针置空，防止悬空
    delete this->web_;                                               // 释放推流服务器（析构里会关端口）
    this->web_ = nullptr;                                            // 指针置空
    cv::destroyAllWindows();                                         // 关掉所有窗口
}                                                                    // 析构结束

// ============================================================================
// 主循环体 —— 由 main.cpp 的显示线程无限调用
//   一次调用 = 取一帧结果 + 画 + 显示 + 处理按键
// ============================================================================
void DetectView::run(LfStack<ImageInfo> &view_info_stack) {          // 入参：识别线程 → 显示线程 的无锁队列
    std::optional<ImageInfo> opt = view_info_stack.pop();            // 取一帧识别结果（队列空时返回空 optional）

    if (!opt.has_value()) {                                          // 还没有结果（相机还没出流 / 识别线程还没起来）
        // 这里【绝对不能】写成 while(!has_value()) pop(); 的死循环 ——
        // 那会 100% 占满一个 CPU 核空转（原来的 Mouse 模块就是这么写的）。
        // 正确做法：睡几毫秒再来看，保证不空转。
        if (this->show_window_ && this->frames_ == 0) {              // 开着窗口而且一帧都还没收到 → 画面还是全黑
            // 【限流】每 200ms 重画一次就够。
            //   这个提示内容是静态的（只有"已等待秒数"在变），
            //   而本循环是 5ms 一轮 —— 原来每轮都 imshow，等于每秒 200 次，
            //   实测把 CPU 烧到 187%（1~2 个核空转）。4 核的板子上这是致命的。
            double now_tick = (double)cv::getTickCount();            // 当前时刻
            if (now_tick - this->last_wait_draw_tick_ > cv::getTickFrequency() * 0.2) {  // 距上次超过 0.2 秒
                this->drawWaiting();                                 // 才真的重画
                this->last_wait_draw_tick_ = now_tick;               // 记下这次重画的时刻
            }                                                        // if 结束
        }                                                            // if 结束
        if (this->show_window_) {                                    // 只有开窗口时才需要跑 HighGUI 事件循环和收按键
            int key = cv::waitKey(5) & 0xFF;                         //   等 5ms，同时取出按键
            this->handleKey(key);                                    //   处理按键
        } else {                                                     // 没开窗口（板子上就是这种）
            std::this_thread::sleep_for(std::chrono::milliseconds(5));  //   主动睡 5ms，别让循环空转烧 CPU
        }                                                            // if 结束
        return;                                                      // 本次没数据，直接返回
    }                                                                // if 结束

    ImageInfo info = std::move(opt.value());                         // 把结果搬出来（move，避免拷贝图像数据）

    // 【省内存/省 CPU】按 s 存图用的东西，只在【开了本地窗口】时才留。
    //   板子上（只有网页推流、没窗口）根本按不了 s，留着纯属白占内存：
    //   last_raw_ 是 1.44MB，画布又 1.44MB，1GB 的板子上没必要常驻。
    if (this->show_window_) {                                        // 只有开窗口时才需要存
        this->last_raw_    = info.ori_mat;                           //   记住原图
        this->last_lights_ = info.lights;                            //   记住识别结果
    }                                                                // if 结束

    // 画布用【复用的成员缓冲】，而不是每帧 clone 一份新的：
    //   copyTo 到尺寸相同的已有 Mat 时会复用内存，只在第一帧分配一次。
    //   原来每帧 clone 要分配+拷贝 1.44MB，60fps 下就是 86MB/s 的内存带宽。
    //   ★必须拷贝，不能直接在 ori_mat 上画★ 否则会污染 Track 手里那份原图。
    info.ori_mat.copyTo(this->canvas_buf_);                          // 拷贝到复用画布
    cv::Mat &canvas = this->canvas_buf_;                             // 起个别名，下面的代码不用改
    this->drawOverlay(canvas, info);                                 // 把识别框 / 角度 / 状态栏画上去
    ++this->frames_;                                                 // 帧计数 +1

    // ---------- 输出①：网页推流（板子上没桌面时靠它看画面）----------
    //   推的是【画好框的 canvas】，所以远程也能看到识别框和解算结果，不只是原始画面。
    //   publish() 内部自己按配置的帧率限流，并且只存最新一帧，绝不阻塞这里。
    if (this->web_ != nullptr) {                                     // 开了推流
        this->web_->publish(canvas);                                 //   把这一帧丢给推流服务器
    }                                                                // if 结束

    // ---------- 输出②：本地窗口（有桌面时才用）----------
    if (this->show_window_) {                                        // 开了本地窗口才走 HighGUI
        cv::imshow(kWinMain, canvas);                                // 显示主画面
        if (this->show_mask_) {                                      // 如果掩膜窗口是开的
            cv::imshow(kWinMask, this->mask_detector_->makeMask(info.ori_mat));  // 显示二值掩膜（白=绿色区域）
        }                                                            // if 结束

        int key = cv::waitKey(1) & 0xFF;                             // 等 1ms 让窗口刷新，并取出按键
        this->handleKey(key);                                        // 处理按键

        // ---------- 用户直接点窗口右上角的 X 关掉窗口时，程序也应该退出 ----------
        //   不然 imshow 会把窗口又弹出来，看起来像"关不掉"
        //   ★两个保护条件，缺一不可：
        //     1) frames_ > 60 —— 必须确实画过一阵子之后才判断。
        //        刚建好窗口时，某些窗口管理器还没把窗口映射出来，
        //        WND_PROP_VISIBLE 会短暂返回 0；不做这个保护，程序会"一启动就自己退出"。
        //     2) vis == 0.0 —— 只认"明确报告不可见"这一种情况。
        //        不支持这个属性的后端会返回 -1 之类的非 0 值，那种情况一律忽略 ——
        //        绝不能让一个后端不支持的属性把程序搞退出。
        if (this->frames_ > 60) {                                    // 画够 60 帧（约 1 秒）之后才开始判断
            double vis = cv::getWindowProperty(kWinMain, cv::WND_PROP_VISIBLE);  // 查窗口可见性
            if (vis == 0.0) {                                        // 明确报告"不可见"（= 用户点了 X）
                printf("\n[DetectView] 窗口已关闭，程序退出\n");      // 提示
                fflush(stdout);                                      // 立刻刷出
                _exit(0);                                            // 工作线程里必须用 _exit（见 main.cpp 的说明）
            }                                                        // if 结束
        }                                                            // if 结束
    }                                                                // if 结束
}                                                                    // run 结束

// ============================================================================
// drawOverlay() —— 把识别结果画到画布上
//   ★所有画到图像上的文字必须是纯英文：
//     cv::putText 用的是 Hershey 字体，不支持中文，画中文会变成一串问号。
// ============================================================================
void DetectView::drawOverlay(cv::Mat &canvas,                        // 要画的画布（会被直接修改）
                             const ImageInfo &info) {                // 这一帧的全部信息
    const std::vector<LightInfo> &lights = info.lights;              // 取识别结果列表（引用，不拷贝）
    // ---------- 十字准星：画在【光心】上，也就是枪口会打中的位置 ----------
    //   ★不要改回 canvas.cols/2, canvas.rows/2★
    //   画面几何中心（640,360）和光心（641.27,327.77）差 32 像素，
    //   而这个准星就是拿来"看目标有没有对正枪口"的，画错位置等于骗自己。
    const int cx_px = (int)std::lround(this->cross_x_);              // 光心 x（四舍五入到整数像素）
    const int cy_px = (int)std::lround(this->cross_y_);              // 光心 y
    cv::line(canvas, cv::Point(cx_px, cy_px - 15),                   // 竖线上半段
             cv::Point(cx_px, cy_px + 15),                           // 竖线下半段
             cv::Scalar(255, 255, 255), 1);                          // 白色细线
    cv::line(canvas, cv::Point(cx_px - 15, cy_px),                   // 横线左半段
             cv::Point(cx_px + 15, cy_px),                           // 横线右半段
             cv::Scalar(255, 255, 255), 1);                          // 白色细线

    // ---------- 逐个目标画框 ----------
    for (size_t i = 0; i < lights.size(); ++i) {                     // 遍历所有目标
        const LightInfo &l = lights[i];                              // 取第 i 个（引用，不拷贝）

        // 第 1 个是面积最大的、也就是最终会被 Track 采用的那个 → 用绿色突出显示
        // 其余的备选 → 黄色（BGR 顺序：红绿蓝，（0,255,0）= 纯绿，（0,255,255）= 黄）
        cv::Scalar color = (i == 0) ? cv::Scalar(0, 255, 0)          // 第 1 个：绿色
                                    : cv::Scalar(0, 255, 255);       // 其余：黄色

        cv::rectangle(canvas, l.box, color, 2);                      // 画外接矩形，线宽 2，够醒目
        cv::circle(canvas, l.center_point, 4, color, cv::FILLED);    // 质心：实心小圆点
        cv::circle(canvas, l.center_point, 9, cv::Scalar(255, 255, 255), 1);  // 质心外再套一个白圈

        char txt[160];                                               // 文字缓冲区
        snprintf(txt, sizeof(txt),                                   // 拼一行说明文字（全英文）
                 "#%zu box=%.0fx%.0f c=(%.0f,%.0f) score=%.2f",      // 格式：序号 / 框宽高 / 中心坐标 / 得分
                 i + 1,                                              // 序号
                 l.box.width, l.box.height,                          // 外接框宽、高
                 l.center_point.x, l.center_point.y,                 // 质心坐标
                 l.score);                                           // 归一化得分

        int ty = (int)l.box.y - 10;                                  // 文字默认画在框上方 10 像素
        if (ty < 40) {                                               // 太靠上会被状态栏挡住
            ty = (int)(l.box.y + l.box.height) + 24;                 // 那就改画到框的下面
        }                                                            // if 结束
        cv::putText(canvas, txt, cv::Point((int)l.box.x, ty),        // 把文字画上去
                    cv::FONT_HERSHEY_SIMPLEX, 0.55, color, 2);       // 字体 / 字号 / 颜色 / 线宽
    }                                                                // for 结束

    // ---------- 左上角：解算结果面板（要下发给飞控的那三个量）----------
    //   数据来源：Track 每帧算完 yaw/pitch/距离后填进 ImageInfo.vd 再推过来。
    //   有了它，一个窗口就能同时回答两个问题：
    //     "识别到了吗"（看框 / 看 NO TARGET）和 "解算对不对"（看下面这几个数）。
    const VisionData &vd = info.vd;                                  // 取解算结果（引用）

    char ln0[128], ln1[128], ln2[128], ln3[128];                     // 四行文字的缓冲区
    // 【单位说明 —— 只改显示，不改数据】
    //   vd.yaw_error / vd.pitch_error 内部存的还是【弧度(rad)】：
    //   串口发给飞控的就是这个弧度值，飞控 PID 也是按弧度调的。
    //   这里只是【为了让肉眼好读】乘 57.29578 换成度再打印，
    //   ★千万不要去改 vd 里的值，否则飞控收到的单位就变了★
    //   换算： 度 = 弧度 × 57.29578      弧度 = 度 ÷ 57.29578
    const double kRad2Deg = 57.29578;                                // 弧度 → 度的换算系数
    snprintf(ln0, sizeof(ln0), "yaw   : %+8.2f deg", vd.yaw_error   * kRad2Deg);  // 偏航角误差(度)：正=目标偏右
    snprintf(ln1, sizeof(ln1), "pitch : %+8.2f deg", vd.pitch_error * kRad2Deg);  // 俯仰角误差(度)：正=目标偏上
    // 【已删除】原来这里还有一行 roll，跟着 roll 解算一起删掉了
    snprintf(ln2, sizeof(ln2), "dist  : %9.3f m",    vd.distance);    // 到目标的直线距离(m)
    snprintf(ln3, sizeof(ln3), "status: %s",         vd.target_status ? "FOUND" : "LOST");  // 目标状态
    const char *panel[4] = {ln0, ln1, ln2, ln3};                     // 汇总成数组，方便循环画
    for (int i = 0; i < 4; ++i) {                                    // 逐行画
        cv::putText(canvas, panel[i],                                // 文字
                    cv::Point(10, 62 + i * 26),                      // 位置：左侧，从状态栏下面开始
                    cv::FONT_HERSHEY_SIMPLEX, 0.6,                   // 字体 / 字号
                    cv::Scalar(0, 255, 0), 2);                       // 绿色（BGR）
    }                                                                // for 结束

    // ---------- 状态栏（顶部黑底）----------
    double elapsed = ((double)cv::getTickCount() - this->t0_) / cv::getTickFrequency();  // 已经跑了多少秒
    double fps = (elapsed > 0.5) ? (this->frames_ / elapsed) : 0.0;  // 实际显示帧率（跑够 0.5 秒才统计，避开启动抖动）

    char bar[256];                                                   // 状态栏文字缓冲
    // 【新增】画面平均亮度 avg —— 用来一眼区分两种完全不同的"黑屏"：
    //   · 窗口里有状态栏、但 avg 很小（<10）→ 相机在正常出帧，只是帧本身全黑
    //     （曝光/增益被写坏、或被别的程序占用）
    //   · 窗口里连状态栏都没有、纯黑一片 → 压根没收到帧（相机没出流）
    //     这时窗口会显示 WAITING FOR CAMERA 和已等待秒数，终端里还有相机报错
    cv::Scalar mean_bgr = cv::mean(info.ori_mat);                    // 求原图三个通道各自的均值
    double avg = (mean_bgr[0] + mean_bgr[1] + mean_bgr[2]) / 3.0;    // 取平均当作"画面亮度"

    snprintf(bar, sizeof(bar),                                       // 拼状态栏（全英文）
             "%.1f fps   %dx%d   det=%zu   mask=%s   avg=%.0f",      // 格式：帧率/分辨率/检出个数/掩膜/画面亮度
             fps, canvas.cols, canvas.rows, lights.size(),           // 填帧率、宽、高、检出个数
             this->show_mask_ ? "on" : "off", avg);                  // 填掩膜开关、画面平均亮度

    cv::rectangle(canvas, cv::Rect(0, 0, canvas.cols, 34),           // 顶部画一条黑底
                  cv::Scalar(0, 0, 0), cv::FILLED);                  // 填充
    cv::putText(canvas, bar, cv::Point(8, 25),                       // 状态栏文字
                cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 255, 255), 2);  // 黄色

    // ---------- 一个都没识别到时，用大红字直接写在画面中间 ----------
    //   这样"到底有没有识别到"一眼就能看出来，不用去数框
    if (lights.empty()) {                                            // 没有目标
        const char *msg = "NO TARGET";                               // 提示文字（英文）
        int baseline = 0;                                            // 文字基线（putText 要求传，这里用不到具体值）
        cv::Size sz = cv::getTextSize(msg, cv::FONT_HERSHEY_SIMPLEX, 1.6, 4, &baseline);  // 先量一下文字多大，才能居中
        cv::putText(canvas, msg,                                     // 画上去
                    cv::Point((canvas.cols - sz.width) / 2,          // x：水平居中
                              (canvas.rows + sz.height) / 2),        // y：垂直居中
                    cv::FONT_HERSHEY_SIMPLEX, 1.6,                   // 字体 / 字号（放大）
                    cv::Scalar(0, 0, 255), 4);                       // 红色、粗线
    }                                                                // if 结束
}                                                                    // drawOverlay 结束

// ============================================================================
// drawWaiting() —— 还没收到相机帧时显示的画面
//   目的：让"黑屏"这件事自己把原因说清楚，而不是留一个没有任何信息的黑窗口
//   窗口连状态栏都没有 → 就是这里在显示；说明【一帧都没收到】，不是识别的问题
// ============================================================================
void DetectView::drawWaiting() {                                     // 无参
    double waited = ((double)cv::getTickCount() - this->t0_) / cv::getTickFrequency();  // 已经等了多久（秒）

    cv::Mat img = this->placeholder_.clone();                        // 从底图复制一份来画（不动底图本身）
    int bl = 0;                                                      // 文字基线（getTextSize 要求传，这里只要宽度）

    char line[128];                                                  // 主提示文字缓冲
    snprintf(line, sizeof(line), "WAITING FOR CAMERA ...  %.0f s", waited);  // 主提示 + 已等待秒数
    // 文字全部纯英文：putText 用的是 Hershey 字体，画中文会变成一串问号
    cv::Size sz = cv::getTextSize(line, cv::FONT_HERSHEY_SIMPLEX, 1.0, 3, &bl);  // 量一下主提示多宽，好水平居中
    cv::putText(img, line, cv::Point((img.cols - sz.width) / 2, img.rows / 2 - 20),  // 画主提示（居中偏上）
                cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(0, 255, 255), 3);          // 黄色粗体

    // 三行副提示，解释"这是怎么回事"和"该去哪看错误"
    const char *hint[3] = {                                          // 三行提示内容
            "no frame from the camera yet",                          // ① 一帧都没收到
            "camera error is printed in the TERMINAL",               // ② 真正的报错在终端里
            "if it lasts >10s the program exits by itself"           // ③ 超过 10 秒程序会自己退出并打印排查步骤
    };                                                               // 数组结束
    for (int i = 0; i < 3; ++i) {                                    // 逐行画
        cv::Size s = cv::getTextSize(hint[i], cv::FONT_HERSHEY_SIMPLEX, 0.6, 2, &bl);  // 量宽度
        cv::putText(img, hint[i],                                    // 文字
                    cv::Point((img.cols - s.width) / 2, img.rows / 2 + 30 + i * 32),  // 位置：居中，逐行往下
                    cv::FONT_HERSHEY_SIMPLEX, 0.6,                   // 字体 / 字号
                    cv::Scalar(200, 200, 200), 2);                   // 灰色
    }                                                                // for 结束

    cv::imshow(kWinMain, img);                                       // 把这张提示图显示出去
}                                                                    // drawWaiting 结束

// ============================================================================
// handleKey() —— 处理按键
// ============================================================================
void DetectView::handleKey(int key) {                                // 入参：cv::waitKey 取到的按键码
    if (key == 'm' || key == 'M') {                                  // 按 m：开关掩膜窗口
        this->show_mask_ = !this->show_mask_;                        // 取反
        if (!this->show_mask_) {                                     // 如果刚才是开、现在关掉了
            cv::destroyWindow(kWinMask);                             // 把窗口销毁，别留个空壳在那儿
        }                                                            // if 结束
        printf("[DetectView] 掩膜窗口: %s\n", this->show_mask_ ? "开" : "关");  // 提示状态
        fflush(stdout);                                              // 立刻刷出
    } else if (key == 's' || key == 'S') {                           // 按 s：存图
        this->saveSnapshots();                                       // 存原图 / 带框图 / 掩膜图
    } else if (key == 'q' || key == 'Q' || key == 27) {              // 按 q 或 ESC：退出整个程序
        printf("\n[DetectView] 收到退出按键，程序结束\n");            // 提示
        fflush(stdout);                                              // 立刻刷出
        _exit(0);                                                    // 工作线程里必须用 _exit（见 main.cpp 的说明）
    }                                                                // if 结束
}                                                                    // handleKey 结束

// ============================================================================
// saveSnapshots() —— 存三张图到 snapshots/ 目录
//   用途：相机不在别人手里 / 远程排查时，把这三张图发出来就能定位问题：
//     snap_raw.png    原图 → 能看出灯到底有没有进画面、什么颜色
//     snap_mask.png   掩膜 → 能看出 HSV 阈值框住了什么（一眼看出是阈值问题还是形状问题）
//     snap_detect.png 带框 → 能看出最终判定的结果
// ============================================================================
void DetectView::saveSnapshots() {                                   // 无参
    mkdir("snapshots", 0755);                                        // 建目录（已存在会失败，无所谓，忽略返回值）

    if (!this->last_raw_.empty()) {                                  // 有原图才存
        cv::imwrite("snapshots/snap_raw.png", this->last_raw_);      // ① 原图
    }                                                                // if 结束
    if (!this->canvas_buf_.empty()) {                                // 有画好的图才存
        cv::imwrite("snapshots/snap_detect.png", this->canvas_buf_);  // ② 带识别框的图（复用画布就是最近一帧）
    }                                                                // if 结束
    if (this->mask_detector_ != nullptr && !this->last_raw_.empty()) {  // 有识别器也有原图才存掩膜
        cv::imwrite("snapshots/snap_mask.png",                       // ③ 二值掩膜
                    this->mask_detector_->makeMask(this->last_raw_));  // 现算一张（参数和识别线程一致）
    }                                                                // if 结束

    printf("[DetectView] 已存 3 张图到 snapshots/ ：snap_raw.png / snap_detect.png / snap_mask.png\n");  // 告诉用户存哪了
    fflush(stdout);                                                  // 立刻刷出
}                                                                    // saveSnapshots 结束
