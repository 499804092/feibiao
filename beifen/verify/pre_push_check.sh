#!/usr/bin/env bash
# 只读：上传前安全检查 + 仓库状态复查
FB=/home/rsve/Desktop/feibiao
cd "$FB" || exit 1

echo "############ 1) .vscode 里有什么（可能有敏感信息）############"
if [ -d .vscode ]; then
  find .vscode -type f 2>/dev/null | while read f; do
    echo "  --- $f ($(wc -c < "$f") 字节) ---"
    sed 's/^/      /' "$f" 2>/dev/null | head -20
  done
else
  echo "  （没有 .vscode）"
fi

echo
echo "############ 2) 扫一遍所有脚本里有没有写死的密码/令牌 ############"
echo "  --- 匹配 SUDOPW= / password= / token= 的行 ---"
grep -rInE 'SUDOPW=[^$]|password\s*=\s*[^$]|token\s*=\s*[^$]|ghp_|github_pat_' . 2>/dev/null \
  | grep -v 'Binary file' | head -20 | sed 's/^/    /' || echo "    无"
echo "  --- 有没有 id_rsa / .pem / .key 之类的私钥文件 ---"
find . -type f \( -name 'id_rsa*' -o -name '*.pem' -o -name '*.key' -o -name '*credential*' \) 2>/dev/null | sed 's/^/    /' || true
echo "    （没有输出就是没有）"

echo
echo "############ 3) 复查 GitHub 仓库状态 ############"
echo "  --- git ls-remote（能列出来说明仓库有内容且可访问）---"
timeout 20 git ls-remote https://github.com/499804092/feibiao.git 2>&1 | head -10 | sed 's/^/    /'
echo "  退出码: $?"
echo "  --- 网页状态 ---"
code=$(timeout 15 curl -s -o /dev/null -w '%{http_code}' https://github.com/499804092/feibiao 2>/dev/null)
echo "    HTTP $code （200 = 公开可见）"

echo
echo "############ 4) 上传内容清单（按是否建议上传分类）############"
echo "  --- 建议上传（源码/配置/文档/工具）---"
for d in Vision_Code Electric_Control_Code 说明.md uvc_detect_view.cpp uvc_detect_view; do
  [ -e "$d" ] && echo "    ✅ $d  ($(du -sh "$d" 2>/dev/null | cut -f1))"
done
echo "  --- 建议排除（编译产物，可重新生成）---"
[ -d Vision_Code/build ] && echo "    ❌ Vision_Code/build  ($(du -sh Vision_Code/build 2>/dev/null | cut -f1))"
echo "  --- 建议排除（备份/测试残留，含损坏的 19MB 模型）---"
echo "    ❌ beifen/备份/Katrin.bin.BAD_20260921  ($(du -sh beifen/备份/Katrin.bin.BAD_20260921 2>/dev/null | cut -f1))"
[ -d beifen/测试残留 ] && echo "    ❌ beifen/测试残留  ($(du -sh beifen/测试残留 2>/dev/null | cut -f1))"
echo "  --- 你自己决定（历史记录/诊断工具）---"
echo "    ?  beifen/verify  ($(du -sh beifen/verify 2>/dev/null | cut -f1)) —— 诊断脚本，可能对复习有用"
echo "    ?  beifen/旧文档   ($(du -sh beifen/旧文档 2>/dev/null | cut -f1)) —— 之前的文档"
echo "    ?  Vision_Code/config/openvino/Katrin.bin  ($(du -sh Vision_Code/config/openvino/Katrin.bin 2>/dev/null | cut -f1)) —— 模型文件，识别必需"
