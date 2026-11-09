#!/usr/bin/env bash
# VM：合并 VS Code 设置（保留原来的 cmake 配置）+ 带 BOM 重新编译验证
FB=/home/rsve/Desktop/feibiao
cd "$FB" || exit 1

echo "############ 1) 合并 .vscode/settings.json ############"
mkdir -p .vscode
cat > .vscode/settings.json <<'JSONEOF'
{
  "cmake.sourceDirectory": "/home/rsve/Desktop/feibiao/Vision_Code",
  "files.encoding": "utf8",
  "files.autoGuessEncoding": false,
  "files.eol": "\n"
}
JSONEOF
cat .vscode/settings.json | sed 's/^/  /'

echo
echo "############ 2) 确认源码文件都带 BOM 了 ############"
n=0; nb=0
while IFS= read -r f; do
  n=$((n+1))
  if [ "$(head -c 3 "$f" | od -An -tx1 | tr -d ' \n')" = "efbbbf" ]; then nb=$((nb+1)); fi
done < <(find Vision_Code/src Vision_Code/main.cpp uvc_color_view.cpp -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.inl' -o -name '*.h' \) 2>/dev/null)
echo "  源码文件 $n 个，其中带 BOM 的 $nb 个"

echo
echo "############ 3) 带 BOM 重新编译（验证 BOM 不影响编译）############"
cd "$FB/Vision_Code/build" || exit 1
rm -rf CMakeFiles CMakeCache.txt          # 清掉缓存，强制全部重编，确保每个带 BOM 的文件都被编到
cmake .. > /tmp/cm.log 2>&1
echo "  cmake rc=$?"
make -j$(nproc) > /tmp/mk.log 2>&1
echo "  make rc=$?  error=$(grep -c 'error:' /tmp/mk.log)  warning=$(grep -c 'warning:' /tmp/mk.log)"
grep -n 'error:' /tmp/mk.log | head -20
ls -l Dart 2>&1 | sed 's/^/  /'

echo
echo "############ 4) 编译 uvc_color_view（图像文字改成英文了）############"
cd "$FB" || exit 1
g++ -std=c++17 -O2 -Wall uvc_color_view.cpp \
    Vision_Code/src/detect/src/color/ColorDetector.cpp \
    -I Vision_Code/src/detect/inc \
    -o uvc_color_view $(pkg-config --cflags --libs opencv4) 2>/tmp/uv.log
echo "  g++ rc=$?  error=$(grep -c 'error:' /tmp/uv.log)  warning=$(grep -c 'warning:' /tmp/uv.log)"
grep -n 'error:\|warning:' /tmp/uv.log | head -10
ls -l uvc_color_view 2>&1 | sed 's/^/  /'

echo
echo "############ 5) 确认图像上的文字全是 ASCII ############"
echo -n "  putText 里还有中文吗: "
grep -n 'putText' uvc_color_view.cpp | grep -P '[\x{4e00}-\x{9fff}]' >/dev/null 2>&1 && echo "有（要改）" || echo "没有 ✅"
