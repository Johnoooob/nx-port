#!/bin/bash
# 另一个游戏的试启动（Mac 宿主）：改写四个模块（主程序以 game 为名 + sdk + 两个 nn::ro 动态模块），编宿主，跑一次并摘出日志要点。
# 用法：bringup.sh [最长秒数，缺省 30]    开窗口：DM_WINDOW=1 bringup.sh    重新改写模块：BRINGUP_REPATCH=1 bringup.sh
# 游戏文件：$DM_ROOT/data/switch/hono/Update/{ExeFS/main, ExeFS/sdk, RomFS/…}（RomFS 里要有 NmplDLL.nro 与 lz4.nro）。
# 着色器：BRINGUP_SHADERS=1 bringup.sh（导出 + 离线翻译，做一次即可；游戏进到新场景出现新着色器时再做）。
# 声音：BRINGUP_AUDIO=1 bringup.sh 才真出声（缺省不出声）。
# 完整日志：$DM_ROOT/run/hono/latest.log。主程序不叫 main，宿主据此进入试启动模式（原项目那款游戏的专属逻辑全部跳过，可选接管生效）。
# Bring-up of another game (Mac host): rewrite the four modules (main program under the name game + sdk + the two nn::ro dynamic modules), build the host, run once and extract the key log lines.
# Usage: bringup.sh [max seconds, default 30]    with a window: DM_WINDOW=1 bringup.sh    rewrite the modules again: BRINGUP_REPATCH=1 bringup.sh
# Game files: $DM_ROOT/data/switch/hono/Update/{ExeFS/main, ExeFS/sdk, RomFS/...} (RomFS must contain NmplDLL.nro and lz4.nro).
# Full log: $DM_ROOT/run/hono/latest.log. The main program is not named main, which puts the host into bring-up mode (all logic specific to the original project's game is skipped, optional takeovers are active).
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd); R=${DM_ROOT:-$HOME/dm-work}
G=$R/data/switch/hono/Update; O=$R/build/cache/dm/out/hono-r0; RUN=$R/run/hono; PY=${DM_PY:-$R/build/cache/dm/venv/bin/python3}
export DEVELOPER_DIR=${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}
[ -f "$G/ExeFS/main" ] && [ -f "$G/RomFS/NmplDLL.nro" ] || { echo "🔴 没找到游戏文件：$G"; exit 1; }
mkdir -p "$O" "$RUN/save" "$RUN/tmp"
if [ "${BRINGUP_REPATCH:-0}" = 1 ] || [ ! -f "$O/game.patched.bin" ] || [ "$HERE/patch_nso.py" -nt "$O/game.patched.bin" ]; then
  for spec in game:ExeFS/main sdk:ExeFS/sdk NmplDLL:RomFS/NmplDLL.nro lz4:RomFS/lz4.nro; do n=${spec%%:*}; f=${spec#*:}
    "$PY" "$HERE/patch_nso.py" "$G/$f" "$O" "$n" > "$O/$n.patch.log" 2>&1 || { echo "🔴 改写 $n 失败：$(tail -2 "$O/$n.patch.log")"; exit 1; }
    echo "已改写 $n"; done
fi
( source "$R/build/cache/dm/venv/bin/activate" && make -s -C "$HERE/../host" ) || { echo "🔴 宿主没编过"; exit 1; }
[ -f "$RUN/latest.log" ] && mv "$RUN/latest.log" "$RUN/prev.log"
# 着色器：BRINGUP_SHADERS=1 时先带导出跑一遍（把游戏设过的着色器存下来），再离线翻译成 Metal 库；之后每次运行自动用这批库
# Shaders: with BRINGUP_SHADERS=1, first run once with dumping on (storing every shader the game sets), then translate them offline into Metal libraries; later runs use these libraries automatically
SH=$R/build/cache/dm/hono_shaders
if [ "${BRINGUP_SHADERS:-0}" = 1 ]; then
  mkdir -p "$SH/rec"
  ( cd "$HERE/../host" && DM_SHADER_DUMP="$SH/rec" DM_ROMFS="$G/RomFS" DM_SAVE="$RUN/save" DM_TEMP="$RUN/tmp" DM_WATCH="${1:-30}" ./dmhost "$O" game sdk NmplDLL lz4 > "$RUN/dump.log" 2>&1 )
  echo "已导出着色器记录 $(ls "$SH/rec" | wc -l | tr -d ' ') 个，开始翻译（首次几分钟）"
  DOTNET_ROOT=${DOTNET_ROOT:-/opt/homebrew/opt/dotnet/libexec} J=${J:-10} "$PY" "$HERE/../gfx/tools/dmsh_dump_build.py" "$SH/rec" "$SH" > "$SH/build.log" 2>&1 || { echo "🔴 着色器翻译失败：$(tail -3 "$SH/build.log")"; exit 1; }
  grep -E "^着色器记录|^着色器 [0-9]+：" "$SH/build.log"
fi
# 每次运行都开着导出（很便宜）：玩到新场景时新出现的着色器会存下来；下次运行前发现有没翻译过的记录就自动补翻（已翻译的走缓存，只花新增那几个的时间）
# Dumping stays on in every run (it is cheap): shaders that first appear in a new scene get stored; before the next run, records not yet translated are translated automatically (already translated ones hit the cache, so only the new ones cost time)
mkdir -p "$SH/rec"
if [ "${BRINGUP_SHADERS:-0}" != 1 ] && [ -n "$(ls "$SH/rec" 2>/dev/null)" ]; then
  CV=$(sed -n 's/^#define DMSH_CHAIN "\(.*\)"/\1/p' "$HERE/../gfx/nvn/dmsh_chain.h")
  NEW=0; [ -d "$SH/lib/b-${CV}" ] || NEW=1   # 当前翻译链版本的库还没出过（换了源码或工具）：整批重出 | The libraries for the current translation chain version have not been built yet (sources or tools changed): rebuild the whole batch
  for f in "$SH/rec"/*.rec; do k=$(basename "$f" .rec); grep -q "$k" "$SH/map/shader_map.tsv" 2>/dev/null || grep -q "$k" "$SH/skipped.txt" 2>/dev/null || NEW=$((NEW + 1)); done
  if [ "$NEW" -gt 0 ]; then echo "着色器库需要更新（新记录或翻译链版本变了），翻译中…"
    DOTNET_ROOT=${DOTNET_ROOT:-/opt/homebrew/opt/dotnet/libexec} J=${J:-10} "$PY" "$HERE/../gfx/tools/dmsh_dump_build.py" "$SH/rec" "$SH" > "$SH/build.log" 2>&1 || { echo "🔴 着色器翻译失败：$(tail -3 "$SH/build.log")"; exit 1; }
    grep -E "^着色器记录|^着色器 [0-9]+：" "$SH/build.log"; fi
fi
SHENV=(DM_NVN_ALL_WRITABLE=1 DM_NVN_ALIAS=1 DM_NVN_LINEAR_SYNC=1 DM_NVN_ASTC_VOID=1 DM_NVN_TILE_W=1 DM_NVN_ALPHA_TEST=1 DM_NVN_GS_QUAD=1 DM_SHADER_DUMP="$SH/rec"); [ -f "$SH/map/shader_map.tsv" ] && SHENV=(DM_NVN_ALL_WRITABLE=1 DM_NVN_ALIAS=1 DM_NVN_LINEAR_SYNC=1 DM_NVN_ASTC_VOID=1 DM_NVN_TILE_W=1 DM_NVN_ALPHA_TEST=1 DM_NVN_GS_QUAD=1 DM_SHADER_DUMP="$SH/rec" DM_NVN_SHMAP="$SH/map/shader_map.tsv" DM_MSL_DIR="$SH/lib")
# 声音：缺省不出声（混音照常在算，只是不送扬声器）。要听：BRINGUP_AUDIO=1 bringup.sh；先把系统音量调低再开
# Sound: off by default (the mix is still computed, just not sent to the speakers). To listen: BRINGUP_AUDIO=1 bringup.sh; turn the system volume down first
[ "${BRINGUP_AUDIO:-0}" = 1 ] && SHENV+=(DM_AUDIO=1)
( cd "$HERE/../host" && env "${SHENV[@]}" DM_ROMFS="$G/RomFS" DM_SAVE="$RUN/save" DM_TEMP="$RUN/tmp" DM_WATCH="${1:-30}" ./dmhost "$O" game sdk NmplDLL lz4 > "$RUN/latest.log" 2>&1 ); rc=$?
echo "退出码 ${rc}（142 = 到时间被看门狗结束，属正常；139 / 134 = 崩溃或中止）　完整日志：$RUN/latest.log"
echo "—— 要点 ——"
L="$RUN/latest.log"
grep -v "强制接管条目无任何引用" "$L" | grep -E ">>> nnMain|nn::ro::LoadModule →|audio：|Metal 设备|WindowInitialize|UNIMPL|Break#|FATAL|CRASH|^\[dm\] bt:|【黄】" | grep -v "CRASH sig=14" | awk '!seen[$0]++' | cut -c1-200 | head -30
echo "出帧：$(grep -E "^\[dm\] \[nvn\] 帧 [0-9]+：" "$L" | tail -1 | sed -E 's/^\[dm\] \[nvn\] (帧 [0-9]+)：.*/已到\1/')　$(grep -E "计时 至帧" "$L" | tail -1 | sed -E 's/.*(帧时间 p50 [0-9.]+).*/\1 ms/')"
echo "着色器未映射（不同的控制段）：$(grep -oE "着色器控制段 [0-9a-f]+ 不在映射表里" "$L" | sort -u | wc -l | tr -d ' ') 个"
echo "未实现的图形函数（调用次数为日志里报红的次数，每 256 次调用报一次）："
grep "NVN 未实现被调用" "$L" | sed -E 's/.*调用：([A-Za-z0-9]+).*/\1/' | sort | uniq -c | sort -rn | sed 's/^/  /'
grep -E "【红】" "$L" | grep -v "NVN 未实现被调用\|不在映射表里\|取址未知名字" | sed -E 's/0x[0-9a-f]+/0x…/g; s/[0-9a-f]{16}/…/g' | sort | uniq -c | sort -rn | cut -c1-200 | head -12
