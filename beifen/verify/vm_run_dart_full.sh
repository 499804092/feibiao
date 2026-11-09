#!/usr/bin/env bash
# VM：编译 + 完整跑一次 Dart（不截断输出）
FB=/home/rsve/Desktop/feibiao
cd "$FB/Vision_Code/build" || exit 1

echo "===== 1) 编译 ====="
make -j$(nproc) > /tmp/mk.log 2>&1
echo "make rc=$?   error=$(grep -c 'error:' /tmp/mk.log)   warning=$(grep -c 'warning:' /tmp/mk.log)"
grep -n 'error:' /tmp/mk.log | head -20
ls -l Dart | sed 's/^/  /'

echo
echo "===== 2) 实测：跑 Dart 20 秒，输出全打出来 ====="
export DISPLAY=:0
timeout 20 ./Dart > /tmp/dart.out 2>&1
rc=$?
echo "  退出码=$rc （124 = 被 timeout 杀掉，说明一直在正常跑）"
echo "  ---- 输出开始（完整）----"
cat /tmp/dart.out
echo "  ---- 输出结束 ----"

echo
echo "===== 3) 跑完检查残留 ====="
sleep 1
pgrep -a -f 'Dart' | grep -v pgrep | sed 's/^/  ⚠️ 残留: /' || echo "  ✅ 无残留"
