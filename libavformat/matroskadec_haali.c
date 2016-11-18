/*
 * Matroska Demuxer based on Haalis MatroskaParser Library
 * Copyright (c) 2011-2012 Hendrik Leppkes
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

#include <stdio.h>
#include "MatroskaParser.h"

#include "avformat.h"
#include "internal.h"
#include "avio_internal.h"
/* For ff_codec_get_id(). */
#include "riff.h"
#include "isom.h"
#include "rm.h"
#include "matroska.h"
#include "libavcodec/bytestream.h"
#include "libavcodec/mpeg4audio.h"
#include "libavutil/intfloat.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/avstring.h"
#include "libavutil/dict.h"
#include "libavutil/mastering_display_metadata.h"
#if CONFIG_ZLIB
#include <zlib.h>
#endif

#include <io.h>
#include <windows.h>

#define IO_BUFFER_SIZE 32768

#define FF_PRI_UID "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x"
#define FF_ARG_UID(uid) (uint8_t)uid[0], (uint8_t)uid[1], (uint8_t)uid[2], (uint8_t)uid[3], \
                        (uint8_t)uid[4], (uint8_t)uid[5], (uint8_t)uid[6], (uint8_t)uid[7], \
                        (uint8_t)uid[8], (uint8_t)uid[9], (uint8_t)uid[10], (uint8_t)uid[11], \
                        (uint8_t)uid[12], (uint8_t)uid[13], (uint8_t)uid[14], (uint8_t)uid[15]

static const char *matroska_doctypes[] = { "matroska", "webm" };

typedef struct AVIOStream {
  InputStream base;
  AVIOContext *pb;
} AVIOStream;

typedef struct MatroskaTrack {
  TrackInfo *info;
  CompressedStream *cs;
  AVStream *stream;
  int ms_compat;
  int refresh_extradata;
} MatroskaTrack;

typedef struct MatroskaSegment {
  int index;
  AVIOStream   *iostream;
  MatroskaFile *matroska;
  SegmentInfo  *info;
  char          UID[16];
  int           free_avio;
  int           failed;
} MatroskaSegment;

typedef struct VirtualTimelineEntry {
  int index;
  Chapter *chapter;
  MatroskaSegment *segment;
  ulonglong start;
  ulonglong stop;
  longlong offset;
  int need_seek;
} VirtualTimelineEntry;

typedef struct MatroskaDemuxContext {
  MatroskaFile    *matroska;

  int num_tracks;
  ulonglong track_mask;
  MatroskaTrack *tracks;

  char CSBuffer[4096];
  unsigned BufferSize;

  Chapter **editions;
  AVEdition *aveditions;
  int num_editions;

  VirtualTimelineEntry *timeline;
  int num_timeline;

  int virtual_timeline;
  int timeline_position;

  Chapter *fake_edition;

  MatroskaSegment **segments;
  int num_segments;
  int segments_scanned;

  // Options
  int next_edition;
  int active_edition;
} MatroskaDemuxContext;

static int aviostream_read(struct AVIOStream *cc,ulonglong pos,void *buffer,int count)
{
  int ret;
  int64_t ret64, cur_pos;

  if (count == 0)
    return 0;

  cur_pos = avio_tell(cc->pb);
  if (cur_pos != pos) {
    /* Seek to the desired position */
    ret64 = avio_seek(cc->pb, pos, SEEK_SET);
    if(ret64 < 0) {
      av_log(cc->pb, AV_LOG_ERROR, "aviostream_scan(): Seek to %"PRIu64" failed with code %"PRId64, pos, ret64);
      return -1;
    }
  }

  /* Read the requested number of bytes */
  ret = avio_read(cc->pb, (unsigned char *)buffer, count);
  if (ret == AVERROR_EOF) {
    return 0;
  } else if (ret < 0) {
    return -1;
  }
  return ret;
}

static longlong aviostream_scan(struct AVIOStream *cc,ulonglong start,unsigned signature)
{
  int64_t ret64, cur_pos;
  unsigned cmp = 0;

  cur_pos = avio_tell(cc->pb);
  if (cur_pos != start) {
    /* Seek to the desired position */
    ret64 = avio_seek(cc->pb, start, SEEK_SET);
    if(ret64 < 0) {
      av_log(cc->pb, AV_LOG_ERROR, "aviostream_scan(): Seek to %"PRIu64" failed with code %"PRId64, start, ret64);
      return -1;
    }
  }

  /* Scan for the byte signature, until EOF was found */
  while(!cc->pb->eof_reached) {
    cmp = ((cmp << 8) | avio_r8(cc->pb)) & 0xffffffff;
    if (cmp == signature)
      return avio_tell(cc->pb) - 4;
  }

  return -1;
}

static unsigned aviostream_getcachesize(struct AVIOStream *cc)
{
  return cc->pb->max_packet_size ? cc->pb->max_packet_size : IO_BUFFER_SIZE;
}

static const char *aviostream_geterror(struct AVIOStream *cc)
{
  return "avio error";
}

static void *aviostream_memalloc(struct AVIOStream *cc, size_t size)
{
  return av_malloc(size);
}

static void *aviostream_memrealloc(struct AVIOStream *cc, void *mem, size_t newsize)
{
  return av_realloc(mem, newsize);
}

static void aviostream_memfree(struct AVIOStream *cc, void *mem)
{
  av_free(mem);
}

static int aviostream_progress(struct AVIOStream *cc, ulonglong cur, ulonglong max)
{
  return 1;
}

static longlong aviostream_getfilesize(struct AVIOStream *cc)
{
  return avio_size(cc->pb);
}

static AVIOStream *aviostream_create(AVIOContext *pb)
{
  AVIOStream *iostream = (AVIOStream *)av_mallocz(sizeof(AVIOStream));
  iostream->base.read = (int (*)(InputStream *,ulonglong,void *,int))aviostream_read;
  iostream->base.scan = (longlong (*)(InputStream *,ulonglong,unsigned int))aviostream_scan;
  iostream->base.getcachesize = (unsigned (*)(InputStream *cc))aviostream_getcachesize;
  iostream->base.geterror = (const char *(*)(InputStream *))aviostream_geterror;
  iostream->base.memalloc = (void *(*)(InputStream *,size_t))aviostream_memalloc;
  iostream->base.memrealloc = (void *(*)(InputStream *,void *,size_t))aviostream_memrealloc;
  iostream->base.memfree = (void (*)(InputStream *,void *))aviostream_memfree;
  iostream->base.progress = (int (*)(InputStream *,ulonglong,ulonglong))aviostream_progress;
  iostream->base.getfilesize = (longlong (*)(InputStream *))aviostream_getfilesize;
  iostream->pb = pb;

  return iostream;
}

/* Taken vanilla from ffmpeg */
static int mkv_probe(AVProbeData *p)
{
  uint64_t total = 0;
  int len_mask = 0x80, size = 1, n = 1, i;

  /* EBML header? */
  if (AV_RB32(p->buf) != EBML_ID_HEADER)
    return 0;

  /* length of header */
  total = p->buf[4];
  while (size <= 8 && !(total & len_mask)) {
    size++;
    len_mask >>= 1;
  }
  if (size > 8)
    return 0;
  total &= (len_mask - 1);
  while (n < size)
    total = (total << 8) | p->buf[4 + n++];

  /* Does the probe data contain the whole header? */
  if (p->buf_size < 4 + size + total)
    return 0;

  /* The header should contain a known document type. For now,
  * we don't parse the whole header but simply check for the
  * availability of that array of characters inside the header.
  * Not fully fool-proof, but good enough. */
  for (i = 0; i < FF_ARRAY_ELEMS(matroska_doctypes); i++) {
    size_t probelen = strlen(matroska_doctypes[i]);
    if (total < probelen)
      continue;
    for (n = 4+size; n <= 4+size+total-probelen; n++)
      if (!memcmp(p->buf+n, matroska_doctypes[i], probelen))
        return AVPROBE_SCORE_MAX;
  }

  // probably valid EBML header but no recognized doctype
  return AVPROBE_SCORE_MAX/2;
}

static int matroska_aac_profile(char *codec_id)
{
  static const char * const aac_profiles[] = { "MAIN", "LC", "SSR" };
  int profile;

  for (profile=0; profile<FF_ARRAY_ELEMS(aac_profiles); profile++)
    if (strstr(codec_id, aac_profiles[profile]))
      break;
  return profile + 1;
}

static int matroska_aac_sri(int samplerate)
{
  int sri;

  for (sri=0; sri<FF_ARRAY_ELEMS(avpriv_mpeg4audio_sample_rates); sri++)
    if (avpriv_mpeg4audio_sample_rates[sri] == samplerate)
      break;
  return sri;
}

static int mkv_uid_compare(char first[16], char second[16])
{
  return !memcmp(first, second, 16);
}

static int mkv_uid_zero(char uid[16])
{
  char zero[16] = {0};
  return mkv_uid_compare(uid, zero);
}

static MatroskaSegment* mkv_open_segment(AVFormatContext *s, AVIOContext *pb, ulonglong base)
{
  MatroskaDemuxContext *ctx = (MatroskaDemuxContext *)s->priv_data;
  char ErrorMessage[256];

  MatroskaSegment *segment = av_mallocz(sizeof(*segment));
  segment->index    = ctx->num_segments;
  segment->iostream = aviostream_create(pb);
  segment->matroska = mkv_OpenEx(&segment->iostream->base, base, 0, ErrorMessage, sizeof(ErrorMessage));

  if (!segment->matroska) {
    av_log(s, AV_LOG_ERROR, "mkv_OpenEx returned error: %s\n", ErrorMessage);
    av_freep(&segment->iostream);
    av_freep(&segment);
    return NULL;
  }

  segment->info = mkv_GetFileInfo(segment->matroska);
  memcpy(segment->UID, segment->info->UID, 16);

  av_dynarray_add(&ctx->segments, &ctx->num_segments, segment);
  return segment;
}

