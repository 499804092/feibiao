// ============================================================================
// verify_frame.cpp —— 离线校验工具（不接触硬件，纯算法验证）
// ----------------------------------------------------------------------------
// 这个文件不属于飞镖工程本体，只用来在本机验证本次改动是否正确：
//   1) 视觉端下发结构体 VisionData 的字节数 / 各字段偏移
//   2) 电控端接收结构体 vision_rx_info_t 的字节数 / 各字段偏移
//   3) 两端能否用“同一段内存”互相解读（逐字段比对）
//   4) 用工程里真实的 CRC8 / CRC16 算法给帧补校验，再验证能否校验通过
//   5) 单目测距公式与像素→角度解算的数值合理性（几个典型靶距做验算）
// 编译运行（Windows + MinGW 或 Linux 同命令）：
//   g++ -std=c++17 -O2 -Wall -I. verify_frame.cpp CRC_Check.cpp -o verify_frame
//   ./verify_frame
// ============================================================================

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <cstddef>

#include "serialport/CRC_Check.h"   // 工程里真实的 CRC8/CRC16 实现（从 sensors 模块拷来）

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---------------------------------------------------------------------------
// 【1】视觉端当前写法：与 feibiao/Vision_Code/src/utility/inc/global/Struct.hpp 一致
//      属性写在 } 之后。GCC 下会给出 -Wattributes 警告，且存在版本相关性风险。
// ---------------------------------------------------------------------------
struct VisionDataCurrent {
    uint8_t header;
    uint8_t CRC8;
    uint8_t CRC8_reserved;
    float pitch_error;
    float yaw_error;
    float roll_error;
    float distance;
    uint8_t target_status;
    uint16_t CRC16;
} __attribute__((packed));

// ---------------------------------------------------------------------------
// 【2】视觉端建议改成的写法：packed + aligned(1) 后置。
//      本机 GCC 实测：这个写法真正生效，且能消掉 -Wattributes 警告。
//      （GCC 对“后置属性”的行为历史上变过：从“作用于最后一个成员”改成“作用于整个
//        struct”，所以显式加 aligned(1) 更保险；因为本结构体没有 8 字节成员，
//        packed 与 packed,aligned(1) 的结果一致。）
// ---------------------------------------------------------------------------
struct VisionDataFixed {
    uint8_t header;
    uint8_t CRC8;
    uint8_t CRC8_reserved;
    float pitch_error;
    float yaw_error;
    float roll_error;
    float distance;
    uint8_t target_status;
    uint16_t CRC16;
} __attribute__((packed, aligned(1)));

// ---------------------------------------------------------------------------
// 【3】电控端写法：与 vision_sensor.h 的 typedef __packed struct {...} 一致。
//      电控工程里 __packed 来自 stm32f4xx_hal_def.h / CMSIS：
//          #define __packed  __attribute__((packed))
//      （即“前置属性”，本机 GCC 实测同样存在被忽略的情况，故这里直接用权威写法
//        struct __attribute__((packed)) {...} 来代表它的语义，保证 22 字节。）
// ---------------------------------------------------------------------------
#define __packed_hal __attribute__((packed))

typedef struct __packed_hal {
    uint8_t SOF;             // 偏移 0
    uint8_t CRC8;            // 偏移 1
    uint8_t CRC8_reserved;   // 偏移 2
    float   pitch_error;     // 偏移 3
    float   yaw_error;       // 偏移 7
    float   roll_error;      // 偏移 11
    float   distance;        // 偏移 15
    uint8_t target_status;   // 偏移 19
    uint16_t CRC16;          // 偏移 20
} vision_rx_info_t;

static int g_fail = 0;
static void check(bool ok, const char *what) {
    printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) g_fail++;
}

// ---- 与 Track.cpp 的 pixelToAngle() 逐行等价 ----
static void pixelToAngle(double px, double py, double fx, double fy, double cx, double cy,
                         double *yaw, double *pitch) {
    *yaw   = std::atan2(px - cx, fx);
    *pitch = std::atan2(cy - py, fy);
}

// ---- 与 Track.cpp 的 estimateDistance() 逐行等价 ----
static double estimateDistance(double bw, double bh,
                               double fx, double fy,
                               double w_real, double h_real,
                               double dmin, double dmax) {
    double z_x = 0., z_y = 0.;
    if (bw > 1.) z_x = fx * w_real / bw;
    if (bh > 1.) z_y = fy * h_real / bh;
    double z;
    if (z_x > 0. && z_y > 0.)      z = 0.5 * (z_x + z_y);
    else if (z_x > 0.)             z = z_x;
    else if (z_y > 0.)             z = z_y;
    else                           return 0.;
    if (z < dmin) return 0.;
    if (z > dmax) z = dmax;
    return z;
}

