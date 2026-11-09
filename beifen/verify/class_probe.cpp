// ============================================================================
// class_probe.cpp —— 搞清楚"每个检测框到底属于哪一类"
// ----------------------------------------------------------------------------
// 【要解决的问题】
//   工程里写的是 `if (classIdPoint.x != 8) continue;`，也就是只保留第 8 类。
//   但这个 8 是硬编码的，模型 .xml 里**不含类别名**，无法从文件看出第 8 类是什么。
//   所以需要实测：拿摄像头对准目标，看模型把"绿色引导灯"判成第几类。
//
//   这个工具就是干这个的：实时推理，把每个检测框的
//     - 位置、尺寸
//     - 9 个类别的得分（全打出来，不筛选）
//     - 框内像素的 HSV 统计（判断实际是什么颜色）
//   一起打印出来。这样就能一眼看出"绿光 → 第几类"。
//
// 【用法】
//   ./class_probe                                   # 用默认模型路径，跑 30 秒
//   ./class_probe <模型.xml> <模型.bin> <秒数>
//   ./class_probe --img shot.png                    # 对一张图片推理（不用相机）
//
// 【重要】相机取图流程与工程里 UvcCamera 一致：
//   MJPG → 640x480 → 缩放到网络输入 384x640（letterbox）
//   BGR→RGB，像素 /255 —— 与 OpenvinoInfer 的预处理配置保持一致
// ============================================================================

#include <opencv2/opencv.hpp>
#include <openvino/openvino.hpp>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>

// ---------------------------------------------------------------------------
// letterBox：与工程 OpenvinoInfer::letterBox 完全一致
//   把 640x480 的相机图等比缩放到 640x384 的画布（网络输入）
// ---------------------------------------------------------------------------
static void letterBox(const cv::Mat &src, cv::Mat &dst, double &scale, int &dx, int &dy,
                      const cv::Size &dst_size) {
    int w = src.cols, h = src.rows;
    double r = std::min((double)dst_size.width / w, (double)dst_size.height / h);
    int new_w = (int)(w * r), new_h = (int)(h * r);
    cv::Mat resized;
    cv::resize(src, resized, cv::Size(new_w, new_h), cv::INTER_CUBIC);
    dst = cv::Mat(dst_size, CV_8UC3, cv::Scalar(128, 128, 128));
    dx = (int)((dst_size.width - new_w) / 2.);
    dy = (int)((dst_size.height - new_h) / 2.);
    resized.copyTo(dst(cv::Rect(dx, dy, new_w, new_h)));
    scale = r;
}

// 统计一个区域的颜色特征（判断是绿、红、蓝还是白/暗）
// 注意 OpenCV 的 H 范围是 [0,180]（不是 360），所以：
//   纯绿 H=60，纯红 H=0/180，纯蓝 H=120
static const char *colorName(const cv::Mat &bgr_roi) {
    cv::Mat hsv;
    cv::cvtColor(bgr_roi, hsv, cv::COLOR_BGR2HSV);
    cv::Scalar m = cv::mean(hsv);
    double H = m[0], S = m[1], V = m[2];
    if (V < 60) return "很暗";
    if (S < 60) return V > 200 ? "白/过曝" : "灰";
    // 按主导色相判断（阈值放宽，带光晕的灯颜色会偏）
    if (H >= 40 && H <= 90)  return "绿";      // 绿：约 80~180 度
    if (H >= 25 && H < 40)   return "黄绿/黄";
    if (H < 12 || H > 168)   return "红";
    if (H < 25)              return "橙";
    if (H > 90 && H <= 135)  return "蓝";
    return "紫/粉";
}

