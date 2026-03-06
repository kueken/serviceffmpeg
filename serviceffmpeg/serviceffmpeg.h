#ifndef __SERVICEFFMPEG_H
#define __SERVICEFFMPEG_H

/*
 * serviceffmpeg.h
 *
 * Enigma2 media service plugin - pure FFmpeg backend
 * External player process architecture (no GStreamer dependency)
 *
 * Architecture:
 *   serviceffmpeg.so  (this plugin, in-process Enigma2 glue)
 *       |
 *       | Unix Domain Socket (JSON protocol)
 *       |
 *   ffmpeg-player  (external process, FFmpeg demux/decode/sink)
 *       |
 *       +-- /dev/dvb/adapter0/audio0  (ES inject, HW decoder)
 *       +-- /dev/dvb/adapter0/video0  (ES inject, HW decoder)
 *       +-- ALSA fallback             (software decode, mipsel)
 *       +-- AVFormatContext output    (stream recording)
 *
 * Target: Enigma2 / OpenPLi / OpenATV / OpenVIX
 * Hardware: mipsel (Broadcom BCM7xxx), ARM (Hisilicon, AML)
 *           - tested on Xtrend et9200 (mipsel/BCM)
 *
 * Replaces: servicemp3 (GStreamer) and servicehisilicon (closed HAL)
 *
 * Authors: Based on concepts from:
 *   - libeplayer3 (crow, schischu, hellmaster1024, konfetti, Taapat)
 *   - servicemp3 (OpenPLi)
 *   - libstb-hal-ddt (Duckbox-Developers)
 *
 * License: GPL v2
 */

#include <lib/base/ebase.h>
#include <lib/base/eerror.h>
#include <lib/base/thread.h>
#include <lib/base/object.h>
#include <lib/base/nconfig.h>
#include <lib/dvb/epgcache.h>
#include <lib/service/iservice.h>
#include <lib/gui/esubtitle.h>

#include <string>
#include <list>
#include <map>
#include <vector>

/* ======================================================================
 * IPC Protocol constants (serviceffmpeg <-> ffmpeg-player process)
 * ====================================================================== */
#define SFMP_SOCKET_PATH        "/tmp/sfmp-%d.sock"  /* %d = pid */
#define SFMP_PROTO_VERSION      1

/* Commands (E2 -> Player) */
#define SFMP_CMD_PLAY           "play"
#define SFMP_CMD_STOP           "stop"
#define SFMP_CMD_PAUSE          "pause"
#define SFMP_CMD_RESUME         "resume"
#define SFMP_CMD_SEEK           "seek"           /* pos_ms */
#define SFMP_CMD_SEEK_RELATIVE  "seek_rel"       /* delta_ms */
#define SFMP_CMD_SET_AUDIO      "set_audio"      /* stream_id */
#define SFMP_CMD_SET_SUBTITLE   "set_subtitle"   /* stream_id, -1=off */
#define SFMP_CMD_SET_SPEED      "set_speed"      /* numerator (1=normal, 2=2x, -1=rev) */
#define SFMP_CMD_GET_INFO       "get_info"
#define SFMP_CMD_SET_USERAGENT  "set_ua"
#define SFMP_CMD_SET_HEADERS    "set_headers"    /* key:val\nkey:val */
#define SFMP_CMD_RECORD_START   "record_start"   /* output_path */
#define SFMP_CMD_RECORD_STOP    "record_stop"
#define SFMP_CMD_BUFFERSIZE     "set_bufsize"    /* bytes */

/* Events (Player -> E2) */
#define SFMP_EVT_STARTED        "started"
#define SFMP_EVT_EOF            "eof"
#define SFMP_EVT_SOF            "sof"             /* reached start while rewinding */
#define SFMP_EVT_ERROR          "error"           /* + error_code + message */
#define SFMP_EVT_INFO           "info"            /* stream info JSON */
#define SFMP_EVT_POSITION       "position"        /* pos_ms, duration_ms */
#define SFMP_EVT_BUFFERING      "buffering"       /* percentage 0..100 */
#define SFMP_EVT_BUFFER_DONE    "buffer_done"
#define SFMP_EVT_AUDIO_TRACKS   "audio_tracks"    /* JSON array */
#define SFMP_EVT_SUB_TRACKS     "sub_tracks"      /* JSON array */
#define SFMP_EVT_VIDEO_SIZE     "video_size"      /* width, height, aspect, fps */
#define SFMP_EVT_SUBTITLE_PAGE  "subtitle"        /* text, pts_ms, duration_ms */
#define SFMP_EVT_SUBTITLE_CLEAR "subtitle_clear"
#define SFMP_EVT_RECORD_INFO    "record_info"     /* bytes_written */

