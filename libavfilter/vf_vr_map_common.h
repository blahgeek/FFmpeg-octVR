#ifndef LIBAVFILTER_VF_VR_MAP_COMMON_H_
#define LIBAVFILTER_VF_VR_MAP_COMMON_H_ value

#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"
#include "libavutil/eval.h"
#include "libavutil/avstring.h"
#include "libavutil/internal.h"
#include "libavutil/libm.h"
#include "libavutil/imgutils.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"
#include "bufferqueue.h"
#include "framesync.h"

extern int vr_map_query_formats(AVFilterContext *ctx);
extern int vr_map_init(AVFilterContext *ctx);
extern void vr_map_uninit(AVFilterContext *ctx);
extern int vr_map_activate(AVFilterContext *ctx);

extern const AVOption vr_map_options[];

typedef struct {
    const AVClass *avclass;
    FFFrameSync fs;
} VRMapBaseContext;

#endif /* ifndef LIBAVFILTER_VF_VR_MAP_COMMON_H_ */
