/* 
* @Author: BlahGeek
* @Date:   2015-09-01
* @Last Modified by:   BlahGeek
* @Last Modified time: 2019-01-06
*/

#include <stdio.h>
#include <math.h>

#include <fstream>
#include <iostream>
#include <iomanip>

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
#include <string>
#include <sstream>
#include <vector>

#include "opencv2/core/cuda.hpp"
#include "octvr.hpp"

// helper
static std::vector<std::string> &split(const std::string &s, char delim, std::vector<std::string> &elems) {
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim))
        elems.push_back(item);
    return elems;
}

std::vector<std::string> split(const char * s, char delim) {
    if(s == NULL) return std::vector<std::string>();
    std::vector<std::string> elems;
    split(std::string(s), delim, elems);
    return elems;
}


typedef struct {
    const AVClass *avclass;

    // opts
    int opt_inputs;
    char * opt_outputs;
    int opt_crop_x, opt_crop_w;
    char * opt_blend;
    char * opt_exposure;
    char * opt_region;
    int opt_width, opt_height;
    int opt_preview_width, opt_preview_height;

    // parsed opts
    int * blend_modes;
    int * gain_modes;
    cv::Rect_<double> * output_regions;

    // storage
    int input_format;
    vr::MapperTemplate ** mapper_templates;

    int outputs; // size of most above lists

    vr::AsyncMultiMapper * async_remapper;

    // others
    cv::Size * in_sizes;
    AVFrame ** last_frames;
    struct FFBufQueue * queues;

} VRMapContext;

static int query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *formats = NULL;
    ff_add_format(&formats, AV_PIX_FMT_YUV420P);
    ff_add_format(&formats, AV_PIX_FMT_YUVJ420P);
    for(size_t i = 0 ; i < ctx->nb_inputs; i += 1) {
        if(ctx->inputs[i] && !ctx->inputs[i]->out_formats)
            ff_formats_ref(formats, &ctx->inputs[i]->out_formats);
    }

    AVFilterFormats *oformats = NULL;
    ff_add_format(&oformats, AV_PIX_FMT_YUV420P);
    for(size_t i = 0 ; i < ctx->nb_outputs ; i += 1) {
        if(ctx->outputs[i] && !ctx->outputs[i]->in_formats)
            ff_formats_ref(oformats, &ctx->outputs[i]->in_formats);
    }

    return 0;
}

