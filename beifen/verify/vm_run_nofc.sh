#!/usr/bin/env bash
# VM：编译 + 实际跑一次 Dart（无飞控测试模式）
FB=/home/rsve/Desktop/feibiao
cd "$FB/Vision_Code/build" || exit 1

echo "===== 1) 编译 ====="
make -j$(nproc) > /tmp/mk.log 2>&1
echo "make rc=$?   error=$(grep -c 'error:' /tmp/mk.log)   warning=$(grep -c 'warning:' /tmp/mk.log)"
grep -n 'error:' /tmp/mk.log | head -20
ls -l Dart | sed 's/^/  /'

echo
echo "===== 2) BOM 检查 ====="
for f in "$FB/Vision_Code/main.cpp" \
         "$FB/Vision_Code/src/sensors/src/serialport/SerialPort.cpp" \
         "$FB/Vision_Code/src/sensors/src/uvc/UvcCamera.cpp"; do
  b=$(head -c 3 "$f" | od -An -tx1 | tr -d ' \n')
  if [ "$b" = "efbbbf" ]; then s=yes; else s=NO; fi
  printf '  %-24s BOM=%s\n' "$(basename $f)" "$s"
done

echo
echo "===== 3) 实测：跑 Dart 15 秒（无飞控测试模式）====="
echo "  ---- 输出开始 ----"
cd "$FB/Vision_Code/build" || exit 1
export DISPLAY=:0
timeout 25 ./Dart 2>&1 | head -40
echo "  ---- 输出结束（退出码 ${PIPESTATUS[0]}）----"

echo
echo "===== 4) 跑完检查残留 ====="
sleep 1
pgrep -a -f 'Dart|uvc_color_view' | grep -v pgrep | sed 's/^/  ⚠️ 残留: /' || echo "  ✅ 无残留"