static void report(cv::Mat &frame_bgr, ov::InferRequest &req, ov::Shape shape,
                   const std::string &tag, double score_thr) {
    printf("\n########## %s ##########\n", tag.c_str());
    printf("张量形状: [%zu, %zu, %zu]  (候选数=%zu, 每候选=%zu)\n",
           shape[0], shape[1], shape[2], shape[1], shape[2]);

    const ov::Tensor &out = req.get_output_tensor(0);
    const float *data = out.data<const float>();
    size_t N = shape[1], C = shape[2];

    int found = 0;
    for (size_t i = 0; i < N; ++i) {
        const float *d = &data[i * C];
        float conf = d[4];                                  // objectness
        if (conf < score_thr) continue;

        // 类别头 [5,5+9)，找最高分那一类
        int best = -1; float bestv = 0.f;
        for (int c = 0; c < 9; ++c) {
            if (d[5 + c] > bestv) { bestv = d[5 + c]; best = c; }
        }

        double cx = d[0], cy = d[1], bw = d[2], bh = d[3];
        int x1 = std::max(0, (int)(cx - bw / 2)), y1 = std::max(0, (int)(cy - bh / 2));
        int x2 = std::min(frame_bgr.cols, (int)(cx + bw / 2));
        int y2 = std::min(frame_bgr.rows, (int)(cy + bh / 2));

        printf("\n--- 检测 %d ---\n", ++found);
        printf("  框(画布系) : 中心(%.1f,%.1f) 尺寸 %.1fx%.1f\n", cx, cy, bw, bh);
        printf("  objectness : %.3f\n", conf);
        printf("  类别 argmax: 第 %d 类  得分 %.3f\n", best, bestv);
        printf("  9 类得分   : ");
        for (int c = 0; c < 9; ++c) printf("[%d]%.2f ", c, d[5 + c]);
        printf("\n");
        printf("  颜色头(4)  : ");
        for (int c = 0; c < 4; ++c) printf("[%d]%.2f ", c, d[14 + c]);
        printf("\n");
        if (x2 - x1 > 2 && y2 - y1 > 2) {
            cv::Mat roi = frame_bgr(cv::Rect(x1, y1, x2 - x1, y2 - y1));
            cv::Scalar mb = cv::mean(roi);
            cv::Mat hsv; cv::cvtColor(roi, hsv, cv::COLOR_BGR2HSV);
            cv::Scalar mh = cv::mean(hsv);
            printf("  框内像素   : BGR均值 (%.0f,%.0f,%.0f)  HSV均值 (%.0f,%.0f,%.0f)  → 颜色判断: %s\n",
                   mb[0], mb[1], mb[2], mh[0], mh[1], mh[2], colorName(roi));
        }
    }
    if (found == 0) {
        printf("  （没有超过阈值 %.2f 的检测框）\n", score_thr);
    }
    printf("共 %d 个检测框\n", found);
}

