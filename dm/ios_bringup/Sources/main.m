// 另一个游戏的最小 iOS 外壳（试启动用）：一个全屏 Metal 画面 + 一层屏幕虚拟按键，底层是 dm/host 与 dm/gfx 的同一份源码。
// 与 dm/ios（暗黑 II 的外壳）无关：没有触屏界面改造、没有资源下载、没有设置页。手柄键位经 dm_input_set 交给宿主（与 Mac 前端 front_mac.m 相同）。
// 目录：游戏资源在 App 容器的 Library/Application Support/romfs（从 Mac 用 devicectl 推入，见 build.sh），存档在 Documents/save，日志在 Documents/log.txt。
// 模块：改写后的四个模块（game sdk NmplDLL lz4）由 build.sh 经 dm/ios/tools/gen_embed.py 编进可执行文件，装载走 loader.c 的 iOS 分支。
// Minimal iOS shell for another game (bring-up): one full-screen Metal picture + one layer of on-screen virtual buttons, on top of the same dm/host and dm/gfx sources.
// Unrelated to dm/ios (Diablo II's shell): no touch UI rework, no asset download, no settings page. Controller state goes to the host through dm_input_set (same as the Mac front end front_mac.m).
// Directories: game assets in the App container's Library/Application Support/romfs (pushed from the Mac with devicectl, see build.sh), saves in Documents/save, log in Documents/log.txt.
// Modules: the four rewritten modules (game sdk NmplDLL lz4) are compiled into the executable by build.sh through dm/ios/tools/gen_embed.py; loading takes the iOS branch of loader.c.
#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#import <AVFoundation/AVFoundation.h>
#include <pthread.h>
#include "dm.h"
#include "dm_input.h"
#include "front.h"
#include "hle.h"

int dm_main(int argc, char **argv);   // dm/host/main.c，以 -Dmain=dm_main 编入 | dm/host/main.c, compiled in with -Dmain=dm_main
void dm_gfx_suspend(int on);

static CAMetalLayer *g_layer;
void *front_metal_layer(void) { return (__bridge void *)g_layer; }
uint64_t front_game_res(int docked) { (void)docked; return 720ull << 32 | 1280; }
void front_run(void (*guest)(void)) { guest(); }   // 已在大栈线程上（boot_thread） | Already on the large-stack thread (boot_thread)
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    (void)sub; (void)max_chars;
    __block int ok = 0; __block NSString *text = nil; dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_main_queue(), ^{
        UIAlertController *a = [UIAlertController alertControllerWithTitle:title && *title ? @(title) : @"输入" message:guide && *guide ? @(guide) : nil preferredStyle:UIAlertControllerStyleAlert];
        [a addTextFieldWithConfigurationHandler:^(UITextField *f) { f.text = initial ? @(initial) : @""; }];
        [a addAction:[UIAlertAction actionWithTitle:@"取消" style:UIAlertActionStyleCancel handler:^(UIAlertAction *x) { (void)x; dispatch_semaphore_signal(sem); }]];
        [a addAction:[UIAlertAction actionWithTitle:@"确定" style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) { (void)x; ok = 1; text = a.textFields.firstObject.text; dispatch_semaphore_signal(sem); }]];
        UIWindow *w = nil; for (UIScene *s in UIApplication.sharedApplication.connectedScenes) if ([s isKindOfClass:UIWindowScene.class]) w = ((UIWindowScene *)s).keyWindow ?: w;
        if (w.rootViewController) [w.rootViewController presentViewController:a animated:YES completion:nil]; else dispatch_semaphore_signal(sem);
    });
    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
    if (!ok) return 0;
    snprintf(out, (size_t)out_cap, "%s", text.UTF8String ?: "");
    return 1;
}
void front_save_failed(int err) { LOG("【红】存档写入失败 errno=%d", err); }

