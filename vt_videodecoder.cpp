#include <time.h>
#include <sys/time.h>
#include <math.h>
#include <iostream>
#include <memory>
#include <sstream>
#include "vt_videodecoder.h"
#include "vt_videothumbnailer.h"

extern "C" {
#ifdef LATEST_GREATEST_FFMPEG
#include <libavutil/opt.h>
#endif
#include <libswscale/swscale.h>
#include <libavutil/display.h>
}

extern int gArgc;
extern char gArgv[32][100+1];
static AVDictionary *_OutputHeaderOpts = NULL;
static struct timeval tsFrom, tsTo;

// TIZEN CODE PARAMETERS
#define _MM_CHUNK_NUM           8                               /*FIXME*/
#define _MM_CHUNK_LIMIT         (_MM_CHUNK_NUM >> 1)
#define _MM_CHUNK_DIFF_LIMIT    ((_MM_CHUNK_LIMIT << 1) | 1)    /*FIXME*/
#define MIN_PTS_INTERVAL        (0.5)

// FRAME SEARCH PARAMETERS
#define _RETRY_SEARCH_LIMIT     150
#define _KEY_SEARCH_LIMIT       (_RETRY_SEARCH_LIMIT<<1)     /*2 = 1 read. some frame need to read one more*/
#define _FRAME_SEARCH_LIMIT     1000

#define DEFAULT_PERCENT		10

static unsigned int _diff_memory (const void *s1, const void *s2, unsigned int n)
{
    char *s = (char *)s1;
    char *d = (char *)s2;
    int i;
    int ret;
    int tmp;

    for (i = 0, ret = 0; i < n; i++) {
        if (*s++ != *d++) {
            tmp = (*s - *d);
            ret += (tmp < 0 ? -tmp : tmp);
        }
    }
    ret /= n;
    return ret;
}

static int _is_good_pgm (unsigned char *buf, int wrap, int xsize, int ysize)
{
    int i;
    int step;
    int point;
    unsigned char *cnt;     /*center line of image*/
    int is_different;
    unsigned int sum_diff;
    int cnt_offset;

    /*set center line*/
    step = ysize >> 3;
    cnt_offset = (ysize >> 1 );
    cnt = buf + cnt_offset * wrap;

    /*if too small, always ok return.*/
    if (ysize < _MM_CHUNK_NUM)
        return 1;

    for (point = 0, sum_diff = 0, i = step; i < ysize; i += step)
    {
        if (i != cnt_offset)
        {
            /*binary compare*/
            is_different = _diff_memory (cnt, buf + i * wrap, xsize);
            point += (is_different != 0);
            sum_diff += is_different;

            if (point >= _MM_CHUNK_LIMIT)
            {
                if (sum_diff > _MM_CHUNK_DIFF_LIMIT)
                    return 1;
                else
                    return 0;
            }
        }
    }
    return 0;
}

