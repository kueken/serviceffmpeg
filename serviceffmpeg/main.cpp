/*
 * ffmpeg-player/main.cpp
 *
 * External player process for serviceffmpeg.
 * Connects to Enigma2 plugin via Unix domain socket.
 * Uses FFmpeg for all demux/decode/protocol operations.
 * Writes Elementary Streams to /dev/dvb kernel sink (HW decode via BCM).
 *
 * License: GPL v2
 *
 * Fixes vs. original:
 *   FIX 1: VIDEO_SET_STREAMTYPE with bcm_ioctls.h codec mapping
 *   FIX 2: AUDIO_SET_ENCODING with bcm_ioctls.h audio type mapping
 *   FIX 3: VIDEO_SET_CODEC_DATA for H.264/HEVC extradata (SPS/PPS/VPS)
 *   FIX 4: AUDIO_CLEAR_BUFFER after audio track switch
 *   FIX 5: Correct PES stream_id for private-stream audio (AC3/DTS/LPCM)
 *   FIX 6: VIDEO_CLEAR_BUFFER + AUDIO_CLEAR_BUFFER after seek
 */

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/time.h>
#include <libavutil/opt.h>
#include <libavutil/dict.h>
#include <libswresample/swresample.h>
#ifdef HAVE_LIBASS
#include <ass/ass.h>
#endif
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <linux/dvb/audio.h>
#include <linux/dvb/video.h>
#include <sys/ioctl.h>

/* BCM-specific stream type enums and VIDEO_SET_CODEC_DATA ioctl */
#include "bcm_ioctls.h"

#include <string>
#include <vector>
#include <map>
#include <atomic>

/* ======================================================================
 * Configuration / compile-time options
 * ====================================================================== */
#define PLAYER_VERSION              "1.0.0-bcm"
#define DVB_AUDIO_DEVICE            "/dev/dvb/adapter0/audio0"
#define DVB_VIDEO_DEVICE            "/dev/dvb/adapter0/video0"
#define BUFFER_SIZE_DEFAULT         (4   * 1024 * 1024)
#define POSITION_REPORT_INTERVAL_MS 500

/* ======================================================================
 * FIX 1: BCM video codec mapping
 * Maps FFmpeg AVCodecID -> video_stream_type_t (bcm_ioctls.h)
 * ====================================================================== */
static video_stream_type_t codec_to_bcm_video(AVCodecID id)
{
    switch (id)
    {
    case AV_CODEC_ID_MPEG1VIDEO:  return STREAMTYPE_MPEG1;
    case AV_CODEC_ID_MPEG2VIDEO:  return STREAMTYPE_MPEG2;
    case AV_CODEC_ID_H264:        return STREAMTYPE_MPEG4_H264;
    case AV_CODEC_ID_HEVC:        return STREAMTYPE_MPEG4_H265;
    case AV_CODEC_ID_H263:        return STREAMTYPE_H263;
    case AV_CODEC_ID_MPEG4:       return STREAMTYPE_MPEG4_Part2;
    case AV_CODEC_ID_MSMPEG4V3:   return STREAMTYPE_DIVX311;
    case AV_CODEC_ID_DIVX:        return STREAMTYPE_DIVX4;
    case AV_CODEC_ID_VC1:         return STREAMTYPE_VC1;
    case AV_CODEC_ID_WMV3:        return STREAMTYPE_VC1_SM;
    case AV_CODEC_ID_VP6:
    case AV_CODEC_ID_VP6F:
    case AV_CODEC_ID_VP6A:        return STREAMTYPE_VB6;
    case AV_CODEC_ID_VP8:         return STREAMTYPE_VB8;
    case AV_CODEC_ID_VP9:         return STREAMTYPE_VB9;
    case AV_CODEC_ID_SPARK:       return STREAMTYPE_SPARK;
    case AV_CODEC_ID_MJPEG:       return STREAMTYPE_MJPEG;
    case AV_CODEC_ID_RV30:        return STREAMTYPE_RV30;
    case AV_CODEC_ID_RV40:        return STREAMTYPE_RV40;
    case AV_CODEC_ID_XVID:        return STREAMTYPE_XVID;
#ifdef AV_CODEC_ID_AVS2
    case AV_CODEC_ID_AVS2:        return STREAMTYPE_AVS2;
#endif
    default:
        fprintf(stderr, "[ffmpeg-player] WARNING: unknown video codec %s (%d), "
                "defaulting to STREAMTYPE_MPEG2\n",
                avcodec_get_name(id), (int)id);
        return STREAMTYPE_MPEG2;
    }
}

/* ======================================================================
 * FIX 2: BCM audio codec mapping
 * Maps FFmpeg AVCodecID -> audio_stream_type_t (bcm_ioctls.h)
 * ====================================================================== */
