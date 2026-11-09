#ifndef __ARMOR_DETECT_HPP__                                         // 防止头文件被重复包含（保护宏开始）
#define __ARMOR_DETECT_HPP__                                         // 定义保护宏

#include <opencv2/opencv.hpp>                                        // OpenCV 主头文件
#include <deque>                                                     // std::deque 双端队列（成员里用到了）

#include "lockfree/LockFree.hpp"                                     // 无锁队列（线程间传数据用）
#include "color/ColorDetector.hpp"                                   // 传统视觉识别器：HSV 找绿色引导灯
#include "global/Struct.hpp"                                         // 工程通用结构体：FromData / ImageInfo / LightInfo

// ============================================================================
// Detect —— 识别线程
//   职责：从取图队列拿一帧 → 用 ColorDetector 找引导灯 → 转成 LightInfo
//         分发给跟踪线程和识别显示线程
//
//   【本任务改动】原来的 OpenVINO 模型识别（OpenvinoInfer）已整体删除，
//   现在只用传统视觉（HSV 颜色分割 + 形状筛选），不依赖任何模型文件。
// ============================================================================
class Detect {                                                       // 定义识别线程类
public:
    explicit Detect(const char *mode);                               // 构造函数：mode 是调试模式标识（如 "debug"）
    Detect() = delete;                                               // 禁止无参构造（必须传 mode）
    ~Detect();                                                       // 析构函数：释放识别器

    void detect(                                                     // 核心接口：处理一帧（在识别线程里循环调用）
            LfStack<FromData> &message_stack,                        // 入：取图线程 → 识别线程 的队列
            LfStack<ImageInfo> &image_info_stack);                   // 出：识别线程 → 跟踪线程 的队列
                                                                     // 【改动】原来还有第三个参数 view_info_stack（识别线程 → 显示线程）。
                                                                     //   现在显示线程改由 Track 来喂 —— Track 手里才有解算出的角度，
                                                                     //   由它统一推一帧过去，显示窗口一屏能同时看到识别框和角度。

private:
    ColorDetector *color_detector_ = nullptr;                        // 识别器指针（构造时 new，析构时 delete）

    std::deque<double> time_minus_queue_;                            // 帧间隔队列（历史遗留，当前逻辑没用到）

    std::string mode_;                                               // 调试模式标识
};                                                                   // 类定义结束

#endif // __ARMOR_DETECT_HPP__                                       // 保护宏结束
