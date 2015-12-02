/* 
* @Author: BlahGeek
* @Date:   2015-09-01
* @Last Modified by:   BlahGeek
* @Last Modified time: 2015-11-29
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

#include "opencv2/core/cuda.hpp"

typedef struct {
    const AVClass *avclass;

    int nb_inputs;
    char * data_file;

    int out_width, out_height;
    vr::MultiMapper * remapper;
    vr::AsyncMultiMapper * async_remapper;

    cv::Size * in_sizes;

    AVFrame ** last_frames;

    struct FFBufQueue *queues;
} VRMapContext;

static int query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *formats = NULL;
    ff_add_format(&formats, AV_PIX_FMT_BGRA); // for OpenCV
    for(int i = 0 ; i < ctx->nb_inputs; i += 1) {
        if(ctx->inputs[i] && !ctx->inputs[i]->out_formats)
            ff_formats_ref(formats, &ctx->inputs[i]->out_formats);
    }

    AVFilterFormats *oformats = NULL;
    ff_add_format(&oformats, AV_PIX_FMT_BGR24);
    ff_formats_ref(oformats, &ctx->outputs[0]->in_formats);

    return 0;
}

static int push_frame(AVFilterContext * ctx) {
    VRMapContext *s = static_cast<VRMapContext *>(ctx->priv);

    if(std::any_of(ctx->inputs, ctx->inputs + s->nb_inputs,
                   [](AVFilterLink *l){ return l->closed; }))
        return AVERROR_EOF;
    if(!std::all_of(s->queues, s->queues + s->nb_inputs,
                    [](struct FFBufQueue &q){ return q.available; }))
        return 0;

    vr::Timer timer("FFMpeg Filter");

    std::vector<AVFrame *> frames(s->nb_inputs);
    for(int i = 0 ; i < s->nb_inputs ; i += 1) {
        frames[i] = ff_bufqueue_get(&s->queues[i]);
        av_assert0(frames[i] != nullptr);
    }

    timer.tick("Get frames from buffer");

    av_log(ctx, AV_LOG_WARNING, "out: %dx%d\n", s->out_width, s->out_height);
    AVFrame * out = ff_get_video_buffer(ctx->outputs[0], s->out_width, s->out_height);
    if(!out) return AVERROR(ENOMEM);
    av_frame_copy_props(out, frames[0]);

    av_assert0(out->data[0] != nullptr);
    av_assert0(out->data[1] == nullptr);
    cv::Mat out_mat(s->out_height, s->out_width, CV_8UC3, out->data[0], out->linesize[0]);

    std::vector<cv::Mat> in_mats;
    for(int i = 0 ; i < frames.size() ; i += 1) {
        auto & f = frames[i];
        in_mats.emplace_back(f->height, f->width, CV_8UC4, f->data[0], f->linesize[0]);
    }

    timer.tick("Constructing matrics");
    
    s->async_remapper->push(in_mats, out_mat);

    AVFrame * real_out = NULL;
    if(s->last_frames == NULL) {
        real_out = ff_get_video_buffer(ctx->outputs[0], s->out_width, s->out_height);
        s->last_frames = new AVFrame * [s->nb_inputs + 1];
    } else {
        s->async_remapper->pop();
        real_out = s->last_frames[s->nb_inputs];
        for(int i = 0 ; i < s->nb_inputs ; i += 1)
            av_frame_free(&s->last_frames[i]);
    }

    for(int i = 0 ; i < s->nb_inputs ; i += 1)
        s->last_frames[i] = frames[i];
    s->last_frames[s->nb_inputs] = out;

    return ff_filter_frame(ctx->outputs[0], real_out);
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
    s->in_sizes[in_no] = cv::Size(inlink->w, inlink->h);

    if(in_no == s->nb_inputs - 1) {
        s->async_remapper = vr::AsyncMultiMapper::New(s->remapper, 
                std::vector<cv::Size>(s->in_sizes, s->in_sizes + s->nb_inputs));
        av_log(ctx, AV_LOG_INFO, "Init async remapper done\n");
    }

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

    s->in_sizes = new cv::Size [s->nb_inputs];

    av_assert0(s->remapper == nullptr);
    av_assert0(s->data_file);

    std::ifstream f(s->data_file);
    s->remapper = vr::MultiMapper::New(f);
    av_log(ctx, AV_LOG_INFO, "Init remapper done\n");

    auto final_size = s->remapper->get_output_size();
    s->out_width = final_size.width;
    s->out_height = final_size.height;

    av_log(ctx, AV_LOG_DEBUG, "init: final size: %dx%d\n",
           s->out_width, s->out_height);

    return 0;
}

static void uninit(AVFilterContext *ctx) {
    av_log(ctx, AV_LOG_INFO, "uniniting...\n");

    VRMapContext *s = static_cast<VRMapContext *>(ctx->priv);
    for(int i = 0 ; i < s->nb_inputs ; i += 1) {
        ff_bufqueue_discard_all(&s->queues[i]);
        av_freep(&s->queues[i]);
        av_freep(&ctx->input_pads[i].name);
    }
    //if(s->remapper) {
        //delete s->remapper;
        //s->remapper = NULL;
    //}
    //if(s->async_remapper) {
        //delete s->async_remapper;
        //s->async_remapper = NULL;
    //}
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
    { "data", "Dumped data file", OFFSET(data_file), AV_OPT_TYPE_STRING, {.str = NULL}, CHAR_MIN, CHAR_MAX, FLAGS},
    { NULL }
};

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

// AVFILTER_DEFINE_CLASS(vr_map);
static const AVClass vr_map_class = {
    "vr_map", // class_name
    av_default_item_name, // item_name
    vr_map_options, // option
    LIBAVUTIL_VERSION_INT, // version
    0, 0,
    NULL, // child_next
    NULL, // child_class_next
    AV_CLASS_CATEGORY_FILTER, // category
    NULL, // get_category
    NULL, // query_ranges
};

// g++傻逼
static const AVFilterPad avfilter_vf_vr_map_output_ = {
    "default", // name
    AVMEDIA_TYPE_VIDEO, //type
    0, 0, // deprecated
    NULL, // start_frame
    NULL, // get_video_buffer
    NULL, // get_audio_buffer
    NULL, // end_frame
    NULL, // draw_slice
    NULL, // filter_frame
    NULL, // poll_frame
    request_frame, // request_frame
    config_output, // config_props
    0, // needs_fifo,
    0 // needs_writable
};

static const AVFilterPad avfilter_vf_vr_map_output_empty = {NULL};

static const AVFilterPad avfilter_vf_vr_map_outputs[] = {
    avfilter_vf_vr_map_output_,
    avfilter_vf_vr_map_output_empty,
};

AVFilter ff_vf_vr_map = {
    "vr_map", // name
    NULL_IF_CONFIG_SMALL("VR Mapping"), // description
    NULL, // inputs
    avfilter_vf_vr_map_outputs, // outputs
    &vr_map_class, // priv_class
    AVFILTER_FLAG_DYNAMIC_INPUTS, // flags
    init, // init
    NULL, // init_dict
    uninit, // uninit
    query_formats, // query_formats
    sizeof(VRMapContext), // priv_size
    NULL, // next
    NULL, // process_command
    NULL, // init_opaque
};