/* Error codes */
#define SFMP_ERR_OPEN_FAILED    1
#define SFMP_ERR_CODEC_FAILED   2
#define SFMP_ERR_NETWORK        3
#define SFMP_ERR_UNSUPPORTED    4
#define SFMP_ERR_TIMEOUT        5
#define SFMP_ERR_PERMISSION     6

/* ======================================================================
 * Track info structures
 * ====================================================================== */
struct sFfmpegAudioTrack
{
    int     id;              /* stream index in container */
    int     pid;             /* for DVB compatibility */
    std::string language;    /* ISO 639 e.g. "deu" */
    std::string codec;       /* "ac3", "aac", "mp3", "eac3", "dts", "opus", ... */
    int     channels;
    int     samplerate;
    int     bitrate;
};

struct sFfmpegSubtitleTrack
{
    int     id;
    std::string language;
    std::string codec;       /* "srt", "ass", "dvd_sub", "dvb_sub", "pgssub", "webvtt" */
    bool    bitmap;          /* true=bitmap sub (dvb/pgs), false=text */
};

struct sFfmpegVideoInfo
{
    std::string codec;       /* "h264", "hevc", "mpeg2video", "vp9", ... */
    int     width;
    int     height;
    int     aspect;          /* ETSI: 1=4:3, 2=16:9, 3=2.21:1 */
    int     framerate;       /* fps * 1000 */
    int     bitrate;
    bool    progressive;
};

struct sFfmpegStreamInfo
{
    sFfmpegVideoInfo            video;
    std::vector<sFfmpegAudioTrack>   audio_tracks;
    std::vector<sFfmpegSubtitleTrack> sub_tracks;
    int64_t duration_ms;
    bool    seekable;
    bool    is_live;
    std::string title;
    std::string container;   /* "matroska", "mov,mp4", "hls", "mpegts", ... */
};

/* ======================================================================
 * eStaticServiceFfmpegInfo
 * Static info (filename, size, mtime) without opening the file
 * ====================================================================== */
class eStaticServiceFfmpegInfo
    : public iStaticServiceInformation
{
    DECLARE_REF(eStaticServiceFfmpegInfo);
public:
    eStaticServiceFfmpegInfo();
    RESULT getName(const eServiceReference &ref, std::string &name);
    int    getLength(const eServiceReference &ref);
    int    getInfo(const eServiceReference &ref, int w);
    long long getFileSize(const eServiceReference &ref);
    RESULT getEvent(const eServiceReference &ref, ePtr<eServiceEvent> &evt, time_t start_time);
};

/* ======================================================================
 * eServiceFfmpegInfoContainer
 * ====================================================================== */
class eServiceFfmpegInfoContainer
    : public iServiceInfoContainer
{
    DECLARE_REF(eServiceFfmpegInfoContainer);
    double          m_double;
    unsigned char  *m_buf;
    unsigned int    m_buf_size;
public:
    eServiceFfmpegInfoContainer();
    ~eServiceFfmpegInfoContainer();
    double        getDouble(unsigned int idx) const;
    unsigned char *getBuffer(unsigned int &size) const;
    void setDouble(double v);
};

/* ======================================================================
 * eServiceFactoryFfmpeg
 * Registers serviceffmpeg as handler for all media extensions
 * Replaces servicemp3 (0x1001) as default media handler
 * ====================================================================== */
class eServiceFactoryFfmpeg
    : public iServiceHandler
{
    DECLARE_REF(eServiceFactoryFfmpeg);
    ePtr<eStaticServiceFfmpegInfo> m_service_info;
public:
    eServiceFactoryFfmpeg();
    ~eServiceFactoryFfmpeg();

    enum { id = 0x1002 };   /* unique service type ID, above servicemp3=0x1001 */

    RESULT play(const eServiceReference &, ePtr<iPlayableService> &);
    RESULT record(const eServiceReference &, ePtr<iRecordableService> &);
    RESULT list(const eServiceReference &, ePtr<iListableService> &);
    RESULT info(const eServiceReference &, ePtr<iStaticServiceInformation> &);
    RESULT offlineOperations(const eServiceReference &, ePtr<iServiceOfflineOperations> &);
};

/* ======================================================================
 * eServiceFfmpeg
 * The main service implementation - all playback interfaces
 * ====================================================================== */