static int push_frame(AVFilterContext * ctx) {
    VRMapContext *s = static_cast<VRMapContext *>(ctx->priv);

    bool inputs_eof = std::any_of(ctx->inputs, ctx->inputs + ctx->nb_inputs,
                                  [](AVFilterLink *l){ return l->closed; });
    bool queues_available = std::all_of(s->queues, s->queues + ctx->nb_inputs,
                                        [](struct FFBufQueue &q){ return q.available; });
    bool has_last_frame = s->last_frames != NULL;

    if(!queues_available && inputs_eof && !has_last_frame)
        return AVERROR_EOF;
    if(!queues_available && !inputs_eof)
        return 0;

    vr::Timer timer("FFMpeg Filter");

    std::vector<AVFrame *> frames(ctx->nb_inputs, NULL);
    AVFrame * out_frame = NULL;

    std::vector<std::tuple<cv::Mat, cv::Mat, cv::Mat>> in_mats;
    std::tuple<cv::Mat, cv::Mat, cv::Mat> out_mat;

    if(queues_available) {
        for(size_t i = 0 ; i < ctx->nb_inputs ; i += 1) {
            frames[i] = ff_bufqueue_get(&s->queues[i]);
            av_assert0(frames[i] != nullptr);

            auto & f = frames[i];
            int real_w = s->opt_crop_w != 0 ? s->opt_crop_w : f->width;
            av_assert0(real_w % 2 == 0 && s->opt_crop_x % 2 == 0);

            in_mats.emplace_back(cv::Mat(f->height, real_w, CV_8U,
                                         f->data[0] + s->opt_crop_x,
                                         f->linesize[0]),
                                 cv::Mat(f->height / 2, real_w / 2, CV_8U,
                                         f->data[1] + s->opt_crop_x / 2,
                                         f->linesize[1]),
                                 cv::Mat(f->height / 2, real_w / 2, CV_8U,
                                         f->data[2] + s->opt_crop_x / 2,
                                         f->linesize[2]));
        }
        timer.tick("Prepare inputs");

        out_frame = ff_get_video_buffer(ctx->outputs[0], s->opt_width, s->opt_height);
        av_frame_copy_props(out_frame, frames[0]);
        out_mat = std::make_tuple(cv::Mat(cv::Size(s->opt_width, s->opt_height), CV_8U,
                                          out_frame->data[0], out_frame->linesize[0]),
                                  cv::Mat(cv::Size(s->opt_width / 2, s->opt_height / 2), CV_8U,
                                          out_frame->data[1], out_frame->linesize[1]),
                                  cv::Mat(cv::Size(s->opt_width / 2, s->opt_height / 2), CV_8U,
                                          out_frame->data[2], out_frame->linesize[2]));

        timer.tick("Prepare outputs");
        s->async_remapper->push(in_mats, out_mat);
    }

    AVFrame * real_out_frame = NULL;
    if(!has_last_frame)
        s->last_frames = new AVFrame * [ctx->nb_inputs + 1];
    else {
        s->async_remapper->pop();
        real_out_frame = s->last_frames[ctx->nb_inputs];
        for(size_t i = 0 ; i < ctx->nb_inputs ; i += 1)
            av_frame_free(&s->last_frames[i]);
        timer.tick("Pop last frames");
    }

    if(queues_available) {
        for(size_t i = 0 ; i < ctx->nb_inputs ; i += 1)
            s->last_frames[i] = frames[i];
        s->last_frames[ctx->nb_inputs] = out_frame;
    } else {
        delete [] s->last_frames;
        s->last_frames = NULL;
    }

    if(real_out_frame)
        ff_filter_frame(ctx->outputs[0], real_out_frame);
    timer.tick("Do next filter");

    return 0;
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
    if(s->opt_crop_w != 0)
        av_log(ctx, AV_LOG_WARNING, "Using width %d for input %d\n", s->opt_crop_w, in_no);
    s->in_sizes[in_no] = cv::Size(s->opt_crop_w != 0 ? s->opt_crop_w : inlink->w, inlink->h);

    if(s->input_format != 0 && s->input_format != inlink->format) {
        av_log(ctx, AV_LOG_ERROR, "Pixel formats for all inputs should be same.\n");
        return -1;
    }
    if(s->input_format == 0) {
        s->input_format = inlink->format;
        av_assert0(s->input_format == AV_PIX_FMT_YUV420P || s->input_format == AV_PIX_FMT_YUVJ420P);
    }

    if(in_no == ctx->nb_inputs - 1) {
        std::vector<vr::MapperTemplate> _templates;
        for(int i = 0 ; i < s->outputs ; i += 1)
            _templates.push_back(*s->mapper_templates[i]);

        s->async_remapper = vr::AsyncMultiMapper::New(
            _templates,
            std::vector<cv::Size>(s->in_sizes, s->in_sizes + ctx->nb_inputs),
            cv::Size(s->opt_width, s->opt_height),
            std::vector<int>(s->blend_modes, s->blend_modes + s->outputs),
            std::vector<int>(s->gain_modes, s->gain_modes + s->outputs),
            std::vector<cv::Rect_<double>>(s->output_regions, s->output_regions + s->outputs),
            cv::Size(s->opt_preview_width, s->opt_preview_height)
        );
        av_log(ctx, AV_LOG_INFO, "Init async remapper done\n");
    }

    return 0;
}

static int config_output(AVFilterLink *link)
{
    VRMapContext *s = static_cast<VRMapContext *>(link->src->priv);

    link->w = s->opt_width;
    link->h = s->opt_height;
    return 0;
}

