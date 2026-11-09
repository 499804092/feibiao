#!/usr/bin/env bash
# 只读：查清 feibiao 里已有的 .git 是什么来头
FB=/home/rsve/Desktop/feibiao
cd "$FB" || exit 1

echo "############ 1) .git 目录信息 ############"
ls -la .git 2>/dev/null | head -12 | sed 's/^/  /'
echo "  .git 大小: $(du -sh .git 2>/dev/null | cut -f1)"

echo
echo "############ 2) 现有 remote（关键：指向谁）############"
git remote -v 2>/dev/null | sed 's/^/  /' || echo "  （无 remote）"
[ -z "$(git remote)" ] && echo "  ⇒ 没有配置任何远程仓库"

echo
echo "############ 3) 现有提交历史（前 8 条）############"
git log --oneline -8 2>/dev/null | sed 's/^/  /' || echo "  （没有任何提交）"
echo "  提交总数: $(git rev-list --count HEAD 2>/dev/null || echo 0)"

echo
echo "############ 4) 当前分支 ############"
git branch -a 2>/dev/null | sed 's/^/  /' || echo "  （无分支）"

echo
echo "############ 5) 工作区状态（摘要）############"
echo "  未跟踪/已修改 文件数: $(git status --porcelain 2>/dev/null | wc -l)"
git status --porcelain 2>/dev/null | head -15 | sed 's/^/    /'

echo
echo "############ 6) GitHub 上这个仓库存在吗 ############"
code=$(timeout 15 curl -s -o /dev/null -w '%{http_code}' https://github.com/499804092/feibiao 2>/dev/null)
echo "  https://github.com/499804092/feibiao  → HTTP $code"
case "$code" in
  200) echo "  ⇒ 仓库存在且公开" ;;
  404) echo "  ⇒ 仓库不存在，或者是【私有仓库】（未登录时也返回 404）" ;;
  000) echo "  ⇒ 请求失败（网络问题）" ;;
  *)   echo "  ⇒ 未知状态 $code" ;;
esac

echo
echo "############ 7) SSH 到 GitHub 的认证状态 ############"
timeout 15 ssh -o StrictHostKeyChecking=accept-new -T git@github.com 2>&1 | head -3 | sed 's/^/  /'
echo "  （出现 successfully authenticated 才说明公钥已加到 GitHub）"
