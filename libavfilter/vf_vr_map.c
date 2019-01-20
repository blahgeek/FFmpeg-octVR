#include "./vf_vr_map_common.h"

// g++真心傻逼

//             Clang大法好
// 天灭g++               退GNU保平安
// 人在做 天在看          代码耦合留祸患
// 内存占用电脑灭         跳出GNU保平安
// 诚心诚念Clang好        模块设计有保障
// 众生都为码农来         现世险恶忘前缘
// Chris Lattner说真相   教你脱险莫拒绝
// 早日摆脱g++           早日获得新生
// 上网搜索「九评GNU」     有 真 相

/* AVFILTER_DEFINE_CLASS(vr_map); */
FRAMESYNC_DEFINE_CLASS(vr_map, VRMapBaseContext, fs);

AVFilter ff_vf_vr_map = {
    .name = "vr_map",
    .description = NULL_IF_CONFIG_SMALL("VR Mapping"),
    .priv_size = 1024,
    .priv_class = &vr_map_class,
    .preinit = &vr_map_framesync_preinit,
    .init = vr_map_init,
    .uninit = vr_map_uninit,
    .query_formats = vr_map_query_formats,
    .activate = vr_map_activate,
    .inputs = NULL,
    .outputs = NULL,
    .flags = AVFILTER_FLAG_DYNAMIC_INPUTS | AVFILTER_FLAG_DYNAMIC_OUTPUTS,
};
