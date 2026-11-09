// ============================================================================
// ColorDetector.cpp —— 传统视觉识别引导灯（HSV 颜色分割 + 形状筛选）
// 算法说明见 ColorDetector.hpp 顶部（来源：大连理工大学飞镖算法【进阶版】）
// ============================================================================

#include "color/ColorDetector.hpp"                                   // 自己的头文件（里面有 Params / Light 的定义）

#include <algorithm>                                                 // std::sort / std::min / std::max
#include <cmath>                                                     // std::lround：按比例算亮度门槛时四舍五入
#include <cstdio>                                                    // printf：打印参数用

// ============================================================================
// Params::fromJson() —— 从 config/tuning.yaml 的 "color" 节点读参数
//   目的是让阈值可以"改配置就生效"，不用重新编译
// ============================================================================
ColorDetector::Params ColorDetector::Params::fromJson(const cv::FileNode &node) {   // 入参是配置里的 color 节点
    Params p;                                                        // 先构造一个对象，此时它持有的是代码里的默认值

    if (node.empty()) {                                              // 配置里根本没写 color 节点
        printf("[ColorDetector] 没有拿到 color 配置节点，全部用默认参数\n");  // 提示一下，方便排查
        return p;                                                    // 直接返回默认参数（= 大连理工那套）
    }                                                                // if 结束

    if (!node["h_min"].empty()) node["h_min"] >> p.h_min;            // 有 h_min 就覆盖；>> 是 OpenCV 的"从配置读值"写法
    if (!node["h_max"].empty()) node["h_max"] >> p.h_max;            // 色调上限
    if (!node["s_min"].empty()) node["s_min"] >> p.s_min;            // 饱和度下限
    if (!node["s_max"].empty()) node["s_max"] >> p.s_max;            // 饱和度上限
    if (!node["v_min"].empty()) node["v_min"] >> p.v_min;            // 亮度下限
    if (!node["v_max"].empty()) node["v_max"] >> p.v_max;            // 亮度上限

    if (!node["aspect_max"].empty())      node["aspect_max"]      >> p.aspect_max;      // 长宽比上限（1.60）
    if (!node["extent_min"].empty())      node["extent_min"]      >> p.extent_min;      // 填充率下限（0.45）
    if (!node["circularity_min"].empty()) node["circularity_min"] >> p.circularity_min; // 圆度下限（0 = 关闭）
    if (!node["min_area"].empty())        node["min_area"]        >> p.min_area;        // 最小轮廓面积
    if (!node["ref_area"].empty())        node["ref_area"]        >> p.ref_area;        // 面积归一化基准
    if (!node["max_targets"].empty())     node["max_targets"]     >> p.max_targets;     // 最多返回几个目标
    if (!node["morph_size"].empty())      node["morph_size"]      >> p.morph_size;      // 形态学核大小

    if (!node["roi_top"].empty())    node["roi_top"]    >> p.roi_top;                   // ROI 上边界（高度比例）
    if (!node["roi_bottom"].empty()) node["roi_bottom"] >> p.roi_bottom;                // ROI 下边界（高度比例）

    // ---- 【新增】掩膜硬化 + 门内自适应亮度切分 ----
    if (!node["use_hull"].empty())    node["use_hull"]    >> p.use_hull;                // 凸包硬化开关（1/0）
    if (!node["v_ref_ratio"].empty()) node["v_ref_ratio"] >> p.v_ref_ratio;             // 门内自适应亮度比例（0=关）
    if (!node["v_ref_s_min"].empty()) node["v_ref_s_min"] >> p.v_ref_s_min;             // 细化时的饱和度下限
    if (!node["v_ref_min_area"].empty()) node["v_ref_min_area"] >> p.v_ref_min_area;    // 细化门槛：门内最大块至少多大

    p.sanitize();                                                    // 读完做一次自检，防止配置里写了非法值
    return p;                                                        // 返回填好的参数
}                                                                    // fromJson 结束

