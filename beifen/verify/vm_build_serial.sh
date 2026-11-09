#!/usr/bin/env bash
# VM：编译验证串口报错改进
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
S="$FB/Vision_Code/src/sensors/src/serialport/SerialPort.cpp"
H="$FB/Vision_Code/src/sensors/inc/serialport/SerialPort.h"
echo "  SerialPort.h  里有 isOpen(): $(grep -c 'isOpen' $H)"
echo "  SerialPort.h  fd_ 有初值:   $(grep -c 'int fd_ = -1' $H)"
echo "  SerialPort.cpp 有明确报错:  $(grep -c '打不开串口' $S)"
echo "  SerialPort.cpp 只提示一次:  $(grep -c 'warned_not_open_' $S)"
echo "  SerialPort.cpp send 有保护: $(grep -c 'if (this->fd_ < 0)' $S)"

echo
echo "===== 确认 BOM 还在（VS Code 中文显示）====="
for f in "$S" "$H"; do
  printf '  %-20s BOM=%s\n' "$(basename $f)" "$([ "$(head -c 3 "$f" | od -An -tx1 | tr -d ' \n')" = "efbbbf" ] && echo yes || echo NO)"
done
