// ============================================================================
// calib_camera_usb.cpp —— 从采集到的图片算出相机内参
// ----------------------------------------------------------------------------
// 【它干什么】
//   把 calib_capture_usb 存的那些棋盘格图片读进来，找角点（含亚像素细化），
//   调 cv::calibrateCamera 算出相机内参 fx / fy / cx / cy 和畸变系数，
//   然后：
//     · 打印重投影误差，并列出误差最大的几张图（让你知道该删哪几张重来）
//     · 打印能直接粘进 Vision_Code/config/tuning.yaml 的 camera 节点
//     · 把结果写成 intrinsics.yaml 存下来
//
// 【和原版的关系】
//   对标 spr_vision_26/calibration/calibrate_camera.cpp，求解条件和它一致：
//     · 网格点 3D 坐标 {j*d, i*d, 0}
//     · flags = CALIB_FIX_K3（视场角小，不需要 k3）
//     · TermCriteria(COUNT+EPS, 100, DBL_EPSILON)（默认 30 次有时会发散，故设 100）
//   多做的：① 角点亚像素细化 cornerSubPix（原版没有，加上精度明显更好）
//           ② 逐张报误差、按误差排序，能挑出拍糊的图
//           ③ 直接输出 tuning.yaml 的 camera 节点
//           ④ 检查标定分辨率是否和 tuning.yaml 里实际跑的分辨率一致（不一致会给出换算系数）
//
// 【编译】见同目录 build_calib.sh
//
// 【用法】
//   ./calib_camera_usb                                  # 读 calib_out，默认板子 11x8 内角点 25mm
//   ./calib_camera_usb --in mydata --cols 11 --rows 8 --square 25
//   ./calib_camera_usb --save-undistort 3               # 顺便存 3 张去畸变后的图，肉眼看直不直
//   ./calib_camera_usb --show                           # 逐张弹窗看角点找得对不对（要桌面）
//   ./calib_camera_usb --fix-k3 0                       # 不固定 k3（广角镜头才需要）
//
// 【结果怎么看】
//   重投影误差 < 0.5 px  → 很好
//   0.5 ~ 1.0 px         → 能用，但可以删掉误差最大的几张重来
//   > 1.0 px             → 有拍糊的图或角点找错了，看列出来的那几张
// ============================================================================

#include <opencv2/opencv.hpp>   // OpenCV：读图、找角点、标定

#include <algorithm>            // std::sort
#include <cctype>               // isdigit
#include <cfloat>               // DBL_EPSILON
#include <cmath>                // sqrt / atan / fabs
#include <cstdio>               // printf / snprintf
#include <cstdlib>              // atoi / atof
#include <string>               // std::string
#include <sys/stat.h>           // stat：判断目录存不存在
#include <vector>               // std::vector

// ---------------------------------------------------------------------------
// resolveConfig() —— 找 tuning.yaml 的真实位置
//   默认路径写的是 "Vision_Code/config/tuning.yaml"，那是【在 feibiao/ 下跑】时的
//   相对路径。但可执行文件在 feibiao/calib_usb/ 里，大家自然会在那里面跑，
//   这时相对路径就对不上 —— 而读不到配置【不会报错】，只是安静地跳过分辨率核对，
//   于是"标定分辨率和你实际跑的分辨率不一致"这个最要命的坑就查不出来了。
// ---------------------------------------------------------------------------
static std::string resolveConfig(const std::string & given, bool from_cli) {
    if (from_cli) return given;                                       // 命令行明确给了就用它，不猜
    const char * cands[] = {
        "Vision_Code/config/tuning.yaml",                             // 在 feibiao/ 下跑
        "../Vision_Code/config/tuning.yaml",                          // 在 feibiao/calib_usb/ 下跑（最常见）
        "../../Vision_Code/config/tuning.yaml",                       // 在更深的子目录里跑
        "config/tuning.yaml",                                         // 在 Vision_Code/ 下跑
    };
    for (const char * c : cands) {                                    // 挨个试能不能打开
        FILE * f = fopen(c, "r");
        if (f) {
            fclose(f);
            return std::string(c);                                    // 找到就用这个
        }
    }
    return given;                                                     // 都没找到，原样返回
}

