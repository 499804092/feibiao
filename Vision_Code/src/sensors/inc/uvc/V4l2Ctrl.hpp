/**
 ******************************************************************************
 * @file    V4l2Ctrl.hpp
 * @brief   UVC 摄像头「曝光 / 增益 / 亮度」等 V4L2 控制项读写（纯头文件，不用改 CMake）
 ******************************************************************************
 * 【为什么自己写 V4L2 ioctl，而不用 OpenCV 的 cap.set()】
 *   cv::CAP_PROP_EXPOSURE / CAP_PROP_AUTO_EXPOSURE 在 OpenCV 的 V4L2 后端里
 *   映射一直很混乱（不同 OpenCV 版本把它们映射到不同的 V4L2 CID），实测在这台
 *   相机的 uvcvideo 驱动上经常「设了没反应」。
 *   而 v4l2-ctl 用的就是下面这套 ioctl，在本机 /dev/video0 上已实测可用，
 *   所以这里直接照搬同一套机制，行为与 v4l2-ctl 完全一致。
 *
 * 【为什么不会打断正在跑的取流】
 *   V4L2 允许同一个 /dev/videoN 被多次 open。这里开的第二个 fd 只用来
 *   下发/查询控制项，不做 mmap、不做 STREAMON，所以不会影响 OpenCV 正在跑的
 *   取流（v4l2-ctl 也是这么干的：可以一边预览一边调曝光）。
 *
 * 【两种用法的区别，调参前一定要知道】
 *   - V4L2_CID_EXPOSURE_AUTO  是「菜单型」控制：
 *         1 = 手动曝光（这时 exposure_time_absolute 才听你的）
 *         3 = 自动/光圈优先（这时你写 exposure_time_absolute 会被驱动忽略！）
 *     ★ 所以调手动曝光的正确顺序永远是：先写成 1，再写曝光值。顺序反了必然没反应。
 *   - V4L2_CID_EXPOSURE_ABSOLUTE（v4l2-ctl 里叫 exposure_time_absolute）
 *     单位是 100 微秒。值越小画面越暗，噪声也越少。
 ******************************************************************************
 */

#ifndef V4L2_CTRL_HPP_
#define V4L2_CTRL_HPP_

#include <linux/videodev2.h>    // V4L2 控制项 ID 定义
#include <sys/ioctl.h>          // ioctl()
#include <fcntl.h>              // open()
#include <unistd.h>             // close()
#include <cstdio>
#include <cstring>
#include <string>

