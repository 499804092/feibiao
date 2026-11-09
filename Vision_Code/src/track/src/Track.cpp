#include "Track.hpp"

#include "global/Config.hpp"

#include "function/function.hpp"

// 【新增】引入数学库：本任务新增的 atan2()(角度解算) 和 fabs()(判零) 都在这里
#include <cmath>

// 【新增】队列为空时需要主动让出 CPU（见 track() 里的说明）
//   ⚠️ 板子上只有 4 个核，这里原来是个 100% 空转的死循环，会白白吃掉一个核
#include <chrono>
#include <thread>

Track::Track(SerialPort &sp, const char *mode) : sp_(sp), mode_(mode) {
    J_TRACK.config_["ktrack"]["path"] >> this->path_;

    this->config_.open(this->path_, cv::FileStorage::READ);

    this->last_image_timestamp_ = function::getNowTimestamp();
    this->target_.state = LOST;

    // 【改动】原来这里还有三行，是给 Track 自己开一个调试窗口：
    //     cv::namedWindow("track", cv::WINDOW_NORMAL);
    //     cv::moveWindow("track", 1180, 0);
    //     cv::resizeWindow("track", cv::Size(300, 768));
    //   已经删掉了，原因有两个：
    //     1) 那个窗口被移到 x=1180，屏幕不够宽就整个跑到屏幕外，根本看不见
    //        —— 这就是"运行 Dart 看不到识别结果"的原因之一；
    //     2) 更关键：它会让 Track 线程和识别显示线程【同时】调 cv::imshow/cv::waitKey，
    //        而 OpenCV 的 HighGUI 不是线程安全的，随时可能崩。
    //   现在显示统一由 view 模块（DetectView）负责，Track 只负责把结果推给它。
}


void Track::track(LfStack<ImageInfo> &image_info_stack) {
    auto opt = image_info_stack.pop();                               // 取一帧识别结果

    // 队列空的时候【绝对不能】死循环空转。
    //   原来写的是 while(!opt.has_value()) opt = pop(); —— 这一句会让本线程
    //   100% 占满一个 CPU 核。RK3566 只有 4 个 A55 核（板子还有 1GB 内存的压力），
    //   白白烧掉一个核会拖慢识别、推流和相机取图，反而让画面更卡。
    //   这里改成"睡 1ms 再来看"：60fps 下每帧间隔 16.7ms，
    //   多等 1ms 对时效性几乎没有影响（平均只增加 0.5ms），却能把一个核让出来。
    while (!opt.has_value()) {                                       // 队列还是空的
        std::this_thread::sleep_for(std::chrono::milliseconds(1));   // 让出 CPU，睡 1ms
        opt = image_info_stack.pop();                                // 再取一次
    }                                                                // while 结束

    if (this->mode_ == "debug") {
        J_TRACK.updateJson();
    }

    this->updateTimeInterval(opt.value().timestamp);
    this->chooseTarget(opt.value());
}

void Track::updateTimeInterval(const foxglove::Time &image_timestamp) {
    this->image_time_interval_ = function::timestampMinus(image_timestamp, this->last_image_timestamp_) / 1e3;
    this->last_image_timestamp_ = image_timestamp;
}


void Track::reset() {
    this->target_.vec.clear();          // 清空目标历史队列
    this->target_.drop_frame = 0.;      // 丢帧计时清零
    this->target_.found_frame = 0.;     // 命中帧计时清零
    this->target_.state = LOST;         // 状态复位为"丢失"

    this->cache_.lights.clear();        // 清空本帧检测到的所有候选目标

    this->init_filter_ = false;         // 滤波器复位标志：下一帧重新初始化(含新增的4个滤波器)
    this->lost_log_cnt_ = 0;            // 【新增】丢失帧心跳计数清零，重新从第 0 帧开始发
}

void Track::insertTargetVec(
        std::deque<LightInfo> &vec,
        const LightInfo &light,
        const size_t &size) {
    if (vec.size() > size) {
        vec.pop_front();
    }
    
    vec.push_back(light);
}

