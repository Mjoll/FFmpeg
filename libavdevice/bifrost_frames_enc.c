/*
 * Bifrost Frames shared memory output device
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

#include "libavcodec/codec_id.h"
#include "libavcodec/defs.h"
#include "libavcodec/packet.h"
#include "libavcodec/packet_internal.h"
#include "libavutil/avutil.h"
#include "libavutil/log.h"
#include "libavutil/mathematics.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/fifo.h"
#include "libavutil/pixdesc.h"
#include "libavutil/pixfmt.h"

#include "libavformat/avformat.h"
#include "libavformat/mux.h"
#include "libavutil/rational.h"

#include <bifrost_frames.h>

#include "bifrost_frames_common.h"

struct AudioState {
    int stream_index;
    int64_t end_offset;

    /**
     *  The size of one sample for each channel.
     */
    size_t fifo_element_size;
    AVRational time_base;
    AVFifo *fifo;
};

static int64_t available_audio_samples(struct AudioState *audio)
{
    return av_fifo_can_read(audio->fifo);
}

struct VideoState {
    int stream_index;
    /**
     *  The expected next frame to be put into the packet list
     */
    uint64_t expected_frame_number;

    /**
     *  Number of frames currently in the packet list
     */
    AVRational time_base;
    struct PacketList queue;
    uint64_t pending_frame_count;
};

typedef struct BifrostFramesMuxContext {
    const AVClass *class;

    int create;
    int queue_size;
    int pool_size;
    int audio_repair_threshold;
    int max_held_video_frames;
    int close_on_end;
    int64_t start_frame_number;

    int64_t timeout_us;

    BifrostStream stream;
    BifrostFrame frame;
    uint64_t frame_number;

    struct BifrostFraction frame_rate;
    struct BifrostVideoInfo video_info;
    struct BifrostAudioInfo audio_info;

    /**
     *  Packets needed to be held back until we figured out the overall stream
     *  timing. Only used duration initialization.
     */
    struct PacketList init_packets;

    int main_stream_index;
    AVRational main_stream_time_base;
    int64_t first_pts;

    struct VideoState video;
    struct AudioState audio;

    int push_failed;
} BifrostFramesMuxContext;

static int av_pixel_format_to_bifrost(enum AVPixelFormat format,
                                      enum BifrostPixelFormat *bifrost_pixel_format)
{
    int ret = 0;
    switch (format) {
    case AV_PIX_FMT_RGBA:
        *bifrost_pixel_format = BIFROST_PIXEL_FORMAT_RGBA8;
        break;
    case AV_PIX_FMT_RGBA64:
        *bifrost_pixel_format = BIFROST_PIXEL_FORMAT_RGBA16;
        break;
    default:
        ret = AVERROR(EINVAL);
        break;
    }
    return ret;
}

static enum BifrostFieldOrder av_field_order_to_bifrost(AVFormatContext *avctx,
                                                        enum AVFieldOrder format)
{
    switch (format) {
    case AV_FIELD_PROGRESSIVE:
        return BIFROST_FIELD_ORDER_PROGRESSIVE;
    case AV_FIELD_TT:
    case AV_FIELD_TB:
        return BIFROST_FIELD_ORDER_TOP_FIELD_FIRST;
    case AV_FIELD_BB:
    case AV_FIELD_BT:
        return BIFROST_FIELD_ORDER_BOTTOM_FIELD_FIRST;
    default:
        av_log(avctx, AV_LOG_WARNING, "Missing field order information. Assuming progressive.\n");
        return BIFROST_FIELD_ORDER_PROGRESSIVE;
    }
}

