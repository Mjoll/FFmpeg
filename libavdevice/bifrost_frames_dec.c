/*
 * Bifrost Frames shared memory input device
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

#include <string.h>

#include "libavcodec/codec_id.h"
#include "libavcodec/defs.h"
#include "libavcodec/packet.h"
#include "libavcodec/packet_internal.h"
#include "libavutil/channel_layout.h"
#include "libavutil/log.h"
#include "libavutil/opt.h"

#include "libavformat/avformat.h"
#include "libavformat/demux.h"

#include "libavutil/pixfmt.h"

#include <bifrost_frames.h>

#include "bifrost_frames_common.h"

typedef struct BifrostFramesDemuxContext {
    const AVClass *class;

    int64_t timeout_us;
    int create;
    int queue_size;
    int pool_size;

    BifrostStream stream;

    int has_video;
    int has_audio;

    PacketList pkt_list;

    struct BifrostFrameQueryResult frame_query_result;
    struct BifrostVideoInfo video_info;
    struct BifrostAudioInfo audio_info;
} BifrostFramesDemuxContext;

static enum AVPixelFormat bifrost_pixel_format_to_av(AVFormatContext *avctx,
                                                     enum BifrostPixelFormat pixel_format)
{
    switch (pixel_format) {
    case BIFROST_PIXEL_FORMAT_RGBA8:  return AV_PIX_FMT_RGBA;
    case BIFROST_PIXEL_FORMAT_RGBA16: return AV_PIX_FMT_RGBA64;
    default:
        av_log(avctx, AV_LOG_ERROR, "Unsupported pixel format.\n");
        return AV_PIX_FMT_NONE;
    }
}

static enum AVFieldOrder bifrost_field_order_to_av(AVFormatContext *avctx,
                                                   enum BifrostFieldOrder field_order)
{
    switch (field_order) {
    case BIFROST_FIELD_ORDER_PROGRESSIVE:        return AV_FIELD_PROGRESSIVE;
    // TODO: Check which field order enums Claude used.
    case BIFROST_FIELD_ORDER_TOP_FIELD_FIRST:    return AV_FIELD_TT;
    case BIFROST_FIELD_ORDER_BOTTOM_FIELD_FIRST: return AV_FIELD_BB;
    default:
        av_log(avctx, AV_LOG_ERROR, "Unsupported field order.\n");
        return AV_FIELD_UNKNOWN;
    }
}

static enum AVCodecID bifrost_audio_format_to_av_codec_id(AVFormatContext *avctx,
                                                          enum BifrostAudioFormat audio_format)
{
    switch (audio_format) {
#if AV_HAVE_BIGENDIAN
    case BIFROST_AUDIO_FORMAT_F32: return AV_CODEC_ID_PCM_F32BE;
    case BIFROST_AUDIO_FORMAT_S16: return AV_CODEC_ID_PCM_S16BE;
#else
    case BIFROST_AUDIO_FORMAT_F32: return AV_CODEC_ID_PCM_F32LE;
    case BIFROST_AUDIO_FORMAT_S16: return AV_CODEC_ID_PCM_S16LE;
#endif
    default:
        av_log(avctx, AV_LOG_ERROR, "Unsupported audio format.\n");
        return AV_CODEC_ID_NONE;
    }
}

static int build_packets(AVFormatContext *avctx, PacketList *pkt_list,
                         int has_video, int has_audio, BifrostFrameLock lock)
{
    int ret = 0;
    int stream_index = 0;
    AVPacket *pkt = NULL;
    uint64_t frame_number = 0;

    pkt = av_packet_alloc();
    if (pkt == NULL) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    BIFROST_CHECK(bifrost_frame_lock_get_frame_number(lock, &frame_number));

    if (has_video) {
        struct BifrostVideoBuffer video_buffer = { 0 };
        BIFROST_CHECK(bifrost_frame_lock_query_video_buffer(lock, &video_buffer));

        ret = av_new_packet(pkt, video_buffer.size);
        if (ret != 0) {
            goto end;
        }
        memcpy(pkt->data, video_buffer.ptr, video_buffer.size);
        pkt->stream_index = stream_index;
        pkt->pts = frame_number;
        pkt->dts = frame_number;
        ret = avpriv_packet_list_put(pkt_list, pkt, NULL, 0);
        if (ret != 0) {
            goto end;
        }

        stream_index++;
    }

    if (has_audio) {
        struct BifrostAudioBuffer audio_buffer = { 0 };
        BIFROST_CHECK(bifrost_frame_lock_query_audio_buffer(lock, &audio_buffer));

        ret = av_new_packet(pkt, audio_buffer.size);
        if (ret != 0) {
            goto end;
        }
        memcpy(pkt->data, audio_buffer.ptr, audio_buffer.size);
        pkt->stream_index = stream_index;
        pkt->pts = audio_buffer.sample_offset;
        pkt->dts = audio_buffer.sample_offset;
        ret = avpriv_packet_list_put(pkt_list, pkt, NULL, 0);
        if (ret != 0) {
            goto end;
        }

        stream_index++;
    }
end:
    av_packet_free(&pkt);
    return ret;
}

static int bifrost_frames_read_header(AVFormatContext *avctx)
{
    int ret = 0;
    BifrostStream stream = NULL;
    BifrostFrame frame = NULL;
    BifrostFrameLock lock = NULL;
    BifrostFramesDemuxContext *ctx = avctx->priv_data;

    if (ctx == NULL) {
        av_log(avctx, AV_LOG_ERROR, "No private data\n");
        ret = AVERROR(EINVAL);
        goto end;
    }

    if (avctx->url == NULL) {
        av_log(avctx, AV_LOG_ERROR, "Missing URL\n");
        ret = AVERROR(EINVAL);
        goto end;
    }

    if (ctx->create) {
        BIFROST_CHECK(bifrost_stream_create(avctx->url, ctx->queue_size, ctx->pool_size, &stream));
    } else {
        BIFROST_CHECK(bifrost_stream_open(avctx->url, &stream));
    }

    ret = ff_bifrost_register_interrupt_callback(avctx, stream);
    if (ret != 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to register interrupt callback.\n");
        goto end;
    }

    BIFROST_CHECK(bifrost_stream_pop(stream, ctx->timeout_us / 1000, &frame));

    if (frame == NULL) {
        av_log(avctx, AV_LOG_ERROR, "End of stream encountered during initialization\n");
        ret = AVERROR_EOF;
        goto end;
    }

    BIFROST_CHECK(bifrost_frame_query(frame, &ctx->frame_query_result));

    if (ctx->frame_query_result.has_video) {
        AVStream *st = NULL;

        BIFROST_CHECK(bifrost_frame_query_video_info(frame, &ctx->video_info));

        st = avformat_new_stream(avctx, NULL);
        if (st == NULL) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
        st->codecpar->codec_type              = AVMEDIA_TYPE_VIDEO;
        st->codecpar->codec_id                = AV_CODEC_ID_RAWVIDEO;
        st->codecpar->framerate.num           = ctx->frame_query_result.frame_rate.numerator;
        st->codecpar->framerate.den           = ctx->frame_query_result.frame_rate.denominator;
        st->codecpar->width                   = ctx->video_info.resolution.width;
        st->codecpar->height                  = ctx->video_info.resolution.height;
        st->codecpar->sample_aspect_ratio.num = ctx->video_info.resolution.pixel_aspect_ratio.numerator;
        st->codecpar->sample_aspect_ratio.den = ctx->video_info.resolution.pixel_aspect_ratio.denominator;
        st->time_base.num    = st->codecpar->framerate.den;
        st->time_base.den    = st->codecpar->framerate.num;
        st->r_frame_rate.num = st->codecpar->framerate.num;
        st->r_frame_rate.den = st->codecpar->framerate.den;

        st->codecpar->format = bifrost_pixel_format_to_av(avctx, ctx->video_info.pixel_format);
        if (st->codecpar->format == AV_PIX_FMT_NONE) {
            ret = AVERROR(EINVAL);
            goto end;
        }

        st->codecpar->field_order = bifrost_field_order_to_av(avctx, ctx->video_info.field_order);
        if (st->codecpar->field_order == AV_FIELD_UNKNOWN) {
            ret = AVERROR(EINVAL);
            goto end;
        }

        ctx->has_video = 1;
    }

    if (ctx->frame_query_result.has_audio) {
        AVStream *st = NULL;
        BIFROST_CHECK(bifrost_frame_query_audio_info(frame, &ctx->audio_info));

        st = avformat_new_stream(avctx, NULL);
        if (st == NULL) {
            ret = AVERROR(ENOMEM);
            goto end;
        }

        st->codecpar->codec_id = bifrost_audio_format_to_av_codec_id(avctx, ctx->audio_info.format);

        if (st->codecpar->codec_id == AV_CODEC_ID_NONE) {
            ret = AVERROR(EINVAL);
            goto end;
        }

        st->codecpar->codec_type            = AVMEDIA_TYPE_AUDIO;
        st->codecpar->sample_rate           = ctx->audio_info.sample_rate;
        st->codecpar->ch_layout.nb_channels = ctx->audio_info.channel_count;
        st->time_base.num = 1;
        st->time_base.den = ctx->audio_info.sample_rate;

        ctx->has_audio = 1;
    }

    BIFROST_CHECK(bifrost_frame_lock(frame, &lock));

    ret = build_packets(avctx, &ctx->pkt_list, ctx->has_video, ctx->has_audio, lock);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to build packets from frame\n");
        goto end;
    }

    if (lock != NULL) {
        BIFROST_LOG(bifrost_frame_unlock(&lock));
    }

    if (frame != NULL) {
        BIFROST_LOG(bifrost_frame_release(stream, &frame));
    }

    ctx->stream = stream;
    stream = NULL;

end:
    if (lock != NULL) {
        BIFROST_LOG(bifrost_frame_unlock(&lock));
    }

    if (frame != NULL) {
        BIFROST_LOG(bifrost_frame_release(stream, &frame));
    }

    if (stream != NULL) {
        BIFROST_LOG(bifrost_stream_destroy(&stream));
    }

    return ret;
}

#define CHECK_EQUAL(field, what) \
    if (a->field != b->field) { \
        av_log(avctx, AV_LOG_ERROR, what " changed from %"PRId64" to %"PRId64"\n", \
               (int64_t)b->field, (int64_t)a->field); \
        return AVERROR(EINVAL); \
    }

static int check_frame_query_result_equal(AVFormatContext *avctx,
                                          struct BifrostFrameQueryResult *a,
                                          struct BifrostFrameQueryResult *b)
{
    CHECK_EQUAL(frame_rate.numerator, "Frame rate numerator");
    CHECK_EQUAL(frame_rate.denominator, "Frame rate denominator");
    return 0;
}

static int check_video_info_equal(AVFormatContext *avctx,
                                  struct BifrostVideoInfo *a,
                                  struct BifrostVideoInfo *b)
{
    CHECK_EQUAL(resolution.width, "Horizontal resolution");
    CHECK_EQUAL(resolution.height, "Vertical resolution");
    CHECK_EQUAL(resolution.pixel_aspect_ratio.numerator, "Pixel aspect ratio numerator");
    CHECK_EQUAL(resolution.pixel_aspect_ratio.denominator, "Pixel aspect ratio denominator");
    CHECK_EQUAL(pixel_format, "Pixel format");
    CHECK_EQUAL(field_order, "Field order");
    return 0;
}

static int check_audio_info_equal(AVFormatContext *avctx,
                                  struct BifrostAudioInfo *a,
                                  struct BifrostAudioInfo *b)
{
    CHECK_EQUAL(sample_rate, "Sample rate");
    CHECK_EQUAL(channel_count, "Number of audio channelsj");
    CHECK_EQUAL(format, "Audio format");
    return 0;
}

#undef CHECK_EQUAL

static int read_frame_and_build_packets(AVFormatContext *avctx,
                                        BifrostFramesDemuxContext *ctx)
{
    int ret = 0;
    BifrostFrame frame = NULL;
    BifrostFrameLock lock = NULL;
    struct BifrostVideoInfo video_info = { 0 };
    struct BifrostAudioInfo audio_info = { 0 };
    struct BifrostFrameQueryResult frame_query_result = { 0 };

    if (ctx->stream == NULL) {
        av_log(avctx, AV_LOG_ERROR, "Not initialized!\n");
        ret = AVERROR(EINVAL);
        goto end;
    }

    BIFROST_CHECK(bifrost_stream_pop(ctx->stream, ctx->timeout_us / 1000, &frame));

    if (frame == NULL) {
        av_log(avctx, AV_LOG_INFO, "Stream ended\n");
        ret = AVERROR_EOF;
        goto end;
    }

    BIFROST_CHECK(bifrost_frame_query(frame, &frame_query_result));

    ret = check_frame_query_result_equal(avctx, &frame_query_result, &ctx->frame_query_result);
    if (ret != 0) {
        av_log(avctx, AV_LOG_ERROR, "Changing format mid-stream is not supported.\n");
        goto end;
    }

    if (ctx->frame_query_result.has_video) {
        if (!frame_query_result.has_video) {
            av_log(avctx, AV_LOG_ERROR, "Stream changed from having video to not having video\n");
            ret = AVERROR(EINVAL);
            goto end;
        }
        BIFROST_CHECK(bifrost_frame_query_video_info(frame, &video_info));

        ret = check_video_info_equal(avctx, &video_info, &ctx->video_info);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "Changing format mid-stream is not supported.\n");
            goto end;
        }
    } else if (frame_query_result.has_video) {
        av_log(avctx, AV_LOG_ERROR, "Stream changed from not having video to having video\n");
        ret = AVERROR(EINVAL);
        goto end;
    }

    if (ctx->frame_query_result.has_audio) {
        if (!frame_query_result.has_audio) {
            av_log(avctx, AV_LOG_ERROR, "Stream changed from having audio to not having audio\n");
            ret = AVERROR(EINVAL);
            goto end;
        }
        BIFROST_CHECK(bifrost_frame_query_audio_info(frame, &audio_info));

        ret = check_audio_info_equal(avctx, &audio_info, &ctx->audio_info);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "Changing format mid-stream is not supported.\n");
            goto end;
        }
    } else if (frame_query_result.has_audio) {
        av_log(avctx, AV_LOG_ERROR, "Stream changed from not having audio to having audio\n");
        ret = AVERROR(EINVAL);
        goto end;
    }

    BIFROST_CHECK(bifrost_frame_lock(frame, &lock));

    ret = build_packets(avctx, &ctx->pkt_list, ctx->has_video, ctx->has_audio, lock);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to build packets from frame\n");
        goto end;
    }
end:
    if (lock != NULL) {
        BIFROST_LOG(bifrost_frame_unlock(&lock));
    }
    if (frame != NULL) {
        BIFROST_LOG(bifrost_frame_release(ctx->stream, &frame));
    }
    return ret;
}

static int bifrost_frames_read_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    int ret = 0;
    BifrostFramesDemuxContext *ctx = avctx->priv_data;

    if (ctx == NULL) {
        av_log(avctx, AV_LOG_ERROR, "No private data\n");
        ret = AVERROR(EINVAL);
        goto end;
    }

    while (avpriv_packet_list_get(&ctx->pkt_list, pkt) != 0) {
        ret = read_frame_and_build_packets(avctx, ctx);

        if (ret != 0) {
            goto end;
        }
    }
end:
    return ret;
}

static int bifrost_frames_read_close(AVFormatContext *avctx)
{
    int ret = AVERROR(EIO);
    BifrostFramesDemuxContext *ctx = avctx->priv_data;

    avpriv_packet_list_free(&ctx->pkt_list);

    if (ctx->stream != NULL) {
        BIFROST_CHECK(bifrost_stream_clear_interrupt_callback(ctx->stream));
        BIFROST_CHECK(bifrost_stream_destroy(&ctx->stream));
    }

    ret = 0;
end:
    return ret;
}

#define OFFSET(x) offsetof(BifrostFramesDemuxContext, x)
#define DEC AV_OPT_FLAG_DECODING_PARAM
static const AVOption options[] = {
    { "timeout",    "time to wait for a frame",                         OFFSET(timeout_us), AV_OPT_TYPE_DURATION, { .i64 = 5000000 }, 0, INT64_MAX, DEC },
    { "create",     "create the stream instead of opening it",          OFFSET(create),     AV_OPT_TYPE_BOOL,     { .i64 = 0 },       0, 1,         DEC },
    { "queue_size", "queue size when creating the stream",              OFFSET(queue_size), AV_OPT_TYPE_INT,      { .i64 = 5 },       1, INT_MAX,   DEC },
    { "pool_size",  "initial frame pool size when creating the stream", OFFSET(pool_size),  AV_OPT_TYPE_INT,      { .i64 = 10 },      1, INT_MAX,   DEC },
    { NULL },
};

static const AVClass bifrost_frames_demuxer_class = {
    .class_name = "bifrost_frames indev",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
    .category   = AV_CLASS_CATEGORY_DEVICE_INPUT,
};

const FFInputFormat ff_bifrost_frames_demuxer = {
    .p.name         = "bifrost_frames",
    .p.long_name    = NULL_IF_CONFIG_SMALL("Bifrost Frames shared memory input"),
    .p.flags        = AVFMT_NOFILE,
    .p.priv_class   = &bifrost_frames_demuxer_class,
    .priv_data_size = sizeof(BifrostFramesDemuxContext),
    .flags_internal = FF_INFMT_FLAG_INIT_CLEANUP,
    .read_header    = bifrost_frames_read_header,
    .read_packet    = bifrost_frames_read_packet,
    .read_close     = bifrost_frames_read_close,
};
