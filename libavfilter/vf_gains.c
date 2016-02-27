// blahgeek

#include "libavutil/opt.h"
#include "libavutil/bprint.h"
#include "libavutil/eval.h"
#include "libavutil/file.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/avassert.h"
#include "libavutil/pixdesc.h"
#include "avfilter.h"
#include "drawutils.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

#define R 0
#define G 1
#define B 2
#define A 3

typedef struct {
    const AVClass *class;

    float * gains;
    int gains_count;

    uint8_t lut[256];
    uint8_t rgba_map[4];

    char *gain_file;
    int step;

    int current_frame;
} GainsContext;

typedef struct ThreadData {
    AVFrame *in, *out;
} ThreadData;

#define OFFSET(x) offsetof(GainsContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM
static const AVOption gains_options[] = {
    { "file", "set data file name", OFFSET(gain_file), AV_OPT_TYPE_STRING, {.str=NULL}, .flags = FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(gains);

static av_cold int init(AVFilterContext *ctx) {
    GainsContext *gain_ctx = ctx->priv;

    FILE * f = fopen(gain_ctx->gain_file, "r");
    if(!f)
        return AVERROR_INVALIDDATA;
    fscanf(f, "%d", &(gain_ctx->gains_count));
    gain_ctx->gains = (float *)malloc(sizeof(float) * gain_ctx->gains_count);
    for(int i = 0 ; i < gain_ctx->gains_count ; i += 1) {
        int ret = fscanf(f, "%f", gain_ctx->gains + i);
        if(ret != 1)
            return AVERROR_INVALIDDATA;
    }
    fclose(f);

    av_log(ctx, AV_LOG_INFO, "%d gains read\n", gain_ctx->gains_count);

    return 0;
}

static int query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_RGB24,  AV_PIX_FMT_BGR24,
        AV_PIX_FMT_RGBA,   AV_PIX_FMT_BGRA,
        AV_PIX_FMT_ARGB,   AV_PIX_FMT_ABGR,
        AV_PIX_FMT_0RGB,   AV_PIX_FMT_0BGR,
        AV_PIX_FMT_RGB0,   AV_PIX_FMT_BGR0,
        AV_PIX_FMT_NONE
    };
    AVFilterFormats *fmts_list = ff_make_format_list(pix_fmts);
    if (!fmts_list)
        return AVERROR(ENOMEM);
    return ff_set_common_formats(ctx, fmts_list);
}

static int config_input(AVFilterLink *inlink)
{
    GainsContext *ctx = inlink->dst->priv;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(inlink->format);

    ff_fill_rgba_map(ctx->rgba_map, inlink->format);
    ctx->step = av_get_padded_bits_per_pixel(desc) >> 3;

    return 0;
}

static int filter_slice(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs)
{
    int x, y;
    const GainsContext *gains_ctx = ctx->priv;
    const ThreadData *td = arg;
    const AVFrame *in  = td->in;
    const AVFrame *out = td->out;
    const int direct = out == in;
    const int step = gains_ctx->step;
    const uint8_t r = gains_ctx->rgba_map[R];
    const uint8_t g = gains_ctx->rgba_map[G];
    const uint8_t b = gains_ctx->rgba_map[B];
    const uint8_t a = gains_ctx->rgba_map[A];
    const int slice_start = (in->height *  jobnr   ) / nb_jobs;
    const int slice_end   = (in->height * (jobnr+1)) / nb_jobs;
    uint8_t       *dst = out->data[0] + slice_start * out->linesize[0];
    const uint8_t *src =  in->data[0] + slice_start *  in->linesize[0];

    for (y = slice_start; y < slice_end; y++) {
        for (x = 0; x < in->width * step; x += step) {
            dst[x + r] = gains_ctx->lut[src[x + r]];
            dst[x + g] = gains_ctx->lut[src[x + g]];
            dst[x + b] = gains_ctx->lut[src[x + b]];
            if (!direct && step == 4)
                dst[x + a] = src[x + a];
        }
        dst += out->linesize[0];
        src += in ->linesize[0];
    }
    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    AVFrame *out;
    ThreadData td;
    GainsContext * gains_ctx = ctx->priv;

    if (av_frame_is_writable(in)) {
        out = in;
    } else {
        out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
        if (!out) {
            av_frame_free(&in);
            return AVERROR(ENOMEM);
        }
        av_frame_copy_props(out, in);
    }

    av_log(ctx, AV_LOG_DEBUG, "Processing frame %d\n", gains_ctx->current_frame);
    for(int i = 0 ; i < 256 ; i += 1) {
        int _x = i * gains_ctx->gains[gains_ctx->current_frame];
        if(_x > 255) _x = 255;
        gains_ctx->lut[i] = (uint8_t)(_x);
    }

    td.in  = in;
    td.out = out;
    ctx->internal->execute(ctx, filter_slice, &td, NULL, FFMIN(outlink->h, ctx->graph->nb_threads));

    gains_ctx->current_frame += 1;
    if(gains_ctx->current_frame >= gains_ctx->gains_count)
        gains_ctx->current_frame = (gains_ctx->gains_count - 1);

    if (out != in)
        av_frame_free(&in);

    return ff_filter_frame(outlink, out);
}

static const AVFilterPad gains_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
        .config_props = config_input,
    },
    { NULL }
};

static const AVFilterPad gains_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
    { NULL }
};

AVFilter ff_vf_gains = {
    .name          = "gains",
    .description   = NULL_IF_CONFIG_SMALL("Adjust components gains."),
    .priv_size     = sizeof(GainsContext),
    .init          = init,
    .query_formats = query_formats,
    .inputs        = gains_inputs,
    .outputs       = gains_outputs,
    .priv_class    = &gains_class,
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC | AVFILTER_FLAG_SLICE_THREADS,
};
