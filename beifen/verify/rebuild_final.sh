#!/usr/bin/env bash
cd /home/rsve/Desktop/feibiao/Vision_Code/build || exit 1
make -j4 > /tmp/bd2.log 2>&1
echo "make rc=$?   error=$(grep -c 'error:' /tmp/bd2.log)   warning=$(grep -c 'warning:' /tmp/bd2.log)"
grep -n 'error:' /tmp/bd2.log | head -10 | sed 's/^/  /'
grep -n 'warning:' /tmp/bd2.log | head -10 | sed 's/^/  /'
ls -l Dart | sed 's/^/  /'

cd /home/rsve/Desktop/feibiao/verify || exit 1
g++ -std=c++17 -O2 -Wall uvc_detect_view.cpp -o uvc_detect_view \
  $(pkg-config --cflags --libs opencv4) -lopenvino 2>/tmp/e3.log \
  && echo "  ✓ uvc_detect_view  error=0 warning=$(grep -c 'warning:' /tmp/e3.log)" \
  || { echo "  ✗ 编译失败"; grep -n 'error:' /tmp/e3.log | head -20; }
