#!/bin/bash
# 另一个游戏的 iOS 试启动包：生成 Gen/（内嵌模块、源文件链接、iOS 着色器库）→ xcodegen → xcodebuild → 装机 → 推游戏资源。
# 用法：build.sh build | install | push | log       （设备与签名取自 dm/ios_bringup/local.env：DM_DEVICE / DM_IOS_UDID / DM_TEAM_ID；包名 = BU_BUNDLE_ID）
#   build   出包（免费账号：手机要连着并解锁）
#   install 装到手机
#   push    把 RomFS 推进 App 容器的 Library/Application Support/romfs（约 4 GB，装好 App 之后做一次）
#   log     把手机上的 Documents/log.txt 拉回 $DM_ROOT/run/hono/ios_log.txt 并摘出要点
# iOS bring-up package for another game: generate Gen/ (embedded modules, source links, iOS shader libraries) -> xcodegen -> xcodebuild -> install -> push game assets.
# Usage: build.sh build | install | push | log       (device and signing come from dm/ios_bringup/local.env: DM_DEVICE / DM_IOS_UDID / DM_TEAM_ID; bundle ID = BU_BUNDLE_ID)
#   build   build the package (free account: the phone must be connected and unlocked)
#   install install onto the phone
#   push    push RomFS into the App container's Library/Application Support/romfs (about 4 GB, once after installing the App)
#   log     pull Documents/log.txt from the phone to $DM_ROOT/run/hono/ios_log.txt and extract the key lines
set -u
cd "$(dirname "$0")"
R=${DM_ROOT:-$HOME/dm-work}; G=$R/data/switch/hono/Update; O=$R/build/cache/dm/out/hono-r0; SH=$R/build/cache/dm/hono_shaders; RUN=$R/run/hono; PY=$R/build/cache/dm/venv/bin/python3
export DEVELOPER_DIR=${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer} DOTNET_ROOT=${DOTNET_ROOT:-/opt/homebrew/opt/dotnet/libexec}
set -a; [ -f local.env ] && . ./local.env; set +a
: "${DM_TEAM_ID:?先在 dm/ios_bringup/local.env 里填 DM_TEAM_ID}"
: "${BU_BUNDLE_ID:?先在 dm/ios_bringup/local.env 里填 BU_BUNDLE_ID}"; export BU_BUNDLE_ID
DD=$R/run/ios_bringup/dd; APP=$DD/Build/Products/Release-iphoneos/BringupIOS.app; mkdir -p "$R/run/ios_bringup" "$RUN"
t(){ echo "[$(date +%T)] $*"; }
case "${1:-}" in
build)
  [ -f "$O/game.patched.bin" ] && [ -f "$O/NmplDLL.patched.bin" ] || { t "🔴 没有改写产物 ${O}：先跑一次 dm/tools/bringup.sh"; exit 1; }
  rm -rf Gen; mkdir -p Gen/dm Gen/msl
  ( source "$R/build/cache/dm/venv/bin/activate" && python3 tools/gen_build.py ../host Gen > Gen/dm.xcconfig ) || { t "🔴 gen_build 失败"; exit 1; }
  F=$("$PY" tools/gen_embed.py "$O" Gen/embed.S game sdk NmplDLL lz4) || { t "🔴 gen_embed 失败"; exit 1; }
  echo "OTHER_LDFLAGS = \$(inherited) \$(DM_FRAMEWORKS) -framework AVFoundation $F" >> Gen/dm.xcconfig
  for m in game sdk NmplDLL lz4; do cp "$O/$m.fixups.tsv" "$O/$m.gates.tsv" Gen/dm/; done
  t "出 iOS 版着色器库（已出过的走缓存）"
  IOSOUT=$SH/ioslib J=${J:-10} "$PY" ../gfx/tools/dmsh_dump_build.py "$SH/rec" "$SH" > "$SH/build_ios.log" 2>&1 || { t "🔴 着色器出包失败：$(tail -3 "$SH/build_ios.log")"; exit 1; }
  grep -E "^着色器 [0-9]+：|_gq" "$SH/build_ios.log"
  V=$(sed -n 's/^#define DMSH_CHAIN "\(.*\)"/\1/p' ../gfx/nvn/dmsh_chain.h)
  [ -d "$SH/ioslib/b-$V" ] || { t "🔴 没有 iOS 着色器库 $SH/ioslib/b-$V"; exit 1; }
  cp -cR "$SH/ioslib/b-$V" Gen/msl/; cp "$SH/map/shader_map.tsv" Gen/dm/
  t "着色器库 $(ls Gen/msl/b-$V | grep -c metallib) 个"
  xcodegen generate -q >/dev/null || { t "🔴 xcodegen 失败"; exit 1; }
  DEST='generic/platform=iOS'; [ -n "${DM_IOS_UDID:-}" ] && DEST="id=$DM_IOS_UDID"
  xcodebuild -project BringupIOS.xcodeproj -scheme BringupIOS -configuration Release -destination "$DEST" -derivedDataPath "$DD" -allowProvisioningUpdates build > "$R/run/ios_bringup/build.log" 2>&1 \
    || { t "🔴 构建失败（$R/run/ios_bringup/build.log）"; grep -E "error:|Undefined symbols|ld: " "$R/run/ios_bringup/build.log" | head -20; grep -A40 "Undefined symbols" "$R/run/ios_bringup/build.log" | grep '^  "' | head -40; exit 1; }
  t "构建完成：${APP}（$(du -sh "$APP" | cut -f1)）";;
