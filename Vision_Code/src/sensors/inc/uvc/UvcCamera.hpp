/**
 ******************************************************************************
 * @file    UvcCamera.hpp
 * @brief   UVC 免驱摄像头取图（本项目唯一的相机后端）
 ******************************************************************************
 * 【设计说明】
 *   本项目使用的是「亚博智能 USB 摄像头 1080P」这类**标准 UVC 免驱摄像头**：
 *   由 Linux 内核 uvcvideo 驱动暴露成 /dev/video*，用 OpenCV 的 cv::VideoCapture
 *   直接读即可 —— **不需要任何厂商 SDK**。
 *
 *   它继承 sensors::BaseCamera，实现 cameraInit() 和 getImage() 两个纯虚函数，
 *   所以 main.cpp 里的取图线程只需要：
 *       UvcCamera cam(sp);
 *       cam.cameraInit();
 *       cam.refreshImage(fd_msgs);
 *   下游（检测 → 跟踪 → 解算 → 串口下发）完全不受影响。
 *
 * 【依赖】只需 OpenCV（本机 4.5.4，已确认带 v4l/v4l2 后端）
 ******************************************************************************
 */

#ifndef UVC_CAMERA_HPP_
#define UVC_CAMERA_HPP_

#include "sensors.hpp"          // sensors::BaseCamera（含 FromData / SerialPort 引用）

class UvcCamera : public virtual sensors::BaseCamera {
public:
    explicit UvcCamera(SerialPort &sp);
    ~UvcCamera();

    void cameraInit() override;     // 打开设备并配置分辨率/帧率

    // ========================================================================
    // 【新增】曝光 / 增益控制接口
    //   为什么要加：引导灯识别靠「颜色 + 亮度」，环境太亮时画面过曝、噪声大，
    //   识别会飘。把曝光压下来能显著减少噪声（这一步是硬件层面降噪，比任何
    //   软件滤波都有效）。具体数值由 config/tuning.yaml 的 uvc.exposure
    //   配置，也可以运行时调。
    // ========================================================================

    // 运行时调整曝光/增益（参数给 <0 表示这一项不动）
    //   auto_mode : V4L2 的 auto_exposure —— 1 = 手动，3 = 自动(光圈优先)
    //   value     : 手动曝光绝对值，单位 100 微秒，越小画面越暗、噪声越少
    //   gain      : 增益，越小噪声越少（但画面更暗）
    //   ★ 内部已经处理好顺序：先切手动，再写曝光值。顺序反了驱动会忽略曝光值。
    //   返回 true 表示至少成功下发了一项。
    bool setExposureParams(int auto_mode, int value, int gain);

    // 读回相机当前「真实生效」的值（调参时用来确认到底写进去没有）
    void readExposureParams(int &auto_mode, int &value, int &gain);

    // 把相机支持的所有曝光/亮度相关控制项及其取值范围打印出来
    //   手动调参前先看这个，就知道这台相机能填多少、默认是多少
    void printCameraControls();

private:
    void getImage() override;                                   // 取一帧（+ 电控指令）
    void cameraModeChange(int now_mode, int my_color) override; // 兼容接口，未使用
    void assertSuccess(uint32_t status) override;               // 兼容接口，未使用

    // ---- 从 config/tuning.yaml 读出来的配置 ----
    int   device_index_ = 0;        // /dev/videoN 的 N
    int   req_width_    = 640;      // 请求的宽（要用原图尺寸，别用 1920 全图）
    int   req_height_   = 480;      // 请求的高
    int   req_fps_      = 120;      // 请求的帧率
    int   orientation_  = 0;        // 【重要】画面朝向修正，见下方说明
    bool  use_mjpg_     = true;     // true = MJPG（高帧率必须），false = YUYV

    // ---- 【新增】曝光/增益配置（对应 tuning.yaml 的 uvc.exposure）----
    int   exp_enable_     = 0;      // 0 = 完全不碰相机（安全默认值）；1 = 按下面参数设置
    int   exp_auto_       = 3;      // V4L2 auto_exposure：1 = 手动，3 = 自动
    int   exp_value_      = -1;     // 手动曝光绝对值（100µs 为单位）；<0 = 不改
    int   exp_gain_       = -1;     // 增益；<0 = 不改
    int   exp_brightness_ = -1;     // 亮度；<0 = 不改
    int   exp_backlight_  = -1;     // 背光补偿；<0 = 不改

    // 专门用来下发控制项的 fd —— 和取流用的 cap_ 是两个独立的设备句柄。
    //   V4L2 允许同一个 /dev/videoN 被多次 open，第二个句柄只做控制，
    //   不会打断正在跑的取流（v4l2-ctl 也是这么干的）。
    int   ctrl_fd_        = -1;

    // ---- 【新增】掉线自恢复的「刹车」（防止重开风暴）----
    //   原来一读帧失败就立刻 release()+open() 重开设备，没有退避也没有次数上限。
    //   相机出流本来就不稳时会变成「失败→重开→失败→重开」的循环，持续反复冲击
    //   USB 设备，反而可能把相机搞进坏状态。下面这几个计数就是给它加刹车。
    int   read_fail_cnt_      = 0;  // 连续读帧失败次数（读到帧就清零）
    int   reconnect_fail_cnt_ = 0;  // 连续重连后仍然读不到帧的次数

    static constexpr int READ_FAIL_TOLERANCE = 10;  // 前 10 次失败先容忍（UVC 偶发丢帧正常）
    static constexpr int RECONNECT_LIMIT     = 3;   // 最多连续重连 3 次
    static constexpr int STREAM_CHECK_SEC    = 10;  // 启动时最多等多少秒出流

    void applyExposureConfig();     // 按配置下发一次（只在 cameraInit 里调，见实现说明）
    int  ensureCtrlFd();            // 惰性打开控制通道，返回 fd（失败返回 -1）
    bool waitFirstFrame();          // 启动时校验「真的能读到帧」，带看门狗
    void printStreamTroubleshooting();  // 打印出不了流时的排查步骤（多处共用）

    cv::VideoCapture cap_;          // OpenCV 采集对象
    bool  opened_ = false;
};

#endif // UVC_CAMERA_HPP_