// ==========================
// VT_VideoDecoder Definision
// ==========================
int VT_VideoDecoder::initializeVideoFilters(VT_RequestInfo& requestInfo)
{
    // buffer -> (transpose) -> (yadif) -> scale -> crop -> format -> buffersink
    char args[512] = "";
    int ret = 0;
    float   srcARatio = 0.0f, dstARatio = 0.0f;
    bool fixWidth = true;

    srcARatio = (float)requestInfo.m_originalWidth/requestInfo.m_originalHeight;
    dstARatio = (float)requestInfo.m_requestWidth/requestInfo.m_requestHeight;
    writeLog( VT_LOG_DBG, "[DBG] [%s:%d] srcARatio : %f, dstARatio : %f\n", __func__, __LINE__, srcARatio, dstARatio );

    if (srcARatio > dstARatio)
        fixWidth = false;

    avfilter_register_all();

    AVFilterContext *lastFilter = NULL;
    AVRational time_base = {1,1};
    AVCodecContext *dec_ctx = m_pVideoCodecContext;
    if(m_pFormatContext)
    {
        time_base = m_pFormatContext->streams[m_VideoStream]->time_base;
    }

    m_filter_graph = avfilter_graph_alloc();
    if (!m_filter_graph) {
        writeLog( VT_LOG_ERR, "[ERR] [%s:%d]\n", __func__, __LINE__);
        ret = AVERROR(ENOMEM);
        return -1;
    }

    // Some recording file, there's no information below.
    // If it's invalid value, set default(1).
    if(dec_ctx->sample_aspect_ratio.num <= 0)
        dec_ctx->sample_aspect_ratio.num = 1;
    if(dec_ctx->sample_aspect_ratio.den <= 0)
        dec_ctx->sample_aspect_ratio.den = 1;
    if(time_base.num <= 0)
        time_base.num = 1;
    if(time_base.den <= 0)
        time_base.den = 1;

    /* buffer video source: the decoded frames from the decoder will be inserted here. */
    snprintf(args, sizeof(args),
            "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
            requestInfo.m_originalWidth, requestInfo.m_originalHeight,dec_ctx->pix_fmt,
            time_base.num, time_base.den,
            dec_ctx->sample_aspect_ratio.num, dec_ctx->sample_aspect_ratio.den );

    writeLog( VT_LOG_DBG, "[DBG] [%s:%d] args=%s\n", __func__, __LINE__, args );
    ret = avfilter_graph_create_filter(&m_buffersrc_ctx, avfilter_get_by_name("buffer"), "tn_in",
            args, NULL, m_filter_graph);
    if (ret < 0) {
        writeLog( VT_LOG_ERR, "[ERR] [%s:%d] Cannot create buffer source/ret:%d\n", __func__, __LINE__, ret );
        return ret;
    }

     // << buffer
    /* buffer video sink: to terminate the filter chain. */
    ret = avfilter_graph_create_filter(&m_buffersink_ctx, avfilter_get_by_name("buffersink"), "tn_out",
            NULL, NULL, m_filter_graph);
    if (ret < 0) {
        writeLog( VT_LOG_ERR, "[ERR] [%s:%d] Cannot create buffer sink\n", __func__, __LINE__ );
        return ret;
    }
    lastFilter = m_buffersink_ctx;
    // << buffersink    // last(buffersink)

    // -----------------------------------------------------

    // Format Filter
    AVFilterContext* formatFilter = NULL;
    snprintf(args, sizeof(args), "%s", requestInfo.m_pix_fmt_string.c_str() );
    ret = avfilter_graph_create_filter(&formatFilter, avfilter_get_by_name("format"), "tn_format",
            args, NULL, m_filter_graph);
    if (ret < 0) {
        writeLog( VT_LOG_ERR, "[ERR] [%s:%d] fail to create filter\n", __func__, __LINE__ );
        return ret;
    }
    ret = avfilter_link(formatFilter, 0, lastFilter, 0);
    if ( ret < 0 ) {
        writeLog( VT_LOG_ERR, "[ERR] [%s:%d] fail to link filter (ret:%d)\n", __func__, __LINE__, ret );
        return ret;
    }
    lastFilter = formatFilter;
    // << format

    // Crop Filter  :
    AVFilterContext* cropFilter = NULL;
    snprintf(args, sizeof(args), "%d:%d", requestInfo.m_requestWidth, requestInfo.m_requestHeight );
    writeLog( VT_LOG_DBG, "[DBG] [%s:%d] args : %s\n", __func__, __LINE__, args );
    ret = avfilter_graph_create_filter(&cropFilter, avfilter_get_by_name("crop"), "tn_crop",
            args, NULL, m_filter_graph);
    if (ret < 0) {
        writeLog( VT_LOG_ERR, "[ERR] [%s:%d] fail to create filter\n", __func__, __LINE__ );
        return ret;
    }
    ret = avfilter_link(cropFilter, 0, lastFilter, 0);
    if ( ret < 0 ) {
        writeLog( VT_LOG_ERR, "[ERR] [%s:%d] fail to link filter\n", __func__, __LINE__ );
        return ret;
    }
    lastFilter = cropFilter;

    // Scale Filter :
    AVFilterContext* scaleFilter = NULL;
    if ( fixWidth )
        snprintf(args, sizeof(args), "%d:-1", requestInfo.m_requestWidth);
    else
        snprintf(args, sizeof(args), "-1:%d", requestInfo.m_requestHeight );
    writeLog( VT_LOG_DBG, "[DBG] [%s:%d] args : %s\n", __func__, __LINE__, args );
    ret = avfilter_graph_create_filter(&scaleFilter, avfilter_get_by_name("scale"), "tn_scale",
            args, NULL, m_filter_graph);
    if (ret < 0) {
        writeLog( VT_LOG_ERR, "[ERR] [%s:%d] fail to create filter\n", __func__, __LINE__ );
        return ret;
    }
    ret = avfilter_link(scaleFilter, 0, lastFilter, 0 );
    if ( ret < 0 ) {
        writeLog( VT_LOG_ERR, "[ERR] [%s:%d] fail to link filter (ret:%d)\n", __func__, __LINE__, ret );
        return ret;
    }
    lastFilter = scaleFilter;

    /*
    // Motion thumbnail doesn't support fps.
    AVFilterContext* fpsFilter = NULL;
    snprintf(args, sizeof(args), "fps=1/2" );
    ret = avfilter_graph_create_filter(&fpsFilter, avfilter_get_by_name("fps"), "tn_fps",
            args, NULL, m_filter_graph);
    if (ret < 0) {
        writeLog( VT_LOG_ERR, "[ERR] [%s:%d] fail to create filter\n", __func__, __LINE__ );
        return ret;
    }
    ret = avfilter_link(fpsFilter, 0, lastFilter, 0);
    if ( ret < 0 ) {
        writeLog( VT_LOG_ERR, "[ERR] [%s:%d] fail to link filter (ret:%d)\n", __func__, __LINE__, ret );
        return ret;
    }
    lastFilter = fpsFilter;
    */
    // << fps

    #if 0   // left/right pic cannot export thumbnail
    // CONDITIONAL : yadif Filter
    if (m_pFrame->interlaced_frame != 0)
    {
        writeLog( VT_LOG_DBG, "[DBG] [%s:%d] interlaced_frame!\n", __func__, __LINE__ );
        AVFilterContext* yadifFilter = NULL;
        avfilter_graph_create_filter(&yadifFilter, avfilter_get_by_name("yadif"), "tn_deint",
                "deint=1", NULL, m_filter_graph);

        if (ret < 0) {
            writeLog( VT_LOG_ERR, "[ERR] [%s:%d] fail to create filter\n", __func__, __LINE__ );
            return ret;
        }
        ret = avfilter_link(yadifFilter, 0, lastFilter, 0);
        if ( ret < 0 ) {
            writeLog( VT_LOG_ERR, "[ERR] [%s:%d] fail to link filter (ret:%d)\n", __func__, __LINE__, ret );
            return ret;
        }
        lastFilter = yadifFilter;
    }
    #endif

    // CONDITIONAL : rotate Filter
/*    int rotation = getStreamRotation();
    if ( rotation != -1 )
    {
        writeLog( VT_LOG_DBG, "[DBG] [%s:%d] rotation : %d\n", __func__, __LINE__, rotation );
        AVFilterContext* rotateFilter = NULL;
        snprintf(args, sizeof(args), "%d", rotation );
        avfilter_graph_create_filter(&rotateFilter, avfilter_get_by_name("transpose"), "tn_rotate",
                args, NULL, m_filter_graph);

        if (ret < 0) {
            writeLog( VT_LOG_ERR, "[ERR] [%s:%d] fail to create filter\n", __func__, __LINE__ );
            return ret;
        }
        ret = avfilter_link(rotateFilter, 0, lastFilter, 0);
        if ( ret < 0 ) {
            writeLog( VT_LOG_ERR, "[ERR] [%s:%d] fail to link filter (ret:%d)\n", __func__, __LINE__, ret );
            return ret;
        }
        lastFilter = rotateFilter;
    }
*/
    ret = avfilter_link(m_buffersrc_ctx, 0, lastFilter, 0);
    if ( ret < 0 ) {
        writeLog( VT_LOG_ERR, "[ERR] [%s:%d] fail to link filter (ret:%d)\n", __func__, __LINE__, ret );
        return ret;
    }

    ret = avfilter_graph_config(m_filter_graph, NULL);
    if ( ret < 0 ) {
        writeLog( VT_LOG_ERR, "[ERR] [%s:%d] fail to graph config\n", __func__, __LINE__ );
        return ret;
    }

    // -----------------------------------------------------
    writeLog( VT_LOG_DBG, "[DBG] [%s:%d] DONE\n", __func__, __LINE__);
    return ret;
}

