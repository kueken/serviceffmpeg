/*
 * ffmpeg-player/main.cpp
 *
 * External player process for serviceffmpeg.
 * Connects to Enigma2 plugin via Unix domain socket.
 * Uses FFmpeg for all demux/decode/protocol operations.
 * Writes Elementary Streams to /dev/dvb kernel sink (HW decode)
 * or decodes in software and uses ALSA (fallback, mipsel et9200).
 *
 * Build requirements:
 *   libavformat, libavcodec, libavutil, libswresample, libass (optional)
 *
 * License: GPL v2
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

#include <string>
#include <vector>
#include <map>
#include <atomic>

/* ======================================================================
 * Configuration / compile-time options
 * ====================================================================== */
#define PLAYER_VERSION          "1.0.0"
#define DVB_AUDIO_DEVICE        "/dev/dvb/adapter0/audio0"
#define DVB_VIDEO_DEVICE        "/dev/dvb/adapter0/video0"
#define IPC_RECV_BUF_SIZE       8192
#define VIDEO_WRITE_BUF_SIZE    (512 * 1024)
#define AUDIO_WRITE_BUF_SIZE    (64  * 1024)
#define BUFFER_SIZE_DEFAULT     (4   * 1024 * 1024)
#define SEEK_THRESHOLD_MS       500
#define POSITION_REPORT_INTERVAL_MS  500

/* ======================================================================
 * Simple JSON helpers (matches serviceffmpeg.cpp)
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
    int     stream_idx;  /* AVStream index */
    int     pid;
    std::string lang;
    std::string codec;
    int     channels;
    int     samplerate;
    int     bitrate;
};

struct SubTrack {
    int     stream_idx;
    std::string lang;
    std::string codec;
    bool    bitmap;
};

/* ======================================================================
 * Player state
 * ====================================================================== */
struct PlayerState
{
    /* IPC */
    int             ipc_fd;
    std::string     recv_buf;

    /* Config */
    std::string     uri;
    std::string     useragent;
    std::string     extra_headers;
    int             buffer_size;

    /* FFmpeg context */
    AVFormatContext *fmt_ctx;
    int             video_stream_idx;
    int             audio_stream_idx;
    int             sub_stream_idx;

    /* Tracks */
    std::vector<AudioTrack> audio_tracks;
    std::vector<SubTrack>   sub_tracks;
    int             active_audio_track;
    int             active_sub_track;   /* -1 = disabled */

    /* Playback control */
    std::atomic<bool>   running;
    std::atomic<bool>   paused;
    std::atomic<bool>   stop_requested;
    std::atomic<int>    speed;          /* 0=normal, +n=FF, -n=REW */
    std::atomic<int64_t> seek_target_ms; /* -1 = no seek pending */
    pthread_mutex_t     seek_mutex;

    /* DVB output */
    int             dvb_video_fd;
    int             dvb_audio_fd;
    bool            hw_sink_available;

    /* Recording */
    AVFormatContext *record_ctx;
    std::string     record_path;
    bool            recording;
    pthread_mutex_t record_mutex;

    /* Position tracking */
    int64_t         duration_ms;
    int64_t         position_ms;
    int64_t         last_position_report_ms;
    bool            is_live;
    bool            seekable;

    /* libass */
#ifdef HAVE_LIBASS
    ASS_Library    *ass_lib;
    ASS_Renderer   *ass_renderer;
    ASS_Track      *ass_track;
#endif

