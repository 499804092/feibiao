#!/usr/bin/env bash
# ============================================================================
# fix_camera.sh —— 摄像头打不开时的一键修复
# ----------------------------------------------------------------------------
# 【什么时候用】
#   运行 uvc_view / uvc_detect_view 时报下面任一错误：
#     - can't open camera by index          （设备被占用）
#     - select() timeout                     （设备卡住/被占用）
#     - 打不开 /dev/video0
#
# 【原因】
#   摄像头同一时刻只能被一个程序打开。如果上次的预览窗口是被强制关闭的
#   （Ctrl+C、关终端、kill -9），进程可能变成僵尸一直占着 /dev/video0，
#   后面再运行就永远打不开。
#
# 【用法】
#   cd ~/Desktop/feibiao/verify
#   chmod +x fix_camera.sh     # 第一次需要
#   ./fix_camera.sh
#
#   如果跑完还是不行 → 在 VMware 里把摄像头断开再重新连接一次。
# ============================================================================
set -u

echo "===================================================="
echo " 摄像头占用清理"
echo "===================================================="

# ---------- 1) 找出占用摄像头的进程 ----------
echo
echo "[1/3] 查找占用摄像头的进程..."

FOUND=0
for dev in /dev/video0 /dev/video1; do
    [ -e "$dev" ] || continue
    # fuser 需要权限，用 sudo 试一下；不行就退回到 ps 匹配
    pids=$(sudo -n fuser "$dev" 2>/dev/null || true)
    if [ -n "$pids" ]; then
        echo "  $dev 被这些进程占用：$pids"
        FOUND=1
    fi
done

# 再按进程名兜底扫一遍（即使 fuser 没权限也能查到）
if [ "$FOUND" -eq 0 ]; then
    ps -ef | grep -E "uvc_view|uvc_detect_view|uvc_test|uvc_orient|uvc_probe|class_probe|fps_breakdown|Dart" \
        | grep -v grep | while read -r line; do
            echo "  发现残留进程：$line"
          done
fi

# ---------- 2) 正常终止（用 SIGTERM，不用 -9） ----------
echo
echo "[2/3] 正常终止残留进程（SIGTERM，让它有机会释放设备）..."
for name in uvc_view uvc_detect_view uvc_test uvc_orient uvc_probe class_probe fps_breakdown Dart; do
    if pkill -f "$name" 2>/dev/null; then
        echo "  已终止: $name"
    fi
done
sleep 3

# ---------- 3) 验证摄像头能否出图 ----------
echo
echo "[3/3] 验证摄像头（用最小带宽唤醒，避免再次卡死）..."

if [ ! -e /dev/video0 ]; then
    echo "  ✗ /dev/video0 不存在 —— 摄像头没接进虚拟机"
    echo "    请在 VMware 里挂载："
    echo "      虚拟机(VM) → 可移动设备 → 找到摄像头 → 连接(断开与主机连接)"
    exit 1
fi

out=$(timeout 12 ./uvc_view --headless 0 320 240 30 2>&1)
if echo "$out" | grep -q "平均"; then
    echo "  ✓ 摄像头正常！"
    echo "$out" | grep -E "平均|✓ 画面" | sed 's/^/    /'
    echo
    echo "现在可以正常用了："
    echo "  ./uvc_detect_view --orientation 2 --thr 0.20      # 实时检测（需桌面）"
    echo "  ./uvc_view 0 640 480 120 2                       # 只看画面"
    echo
    echo "★ 重要：退出时按 q 键，不要 Ctrl+C 或直接关窗口 ★"
    exit 0
else
    echo "  ✗ 仍然打不开。请做下面两步："
    echo "    1) 再跑一次本脚本（设备释放有时需要几秒）"
    echo "    2) 还不行 → 在 VMware 里把摄像头【断开】再【重新连接】"
    echo
    echo "  当前设备状态："
    ls -l /dev/video* 2>/dev/null | sed 's/^/    /'
    exit 1
fi
