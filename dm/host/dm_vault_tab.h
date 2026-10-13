// 占位表（不含任何游戏数据），理由同 dm_hud_exp.h | Placeholder table (contains no game data), for the same reason as dm_hud_exp.h
#define VT_NPART 1
static const char *const vt_part_name[VT_NPART] = { "" };
typedef struct { uint8_t kind, part, tier; uint16_t row, lvl; int16_t set; const char *key, *icon; } vt_row;
static const vt_row vt_rows[] = { {0, 0, 0, 0, 0, -1, "", ""} };
#define VT_NROW (int)(sizeof vt_rows / sizeof vt_rows[0])
static const char *const vt_set_key[] = { "" };
#define VT_NSET (int)(sizeof vt_set_key / sizeof vt_set_key[0])