// ---------- 屏幕虚拟按键 ----------
// 键位（位号与 front_mac.m 的键盘映射一致）：0 A 1 B 2 X 3 Y 4 按左摇杆 5 按右摇杆 6 L 7 R 8 ZL 9 ZR 10 + 11 − 12 ← 13 ↑ 14 → 15 ↓。
// 布局（横屏）：左下左摇杆、其上方十字键；右下 ABXY、其左侧右摇杆；上边两侧 L / ZL 与 R / ZR；上边中间 − 与 +。摇杆是「按下处为中心」的浮动摇杆。
// ---------- On-screen virtual buttons ----------
// Buttons (bit numbers match the keyboard mapping in front_mac.m): 0 A 1 B 2 X 3 Y 4 left stick press 5 right stick press 6 L 7 R 8 ZL 9 ZR 10 + 11 - 12 left 13 up 14 right 15 down.
// Layout (landscape): left stick at bottom left with the d-pad above it; ABXY at bottom right with the right stick to their left; L / ZL and R / ZR at the two top corners; - and + at the top centre. The sticks are floating ("centre where pressed").
typedef struct { int bit; CGFloat cx, cy, r; const char *label; } pad_btn;   // cx / cy：按视图宽高的比例；r：按视图高的比例 | cx / cy: fractions of the view width / height; r: fraction of the view height
static const pad_btn k_btn[] = {
    {0, 0.925, 0.66, 0.085, "A"}, {1, 0.865, 0.80, 0.085, "B"}, {2, 0.865, 0.52, 0.085, "X"}, {3, 0.805, 0.66, 0.085, "Y"},
    {6, 0.075, 0.09, 0.075, "L"}, {8, 0.175, 0.09, 0.075, "ZL"}, {7, 0.925, 0.09, 0.075, "R"}, {9, 0.825, 0.09, 0.075, "ZR"},
    {11, 0.44, 0.07, 0.055, "−"}, {10, 0.56, 0.07, 0.055, "+"},
    {12, 0.055, 0.42, 0.06, "←"}, {14, 0.155, 0.42, 0.06, "→"}, {13, 0.105, 0.31, 0.06, "↑"}, {15, 0.105, 0.53, 0.06, "↓"},
    {4, 0.30, 0.90, 0.05, "LS"}, {5, 0.70, 0.90, 0.05, "RS"},
};
enum { NBTN = sizeof k_btn / sizeof *k_btn };
@interface PadView : UIView { @public UITouch *btnTouch[NBTN]; UITouch *stickTouch[2]; CGPoint stickOrigin[2]; int32_t sx[2], sy[2]; CAShapeLayer *btnLayer[NBTN]; CAShapeLayer *knob[2]; } @end
@implementation PadView
- (instancetype)initWithFrame:(CGRect)f {
    self = [super initWithFrame:f]; self.multipleTouchEnabled = YES; self.backgroundColor = UIColor.clearColor; self.opaque = NO;
    for (int i = 0; i < NBTN; i++) { CAShapeLayer *l = [CAShapeLayer layer]; l.fillColor = [UIColor colorWithWhite:1 alpha:0.10].CGColor; l.strokeColor = [UIColor colorWithWhite:1 alpha:0.35].CGColor; l.lineWidth = 1.5; [self.layer addSublayer:l]; btnLayer[i] = l;
        CATextLayer *t = [CATextLayer layer]; t.string = @(k_btn[i].label); t.alignmentMode = kCAAlignmentCenter; t.foregroundColor = [UIColor colorWithWhite:1 alpha:0.6].CGColor; t.contentsScale = UIScreen.mainScreen.scale; t.name = @"t"; [l addSublayer:t]; }
    for (int s = 0; s < 2; s++) { CAShapeLayer *k = [CAShapeLayer layer]; k.fillColor = [UIColor colorWithWhite:1 alpha:0.25].CGColor; k.strokeColor = [UIColor colorWithWhite:1 alpha:0.5].CGColor; k.lineWidth = 1.5; k.hidden = YES; [self.layer addSublayer:k]; knob[s] = k; }
    return self;
}
- (void)layoutSubviews {
    [super layoutSubviews]; CGFloat W = self.bounds.size.width, H = self.bounds.size.height;
    [CATransaction begin]; [CATransaction setDisableActions:YES];
    for (int i = 0; i < NBTN; i++) { CGFloat r = k_btn[i].r * H; CGRect b = CGRectMake(k_btn[i].cx * W - r, k_btn[i].cy * H - r, 2 * r, 2 * r); btnLayer[i].frame = b; btnLayer[i].path = [UIBezierPath bezierPathWithOvalInRect:CGRectMake(0, 0, 2 * r, 2 * r)].CGPath;
        for (CALayer *t in btnLayer[i].sublayers) if ([t.name isEqualToString:@"t"]) { ((CATextLayer *)t).fontSize = r * 0.8; t.frame = CGRectMake(0, r * 0.45, 2 * r, r * 1.1); } }
    [CATransaction commit];
}
- (int)buttonAt:(CGPoint)p { CGFloat W = self.bounds.size.width, H = self.bounds.size.height; for (int i = 0; i < NBTN; i++) { CGFloat dx = p.x - k_btn[i].cx * W, dy = p.y - k_btn[i].cy * H, r = k_btn[i].r * H * 1.15; if (dx * dx + dy * dy <= r * r) return i; } return -1; }
- (int)stickZoneAt:(CGPoint)p { CGFloat W = self.bounds.size.width, H = self.bounds.size.height; if (p.y < H * 0.55) return -1; if (p.x < W * 0.45) return 0; if (p.x > W * 0.52 && p.x < W * 0.76) return 1; return -1; }
- (void)push {
    uint64_t b = 0; for (int i = 0; i < NBTN; i++) if (btnTouch[i]) b |= 1ull << k_btn[i].bit;
    dm_input_set(b, sx[0], sy[0], sx[1], sy[1]);
    [CATransaction begin]; [CATransaction setDisableActions:YES];
    for (int i = 0; i < NBTN; i++) btnLayer[i].fillColor = [UIColor colorWithWhite:1 alpha:btnTouch[i] ? 0.40 : 0.10].CGColor;
    [CATransaction commit];
}
- (void)moveStick:(int)s to:(CGPoint)p {
    CGFloat R = self.bounds.size.height * 0.16, dx = p.x - stickOrigin[s].x, dy = p.y - stickOrigin[s].y, d = sqrt(dx * dx + dy * dy);
    if (d > R) { dx *= R / d; dy *= R / d; }
    sx[s] = (int32_t)(dx / R * 32767); sy[s] = (int32_t)(-dy / R * 32767);   // 屏幕 y 向下，摇杆 y 向上为正 | Screen y points down, stick y is positive upward
    CGFloat kr = R * 0.45; [CATransaction begin]; [CATransaction setDisableActions:YES];
    knob[s].hidden = NO; knob[s].path = [UIBezierPath bezierPathWithOvalInRect:CGRectMake(stickOrigin[s].x + dx - kr, stickOrigin[s].y + dy - kr, 2 * kr, 2 * kr)].CGPath;
    [CATransaction commit];
}
- (void)touchesBegan:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e {
    (void)e;
    for (UITouch *t in ts) { CGPoint p = [t locationInView:self]; int i = [self buttonAt:p];
        if (i >= 0) { btnTouch[i] = t; continue; }
        int s = [self stickZoneAt:p]; if (s >= 0 && !stickTouch[s]) { stickTouch[s] = t; stickOrigin[s] = p; [self moveStick:s to:p]; } }
    [self push];
}
- (void)touchesMoved:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e {
    (void)e;
    for (UITouch *t in ts) { for (int s = 0; s < 2; s++) if (stickTouch[s] == t) [self moveStick:s to:[t locationInView:self]];
        for (int i = 0; i < NBTN; i++) if (btnTouch[i] == t && [self buttonAt:[t locationInView:self]] != i) {   // 手指滑到相邻键：换键（十字键 / ABXY 上滑动用） | Finger slides onto a neighbouring button: switch buttons (for sliding across the d-pad / ABXY)
            int j = [self buttonAt:[t locationInView:self]]; btnTouch[i] = nil; if (j >= 0 && !btnTouch[j]) btnTouch[j] = t; } }
    [self push];
}
- (void)endTouches:(NSSet<UITouch *> *)ts {
    for (UITouch *t in ts) { for (int i = 0; i < NBTN; i++) if (btnTouch[i] == t) btnTouch[i] = nil;
        for (int s = 0; s < 2; s++) if (stickTouch[s] == t) { stickTouch[s] = nil; sx[s] = sy[s] = 0; knob[s].hidden = YES; } }
    [self push];
}
- (void)touchesEnded:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e { (void)e; [self endTouches:ts]; }
- (void)touchesCancelled:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e { (void)e; [self endTouches:ts]; }
@end