auto Track::frameLimit(const double &frame, const double &plus, const double &limit) -> double {
    return frame < limit ? frame + plus : limit;
}

void Track::updateState() {
    if (!this->cache_.lights.empty() && this->target_.state == LOST) {
        this->target_.drop_frame = 0.;
        this->target_.found_frame = Track::frameLimit(
                this->target_.found_frame,
                this->image_time_interval_,
                (double) J_TRACK.config_["selection"]["limit_found_frame"]);
        if (this->target_.found_frame >= (double) J_TRACK.config_["selection"]["min_found_frame"]) {
                this->target_.state = FOUND;
                double max_score = 0.;
                LightInfo max_score_light;
                for (const auto &light : this->cache_.lights) {
                    if (light.score > max_score) {
                        max_score = light.score;
                        max_score_light = light;
                    }
                }
                Track::insertTargetVec(
                        this->target_.vec,
                        max_score_light,
                        5);
        }
    } else if (this->target_.state != LOST) {
        if (!this->cache_.lights.empty()) {

            this->target_.state = FOUND;
            this->target_.drop_frame = 0.;
            this->target_.found_frame = Track::frameLimit(
                    this->target_.found_frame,
                    this->image_time_interval_,
                    (double) J_TRACK.config_["selection"]["limit_found_frame"]);

            LightInfo target_light;
            auto min_distance = DBL_MAX;
            LightInfo target_light_back = this->target_.vec.back();
            for (auto &light : this->cache_.lights) {
                double distance = cv::norm(target_light_back.center_point - light.center_point);
                if (distance < min_distance) {
                    min_distance = distance;
                    target_light = light;
                }
            }
            Track::insertTargetVec(
                    this->target_.vec,
                    target_light,
                    5);
        } else {
            this->target_.state = DROP;
            this->target_.found_frame = 0.;
            this->target_.drop_frame = Track::frameLimit(
                    this->target_.drop_frame,
                    this->image_time_interval_,
                    (double) J_TRACK.config_["selection"]["limit_drop_frame"]);
        }
    } else if (this->cache_.lights.empty()) {
        this->target_.found_frame = 0.0;
        this->target_.drop_frame = 0.0;
    }

    if (this->target_.drop_frame >= (double) J_TRACK.config_["selection"]["max_drop_frame"]) {
        std::cout << "drop frame too much, reset." << std::endl;
        this->reset();
    }
}

