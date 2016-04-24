/* 
* @Author: BlahGeek
* @Date:   2015-09-01
* @Last Modified by:   BlahGeek
* @Last Modified time: 2016-04-24
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

#include "opencv2/core/cuda.hpp"
#include "octvr.hpp"

typedef struct {
    const AVClass *avclass;

    int nb_inputs;
    char * output_templates;
    int blend;
    int enable_gain_compensator;

    int input_format;

    int crop_x, crop_w;
    int scale_ow, scale_oh;
    int preview_ow, preview_oh;

    int merge;
    int nb_outputs;
    int nb_outputs_merged; // = merge ? 1 : nb_outputs
    vr::MapperTemplate ** mapper_templates;

    vr::AsyncMultiMapper * async_remapper;

    cv::Size * in_sizes;
    AVFrame ** last_frames;

    struct FFBufQueue *queues;
} VRMapContext;

static int query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *formats = NULL;
    ff_add_format(&formats, AV_PIX_FMT_UYVY422);
    ff_add_format(&formats, AV_PIX_FMT_YUYV422);
    for(int i = 0 ; i < ctx->nb_inputs; i += 1) {
        if(ctx->inputs[i] && !ctx->inputs[i]->out_formats)
            ff_formats_ref(formats, &ctx->inputs[i]->out_formats);
    }

    AVFilterFormats *oformats = NULL;
    ff_add_format(&oformats, AV_PIX_FMT_UYVY422);
    for(int i = 0 ; i < ctx->nb_outputs ; i += 1) {
        if(ctx->outputs[i] && !ctx->outputs[i]->in_formats)
            ff_formats_ref(oformats, &ctx->outputs[i]->in_formats);
    }

    return 0;
}

static int push_frame(AVFilterContext * ctx) {
    VRMapContext *s = static_cast<VRMapContext *>(ctx->priv);

    bool inputs_eof = std::any_of(ctx->inputs, ctx->inputs + s->nb_inputs,
                                  [](AVFilterLink *l){ return l->closed; });
    bool queues_available = std::all_of(s->queues, s->queues + s->nb_inputs,
                                        [](struct FFBufQueue &q){ return q.available; });
    bool has_last_frame = s->last_frames != NULL;

    if(!queues_available && inputs_eof && !has_last_frame)
        return AVERROR_EOF;
    if(!queues_available && !inputs_eof)
        return 0;

    vr::Timer timer("FFMpeg Filter");

    std::vector<AVFrame *> frames(s->nb_inputs, NULL);
    std::vector<AVFrame *> out_frames(s->nb_outputs_merged, NULL);
    std::vector<cv::Mat> in_mats, out_mats;

    if(queues_available) {
        for(int i = 0 ; i < s->nb_inputs ; i += 1) {
            frames[i] = ff_bufqueue_get(&s->queues[i]);
            av_assert0(frames[i] != nullptr);

            auto & f = frames[i];
            int real_w = s->crop_w != 0 ? s->crop_w : f->width;
            av_assert0(real_w % 2 == 0 && s->crop_x % 2 == 0);
            in_mats.emplace_back(f->height, real_w,
                                 CV_8UC2, 
                                 f->data[0] + s->crop_x * 2, 
                                 f->linesize[0]);
        }
        timer.tick("Prepare inputs");

        std::vector<cv::Size> out_sizes;
        cv::Size merge_size = cv::Size(0, 0);
        for(int i = 0 ; i < s->nb_outputs ; i += 1) {
            cv::Size out_size = cv::Size(s->scale_ow, s->scale_oh);
            if(out_size.area() == 0)
                out_size = s->mapper_templates[i]->out_size;
            av_log(ctx, AV_LOG_INFO, "Output #%d: %dx%d (scaled from %dx%d)\n", i, 
                   out_size.width, out_size.height, 
                   s->mapper_templates[i]->out_size.width, s->mapper_templates[i]->out_size.height);
            out_sizes.push_back(out_size);
            merge_size.width = out_size.width;
            merge_size.height += out_size.height;
        }

        if(s->merge) {
            av_log(ctx, AV_LOG_INFO, "Merge output: %dx%d\n", merge_size.width, merge_size.height);
            out_frames[0] = ff_get_video_buffer(ctx->outputs[0], merge_size.width, merge_size.height);
            av_frame_copy_props(out_frames[0], frames[0]);

            int current_height = 0;
            for(int i = 0 ; i < s->nb_outputs ; i += 1) {
                out_mats.emplace_back(out_sizes[i], CV_8UC2,
                                      out_frames[0]->data[0] + out_frames[0]->linesize[0] * current_height,
                                      out_frames[0]->linesize[0]);
                current_height += out_sizes[i].height;
            }
        }
        else {
            for(int i = 0 ; i < s->nb_outputs ; i += 1) {
                out_frames[i] = ff_get_video_buffer(ctx->outputs[i], out_sizes[i].width, out_sizes[i].height);
                av_frame_copy_props(out_frames[i], frames[0]);

                out_mats.emplace_back(out_sizes[i], CV_8UC2,
                                      out_frames[i]->data[0], out_frames[i]->linesize[0]);
            }
        }

        timer.tick("Prepare outputs");
        s->async_remapper->push(in_mats, out_mats);
    }

    AVFrame ** real_out_frames = new AVFrame * [s->nb_outputs_merged];
    if(!has_last_frame) {
        for(int i = 0 ; i < s->nb_outputs_merged ; i += 1)
            real_out_frames[i] = NULL;
        s->last_frames = new AVFrame *[s->nb_inputs + s->nb_outputs_merged];
    } else {
        s->async_remapper->pop();
        for(int i = 0 ; i < s->nb_outputs_merged ; i += 1)
            real_out_frames[i] = s->last_frames[s->nb_inputs + i];
        for(int i = 0 ; i < s->nb_inputs ; i += 1)
            av_frame_free(&s->last_frames[i]);
        timer.tick("Pop last frames");
    }

    if(queues_available) {
        for(int i = 0 ; i < s->nb_inputs ; i += 1)
            s->last_frames[i] = frames[i];
        for(int i = 0 ; i < s->nb_outputs_merged ; i += 1)
            s->last_frames[s->nb_inputs + i] = out_frames[i];
    } else {
        delete [] s->last_frames;
        s->last_frames = NULL;
    }

    for(int i = 0 ; i < s->nb_outputs_merged ; i += 1)
        if(real_out_frames[i])
            ff_filter_frame(ctx->outputs[i], real_out_frames[i]);
    timer.tick("Do next filter");

    delete [] real_out_frames;

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
    if(s->crop_w != 0)
        av_log(ctx, AV_LOG_WARNING, "Using width %d for input %d\n", s->crop_w, in_no);
    s->in_sizes[in_no] = cv::Size(s->crop_w != 0 ? s->crop_w : inlink->w, inlink->h);

    if(s->input_format != 0 && s->input_format != inlink->format) {
        av_log(ctx, AV_LOG_ERROR, "Pixel formats for all inputs should be same.\n");
        return -1;
    }
    if(s->input_format == 0) {
        s->input_format = inlink->format;
        av_assert0(s->input_format == AV_PIX_FMT_UYVY422 || s->input_format == AV_PIX_FMT_YUYV422);
    }

    if(in_no == s->nb_inputs - 1) {
        std::vector<vr::MapperTemplate> _templates;
        std::vector<cv::Size> _scale_outputs;
        for(int i = 0 ; i < s->nb_outputs ; i += 1) {
            _templates.push_back(*s->mapper_templates[i]);
            cv::Size out_size = cv::Size(s->scale_ow, s->scale_oh);
            if(out_size.area() == 0)
                out_size = s->mapper_templates[i]->out_size;
            _scale_outputs.push_back(out_size);
        }
        std::vector<cv::Size> _sizes(s->in_sizes, s->in_sizes + s->nb_inputs);
        s->async_remapper = vr::AsyncMultiMapper::New(_templates, _sizes, 
                                                      s->blend, s->enable_gain_compensator, 
                                                      _scale_outputs,
                                                      cv::Size(s->preview_ow, s->preview_oh),
                                                      s->input_format == AV_PIX_FMT_UYVY422 ? AsyncMultiMapper::UYVY422 : AsyncMultiMapper::YUYV422);
        av_log(ctx, AV_LOG_INFO, "Init async remapper done\n");
    }

    return 0;
}

static int config_output(AVFilterLink *link)
{
    VRMapContext *s = static_cast<VRMapContext *>(link->src->priv);
    unsigned out_no = FF_OUTLINK_IDX(link);

    cv::Size out_size = cv::Size(s->scale_ow, s->scale_oh);
    if(out_size.area() == 0)
        out_size = s->mapper_templates[out_no]->out_size;

    link->w = out_size.width;
    link->h = out_size.height;
    return 0;
}

static int config_merge_output(AVFilterLink *link) {
    VRMapContext *s = static_cast<VRMapContext *>(link->src->priv);

    int width = -1;
    int total_height = 0;
    for(int i = 0 ; i < s->nb_outputs ; i += 1) {
        cv::Size out_size = cv::Size(s->scale_ow, s->scale_oh);
        if(out_size.area() == 0)
            out_size = s->mapper_templates[i]->out_size;
        if(width > 0 && out_size.width != width) {
            av_log(link->src, AV_LOG_ERROR, "Output width does not match while merge = 1\n");
            return AVERROR_INVALIDDATA;
        }
        width = out_size.width;
        total_height += out_size.height;
    }

    link->w = width;
    link->h = total_height;
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

static int init(AVFilterContext *ctx) {
    VRMapContext *s = static_cast<VRMapContext *>(ctx->priv);
    s->queues = static_cast<struct FFBufQueue *>(av_calloc(s->nb_inputs, sizeof(s->queues[0])));
    if(!s->queues)
        return AVERROR(ENOMEM);

    for(int i = 0 ; i < s->nb_inputs ; i += 1) {
        AVFilterPad inpad = { 0 };
        inpad.name = av_strdup(av_asprintf("input%d", i));
        inpad.type = AVMEDIA_TYPE_VIDEO;
        inpad.filter_frame = filter_frame;
        inpad.config_props = config_input;
        ff_insert_inpad(ctx, i, &inpad);
    }
    s->in_sizes = new cv::Size [s->nb_inputs];

    av_assert0(s->output_templates != NULL);
    std::string output_strings(s->output_templates);
    std::vector<int> output_strings_seq_pos;
    for(int i = 0 ; i < output_strings.size() ; i += 1) {
        if(output_strings[i] == '|')
            output_strings_seq_pos.push_back(i);
    }
    output_strings_seq_pos.push_back(output_strings.size());

    s->nb_outputs = output_strings_seq_pos.size();
    s->mapper_templates = new vr::MapperTemplate * [s->nb_outputs];
    for(int i = 0 ; i < output_strings_seq_pos.size() ; i += 1) {
        int start = i == 0 ? 0 : (output_strings_seq_pos[i-1] + 1);
        int stop = output_strings_seq_pos[i];
        std::string filename = output_strings.substr(start, stop-start);
        av_log(ctx, AV_LOG_INFO, "Loading template %s\n", filename.c_str());
        std::ifstream f(filename.c_str(), std::ios::binary);
        try {
            s->mapper_templates[i] = new vr::MapperTemplate(f);
        } catch (std::string & e) {
            av_log(ctx, AV_LOG_ERROR, "Error loading template: %s\n", e.c_str());
            return AVERROR_INVALIDDATA;
        }
        av_log(ctx, AV_LOG_INFO, "Load complete, size: %dx%d\n",
               s->mapper_templates[i]->out_size.width,
               s->mapper_templates[i]->out_size.height);
    }

    s->nb_outputs_merged = (s->merge ? 1 : s->nb_outputs);

    for(int i = 0 ; i < s->nb_outputs_merged ; i += 1) {
        AVFilterPad outpad = { 0 };
        outpad.name = av_strdup(av_asprintf("output%d", i));
        outpad.type = AVMEDIA_TYPE_VIDEO;
        outpad.request_frame = request_frame;
        outpad.config_props = (s->merge ? config_merge_output : config_output);
        ff_insert_outpad(ctx, i, &outpad);
    }

    if(s->merge) {
        s->scale_oh /= s->nb_outputs;
        av_log(ctx, AV_LOG_INFO, "Merge mode, split scale_oh to %d\n");
    }

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
    for(int i = 0 ; i < s->nb_outputs ; i += 1) {
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

#if defined( _MSC_VER )
/* MSVC++ does not support C99-style union initialization when compiling C++
 * Dirty Hack: int64_t 4607182418800017408 == double 1.0. PERFECT!
 */
