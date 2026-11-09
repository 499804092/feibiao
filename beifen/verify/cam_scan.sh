#!/usr/bin/env bash
# 依次测试多组相机参数，找出能稳定出图的那一组
# 用法： ./cam_scan.sh
cd "$(dirname "$0")" || exit 1

echo "===== 相机参数扫描：找能稳定出图的配置 ====="
printf "%-18s %s\n" "配置(宽 高 帧率)" "结果"
printf "%-18s %s\n" "----------------" "----"

test_one() {
    local w=$1 h=$2 f=$3
    local out
    out=$(timeout 12 ./uvc_view --headless 0 "$w" "$h" "$f" 2>&1)
    if echo "$out" | grep -q "平均"; then
        local fps
        fps=$(echo "$out" | grep "平均" | tail -1)
        printf "%-18s ✓ %s\n" "$w x $h @ $f" "$fps"
    elif echo "$out" | grep -q "打不开"; then
        printf "%-18s ✗ 设备打不开（被占用或已掉线）\n" "$w x $h @ $f"
    else
        printf "%-18s ✗ 协商成功但收不到帧（select timeout）\n" "$w x $h @ $f"
    fi
    sleep 2
}

test_one 320 240 30
test_one 320 240 120
test_one 640 480 30
test_one 640 480 60
test_one 640 480 120
test_one 1280 720 30

echo "==========================================="
