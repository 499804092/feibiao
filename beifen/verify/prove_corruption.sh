#!/usr/bin/env bash
# 反证实验：坏 bin 是否必然导致 ScatterND 崩溃？做完立即恢复好文件。
D=/home/rsve/Desktop/feibiao/Vision_Code/config/openvino
GOOD=$D/Katrin.bin
BAD=$D/Katrin.bin.BAD_20260921
SAFE=/tmp/Katrin_good_backup.bin
FRAME=/home/rsve/Desktop/feibiao/verify/uvc_frame.png

echo "好文件 MD5: $(md5sum $GOOD | cut -d' ' -f1)"
echo "坏文件 MD5: $(md5sum $BAD  | cut -d' ' -f1)"

cp -f $GOOD $SAFE          # 先把好文件存到安全处
restore() {
  cp -f $SAFE $GOOD
  echo
  echo ">>> 已恢复好文件，当前 MD5: $(md5sum $GOOD | cut -d' ' -f1)"
}
trap restore EXIT

echo
echo "=========== 实验：换回坏文件后推理 ==========="
cp -f $BAD $GOOD
echo "当前 MD5: $(md5sum $GOOD | cut -d' ' -f1)"
cd /home/rsve/Desktop/feibiao/verify || exit 1
timeout 120 ./class_probe --img "$FRAME" 2>&1 | tail -10
echo "退出码 = ${PIPESTATUS[0]}"
