#ifndef __INLINE_VARIABLE_HPP__
#define __INLINE_VARIABLE_HPP__

#include <opencv2/opencv.hpp>
#include "json/ReJson.hpp"

inline ReJson J_CAPTURE("../config/json/capture.json");
// 【改动】下面三个原来分别读 detect.json / sensors.json / track.json 三个文件，
//   现在【全部指向同一个调参文件】config/tuning.yaml。
//
//   为什么合并：调参时要在三个文件之间来回翻、还要记住哪个参数在哪个文件里，
//   非常容易漏改。合并成一个之后，所有能调的东西都在这一个文件里。
//
//   为什么换成 YAML：JSON 标准不支持注释，而调参最需要的就是"这个值调大调小
//   会怎样"的注释。cv::FileStorage 原生支持 YAML 且支持 # 注释（已实测）。
//
//   代码的读取方式【完全没变】：还是 J_DETECT.config_["color"]["h_min"] 这样读，
//   只是换了个来源文件，所以所有模块一行都不用改。
inline ReJson J_DETECT ("../config/tuning.yaml");
inline ReJson J_SENSORS("../config/tuning.yaml");
inline ReJson J_TRACK  ("../config/tuning.yaml");
// 【改动】原来这里还有一行 J_MOUSE("../config/json/mouse.json")。
//   mouse 模块（用鼠标手动画裁剪框的调试工具）已经整体弃用，
//   源码和它的配置文件都移到了 beifen/已弃用-mouse调试框/。
//   这里一并删掉，免得它还去打开一个已经不存在的配置文件。

#endif