static void mkv_reopen_segment(AVFormatContext *s, MatroskaSegment *segment)
{
  char ErrorMessage[256];

  /* reset packet size */
  segment->iostream->pb->max_packet_size = 0;
  ffio_set_buf_size(segment->iostream->pb, IO_BUFFER_SIZE * 4);

  segment->matroska = mkv_OpenEx(&segment->iostream->base, 0, 0, ErrorMessage, sizeof(ErrorMessage));
  if (!segment->matroska) {
    av_log(s, AV_LOG_ERROR, "mkv_OpenEx returned error: %s\n", ErrorMessage);
    segment->failed = 1;
  }

  segment->info = mkv_GetFileInfo(segment->matroska);
}

static MatroskaSegment* mkv_discover_segment(AVFormatContext *s, AVIOContext *pb)
{
  MatroskaDemuxContext *ctx = (MatroskaDemuxContext *)s->priv_data;
  char ErrorMessage[256];
  SegmentInfo *info;

  MatroskaSegment *segment = av_mallocz(sizeof(*segment));
  segment->index    = ctx->num_segments;
  segment->iostream = aviostream_create(pb);
  pb->max_packet_size = IO_BUFFER_SIZE;

  segment->matroska = mkv_OpenSparse(&segment->iostream->base, ErrorMessage, sizeof(ErrorMessage));

  if (!segment->matroska) {
    av_log(s, AV_LOG_ERROR, "mkv_OpenEx returned error: %s\n", ErrorMessage);
    av_freep(&segment->iostream);
    av_freep(&segment);
    return NULL;
  }

  info = mkv_GetFileInfo(segment->matroska);
  memcpy(segment->UID, info->UID, 16);

  mkv_Close(segment->matroska);
  segment->matroska = NULL;

  av_dynarray_add(&ctx->segments, &ctx->num_segments, segment);
  return segment;
}

static int mkv_find_segment_avio(AVFormatContext *s, AVIOContext *pb, ulonglong base)
{
  MatroskaSegment *segment;

  av_log(s, AV_LOG_INFO, "Scanning for Segment at %"PRId64"\n", base);

  if (base == 0)
    segment = mkv_discover_segment(s, pb);
  else
    segment = mkv_open_segment(s, pb, base);

  if (!segment)
    return 0;

  av_log(s, AV_LOG_INFO, "Found Segment with UID: "FF_PRI_UID"\n", FF_ARG_UID(segment->UID));

  if (base == 0) {
    segment->free_avio = 1;
  }

  return 1;
}

static void mkv_find_segments_file(AVFormatContext *s, const char *path, const char *file)
{
  AVIOContext *pb = NULL;
  int found;
  char *filename = av_asprintf("%s/%s", path, file);

  if (avio_open(&pb, filename, AVIO_FLAG_READ|AVIO_FLAG_AVOID_FSTAT) < 0) {
    av_log(s, AV_LOG_ERROR, "Error opening file %s\n", filename);
    goto done;
  }
  av_log(s, AV_LOG_INFO, "Opening %s...\n", filename);
  found = mkv_find_segment_avio(s, pb, 0);
  if (!found) {
    av_log(s, AV_LOG_WARNING, "File %s could not be opened as MKV\n", filename);
    avio_closep(&pb);
  }

done:
  av_freep(&filename);
}

static void mkv_find_segments(AVFormatContext *s)
{
  char *filename, *filespec;
  const char *path, *file;
  int ret = 0;
  intptr_t handle;
  struct _wfinddata_t finddata;
  wchar_t wfilespec[4096];

  filename = av_strdup(s->filename);
  file = av_basename(filename);
  path = av_dirname(filename);
  filespec = av_asprintf("%s/*.mk?", path);

  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, filespec, -1, wfilespec, 4096)) {
    handle = _wfindfirst(wfilespec, &finddata);
    if (handle != -1) {
      while (ret == 0) {
        char mkvFileName[4096];
        WideCharToMultiByte(CP_UTF8, 0, finddata.name, -1, mkvFileName, 4096, NULL, NULL);

        // Skip the main file, it was processed elsewhere
        if (av_strcasecmp(mkvFileName, file) != 0) {
          mkv_find_segments_file(s, path, mkvFileName);
        }
        ret = _wfindnext(handle, &finddata);
      }
      _findclose(handle);
    }
  }

  av_freep(&filename);
  av_freep(&filespec);
}

static MatroskaSegment* mkv_get_segment(AVFormatContext *s, char uid[16])
{
  MatroskaDemuxContext *ctx = (MatroskaDemuxContext *)s->priv_data;
  int i;

  if (mkv_uid_zero(uid) || mkv_uid_compare(ctx->segments[0]->UID, uid))
    return ctx->segments[0];

  if (!ctx->segments_scanned) {
    /* scan for segments within this file */
    ulonglong base = mkv_GetSegmentTop(ctx->segments[0]->matroska);
    ulonglong size = avio_size(ctx->segments[0]->iostream->pb);

    while (base < size) {
      int found = mkv_find_segment_avio(s, ctx->segments[0]->iostream->pb, base);
      if (!found)
        break;

      base = mkv_GetSegmentTop(ctx->segments[ctx->num_segments-1]->matroska);
    }

    /* and for segments in other files, if allowed */
    if (!(s->flags & AVFMT_FLAG_NOEXTERNAL))
      mkv_find_segments(s);
    ctx->segments_scanned = 1;
  }

  for (i = 1; i < ctx->num_segments; i++) {
    if (!ctx->segments[i]->failed && mkv_uid_compare(ctx->segments[i]->UID, uid)) {
      if (!ctx->segments[i]->matroska) {
        mkv_reopen_segment(s, ctx->segments[i]);
        if (ctx->segments[i]->failed)
          break;
      }
      return ctx->segments[i];
    }
  }
  return NULL;
}

static void mkv_build_timeline(AVFormatContext *s, Chapter *edition)
{
  MatroskaDemuxContext *ctx = (MatroskaDemuxContext *)s->priv_data;
  int i;
  ulonglong duration = 0;

  av_freep(&ctx->timeline);
  ctx->timeline = av_mallocz(sizeof(*ctx->timeline) * edition->nChildren);

  ctx->num_timeline = 0;
  for (i = 0; i < edition->nChildren; i++) {
    Chapter *chapter = &edition->Children[i];
    MatroskaSegment *segment = mkv_get_segment(s, chapter->SegmentUID);
    /* check that the chapter timeline is valid */
    if (chapter->End < chapter->Start) {
      edition->Ordered = 0;
      ctx->virtual_timeline = 0;
      av_freep(&ctx->timeline);
      return;
    }
    if (segment && chapter->Enabled && chapter->End > chapter->Start) {
      VirtualTimelineEntry *vt = &ctx->timeline[ctx->num_timeline];
      vt->index     = ctx->num_timeline;
      vt->chapter   = chapter;
      vt->segment   = segment;
      vt->start     = duration;
      vt->stop      = vt->start + (chapter->End - chapter->Start);
      vt->offset    = (longlong)chapter->Start - (longlong)vt->start;
      vt->need_seek = (vt->index == 0 && (chapter->Start != 0 || segment->index != 0)) || (vt->index > 0 && (chapter->Start != ctx->timeline[vt->index-1].chapter->End || segment != ctx->timeline[vt->index-1].segment));

      duration      = vt->stop;
      ctx->num_timeline++;
    }
  }
  if (ctx->num_timeline) {
    s->duration = duration / (1000000000 / AV_TIME_BASE);
  } else {
    ctx->virtual_timeline = 0;
  }
}

static VirtualTimelineEntry* mkv_get_timeline_entry(AVFormatContext *s, ulonglong virtualTime)
{
  MatroskaDemuxContext *ctx = (MatroskaDemuxContext *)s->priv_data;
  int i;

  if (!ctx->virtual_timeline)
    return NULL;

  for (i = 0; i < ctx->num_timeline; i++) {
    VirtualTimelineEntry *vt = &ctx->timeline[i];
    if (virtualTime >= vt->start && virtualTime < vt->stop)
      return vt;
  }
  return &ctx->timeline[ctx->num_timeline-1];
}

static void mkv_process_chapter(AVFormatContext *s, Chapter *chapter, int level, longlong offset)
{
  unsigned i;
  if (chapter->UID && chapter->Enabled && !chapter->Hidden) {
    AVChapter *avchap = avpriv_new_chapter(s, s->nb_chapters, (AVRational){1, 1000000000}, chapter->Start - offset, (chapter->End >= chapter->Start) ? chapter->End - offset : AV_NOPTS_VALUE, chapter->Display ? chapter->Display->String : NULL);

    if (level > 0 && chapter->Display && chapter->Display->String) {
      char *title = (char *)av_mallocz(level + strlen(chapter->Display->String) + 2);
      memset(title, '+', level);
      title[level] = ' ';
      memcpy(&title[level+1], chapter->Display->String, strlen(chapter->Display->String));
      av_dict_set(&avchap->metadata, "title", title, 0);
    }
  }
  if ((chapter->Enabled || level < 0) && chapter->nChildren > 0) {
    for (i = 0; i < chapter->nChildren; i++) {
      mkv_process_chapter(s, chapter->Children + i, level + 1, offset);
    }
  }
}

static void mkv_process_chapters(AVFormatContext *s, Chapter *edition)
{
  MatroskaDemuxContext *ctx = (MatroskaDemuxContext *)s->priv_data;
  int i;

  /* Free the old chapters */
  while(s->nb_chapters--) {
    av_dict_free(&s->chapters[s->nb_chapters]->metadata);
    av_freep(&s->chapters[s->nb_chapters]);
  }
  av_freep(&s->chapters);
  s->nb_chapters = 0;
  /* Add new chapters */
  if (ctx->virtual_timeline) {
    for (i = 0; i < ctx->num_timeline; i++) {
      VirtualTimelineEntry *vt = &ctx->timeline[i];
      mkv_process_chapter(s, vt->chapter, ctx->fake_edition ? -1 : 0, vt->offset);
    }
  } else {
    mkv_process_chapter(s, edition, -1, 0);
  }
}

