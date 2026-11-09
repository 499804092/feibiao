// ============================================================================
// set_exposure.cpp —— 不打开取流、只下发曝光设置的小工具
// ----------------------------------------------------------------------------
// 用途：
//   1) 验证相机到底接不接受某个曝光值（走的是和 UvcCamera 完全一样的代码路径）
//   2) 在不开预览窗口的情况下直接把曝光设好
//
// 【用法】
//   ./set_exposure                 # 默认「手动 + 40」
//   ./set_exposure 80              # 「手动 + 80」
//   ./set_exposure 80 1            # 「手动 + 80」，并把 auto_priority 设成 1
//   ./set_exposure 80 0            # 「手动 + 80」，priority=0（不推荐，见下）
//   ./set_exposure 0               # 恢复相机默认（自动曝光）
//   参数顺序： ./set_exposure <曝光值> [auto_priority] [设备号]
//              auto_priority 省略时 = -1，表示「不动这一项」（推荐）
//
// 【重要】auto_priority 千万别随便关！
//   本相机 MJPG 640x480 只支持 0.008s(120fps) 这一档帧间隔（见
//   v4l2-ctl --list-formats-ext）。一旦把 auto_priority 设为 0，
//   uvcvideo 会按曝光值去反推帧间隔，推出来的值不在支持列表里 →
//   驱动设置帧率失败 → 取流直接起不来，表现就是 OpenCV 一直报
//   select() timeout、程序像卡死一样（这个坑已经踩过一次）。
// ============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../../Vision_Code/src/sensors/inc/uvc/V4l2Ctrl.hpp"

int main(int argc, char **argv) {
    const int want = (argc > 1) ? atoi(argv[1]) : 40;
    const int prio = (argc > 2) ? atoi(argv[2]) : -1;   // -1 = 不改这一项
    const int dev  = (argc > 3) ? atoi(argv[3]) : 0;

    printf("=========== 曝光下发工具（不取流）===========\n");
    int fd = v4l2ctrl::openDevice(dev);
    if (fd < 0) {
        printf("❌ 打不开 /dev/video%d\n", dev);
        return 1;
    }
    printf("已打开 /dev/video%d 的控制通道   (曝光=%d, priority=%d)\n\n", dev, want, prio);

    printf("---- 下发之前 ----\n");
    v4l2ctrl::printAllRelevant(fd);

    if (want <= 0) {
        // 恢复相机默认：自动曝光
        printf("\n---- 恢复自动曝光（auto_exposure=3, auto_priority=1）----\n");
        printf("  %s auto_exposure -> 3\n",
               v4l2ctrl::set(fd, V4L2_CID_EXPOSURE_AUTO, 3) ? "✓" : "✗");
        printf("  %s exposure_auto_priority -> 1\n",
               v4l2ctrl::set(fd, V4L2_CID_EXPOSURE_AUTO_PRIORITY, 1) ? "✓" : "✗");
        printf("  %s exposure_time_absolute -> 157\n",
               v4l2ctrl::set(fd, V4L2_CID_EXPOSURE_ABSOLUTE, 157) ? "✓" : "✗");
    } else {
        printf("\n---- 下发「手动 + %d」----\n", want);

        // ① 先切手动（必须，否则曝光值会被驱动忽略）
        bool ok1 = v4l2ctrl::set(fd, V4L2_CID_EXPOSURE_AUTO, V4L2_EXPOSURE_MANUAL);
        printf("  %s auto_exposure -> 1 (手动)\n", ok1 ? "✓" : "✗");

        // ② auto_priority：只有显式指定才写
        //    ★ 不要偷偷设成 0 —— 会让 640x480 MJPG 取流直接起不来
        if (prio >= 0) {
            bool ok2 = v4l2ctrl::set(fd, V4L2_CID_EXPOSURE_AUTO_PRIORITY, prio);
            printf("  %s exposure_auto_priority -> %d\n", ok2 ? "✓" : "✗", prio);
        } else {
            printf("  -  exposure_auto_priority 不动（保持驱动默认，推荐）\n");
        }

        // ③ 再写曝光值
        bool ok3 = v4l2ctrl::set(fd, V4L2_CID_EXPOSURE_ABSOLUTE, want);
        printf("  %s exposure_time_absolute -> %d\n", ok3 ? "✓" : "✗", want);
    }

    printf("\n---- 下发之后（读回相机真实值）----\n");
    v4l2ctrl::printAllRelevant(fd);

    int a = -1, v = -1, p = -1;
    v4l2ctrl::get(fd, V4L2_CID_EXPOSURE_AUTO,          a);
    v4l2ctrl::get(fd, V4L2_CID_EXPOSURE_ABSOLUTE,      v);
    v4l2ctrl::get(fd, V4L2_CID_EXPOSURE_AUTO_PRIORITY, p);

    printf("\n---- 判定 ----\n");
    printf("  auto_exposure          = %d  %s\n", a,
           a == V4L2_EXPOSURE_MANUAL ? "(手动，正确)" : "(不是手动！)");
    printf("  exposure_time_absolute = %d\n", v);
    printf("  exposure_auto_priority = %d  %s\n", p,
           p == 0 ? "(已关闭 —— 注意：这会让 640x480 MJPG 取流起不来!)"
                  : "(开着，正常)");

    if (want > 0) {
        if (a == V4L2_EXPOSURE_MANUAL && v == want) {
            printf("\n✅ 成功：相机已接受「手动 + 曝光 %d」\n", want);
            printf("   现在可用 ./uvc_detect_view --no-cfg 测一下能不能出流\n");
        } else {
            printf("\n⚠️ 没完全生效：期望 auto=1 value=%d，实际 auto=%d value=%d\n",
                   want, a, v);
        }
    } else {
        printf("\n✅ 已恢复相机出厂默认\n");
    }

    v4l2ctrl::closeDevice(fd);
    return 0;
}
