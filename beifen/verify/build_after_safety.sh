#!/usr/bin/env bash
# 编译验证（相机当前不可用，只验证编译）
echo "############ 1) 工程 Dart ############"
cd /home/rsve/Desktop/feibiao/Vision_Code/build || exit 1
make -j4 > /tmp/bd.log 2>&1
echo "  make rc=$?   error=$(grep -c 'error:' /tmp/bd.log)   warning=$(grep -c 'warning:' /tmp/bd.log)"
grep -n 'error:' /tmp/bd.log | head -20 | sed 's/^/    /'
grep -n 'warning:' /tmp/bd.log | head -10 | sed 's/^/    /'
ls -l Dart 2>&1 | sed 's/^/  /'

echo
echo "############ 2) 验证工具 ############"
cd /home/rsve/Desktop/feibiao/verify || exit 1
g++ -std=c++17 -O2 -Wall set_exposure.cpp -o set_exposure 2>/tmp/e1.log \
  && echo "  ✓ set_exposure" || { echo "  ✗ set_exposure"; grep -n 'error:' /tmp/e1.log|head; }
echo "    warning=$(grep -c 'warning:' /tmp/e1.log)"
g++ -std=c++17 -O2 -Wall uvc_detect_view.cpp -o uvc_detect_view \
  $(pkg-config --cflags --libs opencv4) -lopenvino 2>/tmp/e2.log \
  && echo "  ✓ uvc_detect_view" || { echo "  ✗ uvc_detect_view"; grep -n 'error:' /tmp/e2.log|head -20; }
echo "    warning=$(grep -c 'warning:' /tmp/e2.log)"
grep -n 'warning:' /tmp/e2.log | head -10 | sed 's/^/    /'

echo
echo "############ 3) 确认配置已是安全状态 ############"
sed -n '/"exposure"/,/}/p' /home/rsve/Desktop/feibiao/Vision_Code/config/json/sensors.json | sed 's/^/  /'

echo
echo "############ 4) 确认危险代码已清除（应全部为 0）############"
for f in Vision_Code/src/sensors/inc/uvc/V4l2Ctrl.hpp \
         Vision_Code/src/sensors/src/uvc/UvcCamera.cpp \
         verify/uvc_detect_view.cpp; do
  n=$(grep -c 'EXPOSURE_AUTO_PRIORITY, 0' /home/rsve/Desktop/feibiao/$f 2>/dev/null)
  printf '  %-60s 残留 priority=0 写入: %s\n' "$f" "$n"
done