static ulonglong mkv_get_track_mask(MatroskaDemuxContext *ctx)
{
  int i;
  ulonglong mask = 0;
  for (i = 0; i < ctx->num_tracks; i++) {
    if (!ctx->tracks[i].stream || ctx->tracks[i].stream->discard == AVDISCARD_ALL)
      mask |= (1ull << i);
  }
  return mask;
}

static void mkv_dump_chapters(Chapter *chapters, int count)
{
  int i;
  av_log(NULL, AV_LOG_INFO, "  -> Chapter List:");
  for (i = 0; i < count; i++) {
    Chapter *chapter = &chapters[i];
    av_log(NULL, AV_LOG_INFO, "   -> Chapter %d", i);
    av_log(NULL, AV_LOG_INFO, "    -> UID:        %"PRIu64, chapter->UID);
    av_log(NULL, AV_LOG_INFO, "    -> SegmentUID: "FF_PRI_UID, FF_ARG_UID(chapter->SegmentUID));
    av_log(NULL, AV_LOG_INFO, "    -> Enabled:    %d", chapter->Enabled);
    av_log(NULL, AV_LOG_INFO, "    -> Hidden:     %d", chapter->Hidden);
    av_log(NULL, AV_LOG_INFO, "    -> Start:      %"PRIu64, chapter->Start);
    av_log(NULL, AV_LOG_INFO, "    -> End:        %"PRIu64, chapter->End);
    if (chapter->Display && chapter->Display->String)
      av_log(NULL, AV_LOG_INFO, "    -> Name:       %s", chapter->Display->String);
  }
}

static void mkv_dump_editions(Chapter *editions, int count)
{
  int i;
  av_log(NULL, AV_LOG_INFO, "MKV Editions:");
  for (i = 0; i < count; i++) {
    Chapter *edition = &editions[i];
    av_log(NULL, AV_LOG_INFO, " -> Edition %d", i);
    av_log(NULL, AV_LOG_INFO, "  -> UID:      %"PRIu64, edition->UID);
    av_log(NULL, AV_LOG_INFO, "  -> Default:  %d", edition->Default);
    av_log(NULL, AV_LOG_INFO, "  -> Hidden:   %d", edition->Hidden);
    av_log(NULL, AV_LOG_INFO, "  -> Ordered:  %d", edition->Ordered);
    av_log(NULL, AV_LOG_INFO, "  -> Chapters: %d", edition->nChildren);
    mkv_dump_chapters(edition->Children, edition->nChildren);
  }
}

static void mkv_process_virtual_cues(AVFormatContext *s, VirtualTimelineEntry *vt)
{
  MatroskaDemuxContext *ctx = (MatroskaDemuxContext *)s->priv_data;
  unsigned int count, u;
  int i;
  Cue *cues = NULL;

  mkv_GetCues(vt->segment->matroska, &cues, &count);
  for (u = 0; u < count; u++) {
    Cue *cue = &cues[u];
    if (cue->Time >= vt->chapter->Start && cue->Time < vt->chapter->End) {
      ulonglong time = cue->Time - vt->offset;
      for(i = 0; i < ctx->num_tracks; i++) {
        if (cue->Track == ctx->tracks[i].info->Number && ctx->tracks[i].stream)
          av_add_index_entry(ctx->tracks[i].stream, -1, time, 0, 0, AVINDEX_KEYFRAME);
      }
    }
  }
}

static void mkv_build_index(AVFormatContext *s)
{
  MatroskaDemuxContext *ctx = (MatroskaDemuxContext *)s->priv_data;
  int i;
  unsigned int count, u;
  Cue *cues = NULL;

  /* free old index */
  for (u = 0; u < s->nb_streams; u++) {
    av_freep(&s->streams[u]->index_entries);
    s->streams[u]->index_entries_allocated_size = 0;
    s->streams[u]->nb_index_entries = 0;
  }
  /* convert Cue entries into av index entries */
  if (ctx->virtual_timeline) {
    for (i = 0; i < ctx->num_timeline; i++) {
      VirtualTimelineEntry *vt = &ctx->timeline[i];
      mkv_process_virtual_cues(s, vt);
    }
  } else {
    mkv_GetCues(ctx->matroska, &cues, &count);
    for (u = 0; u < count; u++) {
      for(i = 0; i < ctx->num_tracks; i++) {
        if (cues[u].Track == ctx->tracks[i].info->Number && ctx->tracks[i].stream)
          av_add_index_entry(ctx->tracks[i].stream, -1, cues[u].Time, 0, 0, AVINDEX_KEYFRAME);
      }
    }
  }
}

static void mkv_switch_edition(AVFormatContext *s, int index)
{
  MatroskaDemuxContext *ctx = (MatroskaDemuxContext *)s->priv_data;
  Chapter *edition = ctx->editions[index];

  av_assert0(index < ctx->num_editions);

  if (edition->Ordered) {
    ctx->virtual_timeline = 1;
    mkv_build_timeline(s, edition);
  } else {
    ctx->virtual_timeline = 0;
    av_freep(&ctx->timeline);
  }

  s->duration = ctx->aveditions[index].duration;

  mkv_process_chapters(s, edition);
  mkv_build_index(s);
}

static int64_t mkv_edition_duration(AVFormatContext *s, Chapter *edition)
{
  MatroskaDemuxContext *ctx = (MatroskaDemuxContext *)s->priv_data;
  int64_t duration = 0;
  int i = 0;

  if (edition->Ordered) {
    for (i = 0; i < edition->nChildren; i++) {
      Chapter *chapter = &edition->Children[i];
      MatroskaSegment *segment = mkv_get_segment(s, chapter->SegmentUID);
      /* check that the chapter timeline is valid */
      if (chapter->End < chapter->Start) {
        duration = ctx->segments[0]->info->Duration;
        break;
      }
      if (segment && chapter->Enabled) {
        duration += (chapter->End - chapter->Start);
      }
    }
  } else {
    duration = ctx->segments[0]->info->Duration;
  }
  return duration / (1000000000 / AV_TIME_BASE);
}

static void mkv_process_editions(AVFormatContext *s, Chapter *editions, int count)
{
  MatroskaDemuxContext *ctx = (MatroskaDemuxContext *)s->priv_data;
  int i;
  int edition_index = -1;

  mkv_dump_editions(editions, count);

  for (i = 0; i < count; i++) {
    Chapter *edition = &editions[i];
    if (!edition->Hidden) {
      if (edition->Default && edition_index == -1)
        edition_index = ctx->num_editions;
      av_dynarray_add(&ctx->editions, &ctx->num_editions, edition);
    }
  }

  if (!ctx->num_editions)
    av_dynarray_add(&ctx->editions, &ctx->num_editions, &editions[0]);

  if (edition_index == -1)
    edition_index = 0;

  ctx->aveditions = av_mallocz(sizeof(*ctx->aveditions) * ctx->num_editions);
  for (i = 0; i < ctx->num_editions; i++) {
    Chapter *edition = ctx->editions[i];
    AVEdition *avedition = &ctx->aveditions[i];
    avedition->index = i;
    avedition->duration = mkv_edition_duration(s, edition);
    avedition->ordered = edition->Ordered;
  }
  ctx->active_edition = edition_index;
  mkv_switch_edition(s, edition_index);
}

static Chapter *mkv_chapter_add_child(Chapter *chapter)
{
  chapter->nChildren++;
  chapter->nChildrenSize = chapter->nChildren * sizeof(Chapter);
  chapter->Children = av_realloc(chapter->Children, chapter->nChildrenSize);

  memset(&chapter->Children[chapter->nChildren - 1], 0, sizeof(Chapter));
  return &chapter->Children[chapter->nChildren - 1];
}

static void mkv_process_link(AVFormatContext *s, Chapter *edition, char uid[16], int prev, int next)
{
  Chapter *chapters = NULL, *chapter = NULL;
  unsigned int count = 0;
  MatroskaSegment *segment = mkv_get_segment(s, uid);
  if (!segment) return;

  if (prev && !mkv_uid_zero(segment->info->PrevUID)) {
    mkv_process_link(s, edition, segment->info->PrevUID, 1, 0);
  }

  chapter = mkv_chapter_add_child(edition);
  memcpy(chapter->SegmentUID, uid, 16);
  chapter->Enabled = 1;
  chapter->Hidden = 1;
  chapter->Start = 0;
  chapter->End = segment->info->Duration;

  mkv_GetChapters(segment->matroska, &chapters, &count);
  if (count > 0) {
    unsigned edition_index = 0, u;
    for (u = 0; u < count; u++) {
      if (chapters[u].Default)
        edition_index = u;
    }
    chapter->Children      = chapters[edition_index].Children;
    chapter->nChildren     = chapters[edition_index].nChildren;
    chapter->nChildrenSize = chapters[edition_index].nChildrenSize;
  }

  if (next && !mkv_uid_zero(segment->info->NextUID)) {
    mkv_process_link(s, edition, segment->info->NextUID, 0, 1);
  }
}

static void mkv_process_filelinks(AVFormatContext *s, SegmentInfo *info)
{
  MatroskaDemuxContext *ctx = (MatroskaDemuxContext *)s->priv_data;
  Chapter *linkingEdition = av_mallocz(sizeof(Chapter));
  linkingEdition->Enabled = 1;
  linkingEdition->Ordered = 1;

  mkv_process_link(s, linkingEdition, info->UID, 1, 1);

  if (linkingEdition->nChildren > 1) {
    ctx->fake_edition = linkingEdition;
    ctx->num_editions = 0;
    av_freep(&ctx->editions);

    mkv_process_editions(s, linkingEdition, 1);
    mkv_switch_edition(s, 0);
  } else {
    av_freep(&linkingEdition->Children);
    av_freep(&linkingEdition);
  }
}

