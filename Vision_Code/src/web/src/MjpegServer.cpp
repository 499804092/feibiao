// ============================================================================
// MjpegServer.cpp —— MJPEG over HTTP 推流实现（用途和设计见 MjpegServer.hpp）
// ============================================================================

#include "MjpegServer.hpp"                                           // 自己的头文件

#include <arpa/inet.h>                                               // htons / inet_addr
#include <cerrno>                                                    // errno
#include <chrono>                                                    // std::chrono::milliseconds（等新帧的超时）
#include <csignal>                                                   // ::signal / SIGPIPE / SIG_IGN
#include <cstdio>                                                    // printf / snprintf
#include <cstring>                                                   // memset / strchr / strerror
#include <netinet/in.h>                                              // sockaddr_in
#include <netinet/tcp.h>                                             // TCP_NODELAY
#include <sys/socket.h>                                              // socket / bind / listen / accept / send
#include <unistd.h>                                                  // close / read

// 一帧的边界字符串。浏览器就是靠这个分帧的，随便取一个不会出现在 JPEG 数据里的字符串。
// JPEG 以 0xFFD8 开头、0xFFD9 结尾，其中不会出现这么长的 ASCII，所以安全。
static const char *kBOUNDARY = "dartframe";

// ============================================================================
// 构造函数：只记参数，不干活（真正的启动在 start() 里）
// ============================================================================
MjpegServer::MjpegServer(int port, int quality, double max_fps, int out_width)
        : port_(port),                                               // 监听端口
          quality_(quality),                                         // JPEG 质量
          out_width_(out_width) {                                    // 输出宽度
    if (max_fps < 1.0) { max_fps = 1.0; }                            // 帧率下限保护，避免下面除零
    this->min_interval_ = 1.0 / max_fps;                             // 换算成"两次推流最小间隔"
    if (this->quality_ < 1)   { this->quality_ = 1; }                // JPEG 质量夹紧到 1~100
    if (this->quality_ > 100) { this->quality_ = 100; }
}                                                                    // 构造函数结束

// ============================================================================
// 析构函数：停服务、关套接字
// ============================================================================
MjpegServer::~MjpegServer() {                                        // 析构
    this->running_.store(false);                                     // 先让所有循环退出
    if (this->listen_fd_ >= 0) {                                     // 套接字还开着
        ::shutdown(this->listen_fd_, SHUT_RDWR);                     // 主动断开：让阻塞中的 accept 立刻返回
        ::close(this->listen_fd_);                                   // 关掉监听套接字
        this->listen_fd_ = -1;                                       // 置 -1 防止重复关
    }                                                                // if 结束
    this->cv_.notify_all();                                          // 叫醒所有等新帧的客户端线程
    if (this->accept_thread_.joinable()) {                           // accept 线程还在
        this->accept_thread_.join();                                 // 等它退出
    }                                                                // if 结束
    // 注意：各个浏览器的 client 线程是 detach 的，这里不等它们。
    //   原因是本程序正常退出走的是 _exit()（见 main.cpp 的说明），
    //   进程一结束这些线程自然就没了，没必要为它们加一套复杂的退出同步。
}                                                                    // 析构结束