static audio_stream_type_t codec_to_bcm_audio(AVCodecID id)
{
    switch (id)
    {
    case AV_CODEC_ID_AC3:         return AUDIOTYPE_AC3;
    case AV_CODEC_ID_EAC3:        return AUDIOTYPE_AC3_PLUS;
    case AV_CODEC_ID_MP2:         return AUDIOTYPE_MPEG;
    case AV_CODEC_ID_MP3:         return AUDIOTYPE_MP3;
    case AV_CODEC_ID_DTS:         return AUDIOTYPE_DTS;
    case AV_CODEC_ID_DTSHD:       return AUDIOTYPE_DTS_HD;
    case AV_CODEC_ID_AAC:         return AUDIOTYPE_AAC;
    case AV_CODEC_ID_AAC_LATM:    return AUDIOTYPE_AAC_HE;
    case AV_CODEC_ID_PCM_S16LE:
    case AV_CODEC_ID_PCM_S16BE:
    case AV_CODEC_ID_PCM_S24LE:
    case AV_CODEC_ID_PCM_S24BE:   return AUDIOTYPE_LPCM;
    case AV_CODEC_ID_WMAV1:
    case AV_CODEC_ID_WMAV2:       return AUDIOTYPE_WMA;
    case AV_CODEC_ID_WMAPRO:      return AUDIOTYPE_WMA_PRO;
    case AV_CODEC_ID_OPUS:        return AUDIOTYPE_OPUS;
    case AV_CODEC_ID_VORBIS:      return AUDIOTYPE_VORBIS;
    case AV_CODEC_ID_AMR_NB:
    case AV_CODEC_ID_AMR_WB:      return AUDIOTYPE_AMR;
    default:
        fprintf(stderr, "[ffmpeg-player] WARNING: unknown audio codec %s (%d), "
                "defaulting to AUDIOTYPE_MPEG\n",
                avcodec_get_name(id), (int)id);
        return AUDIOTYPE_MPEG;
    }
}

/* ======================================================================
 * FIX 5: PES stream_id per audio codec
 * Standard MPEG audio  -> 0xC0
 * Private Stream 1     -> 0xBD  (AC3, DTS, LPCM, WMA, Opus, Vorbis ...)
 * ====================================================================== */
static uint8_t audio_pes_stream_id(AVCodecID id)
{
    switch (id)
    {
    case AV_CODEC_ID_AC3:
    case AV_CODEC_ID_EAC3:
    case AV_CODEC_ID_DTS:
    case AV_CODEC_ID_DTSHD:
    case AV_CODEC_ID_PCM_S16LE:
    case AV_CODEC_ID_PCM_S16BE:
    case AV_CODEC_ID_PCM_S24LE:
    case AV_CODEC_ID_PCM_S24BE:
    case AV_CODEC_ID_WMAV1:
    case AV_CODEC_ID_WMAV2:
    case AV_CODEC_ID_WMAPRO:
    case AV_CODEC_ID_OPUS:
    case AV_CODEC_ID_VORBIS:
    case AV_CODEC_ID_TRUEHD:
        return 0xBD;   /* Private Stream 1 */
    default:
        return 0xC0;   /* MPEG audio */
    }
}

/* ======================================================================
 * JSON helpers
 * ====================================================================== */
static std::string jstr(const std::string &k, const std::string &v)
{ return "\"" + k + "\":\"" + v + "\""; }
static std::string jint(const std::string &k, long long v)
{ char b[64]; snprintf(b,sizeof(b),"%lld",v); return "\""+k+"\":"+b; }
static std::string jbool(const std::string &k, bool v)
{ return "\"" + k + "\":" + (v ? "true" : "false"); }
static std::string json_get_str(const std::string &j, const std::string &k)
{
    std::string n = "\"" + k + "\":\"";
    size_t p = j.find(n); if (p==std::string::npos) return "";
    p += n.length(); size_t e = j.find('"', p);
    return (e==std::string::npos) ? "" : j.substr(p, e-p);
}
static long long json_get_int(const std::string &j, const std::string &k)
{
    std::string n = "\"" + k + "\":";
    size_t p = j.find(n); if (p==std::string::npos) return 0;
    return strtoll(j.c_str() + p + n.length(), NULL, 10);
}

/* ======================================================================
 * Track descriptors
 * ====================================================================== */
struct AudioTrack {
    int         stream_idx;
    int         pid;
    std::string lang;
    std::string codec;
    AVCodecID   codec_id;
    int         channels;
    int         samplerate;
    int         bitrate;
};
struct SubTrack {
    int         stream_idx;
    std::string lang;
    std::string codec;
    bool        bitmap;
};

/* ======================================================================
 * Player state
 * ====================================================================== */
struct PlayerState
{
    int             ipc_fd;
    std::string     recv_buf;
    std::string     uri;
    std::string     useragent;
    std::string     extra_headers;
    int             buffer_size;
    AVFormatContext *fmt_ctx;
    int             video_stream_idx;
    int             audio_stream_idx;
    int             sub_stream_idx;
    std::vector<AudioTrack> audio_tracks;
    std::vector<SubTrack>   sub_tracks;
    int             active_audio_track;
    int             active_sub_track;
    std::atomic<bool>    running;
    std::atomic<bool>    paused;
    std::atomic<bool>    stop_requested;
    std::atomic<int>     speed;
    std::atomic<int64_t> seek_target_ms;
    pthread_mutex_t      seek_mutex;
    int             dvb_video_fd;
    int             dvb_audio_fd;
    bool            hw_sink_available;
    /* Track active codec IDs for PES framing and BCM re-config */
    AVCodecID       active_video_codec_id;
    AVCodecID       active_audio_codec_id;
    AVFormatContext *record_ctx;
    std::string     record_path;
    bool            recording;
    pthread_mutex_t record_mutex;
    int64_t         duration_ms;
    int64_t         position_ms;
    int64_t         last_position_report_ms;
    bool            is_live;
    bool            seekable;
#ifdef HAVE_LIBASS
    ASS_Library    *ass_lib;
    ASS_Renderer   *ass_renderer;
    ASS_Track      *ass_track;
#endif

