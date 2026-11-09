#ifndef __GLOBAL_STRUCT_HPP__
#define __GLOBAL_STRUCT_HPP__

#include <opencv2/opencv.hpp>
#include "Time_generated.h"

// ============================================================================
// 【核心】下发给飞控板的数据结构体
//  原代码只下发 yaw_error(偏航角误差) + target_status(目标状态)；
//  现在按要求下发：pitch(俯仰角误差) + yaw(偏航角误差) + distance(到靶心直线距离)
// ----------------------------------------------------------------------------
// 字节布局（共 18 字节，小端序；4 字节 float 落在偏移 3/7/11，天然 4 字节边界，
//           即使不做 packed 也不会有填充，视觉端与飞控端 sizeof 完全一致）：
//   偏移  0 : uint8_t  header           = 0xA5              帧头
//   偏移  1 : uint8_t  CRC8                                 帧头区 CRC8 字节1
//   偏移  2 : uint8_t  CRC8_reserved                        帧头区 CRC8 字节2
//   偏移  3 : float    pitch_error                           俯仰角误差 (rad)
//   偏移  7 : float    yaw_error                             偏航角误差 (rad)
//   偏移 11 : float    distance                              到目标直线距离 (m)
//   偏移 15 : uint8_t  target_status                          0=丢失 1=找到
//   偏移 16 : uint16_t CRC16                                 整帧 CRC16
//   → 合计 18 字节
//
// 【roll 已整体删除】原来这里还有一个 roll_error(横滚角误差)，已经去掉。
//   为什么删：本工程的检测输出是【轴对齐矩形框】(cv::Rect2d)，框里根本没有倾角
//   信息（倾角是 cv::RotatedRect 才有的）。原来的 estimateRoll() 是拿"长宽比偏离 1"
//   硬凑的近似量 —— 实测对一个正对着的【圆形】引导灯会算出 -1.34° 这种虚假滚转
//   （纯粹是框 51x55 不等宽造成的），真滚转了反而测不出来。
//   既不准、又没被使用（电控那边只取 yaw 和 target_status），属于死代码。
//   高精度 roll 应该来自 IMU 姿态；或者把检测改成 RotatedRect 后取真实倾角。
//
// ⚠️ 协议尚未与电控最终确定。如果以后要把 roll 加回来，
//    请【追加在 distance 之后】，不要插在中间 —— 插在中间会让后面所有字段偏移，
//    电控侧的 memcpy 全部要对齐着改。
//
// 说明1：原 struct 里只有一个 uint8_t CRC8 字段，却调用
//        Append_CRC8_Check_Sum(buf, 3) 写“前 3 字节”，等于 CRC8 越界写进了
//        yaw_error 的首字节；靠“前 3 字节正好是 帧头+2 字节”才勉强能跑通。
//        这里显式补出第 3 个字节 CRC8_reserved，使“帧头区 = header + 2 字节 CRC8”
//        与电控侧 Verify_CRC8_Check_Sum(rxBuf, 3) 严格一一对应，不再越界。
// 说明2：属性写在 `}` 之后的写法（} __attribute__((packed)); ）在 GCC 里会给出
//        “'packed' attribute ignored” 警告，而且历史上 GCC 对该位置属性的处理
//        反复变过（早期作用于最后一个成员，后来作用于整个 struct），存在版本风险。
//        因此这里改成显式 (packed, aligned(1))：语义确定，且实测无警告、就是 18 字节。
//        即使将来换成不做 packed 的编译器，本结构体也不会有填充，仍然安全。
// ============================================================================
struct VisionData {
    uint8_t header = 0xA5;      // 帧头，固定 0xA5，电控侧靠它做帧同步
    uint8_t CRC8 = 0;           // 帧头区 CRC8 字节1（由 Append_CRC8_Check_Sum 填）
    uint8_t CRC8_reserved = 0;  // 帧头区第 3 字节，让帧头区固定 3 字节

    float pitch_error = 0.f;    // 俯仰角误差(rad)：目标在图像中的竖直偏差
                                //   约定 正 = 目标偏上(需抬高枪口)，负 = 目标偏下
    float yaw_error = 0.f;      // 偏航角误差(rad)：目标在图像中的水平偏差
                                //   约定 正 = 目标偏右(需右转)，负 = 目标偏左
    float distance = 0.f;       // 到目标的直线距离(m)，单目测距得到

    uint8_t target_status = 0;  // 目标状态 0: Lost(丢失) 1: Found(找到)
    uint16_t CRC16 = 0;         // 整帧 CRC16，电控侧用它判断整帧是否完整
}__attribute__((packed, aligned(1)));   // 取消对齐填充+按 1 字节对齐，保证与飞控板字节级一致

// publish
struct CarData {
    uint8_t header = 0xA5;
    uint8_t mode; // 0: 不开自瞄, 不录像 1: 开自瞄且录像 2: 录像
    uint8_t CRC8;

    uint8_t status;
    uint8_t number; // 第number号飞镖
    uint8_t dune; //0：完全展开 1：完全关闭 2：正在进行时
    uint16_t CRC16;

}__attribute__((packed));

typedef struct {
    CarData cd;
    foxglove::Time timestamp;
    cv::Mat img;
} FromData;

typedef struct {
    cv::Point2d center_point;   // 目标中心点在原图上的像素坐标
    cv::Rect2d box;             // 目标外接矩形(原图像素系)
    double score;               // 置信度
    // 【已删除】原来这里有一个 angle 字段（目标框倾角，用于横滚 roll 解算）。
    //   删掉的原因：检测输出的是轴对齐矩形框，本来就算不出真实倾角，
    //   而 roll 已经整体去掉，这个字段没有任何地方会读它。
} LightInfo;

typedef struct {
    std::vector<LightInfo> lights;
    foxglove::Time timestamp;

    cv::Mat ori_mat;
    CarData cd;

    // 解算结果（下发给飞控的 pitch / yaw / distance）
    //   识别线程 Detect 往外发的时候这里还是全 0（角度还没算）；
    //   跟踪线程 Track 算完之后会把它填上，再发给识别显示线程 DetectView。
    //   这样显示窗口一个画面里就能同时看到「识别框」和「解算结果」，
    //   不用再让 Track 自己单开一个窗口（两个线程同时调 OpenCV 界面不安全）。
    VisionData vd;

} ImageInfo;


typedef struct {
    std::deque<LightInfo> vec;
    double drop_frame;
    double found_frame;
    int state;
}TargetCache;

typedef struct {
    CarData cd;
    VisionData vd;
    foxglove::Time timestamp;
    cv::Mat img;
} CaptureInfo;

typedef struct {
    cv::Mat raw_image;
} RawImageInfo;


#endif // __GLOBAL_STRUCT_HPP__