// ============================================================================
// start() —— 建套接字、绑定端口、开始监听，并起 accept 线程
// ============================================================================
bool MjpegServer::start() {                                          // 成功返回 true
    // ---------- ① 建 TCP 套接字 ----------
    this->listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);            // IPv4 + TCP
    if (this->listen_fd_ < 0) {                                      // 建失败
        printf("[Web] ❌ 建套接字失败: %s\n", strerror(errno));       // 打印原因
        return false;                                                // 返回失败
    }                                                                // if 结束

    // ---------- ② 允许端口复用 ----------
    //   程序刚退出又立刻重启时，端口还处在 TIME_WAIT，不设这个会报 "Address already in use"
    int on = 1;                                                      // 开关值：1 = 打开
    ::setsockopt(this->listen_fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    // ---------- ③ 绑定端口 ----------
    sockaddr_in addr{};                                              // 地址结构体（{} 保证全 0）
    addr.sin_family      = AF_INET;                                  // IPv4
    addr.sin_addr.s_addr = htonl(INADDR_ANY);                        // 监听本机所有网卡（WiFi/有线都行）
    addr.sin_port        = htons((uint16_t)this->port_);             // 端口要转成网络字节序
    if (::bind(this->listen_fd_, (sockaddr *)&addr, sizeof(addr)) < 0) {   // 绑定
        printf("[Web] ❌ 绑定端口 %d 失败: %s\n", this->port_, strerror(errno));  // 打印原因
        printf("[Web]    最常见的原因：端口被别的程序占了。查是谁占的：\n");        // 给出排查方法
        printf("[Web]        ss -tlnp | grep %d\n", this->port_);                   // 具体命令
        printf("[Web]    或者把 config/tuning.yaml 里的 web.port 换成别的（比如 8081）\n");  // 另一条路
        ::close(this->listen_fd_);                                   // 关掉套接字
        this->listen_fd_ = -1;                                       // 置 -1
        return false;                                                // 返回失败
    }                                                                // if 结束

    // ---------- ④ 开始监听 ----------
    //   第二个参数是"等待队列长度"，给 4 就够了（同时不会有几个浏览器连）
    if (::listen(this->listen_fd_, 4) < 0) {                         // 监听
        printf("[Web] ❌ listen 失败: %s\n", strerror(errno));        // 打印原因
        ::close(this->listen_fd_);                                   // 关掉套接字
        this->listen_fd_ = -1;                                       // 置 -1
        return false;                                                // 返回失败
    }                                                                // if 结束

    // ---------- ⑤ 【关键】忽略 SIGPIPE ----------
    //   浏览器把标签页关掉时，我们正往那个套接字写数据，内核会给进程发 SIGPIPE。
    //   SIGPIPE 默认行为是【直接杀掉整个进程】—— 调个参把浏览器关了，整个飞镖程序
    //   就没了。所以必须忽略它（写失败会返回 -1，我们按错误处理就行）。
    //   另外发送时还会加 MSG_NOSIGNAL 双保险（见 writeAll）。
    ::signal(SIGPIPE, SIG_IGN);                                      // 忽略 SIGPIPE

    this->running_.store(true);                                      // 标记为运行中
    this->accept_thread_ = std::thread(&MjpegServer::acceptLoop, this);  // 起 accept 线程

    printf("[Web] ✅ 推流服务器已启动\n");                            // 告诉用户起来了
    printf("[Web]    浏览器打开:  http://<板子IP>:%d/\n", this->port_);  // 访问地址（固定，可收藏）
    printf("[Web]    纯画面地址:  http://<板子IP>:%d/stream\n", this->port_);  // 直接看流
    printf("[Web]    看板子IP:    hostname -I\n");                    // 提示怎么查 IP
    fflush(stdout);                                                  // 立刻刷出
    return true;                                                     // 返回成功
}                                                                    // start 结束

// ============================================================================
// acceptLoop() —— accept 线程：每来一个浏览器就开一个线程服务它
// ============================================================================
void MjpegServer::acceptLoop() {                                     // 在独立线程里跑
    while (this->running_.load()) {                                  // 只要还在运行
        sockaddr_in cli{};                                           // 客户端地址（这里用不到，占位）
        socklen_t len = sizeof(cli);                                 // 地址长度
        int fd = ::accept(this->listen_fd_, (sockaddr *)&cli, &len); // 等新连接（会阻塞）
        if (fd < 0) {                                                // 出错（多半是被 shutdown 唤醒的）
            if (!this->running_.load()) { break; }                   // 正常退出 → 直接结束
            continue;                                                // 其它错误 → 继续等下一个
        }                                                            // if 结束

        // 关掉 Nagle 算法：MJPEG 是"小包连续发"，攒包反而增加延迟
        int on = 1;                                                  // 开关值
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

        // ---------- 【内存保护】连接数超上限就直接拒绝 ----------
        //   每个连接 = 一个线程 + 一份 JPEG 拷贝。浏览器反复刷新、或者被别的东西
        //   狂连的时候，线程会越堆越多，1GB 内存的板子很容易被拖垮。
        //   超过上限就回 503 让浏览器自己等，绝不无限接。
        if (this->clients_.load() >= kMaxClients) {                   // 已经满了
            const char *body = "too many viewers, try again later\n"; // 拒绝原因
            char head[256];                                          // 响应头
            snprintf(head, sizeof(head),
                     "HTTP/1.1 503 Service Unavailable\r\nContent-Type: text/plain\r\n"
                     "Content-Length: %zu\r\nConnection: close\r\n\r\n", strlen(body));
            writeAll(fd, head, strlen(head));                        // 发头
            writeAll(fd, body, strlen(body));                        // 发正文
            ::close(fd);                                             // 关连接
            continue;                                                // 回去等下一个
        }                                                            // if 结束
        this->clients_.fetch_add(1);                                 // 计数 +1（clientLoop 退出时会减回去）

        // 每个连接一个线程（同时看的人不会有几个，够用且实现简单）
        //   detach：不等它，浏览器断开时线程自己结束
        std::thread(&MjpegServer::clientLoop, this, fd).detach();    // 起线程后立刻 detach
    }                                                                // while 结束
}                                                                    // acceptLoop 结束

