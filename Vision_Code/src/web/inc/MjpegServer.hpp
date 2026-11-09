#ifndef MJPEG_SERVER_HPP_                                            // 防止头文件被重复包含（保护宏开始）
#define MJPEG_SERVER_HPP_                                            // 定义保护宏

// ============================================================================
// MjpegServer.hpp —— 把摄像头画面推到网页上看（MJPEG over HTTP）
// ----------------------------------------------------------------------------
// 【解决什么问题】
//   开发板上只有终端、没有桌面，看不到摄像头画面，调参和排错全靠猜。
//   这个类在板子里开一个极小的 HTTP 服务器，把画面用 MJPEG 格式推出去，
//   电脑/手机浏览器打开固定地址就能实时看画面。
//
// 【为什么用 MJPEG 而不是 RTSP / H.264】
//   参考实现（MaixCAM）用的是 RTSP + H.264 硬件编码，因为那颗芯片有硬件编码器。
//   而 RK3566 上我们只通过 OpenCV 拿 UVC 摄像头，够不到硬件编码器；
//   软编码 H.264 又需要 ffmpeg/x264 这些额外依赖，板子上不一定装得上。
//   MJPEG 就是"一连串 JPEG 图片"，浏览器原生支持，**零外部依赖**：
//     · 服务器端只要 OpenCV 的 imencode（imgcodecs 模块，本来就有）
//     · 客户端只要一个 <img> 标签
//   代价是带宽比 H.264 大（每帧都是完整 JPEG），但局域网调试完全够用：
//   640x480 质量 70 的 JPEG 约 30KB，15fps ≈ 450KB/s，WiFi 毫无压力。
//
// 【怎么用】
//   板子上跑起来之后，在电脑浏览器打开：
//       http://<板子IP>:8080/
//   这个地址是固定的，可以直接收藏。
//   想看纯画面（不带网页外壳）用 /stream，想确认服务活着用 /status。
//
// 【线程模型】
//   · 主线程（显示线程）调 publish() 把最新一帧丢进来 —— 只做加锁存一下，不阻塞
//   · 一个 accept 线程负责接收新连接，每来一个浏览器就开一个 client 线程
//   · 每个 client 线程用条件变量等"有新帧了"再发，不会空转烧 CPU
//   · 帧是"最新覆盖式"：浏览器慢就丢旧帧，永远发最新的 —— 实时画面要的就是这个
// ============================================================================

#include <opencv2/opencv.hpp>                                        // cv::Mat / cv::imencode / cv::resize

#include <atomic>                                                    // std::atomic：running_ 标志
#include <condition_variable>                                        // std::condition_variable：等新帧
#include <cstdint>                                                   // uint64_t
#include <mutex>                                                     // std::mutex：保护最新帧
#include <thread>                                                    // std::thread
#include <vector>                                                    // std::vector<uchar>：JPEG 字节

class MjpegServer {                                                  // 定义"MJPEG 网页服务器"类
public:
    // port        : 监听端口（浏览器访问的端口号）
    // quality     : JPEG 质量 1~100，越大越清晰也越占带宽
    // max_fps     : 推流帧率上限。★不必和相机一样★，调试看画面 10~15fps 完全够，
    //               设小一点能显著降低板子 CPU 占用和 WiFi 带宽
    // out_width   : 输出宽度（0 = 不缩放）。缩到 640 能省一半编码时间
    MjpegServer(int port, int quality, double max_fps, int out_width);
    ~MjpegServer();                                                  // 析构：停止服务器、关套接字

    bool start();                                                    // 启动监听（成功返回 true）
    void publish(const cv::Mat &bgr);                                // 发布一帧（内部会按 max_fps 限流）

    bool running() const { return this->running_.load(); }           // 服务器是否在跑
    int  port()    const { return this->port_; }                     // 实际监听端口

private:
    void acceptLoop();                                               // accept 线程：接新连接
    void clientLoop(int fd);                                         // 每个浏览器的服务线程
    static bool writeAll(int fd, const void *data, size_t len);      // 把 len 字节完整写出去

    int    port_       = 8080;                                       // 监听端口
    int    quality_    = 70;                                         // JPEG 质量
    double min_interval_ = 1.0 / 15.0;                               // 两次推流之间最小间隔(秒)
    int    out_width_  = 0;                                          // 输出宽度，0=原尺寸

    int listen_fd_ = -1;                                             // 监听套接字
    std::atomic<bool> running_{false};                               // 运行标志
    std::thread accept_thread_;                                      // accept 线程

    // ---- 【板子只有 1GB 内存，下面几个是防止内存被吃光的保护】----
    //   · kMaxClients：最多同时服务几个浏览器。每个连接占一个线程 + 一份 JPEG 拷贝，
    //     不管的话（浏览器反复刷新、或被别的东西狂连）线程会越堆越多。
    //     超了就回 503，让浏览器自己等，而不是把板子拖垮。
    //   · clients_：当前连接数（原子变量，accept 线程和 client 线程都会改）。
    static constexpr int kMaxClients = 4;                            // 同时最多 4 个观看端
    std::atomic<int> clients_{0};                                    // 当前连接数

    // 【复用缓冲】缩放用的目标图。
    //   做成成员而不是每帧新建：cv::resize 输出到同尺寸的已有 Mat 时会复用内存，
    //   省掉每帧一次 ~0.9MB 的分配/释放（60fps 下就是 50MB/s 的内存抖动）。
    cv::Mat resize_buf_;

    std::mutex mtx_;                                                 // 保护下面这几项
    std::condition_variable cv_;                                     // "有新帧了"的通知
    std::vector<uchar> jpeg_;                                        // 最新一帧的 JPEG 数据
    uint64_t seq_ = 0;                                               // 帧序号：客户端靠它判断有没有新帧
    double last_pub_tick_ = 0.;                                      // 上次发布的时刻（限流用）
    std::atomic<uint64_t> pub_count_{0};                             // 累计发布帧数（状态页显示）
};

#endif // MJPEG_SERVER_HPP_                                          // 保护宏结束
