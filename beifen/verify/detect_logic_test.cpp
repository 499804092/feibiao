// ============================================================================
// detect_logic_test.cpp —— 用合成图验证【工程的检测筛选逻辑】
// ----------------------------------------------------------------------------
// 【为什么要这个】
//   摄像头不在、或者不方便摆靶子的时候，也能验证工程里那段筛选逻辑对不对：
//     - min_objectness 卡掉低分候选
//     - class_threshold 卡掉低分类别
//     - classId == 8 只留目标类
//     - NMS 去重
//     - fitRec 坐标还原
//   这里用与 OpenvinoInfer 完全相同的参数与流程跑一张合成图。
//
// 【用法】
//   ./detect_logic_test                           # 用默认合成图（绿块）
//   ./detect_logic_test some.png                  # 用你自己的图片
//   ./detect_logic_test some.png 0.20 0.50        # 指定 min_objectness / class_threshold
//
// 注意：这个程序**不读 detect.json**（避免路径问题），阈值从命令行给，
//       默认值与 detect.json 里改好的一致：min_objectness=0.20, class_threshold=0.50
// ============================================================================

#include <opencv2/opencv.hpp>
#include <openvino/openvino.hpp>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>

#define TARGET_CLASS_ID 8

static void letterBox(const cv::Mat &src, cv::Mat &dst, int &dx, int &dy, const cv::Size &ds) {
    int w = src.cols, h = src.rows;
    double r = std::min((double)ds.width / w, (double)ds.height / h);
    int nw = (int)(w * r), nh = (int)(h * r);
    cv::Mat rs;
    cv::resize(src, rs, cv::Size(nw, nh), cv::INTER_CUBIC);
    dst = cv::Mat(ds, CV_8UC3, cv::Scalar(128, 128, 128));
    dx = (int)((ds.width - nw) / 2.);
    dy = (int)((ds.height - nh) / 2.);
    rs.copyTo(dst(cv::Rect(dx, dy, nw, nh)));
}

