#!/usr/bin/env bash
# 只读：查清 OpenVINO / OpenCV 到底是从哪个源装的，好在 NUC 上照装
echo "############ 1) apt 源里的第三方仓库 ############"
grep -rhs '^deb ' /etc/apt/sources.list /etc/apt/sources.list.d/ 2>/dev/null | sed 's/^/  /'
echo "  --- sources.list.d 里的文件 ---"
ls -1 /etc/apt/sources.list.d/ 2>/dev/null | sed 's/^/    /'

echo
echo "############ 2) OpenVINO 相关包的确切名字与版本 ############"
dpkg -l 2>/dev/null | grep -i openvino | awk '{printf "  %-42s %s\n", $2, $3}'

echo
echo "############ 3) OpenCV 相关包 ############"
dpkg -l 2>/dev/null | grep -iE 'libopencv|opencv' | awk '{printf "  %-42s %s\n", $2, $3}' | head -12

echo
echo "############ 4) 编译工具链版本 ############"
echo "  cmake : $(cmake --version 2>/dev/null | head -1)"
echo "  g++   : $(g++ --version 2>/dev/null | head -1)"
echo "  make  : $(make --version 2>/dev/null | head -1)"

echo
echo "############ 5) 头文件和库的实际位置（NUC 上要能对上）############"
for p in /usr/include/opencv4/opencv2/opencv.hpp \
         /usr/lib/x86_64-linux-gnu/libopencv_core.so \
         /usr/include/openvino/openvino.hpp \
         /usr/lib/libopenvino.so; do
  if [ -e "$p" ]; then echo "  ✅ $p"; else echo "  ❌ $p"; fi
done
echo "  --- pkg-config 里 OpenCV 的信息 ---"
pkg-config --modversion opencv4 2>/dev/null | sed 's/^/    opencv4 = /' || echo "    pkg-config 没有 opencv4"
echo "  --- OpenVINO 的 cmake 配置文件 ---"
ls -d /usr/lib/cmake/openvino* /usr/lib/x86_64-linux-gnu/cmake/openvino* 2>/dev/null | sed 's/^/    /'

echo
echo "############ 6) 有没有 Intel GPU（NUC 上可以用 GPU 推理加速）############"
ls /dev/dri 2>/dev/null | sed 's/^/    /' || echo "    没有 /dev/dri"
echo "  --- 当前 detect.json 用的推理设备 ---"
grep -n 'device' /home/rsve/Desktop/feibiao/Vision_Code/config/json/detect.json | sed 's/^/    /'
