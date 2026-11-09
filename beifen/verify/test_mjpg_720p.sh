#!/usr/bin/env bash
# ============================================================================
# 安全版：测 MJPG 1280x720 @60（只测一次，失败就停）
# 安全设计：不写任何控制项 / 不重试 / 看门狗自己退出 / 只在最外层留一条 60 秒保险
# ============================================================================
cd /home/rsve/Desktop/feibiao/beifen/verify || exit 1

echo "############ 0) 取流之前：确认相机空闲 ############"
if pgrep -a -f 'safe_stream_test|uvc_detect_view|Dart|ffmpeg' | grep -v pgrep; then
  echo "  ⚠️ 有残留进程，中止"
  exit 1
fi
echo "  ✅ 无残留进程"

echo
echo "############ 1) 只读确认 1280x720 MJPG@60 这一档存在 ############"
v4l2-ctl -d /dev/video0 --list-formats-ext 2>/dev/null \
  | awk '/MJPG/{m=1} /YUYV/{m=0} m' | grep -B1 -A1 "1280x720" | sed 's/^/    /'
echo "  （应能看到 MJPG 1280x720 的 Interval 0.017s / 60fps）"

echo
echo "############ 2) 编译测试工具 ############"
g++ -std=c++17 -O2 -Wall safe_stream_test.cpp -o safe_stream_test \
  $(pkg-config --cflags --libs opencv4) 2>/tmp/sst.log \
  && echo "  ✓ 编译成功 error=0 warning=$(grep -c 'warning:' /tmp/sst.log)" \
  || { echo "  ✗ 编译失败"; grep -n 'error:' /tmp/sst.log | head; exit 1; }

echo
echo "############ 3) 唯一的一次测试：MJPG 1280x720 @60 ############"
echo "  预期：出流正常读满 30 帧后自己退出；出不了流则看门狗 15 秒后自己退出"
echo "  ---- 输出开始 ----"
timeout 60 ./safe_stream_test 1280 720 60 MJPG 30 15 2>&1
rc=$?
echo "  ---- 输出结束（退出码 $rc）----"
case "$rc" in
  0)   echo "  ⇒ ✅ 这个配置能出流" ;;
  1)   echo "  ⇒ ❌ 出不了流（或读到 0 帧）" ;;
  124) echo "  ⇒ ⚠️ 看门狗没生效、被外层兜住了（异常，需要看代码）" ;;
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