    PlayerState()
        : ipc_fd(-1), buffer_size(BUFFER_SIZE_DEFAULT)
        , fmt_ctx(NULL)
        , video_stream_idx(-1), audio_stream_idx(-1), sub_stream_idx(-1)
        , active_audio_track(-1), active_sub_track(-1)
        , running(false), paused(false), stop_requested(false)
        , speed(0), seek_target_ms(-1)
        , dvb_video_fd(-1), dvb_audio_fd(-1), hw_sink_available(false)
        , active_video_codec_id(AV_CODEC_ID_NONE)
        , active_audio_codec_id(AV_CODEC_ID_NONE)
        , record_ctx(NULL), recording(false)
        , duration_ms(0), position_ms(0), last_position_report_ms(0)
        , is_live(false), seekable(false)
    {
        pthread_mutex_init(&seek_mutex, NULL);
        pthread_mutex_init(&record_mutex, NULL);
#ifdef HAVE_LIBASS
        ass_lib = NULL; ass_renderer = NULL; ass_track = NULL;
#endif
    }
};
static PlayerState G;

/* ======================================================================
 * IPC helpers
 * ====================================================================== */
static void ipc_send(const std::string &evt, const std::string &params = "")
{
    if (G.ipc_fd < 0) return;
    std::string msg = "{\"evt\":\"" + evt + "\"";
    if (!params.empty()) msg += "," + params;
    msg += "}\n";
    write(G.ipc_fd, msg.c_str(), msg.length());
}
static void ipc_send_error(int code, const std::string &msg)
{ ipc_send("error", jint("code",code) + "," + jstr("msg",msg)); }

/* ======================================================================
 * DVB sink open
 *
 * NOTE: VIDEO_SET_STREAMTYPE and AUDIO_SET_ENCODING are NOT called here.
 * They are called in configure_dvb_video_codec() / configure_dvb_audio_codec()
 * AFTER open_input() has identified the actual codec IDs.
 * ====================================================================== */
static bool open_dvb_sink()
{
    G.dvb_audio_fd = open(DVB_AUDIO_DEVICE, O_RDWR | O_CLOEXEC);
    G.dvb_video_fd = open(DVB_VIDEO_DEVICE, O_RDWR | O_CLOEXEC);

    if (G.dvb_audio_fd < 0 || G.dvb_video_fd < 0)
    {
        fprintf(stderr, "[ffmpeg-player] DVB sink unavailable (%s), using SW path\n",
                strerror(errno));
        if (G.dvb_audio_fd >= 0) { close(G.dvb_audio_fd); G.dvb_audio_fd = -1; }
        if (G.dvb_video_fd >= 0) { close(G.dvb_video_fd); G.dvb_video_fd = -1; }
        return false;
    }
    /* Bypass: we feed raw ES — hardware decoder does all decoding */
    int bypass = 1;
    ioctl(G.dvb_audio_fd, AUDIO_SET_BYPASS_MODE, bypass);
    /* Do NOT call VIDEO_PLAY yet — stream type must be set first */
    fprintf(stderr, "[ffmpeg-player] DVB HW sink opened (codec config pending)\n");
    return true;
}

/*
 * configure_dvb_video_codec()
 *
 * FIX 1: VIDEO_SET_STREAMTYPE with the correct BCM stream type enum.
 * FIX 3: VIDEO_SET_CODEC_DATA passes SPS/PPS (H.264) or VPS/SPS/PPS
 *         (HEVC) extradata to the hardware decoder before the first
 *         frame. Without this, BCM H.264/HEVC decoders stall silently.
 *
 * Must be called after open_input() when video_stream_idx is valid.
 */
static void configure_dvb_video_codec()
{
    if (G.dvb_video_fd < 0 || G.video_stream_idx < 0) return;

    AVStream  *vs  = G.fmt_ctx->streams[G.video_stream_idx];
    AVCodecID  cid = vs->codecpar->codec_id;
    G.active_video_codec_id = cid;

    /* FIX 1 */
    video_stream_type_t bcm_type = codec_to_bcm_video(cid);
    if (ioctl(G.dvb_video_fd, VIDEO_SET_STREAMTYPE, (int)bcm_type) < 0)
        fprintf(stderr, "[ffmpeg-player] VIDEO_SET_STREAMTYPE(%d) failed: %m\n",
                (int)bcm_type);
    else
        fprintf(stderr, "[ffmpeg-player] VIDEO_SET_STREAMTYPE = %d (%s)\n",
                (int)bcm_type, avcodec_get_name(cid));

    /* FIX 3 */
    if (vs->codecpar->extradata && vs->codecpar->extradata_size > 0)
    {
        video_codec_data_t cd;
        cd.length = vs->codecpar->extradata_size;
        cd.data   = vs->codecpar->extradata;
        if (ioctl(G.dvb_video_fd, VIDEO_SET_CODEC_DATA, &cd) < 0)
            fprintf(stderr,
                    "[ffmpeg-player] VIDEO_SET_CODEC_DATA failed: %m "
                    "(extradata_size=%d)\n", cd.length);
        else
            fprintf(stderr,
                    "[ffmpeg-player] VIDEO_SET_CODEC_DATA sent (%d bytes)\n",
                    cd.length);
    }
    else
    {
        fprintf(stderr,
                "[ffmpeg-player] No extradata for %s — "
                "H.264/HEVC decoder may not start\n",
                avcodec_get_name(cid));
    }

    /* Safe to start now that stream type and extradata are set */
    ioctl(G.dvb_video_fd, VIDEO_PLAY);
}

/*
 * configure_dvb_audio_codec()
 *
 * FIX 2: AUDIO_SET_ENCODING with the codec-specific BCM audio type.
 * Called after open_input() and after every audio track switch (FIX 4).
 */
static void configure_dvb_audio_codec(AVCodecID cid)
{
    if (G.dvb_audio_fd < 0) return;
    G.active_audio_codec_id = cid;

    audio_stream_type_t bcm_audio = codec_to_bcm_audio(cid);
    if (ioctl(G.dvb_audio_fd, AUDIO_SET_ENCODING, (int)bcm_audio) < 0)
        fprintf(stderr,
                "[ffmpeg-player] AUDIO_SET_ENCODING(%d) failed: %m\n",
                (int)bcm_audio);
    else
        fprintf(stderr,
                "[ffmpeg-player] AUDIO_SET_ENCODING = %d (%s)\n",
                (int)bcm_audio, avcodec_get_name(cid));

    ioctl(G.dvb_audio_fd, AUDIO_PLAY);
}

