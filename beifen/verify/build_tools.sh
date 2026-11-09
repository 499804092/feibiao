#!/usr/bin/env bash
# ============================================================================
# build_tools.sh —— 一键编译 verify/ 下所有验证工具
# ----------------------------------------------------------------------------
# 这些可执行文件不进版本管理（每次同步源码都会丢），每次拿到新代码跑一下即可。
#
# 用法：
#   cd ~/Desktop/feibiao/verify
#   chmod +x build_tools.sh     # 第一次需要
#   ./build_tools.sh
#
# 依赖：OpenCV（必需）、OpenVINO（只有带模型推理的工具需要）
#       sudo apt install libopencv-dev libopenvino-dev
# ============================================================================
set -u
cd "$(dirname "$0")" || exit 1

OPENCV_FLAGS=$(pkg-config --cflags --libs opencv4 2>/dev/null)
if [ -z "$OPENCV_FLAGS" ]; then
    echo "❌ 找不到 OpenCV，请先装：sudo apt install libopencv-dev"
    exit 1
fi

# OpenVINO：只有 class_probe / uvc_detect_view 这两个跑模型的工具需要
OV_FLAGS=""
if [ -f /usr/lib/libopenvino.so ] || pkg-config --exists openvino 2>/dev/null; then
    OV_FLAGS="-lopenvino"
else
    echo "⚠️  没找到 OpenVINO —— class_probe / uvc_detect_view 会编译失败"
    echo "    需要的话装一下：sudo apt install libopenvino-dev"
fi

ok=0; fail=0

build() {   # $1=输出名  $2=额外链接参数  $3..=源文件
    name="$1"; shift
    extra="$1"; shift
    if g++ -std=c++17 -O2 -Wall "$@" -o "$name" $OPENCV_FLAGS $extra 2>"$name.log"; then
        echo "  ✓ $name"
        rm -f "$name.log"
        ok=$((ok+1))
    else
        echo "  ✗ $name   （错误见 $name.log）"
        fail=$((fail+1))
    fi
}

echo "=== 相机工具（只需 OpenCV）==="
build uvc_probe    ""  uvc_probe.cpp
build uvc_test     ""  uvc_test.cpp
build uvc_orient   ""  uvc_orient.cpp
build uvc_view     ""  uvc_view.cpp

echo "=== 检测/识别工具（需要 OpenVINO）==="
build ../../uvc_detect_view "$OV_FLAGS" ../../uvc_detect_view.cpp
build class_probe     "$OV_FLAGS" class_probe.cpp
build detect_logic_test "$OV_FLAGS" detect_logic_test.cpp

echo "=== 算法校验工具 ==="
build verify_detect ""  verify_detect.cpp
build fps_breakdown "$OV_FLAGS" fps_breakdown.cpp
# verify_frame 需要额外带上工程原始的 CRC 实现
if g++ -std=c++17 -O2 -Wall -I. verify_frame.cpp CRC_Check.cpp -o verify_frame 2>verify_frame.log; then
    echo "  ✓ verify_frame"
    rm -f verify_frame.log
    ok=$((ok+1))
else
    echo "  ✗ verify_frame   （错误见 verify_frame.log）"
    fail=$((fail+1))
fi

echo
echo "=== 完成：成功 $ok 个，失败 $fail 个 ==="
echo
echo "常用命令："
echo "  ./uvc_detect_view --orientation 2 --thr 0.20        # 打开相机 + 实时框出目标（需桌面）"
echo "  ./uvc_detect_view --list-ctrls                      # 列相机可调项和范围（不打开画面）"
echo "  ./uvc_detect_view --auto-exp 1 --exposure 80        # 手动曝光（越小越暗、噪声越少）"
echo "  ./uvc_detect_view --img 图片.jpg --out 结果.jpg      # 对单张图片检测并画框"
echo "  ./uvc_view 0 640 480 120 2                          # 只看画面不开检测"
echo "  ./uvc_orient 0 640 480 120                          # 6 种朝向同屏对比"
echo "  ./detect_logic_test --synthetic                     # 用合成图验证筛选逻辑"
echo "  （画面里按 [ ] 实时调曝光，- = 调增益，a 切自动/手动，c 列出可调项）"
[ "$fail" -eq 0 ] || exit 1