int main(int argc, char **argv) {
    std::string xml = "/home/rsve/Desktop/feibiao/Vision_Code/config/openvino/Katrin.xml";
    std::string bin = "/home/rsve/Desktop/feibiao/Vision_Code/config/openvino/Katrin.bin";
    int seconds = 30;
    std::string img_path;
    double score_thr = 0.35;          // 调低一点，尽量多看到候选

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--img") == 0 && i + 1 < argc) {
            img_path = argv[++i];
        } else if (i == 1) {
            xml = argv[i];
        } else if (i == 2) {
            bin = argv[i];
        } else if (i == 3) {
            seconds = atoi(argv[i]);
        }
    }

    printf("================ 类别探测工具 ================\n");
    printf("模型: %s\n", xml.c_str());

    // ---- 载入模型，预处理与工程 OpenvinoInfer 保持一致 ----
    ov::Core core;
    std::shared_ptr<ov::Model> model;
    try {
        model = core.read_model(xml, bin);
    } catch (const std::exception &e) {
        printf("❌ 读模型失败: %s\n", e.what());
        return 1;
    }

    ov::preprocess::PrePostProcessor ppp(model);
    ppp.input().tensor()
        .set_element_type(ov::element::u8)
        .set_layout("NHWC")
        .set_color_format(ov::preprocess::ColorFormat::BGR);
    ppp.input().preprocess()
        .convert_element_type(ov::element::f32)
        .convert_color(ov::preprocess::ColorFormat::RGB)
        .scale({255.f, 255.f, 255.f});
    ppp.input().model().set_layout("NCHW");
    for (int i = 0; i < 3; ++i) ppp.output(i).tensor().set_element_type(ov::element::f32);
    model = ppp.build();

    ov::CompiledModel compiled = core.compile_model(model, "CPU");   // 虚拟机没独显，用 CPU
    ov::InferRequest req = compiled.create_infer_request();
    ov::Shape in_shape = compiled.input().get_shape();
    printf("网络输入形状: [%zu, %zu, %zu, %zu]\n", in_shape[0], in_shape[1], in_shape[2], in_shape[3]);

    cv::Size net_wh(640, 384);        // 与工程 detect.json / Detect.cpp 传的一致

    // ---- 模式 A：对单张图片推理 ----
    if (!img_path.empty()) {
        cv::Mat img = cv::imread(img_path);
        if (img.empty()) { printf("❌ 读不到图片 %s\n", img_path.c_str()); return 1; }
        printf("图片: %s  %dx%d\n", img_path.c_str(), img.cols, img.rows);
        cv::Mat canvas; double sc; int dx, dy;
        letterBox(img, canvas, sc, dx, dy, net_wh);
        ov::Tensor t(ov::element::u8, {1, (size_t)canvas.rows, (size_t)canvas.cols, 3}, canvas.data);
        req.set_input_tensor(t);
        req.infer();
        ov::Shape osh = req.get_output_tensor(0).get_shape();
        report(img, req, osh, "图片推理结果", score_thr);
        return 0;
    }

    // ---- 模式 B：实时相机推理 ----
    int dev = 0, W = 640, H = 480, FPS = 120;
    cv::VideoCapture cap(dev, cv::CAP_V4L2);
    if (!cap.isOpened()) { printf("❌ 打不开 /dev/video%d\n", dev); return 1; }
    cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
    cap.set(cv::CAP_PROP_FRAME_WIDTH, W);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, H);
    cap.set(cv::CAP_PROP_FPS, FPS);
    cap.set(cv::CAP_PROP_BUFFERSIZE, 1);
    printf("相机: /dev/video%d  %dx%d  (实际 %dx%d @%.0f)\n", dev, W, H,
           (int)cap.get(cv::CAP_PROP_FRAME_WIDTH), (int)cap.get(cv::CAP_PROP_FRAME_HEIGHT),
           cap.get(cv::CAP_PROP_FPS));

    printf("\n>>> 现在请把【绿色引导灯】对准镜头中心 <<<\n");
    printf(">>> 运行 %d 秒，每秒打印一次检测结果 <<<\n", seconds);

    cv::Mat frame;
    for (int i = 0; i < 10; ++i) cap.read(frame);        // 丢弃前几帧等曝光稳定

    double t0 = (double)cv::getTickCount();
    int tick = 0;
    while (true) {
        if (!cap.read(frame) || frame.empty()) { printf("读帧失败\n"); break; }
        cv::Mat canvas; double sc; int dx, dy;
        letterBox(frame, canvas, sc, dx, dy, net_wh);

        ov::Tensor t(ov::element::u8, {1, (size_t)canvas.rows, (size_t)canvas.cols, 3}, canvas.data);
        req.set_input_tensor(t);
        req.infer();

        if (++tick % 30 == 0) {                          // 每 30 帧打印一次（约 0.6 秒）
            double el = ((double)cv::getTickCount() - t0) / cv::getTickFrequency();
            char tag[128];
            snprintf(tag, sizeof(tag), "t=%.1fs  第 %d 次采样", el, tick / 30);
            ov::Shape osh = req.get_output_tensor(0).get_shape();
            report(frame, req, osh, tag, score_thr);
            if (el >= seconds) break;
        }
    }

    cap.release();
    printf("\n================ 结束 ================\n");
    printf("结论怎么看：\n");
    printf("  1) 找到【框内像素颜色判断: 绿】的那一条，看它的【类别 argmax: 第 N 类】\n");
    printf("     -> 那个 N 就是绿色引导灯对应的类别号\n");
    printf("  2) 如果它的类别号不是 8，那工程里 classIdPoint.x != 8 的筛选条件就是错的\n");
    printf("  3) 也留意“颜色头”那 4 个值，可能模型专门有一路判颜色\n");
    return 0;
}