static void close_dvb_sink()
{
    if (G.dvb_video_fd >= 0)
    { ioctl(G.dvb_video_fd, VIDEO_STOP); close(G.dvb_video_fd); G.dvb_video_fd = -1; }
    if (G.dvb_audio_fd >= 0)
    { ioctl(G.dvb_audio_fd, AUDIO_STOP); close(G.dvb_audio_fd); G.dvb_audio_fd = -1; }
}

/* ======================================================================
 * PES header builder
 *
 * FIX 5 is applied by the caller: audio stream_id comes from
 * audio_pes_stream_id() so private streams get 0xBD, not 0xC0.
 * ====================================================================== */
static void build_pes_header(uint8_t *pes, uint8_t stream_id,
                             int payload_size, uint64_t pts)
{
    /* PES_packet_length = flags(3) + PTS(5) + payload = payload + 8 */
    int plen = payload_size + 8;
    pes[0] = 0x00; pes[1] = 0x00; pes[2] = 0x01;
    pes[3] = stream_id;
    pes[4] = (plen >> 8) & 0xFF;
    pes[5] = plen & 0xFF;
    pes[6] = 0x80;  /* marker bits */
    pes[7] = 0x80;  /* PTS present */
    pes[8] = 0x05;  /* PES_header_data_length */
    pes[9]  = (uint8_t)(0x21 | ((pts >> 29) & 0x0E));
    pes[10] = (uint8_t)((pts >> 22) & 0xFF);
    pes[11] = (uint8_t)(0x01 | ((pts >> 14) & 0xFE));
    pes[12] = (uint8_t)((pts >> 7)  & 0xFF);
    pes[13] = (uint8_t)(0x01 | ((pts << 1)  & 0xFE));
}

static bool write_dvb_video(const uint8_t *data, int size, int64_t pts_90khz)
{
    if (G.dvb_video_fd < 0) return false;
    uint8_t pes[14];
    build_pes_header(pes, 0xE0, size,
                     pts_90khz == (int64_t)AV_NOPTS_VALUE ? 0 : (uint64_t)pts_90khz);
    write(G.dvb_video_fd, pes, 14);
    write(G.dvb_video_fd, data, size);
    return true;
}

static bool write_dvb_audio(const uint8_t *data, int size,
                             int64_t pts_90khz, AVCodecID cid)
{
    if (G.dvb_audio_fd < 0) return false;
    uint8_t pes[14];
    build_pes_header(pes, audio_pes_stream_id(cid), size,   /* FIX 5 */
                     pts_90khz == (int64_t)AV_NOPTS_VALUE ? 0 : (uint64_t)pts_90khz);
    write(G.dvb_audio_fd, pes, 14);
    write(G.dvb_audio_fd, data, size);
    return true;
}

/* ======================================================================
 * Track info → E2
 * ====================================================================== */
static void send_track_info()
{
    std::string audio_arr = "[";
    for (size_t i = 0; i < G.audio_tracks.size(); i++)
    {
        const AudioTrack &t = G.audio_tracks[i];
        if (i > 0) audio_arr += ",";
        audio_arr += "{";
        audio_arr += jint("id",t.stream_idx) + ",";
        audio_arr += jint("pid",t.pid) + ",";
        audio_arr += jstr("lang",t.lang) + ",";
        audio_arr += jstr("codec",t.codec) + ",";
        audio_arr += jint("ch",t.channels) + ",";
        audio_arr += jint("rate",t.samplerate);
        audio_arr += "}";
    }
    audio_arr += "]";

    std::string sub_arr = "[";
    for (size_t i = 0; i < G.sub_tracks.size(); i++)
    {
        const SubTrack &t = G.sub_tracks[i];
        if (i > 0) sub_arr += ",";
        sub_arr += "{";
        sub_arr += jint("id",t.stream_idx) + ",";
        sub_arr += jstr("lang",t.lang) + ",";
        sub_arr += jstr("codec",t.codec) + ",";
        sub_arr += jbool("bitmap",t.bitmap);
        sub_arr += "}";
    }
    sub_arr += "]";

    AVStream *vs  = G.video_stream_idx >= 0
                    ? G.fmt_ctx->streams[G.video_stream_idx] : NULL;
    AVStream *as_ = G.audio_stream_idx >= 0
                    ? G.fmt_ctx->streams[G.audio_stream_idx] : NULL;

    std::string params;
    params += jbool("seekable", G.seekable) + ",";
    params += jbool("live", G.is_live) + ",";
    params += jint("dur_ms", G.duration_ms) + ",";
    params += jstr("container",
                   G.fmt_ctx->iformat ? G.fmt_ctx->iformat->name : "") + ",";
    AVDictionaryEntry *title = av_dict_get(G.fmt_ctx->metadata, "title", NULL, 0);
    params += jstr("title", title ? title->value : "") + ",";

    if (vs && vs->codecpar)
    {
        params += jstr("vcodec", avcodec_get_name(vs->codecpar->codec_id)) + ",";
        params += jint("w", vs->codecpar->width) + ",";
        params += jint("h", vs->codecpar->height) + ",";
        int aspect = 2;
        if (vs->codecpar->sample_aspect_ratio.num > 0 &&
            vs->codecpar->sample_aspect_ratio.den > 0)
        {
            float sar = (float)vs->codecpar->width / vs->codecpar->height
                      * (float)vs->codecpar->sample_aspect_ratio.num
                      / vs->codecpar->sample_aspect_ratio.den;
            if (sar < 1.5f) aspect = 1;
            else if (sar < 2.0f) aspect = 2;
            else aspect = 3;
        }
        params += jint("aspect", aspect) + ",";
        int fps = 0;
        if (vs->avg_frame_rate.den > 0)
            fps = (int)(1000.0 * vs->avg_frame_rate.num / vs->avg_frame_rate.den);
        params += jint("fps", fps) + ",";
        if (as_) params += jint("vbps", vs->codecpar->bit_rate) + ",";
    }
    params += "\"audio\":" + audio_arr + ",";
    params += "\"subs\":"  + sub_arr;
    ipc_send("info", params);
}