bool VT_VideoDecoder::applyVideoFilters(VT_RequestInfo &requestInfo)
{
    int ret = 0;

    AVFrame *res = av_frame_alloc();
    if (NULL == res) {
        writeLog( VT_LOG_ERR, "[ERR] [%s:%d] Alloc frame error\n", __func__, __LINE__ );
        goto errCond;
    }
    /* push the decoded frame into the filtergraph */
    if (av_buffersrc_write_frame(m_buffersrc_ctx, m_pFrame) < 0) {
        writeLog( VT_LOG_ERR, "[ERR] [%s:%d] Error while feeding the filtergraph\n", __func__, __LINE__ );
        goto errCond;
    }

    /* pull filtered frames from the filtergraph */
    ret = av_buffersink_get_frame(m_buffersink_ctx, res);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
    {
        writeLog( VT_LOG_ERR, "[ERR] [%s:%d]\n", __func__, __LINE__);
        goto errCond;
    }
    if (ret < 0)
    {
        writeLog( VT_LOG_ERR, "[ERR] [%s:%d]\n", __func__, __LINE__);
        goto errCond;
    }

    requestInfo.m_scaledWidth = res->width;
    requestInfo.m_scaledHeight = res->height;
    writeLog( VT_LOG_DBG, "[DBG] [%s:%d] res= %dx%d:%d -> scaledRes= %dx%d\n", __func__, __LINE__, \
            res->width, res->height, res->format, \
            requestInfo.m_scaledWidth, requestInfo.m_scaledHeight);

    av_frame_free(&m_pFrame);
    m_pFrame = res;

    return true;

errCond:
    if(res)
        av_frame_free(&res);
    writeLog( VT_LOG_ERR, "[ERR] [%s:%d] av_buffersink_get_frame return error(%d)\n", \
            __func__, __LINE__, ret );
    return false;
}

//------------------------------

VT_VideoDecoder::VT_VideoDecoder()
{
	m_VideoStream = -1;
	m_pFormatContext	= NULL;
	m_pVideoCodecContext = NULL;
    m_pOutFormatContext = NULL;
    m_pOutCodec = NULL;
    m_pOutCodecContext = NULL;
    m_pOutStream = NULL;
	m_pVideoCodec= NULL;
	m_pVideoStream = NULL;
	m_pFrame = NULL;
	m_pPacket = NULL;

    m_buffersrc_ctx = NULL;
    m_buffersink_ctx = NULL;
    m_filter_graph = NULL;

    m_isEncoderOpened = false;
    m_encFrameCount = 0;
}

VT_VideoDecoder::~VT_VideoDecoder()
{
	destroy();
}