// ---------------------------------------------------------------------------
// 【核心】目标选择 / 解算主流程
//   1) chooseTarget()：目标丢失时不再"只发一帧"，而是持续下发 target_status=0，
//      并把 pitch/yaw/distance 全部清零，避免飞控端抱住上一次的旧角度不放。
//   2) updateVisionData()：一次性解算出下发给飞控板的三个量：
//        偏航 yaw_error、俯仰 pitch_error、直线距离 distance
//   （原来的横滚 roll_error 已整体删除，详见 Struct.hpp 里 VisionData 的说明）
// ---------------------------------------------------------------------------
void Track::chooseTarget(ImageInfo &image_info) {
    this->cache_ = std::move(image_info);   // 接管本帧检测结果(含原图/时间戳/电控指令)
    this->updateState();                    // 更新目标状态机：LOST / DROP / FOUND

    if (this->target_.state != LOST) {      // 状态不是"丢失"→ 有目标，正常解算并下发
        this->updateVisionData();
    } else {                                // 状态是"丢失"→ 清空解算结果后下发
        // 三个量一并清零，飞控端收到 0 会进入保护，而不是继续追旧值
        this->vd_.pitch_error = 0.f;        // 俯仰角误差清零
        this->vd_.yaw_error = 0.f;          // 偏航角误差清零
        this->vd_.distance = 0.f;           // 直线距离清零(0 表示无效)
        this->vd_.target_status = 0;        // 0 = Lost，明确告诉飞控"目标丢了"
        this->init_filter_ = false;         // 下一帧重新初始化滤波器，避免从旧位置猛拉
        // 丢失时不必每帧都发(省串口带宽)，每 50 帧发一次心跳式"丢失帧"
        if (this->lost_log_cnt_++ % 50 == 0) {
            this->sp_.send(this->vd_);      // 打包 + CRC + 串口下发
        }
    }

    // ========================================================================
    // 【关键修复】把这一帧交给显示线程 —— ★不管有没有目标，每帧都必须推★
    // ------------------------------------------------------------------------
    // 【之前的 bug】这行 push 原来写在 updateVisionData() 的末尾，而那个函数
    //   只在 state != LOST 时才被调用。后果是：
    //     目标一旦丢失（终端会打 "drop frame too much, reset."），
    //     Track 就【再也不给显示线程喂数据】→ 显示队列永远是空的 →
    //     窗口画面冻结不动、网页推流也停在最后一帧不再更新。
    //   而操作手恰恰最需要在"丢目标"的时候看到实时画面（好判断是灯没对准、
    //   还是识别阈值不对）。所以必须无条件每帧都推。
    // ========================================================================
    if (this->view_stack_ != nullptr) {                      // 只有显示线程存在时才推（比赛模式为 nullptr）
        ImageInfo out = this->cache_;                        // 浅拷贝：cv::Mat 是引用计数，不会真的复制图像数据
        out.vd = this->vd_;                                  // 带上解算结果（丢失时这三个量都是 0，显示窗口会打 LOST）
        this->view_stack_->pop();                            // 覆盖式：先弹掉旧的
        this->view_stack_->push(out);                        // 再推最新的 —— 队列里永远只有最新一帧，不会积压
    }                                                        // if 结束
}

auto Track::filter(const cv::Point2d &point, const double &dt) -> cv::Point2d {
    return {
            this->filter_x_.filter(point.x, dt),
            this->filter_y_.filter(point.y, dt)};
}

// ---------------------------------------------------------------------------
// 【新增】像素坐标 → 角度误差(rad)
//   针孔相机模型： x_img = fx * (X/Z) + cx  ⇒  tan(θ) = (x_img - cx) / fx
//   竖直方向同一套公式，用 fy / cy。
//   符号约定（与飞控 PID 方向一致，需要反方向时改这里的正负号即可）：
//     yaw_error   > 0 → 目标在画面右侧 → 需要向右转
//     pitch_error > 0 → 目标在画面上方 → 需要抬高枪口
// ---------------------------------------------------------------------------
auto Track::pixelToAngle(const cv::Point2d &point) const -> cv::Point2d {
    double yaw_angle = std::atan2(point.x - this->cx_, this->fx_);    // 水平角误差(rad)
    double pitch_angle = std::atan2(this->cy_ - point.y, this->fy_);  // 竖直角误差(rad)
    return {yaw_angle, pitch_angle};   // 返回 {yaw, pitch}
}

// ---------------------------------------------------------------------------
// 【新增】单目测距（用目标框像素尺寸 + 焦距 + 目标真实尺寸估算直线距离）
//   针孔模型： Z = f * W / w
//     f : 焦距(像素)   W : 目标真实尺寸(m)   w : 目标在图像中占的像素尺寸
//   抗噪处理：水平方向和竖直方向各算一次，取平均。
//   若某方向尺寸非法(<=1px)就只用另一个方向；两个都不合法则返回 0(视为无效)。
// ---------------------------------------------------------------------------
auto Track::estimateDistance(const cv::Rect2d &box) const -> double {
    double z_x = 0.;    // 用宽度算出的距离(m)
    double z_y = 0.;    // 用高度算出的距离(m)

    if (box.width > 1.) {                                     // 宽度像素数有效才计算
        z_x = this->fx_ * this->target_width_m_ / box.width;  // Z = fx * W_real / w_pixel
    }
    if (box.height > 1.) {                                    // 高度像素数有效才计算
        z_y = this->fy_ * this->target_height_m_ / box.height;// Z = fy * H_real / h_pixel
    }

    double z;                                                 // 最终距离(m)
    if (z_x > 0. && z_y > 0.) {                               // 两个方向都算出来了
        z = 0.5 * (z_x + z_y);                                // 取平均，抑制框抖动带来的跳变
    } else if (z_x > 0.) {                                    // 只有宽度可用
        z = z_x;
    } else if (z_y > 0.) {                                    // 只有高度可用
        z = z_y;
    } else {                                                  // 都不可用 → 距离无效
        return 0.;
    }

    if (z < this->dist_min_m_) {                              // 小于设定下限：多半是误检/目标贴脸
        return 0.;                                            // 判为无效，交给飞控做保护
    }
    if (z > this->dist_max_m_) {                              // 大于设定上限：超出可信区间
        z = this->dist_max_m_;                                // 钳位到上限，避免距离爆表
    }
    return z;
}

