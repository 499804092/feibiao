#!/usr/bin/env bash
# 只读：确认预览窗口关掉之后相机已释放、控制值没被动过
echo "--- 残留进程 ---"
if pgrep -a -f 'uvc_detect_view|safe_stream_test|Dart|ffmpeg' | grep -v pgrep; then
  echo "  ⚠️ 有残留"
else
  echo "  ✅ 无残留，相机已释放"
fi

echo "--- 相机当前曝光值（只读）---"
for c in auto_exposure exposure_time_absolute brightness gamma; do
  printf '  %-26s = %s\n' "$c" "$(v4l2-ctl -d /dev/video0 --get-ctrl=$c 2>/dev/null | sed 's/.*: //')"
done

echo "--- 相机是否还在虚拟机上 ---"
ls -l /dev/video0 2>&1 | sed 's/^/  /'
