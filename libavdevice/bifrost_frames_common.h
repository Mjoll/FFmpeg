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

#ifndef AVDEVICE_BIFROST_FRAMES_COMMON_H
#define AVDEVICE_BIFROST_FRAMES_COMMON_H

#include <bifrost_frames.h>

#include "libavformat/avformat.h"

#define BIFROST_CHECK(x) do { \
    int ret2 = x; \
    if (ret2 != BIFROST_STATUS_OK) { \
        av_log(avctx, AV_LOG_ERROR, "Bifrost call to " #x " failed with return code: %d (%s)\n", \
               ret2, bifrost_status_to_string(ret2)); \
        ret = ff_bifrost_frames_status_to_av_error(ret2); \
        goto end; \
    } \
} while (0)

#define BIFROST_LOG(x) do { \
    int ret2 = x; \
    if (ret2 != BIFROST_STATUS_OK) { \
        av_log(avctx, AV_LOG_ERROR, "Bifrost call to " #x " failed with return code: %d (%s)\n", \
               ret2, bifrost_status_to_string(ret2)); \
    } \
} while (0)

int ff_bifrost_frames_status_to_av_error(enum BifrostStatus status);

int ff_bifrost_register_interrupt_callback(AVFormatContext *ctx, BifrostStream stream);

#endif /* AVDEVICE_BIFROST_FRAMES_COMMON_H */
