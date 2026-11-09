#ifndef TRACK_HPP_
#define TRACK_HPP_

#include "serialport/SerialPort.h"
#include "lockfree/LockFree.hpp"

#include "filter/OneEuroFilter.hpp"
class Track {
public:
    explicit Track(SerialPort &sp, const char *mode);
    ~Track() = default;

    template<class ...Args>
    void update(Args &&...args) {
        this->updateArgs(std::forward<Args>(args)...);
    }

    // 【新增】把「识别显示线程」的队列交给 Track
    //   为什么由 Track 来喂显示线程：Track 手里同时有【原始识别结果】
    //   （cache_.lights）和【解算出的 yaw/pitch/roll/距离】（vd_），
    //   由它一次性推给显示线程，显示窗口一个画面就能把两者都画出来。
    //   顺带也就干掉了 Track 原来自己开的那个 "track" 窗口 ——
    //   两个线程同时调 OpenCV 的 imshow/waitKey 是不安全的，必须只留一个。
    //   ★这个函数必须放在 public 区，main.cpp 要用它。
    void setViewStack(LfStack<ImageInfo> &view_stack) { this->view_stack_ = &view_stack; }

private:

    enum STATE {
            LOST = 0,
            DROP = 1,
            FOUND = 2
        };

    template<typename T, class ...Args>
    void updateArgs(T &&t, Args &&...args) {
        if constexpr (std::is_same_v<T, LfStack<ImageInfo>&>) {
            this->track(t);
        } else if constexpr (std::is_same_v<T, LfStack<CaptureInfo>&>) {
            t.pop();
            t.push(CaptureInfo{
                    .cd = this->cache_.cd,
                    .vd = this->vd_,
                    .timestamp = this->cache_.timestamp,
                    .img = this->cache_.ori_mat.clone()
            });
        } else {
            std::cout << "error type in track." << std::endl;
        }
        this->updateArgs(args...);
    }

    void updateArgs() {};

    void track(LfStack<ImageInfo> &image_info_stack);


    void updateTimeInterval(const foxglove::Time &image_timestamp);

    static void insertTargetVec(
            std::deque<LightInfo> &vec,
            const LightInfo &light,
            const size_t &size);

    void updateState();

    void reset();

    static auto frameLimit(const double &frame, const double &plus, const double &limit) -> double;

    void chooseTarget(ImageInfo &image_info);

    auto filter(const cv::Point2d &point, const double &dt) -> cv::Point2d;

    void updateVisionData();

    // 【新增】像素坐标 → 俯仰/偏航角误差(rad)：用相机内参把像素偏差换算成角度
    auto pixelToAngle(const cv::Point2d &point) const -> cv::Point2d;
    // 【新增】单目测距：用目标框的像素尺寸 + 焦距 + 目标真实尺寸估算直线距离(m)
    auto estimateDistance(const cv::Rect2d &box) const -> double;
    // 【已删除】原来这里有一个 estimateRoll()（横滚角解算）。
    //   删掉的原因：检测输出是轴对齐矩形框，框里没有倾角信息，
    //   原来那个实现是拿"长宽比偏离 1"硬凑的，对正对的圆形引导灯会算出
    //   虚假的 -1.34°，真滚转反而测不出来。既不准也没被电控使用。

    SerialPort &sp_;
    std::string mode_;

    // 【新增】识别显示线程的队列指针；由 main.cpp 用 setViewStack() 注入。
    //   为 nullptr 表示没有显示线程（比赛模式 view.enable=0），
    //   此时下面所有显示相关代码都会跳过，没有任何开销。
    LfStack<ImageInfo> *view_stack_ = nullptr;

    TargetCache target_;
    ImageInfo cache_;

    foxglove::Time last_image_timestamp_;
    double image_time_interval_; // s

    VisionData vd_;              // 【保留】待下发给飞控板的解算结果

    bool init_filter_ = false;
    OneEuroFilter filter_x_;     // 【保留】中心点 x(像素) 的一欧元滤波
    OneEuroFilter filter_y_;     // 【保留】中心点 y(像素) 的一欧元滤波
    OneEuroFilter filter_yaw_;   // 【新增】偏航角(rad) 的一欧元滤波，抑制角度抖动
    OneEuroFilter filter_pitch_; // 【新增】俯仰角(rad) 的一欧元滤波
    OneEuroFilter filter_dist_;  // 【新增】距离(m) 的一欧元滤波，抑制距离跳变
    // 【已删除】filter_roll_（横滚角滤波器）—— roll 已整体去掉

    // 【新增】从 json 读入并缓存的相机/目标参数，避免每帧都去查配置(降低耗时)
    double fx_ = 0.;             // 焦距(像素)，水平方向
    double fy_ = 0.;             // 焦距(像素)，竖直方向
    double cx_ = 0.;             // 光心 x(像素)
    double cy_ = 0.;             // 光心 y(像素)
    double target_width_m_ = 0.; // 目标真实宽度(m)
    double target_height_m_ = 0.;// 目标真实高度(m)
    double dist_min_m_ = 0.;     // 距离下限(m)
    double dist_max_m_ = 0.;     // 距离上限(m)
    bool init_params_ = false;   // 【新增】参数是否已加载的标志

    // 【新增】丢失目标时也要持续下发(否则飞控收不到"目标丢失"会一直用旧角度)
    unsigned int lost_log_cnt_ = 0;


    cv::FileStorage config_;
    std::string path_;
};

#endif