// ============================================================================
// writeAll() —— 把 len 字节【完整】写出去
//   TCP 的 send 可能只写进去一部分，必须循环到写完，否则画面会花/断
// ============================================================================
bool MjpegServer::writeAll(int fd, const void *data, size_t len) {   // 成功返回 true
    const char *p = (const char *)data;                              // 转成字节指针方便偏移
    size_t sent = 0;                                                 // 已发送字节数
    while (sent < len) {                                             // 还没发完
        // MSG_NOSIGNAL：对端已关闭时返回 -1 而不是发 SIGPIPE（和 start() 里的忽略是双保险）
        ssize_t n = ::send(fd, p + sent, len - sent, MSG_NOSIGNAL);  // 发剩余部分
        if (n <= 0) {                                                // 出错或对端关闭
            if (errno == EINTR) { continue; }                        // 被信号打断 → 重试
            return false;                                            // 其它错误 → 放弃
        }                                                            // if 结束
        sent += (size_t)n;                                           // 累加已发送
    }                                                                // while 结束
    return true;                                                     // 全部发完
}                                                                    // writeAll 结束

// ============================================================================
// clientLoop() —— 服务一个浏览器连接
// ============================================================================
void MjpegServer::clientLoop(int fd) {                               // 入参：已接受的连接 fd
    // 【内存保护】不管从哪条路径退出，都要把"当前连接数"减回去。
    //   用一个小 RAII 结构挂在栈上，函数一返回就自动减，不会漏。
    struct ClientGuard {                                             // 局部 RAII 守卫
        std::atomic<int> *n;                                         // 指向计数变量
        ~ClientGuard() { this->n->fetch_sub(1); }                    // 析构时减 1
    } guard{&this->clients_};                                        // 构造即生效

    // ---------- ① 读一次 HTTP 请求（只关心第一行的路径）----------
    char req[2048];                                                  // 请求缓冲
    ssize_t n = ::recv(fd, req, sizeof(req) - 1, 0);                 // 收一次就够（GET 请求很小）
    if (n <= 0) {                                                    // 没收到东西
        ::close(fd);                                                 // 关掉连接
        return;                                                      // 结束
    }                                                                // if 结束
    req[n] = '\0';                                                   // 补字符串结束符

    std::string path = "/";                                          // 默认路径
    {                                                                // 从 "GET /xxx HTTP/1.1" 里抠出 /xxx
        const char *s1 = strchr(req, ' ');                           // 第一个空格
        if (s1 != nullptr) {                                         // 找到了
            const char *s2 = strchr(s1 + 1, ' ');                    // 第二个空格
            if (s2 != nullptr) {                                     // 找到了
                path.assign(s1 + 1, (size_t)(s2 - s1 - 1));          // 截出中间的路径
            }                                                        // if 结束
        }                                                            // if 结束
    }                                                                // 作用域结束

    // ---------- ② /status —— 纯文本状态页（确认服务活着 + 看板子负载）----------
    //   ★板子只有 1GB 内存，这个页面能直接在浏览器里看到程序占了多少内存★
    //   RSS 从 /proc/self/statm 读（第 2 个字段是常驻内存页数）。
    if (path == "/status") {                                         // 访问 /status
        long rss_kb = -1;                                            // 常驻内存(KB)，读不到就是 -1
        FILE *f = fopen("/proc/self/statm", "r");                    // 打开自己的内存统计
        if (f != nullptr) {                                          // 打开成功
            long total_pages = 0, rss_pages = 0;                     // 总页数 / 常驻页数
            if (fscanf(f, "%ld %ld", &total_pages, &rss_pages) == 2) {   // 读前两个字段
                rss_kb = rss_pages * (sysconf(_SC_PAGESIZE) / 1024); // 页数 × 每页KB = 实际占用
            }                                                        // if 结束
            fclose(f);                                               // 关文件
        }                                                            // if 结束

        char body[512];                                              // 响应正文
        snprintf(body, sizeof(body),
                 "OK\n"
                 "MjpegServer on port %d\n"
                 "viewers   : %d / %d\n"
                 "frames    : %llu\n"
                 "proc RSS  : %ld KB (%.1f MB)   <-- 板子总共 1GB，盯住这个数\n"
                 "stream    : /stream\n",
                 this->port_,                                    // 端口
                 this->clients_.load(), kMaxClients,             // 当前/最多观看端
                 (unsigned long long)this->pub_count_.load(),    // 累计推流帧数
                 rss_kb, rss_kb > 0 ? rss_kb / 1024.0 : 0.0);    // 常驻内存
        char head[256];                                              // 响应头
        snprintf(head, sizeof(head),
                 "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\n"
                 "Content-Length: %zu\r\nConnection: close\r\n\r\n", strlen(body));
        writeAll(fd, head, strlen(head));                            // 发头
        writeAll(fd, body, strlen(body));                            // 发正文
        ::close(fd);                                                 // 关连接
        return;                                                      // 结束
    }                                                                // if 结束

    // ---------- ③ 其它路径 —— 返回一个极简 HTML 页面 ----------
    //   页面里就一个 <img>，src 指向 /stream；浏览器会自己再开一条连接去拉流。
    if (path != "/stream") {                                         // 不是 /stream 就走这
        const char *html =                                           // 网页内容（极简，纯黑底铺满）
                "<!doctype html><html><head><meta charset=\"utf-8\">"
                "<title>Dart Vision</title>"
                "<style>html,body{margin:0;padding:0;background:#111;height:100%;}"
                "img{display:block;width:100%;height:auto;}</style></head>"
                "<body><img src=\"/stream\" alt=\"stream\"></body></html>\n";
        char head[256];                                              // 响应头
        snprintf(head, sizeof(head),
                 "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                 "Content-Length: %zu\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n",
                 strlen(html));
        writeAll(fd, head, strlen(head));                            // 发头
        writeAll(fd, html, strlen(html));                            // 发网页
        ::close(fd);                                                 // 关连接
        return;                                                      // 结束
    }                                                                // if 结束

    // ---------- ④ /stream —— 真正的 MJPEG 推流 ----------
    //   响应头里的 multipart/x-mixed-replace 就是告诉浏览器：
    //   "后面会一直发多段图片，每段边界是 --dartframe，你收到就替换掉上一张"。
    char head[512];                                                  // 推流响应头
    snprintf(head, sizeof(head),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: multipart/x-mixed-replace; boundary=%s\r\n"
             "Cache-Control: no-cache, no-store, must-revalidate\r\n"
             "Pragma: no-cache\r\n"
             "Connection: close\r\n\r\n",
             kBOUNDARY);
    if (!writeAll(fd, head, strlen(head))) {                         // 头发失败了
        ::close(fd);                                                 // 关连接
        return;                                                      // 结束
    }                                                                // if 结束

    uint64_t last_seq = 0;                                           // 这个客户端上次发出去的帧号
    while (this->running_.load()) {                                  // 只要服务在跑
        std::vector<uchar> frame;                                    // 本次要发的 JPEG
        {                                                            // 上锁范围尽量小
            std::unique_lock<std::mutex> lk(this->mtx_);             // 加锁
            // 等"有新帧"或者"服务停了"。带上谓词，防止虚假唤醒。
            this->cv_.wait_for(lk, std::chrono::milliseconds(500), [&] {
                return !this->running_.load() || this->seq_ != last_seq;
            });
            if (!this->running_.load()) { break; }                   // 服务停了 → 退出
            if (this->seq_ == last_seq || this->jpeg_.empty()) {     // 只是超时、还没有新帧
                continue;                                            // 回去继续等
            }                                                        // if 结束
            frame = this->jpeg_;                                     // 拷一份（拷完就能放锁，发送不占锁）
            last_seq = this->seq_;                                   // 记下帧号
        }                                                            // 解锁

        // 组装这一帧的分段头 + JPEG 数据
        //   ★这里必须"一段一次写"，不能拼成一个大 buffer 再发★ ——
        //   拼的话每帧都要多拷一份几十 KB，弱 CPU 上白白浪费。
        char seg[128];                                               // 分段头
        int seg_len = snprintf(seg, sizeof(seg),
                               "\r\n--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n",
                               kBOUNDARY, frame.size());
        if (!writeAll(fd, seg, (size_t)seg_len)) { break; }          // 发分段头，失败就退出
        if (!writeAll(fd, frame.data(), frame.size())) { break; }    // 发 JPEG 数据，失败就退出
    }                                                                // while 结束

    ::close(fd);                                                     // 关掉这个连接
}                                                                    // clientLoop 结束

