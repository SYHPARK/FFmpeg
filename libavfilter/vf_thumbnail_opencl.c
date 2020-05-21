/*
 * Copyright (c) 2020 Wookhyun Han <wookhyunhan@gmail.com>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/common.h"
#include "libavutil/imgutils.h"
#include "libavutil/opt.h"

#include "avfilter.h"
#include "internal.h"
#include "opencl.h"
#include "opencl_source.h"
#include <time.h>

#define HIST_SIZE (3*256)
//#define TEST
//#define CPU_UTIL

double getMicroTimestamp(){
    long long ns;
    time_t s;  // Seconds
    struct timespec spec;

    clock_gettime(CLOCK_REALTIME, &spec);

    s  = spec.tv_sec;
    ns = spec.tv_nsec;

//    printf("Current time: %"PRIdMAX".%03lld seconds since the Epoch\n",
//           (intmax_t)s, ns);

    double _current_time=0;

    if(ns>99999999)
        _current_time = s + (double)ns/1000000000;
    else
        _current_time = s + (double)ns/100000000;
    return _current_time;
}

static const enum AVPixelFormat supported_formats[] = {
    AV_PIX_FMT_NV12,
    AV_PIX_FMT_YUV420P,
    AV_PIX_FMT_YUV444P,
    AV_PIX_FMT_P010,
    AV_PIX_FMT_P016,
    AV_PIX_FMT_YUV444P16,
};

static int is_format_supported(enum AVPixelFormat fmt)
{
    int i;

    for (i = 0; i < FF_ARRAY_ELEMS(supported_formats); i++)
        if (supported_formats[i] == fmt)
            return 1;
    return 0;
}

struct thumb_frame {
    AVFrame *buf;               ///< cached frame
    int histogram[HIST_SIZE];   ///< RGB color distribution histogram of the frame
};

typedef struct ThumbnailOpenCLContext {
    OpenCLFilterContext ocf;
    int                   initialised;

    int                   n;         ///< current frame
    int                   n_frames;  ///< number of frames for analysis
    struct thumb_frame   *frames;    ///< the n_frames frames
    AVRational            tb;        ///< copy of the input timebase to ease access

    cl_kernel             kernel_uchar;
    cl_kernel             kernel_uchar2;
    cl_mem                hist;
    cl_command_queue      command_queue;
} ThumbnailOpenCLContext;

static int thumbnail_opencl_init(AVFilterContext *avctx)
{
    ThumbnailOpenCLContext *ctx = avctx->priv;
    cl_int cle;
    int err;

    // Allocate frame cache.
    ctx->frames = av_calloc(ctx->n_frames, sizeof(*ctx->frames));
	  if (!ctx->frames) {
        av_log(avctx, AV_LOG_ERROR,
               "Allocation failure, try to lower the number of frames\n");
        return AVERROR(ENOMEM);
    }
    av_log(avctx, AV_LOG_VERBOSE, "batch size: %d frames\n", ctx->n_frames);

    // Load OpenCL program.
    err = ff_opencl_filter_load_program(avctx, &ff_opencl_source_thumbnail, 1);
    if (err < 0)
        goto fail;

    // Create command queue.
    ctx->command_queue = clCreateCommandQueue(ctx->ocf.hwctx->context,
                                              ctx->ocf.hwctx->device_id,
                                              0, &cle);
    printf("clCreateCommandQueue error code: %d\n\n", cle);
    CL_FAIL_ON_ERROR(AVERROR(EIO), "Failed to create OpenCL "
                     "command queue %d.\n", cle);

    // Create kernel.
    ctx->kernel_uchar = clCreateKernel(ctx->ocf.program, "Thumbnail_uchar", &cle);
    CL_FAIL_ON_ERROR(AVERROR(EIO), "Failed to create kernel %d.\n", cle);

    ctx->kernel_uchar2 = clCreateKernel(ctx->ocf.program, "Thumbnail_uchar2", &cle);
    CL_FAIL_ON_ERROR(AVERROR(EIO), "Failed to create kernel %d.\n", cle);

    ctx->hist = clCreateBuffer(ctx->ocf.hwctx->context, 0,
                               sizeof(int) * HIST_SIZE, NULL, &cle);
    CL_FAIL_ON_ERROR(AVERROR(EIO), "Failed to create hist buffer %d.\n", cle);

    ctx->initialised = 1;
    return 0;

fail:
    if (ctx->command_queue)
        clReleaseCommandQueue(ctx->command_queue);
    if (ctx->kernel_uchar)
        clReleaseKernel(ctx->kernel_uchar);
    if (ctx->kernel_uchar2)
        clReleaseKernel(ctx->kernel_uchar2);
    if (ctx->hist)
        clReleaseMemObject(ctx->hist);
    return err;
}

/**
 * @brief        Compute Sum-square deviation to estimate "closeness".
 * @param hist   color distribution histogram
 * @param median average color distribution histogram
 * @return       sum of squared errors
 */