static void mkv_process_tags_edition(AVFormatContext *s, ulonglong UID, struct SimpleTag *tags, unsigned int tagCount)
{
  MatroskaDemuxContext *ctx = (MatroskaDemuxContext *)s->priv_data;
  int i, edition_index = -1;
  unsigned j;
  for (i = 0; i < ctx->num_editions; i++) {
    if (ctx->editions[i]->UID == UID) {
      edition_index = i;
      break;
    }
  }
  if (edition_index == -1)
    return;

  for (j = 0; j < tagCount; j++) {
    if (av_strcasecmp(tags[j].Name, "TITLE") == 0) {
      ctx->aveditions[edition_index].title = tags[j].Value;
      break;
    }
  }
}

static void matroska_convert_tag(AVFormatContext *s, Tag *tag,
                                 AVDictionary **metadata, char *prefix)
{
    const struct SimpleTag *tags = tag->SimpleTags;
    char key[1024];
    int i;

    for (i = 0; i < tag->nSimpleTags; i++) {
        const char *lang = (tags[i].Language[0] && strcmp(tags[i].Language, "und")) ? tags[i].Language : NULL;

        if (!tags[i].Name) {
            av_log(s, AV_LOG_WARNING, "Skipping invalid tag with no TagName.\n");
            continue;
        }
        if (prefix)
            snprintf(key, sizeof(key), "%s/%s", prefix, tags[i].Name);
        else
            av_strlcpy(key, tags[i].Name, sizeof(key));
        if (tags[i].Default || !lang) {
            av_dict_set(metadata, key, tags[i].Value, 0);
        }
        if (lang) {
            av_strlcat(key, "-", sizeof(key));
            av_strlcat(key, lang, sizeof(key));
            av_dict_set(metadata, key, tags[i].Value, 0);
        }
    }
    ff_metadata_conv(metadata, NULL, ff_mkv_metadata_conv);
}

static void mkv_process_tags(AVFormatContext *s, Tag *tags, unsigned int tagCount)
{
  MatroskaDemuxContext *ctx = (MatroskaDemuxContext *)s->priv_data;
  unsigned i, j, k;
  for (i = 0; i < tagCount; i++) {
    Tag *tag = &tags[i];
    if (tag->nSimpleTags > 0 && tag->nTargets > 0) {
      for (j = 0; j < tag->nTargets; j++) {
        switch (tag->Targets[j].Type) {
        case TARGET_CHAPTER:
        case TARGET_ATTACHMENT:
        default:
          /* unsupported */
          break;
        case TARGET_TRACK:
          for (k = 0; k < ctx->num_tracks; k++) {
            if (ctx->tracks[k].info->UID == tag->Targets[j].UID && ctx->tracks[k].stream) {
              matroska_convert_tag(s, tag, &ctx->tracks[k].stream->metadata, NULL);
              break;
            }
          }
          break;
        case TARGET_EDITION:
          mkv_process_tags_edition(s, tag->Targets[j].UID, tag->SimpleTags, tag->nSimpleTags);
          break;
        }
      }
    } else if (tags->nSimpleTags > 0) {
      // global tags
      matroska_convert_tag(s, tag, &s->metadata, NULL);
    }
  }
}

static void mkv_process_attachments(AVFormatContext *s, MatroskaSegment *segment)
{
  Attachment *attachments = NULL;
  unsigned u, count;
  int i;

  mkv_GetAttachments(segment->matroska, &attachments, &count);
  for (u = 0; u < count; u++) {
    Attachment *attach = &attachments[u];

    if (!(attach->Name && attach->MimeType && attach->Length > 0)) {
      av_log(s, AV_LOG_VERBOSE, "Incomplete attachment, skipping");
    } else {
      AVStream *st = avformat_new_stream(s, NULL);
      if (st == NULL)
        break;
      av_dict_set(&st->metadata, "filename", attach->Name, 0);
      av_dict_set(&st->metadata, "mimetype", attach->MimeType, 0);
      st->codecpar->codec_id = AV_CODEC_ID_NONE;
      st->codecpar->codec_type = AVMEDIA_TYPE_ATTACHMENT;

      st->codecpar->extradata = (uint8_t *)av_malloc((size_t)attach->Length + FF_INPUT_BUFFER_PADDING_SIZE);
      if(st->codecpar->extradata == NULL)
        break;
      st->codecpar->extradata_size = (int)attach->Length;
      aviostream_read(segment->iostream, attach->Position, st->codecpar->extradata, st->codecpar->extradata_size);

      for (i=0; ff_mkv_mime_tags[i].id != AV_CODEC_ID_NONE; i++) {
        if (!strncmp(ff_mkv_mime_tags[i].str, attach->MimeType, strlen(ff_mkv_mime_tags[i].str))) {
          st->codecpar->codec_id = ff_mkv_mime_tags[i].id;
          break;
        }
      }
    }
  }
}

static int mkv_generate_extradata(AVFormatContext *s, TrackInfo *info, enum AVCodecID codec_id, uint8_t **extradata_ptr, int *extradata_len)
{
  int extradata_offset = 0, extradata_size = 0;
  uint8_t *extradata = NULL;
  AVIOContext b;

  if (!strcmp(info->CodecID, "V_MS/VFW/FOURCC") && info->CodecPrivateSize >= 40 && info->CodecPrivate != NULL) {
    extradata_offset = 40;
  } else if (!strcmp(info->CodecID, "A_MS/ACM") && info->CodecPrivateSize >= 14 && info->CodecPrivate != NULL) {
    extradata_offset = FFMIN(info->CodecPrivateSize, 18);
  } else if (codec_id == AV_CODEC_ID_ALAC && info->CodecPrivateSize) {
    /* Only ALAC's magic cookie is stored in Matroska's track headers.
    Create the "atom size", "tag", and "tag version" fields the
    decoder expects manually. */
    extradata_size = 12 + info->CodecPrivateSize;
    extradata = av_mallocz(extradata_size + FF_INPUT_BUFFER_PADDING_SIZE);
    if (extradata == NULL)
      return AVERROR(ENOMEM);
    AV_WB32(extradata, extradata_size);
    memcpy(&extradata[4], "alac", 4);
    AV_WB32(&extradata[8], 0);
    memcpy(&extradata[12], info->CodecPrivate, info->CodecPrivateSize);
  } else if (codec_id == AV_CODEC_ID_AAC && !info->CodecPrivateSize) {
    int profile = matroska_aac_profile(info->CodecID);
    int sri = matroska_aac_sri(mkv_TruncFloat(info->AV.Audio.SamplingFreq));
    extradata = (uint8_t *)av_malloc(5 + FF_INPUT_BUFFER_PADDING_SIZE);
    if (extradata == NULL)
      return AVERROR(ENOMEM);
    extradata[0] = (profile << 3) | ((sri&0x0E) >> 1);
    extradata[1] = ((sri&0x01) << 7) | (info->AV.Audio.Channels<<3);
    if (strstr(info->CodecID, "SBR")) {
      sri = matroska_aac_sri(mkv_TruncFloat(info->AV.Audio.OutputSamplingFreq));
      extradata[2] = 0x56;
      extradata[3] = 0xE5;
      extradata[4] = 0x80 | (sri<<3);
      extradata_size = 5;
    } else
      extradata_size = 2;
  } else if (codec_id == AV_CODEC_ID_TTA) {
    extradata_size = 30;
    extradata = (uint8_t *)av_mallocz(extradata_size + FF_INPUT_BUFFER_PADDING_SIZE);
    if (extradata == NULL)
      return AVERROR(ENOMEM);
    ffio_init_context(&b, extradata, extradata_size, 1, NULL, NULL, NULL, NULL);
    avio_write(&b, "TTA1", 4);
    avio_wl16(&b, 1);
    avio_wl16(&b, info->AV.Audio.Channels);
    avio_wl16(&b, info->AV.Audio.BitDepth);
    avio_wl32(&b, mkv_TruncFloat(info->AV.Audio.OutputSamplingFreq));
    avio_wl32(&b, av_rescale(s->duration, info->AV.Audio.OutputSamplingFreq, AV_TIME_BASE));
  } else if (codec_id == AV_CODEC_ID_WAVPACK) {
    return 0;
  }

  if (*extradata_ptr)
    av_freep(extradata_ptr);
  *extradata_len = 0;

  if (extradata) {
    *extradata_ptr = extradata;
    *extradata_len = extradata_size;
  } else if(info->CodecPrivate && info->CodecPrivateSize > 0){
    extradata_size = info->CodecPrivateSize - extradata_offset;
    *extradata_ptr = (uint8_t *)av_mallocz(extradata_size + FF_INPUT_BUFFER_PADDING_SIZE);
    if(*extradata_ptr == NULL)
      return AVERROR(ENOMEM);

    *extradata_len = extradata_size;
    memcpy(*extradata_ptr, (uint8_t *)info->CodecPrivate + extradata_offset, extradata_size);
  }

  return 0;
}

static int get_qt_codec(TrackInfo *track, uint32_t *fourcc, enum AVCodecID *codec_id)
{
    const AVCodecTag *codec_tags;

    codec_tags = track->Type == TT_VIDEO ?
            ff_codec_movvideo_tags : ff_codec_movaudio_tags;

    /* Normalize noncompliant private data that starts with the fourcc
     * by expanding/shifting the data by 4 bytes and storing the data
     * size at the start. */
    if (ff_codec_get_id(codec_tags, AV_RL32((uint8_t*)track->CodecPrivate))) {
        uint8_t *p = av_realloc(track->CodecPrivate,
                                track->CodecPrivateSize + 4);
        if (!p)
            return AVERROR(ENOMEM);
        memmove(p + 4, p, track->CodecPrivateSize);
        track->CodecPrivate = p;
        track->CodecPrivateSize += 4;
        AV_WB32((uint8_t*)track->CodecPrivate, track->CodecPrivateSize);
    }

    *fourcc = AV_RL32((uint8_t*)track->CodecPrivate + 4);
    *codec_id = ff_codec_get_id(codec_tags, *fourcc);

    return 0;
}

