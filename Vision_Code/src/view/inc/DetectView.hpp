#ifndef DETECT_VIEW_HPP_                                             // 防止头文件被重复包含（保护宏开始）
#define DETECT_VIEW_HPP_                                             // 定义保护宏

// ============================================================================
// DetectView.hpp —— 识别结果显示窗口（Dart 的"眼睛"）
// ----------------------------------------------------------------------------
// 【它解决什么问题】
//   在这之前 Dart 唯一的窗口是原版比赛代码留下的 Mouse 调试框（main.cpp 里那个
//   标题叫 "mouse" 的窗口）。那个窗口是"用鼠标手动画裁剪框"的工具：
//   它从队列里只取 ImageInfo.ori_mat（原图），把识别结果 lights 直接丢掉
//   （Mouse.cpp:27 只读 ori_mat，MouseInfo 结构体里连 lights 字段都没有）。
//   所以运行 Dart 永远看不到识别框 —— 屏幕上那个跟着鼠标动的白框，
//   只是存在 config/json/keep.json 里的 4 个死数字，和识别没有任何关系。
//
//   这个类就是补上缺的那一环：把识别结果真正画出来。
//
// 【为什么必须单独开一个线程，而不是在识别线程里直接 imshow】
//   线程之间用的 LfStack 是【无界队列】——LockFree.inl 里 push() 每次都 new 一个
//   节点，满了不阻塞、只增长。如果在识别线程里做 imshow，识别线程会变慢，
//   而取图线程照旧全速 push，积压的帧（800x600x3 ≈ 1.4MB/帧）会把内存吃光。
//   所以显示独立成一个线程：识别线程只管算，算完把结果丢进 view_info_stack
//   （覆盖式写法，队列里永远只有最新一帧），显示线程慢慢画。
//
// 【按键】（窗口获得焦点时按）
//   m         显示 / 隐藏掩膜窗口
//   s         存三张图到 snapshots/ 目录：
//               snap_raw.png     原图（没画任何东西）——远程排查看这个
//               snap_detect.png  带识别框的图
//               snap_mask.png    二值掩膜（调 HSV 阈值就看它）
//   q / ESC   退出整个程序
//
// 【比赛时怎么关掉】
//   config/tuning.yaml 里 "view" 节点的 "enable" 改成 0，
//   main.cpp 就不会启动这个线程，一个窗口都不会弹，也没有任何显示开销。
// ============================================================================

#include <opencv2/opencv.hpp>                                    // OpenCV：画图、窗口、按键
#include <string>                                                // std::string
#include <vector>                                                // std::vector

#include "lockfree/LockFree.hpp"                                 // LfStack：线程间传数据的无锁队列
#include "global/Struct.hpp"                                     // ImageInfo / LightInfo 结构体
#include "color/ColorDetector.hpp"                               // 复用识别器来算掩膜
#include "MjpegServer.hpp"                                       // 【新增】MJPEG 网页推流（板子没桌面时用浏览器看）

class DetectView {                                                   // 定义"识别显示窗口"类
public:
    DetectView();                                                    // 构造函数：读配置、建窗口、建掩膜识别器
    ~DetectView();                                                   // 析构函数：释放识别器、关窗口

    void run(LfStack<ImageInfo> &view_info_stack);                   // 主循环体：取一帧结果 → 画 → 显示/推流 → 处理按键

private:
    void drawOverlay(cv::Mat &canvas,                                // 把识别框 / 解算角度 / 状态栏画到图上
                     const ImageInfo &info);                         // 入参：这一帧的全部信息（识别结果 + 角度）
    void handleKey(int key);                                         // 处理按键
    void saveSnapshots();                                            // 按 s 时存三张图
    void drawWaiting();                                              // 还没收到相机帧时，画"正在等待相机"的提示图

    bool show_window_ = true;                                        // 【新增】是否开本地窗口（板子上没桌面，要设 false）
    bool show_mask_ = false;                                         // 是否显示掩膜窗口
    ColorDetector *mask_detector_ = nullptr;                         // 专门用来算掩膜的识别器（参数与识别线程完全一致）
    MjpegServer *web_ = nullptr;                                     // 【新增】网页推流服务器（为空 = 不推流）

    cv::Mat last_raw_;                                               // 最近一帧原图（按 s 存图时用）
    // 【省内存/省 CPU】画布做成复用缓冲，而不是每帧 clone 一份新的。
    //   原来每帧 info.ori_mat.clone() 都要分配+拷贝 1.44MB（800x600x3），
    //   60fps 下就是 86MB/s 的内存带宽。改成 copyTo 到同一个 Mat：
    //   尺寸不变时会复用已有内存，只在第一帧分配一次。
    //   （按 s 存图也直接用它 —— 它始终是最近一帧画好的图）
    cv::Mat canvas_buf_;                                             // 画框用的复用画布
    std::vector<LightInfo> last_lights_;                             // 最近的识别结果

    long frames_ = 0;                                                // 显示过多少帧（算帧率用）
    double t0_ = 0.0;                                                // 起始时刻（cv::getTickCount 的滴答数）
    // 【限流】上次画"正在等待相机"提示的时刻。
    //   那个提示画面是静态的（只有秒数在变），每 200ms 重画一次就够。
    //   原来每轮循环都画一次，而循环是 5ms 一轮 —— 等于每秒 imshow 200 次，
    //   实测会把 CPU 烧到 187%（1~2 个核白转）。在 4 核的板子上这是致命的。
    double last_wait_draw_tick_ = 0.;

    // 【新增】还没收到相机帧时显示的底图
    //   为什么需要：这个窗口是在构造函数里建的，而相机出流要等几秒
    //   （UvcCamera 启动时最多等 10 秒）。这段时间窗口是【纯黑、一个字都没有】的，
    //   看起来就像"程序坏了"或者"相机黑屏"，完全没法判断卡在哪一步。
    //   有了这张底图，窗口会明确写出 "WAITING FOR CAMERA" 和已经等了多少秒。
    cv::Mat placeholder_;

    // 【新增】十字准星画在哪 —— 用【标定出来的光心】，不是画面几何中心
    //   相机装在枪口正中心 → 枪口轴线 ≈ 相机光轴 → 枪口会打中的那个点，
    //   在画面上就是【光心】的位置，不是画面正中间。
    //   这两个数不一样：实测 cx=641.27 / cy=327.77，
    //   而 1280x720 的几何中心是 640 / 360 —— 水平差 1.3px（无所谓），
    //   竖直差 32px（肉眼看得出来）。准星画错位置的话，
    //   你照着准星去对目标会系统性偏高 32 像素。
    //   数值从 tuning.yaml 的 camera 节点读（就是标定出来的那组）。
    double cross_x_ = 0.;                                            // 准星 x（= camera.cx）
    double cross_y_ = 0.;                                            // 准星 y（= camera.cy）

    // 窗口标题必须是纯英文 —— 窗口标题栏用的也是 Hershey 那套字体，中文会变成问号
    static constexpr const char *kWinMain = "detect  [m]mask [s]save [q]quit";  // 主窗口
    static constexpr const char *kWinMask = "mask (white = green area)";         // 掩膜窗口
};                                                                   // 类定义结束

#endif // DETECT_VIEW_HPP_                                           // 保护宏结束
