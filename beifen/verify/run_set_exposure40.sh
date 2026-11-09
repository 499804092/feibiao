#!/usr/bin/env bash
# 编译并运行曝光下发工具：验证相机接受「手动 + 40」
cd /home/rsve/Desktop/feibiao/verify || exit 1

echo "############ 编译 ############"
if g++ -std=c++17 -O2 -Wall set_exposure.cpp -o set_exposure 2>/tmp/se.log; then
  echo "  ✓ 编译成功（只需内核头文件，不需要 OpenCV）"
else
  echo "  ✗ 编译失败："
  grep -n 'error:' /tmp/se.log | head -20
  exit 1
fi
echo "  warning 数: $(grep -c 'warning:' /tmp/se.log)"
grep -n 'warning:' /tmp/se.log | head -10

echo
echo "############ 运行：下发「手动 + 40」（不取流，安全）############"
./set_exposure 40
echo "退出码 = $?"