static int initialize_video(AVFormatContext *avctx, AVStream *st,
                            BifrostFramesMuxContext *ctx)
{
    int ret = 0;

    if (ctx->video.stream_index != -1) {
        av_log(avctx, AV_LOG_ERROR, "Bifrost output device only supports a single video stream\n");
        ret = AVERROR(EINVAL);
        goto end;
    }
    ctx->video.stream_index = st->index;
    ctx->main_stream_index = ctx->video.stream_index;

    ctx->video_info.resolution.width = st->codecpar->width;
    ctx->video_info.resolution.height = st->codecpar->height;
    if (st->codecpar->sample_aspect_ratio.num != 0) {
        ctx->video_info.resolution.pixel_aspect_ratio.numerator = st->codecpar->sample_aspect_ratio.num;
        ctx->video_info.resolution.pixel_aspect_ratio.denominator = st->codecpar->sample_aspect_ratio.den;
    } else {
        ctx->video_info.resolution.pixel_aspect_ratio.numerator = 1;
        ctx->video_info.resolution.pixel_aspect_ratio.denominator = 1;
    }

    ctx->main_stream_time_base.num = st->time_base.num;
    ctx->main_stream_time_base.den = st->time_base.den;
    ctx->video.time_base.num = st->time_base.num;
    ctx->video.time_base.den = st->time_base.den;

    ctx->frame_rate.numerator = st->avg_frame_rate.num;
    ctx->frame_rate.denominator = st->avg_frame_rate.den;

    if ((ctx->frame_rate.numerator == 0) || (ctx->frame_rate.denominator == 0)) {
        av_log(avctx, AV_LOG_ERROR, "Invalid frame rate: %u/%u\n",
               ctx->frame_rate.numerator, ctx->frame_rate.denominator);
        ret = AVERROR(EINVAL);
        goto end;
    }

    ret = av_pixel_format_to_bifrost(st->codecpar->format, &ctx->video_info.pixel_format);
    if (ret != 0) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported pixel format: %s\n",
               av_get_pix_fmt_name(st->codecpar->format));
        goto end;
    }

    ctx->video_info.field_order = av_field_order_to_bifrost(avctx, st->codecpar->field_order);
end:
    return ret;
}

static int av_codec_id_to_bifrost_audio_format(enum AVCodecID codec_id,
                                               enum BifrostAudioFormat *format)
{
    int ret = 0;
    switch (codec_id) {
#if AV_HAVE_BIGENDIAN
    case AV_CODEC_ID_PCM_S16BE:
        *format = BIFROST_AUDIO_FORMAT_S16;
        break;
    case AV_CODEC_ID_PCM_F32BE:
        *format = BIFROST_AUDIO_FORMAT_F32;
        break;
#else
    case AV_CODEC_ID_PCM_S16LE:
        *format = BIFROST_AUDIO_FORMAT_S16;
        break;
    case AV_CODEC_ID_PCM_F32LE:
        *format = BIFROST_AUDIO_FORMAT_F32;
        break;
#endif
    default:
        ret = AVERROR(EINVAL);
        break;
    }
    return ret;
}

static int initialize_audio(AVFormatContext *avctx, AVStream *st,
                            BifrostFramesMuxContext *ctx)
{
    int ret = 0;

    if (ctx->audio.stream_index != -1) {
        av_log(avctx, AV_LOG_ERROR, "Bifrost output devices only supports a single audio stream.\n");
        ret = AVERROR(EINVAL);
        goto end;
    }

    ctx->audio.stream_index = st->index;
    ctx->audio.time_base = st->time_base;

    if (ctx->main_stream_index == -1) {
        ctx->main_stream_index = st->index;
        ctx->main_stream_time_base = st->time_base;
    }

    ctx->audio_info.sample_rate = st->codecpar->sample_rate;
    ctx->audio_info.channel_count = st->codecpar->ch_layout.nb_channels;

    ret = av_codec_id_to_bifrost_audio_format(st->codecpar->codec_id, &ctx->audio_info.format);
    if (ret != 0) {
        av_log(avctx, AV_LOG_ERROR, "Unspported audio codec: %s\n",
               avcodec_get_name(st->codecpar->codec_id));
        goto end;
    }

    ctx->audio.end_offset = 0;

    size_t element_size = 0;
    switch (ctx->audio_info.format) {
    case BIFROST_AUDIO_FORMAT_S16:
        element_size = 2 * ctx->audio_info.channel_count;
        break;
    case BIFROST_AUDIO_FORMAT_F32:
        element_size = 4 * ctx->audio_info.channel_count;
        break;
    default:
        av_log(avctx, AV_LOG_ERROR, "Unhandled Bifrost audio format\n");
        ret = AVERROR(EINVAL);
        goto end;
    }

    ctx->audio.fifo_element_size = element_size;
    ctx->audio.fifo = av_fifo_alloc2(480000, element_size, 0);
    if (ctx->audio.fifo == NULL) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

end:
    return ret;
}

