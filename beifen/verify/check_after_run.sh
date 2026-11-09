#!/usr/bin/env bash
# 确认相机已释放 + 控制值没有被改动过
echo "--- 残留相机进程（空=已释放）---"
pgrep -a -f 'uvc_detect_view|uvc_view|uvc_test|uvc_orient|Dart' | grep -v pgrep || echo "  无残留，相机已释放"

echo
echo "--- 相机当前控制值（应与加功能前一致）---"
for c in auto_exposure exposure_time_absolute brightness gamma backlight_compensation power_line_frequency; do
  v=$(v4l2-ctl -d /dev/video0 --get-ctrl="$c" 2>/dev/null | sed 's/.*: //')
  printf '  %-28s = %s\n' "$c" "${v:-<读取失败>}"
done
