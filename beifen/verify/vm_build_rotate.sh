#!/usr/bin/env bash
# VM：编译改完的工具 + 查相机占用情况
FB=/home/rsve/Desktop/feibiao
cd "$FB" || exit 1

echo "############ 1) 编译 uvc_color_view（加了旋转功能）############"
g++ -std=c++17 -O2 -Wall uvc_color_view.cpp \
    Vision_Code/src/detect/src/color/ColorDetector.cpp \
    -I Vision_Code/src/detect/inc \
    -o uvc_color_view $(pkg-config --cflags --libs opencv4) 2>/tmp/uv3.log
rc=$?
echo "  g++ rc=$rc  error=$(grep -c 'error:' /tmp/uv3.log)  warning=$(grep -c 'warning:' /tmp/uv3.log)"
grep -n 'error:\|warning:' /tmp/uv3.log | head -15

echo
echo "############ 2) 编译主程序 Dart（配置改过，重编一下）############"
cd "$FB/Vision_Code/build" || exit 1
make -j$(nproc) > /tmp/mk3.log 2>&1
echo "  make rc=$?  error=$(grep -c 'error:' /tmp/mk3.log)"

echo
echo "############ 3) 相机有没有被别的程序占用（你说这个可能影响 120fps）############"
echo "  --- 所有打开 /dev/video0 的进程 ---"
if command -v fuser >/dev/null; then
  sudo -n fuser -v /dev/video0 2>/dev/null || fuser -v /dev/video0 2>/dev/null || echo "    (fuser 需要权限，看下面)"
fi
echo "  --- 名字里带 camera/video/uvc/ffmpeg/opencv 的进程 ---"
ps -eo pid,etime,cmd 2>/dev/null | grep -iE 'uvc|video|ffmpeg|camera|Dart|python.*cv2' | grep -v grep | sed 's/^/    /' || echo "    无"
echo "  --- 当前用户的所有进程（前 15 条）---"
ps -u "$(whoami)" -o pid,etime,cmd 2>/dev/null | head -16 | sed 's/^/    /'

echo
echo "############ 4) 相机当前状态（只读）############"
ls -l /dev/video0 2>&1 | sed 's/^/  /'
for c in auto_exposure exposure_time_absolute; do
  printf '  %-26s = %s\n' "$c" "$(v4l2-ctl -d /dev/video0 --get-ctrl=$c 2>/dev/null | sed 's/.*: //')"
done

echo
echo "############ 5) 确认配置改动 ############"
grep -E 's_min|v_min|min_area|morph_size' "$FB/Vision_Code/config/json/detect.json" | sed 's/^/  /'
echo "  --- sensors.json 里的画面朝向 ---"
grep -E '"orientation"' "$FB/Vision_Code/config/json/sensors.json" | sed 's/^/  /'
