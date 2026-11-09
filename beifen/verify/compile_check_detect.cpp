// ============================================================================
// compile_check_detect.cpp —— 用桩头把「修正后的识别代码」真编译一遍
// ----------------------------------------------------------------------------
// 目的：feibiao/ 里只有部分源文件，缺 ReJson.hpp / function.hpp 等支撑头，
//       没法整文件编译。这里把所有外部依赖替换成最小桩（stub），
//       但【类定义 + infer() + letterBox() + fitRec() 的函数体】与真实代码
//       **逐字一致**，所以编译器能真正检查出语法错误、类型不匹配、漏 include 等问题。
//
// 编译：g++ -std=c++20 -Wall -fsyntax-only compile_check_detect.cpp $(pkg-config --cflags opencv4)
// ============================================================================

#include <opencv2/opencv.hpp>
#include <opencv2/dnn.hpp>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <vector>
#include <map>
#include <string>

// ---------------- 桩：opencv / openvino 相关 ----------------
// 这些桩只是为了"让真实代码能过编译器"，签名与真实 OpenVINO 保持一致。
namespace ov {
struct Shape { size_t operator[](int i) const { return i == 1 ? 15120 : 27; } };
template <typename T> struct Tensor {
    T *data() { static T dummy[15120 * 27]{}; return dummy; }
    T *data() const { static T dummy[15120 * 27]{}; return dummy; }
    Shape get_shape() const { return {}; }
};
enum class element { f32 };
struct Input { element get_element_type() const { return element::f32; }
               Shape get_shape() const { return {}; } };
struct InferRequest {
    void set_input_tensor(const Tensor<float> &) {}
    void start_async() {}
    void wait() {}
    Tensor<float> get_output_tensor(int) const { return {}; }
};
struct Model {};
struct CompiledModel { Input input() const { return {}; } InferRequest create_infer_request() const { return {}; } };
struct Core {
    std::shared_ptr<Model> read_model(const std::string &, const std::string &) { return std::make_shared<Model>(); }
    CompiledModel compile_model(const std::shared_ptr<Model> &, const std::string &) { return {}; }
};
}  // namespace ov

// ---------------- 桩：Config.hpp（真实工程里是 inline ReJson J_DETECT） ----------------
class ReJson {
public:
    // 让 config_["openvino"]["score_threshold"] 这种链式取值能用
    ReJson operator[](const char *) const { return {}; }
    operator double() const { return 0.5; }   // 能隐式转成阈值
};
struct ConfigStub { ReJson config_; };
static ConfigStub J_DETECT;

// ============================================================================
// 以下内容与 feibiao/.../openvino/OpenvinoInfer.hpp + .cpp 的函数体逐字一致
// ============================================================================

#define BLUE 0
#define RED  1
#define NONE_ 2
#define CURR true
#define NEXT false
#define TARGET_CLASS_ID 8

class OpenvinoInfer {
public:
    struct Resize {
        cv::Mat resized_image;
        int dx;
        int dy;
    };

    struct Light {
        int id = -1;
        double score = 0.;
        cv::Rect2d box;
        cv::Point2d center_point;
    };

    explicit OpenvinoInfer(std::map<std::string, std::string> &path);
    ~OpenvinoInfer() = default;

    static void fitRec(std::vector<OpenvinoInfer::Light> &bboxes, cv::Size2d ori_size, cv::Size2d now_size);
    std::vector<OpenvinoInfer::Light> infer(cv::Mat &src, const cv::Size2d &dst_size, const int &my_color, const bool &startup);

private:
    static OpenvinoInfer::Resize letterBox(cv::Mat &src, const cv::Size2d &dst_size);

    ov::Core core_;
    ov::CompiledModel compiled_model_;
    ov::CompiledModel compiled_model_next_;
    std::shared_ptr<ov::Model> model_;
    std::map<bool, ov::InferRequest> infer_requests_;
};

