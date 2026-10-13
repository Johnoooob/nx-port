// 另一个游戏试启动时才用得到的接管（条目都是可选的：理由以 ? 开头，暗黑 II 不引用也不算错）。
// 新版 SDK 里有些接口内部多了一步连系统服务的动作（旧版没有），宿主没有服务进程，在接口层直接应答。
// Takeovers needed only when bringing up another game (every entry is optional: the reason starts with ?, so Diablo II not referencing it is not an error).
// In newer SDKs some interfaces gained an internal step that connects to a system service (older ones had none); the host has no service processes, so it answers at the interface layer.
#include "hle.h"

HLE_FORCE("_ZN2nn2fs12SetAllocatorEPFPvmEPFvS1_mE", "?文件系统由宿主接管，不用客体分配器；新版 SDK 在这里会去连 fsp-srv 查访问日志开关", void, h_bu_fs_SetAllocator, void *alloc, void *dealloc) { (void)alloc; (void)dealloc; }

#include <unistd.h>
void hle_bind_system_event(void *ev, int autoclear);
void hle_signal_system_event(void *ev);

// ---- 运行环境（applet 服务）----
// ---- Operating environment (applet service) ----
HLE_FORCE("_ZN2nn2oe15SetCpuBoostModeENS0_12CpuBoostModeE", "?CPU 加速模式；宿主无此概念", void, h_bu_oe_SetCpuBoost, int mode) { (void)mode; }
HLE_FORCE("_ZN2nn2oe27GetDefaultDisplayResolutionEPiS1_", "?缺省显示分辨率；按掌机 1280x720 报", void, h_bu_oe_GetDefRes, int *w, int *h) { *w = 1280; *h = 720; }
HLE_FORCE("_ZN2nn2oe38GetDefaultDisplayResolutionChangeEventEPNS_2os11SystemEventE", "?分辨率变化事件；宿主永不触发", void, h_bu_oe_GetDefResEvent, void *ev) { hle_bind_system_event(ev, 1); }
HLE_FORCE("_ZN2nn2oe31EnterExitRequestHandlingSectionEv", "?退出请求处理区；宿主不发退出请求", void, h_bu_oe_EnterExit, void) {}
HLE_FORCE("_ZN2nn2oe31LeaveExitRequestHandlingSectionEv", "?退出请求处理区；宿主不发退出请求", void, h_bu_oe_LeaveExit, void) {}

// ---- 垂直同步事件：宿主线程按 60 Hz 触发 ----
// ---- Vsync event: a host thread signals it at 60 Hz ----
static void *g_bu_vsync_ev;
static void *bu_vsync_thread(void *a) { (void)a; for (;;) { usleep(16667); hle_signal_system_event(g_bu_vsync_ev); } return NULL; }
HLE_FORCE("_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", "?垂直同步事件；宿主线程按 60 Hz 触发", Result, h_bu_vi_GetVsyncEvent, void *ev, void *display) {
    (void)display; hle_bind_system_event(ev, 1);
    if (!g_bu_vsync_ev) { g_bu_vsync_ev = ev; pthread_t th; if (pthread_create(&th, NULL, bu_vsync_thread, NULL)) die("垂直同步线程起不来"); pthread_detach(th); LOG("vi::GetDisplayVsyncEvent：宿主 60 Hz 触发线程已起"); }
    return RES_OK;
}

// ---- 输入：分离手柄、体感、触屏先报「没有」 ----
// ---- Input: detached Joy-Con, motion sensor and touch screen report "none" for now ----
HLE_FORCE("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj", "?左手柄单独状态；报空", void, h_bu_hid_JoyLeft, void *s, const uint32_t *id) { (void)id; memset(s, 0, 0x28); }
HLE_FORCE("_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj", "?右手柄单独状态；报空", void, h_bu_hid_JoyRight, void *s, const uint32_t *id) { (void)id; memset(s, 0, 0x28); }
HLE_FORCE("_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", "?体感句柄；报 0 个", int, h_bu_hid_SixHandles, uint32_t *h, int n, const uint32_t *id, uint32_t style) { (void)id; (void)style; for (int i = 0; i < n; i++) h[i] = 0; return 0; }
HLE_FORCE("_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE", "?体感启动；空操作", void, h_bu_hid_SixStart, const uint32_t *h) { (void)h; }
HLE_FORCE("_ZN2nn3hid21GetSixAxisSensorStateEPNS0_18SixAxisSensorStateERKNS0_19SixAxisSensorHandleE", "?体感状态；报全零", void, h_bu_hid_SixState, void *s, const uint32_t *h) { (void)h; memset(s, 0, 0x60); }
HLE_FORCE("_ZN2nn3hid19GetTouchScreenStateILm16EEEvPNS0_16TouchScreenStateIXT_EEE", "?触屏状态；报 0 个触点", void, h_bu_hid_Touch16, void *s) { memset(s, 0, 16); }

// ---- 存档容量管理、网页 ----
// ---- Save-data capacity management, web pages ----
HLE_FORCE("_ZN2nn2fs15GetSaveDataSizeEPlS1_RKNS_7account3UidE", "?存档容量；报 64 MB", Result, h_bu_fs_GetSaveDataSize, int64_t *sz, int64_t *jsz, const void *uid) { (void)uid; *sz = 64 << 20; *jsz = 64 << 20; return RES_OK; }
HLE_FORCE("_ZN2nn2fs18GetSaveDataSizeMaxEPlS1_", "?存档容量上限；报 64 MB", void, h_bu_fs_GetSaveDataSizeMax, int64_t *sz, int64_t *jsz) { *sz = 64 << 20; *jsz = 64 << 20; }
HLE_FORCE("_ZN2nn2fs14ExtendSaveDataERKNS_7account3UidEll", "?存档扩容；宿主目录无上限", Result, h_bu_fs_ExtendSaveData, const void *uid, int64_t sz, int64_t jsz) { (void)uid; (void)sz; (void)jsz; return RES_OK; }
HLE_FORCE("_ZN2nn3web19ShowOfflineHtmlPageEPNS0_26OfflineHtmlPageReturnValueERKNS0_22ShowOfflineHtmlPageArgE", "?离线网页（电子说明书）；宿主不显示", Result, h_bu_web_Offline, void *ret, const void *arg) { (void)ret; (void)arg; LOG("web::ShowOfflineHtmlPage：不显示"); return RES_OK; }
HLE_FORCE("_ZN2nn3web11ShowWebPageEPNS0_18WebPageReturnValueERKNS0_14ShowWebPageArgE", "?网页；宿主不显示", Result, h_bu_web_Page, void *ret, const void *arg) { (void)ret; (void)arg; return RES_FAIL; }