static int bifrost_frames_write_header(AVFormatContext *avctx)
{
    int ret = 0;
    BifrostFramesMuxContext *ctx = avctx->priv_data;

    if (ctx == NULL) {
        av_log(avctx, AV_LOG_ERROR, "Missing private data.\n");
        ret = AVERROR(EINVAL);
        goto end;
    }

    ctx->video.stream_index = -1;
    ctx->audio.stream_index = -1;
    ctx->main_stream_index = -1;

    ctx->first_pts = AV_NOPTS_VALUE;
    ctx->main_stream_time_base = av_make_q(0, 1);

    for (unsigned int i = 0; i < avctx->nb_streams; i++) {
        AVStream *st = avctx->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            ret = initialize_video(avctx, st, ctx);
            if (ret != 0) {
                av_log(avctx, AV_LOG_ERROR, "Failed to initialize video.\n");
                goto end;
            }
        }

        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            ret = initialize_audio(avctx, st, ctx);
            if (ret != 0) {
                av_log(avctx, AV_LOG_ERROR, "Failed to initialize audio.\n");
                goto end;
            }
        }
    }
    if (ctx->video.stream_index == -1 && ctx->audio.stream_index == -1) {
        av_log(avctx, AV_LOG_ERROR, "No video/audio streams in output.\n");
        ret = AVERROR(EINVAL);
        goto end;
    }

    if (ctx->video.stream_index == -1) {
        ctx->frame_rate.numerator = 25;
        ctx->frame_rate.denominator = 1;
    }

    if (ctx->create == 1) {
        BIFROST_CHECK(bifrost_stream_create(avctx->url, ctx->queue_size, ctx->pool_size, &ctx->stream));
    } else {
        BIFROST_CHECK(bifrost_stream_open(avctx->url, &ctx->stream));
    }

    ret = ff_bifrost_register_interrupt_callback(avctx, ctx->stream);
    if (ret != 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to register interrupt callback.\n");
        goto end;
    }
end:
    return ret;
}

static int handle_video_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    int ret = 0;
    AVPacket *tmp_pkt = NULL;

    BifrostFramesMuxContext *ctx = avctx->priv_data;
    av_log(avctx, AV_LOG_DEBUG, "handle_video_packet: pkt->pts=%"PRId64", ctx->first_pts=%"PRId64"\n",
           pkt->pts, ctx->first_pts);

    tmp_pkt = av_packet_alloc();
    if (tmp_pkt == NULL) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    if (ctx->first_pts == AV_NOPTS_VALUE) {
        av_log(avctx, AV_LOG_VERBOSE, "First pts not set.\n");
        ret = av_packet_ref(tmp_pkt, pkt);
        if (ret != 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed to reference incoming packet.");
            goto end;
        }

        ret = avpriv_packet_list_put(&ctx->video.queue, tmp_pkt, NULL, 0);
        if (ret != 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed to put video packet in packet queue\n");
            goto end;
        }
        ctx->video.expected_frame_number = 1;
        ctx->video.pending_frame_count = 1;
        ctx->first_pts = pkt->pts;
        av_log(avctx, AV_LOG_VERBOSE, "Setting first_pts from video: %"PRId64"\n", pkt->pts);
        goto end;
    }

    // The video is always the main time base so there is no need to convert
    // first_pts to the main time base
    int64_t relative_pts = pkt->pts - ctx->first_pts;

    AVRational frame_rate_time_base = av_make_q(ctx->frame_rate.denominator, ctx->frame_rate.numerator);

    int64_t frame_number = av_rescale_q(relative_pts, ctx->video.time_base, frame_rate_time_base);

    av_log(avctx, AV_LOG_DEBUG, "frame_number = %"PRId64", relative_pts = %"PRId64", "
           "video_time_base=%d/%d, frame_rate_time_base=%d/%d\n",
           frame_number, relative_pts, ctx->video.time_base.num, ctx->video.time_base.den,
           frame_rate_time_base.num, frame_rate_time_base.den);

    if (frame_number < ctx->video.expected_frame_number) {
        av_log(avctx, AV_LOG_WARNING, "Dropping video frame\n");
        goto end;
    }

    if (frame_number > ctx->video.expected_frame_number) {
        av_log(avctx, AV_LOG_WARNING, "Frame duplicated %"PRId64" time(s) to fill gap in video timestamps\n",
               frame_number - ctx->video.expected_frame_number);
    }

    while (frame_number >= ctx->video.expected_frame_number) {
        ret = av_packet_ref(tmp_pkt, pkt);
        if (ret != 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed to reference incoming packet.");
            goto end;
        }
        ret = avpriv_packet_list_put(&ctx->video.queue, tmp_pkt, NULL, 0);
        if (ret != 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed to put video packet in packet queue\n");
            goto end;
        }
        ctx->video.expected_frame_number++;
        ctx->video.pending_frame_count++;
    }