bool VT_VideoDecoder::open(VT_RequestInfo& requestInfo)
{
	// Note : Registers all available file formats and codecs with the library so they will be used automatically when a file with the corresponding format/codec is opened
	av_register_all();              // consume 10 msec

	// Note : Reads the file header and stores information about the file format in the AVFormatContext structure. This function only looks at the header

	if (avformat_open_input(&m_pFormatContext, requestInfo.m_videoPath.c_str(), NULL, NULL) != 0)
	{
		requestInfo.m_errorMsg = "Could not open input file";
		return false;
	}

	// Note : Find the first video stream
	for (unsigned int i = 0; i < m_pFormatContext->nb_streams; ++i)
	{
		if (m_pFormatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
		{
			requestInfo.m_originalWidth = m_pFormatContext->streams[i]->codecpar->width;
			requestInfo.m_originalHeight = m_pFormatContext->streams[i]->codecpar->height;
            if ( requestInfo.m_requestWidth == 0 )
                requestInfo.m_requestWidth = requestInfo.m_originalWidth;
            if ( requestInfo.m_requestHeight == 0 )
                requestInfo.m_requestHeight = requestInfo.m_originalHeight;

			AVCodecDescriptor* pCodecDescriptor;
			if((pCodecDescriptor = (AVCodecDescriptor*)avcodec_descriptor_get(m_pFormatContext->streams[i]->codecpar->codec_id))  != NULL)
				requestInfo.m_codec = pCodecDescriptor->name;
			break;
		}
	}

	return true;
}

bool VT_VideoDecoder::decodeThumbnail(VT_RequestInfo& requestInfo)
{
    AVFrame *frame = NULL;
    AVFrame *tmp_frame = NULL;
    AVFrame *first_frame = NULL;
    AVFormatContext *pFormatCtx = m_pFormatContext;
    AVCodecContext *pCodecCtx = m_pVideoCodecContext;

    bool bret;
    int ret;
    int found = 0;
    int i,v;
    int retry = 0;
    int key_detected = 0;
    struct timeval      now;

    first_frame = av_frame_alloc();
    tmp_frame = av_frame_alloc();
    m_pPacket = av_packet_alloc();

    if (!first_frame || !tmp_frame) {
        if (first_frame) av_frame_free (&first_frame);
        if (tmp_frame) av_frame_free (&tmp_frame);
        return false;
    }

    pCodecCtx->skip_frame = AVDISCARD_BIDIR;

    for(i = 0, v = 0, frame = first_frame; i < _KEY_SEARCH_LIMIT && v < _FRAME_SEARCH_LIMIT;)
    {
        gettimeofday( &now, NULL );
        if ( now.tv_sec > requestInfo.m_dueTime.tv_sec ||
                (now.tv_sec == requestInfo.m_dueTime.tv_sec && now.tv_usec >= requestInfo.m_dueTime.tv_usec ) )
        {
            writeLog( VT_LOG_INFO, "[INFO] [%s:%d] timeout(%d) expired!\n",
                    __func__, __LINE__,  requestInfo.m_timeout );
            break;
        }

        // get packet
        ret = getPacket( m_pPacket, pFormatCtx, &key_detected, &i);
        if ( ret == RETURN_BREAK    ) break;
        if ( ret == RETURN_CONTINUE ) continue;

        // get frame
        ret = getFrame( m_pPacket, pCodecCtx, frame, &key_detected, &v);
        if (frame->width == 0 || frame->height == 0)
          continue;

        if (frame->width != 0 && frame->height != 0)
        {
            requestInfo.m_originalWidth = frame->width;
            requestInfo.m_originalHeight = frame->height;
        }
        writeLog( VT_LOG_CHK, "[CHK] [%s:%d] pkt(size:%d, pos:%lld), ret %d, pts(%lld), ptype(%d), key(%d)\n", \
                __func__, __LINE__, m_pPacket->size, m_pPacket->pos, ret, \
                frame->pts, frame->pict_type, frame->key_frame );
        // UHD record thumbnail
        if ( requestInfo.m_isRecord && key_detected == 1 && ret == RETURN_CONTINUE )
        {
            // This code is for latency frame.
            // UHD recordings(mp4) always have latency frame.
            // If use below code in normal video file,
            // you should re-load file to decode next frame.
            av_packet_unref (m_pPacket);
            ret = getFrame( m_pPacket, pCodecCtx, frame, &key_detected, &v);
            if (frame->width != 0 && frame->height != 0)
            {
              requestInfo.m_originalWidth = frame->width;
              requestInfo.m_originalHeight = frame->height;
            }
            writeLog( VT_LOG_CHK, "[CHK] [%s:%d] decode last frame. pkt(size:%d, pos:%lld), ret %d, pts(%lld), ptype(%d), key(%d)\n", \
                     __func__, __LINE__, m_pPacket->size, m_pPacket->pos, ret, \
                     frame->pts, frame->pict_type, frame->key_frame );
        }

        if ( requestInfo.m_requestWidth == 0 )
          requestInfo.m_requestWidth = frame->width;
        if ( requestInfo.m_requestHeight == 0 )
          requestInfo.m_requestHeight = frame->height;
        if ( ret == RETURN_BREAK    ) break;
        if ( ret == RETURN_CONTINUE ) continue;
        key_detected = 0;

        if ( frame->pict_type != AV_PICTURE_TYPE_I && frame->key_frame == 0)
        {
            continue;
        }

        // got_picture
        found++;
        ret = _is_good_pgm (frame->data[0], frame->linesize[0],
            requestInfo.m_originalWidth, requestInfo.m_originalHeight);
        if ( ret )
            break;
        writeLog( VT_LOG_CHK, "[CHK] [%s:%d] cont. by is_good_pgm()\n", __func__, __LINE__ );

        /*reset video frame count & retry searching*/
        i = 0;
        v = 0;
        retry++;

        /*set buffer frame*/
        frame = tmp_frame;

        /*limit of retry.*/
        if (retry > _RETRY_SEARCH_LIMIT)
            break;
    }

    /*free m_pPacket after loop breaking*/
    av_packet_free(&m_pPacket);

    /*set decode frame to output*/
    if (found > 0) {
        bret = true;
        if (retry == 0 || found == retry) {
            m_pFrame = first_frame;
            if (tmp_frame) av_frame_free (&tmp_frame);
        }
        else {
            m_pFrame = tmp_frame;
            if (first_frame) av_frame_free (&first_frame);
        }
        writeLog( VT_LOG_DBG, "[DBG] [%s:%d] SEL FRAME = pict_type(%d), pts(%lld), 0x%llx\n",
                __func__, __LINE__, m_pFrame->pict_type, m_pFrame->pts, m_pFrame->pts);
    }
    else {
        requestInfo.m_errorMsg = "Cannot found key-frame";
        bret = false;
        if (first_frame) av_frame_free (&first_frame);
        if (tmp_frame) av_frame_free (&tmp_frame);
    }

    pCodecCtx->skip_frame = AVDISCARD_NONE;
    return bret;
}

bool VT_VideoDecoder::decodeMotionThumbnail(VT_RequestInfo& requestInfo)
{
    AVFormatContext *pFormatCtx = m_pFormatContext;
    AVCodecContext *pCodecCtx = m_pVideoCodecContext;

    bool bret = true;
    int ret = 0;
    int found = 0;
    int i,v;
    int key_detected = 0;
    double pts_time         = 0.0;
    double pts_next_time    = 0.0;

    int64_t durationFor10Percent = 0LL;
    int64_t currentPosition = 0LL;
	int64_t	requestPosition = 0LL;
	int64_t	timestamp = 0LL;

    struct timeval      now;

    m_pFrame = av_frame_alloc();
    m_pPacket = av_packet_alloc();
    pCodecCtx->skip_frame = AVDISCARD_NONKEY;

    if ( requestInfo.m_seekTime )
    {
        durationFor10Percent = (int64_t)(m_pFormatContext->duration*DEFAULT_PERCENT/100);
        currentPosition = avio_tell( m_pFormatContext->pb );
        requestPosition = durationFor10Percent * 2; // Motion starts 20Percent
        writeLog( VT_LOG_DBG, "[DBG] [%s:%d] durationFor10Percent (%lld), curr(%lld), req(%lld)\n",
                __func__, __LINE__,  durationFor10Percent, currentPosition, requestPosition );
    }

    for (i=0, v=0; i<_KEY_SEARCH_LIMIT && v<_FRAME_SEARCH_LIMIT; )
    {
        // Check timeout
        gettimeofday( &now, NULL );
        if ( now.tv_sec > requestInfo.m_dueTime.tv_sec ||
                (now.tv_sec == requestInfo.m_dueTime.tv_sec && now.tv_usec >= requestInfo.m_dueTime.tv_usec ) )
        {
            writeLog( VT_LOG_INFO, "[INFO] [%s:%d] timeout(%d) expired!\n",
                    __func__, __LINE__,  requestInfo.m_timeout );
            break;
        }

#if 1
        if ( durationFor10Percent && ret == RETURN_OK )
        {
            if ( requestPosition > m_pFormatContext->duration )
            {
                writeLog( VT_LOG_INFO, "[INFO] [%s:%d] over duration Req(%lld) > %lld\n",
                        __func__, __LINE__, requestPosition, m_pFormatContext->duration );
                break;
            }
            if ( found )
            {
                currentPosition = avio_tell( m_pFormatContext->pb );
                requestPosition = requestPosition + durationFor10Percent;
                writeLog( VT_LOG_INFO, "[INFO] [%s:%d] dur10P(%lld), Curr(%lld), Req(%lld) / %lld\n",
                        __func__, __LINE__,
                        durationFor10Percent, currentPosition, requestPosition, m_pFormatContext->duration );
                timestamp = requestPosition;
                if (m_pFormatContext->start_time != AV_NOPTS_VALUE) {
                        timestamp += m_pFormatContext->start_time;
                        writeLog( VT_LOG_DBG, "[DBG] [%s:%d] start_time(%lld) timestamp->(%lld)\n", __func__, __LINE__, m_pFormatContext->start_time, timestamp);
                }
                if (av_seek_frame(m_pFormatContext, -1, timestamp, 0) < 0)
                {
                    writeLog( VT_LOG_INFO, "[INFO] [%s:%d] seek fail..to  %lld (cur : %lld)!\n",
                            __func__, __LINE__, requestPosition, currentPosition );
                }
                avcodec_flush_buffers(m_pVideoCodecContext);
            }
        }
#endif

        // get packet
        ret = getPacket( m_pPacket, pFormatCtx, &key_detected, &i);
        if ( ret == RETURN_BREAK    ) break;
        if ( ret == RETURN_CONTINUE ) continue;
        if ( key_detected == 1 )
            pCodecCtx->skip_frame = AVDISCARD_BIDIR;

        // get frame
        ret = getFrame( m_pPacket, pCodecCtx, m_pFrame, &key_detected, &v);
        if (m_pFrame->width != 0 && m_pFrame->height != 0)
        {
          requestInfo.m_originalWidth = m_pFrame->width;
          requestInfo.m_originalHeight = m_pFrame->height;
        }
        //writeLog( VT_LOG_CHK, "[CHK] [%s:%d] pkt(size:%d, pos:%lld), ret %d, pts(%lld), ptype(%d), key(%d)\n", \
                __func__, __LINE__, m_pPacket->size, m_pPacket->pos, ret, \
                m_pFrame->pts, m_pFrame->pict_type, m_pFrame->key_frame );
        if ( ret == RETURN_BREAK    ) break;
        if ( ret == RETURN_CONTINUE ) continue;
        key_detected = 0;
        pCodecCtx->skip_frame = AVDISCARD_NONKEY;

        // skip non I-picture. (+ search next packet)
        if ( m_pFrame->pict_type != AV_PICTURE_TYPE_I && m_pFrame->key_frame == 0)
        {
            writeLog( VT_LOG_DBG, "[DBG] [%s:%d] pict_type(%d), key(%d) -> SKIP (neither I-frame nor key)\n", \
                    __func__, __LINE__,  m_pFrame->pict_type, m_pFrame->key_frame );
            continue;
        }
        if ( _is_good_pgm (m_pFrame->data[0], m_pFrame->linesize[0], requestInfo.m_originalWidth, requestInfo.m_originalHeight) == 0)
        {
            writeLog( VT_LOG_CHK, "[CHK] [%s:%d] cont. by is_good_pgm()\n", __func__, __LINE__ );
            continue;
        }

        if ( m_isEncoderOpened == false )
        {
            if (!initializeEnc(requestInfo))
            {
                requestInfo.m_errorMsg = "Could not initialize initialzeEnc";
                bret = false;
            }
        }

        // encode frame
        ret = encodeFrame( requestInfo, &found);
        if ( ret == RETURN_BREAK    ) break;
        if ( ret == RETURN_CONTINUE ) continue;

        // check reqeust number of frames.
        if ( found >= requestInfo.m_frames )
            break;
    }
    if ( m_isEncoderOpened )  // open & not finished
    {
        // finishEncFrame
        if ( finishEncFrame(requestInfo) == false )
            writeLog( VT_LOG_DBG, "[DBG] [%s:%d] finishEnc fail\n", __func__, __LINE__);
        else
            m_isEncoderOpened = false;
    }

    /*free m_pPacket after loop breaking*/
    av_packet_free(&m_pPacket);

    /*set decode frame to output*/
    if (found > 0)
        bret = true;
    else
    {
		requestInfo.m_errorMsg = "Cannot found key-frame";
        bret = false;
    }

    pCodecCtx->skip_frame = AVDISCARD_NONE;

    return bret;
}