static double frame_sum_square_err(const int *hist, const double *median)
{
    int i;
    double err, sum_sq_err = 0;

    for (i = 0; i < HIST_SIZE; i++) {
        err = median[i] - (double)hist[i];
        sum_sq_err += err*err;
    }
    return sum_sq_err;
}

static AVFrame *get_best_frame(AVFilterContext *ctx)
{
#ifdef TEST
    static int cnt = 0;
    double st = getMicroTimestamp();
    fprintf(stdout, "%d\t%lf\t", cnt, st); //time count
    fprintf(stdout, "%lf\t", 0);
    fprintf(stdout, "%s\t%s\n", __FUNCTION__, "start"); //time count
#endif
#ifdef CPU_UTIL
    double st = getMicroTimestamp();
    fprintf(stdout, "%lf\t", st); //time count
    fprintf(stdout, "\t");
    fprintf(stdout, "%s\t%s\n", __FUNCTION__, "start"); //time count
#endif
    AVFrame *picref;
    ThumbnailOpenCLContext *s = ctx->priv;
    int i, j, best_frame_idx = 0;
    int nb_frames = s->n;
    double avg_hist[HIST_SIZE] = {0}, sq_err, min_sq_err = -1;

    // average histogram of the N frames
    for (j = 0; j < FF_ARRAY_ELEMS(avg_hist); j++) {
        for (i = 0; i < nb_frames; i++){
            avg_hist[j] += (double)s->frames[i].histogram[j];
	   // printf("%lf\t", (double)s->frames[i].histogram[j]);
	}
	//printf("\n");
        avg_hist[j] /= nb_frames;
    }

    // find the frame closer to the average using the sum of squared errors
    for (i = 0; i < nb_frames; i++) {
        sq_err = frame_sum_square_err(s->frames[i].histogram, avg_hist);
        if (i == 0 || sq_err < min_sq_err)
            best_frame_idx = i, min_sq_err = sq_err;
    }

    // free and reset everything (except the best frame buffer)
    for (i = 0; i < nb_frames; i++) {
        memset(s->frames[i].histogram, 0, sizeof(s->frames[i].histogram));
        if (i != best_frame_idx)
            av_frame_free(&s->frames[i].buf);
    }
    s->n = 0;

    // raise the chosen one
    picref = s->frames[best_frame_idx].buf;
    av_log(ctx, AV_LOG_INFO, "frame id #%d (pts_time=%f) selected "
           "from a set of %d images\n", best_frame_idx,
           picref->pts * av_q2d(s->tb), nb_frames);
    s->frames[best_frame_idx].buf = NULL;
#ifdef TEST
    double dt = getMicroTimestamp();
    fprintf(stdout, "%d\t%lf\t%lf\t", cnt, dt, dt-st); //time count
    fprintf(stdout, "%s\t%s\n", __FUNCTION__, "end"); //time count
    cnt++;
#endif
#ifdef CUP_UTIL
    double dt = getMicroTimestamp();
    fprintf(stdout, "%lf\t\t", dt); //time count
    fprintf(stdout, "%s\t%s\n", __FUNCTION__, "end"); //time count
#endif
    return picref;
}