// ---------- 画面 ----------
// ---------- Picture ----------
@interface GameView : UIView @end
@implementation GameView
+ (Class)layerClass { return CAMetalLayer.class; }
- (void)layoutSubviews { [super layoutSubviews]; CAMetalLayer *l = (CAMetalLayer *)self.layer; CGFloat s = self.window.screen.scale ?: UIScreen.mainScreen.scale; l.contentsScale = s; l.drawableSize = CGSizeMake(self.bounds.size.width * s, self.bounds.size.height * s); }
@end
@interface GameVC : UIViewController @property (strong) GameView *game; @property (strong) PadView *pad; @property (strong) UILabel *note; @end
@implementation GameVC
- (void)viewDidLoad {
    [super viewDidLoad]; self.view.backgroundColor = UIColor.blackColor;
    self.game = [GameView new]; [self.view addSubview:self.game];
    self.pad = [[PadView alloc] initWithFrame:self.view.bounds]; self.pad.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight; [self.view addSubview:self.pad];
    self.note = [UILabel new]; self.note.textColor = UIColor.whiteColor; self.note.numberOfLines = 0; self.note.textAlignment = NSTextAlignmentCenter; self.note.font = [UIFont systemFontOfSize:15]; self.note.hidden = YES; [self.view addSubview:self.note];
}
- (void)viewDidLayoutSubviews {   // 画面按 16:9 居中放进屏幕（两侧留黑），按键层铺满整屏 | The picture is fitted centred at 16:9 (black bars at the sides); the button layer covers the whole screen
    [super viewDidLayoutSubviews]; CGSize b = self.view.bounds.size; CGFloat w = b.width, h = w * 9 / 16; if (h > b.height) { h = b.height; w = h * 16 / 9; }
    self.game.frame = CGRectMake((b.width - w) / 2, (b.height - h) / 2, w, h); self.note.frame = CGRectInset(self.view.bounds, 60, 40);
}
- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures { return UIRectEdgeAll; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations { return UIInterfaceOrientationMaskLandscape; }
@end

// ---------- 启动 ----------
// ---------- Boot ----------
static char *g_argv[] = {"dmhost", "", "game", "sdk", "NmplDLL", "lz4", NULL};
static void *boot_thread(void *a) { (void)a; dm_main(6, g_argv); return NULL; }
@interface SceneDelegate : UIResponder <UIWindowSceneDelegate> @property (strong) UIWindow *window; @end
@implementation SceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)o {
    (void)session; (void)o;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    GameVC *vc = [GameVC new]; self.window.rootViewController = vc; [self.window makeKeyAndVisible];
    [vc.view layoutIfNeeded]; g_layer = (CAMetalLayer *)vc.game.layer; g_layer.device = MTLCreateSystemDefaultDevice(); g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *doc = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject, *sup = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES).firstObject;
    NSString *romfs = [sup stringByAppendingPathComponent:@"romfs"], *res = NSBundle.mainBundle.resourcePath;
    if (![fm fileExistsAtPath:[romfs stringByAppendingPathComponent:@"NmplDLL.nro"]]) {
        vc.note.hidden = NO; vc.pad.hidden = YES;
        vc.note.text = [NSString stringWithFormat:@"还没有游戏资源。\n在 Mac 上运行 dm/ios_bringup/build.sh push，把 RomFS 推到本 App 的 Library/Application Support/romfs，然后重新打开。\n（%@）", romfs];
        return;
    }
    for (NSString *d in @[@"save", @"temp"]) [fm createDirectoryAtPath:[doc stringByAppendingPathComponent:d] withIntermediateDirectories:YES attributes:nil error:nil];
    freopen([doc stringByAppendingPathComponent:@"log.txt"].fileSystemRepresentation, "w", stderr); setvbuf(stderr, NULL, _IOLBF, 0);   // 宿主日志都走 stderr：落到「文件」App 能看到的 Documents/log.txt | The host logs everything to stderr: it lands in Documents/log.txt, visible in the Files app
    setenv("DM_ROMFS", romfs.fileSystemRepresentation, 1);
    setenv("DM_SAVE", [doc stringByAppendingPathComponent:@"save"].fileSystemRepresentation, 1);
    setenv("DM_TEMP", [doc stringByAppendingPathComponent:@"temp"].fileSystemRepresentation, 1);
    setenv("DM_NVN_SHMAP", [res stringByAppendingPathComponent:@"dm/shader_map.tsv"].fileSystemRepresentation, 1);
    setenv("DM_MSL_DIR", [res stringByAppendingPathComponent:@"msl"].fileSystemRepresentation, 1);
    for (NSString *k in @[@"DM_NVN_ALL_WRITABLE", @"DM_NVN_ALIAS", @"DM_NVN_LINEAR_SYNC", @"DM_NVN_ASTC_VOID", @"DM_NVN_TILE_W", @"DM_NVN_ALPHA_TEST", @"DM_NVN_GS_QUAD", @"DM_WINDOW"]) setenv(k.UTF8String, "1", 1);   // 与 dm/tools/bringup.sh 相同的一组开关 | The same set of switches as dm/tools/bringup.sh
    chdir(res.fileSystemRepresentation);
    [AVAudioSession.sharedInstance setCategory:AVAudioSessionCategoryPlayback error:nil]; [AVAudioSession.sharedInstance setActive:YES error:nil];
    UIApplication.sharedApplication.idleTimerDisabled = YES;
    g_argv[1] = strdup([res stringByAppendingPathComponent:@"dm"].fileSystemRepresentation);   // 模块的布局表与门表（*.fixups.tsv / *.gates.tsv）所在目录 | Directory holding the modules' layout and gate tables (*.fixups.tsv / *.gates.tsv)
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 64u << 20);
    pthread_t th; if (pthread_create(&th, &at, boot_thread, NULL)) abort(); pthread_detach(th);
}
- (void)sceneDidBecomeActive:(UIScene *)s { (void)s; if (getenv("DM_ROMFS")) { dm_gfx_suspend(0); dm_audio_suspend(0); dm_focus_set(1); } }
- (void)sceneWillResignActive:(UIScene *)s { (void)s; if (getenv("DM_ROMFS")) dm_focus_set(2); }
- (void)sceneDidEnterBackground:(UIScene *)s { (void)s; if (getenv("DM_ROMFS")) { dm_focus_set(3); dm_audio_suspend(1); dm_gfx_suspend(1); } }
@end
@interface App : UIResponder <UIApplicationDelegate> @end
@implementation App
- (UISceneConfiguration *)application:(UIApplication *)a configurationForConnectingSceneSession:(UISceneSession *)s options:(UISceneConnectionOptions *)o {
    (void)a; (void)o; UISceneConfiguration *c = [[UISceneConfiguration alloc] initWithName:@"Default" sessionRole:s.role]; c.delegateClass = SceneDelegate.class; return c;
}
@end
int main(int argc, char *argv[]) { @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass(App.class)); } }