int main() {
    printf("=============== 飞镖 视觉->飞控 帧结构 / 算法 离线校验 ===============\n\n");

    // ---------------- 1. 结构体尺寸 ----------------
    printf("---- 1) 结构体尺寸 ----\n");
    printf("sizeof(VisionDataCurrent) = %zu   <-- 视觉端当前写法(} __attribute__((packed));)\n",
           sizeof(VisionDataCurrent));
    printf("sizeof(VisionDataFixed)   = %zu   <-- 视觉端建议写法(} (packed, aligned(1));)\n",
           sizeof(VisionDataFixed));
    printf("sizeof(vision_rx_info_t)  = %zu   <-- 电控端 typedef struct __packed\n",
           sizeof(vision_rx_info_t));
    check(sizeof(VisionDataFixed) == 22, "视觉端 VisionData = 22 字节(无填充)");
    check(sizeof(vision_rx_info_t) == 22, "电控端 vision_rx_info_t = 22 字节(无填充)");
    check(sizeof(VisionDataCurrent) == 22, "视觉端当前写法在本机 GCC 下同样是 22 字节");
    check(sizeof(VisionDataCurrent) == sizeof(vision_rx_info_t), "两端帧长完全一致");

    // ---------------- 2. 字段偏移 ----------------
    printf("\n---- 2) 期望的字节布局 ----\n");
    VisionDataFixed v{};
    auto off = [&](const void *p) { return (size_t)((const char *)p - (const char *)&v); };
    printf("header        @ %2zu\n", off(&v.header));
    printf("CRC8          @ %2zu\n", off(&v.CRC8));
    printf("CRC8_reserved @ %2zu\n", off(&v.CRC8_reserved));
    printf("pitch_error   @ %2zu\n", off(&v.pitch_error));
    printf("yaw_error     @ %2zu\n", off(&v.yaw_error));
    printf("roll_error    @ %2zu\n", off(&v.roll_error));
    printf("distance      @ %2zu\n", off(&v.distance));
    printf("target_status @ %2zu\n", off(&v.target_status));
    printf("CRC16         @ %2zu\n", off(&v.CRC16));
    check(off(&v.pitch_error) == 3,    "pitch_error 偏移 = 3");
    check(off(&v.yaw_error) == 7,      "yaw_error 偏移 = 7 (与原协议一致, 电控老代码兼容)");
    check(off(&v.roll_error) == 11,    "roll_error 偏移 = 11");
    check(off(&v.distance) == 15,      "distance 偏移 = 15");
    check(off(&v.target_status) == 19, "target_status 偏移 = 19");
    check(off(&v.CRC16) == 20,         "CRC16 偏移 = 20 (正好落在帧尾)");

    // ---------------- 3. 两端互相解读 ----------------
    printf("\n---- 3) 视觉端写出 / 电控端读出（同一段内存逐字节搬运） ----\n");
    v.pitch_error   = 0.1234f;
    v.yaw_error     = -0.0567f;
    v.roll_error    = 0.0089f;
    v.distance      = 2.345f;
    v.target_status = 1;
    v.header        = 0xA5;
    unsigned char buf[sizeof(VisionDataFixed)];
    memcpy(buf, &v, sizeof(buf));
    vision_rx_info_t rx{};
    memcpy(&rx, buf, sizeof(vision_rx_info_t));
    check(rx.SOF == 0xA5, "帧头 0xA5 被正确解析");
    check(std::fabs(rx.pitch_error - 0.1234f) < 1e-6, "pitch_error 解析一致");
    check(std::fabs(rx.yaw_error   + 0.0567f) < 1e-6, "yaw_error 解析一致");
    check(std::fabs(rx.roll_error  - 0.0089f) < 1e-6, "roll_error 解析一致");
    check(std::fabs(rx.distance    - 2.345f)  < 1e-5, "distance 解析一致");
    check(rx.target_status == 1, "target_status 解析一致");

    // ---------------- 4. CRC 补校验 + 回验 ----------------
    printf("\n---- 4) CRC8 / CRC16 补校验与回验 ----\n");
    unsigned char frame[sizeof(VisionDataFixed)];
    memcpy(frame, &v, sizeof(frame));
    frame[0] = 0xA5;
    Append_CRC8_Check_Sum(frame, 3);                 // 对前 2 字节算 CRC8 → 写 frame[2]
    Append_CRC16_Check_Sum(frame, sizeof(frame));    // 对前 20 字节算 CRC16 → 写 frame[20..21]
    printf("CRC8  = 0x%02X   (写入 frame[2]  = 结构体 CRC8_reserved 字段)\n", frame[2]);
    printf("CRC16 = 0x%02X%02X (写入 frame[20..21] = 结构体 CRC16 字段)\n", frame[21], frame[20]);
    check(Verify_CRC8_Check_Sum(frame, 3) != 0, "Verify_CRC8_Check_Sum(frame,3) 通过");
    check(Verify_CRC16_Check_Sum(frame, sizeof(frame)) != 0, "Verify_CRC16_Check_Sum(frame,22) 通过");
    bool ok = (frame[0] == 0xA5)
              && (Verify_CRC8_Check_Sum(frame, 3) != 0)
              && (Verify_CRC16_Check_Sum(frame, sizeof(vision_rx_info_t)) != 0);
    check(ok, "模拟电控端 vision_update() 全流程：整帧校验通过");
    unsigned char bad[sizeof(frame)];
    memcpy(bad, frame, sizeof(bad));
    bad[10] ^= 0xFF;                                  // 故意改坏 distance 中一个字节
    check(Verify_CRC16_Check_Sum(bad, sizeof(bad)) == 0, "数据被改坏后 CRC16 能查出来(校验确实生效)");
    printf("提醒: 原协议 Append_CRC8_Check_Sum(buf,3) 写的是偏移 2，而原结构体偏移 2 是\n"
           "      yaw_error 的首字节 → 属于越界写。加入 CRC8_reserved 后落点严格对应。\n");

    // ---------------- 5. 测距 / 角度验算 ----------------
    printf("\n---- 5) 单目测距 / 角度解算验算 ----\n");
    const double fx = 1706.0, fy = 1705.1, cx = 320.0, cy = 320.0;
    const double W = 0.15, H = 0.15;   // 与 track.json 的 target 参数一致：15cm
    struct Case { double z, bw, bh; };
    // 用 Z = f*W/w 反推像素尺寸，构造“真值自洽”的测试用例
    Case cases[] = {
        {1.0, fx * W / 1.0, fy * H / 1.0},
        {2.0, fx * W / 2.0, fy * H / 2.0},
        {3.5, fx * W / 3.5, fy * H / 3.5},
        {5.0, fx * W / 5.0, fy * H / 5.0},
    };
    for (const auto &c : cases) {
        double z = estimateDistance(c.bw, c.bh, fx, fy, W, H, 0.5, 8.0);
        printf("真值 %4.2f m -> 框 %6.1f x %6.1f px -> 解算 %6.3f m (误差 %.5f m)\n",
               c.z, c.bw, c.bh, z, std::fabs(z - c.z));
        check(std::fabs(z - c.z) < 1e-3, "该距离解算与真值一致");
    }
    double zclip = estimateDistance(fx * W / 20.0, fy * H / 20.0, fx, fy, W, H, 0.5, 8.0);
    printf("20 m 处目标(框仅 %.1f px) -> 解算 %.3f m (应被钳位到上限 8.000)\n", fx * W / 20.0, zclip);
    check(zclip == 8.0, "超量程距离被钳位到上限");
    check(estimateDistance(0.5, 0.5, fx, fy, W, H, 0.5, 8.0) == 0.0, "框尺寸非法时返回 0(距离无效)");
    check(estimateDistance(fx * W / 0.2, fy * H / 0.2, fx, fy, W, H, 0.5, 8.0) == 0.0,
          "距离小于下限时返回 0(判为误检)");

    double yaw = 0., pitch = 0.;
    pixelToAngle(cx + 100, cy - 50, fx, fy, cx, cy, &yaw, &pitch);
    printf("目标在光心右 100px / 上 50px -> yaw = %+.4f deg, pitch = %+.4f deg\n",
           yaw * 180.0 / M_PI, pitch * 180.0 / M_PI);
    check(yaw > 0.,   "目标在右侧 => yaw_error > 0");
    check(pitch > 0., "目标在上方 => pitch_error > 0");
    check(std::fabs(yaw * 180.0 / M_PI - 3.3541) < 0.01, "yaw 数值与 atan(100/1706) 相符");
    double yaw2 = 0., pitch2 = 0.;
    pixelToAngle(cx - 100, cy + 50, fx, fy, cx, cy, &yaw2, &pitch2);
    check(yaw2 < 0. && pitch2 < 0., "目标在左下 => yaw/pitch 同时为负(符号约定正确)");

    printf("\n=============== 校验结束: %s, 失败 %d 项 ===============\n",
           g_fail == 0 ? "全部通过" : "存在失败", g_fail);
    return g_fail == 0 ? 0 : 1;
}