static int thumbnail_kernel(AVFilterContext *avctx, AVFrame *in, cl_kernel kernel, cl_int offset, int pixel)
{
    int err;
    cl_int cle;
    ThumbnailOpenCLContext *ctx = avctx->priv;
    size_t global_work[2];
    cl_mem src = (cl_mem)in->data[pixel];		//data[1]? data[2]?

    err = ff_opencl_filter_work_size_from_image(avctx, global_work, in, pixel, 0);
//    printf("global work = {%d, %d}\n", global_work[0], global_work[1]);
    if (err < 0)
        return err;

    size_t memsize;
    cle = clGetMemObjectInfo(src, CL_MEM_SIZE, sizeof(size_t), &memsize, NULL);
    size_t width, height;
    if(!pixel){
	width = (in->width) >> 1;
	height = (in->height) >> 1;
    }
    global_work[0] = width;
    global_work[1] = height;

    CL_SET_KERNEL_ARG(kernel, 0, cl_int, &offset);
    CL_SET_KERNEL_ARG(kernel, 1, cl_int, &width);
    CL_SET_KERNEL_ARG(kernel, 2, cl_int, &height);
    CL_SET_KERNEL_ARG(kernel, 3, cl_mem, &ctx->hist);
    CL_SET_KERNEL_ARG(kernel, 4, cl_mem, &src);

#ifdef CPU_UTIL
    double st = getMicroTimestamp();
    fprintf(stdout, "%lf\t", st); //time count
    fprintf(stdout, "\t");
    fprintf(stdout, "%s\t%s\n", __FUNCTION__, "start"); //time count
#endif
    int gwsize=global_work[0];
//    fprintf(stdout, "Opencl Kernel start: %lf\n", getMicroTimestamp());
    cle = clEnqueueNDRangeKernel(ctx->command_queue, kernel, 1, NULL,		//yongbak
				&gwsize, NULL, 0, NULL, NULL);
//    fprintf(stdout, "Opencl Kernel end: %lf\n", getMicroTimestamp());
    //printf("clEnqueueNDRangeKernel Error message: %d\n\n", cle);
#ifdef CPU_UTIL
    double dt = getMicroTimestamp();
    fprintf(stdout, "%lf\t\t", dt); //time count
    fprintf(stdout, "%s\t%s\n", __FUNCTION__, "end"); //time count
#endif

    CL_FAIL_ON_ERROR(AVERROR(EIO), "Failed to enqueue kernel: %d.\n", cle);
    return 0;

fail:
    clFinish(ctx->command_queue);
    return err;
}

