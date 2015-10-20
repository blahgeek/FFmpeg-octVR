/* 
* @Author: BlahGeek
* @Date:   2015-09-01
* @Last Modified by:   BlahGeek
* @Last Modified time: 2015-10-19
*/

#include <stdio.h>
#include <math.h>

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
#include "bufferqueue.h"
}

#include <fstream>
#include <iostream>
#include <algorithm>
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

    int nb_inputs;
    char * options_file;
    vr::json options;

    int out_width, out_height;
    vr::MultiMapper * remapper;

    struct FFBufQueue *queues;
} VRMapContext;

static int query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *formats = NULL;
    ff_add_format(&formats, AV_PIX_FMT_YUV420P);
    return ff_set_common_formats(ctx, formats);
}

static int push_frame(AVFilterContext * ctx) {
    VRMapContext *s = static_cast<VRMapContext *>(ctx->priv);

    if(std::any_of(ctx->inputs, ctx->inputs + s->nb_inputs,
                   [](AVFilterLink *l){ return l->closed; }))
        return AVERROR_EOF;
    if(!std::all_of(s->queues, s->queues + s->nb_inputs,
                    [](struct FFBufQueue &q){ return q.available; }))
        return 0;

    std::vector<AVFrame *> frames(s->nb_inputs);
    for(int i = 0 ; i < s->nb_inputs ; i += 1) {
        frames[i] = ff_bufqueue_get(&s->queues[i]);
        av_assert0(frames[i] != nullptr);
    }

    av_log(ctx, AV_LOG_WARNING, "out: %dx%d\n", s->out_width, s->out_height);
    av_assert0(ctx->outputs != nullptr);
    av_assert0(ctx->outputs[0] != nullptr);
    AVFrame * out = ff_get_video_buffer(ctx->outputs[0], s->out_width, s->out_height);
    if(!out) return AVERROR(ENOMEM);
    av_frame_copy_props(out, frames[0]);

    av_assert0(out->width == s->out_width);
    av_assert0(out->height == s->out_height);

    for(int j = 0 ; j < s->out_height ; j += 1) {
        for(int i = 0 ; i < s->out_width ; i += 1) {
            int p0 = j * out->linesize[0] + i;
            int p1 = (j >> 1) * out->linesize[1] + (i >> 1);
            int p2 = (j >> 1) * out->linesize[2] + (i >> 1);

            auto map = s->remapper->get_map(i, j);
            int frame_index = map.first;
            double real_x = map.second.x;
            double real_y = map.second.y;
            bool valid = !isnan(real_x) && !isnan(real_y);

            AVFrame * in = frames[frame_index];
            av_assert0(in != nullptr);

            if(!valid) {
                out->data[0][p0] = 0;
                if((i & 1) == 0 && (j & 1) == 0)
                    out->data[1][p1] = out->data[2][p2] = 128; // black, zero yields green
                continue;
            }

            av_assert0(out != nullptr);
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

    for(auto f: frames)
        av_frame_free(&f);
    return ff_filter_frame(ctx->outputs[0], out);
}

static int filter_frame(AVFilterLink *inlink, AVFrame * frame) {
    AVFilterContext * ctx = inlink->dst;
    VRMapContext *s = static_cast<VRMapContext *>(ctx->priv);
    unsigned in_no = FF_INLINK_IDX(inlink);

    av_log(ctx, AV_LOG_DEBUG, "filter_frame: %u\n", in_no);
    ff_bufqueue_add(ctx, &s->queues[in_no], frame);

    return push_frame(ctx);
}

static int config_input(AVFilterLink *inlink) {
    AVFilterContext * ctx = inlink->dst;
    VRMapContext *s = static_cast<VRMapContext *>(ctx->priv);
    unsigned in_no = FF_INLINK_IDX(inlink);
    av_log(ctx, AV_LOG_DEBUG, "config_input: input %d size: %dx%d\n",
           in_no, inlink->w, inlink->h);

    s->remapper->add_input(s->options["inputs"][in_no]["type"],
                           s->options["inputs"][in_no]["options"],
                           inlink->w, inlink->h);
    return 0;
}

static int init(AVFilterContext *ctx) {
    VRMapContext *s = static_cast<VRMapContext *>(ctx->priv);
    s->queues = static_cast<struct FFBufQueue *>(av_calloc(s->nb_inputs, sizeof(s->queues[0])));
    if(!s->queues)
        return AVERROR(ENOMEM);

    for(int i = 0 ; i < s->nb_inputs ; i += 1) {
        AVFilterPad inpad = { 0 };
        inpad.name = av_asprintf("input%d", i);
        inpad.type = AVMEDIA_TYPE_VIDEO;
        inpad.filter_frame = filter_frame;
        inpad.config_props = config_input;
        ff_insert_inpad(ctx, i, &inpad);
    }

    std::ifstream f(s->options_file);
    s->options << f;

    av_assert0(s->remapper == nullptr);

    s->remapper = new vr::MultiMapper(s->options["output"]["type"],
                                      s->options["output"]["options"],
                                      s->out_width, s->out_height);
    auto final_size = s->remapper->get_output_size();
    s->out_width = final_size.width;
    s->out_height = final_size.height;

    av_log(ctx, AV_LOG_DEBUG, "init: final size: %dx%d\n",
           s->out_width, s->out_height);

    return 0;
}

static void uninit(AVFilterContext *ctx) {
    VRMapContext *s = static_cast<VRMapContext *>(ctx->priv);
    for(int i = 0 ; i < s->nb_inputs ; i += 1) {
        ff_bufqueue_discard_all(&s->queues[i]);
        av_freep(&s->queues[i]);
        av_freep(&ctx->input_pads[i].name);
    }
    if(s->remapper) {
        delete s->remapper;
        s->remapper = NULL;
    }
}

static int config_output(AVFilterLink *link)
{
    VRMapContext *s = static_cast<VRMapContext *>(link->src->priv);
    link->w = s->out_width;
    link->h = s->out_height;
    return 0;
}

static int request_frame(AVFilterLink *outlink) {
    AVFilterContext *ctx = outlink->src;
    VRMapContext *s = static_cast<VRMapContext *>(ctx->priv);

    for(int i = 0 ; i < ctx->nb_inputs ; i += 1) {
        if(!s->queues[i].available && !ctx->inputs[i]->closed) {
            int ret = ff_request_frame(ctx->inputs[i]);
            if(ret != AVERROR_EOF)
                return ret;
        }
    }

    return push_frame(ctx);
}

#define OFFSET(x) offsetof(VRMapContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)

static const AVOption vr_map_options[] = {
    { "inputs", "Number of input streams", OFFSET(nb_inputs), AV_OPT_TYPE_INT, {.i64 = 2}, 1, INT_MAX, FLAGS},
    { "options", "Options (json file)", OFFSET(options_file), AV_OPT_TYPE_STRING, {.str = NULL}, CHAR_MIN, CHAR_MAX, FLAGS},
    { "out_width", "Output width", OFFSET(out_width), AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, FLAGS},
    { "out_height", "Output height", OFFSET(out_height), AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, FLAGS},
    { NULL }
};

AVFILTER_DEFINE_CLASS(vr_map);

static const AVFilterPad avfilter_vf_vr_map_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_output,
        .request_frame = request_frame,
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
    .outputs       = avfilter_vf_vr_map_outputs,
    .flags         = AVFILTER_FLAG_DYNAMIC_INPUTS,
};