// ---------------------------------------------------------------------------
// dirExists() —— 目录在不在
//   cv::glob 遇到不存在的目录会【直接抛异常把进程打崩】（还打印一堆 core dumped），
//   所以在调它之前先自己查一下，给一句人话。
// ---------------------------------------------------------------------------
static bool dirExists(const std::string & path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// ---------------------------------------------------------------------------
// 用法说明
// ---------------------------------------------------------------------------
static void printUsage() {
    printf("用法: ./calib_camera_usb [选项]\n");
    printf("  --in <目录>         图片目录（默认 calib_out）\n");
    printf("  --cols <n>          棋盘格内角点列数（默认 11 = Q12-340-25 横着拿）\n");
    printf("  --rows <n>          棋盘格内角点行数（默认 8）\n");
    printf("  --square <mm>       方格边长 mm（默认 25）\n");
    printf("  --out-yaml <路径>   内参输出路径（默认 <图片目录>/intrinsics.yaml）\n");
    printf("  --config <路径>     tuning.yaml 路径，用来核对分辨率（默认自动在几个常见位置找）\n");
    printf("  --sb 1              用 SB 检测器找角点\n");
    printf("  --fix-k3 0          不固定 k3（默认 1 = 固定，和原版一致）\n");
    printf("  --min-views <n>     少于多少张就报错退出（默认 8）\n");
    printf("  --save-undistort <n> 存 n 张去畸变后的图用于肉眼检查（默认 0）\n");
    printf("  --show              逐张弹窗看角点（要桌面）\n");
    printf("  --no-save           不写 intrinsics.yaml，只打印\n");
    printf("  --help              显示这段说明\n");
}

// ---------------------------------------------------------------------------
// 从文件名里抠出序号，用来做"自然排序"
//   calib_capture_usb 存的是 1.jpg 2.jpg ... 10.jpg，按字符串排会变成
//   1, 10, 11, 2, 3 ... 很难看，所以按数字排。
//   返回 -1 表示文件名里没有数字（这种排在后面按字母序）。
// ---------------------------------------------------------------------------
static int stemNumber(const std::string & path, std::string & stem) {
    size_t slash = path.find_last_of("/\\");                          // 最后一个路径分隔符
    std::string fn = (slash == std::string::npos) ? path : path.substr(slash + 1);
    size_t dot = fn.find_last_of('.');                                // 扩展名
    stem = (dot == std::string::npos) ? fn : fn.substr(0, dot);
    size_t i = stem.size();                                           // 从后往前找连续数字
    while (i > 0 && isdigit(static_cast<unsigned char>(stem[i - 1]))) --i;
    if (i == stem.size()) return -1;                                  // 一个数字都没有
    return atoi(stem.c_str() + i);                                    // 转成整数
}

// ---------------------------------------------------------------------------
// listImages() —— 把目录下的图片列出来，按文件名里的数字自然排序
// ---------------------------------------------------------------------------
static std::vector<std::string> listImages(const std::string & dir) {
    std::vector<std::string> all;                                     // 汇总所有扩展名
    for (const char * ext : {"jpg", "jpeg", "png", "bmp"}) {          // 常见几种都收
        try {
            std::vector<cv::String> part;
            cv::glob(dir + "/*." + ext, part, false);                 // 找这个扩展名
            for (const auto & p : part) all.push_back(std::string(p));  // 收集起来
        } catch (const cv::Exception &) {
            // 目录不存在/读不了，这里不管 —— 外层已经查过目录了，静默跳过即可
        }
    }
    std::sort(all.begin(), all.end(), [](const std::string & a, const std::string & b) {
        std::string sa, sb;
        int na = stemNumber(a, sa);                                   // a 的序号
        int nb = stemNumber(b, sb);                                   // b 的序号
        if (na >= 0 && nb >= 0) {                                     // 都有序号
            if (na != nb) return na < nb;                             // 小的在前
            return sa < sb;                                           // 序号相同按名字
        }
        if (na >= 0) return true;                                     // 有序号的排在前面
        if (nb >= 0) return false;
        return a < b;                                                 // 都没序号按字母序
    });
    return all;
}

// ---------------------------------------------------------------------------
// findBoard() —— 找棋盘格（和采集程序用的是同一套参数）
// ---------------------------------------------------------------------------
static bool findBoard(
    const cv::Mat & gray, const cv::Size & board, bool use_sb,
    std::vector<cv::Point2f> & corners) {
    corners.clear();
    bool ok = false;
#if CV_VERSION_MAJOR >= 4                                                // SB 检测器要 OpenCV 4
    if (use_sb) {
        ok = cv::findChessboardCornersSB(gray, board, corners);
    } else
#endif
    {
        ok = cv::findChessboardCorners(
            gray, board, corners,
            cv::CALIB_CB_ADAPTIVE_THRESH + cv::CALIB_CB_NORMALIZE_IMAGE);
    }
    return ok && static_cast<int>(corners.size()) == board.width * board.height;
}

// ---------------------------------------------------------------------------
// refineCorners() —— 亚像素角点细化（原版没有这一步，这是精度提升的主要来源）
//   整像素角点最多差 0.5px，细化后一般能到 0.05px 量级，
//   重投影误差和焦距都会明显更稳。
// ---------------------------------------------------------------------------
static void refineCorners(const cv::Mat & gray, std::vector<cv::Point2f> & corners) {
    cv::cornerSubPix(
        gray, corners, cv::Size(11, 11), cv::Size(-1, -1),
        cv::TermCriteria(cv::TermCriteria::EPS + cv::TermCriteria::COUNT, 30, 0.01));
}

// ---------------------------------------------------------------------------
// 网格点 3D 坐标（和原版一致：{j*d, i*d, 0}，原点在左上角那个角点上）
// ---------------------------------------------------------------------------
static std::vector<cv::Point3f> centers3d(const cv::Size & board, double d) {
    std::vector<cv::Point3f> pts;
    for (int i = 0; i < board.height; i++)
        for (int j = 0; j < board.width; j++)
            pts.push_back({static_cast<float>(j * d), static_cast<float>(i * d), 0.0f});
    return pts;
}

// ---------------------------------------------------------------------------
// checkResolution() —— 核对标定分辨率和 tuning.yaml 里实际跑的采集分辨率
//   ★内参和分辨率绑死★：800x600 标出来的 fx 直接用在 640x480 上，
//   解算出来的角度会整体差 25%。这里主动查出来并给出换算系数。
// ---------------------------------------------------------------------------
static void checkResolution(
    const std::string & cfg, const cv::Size & img_size, bool & matched, double & scale) {
    matched = true;
    scale = 1.0;
    cv::FileStorage fs(cfg, cv::FileStorage::READ);
    if (!fs.isOpened()) return;                                       // 读不到就不查
    cv::FileNode node = fs["uvc"];
    if (node.empty()) return;
    int w = 0, h = 0;
    if (!node["width"].empty())  node["width"]  >> w;
    if (!node["height"].empty()) node["height"] >> h;
    if (w <= 0 || h <= 0) return;
    if (w == img_size.width && h == img_size.height) return;          // 一致，没问题
    matched = false;                                                  // 不一致
    scale = static_cast<double>(w) / img_size.width;                  // 目标宽度 / 标定宽度
    printf("\n⚠⚠⚠ 分辨率不一致 ⚠⚠⚠\n");
    printf("  标定用的是   %dx%d\n", img_size.width, img_size.height);
    printf("  但 %s 里 uvc 跑的是 %dx%d\n", cfg.c_str(), w, h);
    printf("  内参和分辨率绑死，直接抄过去角度会整体差 %.0f%%。两个办法：\n",
           std::fabs(scale - 1.0) * 100.0);
    printf("    · 推荐：把 uvc.width/height 改成 %dx%d 重新采集标定\n", img_size.width, img_size.height);
    printf("    · 或者：下面 fx/fy/cx/cy 都乘 %.4f 再用\n", scale);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char ** argv) {
    // ---- 默认值 ----
    std::string in_dir = "calib_out";                                 // 图片目录
    std::string cfg = "Vision_Code/config/tuning.yaml";               // 用来核对分辨率
    std::string out_yaml = "";                                        // 空 = <in_dir>/intrinsics.yaml
    int  cols = 11, rows = 8;                                         // 内角点数（大凡 Q12-340-25）
    double square = 25.0;                                             // 方格边长 mm
    bool use_sb = false;                                              // 是否用 SB 检测器
    bool fix_k3 = true;                                               // 是否固定 k3
    int  min_views = 8;                                               // 最少几张
    int  save_undistort = 0;                                          // 存几张去畸变图
    bool show = false;                                                // 是否逐张弹窗
    bool do_save = true;                                              // 是否写 intrinsics.yaml
    bool cfg_path_cli = false;                                        // 命令行是否明确指定了 --config

    // ---- 解析命令行 ----
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char * def) -> std::string {
            return (i + 1 < argc) ? std::string(argv[++i]) : std::string(def);
        };
        if (a == "--in")                  in_dir = next("calib_out");
        else if (a == "--config")       { cfg = next("Vision_Code/config/tuning.yaml"); cfg_path_cli = true; }
        else if (a == "--out-yaml")       out_yaml = next("");
        else if (a == "--cols")           cols = atoi(next("11").c_str());
        else if (a == "--rows")           rows = atoi(next("8").c_str());
        else if (a == "--square")         square = atof(next("25").c_str());
        else if (a == "--sb")             use_sb = (atoi(next("1").c_str()) != 0);
        else if (a == "--fix-k3")         fix_k3 = (atoi(next("1").c_str()) != 0);
        else if (a == "--min-views")      min_views = atoi(next("8").c_str());
        else if (a == "--save-undistort") save_undistort = atoi(next("0").c_str());
        else if (a == "--show")           show = true;
        else if (a == "--no-save")        do_save = false;
        else if (a == "--help" || a == "-h") { printUsage(); return 0; }
        else { printf("不认识的参数: %s（用 --help 看说明）\n", a.c_str()); return 1; }
    }
    if (out_yaml.empty()) out_yaml = in_dir + "/intrinsics.yaml";     // 默认和图片放一起

    // ---- 找出 tuning.yaml 的真实位置（在不同目录下启动相对路径不一样）----
    cfg = resolveConfig(cfg, cfg_path_cli);

    printf("============ USB 相机内参标定 ============\n");
    printf("图片目录：%s\n", in_dir.c_str());
    printf("标定板：内角点 %d 列 × %d 行（= %d × %d 个方格），方格边长 %.1f mm\n",
           cols, rows, cols + 1, rows + 1, square);

    // ---- 先查目录在不在（cv::glob 遇到不存在的目录会直接把进程打崩）----
    if (!dirExists(in_dir)) {
        printf("✗ 目录 %s 不存在。\n", in_dir.c_str());
        printf("  先用 calib_capture_usb 拍标定图，或者用 --in 指定正确目录。\n");
        return 1;
    }

    // ---- 列图片 ----
    std::vector<std::string> files = listImages(in_dir);
    if (files.empty()) {
        printf("✗ %s 里一张图片都没有。先用 calib_capture_usb 拍标定图。\n", in_dir.c_str());
        return 1;
    }
    printf("找到 %zu 个图片文件\n\n", files.size());

    // ---- 逐张找角点 ----
    const cv::Size board(cols, rows);                                 // (列, 行)
    std::vector<cv::Point3f> objp = centers3d(board, square);         // 同一套 3D 点，每张复用
    std::vector<std::vector<cv::Point3f>> obj_points;                 // 每张图的 3D 点
    std::vector<std::vector<cv::Point2f>> img_points;                 // 每张图的 2D 角点
    std::vector<std::string> used_files;                              // 成功用上的文件名
    cv::Size img_size(0, 0);                                          // 图片尺寸
    bool size_mismatch = false;                                       // 有没有尺寸不一致的图
    int not_found = 0;                                                // 没找到板子的张数

    printf("序号  文件                 状态\n");
    printf("------------------------------------------------------------\n");
    for (const auto & f : files) {
        cv::Mat img = cv::imread(f, cv::IMREAD_COLOR);                // 读图
        if (img.empty()) {                                            // 读不了
            printf("      %-20s 读不了，跳过\n", f.c_str());
            continue;
        }
        if (img_size.width == 0) img_size = img.size();               // 第一张定下尺寸
        else if (img.size() != img_size) {                            // 尺寸和第一张不一样
            printf("      %-20s ⚠ 尺寸 %dx%d 和第一张 %dx%d 不一致，跳过\n",
                   f.c_str(), img.cols, img.rows, img_size.width, img_size.height);
            size_mismatch = true;
            continue;
        }

        cv::Mat gray;
        cv::cvtColor(img, gray, cv::COLOR_BGR2GRAY);                  // 转灰度
        std::vector<cv::Point2f> corners;
        if (!findBoard(gray, board, use_sb, corners)) {               // 没找到
            ++not_found;
            printf("      %-20s ✗ 没找到棋盘格\n", f.c_str());
            continue;
        }
        refineCorners(gray, corners);                                 // 亚像素细化

        if (show) {                                                   // 需要的话弹窗看一下
            cv::Mat dbg = img.clone();
            cv::drawChessboardCorners(dbg, board, corners, true);
            cv::imshow("corners (any key = next)", dbg);
            cv::waitKey(0);
        }

        obj_points.push_back(objp);                                   // 收下 3D 点
        img_points.push_back(corners);                                // 收下 2D 点
        used_files.push_back(f);
        printf("[%3zu] %-20s ✓ %d 个角点\n", used_files.size(), f.c_str(),
               static_cast<int>(corners.size()));
    }

    printf("------------------------------------------------------------\n");
    printf("有效 %zu 张，没找到板子 %d 张\n", used_files.size(), not_found);
    if (show) cv::destroyAllWindows();
    if (size_mismatch) {
        printf("⚠ 有图片尺寸不一致（被跳过了）。标定要求所有图片尺寸完全相同。\n");
    }

    if (static_cast<int>(used_files.size()) < min_views) {             // 张数不够
        printf("\n✗ 有效图片只有 %zu 张，少于要求的 %d 张，标不出来。\n",
               used_files.size(), min_views);
        printf("  回去多拍几张：正对、左右倾、上下俯仰、远近都要有，15~25 张最合适。\n");
        return 1;
    }

    // ---- 标定 ----
    printf("\n>>> 开始标定（%zu 张，图片尺寸 %dx%d）...\n",
           used_files.size(), img_size.width, img_size.height);

    cv::Mat camera_matrix, distort_coeffs;                            // 要解的内参和畸变
    std::vector<cv::Mat> rvecs, tvecs;                                // 每张图的外参
    int flags = fix_k3 ? cv::CALIB_FIX_K3 : 0;                        // 视场角小时不需要 k3
    // 默认迭代 30 次有时会发散，所以放到 100 次（和原版一致）
    auto criteria = cv::TermCriteria(
        cv::TermCriteria::COUNT + cv::TermCriteria::EPS, 100, DBL_EPSILON);
    double rms = 0;
    try {
        rms = cv::calibrateCamera(
            obj_points, img_points, img_size, camera_matrix, distort_coeffs, rvecs, tvecs,
            flags, criteria);
    } catch (const cv::Exception & e) {                               // 标定失败
        printf("✗ 标定失败：%s\n", e.what());
        printf("  常见原因：图片太少 / 全是同一个角度 / 板子规格填错了。\n");
        return 1;
    }

    // ---- 逐张算重投影误差，找出最差的几张 ----
    struct ViewErr {
        int idx;
        std::string file;
        double mean_err;
    };
    std::vector<ViewErr> errs;                                        // 每张图的平均误差
    double err_sum = 0;                                               // 所有角点的误差和
    size_t total_points = 0;                                          // 角点总数
    for (size_t i = 0; i < obj_points.size(); i++) {
        std::vector<cv::Point2f> reproj;
        cv::projectPoints(obj_points[i], rvecs[i], tvecs[i],
                          camera_matrix, distort_coeffs, reproj);     // 把 3D 点投回图像
        double s = 0;
        for (size_t j = 0; j < reproj.size(); j++) {
            s += cv::norm(img_points[i][j] - reproj[j]);              // 实际角点和投影点的距离
        }
        double mean = s / static_cast<double>(reproj.size());
        errs.push_back({static_cast<int>(i), used_files[i], mean});
        err_sum += s;
        total_points += reproj.size();
    }
    double mean_all = total_points ? err_sum / static_cast<double>(total_points) : 0.0;

    // ---- 输出结果 ----
    double fx = camera_matrix.at<double>(0, 0);
    double fy = camera_matrix.at<double>(1, 1);
    double cx = camera_matrix.at<double>(0, 2);
    double cy = camera_matrix.at<double>(1, 2);

    printf("\n==================================================================\n");
    printf("✓ 标定完成\n");
    printf("==================================================================\n");
    printf("有效图片      ：%zu 张\n", used_files.size());
    printf("图片尺寸      ：%dx%d\n", img_size.width, img_size.height);
    printf("重投影误差RMS ：%.4f px      （cv::calibrateCamera 返回值）\n", rms);
    printf("重投影误差均值：%.4f px      （所有角点距离的平均值）\n", mean_all);
    if (rms < 0.5)      printf("评价          ：很好\n");
    else if (rms < 1.0) printf("评价          ：能用，但删掉误差最大的几张重标会更准\n");
    else                printf("评价          ：偏差偏大，看下面列出来的图，多半是拍糊了或板子规格填错\n");

    printf("\n视角偏角：水平 FOV %.1f° ，垂直 FOV %.1f°\n",
           2.0 * std::atan(img_size.width / (2.0 * fx)) * 57.2957795,
           2.0 * std::atan(img_size.height / (2.0 * fy)) * 57.2957795);

    // 误差最大的 5 张列出来，方便删了重标
    std::sort(errs.begin(), errs.end(),
              [](const ViewErr & a, const ViewErr & b) { return a.mean_err > b.mean_err; });
    printf("\n误差最大的几张（删掉它们再标一次通常能明显变好）：\n");
    for (size_t i = 0; i < errs.size() && i < 5; i++) {
        printf("  %-24s %.3f px\n", errs[i].file.c_str(), errs[i].mean_err);
    }

    // ---- 打印能粘进 tuning.yaml 的 camera 节点 ----
    bool res_ok = true;
    double scale = 1.0;
    checkResolution(cfg, img_size, res_ok, scale);                    // 核对分辨率

    printf("\n---------- ① 粘进 %s 的 camera 节点 ----------\n", cfg.c_str());
    printf("camera:\n");
    printf("  fx: %.2f\n", fx);
    printf("  fy: %.2f\n", fy);
    printf("  cx: %.2f\n", cx);
    printf("  cy: %.2f\n", cy);
    if (!res_ok) {
        printf("\n（如果不想重拍，用下面这组按 %.4f 换算过的值，分辨率差得越多越不准）\n", scale);
        printf("camera:\n");
        printf("  fx: %.2f\n", fx * scale);
        printf("  fy: %.2f\n", fy * scale);
        printf("  cx: %.2f\n", cx * scale);
        printf("  cy: %.2f\n", cy * scale);
    }

    // ---- 打印原版格式的 camera_matrix / distort_coeffs ----
    printf("\n---------- ② 粘进 spr_vision_26 的 configs/calibration.yaml ----------\n");
    printf("camera_matrix: [%.6f, 0, %.6f, 0, %.6f, %.6f, 0, 0, 1]\n",
           fx, cx, fy, cy);
    printf("distort_coeffs: [");
    for (int i = 0; i < static_cast<int>(distort_coeffs.total()); i++) {
        printf("%.8f%s", distort_coeffs.at<double>(i),
               (i + 1 < static_cast<int>(distort_coeffs.total())) ? ", " : "");
    }
    printf("]\n");

    // ---- 写 intrinsics.yaml ----
    if (do_save) {
        cv::FileStorage fs(out_yaml, cv::FileStorage::WRITE);         // 写文件
        if (fs.isOpened()) {
            fs << "image_width"  << img_size.width;                   // 内参对应的分辨率
            fs << "image_height" << img_size.height;
            fs << "pattern_cols" << cols;                             // 标定板规格
            fs << "pattern_rows" << rows;
            fs << "square_mm"    << square;
            fs << "views_used"   << static_cast<int>(used_files.size());
            fs << "rms_error_px" << rms;
            fs << "mean_error_px" << mean_all;
            fs << "camera_matrix" << camera_matrix;                   // 3x3
            fs << "distort_coeffs" << distort_coeffs;                 // 1x5
            fs << "tuning_fx" << fx;                                  // 下面四个就是粘进 tuning.yaml 的值
            fs << "tuning_fy" << fy;
            fs << "tuning_cx" << cx;
            fs << "tuning_cy" << cy;
            fs.release();
            printf("\n✓ 已写入 %s\n", out_yaml.c_str());
        } else {
            printf("\n✗ 写不了 %s（目录不存在或没权限）\n", out_yaml.c_str());
        }
    }

    // ---- 去畸变样张：肉眼检查（直线弯不弯）----
    if (save_undistort > 0) {
        printf("\n生成去畸变样张...\n");
        for (int i = 0; i < save_undistort && i < static_cast<int>(used_files.size()); i++) {
            cv::Mat img = cv::imread(used_files[i], cv::IMREAD_COLOR);
            if (img.empty()) continue;
            cv::Mat und;
            cv::undistort(img, und, camera_matrix, distort_coeffs);   // 按标定结果去畸变
            char name[1024];
            snprintf(name, sizeof(name), "%s/undistorted_%d.jpg", in_dir.c_str(), i + 1);
            cv::imwrite(name, und);
            printf("  %s\n", name);
        }
        printf("  看这些图里的棋盘格直线还弯不弯 —— 弯说明标定没收敛好。\n");
    }

    printf("\n告一段落。要回到原版流程，把上面的 camera_matrix / distort_coeffs\n");
    printf("填进 spr_vision_26/configs/calibration.yaml 就能接着做手眼标定。\n");
    return 0;
}
