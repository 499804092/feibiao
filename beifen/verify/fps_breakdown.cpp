// ============================================================================
// fps_breakdown.cpp —— 逐项定位"为什么只有 50 帧"
// ----------------------------------------------------------------------------
// 分别测 5 种配置，找出帧率到底卡在哪一环：
//   A. 只取图不处理          → 相机 + VMware USB 直通的真实上限
//   B. 取图 + resize 到 640x384
//   C. 取图 + letterBox 到 640x384
//   D. 取图 + 模型推理(CPU)
//   E. 取图 + 推理 + 画框(不显示)
//   F. 取图 + 推理 + imshow 显示   ← 实际使用时的帧率
//
// 用法：
//   ./fps_breakdown                 # 默认 640x480 @120
//   ./fps_breakdown 640 480 120
//   ./fps_breakdown 320 240 120
// ============================================================================

#include <opencv2/opencv.hpp>
#include <openvino/openvino.hpp>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>

static double now() { return (double)cv::getTickCount() / cv::getTickFrequency(); }

static void letterBox(const cv::Mat &src, cv::Mat &dst, const cv::Size &ds) {
    int w = src.cols, h = src.rows;
    double r = std::min((double)ds.width / w, (double)ds.height / h);
    int nw = (int)(w * r), nh = (int)(h * r);
    cv::Mat rs;
    cv::resize(src, rs, cv::Size(nw, nh), cv::INTER_LINEAR);   // 用 INTER_LINEAR 更快
    dst = cv::Mat(ds, CV_8UC3, cv::Scalar(128, 128, 128));
    rs.copyTo(dst(cv::Rect((ds.width - nw) / 2, (ds.height - nh) / 2, nw, nh)));
}

