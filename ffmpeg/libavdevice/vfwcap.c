/*
 * VFW capture interface
 * Copyright (c) 2006-2008 Ramiro Polla.
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

#include "libavformat/avformat.h"
#include <vfw.h>
#include <windows.h>

//#define DEBUG_VFW

/* Defines for VFW missing from MinGW.
 * Remove this when MinGW incorporates them. */
#define WM_CAP_START                (0x0400)
#define WM_CAP_SET_CALLBACK_VIDEOSTREAM (WM_CAP_START + 6)
#define WM_CAP_DRIVER_CONNECT       (WM_CAP_START + 10)
#define WM_CAP_DRIVER_DISCONNECT    (WM_CAP_START + 11)
#define WM_CAP_GET_VIDEOFORMAT      (WM_CAP_START + 44)
#define WM_CAP_SET_VIDEOFORMAT      (WM_CAP_START + 45)
#define WM_CAP_SET_PREVIEW          (WM_CAP_START + 50)
#define WM_CAP_SET_OVERLAY          (WM_CAP_START + 51)
#define WM_CAP_SEQUENCE_NOFILE      (WM_CAP_START + 63)
#define WM_CAP_SET_SEQUENCE_SETUP   (WM_CAP_START + 64)
#define WM_CAP_GET_SEQUENCE_SETUP   (WM_CAP_START + 65)

#define HWND_MESSAGE                ((HWND)-3)

#define BI_RGB                      0

/* End of missing MinGW defines */

struct vfw_ctx {
    HWND hwnd;
    HANDLE mutex;
    HANDLE event;
    AVPacketList *pktl;
    AVFormatContext *s;
    unsigned int curbufsize;
    unsigned int frame_num;
};

static enum PixelFormat vfw_pixfmt(DWORD biCompression, WORD biBitCount)
{
    switch(biCompression) {
    case MKTAG('Y', 'U', 'Y', '2'):
        return PIX_FMT_YUYV422;
    case MKTAG('I', '4', '2', '0'):
        return PIX_FMT_YUV420P;
    case BI_RGB:
        switch(biBitCount) { /* 1-8 are untested */
            case 1:
                return PIX_FMT_MONOWHITE;
            case 4:
                return PIX_FMT_RGB4;
            case 8:
                return PIX_FMT_RGB8;
            case 16:
                return PIX_FMT_RGB555;
            case 24:
                return PIX_FMT_BGR24;
            case 32:
                return PIX_FMT_RGB32;
        }
    }
    return -1;
}

