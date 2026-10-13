DM_ROOT ?= $(HOME)/dm-work
CFLAGS=-O1 -g -Wall -Wno-unused-function
# 服务层接管 hle_*.c 按通配纳入：新增文件不改这一行（免合并冲突）
# Service-layer takeover hle_*.c files are included by wildcard: adding a file does not touch this line (avoids merge conflicts)
SRC=main.c loader.c hle.c crash.c diag.c thread.c memstat.m dm_cursor.c dm_cursor_item.c dm_world.c dm_textinput.c dm_update.c dm_actions.c dm_namegen.c dm_cloudsync.c dm_custom.c dm_elysium.c dm_bossrush.c dm_pages.c dm_vault.c dm_vault_ui.c dm_kills.c dm_textbox.c dm_textbox_core.c dm_potpick.c dm_lootfilter.c dm_autobuff.c dm_beltauto.c dm_padmode.c dm_pouch.c dm_drophaptic.c dm_freetp.c dm_townwp.c dm_shoprefresh.c dm_runeword.c dm_uiscreen.c dm_dropfilter.c dm_hud.c dm_rift.c dm_rift_ui.c dm_bright.c touch_mac.c romfs_validate.c dm_touch_controls.c $(sort $(wildcard hle_*.c)) front_mac.m entry.S dm_i18n.c
# NVN 空设备 + nn::vi（dm/gfx/）
# 按通配纳入 dm/gfx 与 dm/gfx/nvn 下的源（tests/ metal/ 不进宿主）：新增文件不改这一行
# NVN null device + nn::vi (dm/gfx/)
# Sources under dm/gfx and dm/gfx/nvn are included by wildcard (tests/ and metal/ do not go into the host): adding a file does not touch this line
GFX=$(sort $(wildcard ../gfx/*.c ../gfx/nvn/*.c ../gfx/nvn/*.m ../gfx/nvn/*.S ../gfx/tex/*.c ../gfx/tex/*.m))
GFX_H=$(sort $(wildcard ../gfx/nvn/*.h ../gfx/nvn/include/*.h ../gfx/tex/*.h))
# 第三方：libdeflate 只编解压相关源（TACT BLTE zlib 块，hle_tact.c）
# Third party: only the decompression-related sources of libdeflate are built (TACT BLTE zlib blocks, hle_tact.c)
LIBDEFLATE=../third_party/libdeflate/lib/deflate_decompress.c ../third_party/libdeflate/lib/zlib_decompress.c ../third_party/libdeflate/lib/adler32.c ../third_party/libdeflate/lib/utils.c ../third_party/libdeflate/lib/arm/cpu_features.c
.DEFAULT_GOAL := dmhost   # 下面先写了表头生成规则，缺省目标要明说（否则 make 只生成表头、不编宿主） | The header generation rules are written first below, so the default goal must be stated explicitly (otherwise make only generates the headers and does not build the host)
dmhost: $(SRC) $(GFX) $(LIBDEFLATE) dm.h hle.h dm_input.h front.h touch_mac.h dm_cursor.h dm_touch_controls.[ch] $(GFX_H)
	clang $(CFLAGS) -I../gfx/nvn/include -fobjc-arc -o $@ $(SRC) $(GFX) $(LIBDEFLATE) -framework Cocoa -framework QuartzCore -framework Metal -framework AudioToolbox -framework ImageIO -framework UniformTypeIdentifiers