static int mkv_parse_video_color(AVStream *st, TrackInfo *info)
{
    // Mastering primaries are CIE 1931 coords, and must be > 0.
    const int has_mastering_primaries =
        info->AV.Video.Colour.MasteringMetadata.PrimaryRChromaticityX > 0 && info->AV.Video.Colour.MasteringMetadata.PrimaryRChromaticityY > 0 &&
        info->AV.Video.Colour.MasteringMetadata.PrimaryGChromaticityX > 0 && info->AV.Video.Colour.MasteringMetadata.PrimaryGChromaticityY > 0 &&
        info->AV.Video.Colour.MasteringMetadata.PrimaryBChromaticityX > 0 && info->AV.Video.Colour.MasteringMetadata.PrimaryBChromaticityY > 0 &&
        info->AV.Video.Colour.MasteringMetadata.WhitePointChromaticityX > 0 && info->AV.Video.Colour.MasteringMetadata.WhitePointChromaticityY > 0;
    const int has_mastering_luminance = info->AV.Video.Colour.MasteringMetadata.LuminanceMax > 0;

    if (info->AV.Video.Colour.MatrixCoefficients != AVCOL_SPC_RESERVED)
        st->codecpar->color_space = info->AV.Video.Colour.MatrixCoefficients;
    if (info->AV.Video.Colour.Primaries != AVCOL_PRI_RESERVED &&
        info->AV.Video.Colour.Primaries != AVCOL_PRI_RESERVED0)
        st->codecpar->color_primaries = info->AV.Video.Colour.Primaries;
    if (info->AV.Video.Colour.TransferCharacteristics != AVCOL_TRC_RESERVED &&
        info->AV.Video.Colour.TransferCharacteristics != AVCOL_TRC_RESERVED0)
        st->codecpar->color_trc = info->AV.Video.Colour.TransferCharacteristics;
    if (info->AV.Video.Colour.Range != AVCOL_RANGE_UNSPECIFIED &&
        info->AV.Video.Colour.Range <= AVCOL_RANGE_JPEG)
        st->codecpar->color_range = info->AV.Video.Colour.Range;
    if (info->AV.Video.Colour.ChromaSitingHorz != MATROSKA_COLOUR_CHROMASITINGHORZ_UNDETERMINED &&
        info->AV.Video.Colour.ChromaSitingVert != MATROSKA_COLOUR_CHROMASITINGVERT_UNDETERMINED &&
        info->AV.Video.Colour.ChromaSitingHorz   < MATROSKA_COLOUR_CHROMASITINGHORZ_NB &&
        info->AV.Video.Colour.ChromaSitingVert  < MATROSKA_COLOUR_CHROMASITINGVERT_NB) {
        st->codecpar->chroma_location =
            avcodec_chroma_pos_to_enum((info->AV.Video.Colour.ChromaSitingHorz - 1) << 7,
                                       (info->AV.Video.Colour.ChromaSitingVert - 1) << 7);
    }

    if (has_mastering_primaries || has_mastering_luminance) {
        // Use similar rationals as other standards.
        const int chroma_den = 50000;
        const int luma_den = 10000;
        AVMasteringDisplayMetadata *metadata =
            (AVMasteringDisplayMetadata*) av_stream_new_side_data(
                st, AV_PKT_DATA_MASTERING_DISPLAY_METADATA,
                sizeof(AVMasteringDisplayMetadata));
        if (!metadata) {
            return AVERROR(ENOMEM);
        }
        memset(metadata, 0, sizeof(AVMasteringDisplayMetadata));
        if (has_mastering_primaries) {
            metadata->display_primaries[0][0] = av_make_q(
                round(info->AV.Video.Colour.MasteringMetadata.PrimaryRChromaticityX * chroma_den), chroma_den);
            metadata->display_primaries[0][1] = av_make_q(
                round(info->AV.Video.Colour.MasteringMetadata.PrimaryRChromaticityY * chroma_den), chroma_den);
            metadata->display_primaries[1][0] = av_make_q(
                round(info->AV.Video.Colour.MasteringMetadata.PrimaryGChromaticityX * chroma_den), chroma_den);
            metadata->display_primaries[1][1] = av_make_q(
                round(info->AV.Video.Colour.MasteringMetadata.PrimaryGChromaticityY * chroma_den), chroma_den);
            metadata->display_primaries[2][0] = av_make_q(
                round(info->AV.Video.Colour.MasteringMetadata.PrimaryBChromaticityX * chroma_den), chroma_den);
            metadata->display_primaries[2][1] = av_make_q(
                round(info->AV.Video.Colour.MasteringMetadata.PrimaryBChromaticityY * chroma_den), chroma_den);
            metadata->white_point[0] = av_make_q(
                round(info->AV.Video.Colour.MasteringMetadata.WhitePointChromaticityX * chroma_den), chroma_den);
            metadata->white_point[1] = av_make_q(
                round(info->AV.Video.Colour.MasteringMetadata.WhitePointChromaticityY * chroma_den), chroma_den);
            metadata->has_primaries = 1;
        }
        if (has_mastering_luminance) {
            metadata->max_luminance = av_make_q(
                round(info->AV.Video.Colour.MasteringMetadata.LuminanceMax * luma_den), luma_den);
            metadata->min_luminance = av_make_q(
                round(info->AV.Video.Colour.MasteringMetadata.LuminanceMin * luma_den), luma_den);
            metadata->has_luminance = 1;
        }
    }

    return 0;
}

