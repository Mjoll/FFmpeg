/*
 * Bifrost Frames shared memory input/output device
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

#include <bifrost_frames.h>

#include "libavutil/error.h"
#include "libavformat/avformat.h"

#include "bifrost_frames_common.h"

int ff_bifrost_frames_status_to_av_error(enum BifrostStatus status)
{
    int err;

    switch (status) {
    case BIFROST_STATUS_OK:
        return 0;
    case BIFROST_STATUS_INTERRUPTED:
        /* Requested by the user, not worth an error message. */
        return AVERROR_EXIT;
    case BIFROST_STATUS_INVALID_ARGUMENT:
    case BIFROST_STATUS_UTF8_ERROR:
    case BIFROST_STATUS_BAD_ENUM_VALUE:
    case BIFROST_STATUS_BAD_RESOLUTION:
    case BIFROST_STATUS_BAD_FRAME_RATE:
    case BIFROST_STATUS_BAD_SAMPLE_RATE:
    case BIFROST_STATUS_STREAM_FORMAT_CHANGE:
        err = AVERROR(EINVAL);
        break;
    case BIFROST_STATUS_NOT_FOUND:
        err = AVERROR(ENOENT);
        break;
    case BIFROST_STATUS_ALREADY_EXISTS:
        err = AVERROR(EEXIST);
        break;
    case BIFROST_STATUS_TIMEOUT:
        err = AVERROR(ETIMEDOUT);
        break;
    case BIFROST_STATUS_QUEUE_CLOSED:
        err = AVERROR(EPIPE);
        break;
    case BIFROST_STATUS_NOT_A_FRAME:
    case BIFROST_STATUS_NOT_A_FRAME_QUEUE:
    case BIFROST_STATUS_NOT_A_STREAM:
    case BIFROST_STATUS_CORRUPT_STREAM:
        err = AVERROR_INVALIDDATA;
        break;
    default:
        err = AVERROR_EXTERNAL;
        break;
    }
    return err;
}

static uint32_t bifrost_interrupt_check(void *opaque)
{
    AVFormatContext *avctx = opaque;
    return avctx->interrupt_callback.callback(avctx->interrupt_callback.opaque);
}

int ff_bifrost_register_interrupt_callback(AVFormatContext *avctx, BifrostStream stream)
{
    int ret = 0;
    if (avctx->interrupt_callback.callback) {
        BIFROST_CHECK(bifrost_stream_register_interrupt_callback(
            stream,
            bifrost_interrupt_check,
            avctx,
            100 // check every 100ms
        ));
    }
end:
    return ret;
}
