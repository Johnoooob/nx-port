# nx-port

[English](README.md) | 简体中文

把 Switch 游戏的原版可执行文件直接跑在 Apple 芯片的 Mac 和 iPhone 上，不模拟 CPU。
ARM64 代码事先改写好，可以原样执行；游戏本来要向主机系统发的那些调用（文件、输入、音频、图形），由一个宿主程序来应答。

仓库里没有任何游戏数据，游戏要用你自己的那一份。

## 来历

这是 [jhaizhou-ops/d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)（MIT）的一个分叉，原项目是围绕一款游戏做的。
我想看看同样的办法换一个游戏能走多远，于是把和原游戏绑定的东西去掉了大半，再补上第二个游戏需要的部分：

- `.nro` 动态模块：离线改写、启动时预先装好，`nn::ro` 由宿主接管（RELR 重定位和 TLS 描述符也处理了）
- 着色器在游戏运行时记录、事后翻译，不需要预先做好的着色器表
- 真正做混音的 AudioRenderer（PCM16 和 DSP-ADPCM）
- 新版 NVN 的对象尺寸、分块贴图布局、CPU 直接写入的线性贴图、共用存储的贴图
- 透明度测试，以及把“点扩成四边形”的几何着色器改成顶点着色器变体在 Metal 上实现
- 一个很小的 iOS App：全屏画面加屏幕虚拟手柄

目前只用一个 NVN 游戏跑通过。换别的游戏多半会碰到还没处理的调用。

## 目录

| 路径 | 内容 |
| --- | --- |
| `dm/tools/patch_nso.py` | 离线改写器（输入 NSO / NRO，输出可装载的映像） |
| `dm/tools/bringup.sh` | 改写、编宿主、翻译着色器，然后在 Mac 上运行 |
| `dm/host/` | 装载器、按名字替换的系统调用、Mac 窗口前端 |
| `dm/gfx/nvn/` | 把 NVN 调用记成指令流，再用 Metal 重放 |
| `dm/gfx/tools/` | 着色器流程（Ryujinx 翻译器 → SPIR-V → MSL → metallib） |
| `dm/ios_bringup/` | iPhone App 和它的构建脚本 |

## 运行

需要一台 Apple 芯片的 Mac、完整的 Xcode、用 Homebrew 装的 `dotnet`、`spirv-cross`、`xcodegen`，以及大约 20 GB 磁盘。
游戏文件、缓存和日志都放在 `~/dm-work`，想换位置就设 `DM_ROOT`。

游戏的两个目录要自己放好（怎么得到它们不在本仓库的范围内）：

- `$DM_ROOT/data/switch/hono/Update/ExeFS`
- `$DM_ROOT/data/switch/hono/Update/RomFS`

然后：

```bash
./setup.sh
```

```bash
dm/tools/bringup.sh
```

`bringup.sh [秒数]` 不开窗口，跑到指定时间（默认 30 秒）就退出。要开窗口玩：

```bash
DM_WINDOW=1 dm/tools/bringup.sh
```

第一次运行只是把游戏用到的着色器记下来，下次启动前才翻译，所以头一两次画面不对是正常的。
默认不出声，要声音就设 `BRINGUP_AUDIO=1`。日志在 `$DM_ROOT/run/hono/latest.log`。

键盘：WASD 是左摇杆，方向键是十字键，J/K/U/I 对应 A/B/X/Y，Q/E 对应 L/R，回车是 +。

iPhone 上：把 `dm/ios_bringup/local.env.example` 复制成 `local.env` 并填好，然后运行：

```bash
dm/ios_bringup/build.sh install
```

`build.sh` 有四个子命令：`build`、`install`、`push`（把游戏资源拷到手机上）和 `log`（把运行日志取回来）。
用免费开发者账号的话，App 每七天要重装一次。

## 调试

| 变量 | 作用 |
| --- | --- |
| `DM_KEYS="A@34,LEFT@54"` | 在第几秒按哪个键，方便无人值守走到某个界面 |
| `DM_NVN_SHOT=800,1900:shot%d.png` | 在这些帧存截图 |
| `DM_NVN_TEXDUMP`、`DM_NVN_PASSDUMP=a-b` | 导出贴图，或某一段绘制过程的输出 |
| `DM_AUDREN_DUMP=文件` | 把混好的音频写进文件 |

为第二个游戏加的图形行为都放在开关后面：`DM_NVN_ALL_WRITABLE`、`DM_NVN_ALIAS`、`DM_NVN_LINEAR_SYNC`、
`DM_NVN_ASTC_VOID`、`DM_NVN_TILE_W`、`DM_NVN_ALPHA_TEST`、`DM_NVN_GS_QUAD`。`bringup.sh` 和 iOS App 会把它们全部打开。

## 许可

MIT，见 [LICENSE](LICENSE)。原项目的版权归原作者所有，仓库里带的第三方代码（Ryujinx 等）各自沿用它们的许可。
与任天堂及任何游戏厂商无关。只用于你自己拥有的游戏。