static int mkv_read_header(AVFormatContext *s)
{
  MatroskaDemuxContext *ctx = (MatroskaDemuxContext *)s->priv_data;
  int i, j, num_tracks, ret;
  char ErrorMessage[256];
  MatroskaSegment *segment;
  Chapter *chapters = NULL;
  Tag *tags = NULL;
  unsigned int tagCount = 0;
  unsigned int count;

  segment = mkv_open_segment(s, s->pb, 0);
  if (!segment)
    return -1;

  ctx->matroska = segment->matroska;
  ctx->next_edition = -1;

  if (segment->info->Duration)
    s->duration = segment->info->Duration / (1000000000 / AV_TIME_BASE);
  av_dict_set(&s->metadata, "title", segment->info->Title, 0);

  /* Process Editions/Chapters */
  mkv_GetChapters(ctx->matroska, &chapters, &count);
  if (count > 0) {
    mkv_process_editions(s, chapters, count);
  }

  /* check for file linking */
  if (!ctx->virtual_timeline) {
    mkv_process_filelinks(s, segment->info);
  }

  /* Read Tags before ctx->matroska gets swapped out, but process them at the end */
  mkv_GetTags(ctx->matroska, &tags, &tagCount);

  if (ctx->virtual_timeline && ctx->timeline[0].need_seek) {
    ctx->matroska = ctx->timeline[ctx->timeline_position].segment->matroska;
    mkv_Seek_CueAware(ctx->matroska, ctx->timeline[0].chapter->Start, MKVF_SEEK_TO_PREV_KEYFRAME, 1);
  }

  /* Tracks */
  ctx->num_tracks = num_tracks = mkv_GetNumTracks(ctx->matroska);
  ctx->tracks = (MatroskaTrack *)av_mallocz(sizeof(MatroskaTrack) * num_tracks);
  for(i = 0; i < num_tracks; i++) {
    MatroskaTrack *track = &ctx->tracks[i];
    TrackInfo *info = mkv_GetTrackInfo(ctx->matroska, i);
    enum AVCodecID codec_id = AV_CODEC_ID_NONE;
    AVStream *st;
    uint32_t fourcc = 0;
    AVIOContext b;
    int bit_depth = -1;

    track->info = info;

    if (!info->Enabled)
      continue;

    if (info->Type != TT_VIDEO && info->Type != TT_AUDIO && info->Type != TT_SUB) {
      av_log(s, AV_LOG_ERROR, "Unknown or unsupported track type: %d", info->Type);
      continue;
    }

    if (info->CodecID == NULL)
      continue;

    av_log(s, AV_LOG_DEBUG, "Track %d, type: %d, codec_id: %s", i, info->Type, info->CodecID);

    if (info->CompEnabled && info->CompMethod == COMP_ZLIB) {
      av_log(s, AV_LOG_DEBUG, "Track is ZLIB compressed");
      track->cs = cs_Create(ctx->matroska, i, ErrorMessage, sizeof(ErrorMessage));
      if (!track->cs) {
        av_log(s, AV_LOG_ERROR, "Creating compressed stream failed: %s", ErrorMessage);
        continue;
      }
    }

    for(j=0; ff_mkv_codec_tags[j].id != AV_CODEC_ID_NONE; j++){
      if(!strncmp(ff_mkv_codec_tags[j].str, info->CodecID, strlen(ff_mkv_codec_tags[j].str))){
        codec_id = ff_mkv_codec_tags[j].id;
        break;
      }
    }

    st = track->stream = avformat_new_stream(s, NULL);
    if (st == NULL)
      return AVERROR(ENOMEM);

    st->id = info->Number;
    st->start_time = 0;

    avpriv_set_pts_info(st, 64, 1, 1000*1000*1000); /* 64 bit pts in ns */

    ret = mkv_generate_extradata(s, info, codec_id, &st->codecpar->extradata, &st->codecpar->extradata_size);
    if (ret < 0)
      return ret;

    if (!strcmp(info->CodecID, "V_MS/VFW/FOURCC") && info->CodecPrivateSize >= 40 && info->CodecPrivate != NULL) {
      track->ms_compat = 1;
      bit_depth = AV_RL16((uint8_t *)info->CodecPrivate + 14);
      fourcc    = AV_RL32((uint8_t *)info->CodecPrivate + 16);
      codec_id = ff_codec_get_id(ff_codec_bmp_tags, fourcc);
    } else if (!strcmp(info->CodecID, "A_MS/ACM") && info->CodecPrivateSize >= 14 && info->CodecPrivate != NULL) {
      ffio_init_context(&b, (uint8_t *)info->CodecPrivate, info->CodecPrivateSize, 0, NULL, NULL, NULL, NULL);
      ret = ff_get_wav_header(s, &b, st->codecpar, info->CodecPrivateSize, 0);
      if (ret < 0)
        return ret;
      codec_id = st->codecpar->codec_id;
    } else if (!strcmp(info->CodecID, "A_QUICKTIME") && (info->CodecPrivateSize >= 32) && (info->CodecPrivate != NULL)) {
      uint16_t sample_size;
      ret = get_qt_codec(info, &fourcc, &codec_id);
      if (ret < 0)
          return ret;
      sample_size = AV_RB16((uint8_t *)info->CodecPrivate + 26);
      if (fourcc == 0) {
        if (sample_size == 8) {
          fourcc = MKTAG('r','a','w',' ');
          codec_id = ff_codec_get_id(ff_codec_movaudio_tags, fourcc);
        } else if (sample_size == 16) {
          fourcc = MKTAG('t','w','o','s');
          codec_id = ff_codec_get_id(ff_codec_movaudio_tags, fourcc);
        }
      }
      if ((fourcc == MKTAG('t','w','o','s') ||
              fourcc == MKTAG('s','o','w','t')) &&
              sample_size == 8)
        codec_id = AV_CODEC_ID_PCM_S8;
    } else if (!strcmp(info->CodecID, "V_QUICKTIME") && (info->CodecPrivateSize >= 21) && (info->CodecPrivate != NULL)) {
      ret = get_qt_codec(info, &fourcc, &codec_id);
      if (ret < 0)
        return ret;
      if (codec_id == AV_CODEC_ID_NONE && AV_RL32((uint8_t *)info->CodecPrivate+4) == AV_RL32("SMI ")) {
        fourcc = MKTAG('S','V','Q','3');
        codec_id = ff_codec_get_id(ff_codec_movvideo_tags, fourcc);
      }
      if (codec_id == AV_CODEC_ID_NONE) {
        char buf[32];
        av_get_codec_tag_string(buf, sizeof(buf), fourcc);
        av_log(s, AV_LOG_ERROR, "mov FourCC not found %s.\n", buf);
      }
      if (info->CodecPrivateSize >= 86) {
        bit_depth = AV_RB16((uint8_t *)info->CodecPrivate + 82);
        /*ffio_init_context(&b, (uint8_t *)info->CodecPrivate,
                          info->CodecPrivateSize,
                          0, NULL, NULL, NULL, NULL);
        if (ff_get_qtpalette(codec_id, &b, track->palette)) {
            bit_depth &= 0x1F;
            track->has_palette = 1;
        }*/
      }
    } else if (!strcmp(info->CodecID, "V_PRORES") && (info->CodecPrivateSize == 4) && (info->CodecPrivate != NULL)) {
      fourcc = AV_RL32((uint8_t *)info->CodecPrivate);
    } else if (codec_id == AV_CODEC_ID_PCM_S16BE) {
      switch (info->AV.Audio.BitDepth) {
      case  8:  codec_id = AV_CODEC_ID_PCM_U8;     break;
      case 24:  codec_id = AV_CODEC_ID_PCM_S24BE;  break;
      case 32:  codec_id = AV_CODEC_ID_PCM_S32BE;  break;
      }
    } else if (codec_id == AV_CODEC_ID_PCM_S16LE) {
      switch (info->AV.Audio.BitDepth) {
      case  8:  codec_id = AV_CODEC_ID_PCM_U8;     break;
      case 24:  codec_id = AV_CODEC_ID_PCM_S24LE;  break;
      case 32:  codec_id = AV_CODEC_ID_PCM_S32LE;  break;
      }
    } else if (codec_id == AV_CODEC_ID_PCM_F32LE && info->AV.Audio.BitDepth == 64) {
      codec_id = AV_CODEC_ID_PCM_F64LE;
    }

    if (codec_id == AV_CODEC_ID_NONE)
      av_log(s, AV_LOG_VERBOSE, "Unknown/unsupported CodecID: %s", info->CodecID);
    /* refresh codec id, if changed above */
    st->codecpar->codec_id = codec_id;

    if (strlen(info->Language) == 0) /* default english language if none is set */
      av_dict_set(&st->metadata, "language", "eng", 0);
    else if (strcmp(info->Language, "und"))
      av_dict_set(&st->metadata, "language", info->Language, 0);
    av_dict_set(&st->metadata, "title", info->Name, 0);

    av_dict_set(&st->metadata, "mkv-codec-id", info->CodecID, 0);

    if (info->Default)
      st->disposition |= AV_DISPOSITION_DEFAULT;
    if (info->Forced)
      st->disposition |= AV_DISPOSITION_FORCED;

    if (info->Type == TT_VIDEO) {
      st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
      st->codecpar->codec_tag  = fourcc;
      if (bit_depth >= 0)
        st->codecpar->bits_per_coded_sample = bit_depth;
      st->codecpar->width  = info->AV.Video.PixelWidth;
      st->codecpar->height = info->AV.Video.PixelHeight;
      if (info->AV.Video.DisplayWidth && info->AV.Video.DisplayHeight) {
        av_reduce(&st->sample_aspect_ratio.num, &st->sample_aspect_ratio.den,
          st->codecpar->height * info->AV.Video.DisplayWidth,
          st->codecpar-> width * info->AV.Video.DisplayHeight,
          1 << 30);
      }
      if (st->codecpar->codec_id != AV_CODEC_ID_H264 && st->codecpar->codec_id != AV_CODEC_ID_HEVC)
        st->need_parsing = AVSTREAM_PARSE_HEADERS;
      av_log(s, AV_LOG_DEBUG, "Default Duration: %"PRId64"\n", info->DefaultDuration);
      if (info->DefaultDuration && info->DefaultDuration > 8000000) {
        av_reduce(&st->r_frame_rate.num, &st->r_frame_rate.den,
                  1000000000, info->DefaultDuration, 100000);
        st->avg_frame_rate = st->r_frame_rate;
      }

      /* export stereo mode flag as metadata tag */
      if (info->AV.Video.StereoMode && info->AV.Video.StereoMode < MATROSKA_VIDEO_STEREOMODE_TYPE_NB)
        av_dict_set(&st->metadata, "stereo_mode", ff_matroska_video_stereo_mode[info->AV.Video.StereoMode], 0);

      // add stream level stereo3d side data if it is a supported format
      if (info->AV.Video.StereoMode < MATROSKA_VIDEO_STEREOMODE_TYPE_NB &&
        info->AV.Video.StereoMode != 10 && info->AV.Video.StereoMode != 12) {
        ret = ff_mkv_stereo3d_conv(st, info->AV.Video.StereoMode);
        if (ret < 0)
            return ret;
      }

      ret = mkv_parse_video_color(st, info);
      if (ret < 0)
          return ret;
      // if we have virtual track, mark the real tracks
      /*for (j=0; j < track->operation.combine_planes.nb_elem; j++) {
        char buf[32];
        if (planes[j].type >= MATROSKA_VIDEO_STEREO_PLANE_COUNT)
          continue;
        snprintf(buf, sizeof(buf), "%s_%d", matroska_video_stereo_plane[planes[j].type], i);
        for (k=0; k < matroska->tracks.nb_elem; k++)
          if (planes[j].uid == tracks[k].uid) {
            av_dict_set(&s->streams[k]->metadata, "stereo_mode", buf, 0);
            break;
          }
      } */
    } else if (info->Type == TT_AUDIO) {
      st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
      st->codecpar->sample_rate = (unsigned int)info->AV.Audio.OutputSamplingFreq;
      st->codecpar->channels = info->AV.Audio.Channels;
      if (st->codecpar->codec_id == AV_CODEC_ID_MP3)
        st->need_parsing = AVSTREAM_PARSE_FULL;
      else if (st->codecpar->codec_id != AV_CODEC_ID_AAC && st->codecpar->codec_id != AV_CODEC_ID_MLP && st->codecpar->codec_id != AV_CODEC_ID_TRUEHD)
        st->need_parsing = AVSTREAM_PARSE_HEADERS;
      if (track->info->CodecDelay > 0) {
        st->codecpar->initial_padding = av_rescale_q(track->info->CodecDelay, (AVRational){1, 1000000000}, (AVRational){1, st->codecpar->sample_rate});
      }
      if (track->info->SeekPreRoll > 0) {
        st->codecpar->seek_preroll = av_rescale_q(track->info->SeekPreRoll, (AVRational){1, 1000000000}, (AVRational){1, st->codecpar->sample_rate});
      }
    } else if (info->Type == TT_SUB) {
      st->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
      if (st->codecpar->codec_id == AV_CODEC_ID_ASS) {
        /* HACK: Try to get the privdata of the main segments SSA track, otherwise DirectShow renderers fail */
        unsigned num = mkv_GetNumTracks(ctx->segments[0]->matroska);
        if (num > i) {
          uint8_t *main_extradata = NULL;
          int main_extradata_size = 0;
          info = mkv_GetTrackInfo(ctx->segments[0]->matroska, i);
          ret = mkv_generate_extradata(s, info, codec_id, &main_extradata, &main_extradata_size);
          if (ret == 0 && main_extradata_size && main_extradata) {
            av_freep(&st->codecpar->extradata);
            st->codecpar->extradata = main_extradata;
            st->codecpar->extradata_size = main_extradata_size;
          }
        }
      }
    }
  }

  for (i = 0; i < ctx->num_segments; i++) {
    if (ctx->segments[i]->matroska)
      mkv_process_attachments(s, ctx->segments[i]);
  }

  if (tagCount > 0 && tags) {
    mkv_process_tags(s, tags, tagCount);
  }

  /* Can only build the index after tracks are loaded */
  mkv_build_index(s);

  /* close segments which were not needed for the virtual timeline */
  for (i = 0; i < ctx->num_segments; i++) {
    if (!ctx->segments[i]->matroska) {
      if (ctx->segments[i]->free_avio)
        avio_closep(&ctx->segments[i]->iostream->pb);
      ctx->segments[i]->free_avio = 0;
      av_freep(&ctx->segments[i]->iostream);
    }
  }

  return 0;
}