static int thumbnail_opencl_filter_frame(AVFilterLink *inlink, AVFrame *input)
{
printf("[*] input frame addr %p\n", input);
//printf("REACH HERE\n");
//    const uint8_t* ptr = input->data[0];
//    printf("RED\tGREEN\tBLUE\n");
//    for(int i=0; i< (inlink->w); i+=3)
//	printf("%d\t%d\t%d\n", ptr[i], ptr[i+1], ptr[i+2]);
	printf("[*] key_frame: %d\n", input->key_frame);
#ifdef TEST
    static int cnt = 0;
    double st = getMicroTimestamp();
//    if(ctx->n == 99)
//	ctx->n = 0;
    fprintf(stdout, "%d\t%lf\t%lf\t", cnt, st, 0);
    fprintf(stdout, "%s\t%s\n", __FUNCTION__, "start");
#endif
#ifdef CPU_UTIL
    double st = getMicroTimestamp();
    fprintf(stdout, "%lf\t", st); //time count
    fprintf(stdout, "\t");
    fprintf(stdout, "%s\t%s\n", __FUNCTION__, "start"); //time count
#endif
    AVFilterContext    *avctx = inlink->dst;
    AVFilterLink     *outlink = avctx->outputs[0];
    ThumbnailOpenCLContext *ctx = avctx->priv;
    AVHWFramesContext *input_frames_ctx;
    AVFrame *output = NULL;
    AVFrame *best = NULL;
    input->is_best_frame = 0;
    int err, p;
    cl_int cle;
    cl_mem src, dst;
    size_t origin[3] = {0, 0, 0};
    size_t region[3] = {0, 0, 1};
    int *hist = NULL;
    enum AVPixelFormat in_format;

    av_log(ctx, AV_LOG_DEBUG, "Filter input: %s, %ux%u (%"PRId64") [%d] frame\n",
           av_get_pix_fmt_name(input->format),
           input->width, input->height, input->pts, ctx->n);

    if (!input->hw_frames_ctx)
        return AVERROR(EINVAL);

    input_frames_ctx = (AVHWFramesContext*)input->hw_frames_ctx->data;
    in_format = input_frames_ctx->sw_format;
    if (!ctx->initialised) {
        if (!is_format_supported(in_format)) {
            err = AVERROR(EINVAL);
            av_log(avctx, AV_LOG_ERROR, "input format %s not supported\n",
                   av_get_pix_fmt_name(in_format));
            goto fail;
        }
        err = thumbnail_opencl_init(avctx);
        if (err < 0)
            goto fail;
    }

    // keep a reference of each frame
    ctx->frames[ctx->n].buf = input;
    hist = ctx->frames[ctx->n].histogram;
//////
//    static int cnt = 0;
//    double st = getMicroTimestamp();
//    fprintf(stdout, "%d\t%lf\t", cnt, st); //time count
//    fprintf(stdout, "%lf\t", 0);
//    fprintf(stdout, "%s\t%s\n", "clEnqueueWriteBuffer", "start"); //time count

    // update current frame to histogram
    cle = clEnqueueWriteBuffer(ctx->command_queue, ctx->hist, CL_FALSE,
                               0, sizeof(int) * HIST_SIZE, hist, 0, NULL, NULL);

//    double dt = getMicroTimestamp();
//    fprintf(stdout, "%d\t%lf\t", cnt, st); //time count
//    fprintf(stdout, "%lf\t", dt-st);
//    fprintf(stdout, "%s\t%s\n", "clEnqueueWriteBuffer", "end"); //time count
//////
    CL_FAIL_ON_ERROR(AVERROR(EIO), "Failed to initialize hist buffer %d.\n", cle);
    fprintf(stdout, "%s:%d\n", __FUNCTION__, __LINE__);
    switch(input_frames_ctx->sw_format) {
        case AV_PIX_FMT_NV12:
        case AV_PIX_FMT_P010LE:
        case AV_PIX_FMT_P016LE:
            err = thumbnail_kernel(avctx, input, ctx->kernel_uchar, 0, 0);
            err |= thumbnail_kernel(avctx, input, ctx->kernel_uchar2, 256, 1);
            if (err < 0)
                goto fail;
            break;
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUV444P:
        case AV_PIX_FMT_YUV444P16:
            err = thumbnail_kernel(avctx, input, ctx->kernel_uchar, 0, 0);
            err |= thumbnail_kernel(avctx, input, ctx->kernel_uchar, 256, 1);
            err |= thumbnail_kernel(avctx, input, ctx->kernel_uchar, 512, 2);
            if (err < 0)
                goto fail;
            break;
        default:
            return AVERROR_BUG;
    }
//////
//    st = getMicroTimestamp();
//    fprintf(stdout, "%d\t%lf\t", cnt, st); //time count
//    fprintf(stdout, "%lf\t", 0);
//    fprintf(stdout, "%s\t%s\n", "clEnqueueReadBuffer", "start"); //time count

    cle = clEnqueueReadBuffer(ctx->command_queue, ctx->hist, CL_FALSE,
                              0, sizeof(int) * HIST_SIZE, hist, 0, NULL, NULL);

//    dt = getMicroTimestamp();
//    fprintf(stdout, "%d\t%lf\t", cnt++, st); //time count
//    fprintf(stdout, "%lf\t", dt-st);
//    fprintf(stdout, "%s\t%s\n", "clEnqueueReadBuffer", "end"); //time count
//////


    CL_FAIL_ON_ERROR(AVERROR(EIO), "Failed to read hist buffer: %d.\n", cle);

    cle = clFinish(ctx->command_queue);
    CL_FAIL_ON_ERROR(AVERROR(EIO), "Failed to finish command queue: %d.\n", cle);

    // no selection until the buffer of N frames is filled up
#ifdef TEST
    double dt = getMicroTimestamp();
    fprintf(stdout, "%d\t%lf\t%lf\t%s\t%s\n", cnt, dt, dt-st, __FUNCTION__, "end");
    cnt++;
#endif
#ifdef CPU_UTIL
    double dt = getMicroTimestamp();
    fprintf(stdout, "%lf\t\t", dt); //time count
    fprintf(stdout, "%s\t%s\n", __FUNCTION__, "end"); //time count
#endif
    fprintf(stdout, "%s:%d\n", __FUNCTION__, __LINE__);
    ctx->n++;
    if (ctx->n < ctx->n_frames){
//	ctx->n = 0;
        return 0;
    }


    fprintf(stdout, "%s:%d\n", __FUNCTION__, __LINE__);
    output = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!output) {
        err = AVERROR(ENOMEM);
        goto fail;
    }
    // Get best frame (Thumbnail).
    fprintf(stdout, "%s:%d\n", __FUNCTION__, __LINE__);
    best = get_best_frame(avctx);
    input->is_best_frame = 1;

    // Copy the best frame to output.
    for (p = 0; p < FF_ARRAY_ELEMS(output->data); p++) {
        src = (cl_mem)best->data[p];
        dst = (cl_mem)output->data[p];
        if (!dst)
            break;
        err = ff_opencl_filter_work_size_from_image(avctx, region, output, p, 0);
        if (err < 0)
            goto fail;

        cle = clEnqueueCopyBuffer(ctx->command_queue, src, dst, 0, 0, output->width * output->height, 0, NULL, NULL);
//	printf("clEnqueueCopyBuffer success %d\n", cle);
//        cle = clEnqueueCopyImage(ctx->command_queue, src, dst,
//                                 origin, origin, region, 0, NULL, NULL);
//        CL_FAIL_ON_ERROR(AVERROR(EIO), "Failed to copy plane %d: %d.\n", p, cle);

    }
    cle = clFinish(ctx->command_queue);
    CL_FAIL_ON_ERROR(AVERROR(EIO), "Failed to finish command queue %d.\n", cle);

    err = av_frame_copy_props(output, best);
    if (err < 0)
        goto fail;
    av_log(ctx, AV_LOG_DEBUG, "Filter output: %s, %ux%u (%"PRId64").\n",
           av_get_pix_fmt_name(output->format),
           output->width, output->height, output->pts);
    return ff_filter_frame(outlink, output);

