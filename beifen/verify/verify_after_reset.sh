#!/usr/bin/env bash
# 复位后验证：程序能不能正常出流 + 曝光 40 有没有自动生效 + 检测是否正常
cd /home/rsve/Desktop/feibiao/verify || exit 1
export DISPLAY=:0

echo "############ 0) 相机当前控制值（复位后应为出厂默认）############"
for c in auto_exposure exposure_time_absolute; do
  printf '  %-28s = %s\n' "$c" "$(v4l2-ctl -d /dev/video0 --get-ctrl=$c 2>/dev/null | sed 's/.*: //')"
done

echo
echo "############ 1) 完整链路：读配置(手动40) + 模型 + 检测，跑 6 秒 ############"
echo "（只用 SIGTERM 兜底，不用 SIGKILL，避免再次弄坏 USB 状态）"
timeout 60 ./uvc_detect_view --orientation 2 --thr 0.20 --headless --seconds 6 2>&1 | tail -16
rc=${PIPESTATUS[0]}
case "$rc" in
  0)   echo "  ✅ 正常出流并退出（rc=0）" ;;
  124) echo "  ❌ 仍然卡死（rc=124）" ;;
  *)   echo "  ⚠️ 异常退出 rc=$rc" ;;
esac

echo
echo "############ 2) 曝光 40 是否真的生效 ############"
./uvc_detect_view --list-ctrls 2>&1 | grep -E 'auto_exposure|exposure_time_abs|auto_priority' | sed 's/^/  /'

echo
echo "############ 3) 残留进程 ############"
pgrep -a -f 'uvc_detect_view|ffmpeg' | grep -v pgrep | sed 's/^/  /' || echo "  无残留，相机已释放"