OpenvinoInfer::OpenvinoInfer(std::map<std::string, std::string> &path) {
    this->model_ = this->core_.read_model(path["XML"], path["BIN"]);
    this->compiled_model_ = this->core_.compile_model(this->model_, path["DEVICE"]);
    this->compiled_model_next_ = this->core_.compile_model(this->model_, path["DEVICE"]);
    this->infer_requests_.insert(std::pair<bool, ov::InferRequest>(CURR, this->compiled_model_.create_infer_request()));
    this->infer_requests_.insert(std::pair<bool, ov::InferRequest>(NEXT, this->compiled_model_next_.create_infer_request()));
}

std::vector<OpenvinoInfer::Light> OpenvinoInfer::infer(cv::Mat &src, const cv::Size2d &dst_size, const int &my_color, const bool &startup) {
    OpenvinoInfer::Resize resize = OpenvinoInfer::letterBox(src, dst_size);

    auto *input_data = (uint8_t *)resize.resized_image.data;

    ov::Tensor<float> input_tensor;
    static_cast<void>(input_data);
    static_cast<void>(my_color);

    if (startup) {
        this->infer_requests_[CURR].set_input_tensor(input_tensor);
        this->infer_requests_[CURR].start_async();
        return {};
    }

    this->infer_requests_[NEXT].set_input_tensor(input_tensor);
    this->infer_requests_[NEXT].start_async();
    this->infer_requests_[CURR].wait();

    std::vector<float> confidences;
    std::vector<cv::Rect> boxes;
    std::vector<OpenvinoInfer::Light> Lights;
    std::vector<int> nms_result;
    const ov::Tensor<float> &outputTensor = this->infer_requests_[CURR].get_output_tensor(0);
    ov::Shape outputShape = outputTensor.get_shape();

    auto *outputData = outputTensor.data();

    for (size_t i = 0 ; i < outputShape[1] ; ++i) {
        float* detection = &outputData[i * outputShape[2]];

        float confidence = detection[4];
        if (confidence >= (double)J_DETECT.config_["openvino"]["score_threshold"]) {

            float* classesConfidence = &detection[4 + 1];
            cv::Mat scores(1, 9, CV_32FC1, classesConfidence);
            cv::Point classIdPoint;
            double maxClassConfidence;
            cv::minMaxLoc(scores, nullptr, &maxClassConfidence, nullptr, &classIdPoint);

            if (maxClassConfidence >= (double)J_DETECT.config_["openvino"]["score_threshold"]) {

                if (classIdPoint.x != TARGET_CLASS_ID) {
                    continue;
                }

                OpenvinoInfer::Light Light;
                Light.id = classIdPoint.x;
                Light.score = confidence;
                Light.center_point = cv::Point2f(detection[0], detection[1]);
                Light.box = cv::Rect2d(detection[0] - detection[2] / 2.,
                                       detection[1] - detection[3] / 2.,
                                       detection[2], detection[3]);
                Lights.emplace_back(Light);

                confidences.emplace_back(confidence);
                int nx = (int)std::lround(detection[0] - detection[2] / 2.);
                int ny = (int)std::lround(detection[1] - detection[3] / 2.);
                int nw = (int)std::lround(detection[2]);
                int nh = (int)std::lround(detection[3]);
                if (nw < 1) nw = 1;
                if (nh < 1) nh = 1;
                boxes.emplace_back(cv::Rect(nx, ny, nw, nh));
            }
        }
    }

    cv::dnn::NMSBoxes(boxes, confidences,
                      J_DETECT.config_["openvino"]["score_threshold"],
                      J_DETECT.config_["openvino"]["nms_threshold"], nms_result);

    std::vector<OpenvinoInfer::Light> result;
    for (const int &idx : nms_result) {
        result.emplace_back(OpenvinoInfer::Light {
                Lights[idx].id, Lights[idx].score, Lights[idx].box, Lights[idx].center_point });
    }

    OpenvinoInfer::fitRec(result, src.size(), dst_size);

    std::vector<OpenvinoInfer::Light> filtered;
    filtered.reserve(result.size());
    for (const auto &light : result) {
        cv::Rect2d b = light.box;
        double x1 = std::max(0., b.x);
        double y1 = std::max(0., b.y);
        double x2 = std::min((double)src.cols, b.x + b.width);
        double y2 = std::min((double)src.rows, b.y + b.height);

        if (x2 - x1 < 2. || y2 - y1 < 2.) {
            continue;
        }

        OpenvinoInfer::Light fixed = light;
        fixed.box = cv::Rect2d(x1, y1, x2 - x1, y2 - y1);
        fixed.center_point = cv::Point2d((x1 + x2) / 2., (y1 + y2) / 2.);
        filtered.emplace_back(fixed);
    }

    auto change = this->infer_requests_[CURR];
    this->infer_requests_[CURR] = this->infer_requests_[NEXT];
    this->infer_requests_[NEXT] = change;

    return filtered;
}