fail:
    clFinish(ctx->command_queue);
    av_frame_free(&output);
    return err;
}

static av_cold void thumbnail_opencl_uninit(AVFilterContext *avctx)
{
    ThumbnailOpenCLContext *ctx = avctx->priv;
    cl_int cle;
    int i;

    if (ctx->kernel_uchar) {
        cle = clReleaseKernel(ctx->kernel_uchar);
        if (cle != CL_SUCCESS)
            av_log(avctx, AV_LOG_ERROR, "Failed to release "
                   "kernel: %d.\n", cle);
    }

    if (ctx->kernel_uchar2) {
        cle = clReleaseKernel(ctx->kernel_uchar2);
        if (cle != CL_SUCCESS)
            av_log(avctx, AV_LOG_ERROR, "Failed to release "
                   "kernel: %d.\n", cle);
    }

    if (ctx->command_queue) {
        cle = clReleaseCommandQueue(ctx->command_queue);
        if (cle != CL_SUCCESS)
            av_log(avctx, AV_LOG_ERROR, "Failed to release "
                   "command queue: %d.\n", cle);
    }

    if (ctx->hist) {
        cle = clReleaseMemObject(ctx->hist);
        if (cle != CL_SUCCESS)
            av_log(avctx, AV_LOG_ERROR, "Failed to release "
                   "buffer: %d.\n", cle);
    }

    for (i = 0; i < ctx->n_frames && ctx->frames && ctx->frames[i].buf; i++)
        av_frame_free(&ctx->frames[i].buf);
    av_freep(&ctx->frames);

    ff_opencl_filter_uninit(avctx);
}