// ---------------------------------------------------------------------------
// 【已删除】横滚角(roll)解算 estimateRoll()
// ---------------------------------------------------------------------------
// 原来这里是这样实现的：
//     double ratio = box.width / box.height;
//     return std::atan((ratio * ratio - 1.) / 3.) * 0.5;
//
// 为什么要删掉它：
//   1) 【原理上就测不出来】本工程的检测输出是"轴对齐矩形框"(cv::Rect2d)，
//      框里【根本不含倾角信息】—— 倾角是 cv::RotatedRect 才有的字段
//      （别写成 box.angle，编译不过）。所以它只能拿"长宽比偏离 1"去硬凑。
//   2) 【算出来的是错的】实测拿正对着的圆形引导灯照片代入（外接框 51x55）：
//          ratio = 51/55 = 0.927
//          roll  = atan((0.927² - 1)/3) × 0.5 = -0.0234 rad ≈ -1.34°
//      一个完全没有滚转的圆形光斑，被硬算成 -1.34° —— 这是光晕扩散导致
//      框稍微不等宽造成的【虚假输出】。反过来，真滚转时反而测不出来。
//   3) 【没人用它】查过电控侧代码（vision_protocol.c），只取 yaw_error 和
//      target_status，pitch/roll/distance 都没取。
//
// 以后真要测 roll 怎么办：
//   · 首选：从 IMU 姿态取（飞控本来就有 IMU，比视觉可靠得多）
//   · 或者：把检测从 boundingRect 改成 cv::minAreaRect(RotatedRect)，取真实倾角
//   · 加回协议时【追加在 distance 之后】，不要插在中间
// ---------------------------------------------------------------------------