#define TI_DIFF(field) (new->AV.field != old->AV.field)
static int mkv_trackinfo_diff(TrackInfo *new, TrackInfo *old)
{
  if (new->Type == TT_VIDEO)
    return TI_DIFF(Video.PixelWidth) || TI_DIFF(Video.PixelHeight) || TI_DIFF(Video.DisplayWidth) || TI_DIFF(Video.DisplayHeight);
  if (new->Type == TT_AUDIO)
    return TI_DIFF(Audio.Channels) || TI_DIFF(Audio.OutputSamplingFreq);

  return 0;
}

static void mkv_switch_segment(AVFormatContext *s, MatroskaFile *segment, int force)
{
  MatroskaDemuxContext *ctx = (MatroskaDemuxContext *)s->priv_data;
  unsigned int num_tracks, u;
  char ErrorMessage[256];

  if (ctx->num_segments <= 1)
    return;

  ctx->matroska = segment;

  num_tracks = mkv_GetNumTracks(ctx->matroska);
  if (num_tracks != ctx->num_tracks) {
    av_log(s, AV_LOG_WARNING, "Number of tracks in segments is different, old: %u, new: %u", ctx->num_tracks, num_tracks);
  }

  for (u = 0; u < min(num_tracks, ctx->num_tracks); u++) {
    TrackInfo *info = mkv_GetTrackInfo(ctx->matroska, u);
    if (force || info->Type == TT_AUDIO || (info->CodecPrivateSize && (info->CodecPrivateSize != ctx->tracks[u].info->CodecPrivateSize || memcmp(info->CodecPrivate, ctx->tracks[u].info->CodecPrivate, info->CodecPrivateSize)))
      || mkv_trackinfo_diff(info, ctx->tracks[u].info)) {
      ctx->tracks[u].refresh_extradata = 1;
    }
    ctx->tracks[u].info = info;

    // Update compression
    if (ctx->tracks[u].cs) {
      cs_Destroy(ctx->tracks[u].cs);
      ctx->tracks[u].cs = NULL;
    }
    if (info->CompEnabled && info->CompMethod == COMP_ZLIB) {
      ctx->tracks[u].cs = cs_Create(ctx->matroska, u, ErrorMessage, sizeof(ErrorMessage));
      if (!ctx->tracks[u].cs) {
        av_log(s, AV_LOG_ERROR, "Creating compressed stream failed: %s", ErrorMessage);
      }
    }
  }
}

#define FRAME_EOF 0x00400000

static int mkv_packet_timeline_update(AVFormatContext *s, longlong *start_time, longlong *end_time, unsigned flags)
{
  MatroskaDemuxContext *ctx = (MatroskaDemuxContext *)s->priv_data;
  int next_timeline = 0;
  if (!ctx->virtual_timeline)
    return 0;

  if (flags & FRAME_EOF) {
    av_log(s, AV_LOG_INFO, "Clip EOF at timeline %d\n", ctx->timeline_position);
    next_timeline = 1;
  } else if (!(flags & FRAME_UNKNOWN_START) && *start_time > 0 && *start_time >= ctx->timeline[ctx->timeline_position].chapter->End) {
    av_log(s, AV_LOG_INFO, "Clip reached chapter boundary at %"PRId64" at timeline %d\n", *start_time, ctx->timeline_position);
    next_timeline = 1;
  }
  if (next_timeline) {
    if (ctx->timeline_position < (ctx->num_timeline-1))
      ctx->timeline_position++;
    else {
      av_log(s, AV_LOG_INFO, "Last Timeline reached, signaling EOF\n");
      return AVERROR_EOF;
    }
    if (ctx->timeline[ctx->timeline_position].need_seek) {
      av_log(s, AV_LOG_INFO, "Seeking to timeline %d (position %"PRId64")\n", ctx->timeline_position, ctx->timeline[ctx->timeline_position].chapter->Start);
      mkv_switch_segment(s, ctx->timeline[ctx->timeline_position].segment->matroska, 0);
      mkv_Seek_CueAware(ctx->matroska, ctx->timeline[ctx->timeline_position].chapter->Start, MKVF_SEEK_TO_PREV_KEYFRAME, 1);
      // Need to discard the current frame, and re-read after the seek
      return AVERROR(EAGAIN);
    }
  }

  if (!(flags & (FRAME_UNKNOWN_START|FRAME_EOF))) {
    *start_time -= ctx->timeline[ctx->timeline_position].offset;

    if (*end_time > ctx->timeline[ctx->timeline_position].chapter->End)
      *end_time = ctx->timeline[ctx->timeline_position].chapter->End;
    *end_time -= ctx->timeline[ctx->timeline_position].offset;
  }
  return (flags & FRAME_EOF) ? AVERROR_EOF : 0;
}

static void mkv_packet_param_change(AVFormatContext *s, TrackInfo *info, enum AVCodecID codec_id, AVPacket *pkt)
{
  uint8_t *extradata = NULL;
  int extralen = 0;
  mkv_generate_extradata(s, info, codec_id, &extradata, &extralen);
  if (extralen > 0) {
    uint8_t *sidedata = av_packet_new_side_data(pkt, AV_PKT_DATA_NEW_EXTRADATA, extralen);
    memcpy(sidedata, extradata, extralen);
  }
  av_freep(&extradata);

  if (info->Type == TT_VIDEO) {
    int size = 4 + 8;
    int aspect_den = 0, aspect_num = 0;
    int flags = AV_SIDE_DATA_PARAM_CHANGE_DIMENSIONS;
    uint8_t *data;

    if (info->AV.Video.DisplayWidth && info->AV.Video.DisplayHeight) {
      size += 8;
      flags |= AV_SIDE_DATA_PARAM_CHANGE_ASPECTRATIO;
      av_reduce(&aspect_num, &aspect_den,
        info->AV.Video.PixelHeight * info->AV.Video.DisplayWidth,
        info->AV.Video.PixelWidth * info->AV.Video.DisplayHeight,
        1 << 30);
    }
    data = av_packet_new_side_data(pkt, AV_PKT_DATA_PARAM_CHANGE, size);
    bytestream_put_le32(&data, flags);
    bytestream_put_le32(&data, info->AV.Video.PixelWidth);
    bytestream_put_le32(&data, info->AV.Video.PixelHeight);
    if (aspect_den && aspect_num) {
      bytestream_put_le32(&data, aspect_num);
      bytestream_put_le32(&data, aspect_den);
    }
  } else if (info->Type == TT_AUDIO) {
    int flags = AV_SIDE_DATA_PARAM_CHANGE_CHANNEL_COUNT | AV_SIDE_DATA_PARAM_CHANGE_SAMPLE_RATE;
    uint8_t *data = av_packet_new_side_data(pkt, AV_PKT_DATA_PARAM_CHANGE, 12);
    bytestream_put_le32(&data, flags);
    bytestream_put_le32(&data, info->AV.Audio.Channels);
    bytestream_put_le32(&data, info->AV.Audio.OutputSamplingFreq);
  }
}

/* reconstruct full wavpack blocks from mangled matroska ones */
static int matroska_parse_wavpack(MatroskaTrack *track, uint8_t *src,
                                  uint8_t **pdst, int *size)
{
    uint8_t *dst = NULL;
    int dstlen   = 0;
    int srclen   = *size;
    uint32_t samples;
    uint16_t ver = 0;
    int ret, offset = 0;

    if (srclen < 12)
        return AVERROR_INVALIDDATA;

    if (track->info->CodecPrivateSize >= 2)
        ver = AV_RL16(track->info->CodecPrivate);

    samples = AV_RL32(src);
    src    += 4;
    srclen -= 4;

    while (srclen >= 8) {
        int multiblock;
        uint32_t blocksize;
        uint8_t *tmp;

        uint32_t flags = AV_RL32(src);
        uint32_t crc   = AV_RL32(src + 4);
        src    += 8;
        srclen -= 8;

        multiblock = (flags & 0x1800) != 0x1800;
        if (multiblock) {
            if (srclen < 4) {
                ret = AVERROR_INVALIDDATA;
                goto fail;
            }
            blocksize = AV_RL32(src);
            src    += 4;
            srclen -= 4;
        } else
            blocksize = srclen;

        if (blocksize > srclen) {
            ret = AVERROR_INVALIDDATA;
            goto fail;
        }

        tmp = av_realloc(dst, dstlen + blocksize + 32);
        if (!tmp) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        dst     = tmp;
        dstlen += blocksize + 32;

        AV_WL32(dst + offset,      MKTAG('w', 'v', 'p', 'k')); // tag
        AV_WL32(dst + offset + 4,  blocksize + 24);            // blocksize - 8
        AV_WL16(dst + offset + 8,  ver);                       // version
        AV_WL16(dst + offset + 10, 0);                         // track/index_no
        AV_WL32(dst + offset + 12, 0);                         // total samples
        AV_WL32(dst + offset + 16, 0);                         // block index
        AV_WL32(dst + offset + 20, samples);                   // number of samples
        AV_WL32(dst + offset + 24, flags);                     // flags
        AV_WL32(dst + offset + 28, crc);                       // crc
        memcpy (dst + offset + 32, src, blocksize);            // block data

        src    += blocksize;
        srclen -= blocksize;
        offset += blocksize + 32;
    }

    *pdst = dst;
    *size = dstlen;

    return 0;

fail:
    av_freep(&dst);
    return ret;
}