end:
    av_packet_free(&tmp_pkt);
    return ret;
}

static int handle_audio_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    int ret = 0;
    void *silence_buffer = NULL;
    uint8_t *src_data = NULL;
    BifrostFramesMuxContext *ctx = avctx->priv_data;

    av_log(avctx, AV_LOG_DEBUG, "handle_audio_packet: pkt->pts = %"PRId64"\n", pkt->pts);

    int64_t audio_first_pts = av_rescale_q(ctx->first_pts, ctx->main_stream_time_base, ctx->audio.time_base);

    int64_t relative_pts = pkt->pts - audio_first_pts;

    AVRational sample_rate_time_base = av_make_q(1, ctx->audio_info.sample_rate);

    int64_t sample_offset = av_rescale_q(relative_pts, ctx->audio.time_base, sample_rate_time_base);

    if (pkt->size % ctx->audio.fifo_element_size != 0) {
        av_log(avctx, AV_LOG_ERROR, "Expected incomming audio packet size to be divisible by %zu (size=%d)\n",
               ctx->audio.fifo_element_size, pkt->size);
        ret = AVERROR(EINVAL);
        goto end;
    }

    int64_t sample_count = pkt->size / ctx->audio.fifo_element_size;

    int64_t max_pts = ctx->audio.end_offset + ctx->audio_repair_threshold;
    int64_t min_pts = ctx->audio.end_offset - ctx->audio_repair_threshold;

    src_data = pkt->data;

    if (max_pts < sample_offset) {
        int64_t missing_samples = sample_offset - ctx->audio.end_offset;
        av_log(avctx, AV_LOG_WARNING,
               "Expected audio with sample offset %"PRId64", but got %"PRId64". "
               "Inserting %"PRId64" samples of silence.\n",
               ctx->audio.end_offset, sample_offset, missing_samples);
        size_t silence_buffer_size = missing_samples * ctx->audio.fifo_element_size;
        silence_buffer = av_mallocz(silence_buffer_size);
        if (silence_buffer == NULL) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
        ctx->audio.end_offset += missing_samples;
        ret = av_fifo_write(ctx->audio.fifo, silence_buffer, missing_samples);
        if (ret != 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed to populate audio FIFO with silence\n");
            ret = AVERROR(EINVAL);
            goto end;
        }
        av_freep(&silence_buffer);
    } else if (min_pts > sample_offset) {
        int64_t trim_sample_count = ctx->audio.end_offset - sample_offset;
        if (trim_sample_count >= sample_count) {
            av_log(avctx, AV_LOG_WARNING, "Entire audio packet dropped\n");
            goto end;
        }
        av_log(avctx, AV_LOG_WARNING,
               "Expected audio with sample offset %"PRId64", but got %"PRId64". "
               "Triming %"PRId64" sample from incoming buffer.\n",
               ctx->audio.end_offset, sample_offset, trim_sample_count);
        size_t trim_byte_count = trim_sample_count * ctx->audio.fifo_element_size;
        src_data += trim_byte_count;
        sample_count -= trim_sample_count;
    }

    ret = av_fifo_write(ctx->audio.fifo, src_data, sample_count);
    if (ret != 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to populate audio FIFO\n");
        ret = AVERROR(EINVAL);
        goto end;
    }

    ctx->audio.end_offset += sample_count;
end:
    if (silence_buffer) {
        av_freep(&silence_buffer);
    }
    return ret;
}