void Track::updateVisionData() {
    // ---- 步骤 0：第一次调用时把相机内参/目标尺寸/距离区间读进来并缓存 ----------
    if (!this->init_params_) {                              // 【新增】只加载一次，省每帧开销
        this->fx_ = (double) J_TRACK.config_["camera"]["fx"];                    // 水平焦距(px)
        this->fy_ = (double) J_TRACK.config_["camera"]["fy"];                    // 竖直焦距(px)
        this->cx_ = (double) J_TRACK.config_["camera"]["cx"];                    // 光心 x(px)
        this->cy_ = (double) J_TRACK.config_["camera"]["cy"];                    // 光心 y(px)
        this->target_width_m_ = (double) J_TRACK.config_["target"]["width_m"];   // 目标宽(m)
        this->target_height_m_ = (double) J_TRACK.config_["target"]["height_m"]; // 目标高(m)
        this->dist_min_m_ = (double) J_TRACK.config_["distance"]["min_m"];       // 距离下限(m)
        this->dist_max_m_ = (double) J_TRACK.config_["distance"]["max_m"];       // 距离上限(m)
        this->init_params_ = true;                          // 标记已加载，后续帧走缓存
    }

    // ---- 步骤 1：对目标中心点做一欧元滤波，得到平滑后的瞄准点 aim ------------
    cv::Point2d aim;                                        // 平滑后的瞄准点(原图像素坐标)
    if (!this->init_filter_) {                              // 第一帧：用当前观测值初始化滤波器
        this->init_filter_ = true;
        this->filter_x_.setParameter(                        // x 方向滤波器初始状态
                this->target_.vec.back().center_point.x,
                0.,
                (double) J_TRACK.config_["euro"]["min_cut_off"],
                (double) J_TRACK.config_["euro"]["beta"],
                (double) J_TRACK.config_["euro"]["d_cut_off"]);
        this->filter_y_.setParameter(                        // y 方向滤波器初始状态
                this->target_.vec.back().center_point.y,
                0.,
                (double) J_TRACK.config_["euro"]["min_cut_off"],
                (double) J_TRACK.config_["euro"]["beta"],
                (double) J_TRACK.config_["euro"]["d_cut_off"]);
        cv::Point2d init_angle = this->pixelToAngle(this->target_.vec.back().center_point);
        this->filter_yaw_.setParameter(                      // 【新增】yaw 角滤波器初始状态
                init_angle.x, 0.,
                (double) J_TRACK.config_["euro"]["min_cut_off"],
                (double) J_TRACK.config_["euro"]["beta"],
                (double) J_TRACK.config_["euro"]["d_cut_off"]);
        this->filter_pitch_.setParameter(                    // 【新增】pitch 角滤波器初始状态
                init_angle.y, 0.,
                (double) J_TRACK.config_["euro"]["min_cut_off"],
                (double) J_TRACK.config_["euro"]["beta"],
                (double) J_TRACK.config_["euro"]["d_cut_off"]);
        this->filter_dist_.setParameter(                     // 【新增】距离滤波器初始状态
                this->estimateDistance(this->target_.vec.back().box), 0.,
                (double) J_TRACK.config_["euro"]["min_cut_off"],
                (double) J_TRACK.config_["euro"]["beta"],
                (double) J_TRACK.config_["euro"]["d_cut_off"]);
        // 【已删除】原来这里还有一段 filter_roll_.setParameter(...)，
        //   roll 已经整体去掉，滤波器也跟着删了。
        aim = this->target_.vec.back().center_point;         // 首帧不滤波，直接用观测点
    } else {
        aim = this->filter(this->target_.vec.back().center_point, this->image_time_interval_);
    }

    auto &vec = this->target_.vec.back();                    // 当前目标的检测信息(中心/框/置信度)
    this->vd_.target_status = 1;                             // 1 = Found，告诉飞控"看到目标了"

    // 【修正】原代码写 number<1 && !mode：number 是 uint8_t，!mode 是逻辑取反恒为 0/1，
    //         混用既不符合本意又触发编译告警；这里改成等价且明确的写法。
    if (this->cache_.cd.number < 1 && this->cache_.cd.mode == 0) {
        this->config_.release();                             // 关闭当前只读句柄
        this->config_.open(this->path_, cv::FileStorage::WRITE);   // 以写方式打开标定文件
        this->config_ << "begin" << vec.center_point.x;      // 记录本次"初始靶心 x"作为标定基准
        this->config_.release();
        this->config_.open(this->path_, cv::FileStorage::READ);    // 再切回只读，供后面读 begin
    }
    // ---- 像素补偿：公共偏移 + 本发专属偏移 ----
    //   【改动】原来是 group[方案名]["0"] + group[方案名][发数] 这种两层嵌套写法。
    //   配置文件从 JSON 换成 YAML 之后必须拆平，原因是实测出来的：
    //     OpenCV 的 YAML 解析器【完全不支持数字形式的 key】——
    //     "0": 0 / 0: 0 / "82_84": 都报 Incorrect indentation，任何嵌套层级都一样。
    //   所以改成 group_common + group_shot_N 这种平铺命名，反而更好调。
    double begin_x = (double) this->config_["begin"];        // 标定基准 x（记录在 ktrack.json 里）
    double group_offset =                                    // 公共偏移 + 本发专属偏移
            ((double) J_TRACK.config_["group_common"] +
             (double) J_TRACK.config_["group_shot_" + std::to_string(this->cache_.cd.number)]);

    // ---- 步骤 2：解算下发给飞控的四个量 ------------------------------------
    cv::Point2d angle = this->pixelToAngle(aim);             // 平滑瞄准点 → {yaw,pitch}(rad)；pitch 直接用它，yaw 见下面

    // ---- yaw：相对【标定基准 begin_x】的角度误差 ----
    //   【bug 修复】原来这里写的是：
    //       yaw = -( atan2(begin_x - vec.center_point.x - group_offset, fx_) + angle.x )
    //   其中 angle.x = atan2(aim.x - cx_, fx_)，而 aim 就是【滤波后的 vec.center_point.x】。
    //   两项里的 x 会完全抵消（小角近似展开）：
    //       -( (begin_x - x - offset)/fx + (x - cx_)/fx ) = -(begin_x - offset - cx_)/fx
    //   代入当前配置 (begin_x=387.5, offset=0+5=5, cx_=320, fx_=1706)：
    //       -62.5/1706 = -0.0366 rad ≈ -2.10°
    //   也就是说不论目标在画面哪个位置，yaw 都吐同一个【恒定常数】，
    //   目标左右移动时 yaw 根本不跟着变 —— 这就是"yaw 解算不准"的根因。
    //   现在只保留标定基准这一项（和下面 pitch 的写法对称，都基于平滑点 aim）：
    //       yaw = -( begin_x - aim.x - offset ) / fx
    //   符号核对：目标在基准右侧 → aim.x > begin_x → 括号内为负 → 取负后为正，
    //             与 tuning.yaml 里 "rules": "to right: +" 的约定一致。
    this->vd_.yaw_error = -float(std::atan2(begin_x - aim.x - group_offset, this->fx_));
    // pitch：竖直方向直接取平滑点的角度误差（cy_ - y，正 = 目标偏上）
    this->vd_.pitch_error = float(angle.y);
    // 【已删除】原来这里还有 roll：
    //     this->vd_.roll_error = float(this->estimateRoll(vec.box));
    //   roll 已整体去掉（原因见上面 estimateRoll 那段注释）
    // distance：单目测距 + 一欧元滤波，得到平滑的直线距离(m)
    this->vd_.distance = float(this->filter_dist_.filter(this->estimateDistance(vec.box),
                                                         this->image_time_interval_));
    // 解算出的 yaw/pitch 再做一次角度域滤波（像素域已滤过，这里抑制角度抖动）
    this->vd_.yaw_error = float(this->filter_yaw_.filter(this->vd_.yaw_error,
                                                         this->image_time_interval_));
    this->vd_.pitch_error = float(this->filter_pitch_.filter(this->vd_.pitch_error,
                                                            this->image_time_interval_));
    // 【已删除】yaw/pitch 之后原来还有一次 filter_roll_.filter(...)

    // ---- 步骤 3：把这一帧交给识别显示线程 ----
    //   【已移到 chooseTarget() 末尾】原来这里有一份 push 到 view_stack_ 的代码。
    //   移走的原因：这个函数只在 state != LOST 时才会被调用，所以目标一丢失
    //   就不再给显示线程喂数据，窗口和网页都会冻住。现在改成在 chooseTarget()
    //   末尾【无条件每帧都推】，详见那里的说明。
    //
    //   历史背景（为什么显示由 Track 来喂而不是 Detect）：
    //     原版 Track 自己 cv::imshow 一个窗口，但那个窗口被 moveWindow 到 x=1180，
    //     屏幕不够宽就跑到屏幕外；而且它和显示线程会同时调 imshow/waitKey，
    //     而 HighGUI 不是线程安全的。所以改成 Track 把数据推给唯一的显示线程。

    // ---- 步骤 4：打包下发 ---------------------------------------------------
    this->sp_.send(this->vd_);   // 内部：memcpy → 填 CRC8/CRC16 → write 到串口
}

