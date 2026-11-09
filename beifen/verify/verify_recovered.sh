#!/usr/bin/env bash
# 相机恢复后的验证（全部有 timeout 兜底，只用 SIGTERM，不用 SIGKILL）
cd /home/rsve/Desktop/feibiao/verify || exit 1

echo "############ 0) 有没有残留进程占着相机 ############"
pgrep -a -f 'uvc_|ocv_min|ffmpeg|Dart' | grep -v pgrep | sed 's/^/  /' || echo "  无残留，相机空闲"

echo
echo "############ 1) 相机是否在（只读，不取流）############"
ls -l /dev/video* 2>&1 | sed 's/^/  /'
echo "  --- 当前曝光相关控制值 ---"
for c in auto_exposure exposure_time_absolute brightness gamma; do
  printf '    %-26s = %s\n' "$c" "$(v4l2-ctl -d /dev/video0 --get-ctrl=$c 2>/dev/null | sed 's/.*: //')"
done
./set_exposure 2>/dev/null | grep -E 'exposure_auto_priority' | tail -1 | sed 's/^/    /'

echo
echo "############ 2) 出流验证（工程同款设置，最多 25 秒）############"
timeout 25 ./ocv_min 5 2>/dev/null | grep -vE '^\[ WARN' | sed 's/^/  /'
rc=${PIPESTATUS[0]}
if [ "$rc" = "0" ]; then
  echo "  ⇒ ✅ 相机恢复正常，能出流"
elif [ "$rc" = "124" ]; then
  echo "  ⇒ ❌ 仍然卡死 —— 需要你在 VMware 里再断开/重连一次相机"
else
  echo "  ⇒ ❌ 失败 rc=$rc"
fi
sleep 2

echo
echo "############ 3) 完整链路：真实程序 + 模型 + 检测（跑 8 秒）############"
export DISPLAY=:0
timeout 70 ./uvc_detect_view --orientation 2 --thr 0.20 --headless --seconds 8 2>&1 | tail -18 | sed 's/^/  /'
rc=${PIPESTATUS[0]}
if [ "$rc" = "0" ]; then
  echo "  ⇒ ✅ 预览程序正常（有画面、有检测、正常退出，没有假死）"
elif [ "$rc" = "124" ]; then
  echo "  ⇒ ❌ 卡死（看门狗应该会打印排查步骤）"
else
  echo "  ⇒ ⚠️ 异常退出 rc=$rc"
fi

echo
echo "############ 4) 残留进程 ############"
pgrep -a -f 'uvc_|ocv_min|ffmpeg' | grep -v pgrep | sed 's/^/  /' || echo "  无残留，相机已释放"