    PlayerState()
        : ipc_fd(-1)
        , buffer_size(BUFFER_SIZE_DEFAULT)
        , fmt_ctx(NULL)
        , video_stream_idx(-1)
        , audio_stream_idx(-1)
        , sub_stream_idx(-1)
        , active_audio_track(-1)
        , active_sub_track(-1)
        , running(false)
        , paused(false)
        , stop_requested(false)
        , speed(0)
        , seek_target_ms(-1)
        , dvb_video_fd(-1)
        , dvb_audio_fd(-1)
        , hw_sink_available(false)
        , record_ctx(NULL)
        , recording(false)
        , duration_ms(0)
        , position_ms(0)
        , last_position_report_ms(0)
        , is_live(false)
        , seekable(false)
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
{
    ipc_send("error", jint("code",code) + "," + jstr("msg",msg));
}

/* ======================================================================
 * DVB hardware sink
 * Open /dev/dvb/adapter0/audio0 and video0
 * If unavailable (e.g. on mipsel et9200 without loaded driver),
 * fall back to software ALSA path
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

    /* Set bypass mode - we feed raw ES, hardware decoder does the rest */
    int bypass = 1;
    ioctl(G.dvb_audio_fd, AUDIO_SET_BYPASS_MODE, bypass);
    ioctl(G.dvb_video_fd, VIDEO_SET_STREAMTYPE, 0 /* auto-detect by driver */);

    ioctl(G.dvb_video_fd, VIDEO_PLAY);
    ioctl(G.dvb_audio_fd, AUDIO_PLAY);

    fprintf(stderr, "[ffmpeg-player] DVB HW sink opened\n");
    return true;
}

static void close_dvb_sink()
{
    if (G.dvb_video_fd >= 0)
    {
        ioctl(G.dvb_video_fd, VIDEO_STOP);
        close(G.dvb_video_fd);
        G.dvb_video_fd = -1;
    }
    if (G.dvb_audio_fd >= 0)
    {
        ioctl(G.dvb_audio_fd, AUDIO_STOP);
        close(G.dvb_audio_fd);
        G.dvb_audio_fd = -1;
    }
}

/* Write raw ES packet to DVB sink with PTS injection */
static bool write_dvb_video(const uint8_t *data, int size, int64_t pts_90khz)
{
    if (G.dvb_video_fd < 0) return false;
    /* Simple PES header for raw ES injection */
    uint8_t pes[14];
    pes[0] = 0x00; pes[1] = 0x00; pes[2] = 0x01; pes[3] = 0xE0; /* start code + stream id */
    int plen = size + 8; /* PES header size */
    pes[4] = (plen >> 8) & 0xFF;
    pes[5] = plen & 0xFF;
    pes[6] = 0x80;       /* marker bits */
    pes[7] = 0x80;       /* PTS flag */
    pes[8] = 0x05;       /* PTS length */
    /* Encode PTS into 5 bytes */
    uint64_t pts = (uint64_t)pts_90khz;
    pes[9]  = (uint8_t)(0x21 | ((pts >> 29) & 0x0E));
    pes[10] = (uint8_t)((pts >> 22) & 0xFF);
    pes[11] = (uint8_t)(0x01 | ((pts >> 14) & 0xFE));
    pes[12] = (uint8_t)((pts >> 7)  & 0xFF);
    pes[13] = (uint8_t)(0x01 | ((pts << 1)  & 0xFE));

    write(G.dvb_video_fd, pes, 14);
    write(G.dvb_video_fd, data, size);
    return true;
}

static bool write_dvb_audio(const uint8_t *data, int size, int64_t pts_90khz)
{
    if (G.dvb_audio_fd < 0) return false;
    uint8_t pes[14];
    pes[0] = 0x00; pes[1] = 0x00; pes[2] = 0x01; pes[3] = 0xC0;
    int plen = size + 8;
    pes[4] = (plen >> 8) & 0xFF;
    pes[5] = plen & 0xFF;
    pes[6] = 0x80;
    pes[7] = 0x80;
    pes[8] = 0x05;
    uint64_t pts = (uint64_t)pts_90khz;
    pes[9]  = (uint8_t)(0x21 | ((pts >> 29) & 0x0E));
    pes[10] = (uint8_t)((pts >> 22) & 0xFF);
    pes[11] = (uint8_t)(0x01 | ((pts >> 14) & 0xFE));
    pes[12] = (uint8_t)((pts >> 7)  & 0xFF);
    pes[13] = (uint8_t)(0x01 | ((pts << 1)  & 0xFE));

    write(G.dvb_audio_fd, pes, 14);
    write(G.dvb_audio_fd, data, size);
    return true;
}

/* ======================================================================
 * Build and send track list JSON to E2
 * ====================================================================== */