class eServiceFfmpeg
    : public iPlayableService
    , public iServiceInformation
    , public iPauseableService
    , public iSeekableService
    , public iAudioTrackSelection
    , public iSubtitleOutput
    , public iRecordableService
    , public Object
{
    DECLARE_REF(eServiceFfmpeg);

public:
    eServiceFfmpeg(eServiceReference ref);
    ~eServiceFfmpeg();

    /* --- iPlayableService --- */
    RESULT connectEvent(const sigc::slot2<void, iPlayableService*, int> &event,
                        ePtr<eConnection> &connection);
    RESULT start();
    RESULT stop();
    RESULT pause(ePtr<iPauseableService> &ptr);
    RESULT seek(ePtr<iSeekableService> &ptr);
    RESULT setSlowMotion(int ratio);
    RESULT setFastForward(int ratio);
    RESULT audioTracks(ePtr<iAudioTrackSelection> &ptr);
    RESULT subtitle(ePtr<iSubtitleOutput> &ptr);
    RESULT info(ePtr<iServiceInformation> &ptr);

    /* --- iServiceInformation --- */
    RESULT getName(std::string &name);
    int    getInfo(int w);
    std::string getInfoString(int w);
    ePtr<iServiceInfoContainer> getInfoObject(int w);

    /* --- iPauseableService --- */
    RESULT pause();
    RESULT unpause();

    /* --- iSeekableService --- */
    RESULT getLength(pts_t &len);
    RESULT seekTo(pts_t to);
    RESULT seekRelative(int direction, pts_t to);
    RESULT getPlayPosition(pts_t &pos);
    RESULT setTrickmode(int trick);
    RESULT isCurrentlySeekable();

    /* --- iAudioTrackSelection --- */
    int    getNumberOfTracks();
    RESULT selectTrack(unsigned int i);
    RESULT getTrackInfo(struct iAudioTrackInfo &, unsigned int n);
    int    getCurrentTrack();

    /* --- iSubtitleOutput --- */
    RESULT enableSubtitles(eWidget *parent, SubtitleTrack &track);
    RESULT disableSubtitles(eWidget *parent);
    RESULT getCachedSubtitle(SubtitleTrack &track);
    RESULT getSubtitleList(std::vector<SubtitleTrack> &subtitle_list);

    /* --- iRecordableService --- */
    RESULT record();
    RESULT stopRecord();
    RESULT frontendInfo(ePtr<iFrontendInformation> &ptr);
    RESULT connectEvent(const sigc::slot2<void, iRecordableService*, int> &event,
                        ePtr<eConnection> &connection);
    RESULT getError(int &error);
    RESULT subServices(ePtr<iSubserviceList> &ptr);

private:
    /* --- State machine --- */
    enum eState { stIdle, stRunning, stPaused, stStopped, stError };
    eState                      m_state;

    /* --- Service reference & metadata --- */
    eServiceReference           m_ref;
    std::string                 m_useragent;
    std::string                 m_extra_headers;

    /* --- Stream info (filled once player sends SFMP_EVT_INFO) --- */
    sFfmpegStreamInfo           m_stream_info;
    int                         m_current_audio_track;
    int                         m_current_sub_track;   /* -1 = disabled */
    int                         m_cached_sub_track;    /* for getCachedSubtitle */

    /* --- Playback state --- */
    bool                        m_paused;
    bool                        m_buffering;
    int                         m_buffer_percentage;
    pts_t                       m_last_position;
    pts_t                       m_duration;
    bool                        m_seekable;
    bool                        m_is_live;
    int                         m_trickmode_speed;     /* 0=normal, +n=FF, -n=REW */

    /* --- Video info --- */
    int                         m_width, m_height, m_aspect, m_framerate;
    bool                        m_progressive;

    /* --- Subtitle widget --- */
    eWidget                    *m_subtitle_widget;

    /* --- Error info --- */
    int                         m_error_code;
    std::string                 m_error_message;

    /* --- Recording --- */
    bool                        m_recording;
    std::string                 m_record_path;
    Signal2<void, iRecordableService*, int> m_rec_event;

    /* --- Enigma2 event signal --- */
    Signal2<void, iPlayableService*, int>   m_event;

    /* --- EPG/NowNext timer --- */
    ePtr<eTimer>                m_nownext_timer;
    ePtr<eServiceEvent>         m_event_now;
    ePtr<eServiceEvent>         m_event_next;

    /* --- IPC to external player process --- */
    pid_t                       m_player_pid;
    int                         m_ipc_fd;            /* connected Unix socket */
    std::string                 m_socket_path;
    ePtr<eSocketNotifier>       m_sn_ipc;
    std::string                 m_ipc_recv_buf;      /* line-oriented recv buffer */

    /* --- Internal methods --- */
    bool    launchPlayer();
    void    killPlayer();
    bool    ipcConnect();
    bool    ipcSend(const std::string &cmd, const std::string &params = "");
    void    ipcReceive(int fd);                      /* called by socket notifier */
    void    handleEvent(const std::string &evt, const std::string &payload);
    void    updateEpgCacheNowNext();

    std::string buildUri() const;
    void        parseTrackList(const std::string &json);
    void        parseVideoInfo(const std::string &json);
    void        parseSubtitle(const std::string &json);
    pts_t       msToPts(int64_t ms) const  { return ms * 90;   }
    int64_t     ptsToMs(pts_t pts) const   { return pts / 90;  }
};

#endif /* __SERVICEFFMPEG_H */