/* ======================================================================
 * open_input() — FFmpeg demuxer setup
 * ====================================================================== */
static bool open_input()
{
    AVDictionary *opts = NULL;
    if (!G.useragent.empty())
        av_dict_set(&opts, "user_agent", G.useragent.c_str(), 0);
    if (!G.extra_headers.empty())
        av_dict_set(&opts, "headers", G.extra_headers.c_str(), 0);
    av_dict_set_int(&opts, "buffer_size", G.buffer_size, 0);
    av_dict_set(&opts, "reconnect", "1", 0);
    av_dict_set(&opts, "reconnect_streamed", "1", 0);
    av_dict_set(&opts, "reconnect_delay_max", "5", 0);
    av_dict_set(&opts, "timeout", "30000000", 0);
    av_dict_set(&opts, "rw_timeout", "30000000", 0);
    av_dict_set(&opts, "hls_use_localtime", "1", 0);

    G.fmt_ctx = avformat_alloc_context();
    if (!G.fmt_ctx) { ipc_send_error(1, "avformat_alloc_context failed"); return false; }

    int ret = avformat_open_input(&G.fmt_ctx, G.uri.c_str(), NULL, &opts);
    av_dict_free(&opts);
    if (ret < 0)
    {
        char errbuf[256]; av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "[ffmpeg-player] avformat_open_input: %s\n", errbuf);
        ipc_send_error(1, "open failed: " + std::string(errbuf));
        avformat_free_context(G.fmt_ctx); G.fmt_ctx = NULL;
        return false;
    }

    avformat_find_stream_info(G.fmt_ctx, NULL);

    if (G.fmt_ctx->duration > 0)
        G.duration_ms = G.fmt_ctx->duration / 1000;

    G.is_live  = (G.fmt_ctx->duration <= 0 || !G.fmt_ctx->pb ||
                  (G.fmt_ctx->ctx_flags & AVFMTCTX_NOHEADER));
    G.seekable = !G.is_live;
    if (G.fmt_ctx->pb && G.fmt_ctx->pb->seekable) G.seekable = true;

    G.video_stream_idx = av_find_best_stream(G.fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    G.audio_stream_idx = av_find_best_stream(G.fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);

    G.audio_tracks.clear();
    G.sub_tracks.clear();

    for (unsigned int i = 0; i < G.fmt_ctx->nb_streams; i++)
    {
        AVStream *st = G.fmt_ctx->streams[i];
        AVDictionaryEntry *lang = av_dict_get(st->metadata, "language", NULL, 0);
        std::string lang_str = lang ? lang->value : "und";

        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            AudioTrack t;
            t.stream_idx = i; t.pid = st->id; t.lang = lang_str;
            t.codec      = avcodec_get_name(st->codecpar->codec_id);
            t.codec_id   = st->codecpar->codec_id;
            t.channels   = st->codecpar->ch_layout.nb_channels;
            t.samplerate = st->codecpar->sample_rate;
            t.bitrate    = (int)st->codecpar->bit_rate;
            G.audio_tracks.push_back(t);
            if ((int)i == G.audio_stream_idx)
                G.active_audio_track = (int)G.audio_tracks.size() - 1;
        }
        else if (st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE)
        {
            SubTrack t;
            t.stream_idx = i; t.lang = lang_str;
            t.codec   = avcodec_get_name(st->codecpar->codec_id);
            t.bitmap  = (st->codecpar->codec_id == AV_CODEC_ID_DVD_SUBTITLE ||
                         st->codecpar->codec_id == AV_CODEC_ID_DVB_SUBTITLE ||
                         st->codecpar->codec_id == AV_CODEC_ID_HDMV_PGS_SUBTITLE);
            G.sub_tracks.push_back(t);
        }
    }

    for (unsigned int i = 0; i < G.fmt_ctx->nb_streams; i++)
        G.fmt_ctx->streams[i]->discard = AVDISCARD_ALL;
    if (G.video_stream_idx >= 0)
        G.fmt_ctx->streams[G.video_stream_idx]->discard = AVDISCARD_DEFAULT;
    if (G.audio_stream_idx >= 0)
        G.fmt_ctx->streams[G.audio_stream_idx]->discard = AVDISCARD_DEFAULT;

    fprintf(stderr,
            "[ffmpeg-player] opened: %s\n"
            "  video=%d (%s)  audio=%d (%s)  dur=%lldms live=%d seekable=%d\n"
            "  audio_tracks=%zu sub_tracks=%zu\n",
            G.uri.c_str(),
            G.video_stream_idx,
            G.video_stream_idx >= 0
              ? avcodec_get_name(G.fmt_ctx->streams[G.video_stream_idx]->codecpar->codec_id)
              : "none",
            G.audio_stream_idx,
            G.audio_stream_idx >= 0
              ? avcodec_get_name(G.fmt_ctx->streams[G.audio_stream_idx]->codecpar->codec_id)
              : "none",
            (long long)G.duration_ms, (int)G.is_live, (int)G.seekable,
            G.audio_tracks.size(), G.sub_tracks.size());
    return true;
}