static void send_track_info()
{
    /* Build audio tracks JSON array */
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

    /* Build subtitle tracks JSON array */
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

    AVStream *vs = (G.video_stream_idx >= 0)
                   ? G.fmt_ctx->streams[G.video_stream_idx] : NULL;
    AVStream *as_ = (G.audio_stream_idx >= 0)
                    ? G.fmt_ctx->streams[G.audio_stream_idx] : NULL;

    std::string params;
    params += jbool("seekable", G.seekable) + ",";
    params += jbool("live", G.is_live) + ",";
    params += jint("dur_ms", G.duration_ms) + ",";
    params += jstr("container",
                   G.fmt_ctx->iformat ? G.fmt_ctx->iformat->name : "") + ",";
    /* Title from metadata */
    AVDictionaryEntry *title = av_dict_get(G.fmt_ctx->metadata, "title", NULL, 0);
    params += jstr("title", title ? title->value : "") + ",";

    if (vs && vs->codecpar)
    {
        params += jstr("vcodec",
                       avcodec_get_name(vs->codecpar->codec_id)) + ",";
        params += jint("w", vs->codecpar->width) + ",";
        params += jint("h", vs->codecpar->height) + ",";
        /* Aspect: map to ETSI values */
        int aspect = 2; /* default 16:9 */
        if (vs->codecpar->sample_aspect_ratio.num > 0 &&
            vs->codecpar->sample_aspect_ratio.den > 0)
        {
            float sar = (float)vs->codecpar->width
                      / vs->codecpar->height
                      * (float)vs->codecpar->sample_aspect_ratio.num
                      / vs->codecpar->sample_aspect_ratio.den;
            if (sar < 1.5f)       aspect = 1; /* 4:3 */
            else if (sar < 2.0f)  aspect = 2; /* 16:9 */
            else                  aspect = 3; /* 2.21:1 */
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
 * Open the input stream via FFmpeg
 * ====================================================================== */
static bool open_input()
{
    AVDictionary *opts = NULL;

    /* User-Agent for HTTP */
    if (!G.useragent.empty())
        av_dict_set(&opts, "user_agent", G.useragent.c_str(), 0);

    /* Parse extra headers (key:value\nkey:value) */
    if (!G.extra_headers.empty())
        av_dict_set(&opts, "headers", G.extra_headers.c_str(), 0);

    /* Buffer size for network streams */
    av_dict_set_int(&opts, "buffer_size", G.buffer_size, 0);

    /* Reconnect for HTTP streams on disconnect */
    av_dict_set(&opts, "reconnect", "1", 0);
    av_dict_set(&opts, "reconnect_streamed", "1", 0);
    av_dict_set(&opts, "reconnect_delay_max", "5", 0);

    /* Timeout 30s */
    av_dict_set(&opts, "timeout", "30000000", 0);  /* microseconds */
    av_dict_set(&opts, "rw_timeout", "30000000", 0);

    /* HLS-specific */
    av_dict_set(&opts, "hls_use_localtime", "1", 0);

    G.fmt_ctx = avformat_alloc_context();
    if (!G.fmt_ctx) { ipc_send_error(1, "avformat_alloc_context failed"); return false; }

    int ret = avformat_open_input(&G.fmt_ctx, G.uri.c_str(), NULL, &opts);
    av_dict_free(&opts);
    if (ret < 0)
    {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "[ffmpeg-player] avformat_open_input: %s\n", errbuf);
        ipc_send_error(1, "open failed: " + std::string(errbuf));
        avformat_free_context(G.fmt_ctx);
        G.fmt_ctx = NULL;
        return false;
    }

    ret = avformat_find_stream_info(G.fmt_ctx, NULL);
    if (ret < 0)
    {
        fprintf(stderr, "[ffmpeg-player] avformat_find_stream_info failed\n");
        /* Non-fatal for some formats, continue */
    }

    /* Duration */
    if (G.fmt_ctx->duration > 0)
        G.duration_ms = G.fmt_ctx->duration / 1000; /* AV_TIME_BASE = 1000000 */

    /* Live / seekable detection */
    G.is_live = (G.fmt_ctx->duration <= 0 ||
                 G.fmt_ctx->pb == NULL  ||
                 (G.fmt_ctx->ctx_flags & AVFMTCTX_NOHEADER));
    G.seekable = !G.is_live;
    if (G.fmt_ctx->pb && G.fmt_ctx->pb->seekable)
        G.seekable = true;

    /* Find best streams */
    G.video_stream_idx = av_find_best_stream(G.fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    G.audio_stream_idx = av_find_best_stream(G.fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);

    /* Enumerate all audio and subtitle tracks */
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
            t.stream_idx = i;
            t.pid        = st->id;
            t.lang       = lang_str;
            t.codec      = avcodec_get_name(st->codecpar->codec_id);
            t.channels   = st->codecpar->channels;
            t.samplerate = st->codecpar->sample_rate;
            t.bitrate    = (int)st->codecpar->bit_rate;
            G.audio_tracks.push_back(t);
            /* Set active audio to best stream */
            if ((int)i == G.audio_stream_idx)
                G.active_audio_track = (int)G.audio_tracks.size() - 1;
        }
        else if (st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE)
        {
            SubTrack t;
            t.stream_idx = i;
            t.lang       = lang_str;
            t.codec      = avcodec_get_name(st->codecpar->codec_id);
            t.bitmap     = (st->codecpar->codec_id == AV_CODEC_ID_DVD_SUBTITLE ||
                            st->codecpar->codec_id == AV_CODEC_ID_DVB_SUBTITLE ||
                            st->codecpar->codec_id == AV_CODEC_ID_HDMV_PGS_SUBTITLE);
            G.sub_tracks.push_back(t);
        }
    }

    /* Disable all streams initially, enable only selected ones */
    for (unsigned int i = 0; i < G.fmt_ctx->nb_streams; i++)
        G.fmt_ctx->streams[i]->discard = AVDISCARD_ALL;

    if (G.video_stream_idx >= 0)
        G.fmt_ctx->streams[G.video_stream_idx]->discard = AVDISCARD_DEFAULT;
    if (G.audio_stream_idx >= 0)
        G.fmt_ctx->streams[G.audio_stream_idx]->discard = AVDISCARD_DEFAULT;

    fprintf(stderr, "[ffmpeg-player] opened: %s\n"
                    "  video=%d audio=%d dur=%lldms live=%d seekable=%d\n"
                    "  audio_tracks=%zu sub_tracks=%zu\n",
            G.uri.c_str(),
            G.video_stream_idx, G.audio_stream_idx,
            (long long)G.duration_ms, (int)G.is_live, (int)G.seekable,
            G.audio_tracks.size(), G.sub_tracks.size());

    return true;
}

/* ======================================================================
 * Recording: mux demuxed packets to output file
 * Uses copy mode (no re-encoding), same as ts recording on STBs
 * ====================================================================== */
static bool start_recording(const std::string &path)
{
    pthread_mutex_lock(&G.record_mutex);

    if (G.recording || !G.fmt_ctx) { pthread_mutex_unlock(&G.record_mutex); return false; }

    int ret = avformat_alloc_output_context2(&G.record_ctx, NULL, "mpegts", path.c_str());
    if (ret < 0 || !G.record_ctx)
    {
        fprintf(stderr, "[ffmpeg-player] record: failed to alloc output context\n");
        pthread_mutex_unlock(&G.record_mutex);
        return false;
    }

    /* Add streams (video + audio) */
    for (unsigned int i = 0; i < G.fmt_ctx->nb_streams; i++)
    {
        AVStream *in_st = G.fmt_ctx->streams[i];
        if (in_st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
            in_st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
            continue;
        AVStream *out_st = avformat_new_stream(G.record_ctx, NULL);
        if (!out_st) continue;
        avcodec_parameters_copy(out_st->codecpar, in_st->codecpar);
        out_st->codecpar->codec_tag = 0;
        out_st->time_base = in_st->time_base;
    }

    if (!(G.record_ctx->oformat->flags & AVFMT_NOFILE))
    {
        ret = avio_open(&G.record_ctx->pb, path.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0)
        {
            fprintf(stderr, "[ffmpeg-player] record: avio_open failed\n");
            avformat_free_context(G.record_ctx);
            G.record_ctx = NULL;
            pthread_mutex_unlock(&G.record_mutex);
            return false;
        }
    }

    ret = avformat_write_header(G.record_ctx, NULL);
    if (ret < 0)
    {
        fprintf(stderr, "[ffmpeg-player] record: write_header failed\n");
        avio_closep(&G.record_ctx->pb);
        avformat_free_context(G.record_ctx);
        G.record_ctx = NULL;
        pthread_mutex_unlock(&G.record_mutex);
        return false;
    }

    G.record_path = path;
    G.recording   = true;
    fprintf(stderr, "[ffmpeg-player] recording to %s\n", path.c_str());
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
        G.record_ctx = NULL;
        G.recording  = false;
        fprintf(stderr, "[ffmpeg-player] recording stopped\n");
    }
    pthread_mutex_unlock(&G.record_mutex);
}

/* ======================================================================
 * Packet output: write to DVB sink or recording context
 * ====================================================================== */
static void process_packet(AVPacket *pkt)
{
    if (!pkt) return;

    int64_t pts_90khz = AV_NOPTS_VALUE;
    AVStream *st = G.fmt_ctx->streams[pkt->stream_index];

    if (pkt->pts != AV_NOPTS_VALUE)
        pts_90khz = av_rescale_q(pkt->pts, st->time_base, {1, 90000});

    /* Update position */
    if (pkt->pts != AV_NOPTS_VALUE)
    {
        int64_t pos_ms = av_rescale_q(pkt->pts, st->time_base, {1, 1000});
        if (pos_ms > 0) G.position_ms = pos_ms;
    }

    /* DVB HW sink */
    if (G.hw_sink_available)
    {
        if (pkt->stream_index == G.video_stream_idx && G.dvb_video_fd >= 0)
            write_dvb_video(pkt->data, pkt->size, pts_90khz);
        else if (pkt->stream_index == G.audio_stream_idx && G.dvb_audio_fd >= 0)
            write_dvb_audio(pkt->data, pkt->size, pts_90khz);
    }
    /* TODO: software decode path (ALSA) for mipsel without HW sink */

    /* Recording: copy packet to output */
    if (G.recording && G.record_ctx)
    {
        pthread_mutex_lock(&G.record_mutex);
        if (G.recording && G.record_ctx)
        {
            /* Find corresponding output stream */
            for (unsigned int oi = 0; oi < G.record_ctx->nb_streams; oi++)
            {
                if (G.record_ctx->streams[oi]->codecpar->codec_id ==
                    G.fmt_ctx->streams[pkt->stream_index]->codecpar->codec_id)
                {
                    AVPacket out_pkt;
                    av_packet_ref(&out_pkt, pkt);
                    out_pkt.stream_index = oi;
                    av_packet_rescale_ts(&out_pkt,
                                         G.fmt_ctx->streams[pkt->stream_index]->time_base,
                                         G.record_ctx->streams[oi]->time_base);
                    av_interleaved_write_frame(G.record_ctx, &out_pkt);
                    av_packet_unref(&out_pkt);
                    break;
                }
            }
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
        /* Find track by stream index */
        for (size_t i = 0; i < G.audio_tracks.size(); i++)
        {
            if (G.audio_tracks[i].stream_idx == id)
            {
                /* Disable old, enable new */
                if (G.audio_stream_idx >= 0)
                    G.fmt_ctx->streams[G.audio_stream_idx]->discard = AVDISCARD_ALL;
                G.audio_stream_idx  = id;
                G.active_audio_track = (int)i;
                G.fmt_ctx->streams[id]->discard = AVDISCARD_DEFAULT;
                fprintf(stderr, "[ffmpeg-player] audio track -> stream %d (%s)\n",
                        id, G.audio_tracks[i].lang.c_str());
                break;
            }
        }
    }
    else if (cmd == "set_subtitle")
    {
        int id = (int)json_get_int(payload, "id");
        if (id < 0)
        {
            /* Disable subtitles */
            if (G.sub_stream_idx >= 0)
                G.fmt_ctx->streams[G.sub_stream_idx]->discard = AVDISCARD_ALL;
            G.sub_stream_idx    = -1;
            G.active_sub_track = -1;
        }
        else
        {
            if (G.sub_stream_idx >= 0)
                G.fmt_ctx->streams[G.sub_stream_idx]->discard = AVDISCARD_ALL;
            G.sub_stream_idx = id;
            G.fmt_ctx->streams[id]->discard = AVDISCARD_DEFAULT;
            /* Find track index */
            for (size_t i = 0; i < G.sub_tracks.size(); i++)
                if (G.sub_tracks[i].stream_idx == id) { G.active_sub_track=(int)i; break; }
        }
    }
    else if (cmd == "set_ua")
    {
        G.useragent = json_get_str(payload, "ua");
    }
    else if (cmd == "set_headers")
    {
        G.extra_headers = json_get_str(payload, "headers");
    }
    else if (cmd == "record_start")
    {
        std::string path = json_get_str(payload, "path");
        start_recording(path);
    }
    else if (cmd == "record_stop")
    {
        stop_recording();
    }
    else if (cmd == "set_bufsize")
    {
        G.buffer_size = (int)json_get_int(payload, "bytes");
    }
    else if (cmd == "play")
    {
        /* Already handled in main loop setup, ignore duplicate */
    }
    else if (cmd == "get_info")
    {
        if (G.fmt_ctx) send_track_info();
    }
}

/* ======================================================================
 * IPC poll: read pending commands from E2 (non-blocking)
 * ====================================================================== */
static void poll_ipc()
{
    if (G.ipc_fd < 0) return;
    char buf[4096];
    ssize_t n = read(G.ipc_fd, buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = 0;
    G.recv_buf += buf;

    size_t pos;
    while ((pos = G.recv_buf.find('\n')) != std::string::npos)
    {
        std::string line = G.recv_buf.substr(0, pos);
        G.recv_buf       = G.recv_buf.substr(pos + 1);
        if (line.empty()) continue;
        std::string cmd = json_get_str(line, "cmd");
        handle_command(cmd, line);
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

    /* Also send video size event separately for E2 */
    if (G.video_stream_idx >= 0)
    {
        AVStream *vs = G.fmt_ctx->streams[G.video_stream_idx];
        int fps = 0;
        if (vs->avg_frame_rate.den > 0)
            fps = (int)(1000.0 * vs->avg_frame_rate.num / vs->avg_frame_rate.den);
        ipc_send("video_size",
                 jint("w", vs->codecpar->width) + "," +
                 jint("h", vs->codecpar->height) + "," +
                 jint("aspect", 2) + "," +
                 jint("fps", fps) + "," +
                 jbool("progressive", true));
    }

    int64_t last_position_ms = av_gettime_relative() / 1000;

    while (!G.stop_requested)
    {
        /* Handle seek request */
        pthread_mutex_lock(&G.seek_mutex);
        int64_t seek_target = G.seek_target_ms.load();
        if (seek_target >= 0) G.seek_target_ms = -1;
        pthread_mutex_unlock(&G.seek_mutex);

        if (seek_target >= 0)
        {
            int64_t seek_ts = seek_target * 1000; /* ms -> us for AV_TIME_BASE */
            int ret = av_seek_frame(G.fmt_ctx, -1,
                                    seek_ts,
                                    seek_target < G.position_ms
                                    ? AVSEEK_FLAG_BACKWARD : 0);
            if (ret < 0)
                fprintf(stderr, "[ffmpeg-player] seek to %lldms failed\n",
                        (long long)seek_target);
            else
                G.position_ms = seek_target;
        }

        /* Pause: poll IPC but don't read media */
        if (G.paused)
        {
            poll_ipc();
            usleep(20000);
            continue;
        }

        /* Poll IPC for commands */
        poll_ipc();

        /* Read next packet */
        int ret = av_read_frame(G.fmt_ctx, pkt);
        if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
        {
            if (ret == AVERROR_EOF)
            {
                fprintf(stderr, "[ffmpeg-player] EOF\n");
                ipc_send("eof");
                break;
            }
            usleep(5000);
            continue;
        }
        if (ret < 0)
        {
            char errbuf[256]; av_strerror(ret, errbuf, sizeof(errbuf));
            fprintf(stderr, "[ffmpeg-player] av_read_frame error: %s\n", errbuf);
            /* For network streams, try to reconnect */
            if (G.is_live)
            {
                ipc_send("buffering", jint("pct", 0));
                usleep(500000);
                /* Try seek to re-open */
                av_seek_frame(G.fmt_ctx, -1, 0, AVSEEK_FLAG_BACKWARD);
                ipc_send("buffer_done");
                continue;
            }
            ipc_send_error(4, errbuf);
            break;
        }

        /* Only process selected streams */
        if (pkt->stream_index != G.video_stream_idx &&
            pkt->stream_index != G.audio_stream_idx &&
            pkt->stream_index != G.sub_stream_idx)
        {
            av_packet_unref(pkt);
            continue;
        }

        process_packet(pkt);
        av_packet_unref(pkt);

        /* Report position periodically */
        int64_t now_ms = av_gettime_relative() / 1000;
        if (now_ms - last_position_ms >= POSITION_REPORT_INTERVAL_MS)
        {
            last_position_ms = now_ms;
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
    fprintf(stderr, "[ffmpeg-player] signal %d received\n", sig);
    G.stop_requested = true;
}

/* ======================================================================
 * main()
 * ====================================================================== */
int main(int argc, char *argv[])
{
    std::string socket_path;

    /* Parse arguments */
    for (int i = 1; i < argc; i++)
    {
        std::string arg = argv[i];
        if (arg.substr(0, 9) == "--socket=")
            socket_path = arg.substr(9);
        else if (arg == "--uri" && i + 1 < argc)
            G.uri = argv[++i];
    }

    if (socket_path.empty() || G.uri.empty())
    {
        fprintf(stderr, "Usage: %s --socket=PATH --uri URI\n", argv[0]);
        return 1;
    }

    fprintf(stderr, "[ffmpeg-player] v%s starting, uri=%s\n",
            PLAYER_VERSION, G.uri.c_str());

    /* Signal handling */
    signal(SIGTERM, sig_handler);
    signal(SIGINT,  sig_handler);
    signal(SIGPIPE, SIG_IGN);

    /* Connect to E2 plugin via Unix socket */
    {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) { perror("socket"); return 1; }

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

        /* Socket is created by E2 before fork, wait up to 3s */
        int connected = 0;
        for (int i = 0; i < 30 && !connected; i++)
        {
            if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0)
                connected = 1;
            else
                usleep(100000);
        }
        if (!connected) { fprintf(stderr, "[ffmpeg-player] IPC connect failed\n"); return 1; }

        /* Non-blocking for poll_ipc() */
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        G.ipc_fd = fd;
    }
    fprintf(stderr, "[ffmpeg-player] IPC connected to %s\n", socket_path.c_str());

    /* Initialize FFmpeg */
#if LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(58, 9, 100)
    av_register_all();
    avcodec_register_all();
#endif
    avformat_network_init();

    /* Try to open DVB HW sink */
    G.hw_sink_available = open_dvb_sink();

    /* Wait for 'play' command from E2 before opening input */
    {
        fd_set rset;
        struct timeval tv = { 10, 0 };
        FD_ZERO(&rset); FD_SET(G.ipc_fd, &rset);
        select(G.ipc_fd + 1, &rset, NULL, NULL, &tv);
        poll_ipc();  /* Process 'play' + any config commands */
    }

    /* Open input */
    if (!open_input())
    {
        close(G.ipc_fd);
        return 1;
    }

    /* Start playback */
    G.running = true;
    playback_loop();

    /* Cleanup */
    stop_recording();
    close_dvb_sink();

    if (G.fmt_ctx)
    {
        avformat_close_input(&G.fmt_ctx);
        G.fmt_ctx = NULL;
    }

    avformat_network_deinit();

    if (G.ipc_fd >= 0) close(G.ipc_fd);

    fprintf(stderr, "[ffmpeg-player] exiting\n");
    return 0;
}