// ============================================================================
// Params::sanitize() —— 参数自检
//   配置写错了不至于让程序行为诡异（比如 ROI 写反导致画面全黑）
// ============================================================================
void ColorDetector::Params::sanitize() {                             // 无返回值，直接改自己的成员
    if (this->roi_bottom <= this->roi_top ||                         // 下边界 <= 上边界（区间为空）
        this->roi_top < 0.0 ||                                       // 或者上边界是负数
        this->roi_bottom > 1.0) {                                    // 或者下边界超过 1（超出画面）
        if (this->roi_top != 0.0 || this->roi_bottom != 1.0) {       // 只有当用户确实改过 ROI 才提示（默认值不吵）
            printf("[ColorDetector] 警告：ROI 参数不合理 (%.2f~%.2f)，改为全图\n",   // 打印警告
                   this->roi_top, this->roi_bottom);                 // 把非法值打出来
        }                                                            // 内层 if 结束
        this->roi_top = 0.0;                                         // 兜底：上边界改回 0
        this->roi_bottom = 1.0;                                      // 兜底：下边界改回 1（= 全图）
    }                                                                // if 结束
    if (this->morph_size > 0 && this->morph_size < 2) {              // 核大小是 1（1x1 的核等于什么都没做）
        this->morph_size = 0;                                        // 直接当"关闭"处理，语义更清楚
    }                                                                // if 结束
    if (this->max_targets < 1) {                                     // 最多返回个数不能小于 1
        this->max_targets = 1;                                       // 兜底成 1
    }                                                                // if 结束
    if (this->aspect_max < 1.0) {                                    // 长宽比上限不能小于 1（长宽比恒 >= 1）
        this->aspect_max = 1.0;                                      // 兜底成 1：等于只接受正方形
    }                                                                // if 结束
    if (this->extent_min < 0.0 || this->extent_min > 1.0) {          // 填充率是面积比，必须落在 0~1
        this->extent_min = 0.0;                                      // 写反了就当成"关闭这一步"
    }                                                                // if 结束
    if (this->circularity_min < 0.0 || this->circularity_min > 1.0) { // 圆度同样只能在 0~1
        this->circularity_min = 0.0;                                 // 非法值一律当"关闭"
    }                                                                // if 结束
    if (this->v_ref_ratio < 0.0 || this->v_ref_ratio >= 1.0) {       // 自适应亮度比例必须在 [0,1)
        this->v_ref_ratio = 0.0;                                     //   1.0 会把所有像素切掉，直接当"关闭"
    }                                                                // if 结束
    if (this->v_ref_s_min < 0)   this->v_ref_s_min = 0;              // 饱和度下限夹到 [0,255]
    if (this->v_ref_s_min > 255) this->v_ref_s_min = 255;            //   写超了不至于让掩膜全空
    if (this->use_hull != 0)     this->use_hull = 1;                 // 归一到 0/1，配置里写 2 也当开
    if (this->v_ref_min_area < 0.0) this->v_ref_min_area = 0.0;      // 细化门槛不能是负数
}                                                                    // sanitize 结束

// ============================================================================
// Params::print() —— 把参数打印成一行，启动时和调参时都用它确认
// ============================================================================
void ColorDetector::Params::print() const {                          // const：只读，不改自己
    printf("[ColorDetector] HSV绿 H[%d,%d] S[%d,%d] V[%d,%d] | "     // 先打 HSV 六个阈值
           "长宽比<=%.2f 填充率>=%.2f 圆度>=%.2f 最小面积=%.0f 形态学=%d ROI高度=%.0f%%~%.0f%% 最多%d个\n",  // 再打形状/面积/其他参数
           this->h_min, this->h_max, this->s_min, this->s_max,      // 填 H/S/V 的值
           this->v_min, this->v_max,                                 // 填 V 的值
           this->aspect_max, this->extent_min, this->circularity_min, this->min_area,  // 填长宽比/填充率/圆度/最小面积
           this->morph_size,                                         // 填形态学核大小
           this->roi_top * 100.0, this->roi_bottom * 100.0,          // ROI 用百分比显示，更直观
           this->max_targets);                                       // 填最多返回几个
    printf("[ColorDetector] 掩膜硬化 凸包=%s | 门内自适应亮度 比例=%.2f 饱和度下限=%d 门槛面积=%.0f %s\n",  // 【新增】第二行
           this->use_hull ? "开" : "关",                              // 凸包开关
           this->v_ref_ratio,                                        // 亮度比例
           this->v_ref_s_min,                                        // 细化用饱和度下限
           this->v_ref_min_area,                                     // 细化门槛面积
           (this->v_ref_ratio > 0.0) ? "" : "(自适应切分=关闭，用原始绿门)");  // 关的时候说清楚
}                                                                    // print 结束