/* ======================================================================
 * Recording
 * ====================================================================== */
static bool start_recording(const std::string &path)
{
    pthread_mutex_lock(&G.record_mutex);
    if (G.recording || !G.fmt_ctx)
    { pthread_mutex_unlock(&G.record_mutex); return false; }

    int ret = avformat_alloc_output_context2(&G.record_ctx, NULL, "mpegts", path.c_str());
    if (ret < 0 || !G.record_ctx)
    { fprintf(stderr,"[ffmpeg-player] record: alloc failed\n");
      pthread_mutex_unlock(&G.record_mutex); return false; }

    for (unsigned int i = 0; i < G.fmt_ctx->nb_streams; i++)
    {
        AVStream *in_st = G.fmt_ctx->streams[i];
        if (in_st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
            in_st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        AVStream *out_st = avformat_new_stream(G.record_ctx, NULL);
        if (!out_st) continue;
        avcodec_parameters_copy(out_st->codecpar, in_st->codecpar);
        out_st->codecpar->codec_tag = 0;
        out_st->time_base = in_st->time_base;
    }

    if (!(G.record_ctx->oformat->flags & AVFMT_NOFILE))
        if (avio_open(&G.record_ctx->pb, path.c_str(), AVIO_FLAG_WRITE) < 0)
        {
            fprintf(stderr,"[ffmpeg-player] record: avio_open failed\n");
            avformat_free_context(G.record_ctx); G.record_ctx = NULL;
            pthread_mutex_unlock(&G.record_mutex); return false;
        }

    if (avformat_write_header(G.record_ctx, NULL) < 0)
    {
        fprintf(stderr,"[ffmpeg-player] record: write_header failed\n");
        avio_closep(&G.record_ctx->pb);
        avformat_free_context(G.record_ctx); G.record_ctx = NULL;
        pthread_mutex_unlock(&G.record_mutex); return false;
    }

    G.record_path = path; G.recording = true;
    fprintf(stderr,"[ffmpeg-player] recording to %s\n", path.c_str());
    pthread_mutex_unlock(&G.record_mutex);
    return true;
}

static void stop_recording()
{
    pthread_mutex_lock(&G.record_mutex);
    if (G.recording && G.record_ctx)
    {
        av_write_trailer(G.record_ctx);
        if (!(G.record_ctx->oformat->flags & AVFMT_NOFILE))
            avio_closep(&G.record_ctx->pb);
        avformat_free_context(G.record_ctx);
        G.record_ctx = NULL; G.recording = false;
        fprintf(stderr,"[ffmpeg-player] recording stopped\n");
    }
    pthread_mutex_unlock(&G.record_mutex);
}

/* ======================================================================
 * Packet output
 * ====================================================================== */
static void process_packet(AVPacket *pkt)
{
    if (!pkt) return;
    AVStream *st = G.fmt_ctx->streams[pkt->stream_index];
    int64_t pts_90khz = AV_NOPTS_VALUE;
    if (pkt->pts != AV_NOPTS_VALUE)
        pts_90khz = av_rescale_q(pkt->pts, st->time_base, {1, 90000});
    if (pkt->pts != AV_NOPTS_VALUE)
    {
        int64_t pos_ms = av_rescale_q(pkt->pts, st->time_base, {1, 1000});
        if (pos_ms > 0) G.position_ms = pos_ms;
    }

    if (G.hw_sink_available)
    {
        if (pkt->stream_index == G.video_stream_idx)
            write_dvb_video(pkt->data, pkt->size, pts_90khz);
        else if (pkt->stream_index == G.audio_stream_idx)
            write_dvb_audio(pkt->data, pkt->size, pts_90khz,
                            G.active_audio_codec_id);   /* FIX 5 */
    }

    if (G.recording && G.record_ctx)
    {
        pthread_mutex_lock(&G.record_mutex);
        if (G.recording && G.record_ctx)
            for (unsigned int oi = 0; oi < G.record_ctx->nb_streams; oi++)
                if (G.record_ctx->streams[oi]->codecpar->codec_id ==
                    G.fmt_ctx->streams[pkt->stream_index]->codecpar->codec_id)
                {
                    AVPacket op; av_packet_ref(&op, pkt);
                    op.stream_index = oi;
                    av_packet_rescale_ts(&op,
                        G.fmt_ctx->streams[pkt->stream_index]->time_base,
                        G.record_ctx->streams[oi]->time_base);
                    av_interleaved_write_frame(G.record_ctx, &op);
                    av_packet_unref(&op); break;
                }
        pthread_mutex_unlock(&G.record_mutex);
    }
}

/* ======================================================================
 * IPC command handler
 * ====================================================================== */
static void handle_command(const std::string &cmd, const std::string &payload)
{
    fprintf(stderr, "[ffmpeg-player] cmd: %s\n", cmd.c_str());

    if (cmd == "pause")
    {
        G.paused = true;
        if (G.dvb_video_fd >= 0) ioctl(G.dvb_video_fd, VIDEO_FREEZE);
        if (G.dvb_audio_fd >= 0) ioctl(G.dvb_audio_fd, AUDIO_PAUSE);
    }
    else if (cmd == "resume")
    {
        G.paused = false;
        if (G.dvb_video_fd >= 0) ioctl(G.dvb_video_fd, VIDEO_CONTINUE);
        if (G.dvb_audio_fd >= 0) ioctl(G.dvb_audio_fd, AUDIO_CONTINUE);
    }
    else if (cmd == "stop")
    {
        G.stop_requested = true;
    }
    else if (cmd == "seek")
    {
        int64_t pos_ms = json_get_int(payload, "pos_ms");
        pthread_mutex_lock(&G.seek_mutex);
        G.seek_target_ms = pos_ms;
        pthread_mutex_unlock(&G.seek_mutex);
    }
    else if (cmd == "seek_rel")
    {
        int64_t delta = json_get_int(payload, "delta_ms");
        pthread_mutex_lock(&G.seek_mutex);
        G.seek_target_ms = G.position_ms + delta;
        pthread_mutex_unlock(&G.seek_mutex);
    }
    else if (cmd == "set_speed")
    {
        G.speed = (int)json_get_int(payload, "speed");
        if (G.dvb_video_fd >= 0)
        {
            if (G.speed == 0 || G.speed == 1)
            {
                ioctl(G.dvb_video_fd, VIDEO_FAST_FORWARD, 0);
                ioctl(G.dvb_video_fd, VIDEO_SLOWMOTION, 0);
                ioctl(G.dvb_video_fd, VIDEO_CONTINUE);
            }
            else if (G.speed > 1)
                ioctl(G.dvb_video_fd, VIDEO_FAST_FORWARD, G.speed.load());
            else
                ioctl(G.dvb_video_fd, VIDEO_FAST_FORWARD, -G.speed.load());
        }
    }
    else if (cmd == "set_audio")
    {
        int id = (int)json_get_int(payload, "id");
        for (size_t i = 0; i < G.audio_tracks.size(); i++)
        {
            if (G.audio_tracks[i].stream_idx != id) continue;

            if (G.audio_stream_idx >= 0)
                G.fmt_ctx->streams[G.audio_stream_idx]->discard = AVDISCARD_ALL;

            G.audio_stream_idx   = id;
            G.active_audio_track = (int)i;
            G.fmt_ctx->streams[id]->discard = AVDISCARD_DEFAULT;

            /* FIX 4: flush hardware audio buffer before re-configuring */
            if (G.dvb_audio_fd >= 0)
            {
                ioctl(G.dvb_audio_fd, AUDIO_PAUSE);
                ioctl(G.dvb_audio_fd, AUDIO_CLEAR_BUFFER);
            }
            /* FIX 2: set encoding type for the new track */
            configure_dvb_audio_codec(G.audio_tracks[i].codec_id);

            fprintf(stderr, "[ffmpeg-player] audio -> stream %d (%s / %s)\n",
                    id, G.audio_tracks[i].lang.c_str(),
                    G.audio_tracks[i].codec.c_str());
            break;
        }
    }
    else if (cmd == "set_subtitle")
    {
        int id = (int)json_get_int(payload, "id");
        if (id < 0)
        {
            if (G.sub_stream_idx >= 0)
                G.fmt_ctx->streams[G.sub_stream_idx]->discard = AVDISCARD_ALL;
            G.sub_stream_idx = -1; G.active_sub_track = -1;
        }
        else
        {
            if (G.sub_stream_idx >= 0)
                G.fmt_ctx->streams[G.sub_stream_idx]->discard = AVDISCARD_ALL;
            G.sub_stream_idx = id;
            G.fmt_ctx->streams[id]->discard = AVDISCARD_DEFAULT;
            for (size_t i = 0; i < G.sub_tracks.size(); i++)
                if (G.sub_tracks[i].stream_idx == id) { G.active_sub_track=(int)i; break; }
        }
    }
    else if (cmd == "set_ua")     { G.useragent    = json_get_str(payload, "ua"); }
    else if (cmd == "set_headers"){ G.extra_headers = json_get_str(payload, "headers"); }
    else if (cmd == "record_start") { start_recording(json_get_str(payload, "path")); }
    else if (cmd == "record_stop")  { stop_recording(); }
    else if (cmd == "set_bufsize")
        { G.buffer_size = (int)json_get_int(payload, "bytes"); }
    else if (cmd == "get_info")
        { if (G.fmt_ctx) send_track_info(); }
    /* "play" is handled in main loop — ignore here */
}

/* ======================================================================
 * IPC poll
 * ====================================================================== */
static void poll_ipc()
{
    if (G.ipc_fd < 0) return;
    char buf[4096];
    ssize_t n = read(G.ipc_fd, buf, sizeof(buf)-1);
    if (n <= 0) return;
    buf[n] = 0;
    G.recv_buf += buf;
    size_t pos;
    while ((pos = G.recv_buf.find('\n')) != std::string::npos)
    {
        std::string line = G.recv_buf.substr(0, pos);
        G.recv_buf       = G.recv_buf.substr(pos + 1);
        if (line.empty()) continue;
        handle_command(json_get_str(line, "cmd"), line);
    }
}

/* ======================================================================
 * Main playback loop
 * ====================================================================== */
static void playback_loop()
{
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) { ipc_send_error(1, "av_packet_alloc failed"); return; }

    ipc_send("started");
    send_track_info();

    if (G.video_stream_idx >= 0)
    {
        AVStream *vs = G.fmt_ctx->streams[G.video_stream_idx];
        int fps = vs->avg_frame_rate.den > 0
                  ? (int)(1000.0 * vs->avg_frame_rate.num / vs->avg_frame_rate.den) : 0;
        ipc_send("video_size",
                 jint("w", vs->codecpar->width) + "," +
                 jint("h", vs->codecpar->height) + "," +
                 jint("aspect", 2) + "," +
                 jint("fps", fps) + "," +
                 jbool("progressive", true));
    }

    int64_t last_pos_report_ms = av_gettime_relative() / 1000;

    while (!G.stop_requested)
    {
        /* Handle pending seek */
        pthread_mutex_lock(&G.seek_mutex);
        int64_t seek_target = G.seek_target_ms.load();
        if (seek_target >= 0) G.seek_target_ms = -1;
        pthread_mutex_unlock(&G.seek_mutex);

        if (seek_target >= 0)
        {
            int ret = av_seek_frame(G.fmt_ctx, -1, seek_target * 1000,
                                    seek_target < G.position_ms
                                    ? AVSEEK_FLAG_BACKWARD : 0);
            if (ret < 0)
                fprintf(stderr, "[ffmpeg-player] seek to %lldms failed\n",
                        (long long)seek_target);
            else
            {
                G.position_ms = seek_target;
                /* FIX 6: clear HW buffers to avoid timestamp discontinuity glitch */
                if (G.dvb_video_fd >= 0) ioctl(G.dvb_video_fd, VIDEO_CLEAR_BUFFER);
                if (G.dvb_audio_fd >= 0) ioctl(G.dvb_audio_fd, AUDIO_CLEAR_BUFFER);
            }
        }

        if (G.paused) { poll_ipc(); usleep(20000); continue; }

        poll_ipc();

        int ret = av_read_frame(G.fmt_ctx, pkt);
        if (ret == AVERROR_EOF)
        {
            fprintf(stderr, "[ffmpeg-player] EOF\n");
            ipc_send("eof"); break;
        }
        if (ret == AVERROR(EAGAIN)) { usleep(5000); continue; }
        if (ret < 0)
        {
            char eb[256]; av_strerror(ret, eb, sizeof(eb));
            fprintf(stderr, "[ffmpeg-player] av_read_frame: %s\n", eb);
            if (G.is_live)
            {
                ipc_send("buffering", jint("pct", 0));
                usleep(500000);
                av_seek_frame(G.fmt_ctx, -1, 0, AVSEEK_FLAG_BACKWARD);
                ipc_send("buffer_done"); continue;
            }
            ipc_send_error(4, eb); break;
        }

        if (pkt->stream_index != G.video_stream_idx &&
            pkt->stream_index != G.audio_stream_idx &&
            pkt->stream_index != G.sub_stream_idx)
        { av_packet_unref(pkt); continue; }

        process_packet(pkt);
        av_packet_unref(pkt);

        int64_t now_ms = av_gettime_relative() / 1000;
        if (now_ms - last_pos_report_ms >= POSITION_REPORT_INTERVAL_MS)
        {
            last_pos_report_ms = now_ms;
            ipc_send("position",
                     jint("pos_ms", G.position_ms) + "," +
                     jint("dur_ms", G.duration_ms));
        }
    }
    av_packet_free(&pkt);
}