// ============================================================================
// publish() —— 发布一帧（由显示线程每帧调用）
//   ★必须极快、绝不阻塞★ 因为它在显示线程的循环里：
//   这里只做"按帧率限流 → 缩放 → 编码 JPEG → 加锁存一下 → 通知"。
// ============================================================================
void MjpegServer::publish(const cv::Mat &bgr) {                      // 入参：要推出去的 BGR 图
    if (!this->running_.load() || bgr.empty()) { return; }           // 没在跑 / 空图 → 直接返回

    // ---------- ① 按 max_fps 限流 ----------
    //   不需要每一帧都推：相机 60fps，我们按配置的帧率推就够了，
    //   省下来的 CPU 和带宽留给识别本身。
    //   ★注意这里丢的是"还没到点就来的帧"，被推出去的永远是【刚画好的这一帧】，
    //     所以限流不会让画面变成旧帧，只是帧率低一些★
    double now_tick = (double)cv::getTickCount();                    // 当前时刻
    double freq     = cv::getTickFrequency();                        // 每秒多少滴答
    if (this->last_pub_tick_ > 0. &&
        (now_tick - this->last_pub_tick_) / freq < this->min_interval_) {   // 还没到下次该推的时间
        return;                                                      // 这一帧跳过
    }                                                                // if 结束
    this->last_pub_tick_ = now_tick;                                 // 记下本次推流时刻

    // ---------- ② 需要的话缩小 ----------
    //   缩放能成倍省下 JPEG 编码时间（RK3566 上编码是大头）。
    //   输出到成员 resize_buf_（不是每帧新建 Mat）：
    //   cv::resize 输出到尺寸相同的已有 Mat 时会复用内存，省掉每帧一次
    //   ~0.9MB 的分配/释放 —— 在 1GB 内存的板子上这点抖动也值得省。
    const cv::Mat *to_encode = &bgr;                                 // 默认直接用原图（用指针，避免整帧拷贝）
    if (this->out_width_ > 0 && bgr.cols > this->out_width_) {       // 配置了宽度且原图更宽
        double s = (double)this->out_width_ / (double)bgr.cols;      // 缩放比例
        cv::resize(bgr, this->resize_buf_, cv::Size(), s, s, cv::INTER_AREA);  // 缩小（INTER_AREA 质量最好）
        to_encode = &this->resize_buf_;                              // 改成用缩放后的图
    }                                                                // if 结束

    // ---------- ③ 编码成 JPEG ----------
    std::vector<uchar> buf;                                          // 编码结果
    std::vector<int> params{cv::IMWRITE_JPEG_QUALITY, this->quality_};  // 编码参数：JPEG 质量
    if (!cv::imencode(".jpg", *to_encode, buf, params)) {            // 编码失败
        return;                                                      // 本帧放弃（不报错刷屏）
    }                                                                // if 结束

    // ---------- ④ 存成"最新帧"并通知所有客户端 ----------
    {
        std::lock_guard<std::mutex> lk(this->mtx_);                  // 加锁
        this->jpeg_.swap(buf);                                       // swap 而不是赋值：零拷贝，buf 顺便被复用
        ++this->seq_;                                                // 帧号 +1（客户端靠它判断有没有新帧）
    }                                                                // 解锁
    this->pub_count_.fetch_add(1);                                   // 累计帧数 +1（状态页显示用）
    this->cv_.notify_all();                                          // 叫醒所有等新帧的浏览器线程

    // ★这里没有"给每个客户端排队"的逻辑，是故意的：
    //   浏览器/网络慢的时候，旧帧直接丢掉、只发最新的。实时画面本来就该这样，
    //   不然会越积越多，画面延迟越拉越大（也会把 1GB 内存吃光）。
}                                                                    // publish 结束