// ============================================================================
// makeMask() —— BGR 图 → 「绿色区域=255(白)，其余=0(黑)」的二值掩膜
//   对应算法第 1、2、3 步：ROI 截取 → BGR转HSV → 双阈值分割
//   返回的掩膜尺寸与原图一致，所以后面找到的坐标不用换算
// ============================================================================
cv::Mat ColorDetector::makeMask(const cv::Mat &src) const {          // 入参是相机原图（BGR 三通道）
    if (src.empty()) {                                               // 图是空的（相机没给数据）
        return cv::Mat();                                            // 返回空矩阵，调用方会自己判断
    }                                                                // if 结束

    // ---------- 第 1 步：ROI 截取（只保留画面某一段高度）----------
    int y0 = 0;                                                      // ROI 上边界的像素 y（默认从最顶上开始）
    int y1 = src.rows;                                               // ROI 下边界的像素 y（默认到最底下结束）
    if (this->params_.roi_top > 0.0 || this->params_.roi_bottom < 1.0) {  // 只有用户配置了非全图 ROI 才真的裁
        y0 = (int)(src.rows * this->params_.roi_top);                // 高度比例 → 像素坐标
        y1 = (int)(src.rows * this->params_.roi_bottom);             // 同上，下边界
        y0 = std::max(0, std::min(y0, src.rows - 1));                // 夹紧到 [0, rows-1]，防止越界
        y1 = std::max(y0 + 1, std::min(y1, src.rows));               // 夹紧到 [y0+1, rows]，保证区间非空
    }                                                                // if 结束
    cv::Rect roi(0, y0, src.cols, y1 - y0);                          // 构造 ROI 矩形：从 y0 开始，高 y1-y0，宽度整幅
    cv::Mat view = src(roi);                                         // 取出 ROI 子图（这是"视图"，不拷贝数据，很快）

    // ---------- 第 2 步：BGR → HSV ----------
    cv::Mat hsv;                                                     // 存放 HSV 图的矩阵
    cv::cvtColor(view, hsv, cv::COLOR_BGR2HSV);                      // 颜色空间转换：BGR 转 HSV（H 0~179, S/V 0~255）

    // ---------- 第 3 步：双阈值分割，只留绿色 ----------
    cv::Mat mask;                                                    // 存放二值掩膜（白=绿色区域，黑=其他）
    cv::inRange(hsv,                                                 // 在 HSV 图里做区间筛选
                cv::Scalar(this->params_.h_min, this->params_.s_min, this->params_.v_min),  // 低阈值 H,S,V
                cv::Scalar(this->params_.h_max, this->params_.s_max, this->params_.v_max),  // 高阈值 H,S,V
                mask);                                               // 输出：落在区间内=255，区间外=0

    // ---------- 额外一步（原算法没有，把 morph_size 设成 0 可完全关闭）----------
    if (this->params_.morph_size >= 2) {                             // 核大小 >= 2 才做形态学
        cv::Mat k = cv::getStructuringElement(                       // 生成形态学"核"（一个小模板）
                cv::MORPH_ELLIPSE,                                   // 形状选椭圆，比方形更贴合圆形光斑
                cv::Size(this->params_.morph_size, this->params_.morph_size));  // 核的尺寸
        cv::morphologyEx(mask, mask, cv::MORPH_OPEN,  k);            // 开运算：先腐蚀再膨胀 → 去掉孤立小白点（噪点）
        cv::morphologyEx(mask, mask, cv::MORPH_CLOSE, k);            // 闭运算：先膨胀再腐蚀 → 把断开的小块连起来
    }                                                                // if 结束

    // ---------- 第 3.5 步【新增】：门内自适应亮度切分（v_ref_ratio = 0 可完全关闭）----------
    //   要解决什么：
    //     引导灯的绿光会照亮它周围的机壳，那一片【也是绿的、只是暗】，于是和灯连成
    //     一整块近方形的掩膜（实测 180x118，长宽比 1.53，离 aspect_max=1.60 只差 5%）。
    //     后果有两个：① 外接框被撑大 → 测距用的像素宽度失真；② 目标框贴着判据上限，
    //     光晕再散一点就整个丢目标。
    //   为什么用"比例"而不是把 v_min 调高：
    //     光晕的绝对亮度随距离/曝光变化很大。固定阈值调高，远距离/暗光下灯本身也会
    //     被一起切掉（实测真机帧 v_min>=140 就整个丢目标，而且开闭运算救不回来）；
    //     调低则近处/过曝时光晕全进来。按"门内自身最亮"的比例切才能自适应。
    //   为什么不会因此误检：
    //     这一刀【只在严格绿门的最大连通块外接框内】生效。严格绿门本身（s_min=60）
    //     在两张无灯画面里实测零检出，是可靠的"门"；门里没东西时这里直接跳过。
    if (this->params_.v_ref_ratio > 0.0) {                            // 比例 > 0 才启用这步
        std::vector<std::vector<cv::Point>> gate_c;                   // 绿门里的轮廓
        cv::findContours(mask, gate_c,                                // 在当前绿门上找轮廓
                         cv::RETR_EXTERNAL,                           // 只要最外层
                         cv::CHAIN_APPROX_SIMPLE);                    // 只存拐点
        // --- ⓪ 门里最大那块有多大？太小就整段跳过，直接走原来的严格绿门 ---
        //   为什么需要这道闸：细化把 s_min 从 60 放到 v_ref_s_min(20)，等于临时降低门槛。
        //   如果门里本来只有一个几像素的杂散绿点（无灯画面上的绿噪声），细化会把它
        //   "养大"到能通过形状判据 → 凭空多出一个假目标。实测无灯帧上出现过 6x6、
        //   面积 11px² 的假目标就是这样被养出来的。
        //   物理上也合理：光晕撑大掩膜只在【灯近、亮、大】时才发生；远距离小目标
        //   本来没这个问题，不细化反而更稳（不会把目标越切越小、导致测距漂移）。
        double gate_max_area = 0.0;                                   // 绿门里最大连通块的面积
        for (size_t i = 0; i < gate_c.size(); ++i) {                  // 遍历门里的所有轮廓
            double a = cv::contourArea(gate_c[i]);                    // 这个轮廓的面积
            if (a > gate_max_area) gate_max_area = a;                 // 记下最大的
        }                                                            // for 结束
        if (!gate_c.empty() &&                                       // 门里有东西（空=没目标，走原逻辑）
            gate_max_area >= this->params_.v_ref_min_area) {          // 而且够大（太小就懒得细化）
            // --- ① 取面积最大的连通块，它的外接框就是"候选区" ---
            size_t bi = 0;                                            // 最大轮廓的下标
            double ba = -1.0;                                         // 当前最大面积
            for (size_t i = 0; i < gate_c.size(); ++i) {              // 遍历所有轮廓
                double a = cv::contourArea(gate_c[i]);                // 这个轮廓的面积
                if (a > ba) { ba = a; bi = i; }                       // 记下更大的那个
            }                                                        // for 结束
            cv::Rect gb = cv::boundingRect(gate_c[bi]);               // 候选区 = 最大连通块的外接框

            // --- ② 外接框四周外扩 15%，免得把灯的边缘一起切掉 ---
            int px = std::max(4, (int)(gb.width  * 0.15));            // 横向外扩量（至少 4 像素）
            int py = std::max(4, (int)(gb.height * 0.15));            // 纵向外扩量（至少 4 像素）
            cv::Rect box(gb.x - px, gb.y - py,                        // 外扩后的候选区：左上角往左上挪
                         gb.width + 2 * px, gb.height + 2 * py);      //   宽高各加两个外扩量
            box &= cv::Rect(0, 0, mask.cols, mask.rows);              // 与图像范围求交，防止越界
            if (box.width <= 0 || box.height <= 0) {                  // 求交后空了（理论上不会发生）
                box = gb;                                             // 兜底退回原始外接框
            }                                                        // if 结束

            // --- ③ 量出候选区里"最亮的像素"，按比例算出这一刀的门槛 ---
            std::vector<cv::Mat> ch;                                  // HSV 的三个通道
            cv::split(hsv, ch);                                       // 拆开：ch[0]=H ch[1]=S ch[2]=V
            cv::Mat in_box = cv::Mat::zeros(mask.size(), CV_8UC1);    // 候选区掩膜（框内 255，框外 0）
            in_box(box).setTo(255);                                   // 把候选区涂白
            double vmax = 0.0;                                        // 候选区内的最高亮度
            cv::minMaxLoc(ch[2], nullptr, &vmax, nullptr, nullptr, in_box);  // 只在候选区内找最大值
            int v_thr = (int)std::lround(this->params_.v_ref_ratio * vmax);  // 比例 → 亮度门槛
            if (v_thr < this->params_.v_min) {                        // 门槛不能低于原来的绝对下限
                v_thr = this->params_.v_min;                          //   （否则等于没切，还多放进噪声）
            }                                                        // if 结束

            // --- ④ 重新分割：亮度够 + 色调仍在绿色区间 + 饱和度放宽到 v_ref_s_min ---
            //   饱和度必须放宽：过曝的灯芯 S 只有 2~30，卡在 s_min=60 外面进不来。
            //   灯芯本来靠第 4 步"填洞"补上，但环一旦被切断就补不上，所以这里先放它进来。
            cv::Mat refined;                                          // 细化后的掩膜
            cv::inRange(hsv,                                          // 在 HSV 上做区间筛选
                        cv::Scalar(this->params_.h_min, this->params_.v_ref_s_min, v_thr),  // 低阈值 H,S,V
                        cv::Scalar(this->params_.h_max, this->params_.s_max, this->params_.v_max),  // 高阈值
                        refined);                                     // 输出：区间内=255
            cv::bitwise_and(refined, in_box, refined);                // 只保留候选区内的部分

            // --- ⑤ 只留最大的一块，避免把旁边零星的亮点一起带进来 ---
            std::vector<std::vector<cv::Point>> rc;                   // 细化后的轮廓
            cv::findContours(refined, rc,                             // 找轮廓
                             cv::RETR_EXTERNAL,                       // 只要最外层
                             cv::CHAIN_APPROX_SIMPLE);                // 只存拐点
            if (!rc.empty()) {                                        // 确实切出了东西才替换
                size_t rbi = 0;                                       // 最大轮廓的下标
                double rba = -1.0;                                    // 当前最大面积
                for (size_t i = 0; i < rc.size(); ++i) {              // 遍历
                    double a = cv::contourArea(rc[i]);                // 面积
                    if (a > rba) { rba = a; rbi = i; }                // 记下更大的
                }                                                    // for 结束
                cv::Mat keep = cv::Mat::zeros(mask.size(), CV_8UC1);  // 新建一张全黑掩膜
                cv::drawContours(keep, rc, (int)rbi,                  // 只把最大那块画上去
                                 cv::Scalar(255), cv::FILLED);        // FILLED = 连内部一起填实
                mask = keep;                                          // 用细化结果替换掉原来的绿门
            }                                                        // if 结束
        }                                                            // if 结束
    }                                                                // if 结束

    // ---------- 第 4 步【关键】：掩膜填洞 ----------
    //   为什么必须做这一步：
    //     引导灯的灯芯被相机拍成过曝的纯白（实测 HSV = H54 S21 V255），
    //     饱和度只有 21，进不了"绿色"掩膜，于是掩膜中间留了一个大黑洞，
    //     整块变成一个"甜甜圈"。甜甜圈外边界毛糙，圆度只有 0.29，
    //     任何形状判据都会把它当成垃圾滤掉 —— 这就是之前识别不出来的根因。
    //   做法：只取最外层轮廓（RETR_EXTERNAL，洞在里面取不到），
    //         清空掩膜后把这些轮廓按"实心"重画一遍，洞就被填上了。
    //   ★不要用闭运算代替：实测 21x21 闭运算会把引导灯和下方的绿色标志
    //     粘成一整块（外接框 191x177），反而彻底识别失败。
    {                                                                // 用花括号限定这些临时变量的作用域
        std::vector<std::vector<cv::Point>> outer;                   // 存放"最外层轮廓"，每个轮廓是一串点
        cv::findContours(mask, outer,                                // 在当前掩膜上找轮廓
                         cv::RETR_EXTERNAL,                          // 只要最外层 —— 中间的洞是内层，取不到
                         cv::CHAIN_APPROX_SIMPLE);                   // 只存拐点，省内存
        mask.setTo(0);                                               // 先把掩膜整张清空（准备重画）
        cv::drawContours(mask, outer, -1,                            // 把所有最外层轮廓都画出来（-1 = 全部）
                         cv::Scalar(255),                            // 画成白色（= 前景）
                         cv::FILLED);                                // FILLED = 连内部一起填满 → 洞没了
    }                                                                // 作用域结束，outer 自动释放

    if (roi.y != 0 || roi.height != src.rows) {                      // 如果刚才裁过 ROI，掩膜现在只有那一条，尺寸对不上
        cv::Mat full = cv::Mat::zeros(src.rows, src.cols, CV_8UC1);  // 造一张和原图等大的全黑掩膜
        mask.copyTo(full(roi));                                      // 把结果贴回对应的位置
        return full;                                                 // 返回全图尺寸的掩膜（保证坐标=原图坐标）
    }                                                                // if 结束
    return mask;                                                     // 没裁剪过就直接返回
}                                                                    // makeMask 结束