static const AVOption vr_map_options[] = {
    { "inputs", "Number of input streams", OFFSET(nb_inputs), AV_OPT_TYPE_INT, {2}, 1, INT_MAX, FLAGS},
    { "outputs", "`|`-seperated output templates", OFFSET(output_templates), AV_OPT_TYPE_STRING, {0}, CHAR_MIN, CHAR_MAX, FLAGS},
    { "crop_x", "Crop X", OFFSET(crop_x), AV_OPT_TYPE_INT, {0}, 0, INT_MAX, FLAGS},
    { "crop_w", "Crop width", OFFSET(crop_w), AV_OPT_TYPE_INT, {0}, 0, INT_MAX, FLAGS},
    { "blend", "Blending param", OFFSET(blend), AV_OPT_TYPE_INT, {128}, INT_MIN, INT_MAX, FLAGS},
    { "scale_ow", "Scale output width", OFFSET(scale_ow), AV_OPT_TYPE_INT, {0}, INT_MIN, INT_MAX, FLAGS},
    { "scale_oh", "Scale output height", OFFSET(scale_oh), AV_OPT_TYPE_INT, {0}, INT_MIN, INT_MAX, FLAGS},
    { "preview_ow", "Preview output width (for QT only)", OFFSET(preview_ow), AV_OPT_TYPE_INT, {0}, 0, INT_MAX, FLAGS},
    { "preview_oh", "Preview output height (for QT only)", OFFSET(preview_oh), AV_OPT_TYPE_INT, {0}, 0, INT_MAX, FLAGS},
    { "enable_gain_compensator", "Enable gain compensator", OFFSET(enable_gain_compensator), AV_OPT_TYPE_INT, {1}, 0, 1, FLAGS},
    { "merge", "Merge multiple output (verticle)", OFFSET(merge), AV_OPT_TYPE_INT, {0}, 0, 1, FLAGS},
    { NULL }
};
#else
static const AVOption vr_map_options[] = {
    { "inputs", "Number of input streams", OFFSET(nb_inputs), AV_OPT_TYPE_INT, {.i64 = 2}, 1, INT_MAX, FLAGS},
    { "outputs", "`|`-seperated output templates", OFFSET(output_templates), AV_OPT_TYPE_STRING, {.str = NULL}, CHAR_MIN, CHAR_MAX, FLAGS},
    { "crop_x", "Crop X", OFFSET(crop_x), AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, FLAGS},
    { "crop_w", "Crop width", OFFSET(crop_w), AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, FLAGS},
    { "blend", "Blending param", OFFSET(blend), AV_OPT_TYPE_INT, {.i64 = 128}, INT_MIN, INT_MAX, FLAGS},
    { "scale_ow", "Scale output width", OFFSET(scale_ow), AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, FLAGS},
    { "scale_oh", "Scale output height", OFFSET(scale_oh), AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, FLAGS},
    { "preview_ow", "Preview output width (for QT only)", OFFSET(preview_ow), AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, FLAGS},
    { "preview_oh", "Preview output height (for QT only)", OFFSET(preview_oh), AV_OPT_TYPE_INT, {.i64 = 0}, 0, INT_MAX, FLAGS},
    { "enable_gain_compensator", "Enable gain compensator", OFFSET(enable_gain_compensator), AV_OPT_TYPE_INT, {.i64 = 1}, 0, 1, FLAGS},
    { "merge", "Merge multiple output (verticle)", OFFSET(merge), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1, FLAGS},
    { NULL }
};
#endif

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