#define dstruct(pctx, sname, var, type) \
    av_log(pctx, AV_LOG_DEBUG, #var":\t%"type"\n", sname->var)

static void dump_captureparms(AVFormatContext *s, CAPTUREPARMS *cparms)
{
    av_log(s, AV_LOG_DEBUG, "CAPTUREPARMS\n");
    dstruct(s, cparms, dwRequestMicroSecPerFrame, "lu");
    dstruct(s, cparms, fMakeUserHitOKToCapture, "d");
    dstruct(s, cparms, wPercentDropForError, "u");
    dstruct(s, cparms, fYield, "d");
    dstruct(s, cparms, dwIndexSize, "lu");
    dstruct(s, cparms, wChunkGranularity, "u");
    dstruct(s, cparms, fUsingDOSMemory, "d");
    dstruct(s, cparms, wNumVideoRequested, "u");
    dstruct(s, cparms, fCaptureAudio, "d");
    dstruct(s, cparms, wNumAudioRequested, "u");
    dstruct(s, cparms, vKeyAbort, "u");
    dstruct(s, cparms, fAbortLeftMouse, "d");
    dstruct(s, cparms, fAbortRightMouse, "d");
    dstruct(s, cparms, fLimitEnabled, "d");
    dstruct(s, cparms, wTimeLimit, "u");
    dstruct(s, cparms, fMCIControl, "d");
    dstruct(s, cparms, fStepMCIDevice, "d");
    dstruct(s, cparms, dwMCIStartTime, "lu");
    dstruct(s, cparms, dwMCIStopTime, "lu");
    dstruct(s, cparms, fStepCaptureAt2x, "d");
    dstruct(s, cparms, wStepCaptureAverageFrames, "u");
    dstruct(s, cparms, dwAudioBufferSize, "lu");
    dstruct(s, cparms, fDisableWriteCache, "d");
    dstruct(s, cparms, AVStreamMaster, "u");
}

static void dump_videohdr(AVFormatContext *s, VIDEOHDR *vhdr)
{
#ifdef DEBUG_VFW
    av_log(s, AV_LOG_DEBUG, "VIDEOHDR\n");
    dstruct(s, vhdr, lpData, "p");
    dstruct(s, vhdr, dwBufferLength, "lu");
    dstruct(s, vhdr, dwBytesUsed, "lu");
    dstruct(s, vhdr, dwTimeCaptured, "lu");
    dstruct(s, vhdr, dwUser, "lu");
    dstruct(s, vhdr, dwFlags, "lu");
    dstruct(s, vhdr, dwReserved[0], "lu");
    dstruct(s, vhdr, dwReserved[1], "lu");
    dstruct(s, vhdr, dwReserved[2], "lu");
    dstruct(s, vhdr, dwReserved[3], "lu");
#endif
}

static void dump_bih(AVFormatContext *s, BITMAPINFOHEADER *bih)
{
    av_log(s, AV_LOG_DEBUG, "BITMAPINFOHEADER\n");
    dstruct(s, bih, biSize, "lu");
    dstruct(s, bih, biWidth, "ld");
    dstruct(s, bih, biHeight, "ld");
    dstruct(s, bih, biPlanes, "d");
    dstruct(s, bih, biBitCount, "d");
    dstruct(s, bih, biCompression, "lu");
    av_log(s, AV_LOG_DEBUG, "    biCompression:\t\"%.4s\"\n",
                   (char*) &bih->biCompression);
    dstruct(s, bih, biSizeImage, "lu");
    dstruct(s, bih, biXPelsPerMeter, "lu");
    dstruct(s, bih, biYPelsPerMeter, "lu");
    dstruct(s, bih, biClrUsed, "lu");
    dstruct(s, bih, biClrImportant, "lu");
}

static int shall_we_drop(struct vfw_ctx *ctx)
{
    AVFormatContext *s = ctx->s;
    const uint8_t dropscore[] = {62, 75, 87, 100};
    const int ndropscores = FF_ARRAY_ELEMS(dropscore);
    unsigned int buffer_fullness = (ctx->curbufsize*100)/s->max_picture_buffer;

    if(dropscore[++ctx->frame_num%ndropscores] <= buffer_fullness) {
        av_log(ctx->s, AV_LOG_ERROR,
              "real-time buffer %d%% full! frame dropped!\n", buffer_fullness);
        return 1;
    }

    return 0;
}

static LRESULT CALLBACK videostream_cb(HWND hwnd, LPVIDEOHDR vdhdr)
{
    struct vfw_ctx *ctx;
    AVPacketList **ppktl, *pktl_next;

    ctx = (struct vfw_ctx *) GetWindowLongPtr(hwnd, GWLP_USERDATA);

    dump_videohdr(ctx->s, vdhdr);

    if(shall_we_drop(ctx))
        return FALSE;

    WaitForSingleObject(ctx->mutex, INFINITE);

    pktl_next = av_mallocz(sizeof(AVPacketList));
    if(!pktl_next)
        goto fail;

    if(av_new_packet(&pktl_next->pkt, vdhdr->dwBytesUsed) < 0) {
        av_free(pktl_next);
        goto fail;
    }

    pktl_next->pkt.pts = vdhdr->dwTimeCaptured;
    memcpy(pktl_next->pkt.data, vdhdr->lpData, vdhdr->dwBytesUsed);

    for(ppktl = &ctx->pktl ; *ppktl ; ppktl = &(*ppktl)->next);
    *ppktl = pktl_next;

    ctx->curbufsize += vdhdr->dwBytesUsed;

    SetEvent(ctx->event);
    ReleaseMutex(ctx->mutex);

    return TRUE;
fail:
    ReleaseMutex(ctx->mutex);
    return FALSE;
}

static int vfw_read_close(AVFormatContext *s);

static int vfw_read_header(AVFormatContext *s, AVFormatParameters *ap)
{
    struct vfw_ctx *ctx = s->priv_data;
    AVCodecContext *codec;
    AVStream *st;
    int devnum;
    int bisize;
    BITMAPINFO *bi;
    CAPTUREPARMS cparms;
    DWORD biCompression;
    WORD biBitCount;
    int width;
    int height;
    int ret;

    if(!ap->time_base.den) {
        av_log(s, AV_LOG_ERROR, "A time base must be specified.\n");
        return AVERROR_IO;
    }

    ctx->s = s;

    ctx->hwnd = capCreateCaptureWindow(NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, 0);
    if(!ctx->hwnd) {
        av_log(s, AV_LOG_ERROR, "Could not create capture window.\n");
        return AVERROR_IO;
    }

    /* If atoi fails, devnum==0 and the default device is used */
    devnum = atoi(s->filename);

    ret = SendMessage(ctx->hwnd, WM_CAP_DRIVER_CONNECT, devnum, 0);
    if(!ret) {
        av_log(s, AV_LOG_ERROR, "Could not connect to device.\n");
        DestroyWindow(ctx->hwnd);
        return AVERROR(ENODEV);
    }

    SendMessage(ctx->hwnd, WM_CAP_SET_OVERLAY, 0, 0);
    SendMessage(ctx->hwnd, WM_CAP_SET_PREVIEW, 0, 0);

    ret = SendMessage(ctx->hwnd, WM_CAP_SET_CALLBACK_VIDEOSTREAM, 0,
                      (LPARAM) videostream_cb);
    if(!ret) {
        av_log(s, AV_LOG_ERROR, "Could not set video stream callback.\n");
        goto fail_io;
    }

    SetWindowLongPtr(ctx->hwnd, GWLP_USERDATA, (LONG_PTR) ctx);

    st = av_new_stream(s, 0);
    if(!st) {
        vfw_read_close(s);
        return AVERROR_NOMEM;
    }

    /* Set video format */
    bisize = SendMessage(ctx->hwnd, WM_CAP_GET_VIDEOFORMAT, 0, 0);
    if(!bisize)
        goto fail_io;
    bi = av_malloc(bisize);
    if(!bi) {
        vfw_read_close(s);
        return AVERROR_NOMEM;
    }
    ret = SendMessage(ctx->hwnd, WM_CAP_GET_VIDEOFORMAT, bisize, (LPARAM) bi);
    if(!ret)
        goto fail_bi;

    dump_bih(s, &bi->bmiHeader);

    width  = ap->width  ? ap->width  : bi->bmiHeader.biWidth ;
    height = ap->height ? ap->height : bi->bmiHeader.biHeight;
    bi->bmiHeader.biWidth  = width ;
    bi->bmiHeader.biHeight = height;

#if 0
    /* For testing yet unsupported compressions
     * Copy these values from user-supplied verbose information */
    bi->bmiHeader.biWidth       = 320;
    bi->bmiHeader.biHeight      = 240;
    bi->bmiHeader.biPlanes      = 1;
    bi->bmiHeader.biBitCount    = 12;
    bi->bmiHeader.biCompression = MKTAG('I','4','2','0');
    bi->bmiHeader.biSizeImage   = 115200;
    dump_bih(s, &bi->bmiHeader);
#endif

    ret = SendMessage(ctx->hwnd, WM_CAP_SET_VIDEOFORMAT, bisize, (LPARAM) bi);
    if(!ret) {
        av_log(s, AV_LOG_ERROR, "Could not set Video Format.\n");
        goto fail_bi;
    }

    biCompression = bi->bmiHeader.biCompression;
    biBitCount = bi->bmiHeader.biBitCount;

    av_free(bi);

    /* Set sequence setup */
    ret = SendMessage(ctx->hwnd, WM_CAP_GET_SEQUENCE_SETUP, sizeof(cparms),
                      (LPARAM) &cparms);
    if(!ret)
        goto fail_io;

    dump_captureparms(s, &cparms);

    cparms.fYield = 1; // Spawn a background thread
    cparms.dwRequestMicroSecPerFrame =
                               (ap->time_base.num*1000000) / ap->time_base.den;
    cparms.fAbortLeftMouse = 0;
    cparms.fAbortRightMouse = 0;
    cparms.fCaptureAudio = 0;
    cparms.vKeyAbort = 0;

    ret = SendMessage(ctx->hwnd, WM_CAP_SET_SEQUENCE_SETUP, sizeof(cparms),
                      (LPARAM) &cparms);
    if(!ret)
        goto fail_io;

    codec = st->codec;
    codec->time_base = ap->time_base;
    codec->codec_type = CODEC_TYPE_VIDEO;
    codec->width = width;
    codec->height = height;
    codec->codec_id = CODEC_ID_RAWVIDEO;
    codec->pix_fmt = vfw_pixfmt(biCompression, biBitCount);
    if(biCompression == BI_RGB)
        codec->bits_per_coded_sample = biBitCount;

    av_set_pts_info(st, 32, 1, 1000);

    if(codec->pix_fmt == -1) {
        av_log(s, AV_LOG_ERROR, "Unknown compression type."
                         "Please report verbose (-v 99) debug information.\n");
        vfw_read_close(s);
        return AVERROR_PATCHWELCOME;
    }

    ctx->mutex = CreateMutex(NULL, 0, NULL);
    if(!ctx->mutex) {
        av_log(s, AV_LOG_ERROR, "Could not create Mutex.\n" );
        goto fail_io;
    }
    ctx->event = CreateEvent(NULL, 1, 0, NULL);
    if(!ctx->event) {
        av_log(s, AV_LOG_ERROR, "Could not create Event.\n" );
        goto fail_io;
    }

    ret = SendMessage(ctx->hwnd, WM_CAP_SEQUENCE_NOFILE, 0, 0);
    if(!ret) {
        av_log(s, AV_LOG_ERROR, "Could not start capture sequence.\n" );
        goto fail_io;
    }

    return 0;

fail_bi:
    av_free(bi);

fail_io:
    vfw_read_close(s);
    return AVERROR_IO;
}

static int vfw_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    struct vfw_ctx *ctx = s->priv_data;
    AVPacketList *pktl = NULL;

    while(!pktl) {
        WaitForSingleObject(ctx->mutex, INFINITE);
        pktl = ctx->pktl;
        if(ctx->pktl) {
            *pkt = ctx->pktl->pkt;
            ctx->pktl = ctx->pktl->next;
            av_free(pktl);
        }
        ResetEvent(ctx->event);
        ReleaseMutex(ctx->mutex);
        if(!pktl) {
            if(s->flags & AVFMT_FLAG_NONBLOCK) {
                return AVERROR(EAGAIN);
            } else {
                WaitForSingleObject(ctx->event, INFINITE);
            }
        }
    }

    ctx->curbufsize -= pkt->size;

    return pkt->size;
}

static int vfw_read_close(AVFormatContext *s)
{
    struct vfw_ctx *ctx = s->priv_data;

    if(ctx->hwnd) {
        SendMessage(ctx->hwnd, WM_CAP_SET_CALLBACK_VIDEOSTREAM, 0, 0);
        SendMessage(ctx->hwnd, WM_CAP_DRIVER_DISCONNECT, 0, 0);
        DestroyWindow(ctx->hwnd);
    }
    if(ctx->mutex)
        CloseHandle(ctx->mutex);
    if(ctx->event)
        CloseHandle(ctx->event);

    return 0;
}

AVInputFormat vfwcap_demuxer = {
    "vfwcap",
    NULL_IF_CONFIG_SMALL("VFW video capture"),
    sizeof(struct vfw_ctx),
    NULL,
    vfw_read_header,
    vfw_read_packet,
    vfw_read_close,
    .flags = AVFMT_NOFILE,
};