int main(int argc, char **argv) {
    std::string img_path = (argc > 1) ? argv[1] : "";
    double min_obj = (argc > 2) ? atof(argv[2]) : 0.20;
    double cls_thr = (argc > 3) ? atof(argv[3]) : 0.50;
    double nms_iou = (argc > 4) ? atof(argv[4]) : 0.25;   // 与 detect.json 的 nms_threshold 一致

    // 第一个参数用 --synthetic / --default 表示不用外部图片，走内置合成图
    if (img_path == "--synthetic" || img_path == "--default") img_path.clear();

    const std::string base = "/home/rsve/Desktop/feibiao/Vision_Code/config/openvino/";

    printf("=========== 工程检测逻辑验证 ===========\n");
    printf("min_objectness = %.2f   class_threshold = %.2f   nms_iou = %.2f\n", min_obj, cls_thr, nms_iou);

    // ---- 准备输入图 ----
    cv::Mat img;
    if (img_path.empty()) {
        // 造合成图：一个绿色块（模拟引导灯）+ 一个红块（应当被忽略）
        img = cv::Mat(480, 640, CV_8UC3, cv::Scalar(35, 35, 35));
        cv::rectangle(img, cv::Rect(240, 150, 150, 150), cv::Scalar(0, 255, 0), -1);   // 绿
        cv::rectangle(img, cv::Rect(60, 150, 120, 120), cv::Scalar(0, 0, 255), -1);    // 红
        cv::imwrite("/tmp/logic_synth.png", img);
        printf("输入：合成图（左红块 + 右绿块），已存 /tmp/logic_synth.png\n");
    } else {
        img = cv::imread(img_path);
        if (img.empty()) { printf("读不到图片 %s\n", img_path.c_str()); return 1; }
        printf("输入：%s  (%dx%d)\n", img_path.c_str(), img.cols, img.rows);
    }

    // ---- 载入模型（预处理与 OpenvinoInfer 一致）----
    ov::Core core;
    auto model = core.read_model(base + "Katrin.xml", base + "Katrin.bin");
    ov::preprocess::PrePostProcessor ppp(model);
    ppp.input().tensor()
        .set_element_type(ov::element::u8).set_layout("NHWC")
        .set_color_format(ov::preprocess::ColorFormat::BGR);
    ppp.input().preprocess()
        .convert_element_type(ov::element::f32)
        .convert_color(ov::preprocess::ColorFormat::RGB)
        .scale({255.f, 255.f, 255.f});
    ppp.input().model().set_layout("NCHW");
    for (int i = 0; i < 3; ++i) ppp.output(i).tensor().set_element_type(ov::element::f32);
    model = ppp.build();
    auto compiled = core.compile_model(model, "CPU");
    auto req = compiled.create_infer_request();

    cv::Size net_wh(640, 384);
    cv::Mat canvas; int dx = 0, dy = 0;
    letterBox(img, canvas, dx, dy, net_wh);

    ov::Tensor t(ov::element::u8, {1, (size_t)canvas.rows, (size_t)canvas.cols, 3}, canvas.data);
    req.set_input_tensor(t);
    req.infer();

    const auto &out = req.get_output_tensor(0);
    const float *d = out.data<const float>();
    ov::Shape sh = out.get_shape();
    size_t N = sh[1], C = sh[2];
    printf("输出张量: [%zu, %zu, %zu]\n\n", sh[0], sh[1], sh[2]);

    // ---- 与 OpenvinoInfer::infer 完全相同的筛选流程 ----
    int raw_cnt = 0, pass_obj = 0, pass_cls = 0, pass_class8 = 0;
    std::vector<cv::Rect> nms_boxes;
    std::vector<float> nms_scores;
    std::vector<std::pair<int, cv::Rect2d>> cands;

    for (size_t i = 0; i < N; ++i) {
        const float *v = &d[i * C];
        float obj = v[4];
        if (obj < 0.05f) continue;
        ++raw_cnt;

        if (obj >= min_obj) ++pass_obj;

        int best = -1; float bestv = 0.f;
        for (int c = 0; c < 9; ++c) if (v[5 + c] > bestv) { bestv = v[5 + c]; best = c; }

        if (obj < min_obj) continue;
        if (bestv < cls_thr) continue;
        ++pass_cls;
        if (best != TARGET_CLASS_ID) continue;
        ++pass_class8;

        cv::Rect2d box(v[0] - v[2] / 2, v[1] - v[3] / 2, v[2], v[3]);
        cands.push_back({best, box});
        nms_boxes.emplace_back((int)std::lround(box.x), (int)std::lround(box.y),
                               (int)std::lround(box.width), (int)std::lround(box.height));
        nms_scores.push_back(obj);
    }

    printf("筛选漏斗：\n");
    printf("  原始候选(obj>0.05)      : %d\n", raw_cnt);
    printf("  过 min_objectness(%.2f)  : %d\n", min_obj, pass_obj);
    printf("  再过 class_threshold(%.2f): %d\n", cls_thr, pass_cls);
    printf("  再过 classId==%d        : %d\n", TARGET_CLASS_ID, pass_class8);

    std::vector<int> keep;
    cv::dnn::NMSBoxes(nms_boxes, nms_scores, (float)min_obj, (float)nms_iou, keep);
    printf("  NMS 去重后              : %zu\n\n", keep.size());

    // ---- fitRec 坐标还原（与工程同一公式）----
    double s = std::max((double)img.cols / net_wh.width, (double)img.rows / net_wh.height);
    printf("最终结果（坐标已还原回原图）：\n");
    for (int k : keep) {
        const cv::Rect2d &b = cands[k].second;
        double x = (b.x - net_wh.width  / 2.0) * s + img.cols / 2.0;
        double y = (b.y - net_wh.height / 2.0) * s + img.rows / 2.0;
        double w = b.width * s, h = b.height * s;
        cv::Rect2d r(x, y, w, h);
        r &= cv::Rect2d(0, 0, img.cols, img.rows);
        printf("  第%d类  obj=%.2f  原图框 (%.0f,%.0f) %.0fx%.0f  中心(%.0f,%.0f)\n",
               cands[k].first, nms_scores[k], r.x, r.y, r.width, r.height,
               r.x + r.width / 2, r.y + r.height / 2);
    }
    if (keep.empty()) printf("  （没有通过筛选的目标）\n");

    printf("\n判定：\n");
    if (!keep.empty() && pass_class8 > 0)
        printf("  ✓ 筛选逻辑工作正常：绿块被识别为第 %d 类并通过阈值\n", TARGET_CLASS_ID);
    else if (pass_obj > 0)
        printf("  ⚠️ 有候选过了 objectness，但没过类别阈值/类别号，检查阈值是否太严\n");
    else
        printf("  ⚠️ 连 objectness 都没过：图里可能没有目标，或 min_objectness 太高\n");
    printf("=======================================\n");
    return 0;
}