static int handle_or_defer_audio_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    int ret = 0;
    AVPacket *tmp_pkt = NULL;
    BifrostFramesMuxContext *ctx = avctx->priv_data;

    av_log(avctx, AV_LOG_DEBUG, "handle_or_defer_audio_packet: pkt->pts = %"PRId64"\n", pkt->pts);

    tmp_pkt = av_packet_alloc();
    if (tmp_pkt == NULL) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    // Only use audio for first_pts if it is the main stream (i.e no video)
    if (pkt->stream_index == ctx->main_stream_index) {
        if (ctx->first_pts == AV_NOPTS_VALUE) {
            ctx->first_pts = pkt->pts;
        }
    }

    if (ctx->first_pts == AV_NOPTS_VALUE) {
        ret = av_packet_ref(tmp_pkt, pkt);
        if (ret != 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed to reference incoming packet.");
            goto end;
        }
        // Since we are repairing the audio before the audio queue (trimming
        // overlapping packet / filling gaps with silence) it also makes sense
        // to fix the alignment of the first audio and first video in the same
        // place. Because of this, we cannot put audio packets into the queue
        // until we know the first_pts which is determined by the first video
        // packet. Until we have this we defer the audio packet handling and
        // put the packets in an init queue.
        ret = avpriv_packet_list_put(&ctx->init_packets, tmp_pkt, NULL, 0);
        if (ret != 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed to put audio packet in deferred init packet list\n");
            goto end;
        }

        av_log(avctx, AV_LOG_VERBOSE, "Cannot handle audio yet. First pts not determined\n");

        goto end;
    }

    if (ctx->init_packets.head != NULL) {
        while (avpriv_packet_list_get(&ctx->init_packets, tmp_pkt) == 0) {
            ret = handle_audio_packet(avctx, tmp_pkt);
            if (ret != 0) {
                av_log(avctx, AV_LOG_ERROR, "Failed to handle deferred audio packet\n");
                goto end;
            }
            av_packet_unref(tmp_pkt);
        }
    }

    ret = handle_audio_packet(avctx, pkt);
    if (ret != 0) {
        goto end;
    }

end:
    av_packet_free(&tmp_pkt);

    return ret;
}

static int build_and_dispatch_frame(AVFormatContext *avctx, int flush)
{
    int ret = 0;
    AVPacket *tmp_pkt = NULL;

    BifrostFrameLock lock = NULL;
    struct BifrostVideoBuffer video_buffer = { 0 };
    struct BifrostAudioBuffer audio_buffer = { 0 };
    int64_t audio_samples_to_take = 0;
    struct BifrostAudioBuffer *audio_buffer_ptr = NULL;
    BifrostFramesMuxContext *ctx = avctx->priv_data;

    int has_video = (ctx->video.stream_index != -1);
    int has_audio = (ctx->audio.stream_index != -1);

    if (ctx->frame == NULL) {
        struct BifrostFrameInfo frame_info = { 0 };

        if (ctx->frame_rate.numerator == 0) {
            ret = 1;
            // Not ready to start building frames yet.
            goto end;
        }

        frame_info.frame_rate = ctx->frame_rate;

        if (has_video) {
            frame_info.video = &ctx->video_info;
        }
        if (has_audio) {
            frame_info.audio = &ctx->audio_info;
        }

        BIFROST_CHECK(bifrost_stream_allocate_frame(ctx->stream, &frame_info, &ctx->frame));
    }

    BIFROST_CHECK(bifrost_frame_lock(ctx->frame, &lock));
    BIFROST_CHECK(bifrost_frame_lock_set_frame_number(lock, ctx->frame_number + ctx->start_frame_number));

    if (has_audio) {
        BIFROST_CHECK(bifrost_frame_lock_query_audio_buffer(lock, &audio_buffer));
        audio_buffer_ptr = &audio_buffer;
        uint64_t available_samples = available_audio_samples(&ctx->audio);
        audio_samples_to_take = FFMIN(available_samples, audio_buffer.sample_count);

        // If audio only we only pad with silence if there is at least 1 available
        // sample. Otherwise there would be nothing ever stopping it.
        int should_pad_with_silence = (flush && ((available_samples > 0) || has_video));

        int force_emit_frame = (ctx->video.pending_frame_count > ctx->max_held_video_frames);
        if (force_emit_frame) {
            av_log(avctx, AV_LOG_WARNING,
                   "Max number of video frames held while waiting for audio (%d). Adding silence.\n",
                   ctx->max_held_video_frames);
        }

        int has_enough_samples = (available_samples >= audio_buffer.sample_count);
        if (!has_enough_samples && !should_pad_with_silence && !force_emit_frame) {
            av_log(avctx, AV_LOG_VERBOSE, "Need %"PRIu64" more audio samples to emit a frame.\n",
                   audio_buffer.sample_count - available_samples);
            ret = 1;
            goto end;
        }
    }

    if (has_video) {
        tmp_pkt = av_packet_alloc();
        if (tmp_pkt == NULL) {
            ret = AVERROR(ENOMEM);
            goto end;
        }

        ret = avpriv_packet_list_get(&ctx->video.queue, tmp_pkt);
        if (ret == AVERROR(EAGAIN)) {
            av_log(avctx, AV_LOG_INFO, "No video frames queued to produce a frame\n");
            ret = 1;
            goto end;
        }

        ctx->video.pending_frame_count--;

        BIFROST_CHECK(bifrost_frame_lock_query_video_buffer(lock, &video_buffer));

        if (tmp_pkt->size != video_buffer.size) {
            av_log(avctx, AV_LOG_ERROR, "Unexpected video packet size %d. Expected %zu",
                   tmp_pkt->size, video_buffer.size);
            ret = AVERROR(EINVAL);
            goto end;
        }

        memcpy(video_buffer.ptr, tmp_pkt->data, video_buffer.size);

        av_packet_unref(tmp_pkt);
    }

    if (audio_buffer_ptr != NULL) {
        ret = av_fifo_read(ctx->audio.fifo, audio_buffer_ptr->ptr, audio_samples_to_take);
        if (ret != 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed to read %"PRIu64" audio samples from queue\n",
                   audio_buffer_ptr->sample_count);
            goto end;
        }
        uint64_t samples_to_pad = (audio_buffer.sample_count - audio_samples_to_take);
        if (samples_to_pad > 0) {
            av_log(avctx, AV_LOG_WARNING, "Padding frame with %"PRIu64" samples\n", samples_to_pad);
            size_t offset = audio_samples_to_take * ctx->audio.fifo_element_size;
            size_t byte_count = samples_to_pad * ctx->audio.fifo_element_size;
            memset(audio_buffer_ptr->ptr + offset, 0, byte_count);

            // Padding with silence is basically the same as putting silence into the
            // audio queue which is empty (otherwise padding wouldn't happen). In order
            // to avoid A/V desync we therefore increment the audio queue end offset.
            ctx->audio.end_offset += samples_to_pad;
        }
    }

    BIFROST_CHECK(bifrost_frame_unlock(&lock));

    ctx->push_failed = 1;
    BIFROST_CHECK(bifrost_stream_push(ctx->stream, ctx->timeout_us / 1000, &ctx->frame));
    ctx->push_failed = 0;

    av_log(avctx, AV_LOG_VERBOSE, "Pushed frame %"PRIu64".\n", ctx->frame_number);

    ctx->frame_number++;
