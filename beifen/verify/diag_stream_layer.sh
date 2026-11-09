#!/usr/bin/env bash
# 判定：相机出流坏在哪一层？（全部有 timeout 兜底）
D=/dev/video0
cd /home/rsve/Desktop/feibiao/verify || exit 1

echo "############ 1) 先把相机恢复成出厂默认状态 ############"
v4l2-ctl -d $D --set-ctrl=auto_exposure=3 2>&1 | sed 's/^/  /'
v4l2-ctl -d $D --set-ctrl=exposure_time_absolute=157 2>&1 | sed 's/^/  /'
for c in auto_exposure exposure_time_absolute; do
  printf '  %-28s = %s\n' "$c" "$(v4l2-ctl -d $D --get-ctrl=$c 2>/dev/null | sed 's/.*: //')"
done

echo
echo "############ 2) 当前设备支持的格式 ############"
v4l2-ctl -d $D --list-formats-ext 2>&1 | head -30 | sed 's/^/  /'

echo
echo "############ 3) 绕开 OpenCV，用 ffmpeg 直接读驱动（MJPG 640x480）############"
rm -f /tmp/t1.jpg
timeout 20 ffmpeg -hide_banner -loglevel warning -f v4l2 -input_format mjpeg \
  -video_size 640x480 -i $D -frames:v 3 -y /tmp/t1.jpg 2>&1 | tail -6 | sed 's/^/  /'
rc=${PIPESTATUS[0]}
if [ -s /tmp/t1.jpg ]; then
  echo "  ✅ ffmpeg MJPG 640x480 成功，抓到 $(stat -c%s /tmp/t1.jpg) 字节"
else
  echo "  ❌ ffmpeg MJPG 640x480 失败或卡死（rc=$rc）"
fi

echo
echo "############ 4) 低带宽格式再试一次（YUYV 320x240）############"
rm -f /tmp/t2.jpg
timeout 20 ffmpeg -hide_banner -loglevel warning -f v4l2 -input_format yuyv422 \
  -video_size 320x240 -i $D -frames:v 3 -y /tmp/t2.jpg 2>&1 | tail -6 | sed 's/^/  /'
rc=${PIPESTATUS[0]}
if [ -s /tmp/t2.jpg ]; then
  echo "  ✅ ffmpeg YUYV 320x240 成功，抓到 $(stat -c%s /tmp/t2.jpg) 字节"
else
  echo "  ❌ ffmpeg YUYV 320x240 失败或卡死（rc=$rc）"
fi

echo
echo "############ 5) USB 底层是否有报错 ############"
tail -40 /var/log/kern.log 2>/dev/null | grep -iE 'uvc|usb 3-2|xhci|urb|bandwidth|isoc' | tail -15 | sed 's/^/  /' || echo "  无相关日志"

echo
echo "############ 6) 残留进程 ############"
pgrep -a -f 'ffmpeg|uvc_detect_view|v4l2-ctl' | grep -v pgrep | sed 's/^/  /' || echo "  无"