static int request_frame(AVFilterLink *outlink) {
    AVFilterContext *ctx = outlink->src;
    VRMapContext *s = static_cast<VRMapContext *>(ctx->priv);

    for(size_t i = 0 ; i < ctx->nb_inputs ; i += 1) {
        if(!s->queues[i].available && !ctx->inputs[i]->closed) {
            int ret = ff_request_frame(ctx->inputs[i]);
            if(ret != AVERROR_EOF)
                return ret;
        }
    }

    return push_frame(ctx);
}

static int init(AVFilterContext *ctx) {
    VRMapContext *s = static_cast<VRMapContext *>(ctx->priv);

    // parse opts
    auto opt_outputs_split = split(s->opt_outputs, '|');
    s->outputs = opt_outputs_split.size();
    av_assert0(s->outputs > 0);

    auto opt_blend_split = split(s->opt_blend, '|');
    av_assert0(opt_blend_split.size() == s->outputs || opt_blend_split.empty());

    auto opt_exposure_split = split(s->opt_exposure, '|');
    av_assert0(opt_exposure_split.size() == s->outputs || opt_exposure_split.empty());

    auto opt_region_split = split(s->opt_region, '|');
    av_assert0(opt_region_split.size() == s->outputs || opt_region_split.empty());

    s->mapper_templates = new vr::MapperTemplate * [s->outputs];
    s->blend_modes = new int [s->outputs];
    s->gain_modes = new int [s->outputs];
    s->output_regions = new cv::Rect_<double> [s->outputs];

    for(int i = 0 ; i < s->outputs ; i += 1) {
        av_log(ctx, AV_LOG_INFO, "Loading template %s\n", opt_outputs_split[i].c_str());
        std::ifstream f(opt_outputs_split[i].c_str(), std::ios::binary);
        try {
            s->mapper_templates[i] = new vr::MapperTemplate(f);
        } catch (std::string & e) {
            av_log(ctx, AV_LOG_ERROR, "Error loading template: %s\n", e.c_str());
            return AVERROR_INVALIDDATA;
        }
        av_log(ctx, AV_LOG_INFO, "Load complete, size: %dx%d\n",
               s->mapper_templates[i]->out_size.width,
               s->mapper_templates[i]->out_size.height);

        if(opt_blend_split.empty())
            s->blend_modes[i] = -1;
        else
            s->blend_modes[i] = std::atoi(opt_blend_split[i].c_str());

        if(opt_exposure_split.empty())
            s->gain_modes[i] = -1;
        else
            s->gain_modes[i] = std::atoi(opt_exposure_split[i].c_str());

        if(opt_region_split.empty())
            s->output_regions[i] = cv::Rect_<double>(0., 0., 1., 1.);
        else {
            auto region_split_more = split(opt_region_split[i].c_str(), '/');
            s->output_regions[i].x = std::atof(region_split_more[0].c_str());
            s->output_regions[i].y = std::atof(region_split_more[1].c_str());
            s->output_regions[i].width = std::atof(region_split_more[2].c_str());
            s->output_regions[i].height = std::atof(region_split_more[3].c_str());
        }

        av_log(ctx, AV_LOG_DEBUG, "Output No.%d: blend=%d, exposure=%d, region=(%.2f,%.2f)-(%.2fx%.2f)\n",
               i, s->blend_modes[i], s->gain_modes[i],
               s->output_regions[i].x, s->output_regions[i].y,
               s->output_regions[i].width, s->output_regions[i].height);
    }

    s->queues = static_cast<struct FFBufQueue *>(av_calloc(s->opt_inputs, sizeof(s->queues[0])));
    if(!s->queues)
        return AVERROR(ENOMEM);

    for(int i = 0 ; i < s->opt_inputs ; i += 1) {
        AVFilterPad inpad = { 0 };
        inpad.name = av_strdup(av_asprintf("input%d", i));
        inpad.type = AVMEDIA_TYPE_VIDEO;
        inpad.filter_frame = filter_frame;
        inpad.config_props = config_input;
        ff_insert_inpad(ctx, i, &inpad);
    }
    s->in_sizes = new cv::Size [s->opt_inputs];

    AVFilterPad outpad = { 0 };
    outpad.name = av_strdup("output0");
    outpad.type = AVMEDIA_TYPE_VIDEO;
    outpad.request_frame = request_frame;
    outpad.config_props = config_output;
    ff_insert_outpad(ctx, 0, &outpad);

    return 0;
}

