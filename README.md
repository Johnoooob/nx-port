# nx-port

English | [简体中文](README.zh-CN.md)

Runs a Switch game's original executable on Apple-silicon Macs and iPhones without emulating the CPU.
The ARM64 code is patched ahead of time so it can run as-is, and a host program answers the system calls the game
would normally make to the console: files, input, audio and graphics.

There is no game data in this repo. You need your own copy of the game.

## Where this comes from

This is a fork of [jhaizhou-ops/d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile) (MIT), which was built
around one game. I wanted to see how far the same approach goes with a different one, so I removed most of what was
tied to the original game and added what the second game needed:

- `.nro` dynamic modules: patched offline, loaded up front, with `nn::ro` handled by the host (RELR relocations and
  TLS descriptors included)
- shaders are recorded while the game runs and translated afterwards, so no prebuilt shader table is needed
- an AudioRenderer that actually mixes (PCM16 and DSP-ADPCM)
- object sizes of the newer NVN, tiled texture layout, linear textures written by the CPU, textures sharing storage
- alpha test, and the point-to-quad geometry shader done as a vertex shader variant on Metal
- a small iOS app: full-screen view plus an on-screen gamepad

So far it has been brought up with one NVN game. Another game will most likely hit calls that aren't handled yet.

## Layout

| Path | What's there |
| --- | --- |
| `dm/tools/patch_nso.py` | offline patcher (NSO / NRO in, loadable image out) |
| `dm/tools/bringup.sh` | patch, build the host, translate shaders and run on the Mac |
| `dm/host/` | loader, system calls replaced by name, Mac window front end |
| `dm/gfx/nvn/` | NVN calls recorded into a command stream and replayed with Metal |
| `dm/gfx/tools/` | shader pipeline (Ryujinx translator → SPIR-V → MSL → metallib) |
| `dm/ios_bringup/` | iPhone app and its build script |

## Running it

You need an Apple-silicon Mac, full Xcode, `dotnet`, `spirv-cross` and `xcodegen` from Homebrew, and about 20 GB of
disk. Game files, caches and logs go to `~/dm-work`; set `DM_ROOT` to move that.

Put the game's two directories in place yourself (this repo doesn't cover how to get them):

- `$DM_ROOT/data/switch/hono/Update/ExeFS`
- `$DM_ROOT/data/switch/hono/Update/RomFS`

Then:

```bash
./setup.sh
```

```bash
dm/tools/bringup.sh
```

`bringup.sh [seconds]` runs without a window and quits after the given time (30 by default). To play in a window:

```bash
DM_WINDOW=1 dm/tools/bringup.sh
```

The first run only records which shaders the game uses. They get translated before the next start, so expect a
broken picture the first time or two. Sound is off unless you set `BRINGUP_AUDIO=1`. The log is at
`$DM_ROOT/run/hono/latest.log`.

Keyboard: WASD is the left stick, arrow keys the D-pad, J/K/U/I are A/B/X/Y, Q/E are L/R, Return is +.

For the iPhone, copy `dm/ios_bringup/local.env.example` to `local.env`, fill it in, and run:

```bash
dm/ios_bringup/build.sh install
```

`build.sh` takes `build`, `install`, `push` (copies the game assets to the phone) and `log` (pulls the run log
back). With a free developer account the app has to be reinstalled every seven days.

## Debugging

| Variable | Effect |
| --- | --- |
| `DM_KEYS="A@34,LEFT@54"` | press a button at a given second, handy for getting to a screen unattended |
| `DM_NVN_SHOT=800,1900:shot%d.png` | save a screenshot at those frames |
| `DM_NVN_TEXDUMP`, `DM_NVN_PASSDUMP=a-b` | dump textures, or the output of a range of passes |
| `DM_AUDREN_DUMP=file` | write the mixed audio to a file |

The graphics behaviour added for the second game sits behind switches: `DM_NVN_ALL_WRITABLE`, `DM_NVN_ALIAS`,
`DM_NVN_LINEAR_SYNC`, `DM_NVN_ASTC_VOID`, `DM_NVN_TILE_W`, `DM_NVN_ALPHA_TEST`, `DM_NVN_GS_QUAD`. `bringup.sh` and
the iOS app turn all of them on.

## License

MIT, see [LICENSE](LICENSE). The original project's copyright stays with its author, and the bundled third-party
code (Ryujinx and others) keeps its own licenses. Not affiliated with Nintendo or any game publisher. Use it only
with games you own.