static int config_props(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    ThumbnailOpenCLContext *s = ctx->priv;

    s->tb = inlink->time_base;

    return ff_opencl_filter_config_input(inlink);
}

static int request_frame(AVFilterLink *link)
{
    AVFilterContext *avctx = link->src;
    ThumbnailOpenCLContext *ctx = avctx->priv;
    AVFrame *best = NULL;
    AVFrame *output = NULL;
    int ret = ff_request_frame(avctx->inputs[0]);
    int err, p;
    cl_int cle;
    cl_mem src, dst;
    size_t origin[3] = {0, 0, 0};
    size_t region[3] = {0, 0, 1};

    if (ret == AVERROR_EOF && ctx->n) {
        // n_frames is larger than the total frame.
        output = ff_get_video_buffer(link, link->w, link->h);
        // Get best frame.
        best = get_best_frame(avctx);
        // Copy the best frame to output.
        for (p = 0; p < FF_ARRAY_ELEMS(output->data); p++) {
            src = (cl_mem)best->data[p];
            dst = (cl_mem)output->data[p];
            if (!dst)
                break;
            err = ff_opencl_filter_work_size_from_image(avctx, region, output, p, 0);
            if (err < 0)
                goto fail;

	    cle = clEnqueueCopyBuffer(ctx->command_queue, src, dst, 0, 0, region[0] * region[1], 0, NULL, NULL);
//            cle = clEnqueueCopyImage(ctx->command_queue, src, dst,
//                                     origin, origin, region, 0, NULL, NULL);
//            CL_FAIL_ON_ERROR(AVERROR(EIO), "Failed to copy plane %d: %d.\n", p, cle);
        }
        cle = clFinish(ctx->command_queue);
        CL_FAIL_ON_ERROR(AVERROR(EIO), "Failed to finish command queue %d.\n", cle);

        err = av_frame_copy_props(output, best);
        if (err < 0)
            goto fail;
        ret = ff_filter_frame(link, output);
        if (ret < 0)
            return ret;
        ret = AVERROR_EOF;
    }
    if (ret < 0)
        return ret;
    return 0;

fail:
    clFinish(ctx->command_queue);
    av_frame_free(&output);
    return err;
}

#define OFFSET(x) offsetof(ThumbnailOpenCLContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM | AV_OPT_FLAG_VIDEO_PARAM)
static const AVOption thumbnail_opencl_options[] = {
    { "n", "set the frames batch size", OFFSET(n_frames), AV_OPT_TYPE_INT, {.i64=100}, 2, INT_MAX, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(thumbnail_opencl);

static const AVFilterPad thumbnail_opencl_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_props,
        .filter_frame = &thumbnail_opencl_filter_frame,
    },
    { NULL }
};

static const AVFilterPad thumbnail_opencl_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .request_frame = request_frame,
        .config_props  = &ff_opencl_filter_config_output,
    },
    { NULL }
};

AVFilter ff_vf_thumbnail_opencl = {
    .name           = "thumbnail_opencl",
    .description    = NULL_IF_CONFIG_SMALL("Select the most representative frame in a given sequence of consecutive frames."),
    .priv_size      = sizeof(ThumbnailOpenCLContext),
    .priv_class     = &thumbnail_opencl_class,
    .init           = &ff_opencl_filter_init,
    .uninit         = &thumbnail_opencl_uninit,
    .query_formats  = &ff_opencl_filter_query_formats,
    .inputs         = thumbnail_opencl_inputs,
    .outputs        = thumbnail_opencl_outputs,
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