bool VT_VideoDecoder::initialize(VT_RequestInfo& requestInfo)
{
	if (avformat_find_stream_info(m_pFormatContext, NULL) < 0)
	{
		requestInfo.m_errorMsg = "Could not find stream information";
		return false;
	}

	// Note : Find the first video stream
	for (unsigned int i = 0; i < m_pFormatContext->nb_streams; ++i)
	{
		if (m_pFormatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
		{
			m_pVideoStream = m_pFormatContext->streams[i];
			m_VideoStream = i;
			break;
		}
	}

	if (m_VideoStream == -1)
	{
		requestInfo.m_errorMsg = "Could not find video stream";
		return false;
	}

	// Note : Get a pointer to the codec context for the stream's information about the code
	// Note : This contains all the information about the codec that the stream is using
//	m_pVideoCodecContext = m_pFormatContext->streams[m_VideoStream]->codec;
    if ((m_pVideoCodecContext = avcodec_alloc_context3(NULL)) == NULL)
    {
        requestInfo.m_errorMsg = "Fail to alloc codec context";
        return false;
    }

    if (avcodec_parameters_to_context(m_pVideoCodecContext, m_pFormatContext->streams[m_VideoStream]->codecpar) < 0)
    {
        requestInfo.m_errorMsg = "Fail to get codec context";
        return false;
    }

    m_pVideoCodecContext->skip_frame =  AVDISCARD_NONKEY;
    m_pVideoCodecContext->skip_loop_filter =  AVDISCARD_ALL;

	// Note : Find the decoder for the video stream
	if ((m_pVideoCodec = avcodec_find_decoder(m_pVideoCodecContext->codec_id)) == NULL)
	{
		// Warning : Set to NULL, otherwise avcodec_close(m_pVideoCodecContext) crashes
		m_pVideoCodecContext = NULL;
		requestInfo.m_errorMsg = "Could not fild video codec";
		return false;
	}

	m_pVideoCodecContext->workaround_bugs = 1;

	// Note : Open codec
	if (avcodec_open2(m_pVideoCodecContext, m_pVideoCodec, NULL) < 0)
	{
		requestInfo.m_errorMsg = "Could not open video codec";
		return false;
	}

	requestInfo.m_codec = getCodec();
	requestInfo.m_originalWidth = getWidth();
	requestInfo.m_originalHeight = getHeight();

  if ( requestInfo.m_requestWidth == 0 )
    requestInfo.m_requestWidth = requestInfo.m_originalWidth;
  if ( requestInfo.m_requestHeight == 0 )
    requestInfo.m_requestHeight = requestInfo.m_originalHeight;
  return true;
}

void VT_VideoDecoder::destroy()
{
    writeLog( VT_LOG_DBG, "[DBG] [%s:%d]\n", __func__, __LINE__ );
    if (m_pOutCodecContext)
    {
        avcodec_close(m_pOutCodecContext);
		m_pOutCodecContext = NULL;
    }
    if ( m_pOutFormatContext )
    {
		avformat_free_context(m_pOutFormatContext);
    }
    closeDecoder();
}

void VT_VideoDecoder::closeDecoder()
{
    writeLog( VT_LOG_DBG, "[DBG] [%s:%d]\n", __func__, __LINE__ );
	if (m_pVideoCodecContext)
	{
        avcodec_free_context(&m_pVideoCodecContext);
		m_pVideoCodecContext = NULL;
	}
   	if (m_pFormatContext)
	{
		avformat_close_input(&m_pFormatContext);
	}
	if (m_pPacket)
	{
		av_packet_free(&m_pPacket);
	}

	if (m_pFrame)
	{
		av_frame_free(&m_pFrame);
		m_pFrame = NULL;
	}

    if ( m_filter_graph )
    {
        avfilter_graph_free(&m_filter_graph);
    }
	m_VideoStream = -1;
}

bool VT_VideoDecoder::decode(VT_RequestInfo& requestInfo)
{
    if ( m_pFrame )
    {
        av_frame_free(&m_pFrame);
        m_pFrame = NULL;
    }

    if(requestInfo.m_isMotion)
        return decodeMotionThumbnail(requestInfo);
    else
        return decodeThumbnail(requestInfo);
}

bool VT_VideoDecoder::seek(VT_RequestInfo& requestInfo)
{
	if(requestInfo.m_seekTime == -1)
	{
		if(requestInfo.m_isMotion)
			requestInfo.m_seekTime = getDuration() * DEFAULT_PERCENT / 100 * 2;
		else
			requestInfo.m_seekTime = getDuration() * DEFAULT_PERCENT / 100;
	}

	int64_t	timestamp = AV_TIME_BASE * static_cast<int64_t>(requestInfo.m_seekTime);
	if (m_pFormatContext->start_time != AV_NOPTS_VALUE) {
		timestamp += m_pFormatContext->start_time;
		writeLog( VT_LOG_DBG, "[DBG] [%s:%d] start_time(%lld) timestamp->(%lld)\n", __func__, __LINE__, m_pFormatContext->start_time, timestamp);
	}

	if (timestamp < 0)
	{
		timestamp = 0;
	}
	if ( !m_pFormatContext->pb->seekable )
	{
		requestInfo.m_errorMsg = "Doesn't support seek";
		return false;
	}
	if (av_seek_frame(m_pFormatContext, -1, timestamp, 0) < 0)
	{
		requestInfo.m_errorMsg = "Could not seek in video";
		return false;
	}
	avcodec_flush_buffers(m_pVideoCodecContext);

    return true;
}

bool VT_VideoDecoder::scale(VT_RequestInfo& requestInfo, VT_VideoFrame& videoFrame)
{
    // initialize Video Filtering
    if ( initializeVideoFilters(requestInfo) < 0 )
    {
        requestInfo.m_errorMsg = "Could not initializeVideoFilters";
        return false;
    }

    if ( applyVideoFilters(requestInfo) == false )
    {
        writeLog( VT_LOG_ERR, "[ERR] [%s:%d] cannot apply video filter\n", __func__, __LINE__);
		requestInfo.m_errorMsg = "Cannot apply video filter";
        return false;
    }

	videoFrame.width = requestInfo.m_scaledWidth;
	videoFrame.height = requestInfo.m_scaledHeight;
	videoFrame.lineSize = videoFrame.width * 3; //m_pFrame->linesize[0];

    writeLog( VT_LOG_DBG, "[DBG] [%s:%d] width(%d), height(%d), linesize(%d)\n", __func__, __LINE__,
            videoFrame.width, videoFrame.height, videoFrame.lineSize );

	videoFrame.frameData.clear();
	videoFrame.frameData.resize(videoFrame.lineSize * videoFrame.height);

    for(int i=0;i<videoFrame.height;i++)
        memcpy(videoFrame.frameData.data()+ (i*videoFrame.lineSize), &(m_pFrame->data[0][i*m_pFrame->linesize[0]]), videoFrame.lineSize);

	return true;
}

string VT_VideoDecoder::getCodec()
{
	if (m_pVideoCodec)
	{
		return m_pVideoCodec->name;
	}

	return "";
}

int VT_VideoDecoder::getWidth()
{
	if (m_pVideoCodecContext)
	{
		return m_pVideoCodecContext->width;
	}

	return -1;
}

int VT_VideoDecoder::getHeight()
{
	if (m_pVideoCodecContext)
	{
		return m_pVideoCodecContext->height;
	}

	return -1;
}

int VT_VideoDecoder::getDuration()
{
	if (m_pFormatContext)
	{
		return static_cast<int>(m_pFormatContext->duration / AV_TIME_BASE);
	}

	return 0;
}

bool VT_VideoDecoder::getEncoderOpened()
{
   return m_isEncoderOpened;
}

// APIs about ffmpeg-Enc
bool VT_VideoDecoder::initializeEnc(VT_RequestInfo &requestInfo)
{
    int ret =0;
    string extOutputFile;

    extOutputFile = requestInfo.m_outputPath;
    if ( requestInfo.m_imageType == "" )
        extOutputFile.append(".webp");
    else
        extOutputFile.append( requestInfo.m_imageType );
    writeLog( VT_LOG_DBG, "[DBG] [%s:%d] tmpOutputFile = %s\n",
            __func__, __LINE__, extOutputFile.c_str());

    //avformat_alloc_output_context2(&m_pOutFormatContext, NULL, NULL, requestInfo.m_outputPath.c_str());
    if (avformat_alloc_output_context2(&m_pOutFormatContext, NULL, NULL, extOutputFile.c_str() ) < 0)
    {
        requestInfo.m_errorMsg = "avformat_alloc_output_context2 ret -> fail";
        return false;
    }
    if ( !m_pOutFormatContext )
    {
        requestInfo.m_errorMsg = "avformat_alloc_output_context2 -> fail";
        return false;
    }

    // webp.. (not use libwebp_anim)
    m_pOutCodec = avcodec_find_encoder_by_name( "libwebp" );
    if ( !m_pOutCodec )
    {
        requestInfo.m_errorMsg = "avcodec_find_decoder -> fail";
        return false;
    }
    writeLog( VT_LOG_DBG, "[DBG] [%s:%d] video_codec(%d) -> out codec(%s)\n",
            __func__, __LINE__, m_pOutFormatContext->oformat->video_codec, m_pOutCodec->name );

    m_pOutStream = avformat_new_stream( m_pOutFormatContext, m_pOutCodec );
    if ( !m_pOutStream )
    {
        requestInfo.m_errorMsg = "avformat_new_stream -> fail";
        return false;
    }

    // >
    // AVStream::codec is deprecated.
    // But, if m_pOutCodecContext is allocated by avcodec_alloc_context3, m_pOutStream->codec makes memory leak.
    // So, use AVStream::codec.
    m_pOutCodecContext = m_pOutStream->codec;

    if(m_pOutCodec->capabilities & AV_CODEC_CAP_TRUNCATED)
        m_pOutCodecContext->flags |= AV_CODEC_FLAG_TRUNCATED; /* we do not send complete frames */

    writeLog( VT_LOG_DBG, "[DBG] [%s:%d] RES orig: %dx%d, req: %dx%d, scaled: %dx%d\n",
            __func__, __LINE__, \
            requestInfo.m_originalWidth, requestInfo.m_originalHeight,
            requestInfo.m_requestWidth, requestInfo.m_requestHeight,
            requestInfo.m_scaledWidth, requestInfo.m_scaledHeight);

    if(m_pFormatContext)
    {
        m_pOutCodecContext->sample_aspect_ratio.num =
            m_pFormatContext->streams[m_VideoStream]->codecpar->sample_aspect_ratio.num;
        m_pOutCodecContext->sample_aspect_ratio.den =
            m_pFormatContext->streams[m_VideoStream]->codecpar->sample_aspect_ratio.den;
    }
    m_pOutCodecContext->width   = requestInfo.m_requestWidth; //requestInfo.m_scaledWidth;
    m_pOutCodecContext->height  = requestInfo.m_requestHeight; //requestInfo.m_scaledHeight;
    m_pOutCodecContext->time_base.den   = 1;   //STREAM_FRAME_RATE; // no effect -> writeEncFrame
    m_pOutCodecContext->time_base.num   = 1;
    m_pOutCodecContext->gop_size        = 10; /* emit one intra frame every twelve frames at most */
    m_pOutCodecContext->pix_fmt         = requestInfo.m_pix_fmt;
    m_pOutStream->time_base             = m_pOutCodecContext->time_base;

    if (avcodec_open2( m_pOutCodecContext, m_pOutCodec, NULL) < 0) {
        requestInfo.m_errorMsg = "avcodec_open2 -> fail";
        return false;
    }

    ret = avcodec_parameters_from_context( m_pOutStream->codecpar, m_pOutCodecContext);
    if ( ret < 0 )
    {
        writeLog( VT_LOG_ERR, "[ERR] [%s:%d] fail to avcodec_parameters_from_context(ret:%d)\n",
                __func__, __LINE__, ret );
        requestInfo.m_errorMsg = "avcodec_parameters_from_context -> fail";
        return false;
    }
    // <
    if (!initializeEncFile(requestInfo))
        return false;

    // initialize Video Filtering
    if ( initializeVideoFilters(requestInfo) < 0 )
    {
        requestInfo.m_errorMsg = "Could not initializeVideoFilters";
        return false;
    }

    return true;
}

// DO - has jobs for same source
bool VT_VideoDecoder::initializeEncFile(VT_RequestInfo &requestInfo)
{
    av_dict_set( &_OutputHeaderOpts, "loop", "0", 0);

    m_encFrameCount=0;
    m_pOutStream->cur_dts = 0LL;

    if ( avio_open(&m_pOutFormatContext->pb, requestInfo.m_outputPath.c_str(), AVIO_FLAG_WRITE) < 0 )
    {
        requestInfo.m_errorMsg = "avio_open -> fail";
        return false;
    }

    if ( avformat_write_header( m_pOutFormatContext, &_OutputHeaderOpts ) < 0 )
    {
        requestInfo.m_errorMsg = "avformat_write_header -> fail";
        return false;
    }
    m_isEncoderOpened = true;
    return true;
}

bool VT_VideoDecoder::writeEncFrame(VT_RequestInfo &requestInfo)
{
    AVPacket outPkt;
    int ret = 0;
    bool bRet = false;

    av_init_packet(&outPkt);
    outPkt.data=NULL;
    outPkt.size=0;
    outPkt.pts = outPkt.dts = m_encFrameCount;

    // time_base.dec = 100
    //m_pFrame->pts = (int64_t)(( m_pOutFormatContext->streams[0]->time_base.den >> 1 ) * m_encFrameCount);
    m_pFrame->pts = (int64_t)( m_pOutFormatContext->streams[0]->time_base.den ) * m_encFrameCount;
    writeLog( VT_LOG_DBG, "[DBG] [%s:%d] cur_dts(%lld), pts(%lld) / timebase(%d/%d), frameCount(%d)\n",
            __func__, __LINE__,
            m_pOutStream->cur_dts,
            m_pFrame->pts,
            m_pOutFormatContext->streams[0]->time_base.num,
            m_pOutFormatContext->streams[0]->time_base.den, m_encFrameCount );

    m_encFrameCount++;

    //ret = avcodec_encode_video2( m_pOutCodecContext, &outPkt, m_pFrame, &got_output);
    if ((ret = avcodec_send_frame(m_pOutCodecContext, m_pFrame)) < 0)
    {
        writeLog( VT_LOG_ERR, "[ERR] [%s:%d] Fail to avcodec_send_frame(ret : %d)\n",
                __func__, __LINE__,ret );
        goto END;
    }

    if ((ret = avcodec_receive_packet(m_pOutCodecContext, &outPkt)) < 0)
    {
        writeLog( VT_LOG_ERR, "[ERR] [%s:%d] Fail to avcodec_receive_packet(ret : %d)\n",
                __func__, __LINE__,ret );
        goto END;
    }

    if ( (ret = av_write_frame(m_pOutFormatContext, &outPkt )) < 0 )
    {
        writeLog( VT_LOG_ERR, "[ERR] [%s:%d] Fail to av_write_frame(ret : %d)\n",
                __func__, __LINE__,ret );
        goto END;
    }
    writeLog( VT_LOG_DBG, "[DBG] [%s:%d] codec(%s), writeframe %d bytes (ret : %d)\n",
            __func__, __LINE__, m_pOutCodecContext->codec->name, outPkt.size, ret );
    bRet = true;

END:
    av_packet_unref(&outPkt);
    return bRet;
}

bool VT_VideoDecoder::finishEncFrame(VT_RequestInfo &requestInfo)
{
    if ( av_write_trailer( m_pOutFormatContext) < 0 )
    {
        requestInfo.m_errorMsg = "av_write_trailer -> fail";
        return false;
    }
    avio_close(m_pOutFormatContext->pb);
    //avformat_free_context(m_pOutFormatContext);   // move to destroyer
    return true;
}

int VT_VideoDecoder::getStreamRotation()
{
    if(NULL == m_pVideoStream)
        return -1;

    int32_t* matrix = (int32_t*)av_stream_get_side_data(m_pVideoStream, AV_PKT_DATA_DISPLAYMATRIX, NULL);
    if (matrix)
    {
        int angle = lround(av_display_rotation_get(matrix));
        if (angle > 45 && angle < 135)
        {
            return 2;
        }
        else if (angle < -45 && angle > -135)
        {
            return 1;
        }
    }

    return -1;
}

int VT_VideoDecoder::getPacket( AVPacket *pPacket, AVFormatContext *pFormatCtx, int *key_detected, int *nKeys)
{
    int ret;

    av_packet_unref (pPacket);
    ret = av_read_frame (pFormatCtx, pPacket);
    if (ret < 0)
    {
        writeLog( VT_LOG_ERR, "[ERR] [%s:%d] av_read_frame() fail\n", __func__, __LINE__ );
        return RETURN_BREAK;
    }
    if ( pPacket->stream_index != m_VideoStream )    // not video
    {
        return RETURN_CONTINUE;
    }

    if ( pPacket->flags & AV_PKT_FLAG_KEY )
    {
        *nKeys = *nKeys + 1;
        *key_detected = 1;
    }
    else if ( *key_detected == 0 )
    {   // non-key && not key_detected
        return RETURN_CONTINUE;
    }
    // COND : (flag & AV_PKT_FLAG_KEY) || key_detected
    writeLog( VT_LOG_DBG, "[DBG] [%s:%d] pkt: flags=0x%x, pts=%lld(0x%llx)\n", __func__, __LINE__,
            pPacket->flags, pPacket->pts, pPacket->pts );

    return RETURN_OK;
}

int VT_VideoDecoder::getFrame( AVPacket *pPacket, AVCodecContext *pCodecCtx, AVFrame *pFrame, int *key_detected, int *nFrames)
{
    int len;
    int got_picture = 1;
    int ret = 0;

    // ffmpeg 3.2.2 doesn't support anymore
    //avcodec_get_frame_defaults(pFrame); -> av_frame_unref
    av_frame_unref(pFrame);   // get frame default

    if ((ret = avcodec_send_packet(pCodecCtx, pPacket)) < 0)
    {
        writeLog( VT_LOG_ERR, "[ERR] [%s:%d] Fail to avcodec_send_packet(ret : %d)\n",
                __func__, __LINE__,ret );
        return RETURN_CONTINUE;
    }
    if ((ret = avcodec_receive_frame(pCodecCtx, pFrame)) < 0)
    {
        writeLog( VT_LOG_ERR, "[ERR] [%s:%d] Fail to avcodec_receive_frame(ret : %d)\n",
                __func__, __LINE__,ret );
        return RETURN_CONTINUE;
    }

// TODO Need to compare tizen code ???
    if (NULL != nFrames)
        *nFrames = *nFrames + 1;
    if (!got_picture)
    {
        // COND-ELSE : got_frame == 0
        if ( key_detected )
            *key_detected = 1;
        return RETURN_CONTINUE;
    }
    // got_frame
    return RETURN_OK;
}

int VT_VideoDecoder::encodeFrame( VT_RequestInfo &requestInfo, int *pFound)
{
    // Apply video filter
    if ( applyVideoFilters(requestInfo) == false )
    {
        writeLog( VT_LOG_ERR, "[ERR] [%s:%d] cannot apply video filter\n", __func__, __LINE__);
        return RETURN_BREAK;
    }

    // Encoding & write frame
    if ( writeEncFrame(requestInfo) == false )
    {
        writeLog( VT_LOG_ERR, "[ERR] [%s:%d] writeEnc fail\n", __func__, __LINE__);
        return RETURN_BREAK;
    }
    *pFound = *pFound + 1;

    return RETURN_OK;
}

