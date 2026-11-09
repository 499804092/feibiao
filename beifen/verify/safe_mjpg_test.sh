#!/usr/bin/env bash
# ============================================================================
# 【安全版】MJPG 出流测试 —— 只取流一次，不写任何控制项，不杀进程
# ----------------------------------------------------------------------------
# 安全设计：
#   ① --no-cfg    → 完全不写 V4L2 控制项（上次的破坏源就是控制项写入）
#   ② 不加 timeout → 让程序自己的看门狗在 10 秒时干净退出
#                    （外部杀进程属于「流运行中异常中断」，才是危险操作）
#   ③ 只测一次    → 失败就停，不做第二轮，避免反复冲击 USB 设备
#   ④ 前后都查进程 → 确认没有残留占用
#   （脚本里那条 timeout 40 只是最后保险，看门狗 10 秒就会自己退出，它不会触发）
# ============================================================================
cd /home/rsve/Desktop/feibiao || exit 1

echo "############ 0) 取流之前：确认没有残留进程 ############"
if pgrep -a -f 'uvc_detect_view|Dart|ffmpeg|ocv_' | grep -v pgrep; then
  echo "  ⚠️ 上面有残留进程，中止测试（避免两个程序抢相机）"
  exit 1
else
  echo "  ✅ 无残留进程，相机空闲"
fi

echo
echo "############ 1) 只读查看相机状态（不取流、不写任何东西）############"
echo "  --- 设备 ---"
ls -l /dev/video0 2>&1 | sed 's/^/    /'
echo "  --- 当前控制值（只读 G_CTRL）---"
for c in auto_exposure exposure_time_absolute brightness gamma; do
  printf '    %-26s = %s\n' "$c" "$(v4l2-ctl -d /dev/video0 --get-ctrl=$c 2>/dev/null | sed 's/.*: //')"
done
echo "  --- 驱动报告的 640x480 支持格式（只读 ENUM_FMT）---"
v4l2-ctl -d /dev/video0 --list-formats-ext 2>/dev/null \
  | grep -A2 "640x480" | grep -E "'|640x480|Interval" | head -8 | sed 's/^/    /'

echo
echo "############ 2) 唯一的一次取流测试 ############"
echo "  格式: MJPG 640x480 @120（--no-cfg：不写任何控制项）"
echo "  预期: 出流正常就在 5 秒后自己退出；出不了流则看门狗 10 秒后打印排查步骤退出"
echo "  ---- 程序输出开始 ----"
timeout 40 ./uvc_detect_view --no-cfg --no-model --headless --seconds 5 2>&1
rc=$?
echo "  ---- 程序输出结束（退出码 $rc）----"
case "$rc" in
  0)   echo "  ⇒ ✅ 出流正常，MJPG 通路是好的" ;;
  1)   echo "  ⇒ ❌ 出不了流（看门狗主动报错退出，这是干净退出，不是被杀）" ;;
  124) echo "  ⇒ ⚠️ 看门狗没生效、被外层 timeout 兜住了（异常情况，需要看代码）" ;;
  *)   echo "  ⇒ ⚠️ 退出码 $rc" ;;
esac

echo
echo "############ 3) 取流之后：确认相机已释放 ############"
sleep 1
if pgrep -a -f 'uvc_detect_view|Dart|ffmpeg' | grep -v pgrep; then
  echo "  ⚠️ 还有进程残留"
else
  echo "  ✅ 无残留进程，相机已释放"
fi