static int mkv_read_packet(AVFormatContext *s, AVPacket *pkt)
{
  MatroskaDemuxContext *ctx = (MatroskaDemuxContext *)s->priv_data;

  int ret;
  unsigned int size, flags, track_num;
  ulonglong start_time, end_time, pos;
  longlong discard_padding;
  MatroskaTrack *track;
  char *frame_data = NULL;

  ulonglong mask = 0;
  if (!(s->flags & AVFMT_FLAG_NETWORK)) {
    mask = mkv_get_track_mask(ctx);
    if (mask != ctx->track_mask) {
      mkv_SetTrackMask(ctx->matroska, mask);
      ctx->track_mask = mask;
    }
  }

again:
  ret = mkv_ReadFrame(ctx->matroska, mask, &track_num, &start_time, &end_time, &pos, &size, &frame_data, &flags, &discard_padding);
  if (ctx->virtual_timeline) {
    if (ret < 0)
      ret = mkv_packet_timeline_update(s, 0, 0, FRAME_EOF);
    else
      ret = mkv_packet_timeline_update(s, &start_time, &end_time, flags);
    if (ret < 0) {
      av_freep(&frame_data);
      if (ret == AVERROR(EAGAIN))
        goto again;
    }
  }
  if (ret < 0) {
    const char * mkv_error = mkv_GetLastError(ctx->matroska);
    if (mkv_error)
      av_log(s, AV_LOG_ERROR, "mkv error: %s\n", mkv_GetLastError(ctx->matroska));
    return AVERROR_EOF;
  }

  track = &ctx->tracks[track_num];
  if (track_num >= ctx->num_tracks || !track->stream || track->stream->discard == AVDISCARD_ALL) {
    av_freep(&frame_data);
    goto again;
  }

  /* zlib compression */
  if (track->cs) {
    unsigned int frame_size = 0;
    pkt->size = 0;
    if (ctx->BufferSize)
      av_new_packet(pkt, ctx->BufferSize);
    cs_NextFrame(track->cs, size, frame_data);
    for(;;) {
      ret = cs_ReadData(track->cs, ctx->CSBuffer, sizeof(ctx->CSBuffer));
      if (ret < 0) {
        av_log(s, AV_LOG_ERROR, "cs_ReadData failed");
        av_freep(&frame_data);
        av_packet_unref(pkt);
        return AVERROR(EIO);
      } else if (ret == 0) {
        size = frame_size;
        break;
      }
      if (pkt->size < (frame_size + ret)) {
        av_grow_packet(pkt, (frame_size + ret));
        ctx->BufferSize = pkt->size;
      }
      memcpy(pkt->data+frame_size, ctx->CSBuffer, ret);
      frame_size += ret;
    }
    pkt->size = size;
    av_freep(&frame_data);
  } else {
    int offset = 0;
    /* header removal compression */
    if (track->info->CompEnabled && track->info->CompMethod == COMP_PREPEND && track->info->CompMethodPrivateSize > 0) {
      offset = track->info->CompMethodPrivateSize;
    }

    if (offset > 0) {
      av_new_packet(pkt, size+offset);
      memcpy(pkt->data, track->info->CompMethodPrivate, offset);
      memcpy(pkt->data+offset, frame_data, size);
      av_freep(&frame_data);
    } else {
      av_packet_from_data(pkt, frame_data, size);
    }
  }

  if (track->stream->codecpar->codec_id == AV_CODEC_ID_WAVPACK) {
    uint8_t *wv_data;
    int wv_size = pkt->size;
    ret = matroska_parse_wavpack(track, pkt->data, &wv_data, &wv_size);
    if (ret < 0) {
        av_log(s, AV_LOG_ERROR, "Error parsing a wavpack block.\n");
        av_packet_unref(pkt);
        return ret;
    }
    av_buffer_unref(&pkt->buf);
    av_packet_from_data(pkt, wv_data, wv_size);
  } else if (track->stream->codecpar->codec_id == AV_CODEC_ID_DVB_SUBTITLE && pkt->size >= 2 && AV_RB16(pkt->data) != 0x2000) {
    int dvbsize = pkt->size + 2;
    uint8_t *dvbdata = av_malloc(dvbsize + FF_INPUT_BUFFER_PADDING_SIZE);
    AV_WB16(dvbdata, 0x2000);
    memcpy(dvbdata+2, pkt->data, pkt->size);
    memset(dvbdata+dvbsize, 0, FF_INPUT_BUFFER_PADDING_SIZE);
    av_buffer_unref(&pkt->buf);
    av_packet_from_data(pkt, dvbdata, dvbsize);
  } else if (!strcmp(track->info->CodecID, "V_PRORES")) {
    size = pkt->size + 8;
    uint8_t *buf = av_malloc(size + FF_INPUT_BUFFER_PADDING_SIZE);
    AV_WB32(buf, pkt->size);
    AV_WB32(buf + 4, MKBETAG('i', 'c', 'p', 'f'));
    memcpy(buf + 8, pkt->data, pkt->size);
    memset(buf+size, 0, FF_INPUT_BUFFER_PADDING_SIZE);
    av_buffer_unref(&pkt->buf);
    av_packet_from_data(pkt, buf, size);
  }

  if (track->refresh_extradata) {
    mkv_packet_param_change(s, track->info, track->stream->codecpar->codec_id, pkt);
    track->refresh_extradata = 0;
  }

  if (discard_padding) {
    uint8_t *side_data = av_packet_new_side_data(pkt,
                                                 AV_PKT_DATA_SKIP_SAMPLES,
                                                 10);
    if(side_data == NULL) {
      av_packet_unref(pkt);
      return AVERROR(ENOMEM);
    }
    discard_padding = av_rescale_q(discard_padding,
                                   (AVRational){1, 1000000000},
                                   (AVRational){1, track->stream->codecpar->sample_rate});
    if (discard_padding > 0) {
      AV_WL32(side_data, 0);
      AV_WL32(side_data + 4, discard_padding);
    } else {
      AV_WL32(side_data, -discard_padding);
      AV_WL32(side_data + 4, 0);
    }
  }

  if (!(flags & FRAME_UNKNOWN_START)) {
    if (track->ms_compat)
      pkt->dts = start_time - track->info->CodecDelay;
    else
      pkt->pts = start_time - track->info->CodecDelay;

    pkt->duration = end_time - start_time;
  }

  pkt->flags = (flags & FRAME_KF) ? AV_PKT_FLAG_KEY : 0;
  pkt->pos = pos;
  pkt->stream_index = track->stream->index;

  return 0;
}

static int mkv_read_close(AVFormatContext *s)
{
  MatroskaDemuxContext *ctx = (MatroskaDemuxContext *)s->priv_data;
  int i;

  for (i = 0; i < ctx->num_segments; i++) {
    mkv_Close(ctx->segments[i]->matroska);
    if (ctx->segments[i]->free_avio)
      avio_closep(&ctx->segments[i]->iostream->pb);
    av_freep(&ctx->segments[i]->iostream);
    av_freep(&ctx->segments[i]);
  }
  av_freep(&ctx->segments);

  for (i = 0; i < ctx->num_tracks; i++) {
    av_freep(&ctx->tracks[i].cs);
  }
  av_freep(&ctx->tracks);
  av_freep(&ctx->editions);
  av_freep(&ctx->timeline);
  av_freep(&ctx->aveditions);

  if (ctx->fake_edition) {
    av_freep(&ctx->fake_edition->Children);
    av_freep(&ctx->fake_edition);
  }

  return 0;
}

static int mkv_read_seek(AVFormatContext *s, int stream_index, int64_t timestamp, int flags)
{
  MatroskaDemuxContext *ctx = (MatroskaDemuxContext *)s->priv_data;
  int mkvflags = (!(flags & AVSEEK_FLAG_ANY) && !(s->flags & AVFMT_FLAG_NETWORK)) ? MKVF_SEEK_TO_PREV_KEYFRAME : 0;
  int64_t cur_dts;

  /* Switch to another edition if requested */
  if (ctx->next_edition > -1) {
    av_log(s, AV_LOG_INFO, "switching to edition %d\n", ctx->next_edition);
    mkv_switch_edition(s, ctx->next_edition);
    ctx->active_edition = ctx->next_edition;
    ctx->next_edition = -1;
  }

  /* Update timeline and segment for ordered chapters */
  if (ctx->virtual_timeline) {
    VirtualTimelineEntry *vt = mkv_get_timeline_entry(s, timestamp);
    mkv_switch_segment(s, vt->segment->matroska, 1);
    ctx->timeline_position = vt->index;
    timestamp += vt->offset;
  }

  /* update track mask */
  if (!(s->flags & AVFMT_FLAG_NETWORK))
    mkv_SetTrackMask(ctx->matroska, mkv_get_track_mask(ctx));

  /* perform seek */
  mkv_Seek_CueAware(ctx->matroska, timestamp, mkvflags, 0);

  /* Update current timestamp */
  cur_dts = mkv_GetLowestQTimecode(ctx->matroska);
  av_log(s, AV_LOG_DEBUG, "mkv_read_seek: requested: %"PRId64", achieved: %"PRId64, timestamp, cur_dts);
  if (cur_dts == -1)
    cur_dts = timestamp;

  if (ctx->virtual_timeline) {
    cur_dts -= ctx->timeline[ctx->timeline_position].offset;
  }

  ff_update_cur_dts(s, ctx->tracks[stream_index].stream, cur_dts);

  return 0;
}

int av_mkv_get_num_editions(AVFormatContext *s)
{
  MatroskaDemuxContext *ctx = (MatroskaDemuxContext *)s->priv_data;
  return ctx->num_editions;
}

int av_mkv_get_editions(AVFormatContext *s, AVEdition **editions)
{
  MatroskaDemuxContext *ctx = (MatroskaDemuxContext *)s->priv_data;
  if (!editions)
    return -1;
  *editions = ctx->aveditions;
  return 0;
}

int av_mkv_set_next_edition(AVFormatContext *s, int index)
{
  MatroskaDemuxContext *ctx = (MatroskaDemuxContext *)s->priv_data;
  if (index < 0 || index >= ctx->num_editions)
    return -1;

  ctx->next_edition = index;
  return 0;
}

int av_mkv_get_edition(AVFormatContext *s)
{
  MatroskaDemuxContext *ctx = (MatroskaDemuxContext *)s->priv_data;
  return ctx->active_edition;
}

AVInputFormat ff_matroska_haali_demuxer = {
    .name           = "matroska",
    .long_name      = NULL_IF_CONFIG_SMALL("Matroska/WebM"),
    .priv_data_size = sizeof(MatroskaDemuxContext),
    .read_probe     = mkv_probe,
    .read_header    = mkv_read_header,
    .read_packet    = mkv_read_packet,
    .read_close     = mkv_read_close,
    .read_seek      = mkv_read_seek,
};
