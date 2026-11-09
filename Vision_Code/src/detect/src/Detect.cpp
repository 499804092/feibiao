#include "Detect.hpp"                                                // 自己的头文件（类定义）

#include "function/function.hpp"                                     // 工程公共函数（时间戳等）
#include "global/Config.hpp"                                         // J_DETECT：读 config/tuning.yaml 的全局对象

// ============================================================================
// Detect.cpp —— 识别线程
//   识别方式：传统视觉（HSV 颜色分割找绿色引导灯），算法见 ColorDetector.hpp
//   【本任务改动】原来的 OpenVINO 模型识别已整体删除，不再需要模型和 OpenVINO。
// ============================================================================

Detect::Detect(const char *mode) : mode_(mode) {                     // 构造函数：mode 是调试模式标识（"debug"）
    ColorDetector::Params params =                                   // 准备一份识别参数
            ColorDetector::Params::fromJson(J_DETECT.config_["color"]);  // 从 tuning.yaml 的 color 节点读出来
    params.print();                                                  // 打印参数，启动时确认一下生效值
    this->color_detector_ = new ColorDetector(params);               // 用这份参数创建识别器（new 到堆上，跟着对象生命周期）
}                                                                    // 构造函数结束

Detect::~Detect() {                                                  // 析构函数
    delete this->color_detector_;                                    // 释放识别器，避免内存泄漏
}                                                                    // 析构结束

// ============================================================================
// detect() —— 识别线程主循环体（每帧调用一次）
// ----------------------------------------------------------------------------
// 与模型版的区别（很重要）：
//   模型版是"双缓冲流水线"：infer() 启动下一帧、返回上一帧的结果，
//   所以"框"和"图像"差一帧，需要用 last_message_ 缓存上一帧来对齐。
//   颜色识别是"单帧直出"：读进来这一帧、立刻算出这一帧的结果，
//   框和图像天然同帧，不需要任何缓存。
// ============================================================================
void Detect::detect(
        LfStack <FromData> &message_stack,                           // 取图线程 → 识别线程 的无锁队列
        LfStack <ImageInfo> &image_info_stack) {                     // 识别线程 → 跟踪线程 的无锁队列
                                                                     // 【改动】原来还有第三个参数 view_info_stack，
                                                                     //   是"识别线程 → 显示线程"的队列。现在显示线程改由
                                                                     //   Track 来喂（Track 手里才有解算出的角度），故删除。
    std::optional<FromData> opt_message_ = message_stack.pop();      // ① 从队列取最新一帧（可能取到空）
    if (!opt_message_.has_value()) {                                 // 队列是空的（取图线程还没给数据）
        return;                                                      // 本帧不做事，直接返回
    }                                                                // if 结束
    auto message = std::move(opt_message_.value());                  // 把取到的数据"搬"出来（move 避免拷贝大图）

    // ========================================================================
    // 【关键】把积压的旧帧全部丢掉，只保留最新的一帧
    // ------------------------------------------------------------------------
    // 为什么必须这么做：取图线程和识别线程之间的 LfStack 是【无界队列】——
    //   LockFree.inl 里的 push() 每次都 new 一个节点，队列满了不阻塞、只增长。
    //   而识别每轮只 pop 一个。只要识别比相机慢（弱 CPU 上非常容易，比如
    //   RK3566 / 树莓派），队列就会无限堆积：每帧 800x600x3 ≈ 1.4MB，
    //   1GB 内存的板子跑几十秒就会被 OOM 杀掉。
    //
    //   丢掉旧帧在实时控制里本来就是【正确】做法：控制器要的是"现在看到的目标"，
    //   处理一帧 200ms 前的旧画面反而有害（角度是过时的）。
    //
    //   注意：这只影响"输入侧积压"。输出侧（image_info_stack）本来就是
    //   覆盖式写法（pop 再 push），不会积压。
    // ========================================================================
    int dropped = 0;                                                 // 本轮丢掉了多少积压旧帧
    while (true) {                                                   // 一直取到队列空为止
        std::optional<FromData> newer = message_stack.pop();         // 再试着取一帧更新的
        if (!newer.has_value()) {                                    // 队列空了
            break;                                                   // 停止
        }                                                            // if 结束
        message = std::move(newer.value());                          // 有更新的 → 替换掉手里这帧（旧的自动释放）
        ++dropped;                                                   // 记一笔
    }                                                                // while 结束

    if (dropped > 0) {                                               // 确实发生了积压
        // 提示"识别跟不上相机"，但最多每秒打一次，免得刷屏把真日志冲掉
        static double last_warn_tick = 0.;                           // 上次打印时的时刻（滴答数）
        double now_tick = (double)cv::getTickCount();                // 当前时刻
        if (now_tick - last_warn_tick > cv::getTickFrequency()) {    // 距离上次超过 1 秒才打印
            printf("[Detect] ⚠️ 识别跟不上相机，本轮丢弃 %d 帧积压（用的仍是最新帧，不影响控制）\n",
                   dropped);                                         // 说明现象
            fflush(stdout);                                          // 立刻刷出
            last_warn_tick = now_tick;                               // 更新计时
        }                                                            // if 结束
    }                                                                // if 结束

    std::vector<ColorDetector::Light> lights =                       // ② 识别：单帧直出
            this->color_detector_->detect(message.img);              // 结果就是【当前这一帧】的，不需要缓存对齐

    ImageInfo i_i;                                                   // ③ 组装这一帧的结果包
    i_i.cd        = message.cd;                                      // 电控指令（和框同一帧）
    i_i.ori_mat   = message.img;                                     // 原图（和框同一帧）
    i_i.timestamp = message.timestamp;                               // 采集时刻，跟踪线程用它算时间间隔

    for (const auto &l : lights) {                                   // ④ 把识别结果转成跟踪线程要的 LightInfo
        LightInfo light_info;                                        // 造一个 LightInfo
        light_info.center_point = l.center_point;                    // 中心点（原图坐标，颜色识别天然就是原图坐标）
        light_info.score        = l.score;                           // 分数（面积归一化值，越大越可能是目标）
        light_info.box          = l.box;                             // 外接矩形（原图坐标）
        // 【已删除】原来这里还有一行 light_info.angle = 0.;
        //   angle（目标框倾角）字段已经跟着 roll 一起删掉了
        i_i.lights.emplace_back(light_info);                         // 放进结果包
    }                                                                // for 结束

    image_info_stack.pop();                                          // ⑤ 先 pop 再 push = "覆盖式"写法
    image_info_stack.push(i_i);                                      //    保证队列里永远只有最新一帧，不会处理积压旧帧

    // 【改动】原来这里还有两行，把同一帧再推给显示线程（那时是给 mouse 调试框的）：
    //     view_info_stack.pop();
    //     view_info_stack.push(i_i);
    //   现在这个动作挪到 Track 里做 —— 因为只有 Track 手里才有解算出来的
    //   yaw/pitch/roll/距离，由它统一推一帧给显示线程，显示窗口一屏就能看全。
}                                                                    // detect 结束