/* ======================================================================
 * Signal handler
 * ====================================================================== */
static void sig_handler(int sig)
{
    fprintf(stderr, "[ffmpeg-player] signal %d\n", sig);
    G.stop_requested = true;
}

/* ======================================================================
 * main()
 * ====================================================================== */
int main(int argc, char *argv[])
{
    std::string socket_path;
    for (int i = 1; i < argc; i++)
    {
        std::string arg = argv[i];
        if (arg.substr(0, 9) == "--socket=") socket_path = arg.substr(9);
        else if (arg == "--uri" && i+1 < argc) G.uri = argv[++i];
    }
    if (socket_path.empty() || G.uri.empty())
    { fprintf(stderr,"Usage: %s --socket=PATH --uri URI\n", argv[0]); return 1; }

    fprintf(stderr, "[ffmpeg-player] v%s uri=%s\n", PLAYER_VERSION, G.uri.c_str());

    signal(SIGTERM, sig_handler);
    signal(SIGINT,  sig_handler);
    signal(SIGPIPE, SIG_IGN);

    /* Connect IPC socket to E2 */
    {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) { perror("socket"); return 1; }
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path)-1);
        int ok = 0;
        for (int i = 0; i < 30 && !ok; i++)
        {
            if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) ok = 1;
            else usleep(100000);
        }
        if (!ok) { fprintf(stderr,"[ffmpeg-player] IPC connect failed\n"); return 1; }
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        G.ipc_fd = fd;
    }
    fprintf(stderr, "[ffmpeg-player] IPC connected to %s\n", socket_path.c_str());