end:
    av_packet_free(&tmp_pkt);

    if (lock != NULL) {
        BIFROST_LOG(bifrost_frame_unlock(&lock));
    }

    return ret;
}

static int bifrost_frames_write_packet(AVFormatContext *avctx, AVPacket *pkt)
{
    int ret = 0;
    BifrostFramesMuxContext *ctx = avctx->priv_data;

    if (pkt->pts == AV_NOPTS_VALUE) {
        av_log(avctx, AV_LOG_ERROR, "Packet is missing pts value.\n");
        ret = AVERROR(EINVAL);
        goto end;
    }

    if (pkt->stream_index == ctx->audio.stream_index) {
        ret = handle_or_defer_audio_packet(avctx, pkt);
        if (ret != 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed to handle audio packet.\n");
            goto end;
        }
    }

    if (pkt->stream_index == ctx->video.stream_index) {
        ret = handle_video_packet(avctx, pkt);
        if (ret != 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed to handle video packet.\n");
            goto end;
        }
    }

    while ((ret = build_and_dispatch_frame(avctx, 0)) == 0) { }

    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to build and dispatch frame.\n");
        goto end;
    }

    ret = 0;

end:
    return ret;
}

static int bifrost_frames_write_trailer(AVFormatContext *avctx)
{
    int ret = 0;
    BifrostFramesMuxContext *ctx = avctx->priv_data;

    if (ctx->stream != NULL) {
        if (ctx->push_failed == 0) {
            av_log(avctx, AV_LOG_VERBOSE, "Flushing.\n");

            while ((ret = build_and_dispatch_frame(avctx, 1)) == 0) { }

            if (ret < 0) {
                av_log(avctx, AV_LOG_ERROR, "Failed to flush\n");
                goto end;
            }

            ret = 0;

            // TODO: Should have a bifrost_stream_close without destroy function so
            // that we can signal to the readers that the stream is done while
            // waiting the stream to be drained. Technically not required but it
            // makes sense either way.

            if (ctx->create == 1) {
                // If we are the creator / owner of the stream we want to wait for consumers to
                // have fully drained the queue before we terminate.
                BIFROST_CHECK(bifrost_stream_wait_until_drained(ctx->stream, ctx->timeout_us / 1000));
            }

            if (ctx->close_on_end == 1) {
                BIFROST_CHECK(bifrost_stream_close(ctx->stream));
            }
        }
    }
end:
    return ret;
}

