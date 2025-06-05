/*
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

#ifndef AVCODEC_H265_REXT_PROFILES_H
#define AVCODEC_H265_REXT_PROFILES_H

#define FF_HEVC_REXT_PROFILE_INVALID             -1

#define FF_HEVC_REXT_PROFILE_MONOCHROME         100
#define FF_HEVC_REXT_PROFILE_MONOCHROME_10      110
#define FF_HEVC_REXT_PROFILE_MONOCHROME_12      112
#define FF_HEVC_REXT_PROFILE_MONOCHROME_16      116

#define FF_HEVC_REXT_PROFILE_MAIN               200
#define FF_HEVC_REXT_PROFILE_MAIN_10            210
#define FF_HEVC_REXT_PROFILE_MAIN_12            212

#define FF_HEVC_REXT_PROFILE_MAIN422_10         310
#define FF_HEVC_REXT_PROFILE_MAIN422_12         312

#define FF_HEVC_REXT_PROFILE_MAIN444            400
#define FF_HEVC_REXT_PROFILE_MAIN444_HT         401
#define FF_HEVC_REXT_PROFILE_MAIN444_10         410
#define FF_HEVC_REXT_PROFILE_MAIN444_10_HT      411
#define FF_HEVC_REXT_PROFILE_MAIN444_12         412
#define FF_HEVC_REXT_PROFILE_MAIN444_14_HT      415
#define FF_HEVC_REXT_PROFILE_MAIN444_16         416
#define FF_HEVC_REXT_PROFILE_MAIN444_16_HT      417

#define FF_HEVC_REXT_PROFILE_SCC_MAIN           900
#define FF_HEVC_REXT_PROFILE_SCC_MAIN_10        910
#define FF_HEVC_REXT_PROFILE_SCC_MAIN444        940
#define FF_HEVC_REXT_PROFILE_SCC_MAIN444_HT     941
#define FF_HEVC_REXT_PROFILE_SCC_MAIN444_10     950
#define FF_HEVC_REXT_PROFILE_SCC_MAIN444_10_HT  951
#define FF_HEVC_REXT_PROFILE_SCC_MAIN444_14_HT  955

#endif
