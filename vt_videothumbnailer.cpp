#include "vt_videothumbnailer.h"
#include "vt_videodecoder.h"

VT_VideoDecoder      *pVideoDecoder = NULL;
bool VT_VideoThumbnailer::holdDecoder = false;
void VT_VideoThumbnailer::setHoldDecoder(bool value)
{
	if (holdDecoder != value)
	{
		writeLog( VT_LOG_DBG, "[DBG] [%s:%d] holdDecoder changed : %d\n", __func__, __LINE__, value );
		if (pVideoDecoder != NULL)
		{
			clearLastSource();
			delete pVideoDecoder;
			pVideoDecoder = NULL;
		}
	}
	holdDecoder = value;
}

bool VT_VideoThumbnailer::generateThumbnail(VT_RequestInfo& requestInfo)
{
	VT_VideoFrame	videoFrame;
    bool ret = true;

    gettimeofday( &(requestInfo.m_startTime), NULL);
    writeLog( VT_LOG_INFO, "[INFO] [%s:%d] start thumbnail %s, Support 4K(%d)\n", __func__, __LINE__,
            requestInfo.m_videoPath.c_str(), requestInfo.m_b4kSupported );

    // Checking lastSource sams as m_videoPath and request size.
    if ( isSameSourceAndSize( requestInfo ) == false )
    {   // different source
        if (pVideoDecoder) {
            delete pVideoDecoder;
        }
        pVideoDecoder = new VT_VideoDecoder();
        writeLog( VT_LOG_DBG, "[DBG] [%s:%d] (NO CACHE) New VideoDecoder(%p)\n", __func__, __LINE__,
                pVideoDecoder );

        if (!pVideoDecoder->open(requestInfo) || !is4kSupported(requestInfo))
            goto FAIL;
        if (!pVideoDecoder->initialize(requestInfo) || !is4kSupported(requestInfo))
            goto FAIL;

        if ( requestInfo.m_seekTime != 0 )
        {
            writeLog( VT_LOG_DBG, "[DBG] [%s:%d] seek to %d time\n", __func__, __LINE__,
                    requestInfo.m_seekTime );
            if ( !pVideoDecoder->seek(requestInfo) )
                goto FAIL;
        }
    }
    else
    {   // same source
        writeLog( VT_LOG_DBG, "[DBG] [%s:%d] (CACHE) Last VideoDecoder(%p)\n",
                __func__, __LINE__, pVideoDecoder );
        requestInfo.m_codec = pVideoDecoder->getCodec();
        requestInfo.m_originalWidth = pVideoDecoder->getWidth();
        requestInfo.m_originalHeight = pVideoDecoder->getHeight();
        if ( requestInfo.m_requestWidth == 0 )
            requestInfo.m_requestWidth = requestInfo.m_originalWidth;
        if ( requestInfo.m_requestHeight == 0 )
            requestInfo.m_requestHeight = requestInfo.m_originalHeight;

        // Need to seek
        writeLog( VT_LOG_DBG, "[DBG] [%s:%d] seek to %d time\n", __func__, __LINE__,
                requestInfo.m_seekTime );
        if ( !pVideoDecoder->seek(requestInfo))
            goto FAIL;
        writeLog( VT_LOG_DBG, "[DBG] [%s:%d] SAME SOURCE -> SKIP Initialize, Open routine!\n",
                __func__, __LINE__);
    }

    writeLog( VT_LOG_DBG, "[DBG] [%s:%d] isMotion(%d)\n", __func__, __LINE__, requestInfo.m_isMotion );
	if (!pVideoDecoder->decode(requestInfo))
    {
        // clear cache
        clearLastSource();
		goto FAIL;
    }

    if(requestInfo.m_isMotion == false ) {
    	if (!pVideoDecoder->scale(requestInfo, videoFrame))
        	goto FAIL;
        ret = (requestInfo.m_imageType == "raw") ? writeRawImage(requestInfo, videoFrame) : writeJpegImage(requestInfo, videoFrame);
    }

    updateLastSource(requestInfo);
    if (!holdDecoder)
    {
        clearLastSource();
        delete pVideoDecoder;
        pVideoDecoder = NULL;
    }
    return ret;

FAIL:
    clearLastSource();
    if (pVideoDecoder)
    {
        delete pVideoDecoder;
        pVideoDecoder = NULL;
    }
    return false;
}

