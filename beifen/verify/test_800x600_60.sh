#!/usr/bin/env bash
# 安全测试：MJPG 800x600 @60 能不能出流（只测一次，不写控制项，看门狗自己退出）
cd /home/rsve/Desktop/feibiao/beifen/verify || exit 1

echo "############ 0) 相机关闲检查 ############"
if pgrep -a -f 'safe_stream_test|uvc_detect_view|Dart|ffmpeg' | grep -v pgrep; then
  echo "  ⚠️ 有残留进程，中止"; exit 1
fi
echo "  ✅ 无残留进程"

echo
echo "############ 1) 只读确认控制器（应为 xhci，因为你改回 3.1 了）############"
lsusb -t 2>/dev/null | grep -E 'root_hub|Video' | sed 's/^/    /'

echo
echo "############ 2) 只读确认 800x600 MJPG@60 这一档存在 ############"
v4l2-ctl -d /dev/video0 --list-formats-ext 2>/dev/null \
  | awk '/MJPG/{m=1} /YUYV/{m=0} m' | grep -A1 "800x600" | sed 's/^/    /'

echo
echo "############ 3) 唯一的一次测试：MJPG 800x600 @60 ############"
timeout 60 ./safe_stream_test 800 600 60 MJPG 30 15 2>&1
rc=$?
case "$rc" in
  0)   echo "  ⇒ ✅ 800x600@60 可用" ;;
  1)   echo "  ⇒ ❌ 800x600@60 出不了流" ;;
  124) echo "  ⇒ ⚠️ 看门狗没生效、被外层兜住（异常）" ;;
  *)   echo "  ⇒ ⚠️ 退出码 $rc" ;;
esac

echo
echo "############ 4) 取流之后：确认已释放 ############"
sleep 1
if pgrep -a -f 'safe_stream_test|uvc_detect_view' | grep -v pgrep; then
  echo "  ⚠️ 有残留"
else
  echo "  ✅ 无残留，相机已释放"
fi