namespace v4l2ctrl {

// ============================================================================
// 打开 /dev/videoN 的「控制通道」
//   返回 >= 0 的 fd 表示成功；-1 表示打不开（设备不存在、没插好、权限不够）
//   注意：这个 fd 和 OpenCV 的取流 fd 是两个独立的东西，可以同时存在。
// ============================================================================
inline int openDevice(int index) {
    char path[64];
    snprintf(path, sizeof(path), "/dev/video%d", index);

    int fd = ::open(path, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        // 某些驱动不允许 O_RDWR 二次打开，只读也能做 QUERYCTRL / S_CTRL
        fd = ::open(path, O_RDONLY | O_NONBLOCK);
    }
    return fd;
}

inline void closeDevice(int fd) {
    if (fd >= 0) {
        ::close(fd);
    }
}

// ============================================================================
// 一个控制项的信息（范围 / 步进 / 默认值 / 名字）
//   ok == false 表示这台相机不支持这个控制项
// ============================================================================
struct CtrlInfo {
    bool        ok   = false;
    int         min  = 0;
    int         max  = 0;
    int         step = 1;
    int         def  = 0;
    int         cur  = 0;
    std::string name;
    std::string type;
};

// 查询一个控制项的范围等信息（不含当前值）
inline CtrlInfo query(int fd, unsigned int id) {
    CtrlInfo info;
    if (fd < 0) {
        return info;
    }

    struct v4l2_queryctrl q;
    memset(&q, 0, sizeof(q));
    q.id = id;
    if (::ioctl(fd, VIDIOC_QUERYCTRL, &q) != 0) {
        return info;                        // 不支持这个控制项
    }
    if (q.flags & V4L2_CTRL_FLAG_DISABLED) {
        return info;                        // 存在但当前被禁用
    }

    info.ok   = true;
    info.min  = q.minimum;
    info.max  = q.maximum;
    info.step = (q.step > 0) ? q.step : 1;
    info.def  = q.default_value;
    info.name = (const char *)q.name;

    switch (q.type) {
        case V4L2_CTRL_TYPE_INTEGER: info.type = "整数"; break;
        case V4L2_CTRL_TYPE_BOOLEAN: info.type = "开关"; break;
        case V4L2_CTRL_TYPE_MENU:    info.type = "菜单"; break;
        default:                     info.type = "其它"; break;
    }
    return info;
}

// 读一个控制项的当前值
inline bool get(int fd, unsigned int id, int &value) {
    if (fd < 0) {
        return false;
    }
    struct v4l2_control c;
    memset(&c, 0, sizeof(c));
    c.id = id;
    if (::ioctl(fd, VIDIOC_G_CTRL, &c) != 0) {
        return false;
    }
    value = c.value;
    return true;
}

// 写一个控制项。返回 true 表示驱动接受了。
//   ★ 驱动会自己把值夹到 [min, max] 并对齐到 step，所以越界不会报错，
//     但你要用 get() 读回来确认到底变成了多少。
inline bool set(int fd, unsigned int id, int value) {
    if (fd < 0) {
        return false;
    }
    struct v4l2_control c;
    memset(&c, 0, sizeof(c));
    c.id    = id;
    c.value = value;
    return ::ioctl(fd, VIDIOC_S_CTRL, &c) == 0;
}

// 打印一个控制项：当前值 + 取值范围（手动调参时靠这个知道能填多少）
inline void printCtrl(int fd, unsigned int id, const char *label) {
    CtrlInfo ci = query(fd, id);
    if (!ci.ok) {
        printf("    %-26s : 本机相机不支持\n", label);
        return;
    }
    int cur = 0;
    get(fd, id, cur);
    printf("    %-26s : 当前=%-7d 范围[%d, %d] 步进=%-4d 默认=%-7d (%s)\n",
           label, cur, ci.min, ci.max, ci.step, ci.def, ci.name.c_str());
}

// ============================================================================
// 一次性打印所有和「画面亮暗 / 噪声」有关的控制项
//   调曝光前先调这个看一遍，就知道这台相机到底能调什么、范围是多少。
// ============================================================================
inline void printAllRelevant(int fd) {
    if (fd < 0) {
        printf("    ❌ 控制通道没打开，无法查询\n");
        return;
    }
    printf("  ------------------ 曝光相关 ------------------\n");
    printCtrl(fd, V4L2_CID_EXPOSURE_AUTO,         "auto_exposure(1手动/3自动)");
    printCtrl(fd, V4L2_CID_EXPOSURE_ABSOLUTE,     "exposure_time_absolute");
    printCtrl(fd, V4L2_CID_EXPOSURE_AUTO_PRIORITY,"exposure_auto_priority");
    printCtrl(fd, V4L2_CID_GAIN,                  "gain(增益)");
    printf("  ------------------ 亮度相关 ------------------\n");
    printCtrl(fd, V4L2_CID_BRIGHTNESS,            "brightness(亮度)");
    printCtrl(fd, V4L2_CID_CONTRAST,              "contrast(对比度)");
    printCtrl(fd, V4L2_CID_GAMMA,                 "gamma(伽马)");
    printCtrl(fd, V4L2_CID_SHARPNESS,             "sharpness(锐度)");
    printCtrl(fd, V4L2_CID_SATURATION,            "saturation(饱和度)");
    printCtrl(fd, V4L2_CID_BACKLIGHT_COMPENSATION,"backlight_compensation");
    printf("  ------------------ 抗闪烁 / 白平衡 ------------------\n");
    printCtrl(fd, V4L2_CID_POWER_LINE_FREQUENCY,  "power_line_frequency");
    printCtrl(fd, V4L2_CID_AUTO_WHITE_BALANCE,    "auto_white_balance");
    printCtrl(fd, V4L2_CID_WHITE_BALANCE_TEMPERATURE, "white_balance_temp");
}

// ============================================================================
// 把「手动曝光值」下发下去 —— 封装了那个最容易踩的顺序坑
//   fd       : 控制通道
//   auto_mode: V4L2 auto_exposure，1 = 手动，3 = 自动；< 0 表示不动
//   value    : 曝光绝对值（100µs 为单位）；< 0 表示不动
//   返回 true 表示至少成功写进去一项
//
// 【绝对不要在这里设 EXPOSURE_AUTO_PRIORITY = 0】
//   实测教训（本相机 Sonix 0c45:6368）：把 auto_priority 设成 0 之后，
//   640x480 MJPG 取流会彻底起不来 —— 该分辨率只支持 0.008s 这一档帧间隔，
//   而驱动会按曝光值反推帧间隔，推出来的值不在支持列表里 → 设置帧率失败 →
//   一帧都读不到。程序表现是 select() timeout / 假死，而且会"粘住"，
//   之后任何曝光值都出不了流，必须重连 USB 相机才能恢复。
// ============================================================================
inline bool applyExposure(int fd, int auto_mode, int value) {
    bool done = false;

    // ① 必须先切成手动，否则驱动会用自动曝光立刻覆盖掉你写的值
    if (auto_mode >= 0) {
        if (set(fd, V4L2_CID_EXPOSURE_AUTO, auto_mode)) {
            done = true;
        }
    }

    // ② 再写曝光绝对值（只在手动模式下才有意义）
    if (value >= 0 && (auto_mode < 0 || auto_mode == V4L2_EXPOSURE_MANUAL)) {
        if (set(fd, V4L2_CID_EXPOSURE_ABSOLUTE, value)) {
            done = true;
        }
    }
    return done;
}

} // namespace v4l2ctrl

#endif // V4L2_CTRL_HPP_
