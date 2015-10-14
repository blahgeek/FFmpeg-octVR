/* 
* @Author: BlahGeek
* @Date:   2015-09-01
* @Last Modified by:   BlahGeek
* @Last Modified time: 2015-10-14
*/

#include <stdio.h>
#include <math.h>
#include <assert.h>

extern "C" {
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
}

#include "libmap.hpp"

#define INTERPOLATE_METHOD(name) \
    static uint8_t name(double x, double y, const uint8_t *src, \
                        int width, int height, int stride, uint8_t def)


static uint8_t PIXEL(const uint8_t * src, int x, int y, int width, int height, int stride, int def) {
    if(x < 0) x += width;
    else if(x >= width) x -= width;

    if(y < 0) y += height;
    else if(y >= height) y -= height;

    return src[x + y * stride];
}

/**
 * Nearest neighbor interpolation
 */
INTERPOLATE_METHOD(interpolate_nearest)
{
    return PIXEL(src, (int)(x + 0.5), (int)(y + 0.5), width, height, stride, def);
}

/**
 * Bilinear interpolation
 */
INTERPOLATE_METHOD(interpolate_bilinear)
{
    int x_c, x_f, y_c, y_f;
    int v1, v2, v3, v4;

    if (x < -1 || x > width || y < -1 || y > height) {
        return def;
    } else {
        x_f = (int)x;
        x_c = x_f + 1;

        y_f = (int)y;
        y_c = y_f + 1;

        v1 = PIXEL(src, x_c, y_c, width, height, stride, def);
        v2 = PIXEL(src, x_c, y_f, width, height, stride, def);
        v3 = PIXEL(src, x_f, y_c, width, height, stride, def);
        v4 = PIXEL(src, x_f, y_f, width, height, stride, def);

        return (v1*(x - x_f)*(y - y_f) + v2*((x - x_f)*(y_c - y)) +
                v3*(x_c - x)*(y - y_f) + v4*((x_c - x)*(y_c - y)));
    }
}

/**
 * Biquadratic interpolation
 */
INTERPOLATE_METHOD(interpolate_biquadratic)
{
    int     x_c, x_f, y_c, y_f;
    uint8_t v1,  v2,  v3,  v4;
    double  f1,  f2,  f3,  f4;

    if (x < - 1 || x > width || y < -1 || y > height)
        return def;
    else {
        x_f = (int)x;
        x_c = x_f + 1;
        y_f = (int)y;
        y_c = y_f + 1;

        v1 = PIXEL(src, x_c, y_c, width, height, stride, def);
        v2 = PIXEL(src, x_c, y_f, width, height, stride, def);
        v3 = PIXEL(src, x_f, y_c, width, height, stride, def);
        v4 = PIXEL(src, x_f, y_f, width, height, stride, def);

        f1 = 1 - sqrt((x_c - x) * (y_c - y));
        f2 = 1 - sqrt((x_c - x) * (y - y_f));
        f3 = 1 - sqrt((x - x_f) * (y_c - y));
        f4 = 1 - sqrt((x - x_f) * (y - y_f));
        return (v1 * f1 + v2 * f2 + v3 * f3 + v4 * f4) / (f1 + f2 + f3 + f4);
    }
}

typedef struct {
    const AVClass *avclass;

    char * in_type_name;
    char * out_type_name;

    char * in_opts;
    char * out_opts;

    double rotate_x, rotate_y, rotate_z; // degree

    int out_width, out_height;
    vr::Remapper * remapper;
} VRMapContext;

static int query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *formats = NULL;
    ff_add_format(&formats, AV_PIX_FMT_YUV420P);
    return ff_set_common_formats(ctx, formats);
}

static int init(AVFilterContext *ctx) {
    return 0;
}

static void uninit(AVFilterContext *ctx) {
    VRMapContext *s = static_cast<VRMapContext *>(ctx->priv);
    if(s->remapper) {
        delete s->remapper;
        s->remapper = NULL;
    }
}

static int config_input(AVFilterLink *link)
{
    AVFilterContext *ctx = link->dst;
    VRMapContext *s = static_cast<VRMapContext *>(ctx->priv);
    av_log(ctx, AV_LOG_VERBOSE, "Input: %dx%d\n", link->w, link->h);

    try {
        s->remapper = new vr::Remapper(s->in_type_name, vr::json::parse(s->in_opts),
                                       s->out_type_name, vr::json::parse(s->out_opts),
                                       s->rotate_z, s->rotate_y, s->rotate_x,
                                       link->w, link->h, s->out_width, s->out_height);
    } catch (std::string & e) {
        av_log(ctx, AV_LOG_ERROR, "Error: %s\n", e.c_str());
        return -1;
    }

    std::pair<int, int> output_size = s->remapper->get_output_size();
    s->out_width = output_size.first;
    s->out_height = output_size.second;
    av_log(ctx, AV_LOG_VERBOSE, "Output: %dx%d\n", s->out_width, s->out_height);

    return 0;
}

