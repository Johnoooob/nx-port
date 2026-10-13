#!/bin/bash
# 准备工作区：核工具、建 Python 环境、编着色器翻译工具、登记翻译链版本。做完后用 dm/tools/bringup.sh 在 Mac 上跑，用 dm/ios_bringup/build.sh 出 iPhone 包。
# 用法：./setup.sh          （先把游戏的 ExeFS、RomFS 两个目录放进 $DM_ROOT/data/switch/hono/Update；DM_ROOT 缺省 ~/dm-work）
# 可以反复跑：做过的步骤会跳过。更新到新提交后再跑一次（翻译链版本会跟着源码变）。
# Prepare the workspace: check tools, create the Python environment, build the shader translation tool, register the translation chain version. Afterwards run on the Mac with dm/tools/bringup.sh and build the iPhone package with dm/ios_bringup/build.sh.
# Usage: ./setup.sh          (first put the game's ExeFS and RomFS directories into $DM_ROOT/data/switch/hono/Update; DM_ROOT defaults to ~/dm-work)
# Safe to rerun: finished steps are skipped. Run it again after updating to a new commit (the translation chain version follows the sources).
set -u
HERE=$(cd "$(dirname "$0")" && pwd); R=${DM_ROOT:-$HOME/dm-work}; G=$R/data/switch/hono/Update
export DEVELOPER_DIR=${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer} DOTNET_ROOT=${DOTNET_ROOT:-/opt/homebrew/opt/dotnet/libexec} DOTNET_CLI_TELEMETRY_OPTOUT=1
say(){ echo "[$(date +%T)] $*"; }; bad=0; miss(){ echo "  ✗ $1"; echo "      → $2"; bad=1; }; ok(){ echo "  ✓ $1"; }
say "1. 工具"
[ "$(uname -m)" = arm64 ] && ok "Apple silicon Mac" || miss "需要 Apple silicon Mac" "换一台 M 系列的 Mac"
xcodebuild -version >/dev/null 2>&1 && ok "$(xcodebuild -version | head -1)" || miss "没有完整的 Xcode" "从 App Store 安装 Xcode"
echo 'void f(){}' > "${TMPDIR:-/tmp}/nxp_t.metal"; xcrun metal -c "${TMPDIR:-/tmp}/nxp_t.metal" -o "${TMPDIR:-/tmp}/nxp_t.air" >/dev/null 2>&1 && ok "Metal 着色器编译器" || miss "Xcode 没有 Metal 着色器编译器" "xcodebuild -downloadComponent MetalToolchain"
for t in xcodegen:xcodegen spirv-cross:spirv-cross dotnet:dotnet python3:python; do c=${t%%:*}; command -v "$c" >/dev/null && ok "$c" || miss "缺 ${c}" "brew install ${t#*:}"; done
say "2. 游戏文件（${G}）"
for f in ExeFS/main ExeFS/sdk RomFS/NmplDLL.nro RomFS/lz4.nro; do [ -f "$G/$f" ] && ok "$f" || miss "缺 ${f}" "把游戏的 ExeFS、RomFS 目录放进 ${G}"; done
[ "$bad" = 0 ] || { echo; echo "还有缺的，补齐后再跑 ./setup.sh"; exit 1; }
say "3. Python 环境"
V=$R/build/cache/dm/venv
[ -x "$V/bin/python3" ] || python3 -m venv "$V" || { echo "✗ 建不了 Python 环境"; exit 1; }
"$V/bin/python3" -c "import lz4.block, capstone" 2>/dev/null || "$V/bin/python3" -m pip -q install lz4 capstone || { echo "✗ 装不了 lz4 / capstone"; exit 1; }
say "4. 着色器翻译工具"
X=$R/build/gfx/dmsh_xlate
"$V/bin/python3" "$HERE/dm/gfx/tools/dmsh_chain.py" --check-tools >/dev/null 2>&1 && [ -x "$X/dmsh_xlate" ] || dotnet publish "$HERE/dm/gfx/tools/dmsh_xlate" -c Release -o "$X" -v quiet > "$R/build/cache/dm/xlate_build.log" 2>&1 || { echo "✗ 翻译工具没编过：$(grep -m1 error "$R/build/cache/dm/xlate_build.log")"; exit 1; }
"$V/bin/python3" "$HERE/dm/gfx/tools/dmsh_chain.py" --check-tools >/dev/null 2>&1 || { "$V/bin/python3" "$HERE/dm/gfx/tools/dmsh_chain.py" --update-lock >/dev/null || { echo "✗ 翻译链登记没成功"; exit 1; }; }
"$V/bin/python3" "$HERE/dm/gfx/tools/dmsh_chain.py" --header >/dev/null || { echo "✗ 翻译链版本头没写成"; exit 1; }
say "工作区就绪：${R}"
cat <<MSG
  下一步（Mac）：
      BRINGUP_SHADERS=1 dm/tools/bringup.sh        第一次：跑一遍并把游戏用到的着色器导出、翻译（几分钟）
      DM_WINDOW=1 dm/tools/bringup.sh 600          开窗口玩（加 BRINGUP_AUDIO=1 出声）
  下一步（iPhone）：
      cp dm/ios_bringup/local.env.example dm/ios_bringup/local.env   填设备号、团队号、包名
      dm/ios_bringup/build.sh build && dm/ios_bringup/build.sh install && dm/ios_bringup/build.sh push
MSG