static void bifrost_frames_deinit(AVFormatContext *avctx)
{
    BifrostFramesMuxContext *ctx = avctx->priv_data;

    avpriv_packet_list_free(&ctx->init_packets);
    avpriv_packet_list_free(&ctx->video.queue);

    if (ctx->audio.fifo != NULL) {
        av_fifo_freep2(&ctx->audio.fifo);
    }

    if (ctx->stream != NULL) {
        if (ctx->frame != NULL) {
            BIFROST_LOG(bifrost_frame_release(ctx->stream, &ctx->frame));
        }
        av_log(avctx, AV_LOG_INFO, "Destroying stream\n");
        BIFROST_LOG(bifrost_stream_clear_interrupt_callback(ctx->stream));
        BIFROST_LOG(bifrost_stream_destroy(&ctx->stream));
    }
}

#define OFFSET(x) offsetof(BifrostFramesMuxContext, x)
#define ENC AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "create",                 "create the stream instead of opening it",               OFFSET(create),                 AV_OPT_TYPE_BOOL,     { .i64 = 1 },       0,         1,   ENC },
    { "queue_size",             "queue size when creating the stream",                   OFFSET(queue_size),             AV_OPT_TYPE_INT,      { .i64 = 5 },       1,   INT_MAX,   ENC },
    { "pool_size",              "initial frame pool size when creating the stream",      OFFSET(pool_size),              AV_OPT_TYPE_INT,      { .i64 = 10 },      1,   INT_MAX,   ENC },
    { "audio_repair_threshold", "number of audio samples mismatch before (pad/trim)",    OFFSET(audio_repair_threshold), AV_OPT_TYPE_INT,      { .i64 = 10 },      0,   INT_MAX,   ENC },
    { "timeout",                "time to wait for a free queue slot",                    OFFSET(timeout_us),             AV_OPT_TYPE_DURATION, { .i64 = 5000000 }, 0, INT64_MAX,   ENC },
    { "max_held_video_frames",  "max video frames held until silent audio is generated", OFFSET(max_held_video_frames),  AV_OPT_TYPE_INT,      { .i64 = 100 },     2,   INT_MAX,   ENC },
    { "close_on_end",           "mark end of stream on close",                           OFFSET(close_on_end),           AV_OPT_TYPE_INT,      { .i64 = 0 },       0,         1,   ENC },
    { "start_frame_number",     "set the frame number of the first written frame",       OFFSET(start_frame_number),     AV_OPT_TYPE_INT,      { .i64 = 0 },       0, INT64_MAX,   ENC },
    { NULL },
};

static const AVClass bifrost_frames_muxer_class = {
    .class_name = "bifrost_frames outdev",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
    .category   = AV_CLASS_CATEGORY_DEVICE_OUTPUT,
};

const FFOutputFormat ff_bifrost_frames_muxer = {
    .p.name         = "bifrost_frames",
    .p.long_name    = NULL_IF_CONFIG_SMALL("Bifrost Frames shared memory output"),
    .p.audio_codec  = AV_NE(AV_CODEC_ID_PCM_S16BE, AV_CODEC_ID_PCM_S16LE),
    .p.video_codec  = AV_CODEC_ID_RAWVIDEO,
    .p.flags        = AVFMT_NOFILE,
    .p.priv_class   = &bifrost_frames_muxer_class,
    .priv_data_size = sizeof(BifrostFramesMuxContext),
    .flags_internal = FF_OFMT_FLAG_MAX_ONE_OF_EACH,
    .write_header   = bifrost_frames_write_header,
    .write_packet   = bifrost_frames_write_packet,
    .write_trailer  = bifrost_frames_write_trailer,
    .deinit         = bifrost_frames_deinit,
};
