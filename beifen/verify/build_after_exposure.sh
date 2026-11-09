#!/usr/bin/env bash
# 编译工程 Dart + 验证工具（在正确的工作目录下）
echo "############ 1) 编译工程 Dart ############"
cd /home/rsve/Desktop/feibiao/Vision_Code/build || { echo "❌ build 目录不存在"; exit 1; }
make -j4 > /tmp/build_dart.log 2>&1
rc=$?
echo "make 退出码 = $rc"
echo "--- error 数量: $(grep -c 'error:' /tmp/build_dart.log) ---"
grep -n 'error:' /tmp/build_dart.log | head -30
echo "--- warning 数量: $(grep -c 'warning:' /tmp/build_dart.log) ---"
echo "--- 与曝光相关的告警/错误 ---"
grep -n 'UvcCamera\|V4l2Ctrl\|videodev' /tmp/build_dart.log | head -20
echo "--- 产物 ---"
ls -l Dart 2>&1

echo
echo "############ 2) 编译验证工具 ############"
cd /home/rsve/Desktop/feibiao/verify || exit 1
chmod +x build_tools.sh
./build_tools.sh 2>&1 | tail -25
echo "uvc_detect_view 产物:"
ls -l uvc_detect_view 2>&1
