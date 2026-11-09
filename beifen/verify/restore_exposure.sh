#!/usr/bin/env bash
# ============================================================================
# restore_exposure.sh —— 还原曝光改动前的状态
# ----------------------------------------------------------------------------
# 把 3 个文件从 .bak_before_exposure_20260921_201923 备份还原，
# 并重新编译工程。还原后曝光行为回到"自动曝光"，和改动前完全一致。
# ============================================================================
set -u
BASE=~/Desktop/feibiao
STAMP=20260921_201923

echo "===================================================="
echo " 还原曝光改动"
echo "===================================================="

restore() {
    local f="$1"
    local bak="$f.bak_before_exposure_$STAMP"
    if [ ! -f "$bak" ]; then
        echo "  ✗ 备份不存在: $bak"
        return 1
    fi
    cp "$bak" "$f"
    echo "  ✓ 已还原: ${f#$BASE/}"
    return 0
}

echo
echo "[1/3] 还原文件..."
restore "$BASE/Vision_Code/config/json/sensors.json"
restore "$BASE/Vision_Code/src/sensors/inc/uvc/UvcCamera.hpp"
restore "$BASE/Vision_Code/src/sensors/src/uvc/UvcCamera.cpp"

echo
echo "[2/3] 确认还原结果..."
echo "  sensors.json 的 uvc 节："
grep -A10 '"uvc"' "$BASE/Vision_Code/config/json/sensors.json" | sed 's/^/    /'
echo
echo "  UvcCamera.hpp 是否还有曝光成员（应该没有）："
if grep -q "exposure_mode_" "$BASE/Vision_Code/src/sensors/inc/uvc/UvcCamera.hpp"; then
    echo "    ✗ 还有残留！"
else
    echo "    ✓ 已清除"
fi
echo "  UvcCamera.cpp 是否还有曝光代码（应该没有）："
if grep -q "CAP_PROP_AUTO_EXPOSURE" "$BASE/Vision_Code/src/sensors/src/uvc/UvcCamera.cpp"; then
    echo "    ✗ 还有残留！"
else
    echo "    ✓ 已清除"
fi

echo
echo "[3/3] 重新编译工程..."
cd "$BASE/Vision_Code/build" || exit 1
make -j4 > /tmp/restore_build.log 2>&1
RC=$?
if [ $RC -eq 0 ]; then
    echo "  ✓ 编译成功"
    ls -la Dart
else
    echo "  ✗ 编译失败，日志尾部："
    tail -10 /tmp/restore_build.log
fi

echo
echo "===================================================="
echo " 还原完成"
echo "===================================================="
echo "现在曝光是【自动模式】，与改动前一致。"
echo
echo "注意：相机控制项可能还残留我设过的值，建议复位一次："
echo "  v4l2-ctl -d /dev/video0 --set-ctrl=auto_exposure=3"
echo "  v4l2-ctl -d /dev/video0 --set-ctrl=backlight_compensation=1"