static void uninit(AVFilterContext *ctx) {
    av_log(ctx, AV_LOG_INFO, "uniniting...\n");

    VRMapContext *s = static_cast<VRMapContext *>(ctx->priv);
    for(size_t i = 0 ; i < ctx->nb_inputs ; i += 1) {
        ff_bufqueue_discard_all(&s->queues[i]);
        av_freep(&s->queues[i]);
        av_freep(&ctx->input_pads[i].name);
    }
    for(size_t i = 0 ; i < s->outputs ; i += 1) {
        delete s->mapper_templates[i];
        s->mapper_templates[i] = NULL;
    }
    if(s->async_remapper) {
        delete s->async_remapper;
        s->async_remapper = NULL;
    }
}

#define OFFSET(x) offsetof(VRMapContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)

static const AVOption vr_map_options[] = {
    { "inputs", "Number of input streams", OFFSET(opt_inputs), AV_OPT_TYPE_INT, {2}, 1, INT_MAX, FLAGS},
    { "outputs", "`|`-seperated output templates", OFFSET(opt_outputs), AV_OPT_TYPE_STRING, {0}, CHAR_MIN, CHAR_MAX, FLAGS},
    { "crop_x", "Crop X", OFFSET(opt_crop_x), AV_OPT_TYPE_INT, {0}, 0, INT_MAX, FLAGS},
    { "crop_w", "Crop width", OFFSET(opt_crop_w), AV_OPT_TYPE_INT, {0}, 0, INT_MAX, FLAGS},
    { "blend", "`|`-seperated blending param", OFFSET(opt_blend), AV_OPT_TYPE_STRING, {0}, CHAR_MIN, CHAR_MAX, FLAGS},
    { "exposure", "`|`-seperated exposure param", OFFSET(opt_exposure), AV_OPT_TYPE_STRING, {0}, CHAR_MIN, CHAR_MAX, FLAGS},
    { "region", "`|`-seperated region param", OFFSET(opt_region), AV_OPT_TYPE_STRING, {0}, CHAR_MIN, CHAR_MAX, FLAGS},
    { "width", "Output width", OFFSET(opt_width), AV_OPT_TYPE_INT, {3840}, INT_MIN, INT_MAX, FLAGS},
    { "height", "Output height", OFFSET(opt_height), AV_OPT_TYPE_INT, {2160}, INT_MIN, INT_MAX, FLAGS},
    { "preview_width", "Preview output width (for QT only)", OFFSET(opt_preview_width), AV_OPT_TYPE_INT, {0}, 0, INT_MAX, FLAGS},
    { "preview_height", "Preview output height (for QT only)", OFFSET(opt_preview_height), AV_OPT_TYPE_INT, {0}, 0, INT_MAX, FLAGS},
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

extern "C"{

AVFilter ff_vf_vr_map = {
    "vr_map", // name
    NULL_IF_CONFIG_SMALL("VR Mapping"), // description
    NULL, // inputs
    NULL, // outputs
    &vr_map_class, // priv_class
    AVFILTER_FLAG_DYNAMIC_INPUTS | AVFILTER_FLAG_DYNAMIC_OUTPUTS, // flags
    init, // init
    NULL, // init_dict
    uninit, // uninit
    query_formats, // query_formats
    sizeof(VRMapContext), // priv_size
    NULL, // next
    NULL, // process_command
    NULL, // init_opaque
};

}
