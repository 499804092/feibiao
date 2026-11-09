#!/usr/bin/env bash
# 诊断「一打开相机就卡死」——只读检查，不杀进程
echo "############ 1) 现在有哪些相关进程在跑 ############"
ps -eo pid,ppid,stat,etime,pcpu,cmd 2>/dev/null | grep -E 'uvc_|Dart|v4l2|ffmpeg' | grep -v grep | sed 's/^/  /' || echo "  无"
echo
echo "  说明：STAT 里的 D = 卡在内核驱动里（不可中断睡眠），R/S = 正常"

echo
echo "############ 2) 谁占着 /dev/video0 ############"
if command -v fuser >/dev/null; then
  for p in /dev/video0; do
    echo "  $p:"
    fuser -v "$p" 2>&1 | sed 's/^/    /'
  done
fi

echo
echo "############ 3) 内核日志（uvcvideo 报错会在这里）############"
if sudo -n dmesg >/dev/null 2>&1; then
  sudo -n dmesg 2>/dev/null | grep -iE 'uvc|usb.*video|video0' | tail -25 | sed 's/^/  /'
else
  echo "  (需要 sudo 才能读 dmesg，试试下面的方式)"
  dmesg 2>/dev/null | tail -20 | sed 's/^/  /' || echo "  dmesg 读不到（dmesg_restrict=1）"
  echo "  --- /var/log/kern.log 尾部 ---"
  tail -25 /var/log/kern.log 2>/dev/null | sed 's/^/  /' || echo "  读不到 kern.log"
fi

echo
echo "############ 4) 相机当前控制值 ############"
for c in auto_exposure exposure_time_absolute brightness; do
  v=$(v4l2-ctl -d /dev/video0 --get-ctrl="$c" 2>/dev/null | sed 's/.*: //')
  printf '  %-28s = %s\n' "$c" "${v:-<读取失败>}"
done
echo "  (exposure_auto_priority 用我们的工具读)"
cd /home/rsve/Desktop/feibiao/verify 2>/dev/null && ./set_exposure 2>/dev/null | grep -E 'exposure_auto_priority|auto_exposure|exposure_time_abs' | tail -3 | sed 's/^/  /'

echo
echo "############ 5) USB 设备状态 ############"
lsusb 2>/dev/null | grep -iE 'cam|video|webcam|image' | sed 's/^/  /' || echo "  没找到摄像头 USB 设备"
echo "  --- /dev/video* ---"
ls -l /dev/video* 2>&1 | sed 's/^/  /'