static int config_output(AVFilterLink *link)
{
    VRMapContext *s = static_cast<VRMapContext *>(link->src->priv);

    link->w = s->out_width;
    link->h = s->out_height;

    return 0;
}

static int filter_frame(AVFilterLink *link, AVFrame *in)
{
    AVFilterContext *ctx = link->dst;
    VRMapContext *s = static_cast<VRMapContext *>(ctx->priv);
    AVFilterLink *outlink = ctx->outputs[0];

    AVFrame * out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if(!out) {
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }
    av_frame_copy_props(out, in);

    for(int j = 0 ; j < outlink->h ; j += 1) {
        for(int i = 0 ; i < outlink->w ; i += 1) {
            int index = j * outlink->w + i;

            int p0 = j * out->linesize[0] + i;
            int p1 = (j >> 1) * out->linesize[1] + (i >> 1);
            int p2 = (j >> 1) * out->linesize[2] + (i >> 1);

            vr::PointAndFlag map = s->remapper->get_map(i, j);
            double real_x = std::get<0>(map);
            double real_y = std::get<1>(map);
            bool valid = std::get<2>(map);

            if(!valid) {
                out->data[0][p0] = 0;
                if((i & 1) == 0 && (j & 1) == 0)
                    out->data[1][p1] = out->data[2][p2] = 128; // black, zero yields green
                continue;
            }

            out->data[0][p0] = 
                interpolate_bilinear(real_x, real_y, in->data[0],
                                     in->width, in->height, in->linesize[0], 0);
            if((i & 1) == 0 && (j & 1) == 0) {
                out->data[1][p1] = 
                    interpolate_bilinear(real_x / 2, real_y / 2, in->data[1],
                                         in->width >> 1, in->height >> 1, in->linesize[1], 0);
                out->data[2][p2] = 
                    interpolate_bilinear(real_x / 2, real_y / 2, in->data[2],
                                         in->width >> 1, in->height >> 1, in->linesize[2], 0);
            }
        }
    }

    av_frame_free(&in);
    return ff_filter_frame(outlink, out);
}

#define OFFSET(x) offsetof(VRMapContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)

static const AVOption vr_map_options[] = {
    { "in", "Input projection type", OFFSET(in_type_name), AV_OPT_TYPE_STRING, {.str = "equirectangular"}, CHAR_MIN, CHAR_MAX, FLAGS},
    { "out", "Output projection type", OFFSET(out_type_name), AV_OPT_TYPE_STRING, {.str = "normal"}, CHAR_MIN, CHAR_MAX, FLAGS},
    { "in_opts", "Camera parameter for input", OFFSET(in_opts), AV_OPT_TYPE_STRING, {.str = "{}"}, CHAR_MIN, CHAR_MAX, FLAGS},
    { "out_opts", "Camera parameter for output", OFFSET(out_opts), AV_OPT_TYPE_STRING, {.str = "{}"}, CHAR_MIN, CHAR_MAX, FLAGS},
    { "out_width", "Output width", OFFSET(out_width), AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, FLAGS},
    { "out_height", "Output height", OFFSET(out_height), AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, FLAGS},
    { "rotate_z", "Rotate degree based on Z-axis", OFFSET(rotate_z), AV_OPT_TYPE_DOUBLE, {.dbl = 0}, 0, 360, FLAGS},
    { "rotate_y", "Rotate degree based on Y-axis", OFFSET(rotate_y), AV_OPT_TYPE_DOUBLE, {.dbl = 0}, 0, 360, FLAGS},
    { "rotate_x", "Rotate degree based on X-axis", OFFSET(rotate_x), AV_OPT_TYPE_DOUBLE, {.dbl = 0}, 0, 360, FLAGS},
    { NULL }
};

AVFILTER_DEFINE_CLASS(vr_map);

static const AVFilterPad avfilter_vf_vr_map_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
        .config_props = config_input,
    },
    { NULL }
};

static const AVFilterPad avfilter_vf_vr_map_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_output,
    },
    { NULL }
};

AVFilter ff_vf_vr_map = {
    .name          = "vr_map",
    .description   = NULL_IF_CONFIG_SMALL("VR Mapping"),
    .priv_size     = sizeof(VRMapContext),
    .priv_class    = &vr_map_class,
    .query_formats = query_formats,
    .init          = init,
    .uninit        = uninit,
    .inputs        = avfilter_vf_vr_map_inputs,
    .outputs       = avfilter_vf_vr_map_outputs,
};