bool VT_VideoThumbnailer::getHeaderInfo(VT_RequestInfo& requestInfo)
{
	VT_VideoDecoder	videoDecoder;

	if (!videoDecoder.open(requestInfo))
		return false;

	if(!checkResolution(requestInfo))
		return false;

	return true;
}

bool VT_VideoThumbnailer::writeRawImage(VT_RequestInfo& requestInfo, VT_VideoFrame& videoFrame)
{
	FILE*	fp = NULL;
	bool	retVal = true;

	if ((fp = fopen(requestInfo.m_outputPath.c_str(), "wb")) == NULL)
	{
		requestInfo.m_errorMsg = "Failed to open output file";
		return false;
	}

	if (fwrite(&(videoFrame.frameData.front()), sizeof(uint8_t), videoFrame.lineSize * videoFrame.height, fp) < videoFrame.lineSize * videoFrame.height)
	{
		requestInfo.m_errorMsg = "Failed to write output raw file";
		retVal = false;
	}

	if (fp)
	{
		fsync(fileno(fp));
		fclose(fp);
	}

	if(retVal == false)
	{
		if(remove(requestInfo.m_outputPath.c_str()) != 0)
		{
			requestInfo.m_errorMsg += " / Could not remove output path";
		}
	}

	return retVal;
}

bool VT_VideoThumbnailer::writeJpegImage(VT_RequestInfo& requestInfo, VT_VideoFrame& videoFrame)
{
	FILE*			fp = NULL;
	jpeg_compress_struct	compression;
	jpeg_error_mgr		errorHandler;
	uint8_t*			rgbData = &(videoFrame.frameData.front());

	if ((fp = fopen(requestInfo.m_outputPath.c_str(), "wb")) == NULL)
	{
		requestInfo.m_errorMsg = "Failed to open output file";
		return false;
	}

	compression.err = jpeg_std_error(&errorHandler);
	jpeg_create_compress(&compression);
	jpeg_stdio_dest(&compression, fp);

	compression.image_width = videoFrame.width;
	compression.image_height = videoFrame.height;
	compression.input_components = 3;
	compression.in_color_space = JCS_RGB;

	jpeg_set_defaults(&compression);
	jpeg_set_quality(&compression, 100, TRUE);
	jpeg_start_compress(&compression, TRUE);

	while (compression.next_scanline < compression.image_height)
	{
		(void) jpeg_write_scanlines(&compression, &rgbData, 1);
		rgbData += videoFrame.lineSize;
	}

	jpeg_finish_compress(&compression);

	if (fp)
	{
		fsync(fileno(fp));
		fclose(fp);
	}

	jpeg_destroy_compress(&compression);
	return true;
}

bool VT_VideoThumbnailer::is4kSupported(VT_RequestInfo& requestInfo)
{
    if ( requestInfo.m_b4kSupported )
        return true;

	float	fCeilW = 0.0;
	float	fCeilH = 0.0;
	int	nMBs = 0;

	if (requestInfo.m_b4kSupported == true)
		return true;

	fCeilW	= ceilf((float)requestInfo.m_originalWidth/16);
	fCeilH	= ceilf((float)requestInfo.m_originalHeight/16);
	nMBs	= (int)fCeilW * (int)fCeilH;

	if (nMBs > 8704)
	{
		requestInfo.m_errorMsg = "Could not generate 4K video thumbnail";
		return false;
	}

	return true;
}

bool VT_VideoThumbnailer::checkResolution(VT_RequestInfo& requestInfo)
{
	if((requestInfo.m_originalWidth <= 0) || (requestInfo.m_originalHeight <= 0))
	{
		requestInfo.m_errorMsg = "No resolution information";
		return false;
	}

	return true;
}