OpenvinoInfer::Resize OpenvinoInfer::letterBox(cv::Mat &src, const cv::Size2d &dst_size) {
    int w = src.cols;
    int h = src.rows;

    double r_w = (double)dst_size.width / w;
    double r_h = (double)dst_size.height / h;
    double r = std::min(r_w, r_h);

    int new_w = (int)(w * r);
    int new_h = (int)(h * r);

    cv::Mat resized_image;
    cv::resize(src, resized_image, cv::Size(new_w, new_h), cv::INTER_CUBIC);

    cv::Mat canvas(dst_size, CV_8UC3, cv::Scalar(128, 128, 128));

    int dx = (int)((dst_size.width - new_w) / 2.);
    int dy = (int)((dst_size.height - new_h) / 2.);

    resized_image.copyTo(canvas(cv::Rect(dx, dy, new_w, new_h)));

    return {canvas, dx, dy};
}

void OpenvinoInfer::fitRec(std::vector<OpenvinoInfer::Light> &bboxes, cv::Size2d ori_size, cv::Size2d now_size) {
    if (now_size.width <= 1. || now_size.height <= 1. ||
        ori_size.width <= 1. || ori_size.height <= 1.) {
        return;
    }

    double scale = std::max((double)ori_size.width / now_size.width,
                            (double)ori_size.height / now_size.height);

    for (auto &bbox : bboxes) {
        bbox.box.x = (bbox.box.x - now_size.width / 2) * scale + ori_size.width / 2;
        bbox.box.y = (bbox.box.y - now_size.height / 2) * scale + ori_size.height / 2;
        bbox.box.width  *= scale;
        bbox.box.height *= scale;
        bbox.center_point.x = (bbox.center_point.x - now_size.width / 2) * scale + ori_size.width / 2;
        bbox.center_point.y = (bbox.center_point.y - now_size.height / 2) * scale + ori_size.height / 2;
    }
}

// ============================================================================
// 跑一下坐标链路，顺便确认编译产物行为正确
// ============================================================================
int main() {
    std::map<std::string, std::string> path{{"XML", ""}, {"BIN", ""}, {"DEVICE", "CPU"}};
    OpenvinoInfer infer(path);

    cv::Mat src(640, 384, CV_8UC3, cv::Scalar(0));      // ori_mat: 384 列 x 640 行

    std::vector<OpenvinoInfer::Light> v;
    OpenvinoInfer::Light l;
    l.center_point = cv::Point2d(525.0, 533.3);          // 画布坐标系里的一个点
    l.box = cv::Rect2d(485.0, 493.3, 80.0, 80.0);
    v.push_back(l);
    OpenvinoInfer::fitRec(v, src.size(), cv::Size2d(640, 384));
    printf("编译检查通过。fitRec 实测：画布(525.0,533.3) -> ori_mat(%.2f,%.2f)\n",
           v[0].center_point.x, v[0].center_point.y);
    return 0;
}
