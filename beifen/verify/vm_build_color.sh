#!/usr/bin/env bash
# VM 上：清掉模型相关文件 + 全新编译，验证改成颜色识别后能不能编过
FB=/home/rsve/Desktop/feibiao

echo "===== 1) 删掉旧模型识别代码 ====="
rm -rf "$FB/Vision_Code/src/detect/inc/openvino" "$FB/Vision_Code/src/detect/src/openvino"
echo "  已删 openvino 目录"

echo
echo "===== 2) 模型文件移出 config ====="
mkdir -p "$FB/beifen/模型已弃用"
[ -d "$FB/Vision_Code/config/openvino" ] && mv "$FB/Vision_Code/config/openvino" "$FB/beifen/模型已弃用/" && echo "  已移走 config/openvino"

echo
echo "===== 3) 全新编译（不用 OpenVINO 了）====="
rm -rf "$FB/Vision_Code/build"
mkdir -p "$FB/Vision_Code/build"
cd "$FB/Vision_Code/build" || exit 1
cmake .. > /tmp/cm.log 2>&1
echo "cmake rc=$?"
grep -E 'OpenCV|OpenVINO|ENABLE_CAPTURE|Error|error' /tmp/cm.log | head -8

make -j$(nproc) > /tmp/mk.log 2>&1
echo "make rc=$?   error=$(grep -c 'error:' /tmp/mk.log)   warning=$(grep -c 'warning:' /tmp/mk.log)"
grep -n 'error:' /tmp/mk.log | head -25
echo "  --- 产物 ---"
ls -l Dart 2>&1

echo
echo "===== 4) 编译看画面工具（只要 OpenCV）====="
cd "$FB" || exit 1
g++ -std=c++17 -O2 -Wall uvc_color_view.cpp \
    Vision_Code/src/detect/src/color/ColorDetector.cpp \
    -I Vision_Code/src/detect/inc \
    -o uvc_color_view $(pkg-config --cflags --libs opencv4) > /tmp/uv.log 2>&1
echo "g++ rc=$?   error=$(grep -c 'error:' /tmp/uv.log)"
grep -n 'error:' /tmp/uv.log | head -20
ls -l uvc_color_view 2>&1
