#!/usr/bin/env bash
# VM：编译验证「无飞控测试模式」
FB=/home/rsve/Desktop/feibiao
cd "$FB/Vision_Code/build" || exit 1

echo "===== 编译 ====="
make -j$(nproc) > /tmp/mk.log 2>&1
echo "make rc=$?   error=$(grep -c 'error:' /tmp/mk.log)   warning=$(grep -c 'warning:' /tmp/mk.log)"
grep -n 'error:' /tmp/mk.log | head -20
grep -n 'warning:' /tmp/mk.log | head -10
ls -l Dart 2>&1 | sed 's/^/  /'

echo
echo "===== 确认改动到位 ====="
echo -n "  SerialPort.h  isOptional():     "; grep -c 'isOptional' "$FB/Vision_Code/src/sensors/inc/serialport/SerialPort.h"
echo -n "  SerialPort.cpp 读 optional 配置: "; grep -c 'optional' "$FB/Vision_Code/src/sensors/src/serialport/SerialPort.cpp"
echo -n "  UvcCamera.cpp 测试模式分支:      "; grep -c 'sp_.isOpen()' "$FB/Vision_Code/src/sensors/src/uvc/UvcCamera.cpp"
echo -n "  main.cpp 退出判断:               "; grep -c 'isOptional' "$FB/Vision_Code/main.cpp"
echo -n "  sensors.json optional 值:        "; grep -A1 '"optional"' "$FB/Vision_Code/config/json/sensors.json" | tail -1 | tr -d ' '

echo
echo "===== BOM 检查 ====="
for f in Vision_Code/main.cpp Vision_Code/src/sensors/src/serialport/SerialPort.cpp Vision_Code/src/sensors/inc/serialport/SerialPort.h Vision_Code/src/sensors/src/uvc/UvcCamera.cpp; do
  printf '  %-46s BOM=%s\n' "$(basename $f)" "$([ "$(head -c 3 "$FB/$f" | od -An -tx1 | tr -d ' \n')" = "efbbbf" ] && echo yes || echo NO)"
done
