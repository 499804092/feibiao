#!/bin/bash
# ============================================================================
# build_calib.sh —— 编译 calib_usb 里的两个标定程序
# ----------------------------------------------------------------------------
# 用法：
#   bash build_calib.sh
#
# 只要 OpenCV 4，不需要 OpenVINO、不需要 yaml-cpp、不需要 fmt。
# 虚拟机/笔记本上跑标定用这套就够了；开发板上也一样能编（但你不会在板子上标定）。
#
# 缺 OpenCV 时先装：
#   sudo apt install -y build-essential cmake pkg-config libopencv-dev
# ============================================================================
set -e
cd "$(dirname "$0")"

CXX="${CXX:-g++}"
FLAGS="-std=c++17 -O2 -Wall"

if ! pkg-config --exists opencv4; then
    echo "✗ 找不到 opencv4，先装："
    echo "    sudo apt install -y libopencv-dev"
    exit 1
fi

echo "OpenCV 版本：$(pkg-config --modversion opencv4)"
OPENCV="$(pkg-config --cflags --libs opencv4)"

for src in calib_capture_usb calib_camera_usb; do
    echo ">>> 编译 $src"
    # shellcheck disable=SC2086
    $CXX $FLAGS "$src.cpp" -o "$src" $OPENCV
done

echo
echo "✓ 编译完成："
ls -la calib_capture_usb calib_camera_usb
echo
echo "下一步："
echo "  ./calib_capture_usb --auto        # 拍标定图"
echo "  ./calib_camera_usb                # 算内参"