int main(int argc, char **argv) {
    int W   = (argc > 1) ? atoi(argv[1]) : 640;
    int H   = (argc > 2) ? atoi(argv[2]) : 480;
    int FPS = (argc > 3) ? atoi(argv[3]) : 120;
    const double SEC = 4.0;                  // 每项测 4 秒

    printf("================ 帧率瓶颈逐项定位 ================\n");
    printf("请求 %dx%d @%d fps\n\n", W, H, FPS);

    // ---------- 打开相机 ----------
    cv::VideoCapture cap(0, cv::CAP_V4L2);
    if (!cap.isOpened()) { printf("打不开相机\n"); return 1; }
    cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
    cap.set(cv::CAP_PROP_FRAME_WIDTH, W);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, H);
    cap.set(cv::CAP_PROP_FPS, FPS);
    cap.set(cv::CAP_PROP_BUFFERSIZE, 1);
    printf("实际生效: %dx%d @%.1f fps\n\n",
           (int)cap.get(cv::CAP_PROP_FRAME_WIDTH),
           (int)cap.get(cv::CAP_PROP_FRAME_HEIGHT),
           cap.get(cv::CAP_PROP_FPS));

    // ---------- 载入模型 ----------
    const std::string base = "/home/rsve/Desktop/feibiao/Vision_Code/config/openvino/";
    ov::Core core;
    auto model = core.read_model(base + "Katrin.xml", base + "Katrin.bin");
    ov::preprocess::PrePostProcessor ppp(model);
    ppp.input().tensor().set_element_type(ov::element::u8).set_layout("NHWC")
        .set_color_format(ov::preprocess::ColorFormat::BGR);
    ppp.input().preprocess().convert_element_type(ov::element::f32)
        .convert_color(ov::preprocess::ColorFormat::RGB).scale({255.f, 255.f, 255.f});
    ppp.input().model().set_layout("NCHW");
    for (int i = 0; i < 3; ++i) ppp.output(i).tensor().set_element_type(ov::element::f32);
    model = ppp.build();
    auto compiled = core.compile_model(model, "CPU");
    auto req = compiled.create_infer_request();
    printf("模型已载入 (CPU)\n\n");

    cv::Mat frame, canvas;
    cv::Size net_wh(640, 384);

    // 预热：丢弃 20 帧
    for (int i = 0; i < 20; ++i) cap.read(frame);

    auto run = [&](const char *name, int mode) {
        cv::Mat shown;
        double t0 = now();
        long n = 0;
        while (true) {
            if (!cap.read(frame) || frame.empty()) break;
            ++n;
            switch (mode) {
                case 0:                                  // A 只取图
                    break;
                case 1:                                  // B 只 resize
                    cv::resize(frame, canvas, net_wh);
                    break;
                case 2:                                  // C letterBox
                    letterBox(frame, canvas, net_wh);
                    break;
                case 3: {                                // D 推理
                    letterBox(frame, canvas, net_wh);
                    ov::Tensor t(ov::element::u8,
                                 {1, (size_t)canvas.rows, (size_t)canvas.cols, 3}, canvas.data);
                    req.set_input_tensor(t);
                    req.infer();
                    break;
                }
                case 4: {                                // E 推理 + 画框（不显示）
                    letterBox(frame, canvas, net_wh);
                    ov::Tensor t(ov::element::u8,
                                 {1, (size_t)canvas.rows, (size_t)canvas.cols, 3}, canvas.data);
                    req.set_input_tensor(t);
                    req.infer();
                    cv::rectangle(frame, cv::Rect(10, 10, 100, 100), cv::Scalar(0, 255, 0), 2);
                    break;
                }
                case 5: {                                // F 推理 + 显示
                    letterBox(frame, canvas, net_wh);
                    ov::Tensor t(ov::element::u8,
                                 {1, (size_t)canvas.rows, (size_t)canvas.cols, 3}, canvas.data);
                    req.set_input_tensor(t);
                    req.infer();
                    cv::rectangle(frame, cv::Rect(10, 10, 100, 100), cv::Scalar(0, 255, 0), 2);
                    cv::imshow("fps test", frame);
                    cv::waitKey(1);
                    break;
                }
            }
            if (now() - t0 >= SEC) break;
        }
        double el = now() - t0;
        printf("  %-32s %6.1f fps   (%ld 帧 / %.1f 秒)  平均 %.1f ms/帧\n",
               name, n / el, n, el, 1000.0 * el / std::max(1L, n));
        return n / el;
    };

    double fA = run("A. 只取图（不处理）", 0);
    double fB = run("B. + resize", 1);
    double fC = run("C. + letterBox", 2);
    double fD = run("D. + 模型推理(CPU)", 3);
    double fE = run("E. + 画框（不显示）", 4);

    // 显示这一项需要桌面环境，SSH 下会失败，所以放在最后并容错
    printf("\n  （下一项需要桌面环境，SSH 下可能报错，属正常）\n");
    double fF = 0;
    try {
        fF = run("F. + imshow 显示", 5);
    } catch (const std::exception &e) {
        printf("  F 项跳过（无显示环境）: %s\n", e.what());
    }
    cv::destroyAllWindows();

    printf("\n================ 分析 ================\n");
    printf("相机上限(A)        : %.1f fps\n", fA);
    printf("加 resize 后(B)    : %.1f fps\n", fB);
    printf("加 letterBox 后(C) : %.1f fps\n", fC);
    printf("加推理后(D)        : %.1f fps\n", fD);
    printf("加画框后(E)        : %.1f fps\n", fE);
    if (fF > 0) printf("加显示后(F)        : %.1f fps\n", fF);

    printf("\n瓶颈定位：\n");
    if (fA < FPS * 0.7)
        printf("  ★ 取图本身就达不到 %d fps（只有 %.1f）→ 瓶颈在【相机/驱动/VMware USB 直通】\n",
               FPS, fA);
    if (fD > 0 && fD < fA * 0.8)
        printf("  ★ 加上推理后明显掉帧（%.1f → %.1f）→ 推理是瓶颈，可以换 GPU 或隔帧推理\n",
               fA, fD);
    if (fF > 0 && fF < fE * 0.9)
        printf("  ★ 加上显示后明显掉帧（%.1f → %.1f）→ imshow 是瓶颈，可以隔帧显示或缩小窗口\n",
               fE, fF);
    printf("======================================\n");
    return 0;
}