// ============================================================================
// detect() —— 核心接口：识别一帧，返回目标列表
//   对应算法第 5~8 步：找轮廓 → 长宽比筛选 → 填充率筛选 → 取面积最大
//   （第 4 步"掩膜填洞"在 makeMask() 里做）
// ============================================================================
std::vector<ColorDetector::Light> ColorDetector::detect(const cv::Mat &src) {   // 入参：相机原图；返回：目标列表
    std::vector<ColorDetector::Light> out;                           // 结果容器，先建空的
    if (src.empty()) {                                               // 图是空的
        return out;                                                  // 返回空列表
    }                                                                // if 结束

    cv::Mat mask = this->makeMask(src);                              // 第1~3步：拿到绿色区域的二值掩膜
    if (mask.empty()) {                                              // 掩膜生成失败
        return out;                                                  // 返回空列表
    }                                                                // if 结束

    // ---------- 第 5 步：找绿色部分的轮廓 ----------
    std::vector<std::vector<cv::Point>> contours;                    // 轮廓容器：每个轮廓是一串点
    cv::findContours(mask, contours,                                 // 在掩膜上找轮廓
                     cv::RETR_EXTERNAL,                              // 只要最外层轮廓 —— 一个色块就是一个目标
                     cv::CHAIN_APPROX_SIMPLE);                       // 只存拐点，省内存（直线段中间的点不要）

    for (const auto &c : contours) {                                 // 逐个轮廓做筛选
        // ---------- 第 6 步：外接矩形 + 长宽比筛选 ----------
        cv::Rect box = cv::boundingRect(c);                          // 求这个轮廓的"正着放"的外接矩形
        if (box.width <= 0 || box.height <= 0) {                     // 宽或高是 0（退化情况）
            continue;                                                // 跳过这个轮廓
        }                                                            // if 结束
        double w = box.width;                                        // 矩形的宽（像素）
        double h = box.height;                                       // 矩形的高（像素）
        double aspect = (w > h) ? (w / h) : (h / w);                 // 长宽比 = 长边 / 短边，这样恒 >= 1
        if (aspect > this->params_.aspect_max) {                     // 长宽比超过上限（1.60）
            continue;                                                // 太扁/太长（灯带、标志条），不是圆形引导灯 → 跳过
        }                                                            // if 结束

        // ---------- 面积筛选 ----------
        // 【新增】凸包硬化：算面积/填充率/圆度时先用凸包把轮廓"包实"。
        //   为什么：第 4 步"填洞"是靠 drawContours(FILLED) 重画最外层轮廓实现的。
        //   但如果绿环上有一像素的漏缝，RETR_EXTERNAL 会顺着外圈绕进洞里，得到的
        //   是一条自相交的细带子（实测外接框 29x38 而面积只有 196），重画出来仍然是
        //   空心环 → 填充率只有 0.18 → 被 extent_min 判死，目标整个丢失。
        //   （真机实拍帧在 v_min>=140 就是这样死的，而且开/闭运算都救不回来。）
        //   凸包是超集，只会把这种"因为空心而被误杀"的候选救回来，绝不会让原本
        //   合格的候选变得不合格 —— 所以是纯收益的硬化，不会引入新的误杀。
        //   ★注意：外接框和质心仍然取【原轮廓】★ 所以现有掩膜尺寸和输出几何
        //   完全不变，target.width_m 不需要重新标定。
        std::vector<cv::Point> hull;                                 // 凸包顶点（只在启用时才会填）
        const std::vector<cv::Point> *shape = &c;                    // 默认：用原轮廓来算判据
        if (this->params_.use_hull) {                                // 启用了凸包硬化
            cv::convexHull(c, hull);                                 // 求这个轮廓的凸包
            shape = &hull;                                           // 判据改用凸包
        }                                                            // if 结束
        double area = cv::contourArea(*shape);                       // 算围住的面积（像素²）
        if (area < this->params_.min_area) {                         // 面积太小
            continue;                                                // 当噪点丢掉
        }                                                            // if 结束

        // ---------- 第 7 步：填充率筛选 ----------
        //   填充率 = 轮廓面积 ÷ 外接矩形面积，衡量"这块东西有多实心"。
        //   实测引导灯填洞后是 0.70~0.76；细长灯带、断裂碎块、稀疏噪点都很低。
        //   之所以不用圆度：发光光斑的边界是毛糙的，圆度在 0.29~0.57 之间乱跳，
        //   而填充率很稳定 —— 拿它当判据才不会把真的引导灯滤掉。
        if (this->params_.extent_min > 0.0) {                        // 下限 > 0 才启用这一步（0 = 不筛）
            double extent = area / (w * h);                          // 填充率 = 面积 ÷ 外接矩形面积
            if (extent < this->params_.extent_min) {                 // 低于下限（0.45）
                continue;                                            // 太"空"，不是实心光斑 → 跳过
            }                                                        // if 结束
        }                                                            // if 结束

        // ---------- 圆度筛选（可选，默认关闭）----------
        if (this->params_.circularity_min > 0.0) {                   // 圆度下限 > 0 才启用这一步
            double per = cv::arcLength(*shape, true);                // 算周长；true = 轮廓是闭合的（和面积同源，保证口径一致）
            if (per < 1e-6) {                                        // 周长几乎为 0（防除零）
                continue;                                            // 跳过
            }                                                        // if 结束
            double circularity = 4.0 * CV_PI * area / (per * per);   // 圆度 = 4π·面积 / 周长²（正圆 = 1）
            if (circularity < this->params_.circularity_min) {       // 圆度低于下限
                continue;                                            // 不够圆 → 跳过
            }                                                        // if 结束
        }                                                            // if 结束

        // ---------- 通过全部筛选，记录这个目标 ----------
        cv::Moments m = cv::moments(c);                              // 算轮廓的"矩"，用来求质心
        cv::Point2d center;                                          // 存放质心坐标
        if (m.m00 > 1e-6) {                                          // m00 是零阶矩（≈面积），不为 0 才算得出来
            center = cv::Point2d(m.m10 / m.m00, m.m01 / m.m00);      // 质心 = (一阶矩/零阶矩)，比取矩形中心更准
        } else {                                                     // 面积几乎为 0 的退化情况
            center = cv::Point2d(box.x + w / 2.0, box.y + h / 2.0);  // 退而求其次：用外接矩形的中心
        }                                                            // if 结束

        ColorDetector::Light light;                                  // 造一个目标对象
        light.area         = area;                                   // 填原始面积（第 7 步排序用的就是它）
        light.box          = cv::Rect2d(box.x, box.y, w, h);         // 填外接矩形（原图坐标）
        light.center_point = center;                                 // 填质心（原图坐标）
        light.score        = std::min(1.0, area / std::max(1.0, this->params_.ref_area));  // 面积归一化当"置信度"，上限 1.0
        out.push_back(light);                                        // 加入结果列表
    }                                                                // for 遍历轮廓结束

    // ---------- 第 8 步：按面积从大到小排，取最大的（们）----------
    std::sort(out.begin(), out.end(),                                // 对结果列表排序
              [](const ColorDetector::Light &a, const ColorDetector::Light &b) {  // 比较函数
                  return a.area > b.area;                                // 面积大的排前面（降序）
              });                                                        // sort 结束
    if ((int)out.size() > this->params_.max_targets) {               // 排完之后如果数量超过上限
        out.resize(this->params_.max_targets);                       // 截断，只留前面最大的那几个
    }                                                                // if 结束

    return out;                                                      // 返回：面积最大的那个就是基地引导灯
}                                                                    // detect 结束
