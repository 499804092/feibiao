#!/usr/bin/env bash
# 同步后：确认配置内容 → 重新编译预览工具 → 用图片模式验证配置解析（不碰相机）
echo "############ 1) 配置文件里的 exposure 段 ############"
sed -n '/"exposure"/,/}/p' /home/rsve/Desktop/feibiao/Vision_Code/config/json/sensors.json | sed 's/^/  /'

echo
echo "############ 2) 重新编译 uvc_detect_view ############"
cd /home/rsve/Desktop/feibiao/verify || exit 1
if g++ -std=c++17 -O2 -Wall uvc_detect_view.cpp -o uvc_detect_view $(pkg-config --cflags --libs opencv4) -lopenvino 2>/tmp/ub.log; then
  echo "  ✓ 编译成功 0 error"
else
  echo "  ✗ 编译失败，错误如下："
  grep -n 'error:' /tmp/ub.log | head -20
  exit 1
fi
echo "  warning 数: $(grep -c 'warning:' /tmp/ub.log)"
grep -n 'warning:' /tmp/ub.log | head -10

echo
echo "############ 3) 用图片模式验证配置解析（不碰相机）############"
echo "期望看到：从配置读到曝光: enable=1 auto=1 value=40 gain=-1"
./uvc_detect_view --img /home/rsve/Desktop/feibiao/verify/uvc_frame.png --out /tmp/exp40_check.jpg 2>&1 | head -12
echo "退出码 = $?"