#if LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(58, 9, 100)
    av_register_all();
    avcodec_register_all();
#endif
    avformat_network_init();

    /* Open DVB sink — codec type NOT configured yet (codec unknown) */
    G.hw_sink_available = open_dvb_sink();

    /* Wait for 'play' command + optional config commands from E2 */
    {
        fd_set rset; struct timeval tv = { 10, 0 };
        FD_ZERO(&rset); FD_SET(G.ipc_fd, &rset);
        select(G.ipc_fd+1, &rset, NULL, NULL, &tv);
        poll_ipc();
    }

    /* Open input — AVCodecID now known for all streams */
    if (!open_input()) { close(G.ipc_fd); return 1; }

    /*
     * FIX 1 + 2 + 3: configure BCM decoders with the actual codec IDs.
     * This is the first safe moment to call VIDEO_SET_STREAMTYPE,
     * VIDEO_SET_CODEC_DATA and AUDIO_SET_ENCODING.
     */
    if (G.hw_sink_available)
    {
        configure_dvb_video_codec();
        if (G.audio_stream_idx >= 0)
            configure_dvb_audio_codec(
                G.fmt_ctx->streams[G.audio_stream_idx]->codecpar->codec_id);
    }

    G.running = true;
    playback_loop();

    stop_recording();
    close_dvb_sink();
    if (G.fmt_ctx) { avformat_close_input(&G.fmt_ctx); G.fmt_ctx = NULL; }
    avformat_network_deinit();
    if (G.ipc_fd >= 0) close(G.ipc_fd);
    fprintf(stderr, "[ffmpeg-player] exiting\n");
    return 0;
}