install)
  : "${DM_DEVICE:?先在 dm/ios_bringup/local.env 里填 DM_DEVICE}"
  xcrun devicectl device install app --device "$DM_DEVICE" "$APP" > "$R/run/ios_bringup/install.log" 2>&1 || { t "🔴 装机失败"; tail -5 "$R/run/ios_bringup/install.log"; exit 1; }
  t "装机完成（包名 ${BU_BUNDLE_ID}）";;
push)
  : "${DM_DEVICE:?先在 dm/ios_bringup/local.env 里填 DM_DEVICE}"
  t "推 RomFS（$(du -sh "$G/RomFS" | cut -f1)），要几分钟"
  xcrun devicectl device copy to --device "$DM_DEVICE" --domain-type appDataContainer --domain-identifier "$BU_BUNDLE_ID" --source "$G/RomFS" --destination 'Library/Application Support/romfs' > "$R/run/ios_bringup/push.log" 2>&1 \
    || { t "🔴 推送失败"; tail -5 "$R/run/ios_bringup/push.log"; exit 1; }
  t "推送完成";;
log)
  : "${DM_DEVICE:?先在 dm/ios_bringup/local.env 里填 DM_DEVICE}"
  xcrun devicectl device copy from --device "$DM_DEVICE" --domain-type appDataContainer --domain-identifier "$BU_BUNDLE_ID" --source Documents/log.txt --destination "$RUN/ios_log.txt" > /dev/null 2>&1 || { t "🔴 拉不到日志（App 还没跑过，或手机没连）"; exit 1; }
  L=$RUN/ios_log.txt; echo "日志 ${L}（$(wc -l < "$L" | tr -d ' ') 行）"
  grep -v "强制接管条目无任何引用" "$L" | grep -E ">>> nnMain|nn::ro::LoadModule →|audio：|Metal 设备|WindowInitialize|UNIMPL|Break#|FATAL|CRASH|^\[dm\] bt:|【黄】" | awk '!seen[$0]++' | cut -c1-200 | head -30
  echo "出帧：$(grep -E "^\[dm\] \[nvn\] 帧 [0-9]+：" "$L" | tail -1 | sed -E 's/^\[dm\] \[nvn\] (帧 [0-9]+)：.*/已到\1/')　$(grep -E "计时 至帧" "$L" | tail -1 | sed -E 's/.*(帧时间 p50 [0-9.]+).*/\1 ms/')"
  grep -E "内存 至帧" "$L" | tail -1 | cut -c1-160
  grep -E "【红】" "$L" | grep -v "NVN 未实现被调用\|取址未知名字" | sed -E 's/0x[0-9a-f]+/0x…/g' | sort | uniq -c | sort -rn | cut -c1-200 | head -12;;
*) echo "用法：build.sh build | install | push | log"; exit 2;;
esac
