#ifndef __SERVICEFFMPEG_H
#define __SERVICEFFMPEG_H

/*
 * serviceffmpeg.h
 *
 * Enigma2 media service plugin - pure FFmpeg backend
 * External player process architecture (no GStreamer dependency)
 *
 * Fixed for scarthgap / OpenPLi 9.x:
 *   - sigc++-3.0: slot2<> -> slot<void(T*,int)>, Signal2 -> Signal<void(T*,int)>
 *   - iPlayableService / iRecordableService: new virtual interface methods
 *   - iAudioTrackInfo: m_language (not m_lang), no m_type member
 *   - eWidget: no destroy()/setPage() - use eSubtitleWidget
 *   - iServiceInformation: no sAudioCodec/sVideoCodec constants in this version
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
#define SFMP_SOCKET_PATH        "/tmp/sfmp-%d.sock"
#define SFMP_PROTO_VERSION      1

#define SFMP_CMD_PLAY           "play"
#define SFMP_CMD_STOP           "stop"
#define SFMP_CMD_PAUSE          "pause"
#define SFMP_CMD_RESUME         "resume"
#define SFMP_CMD_SEEK           "seek"
#define SFMP_CMD_SEEK_RELATIVE  "seek_rel"
#define SFMP_CMD_SET_AUDIO      "set_audio"
#define SFMP_CMD_SET_SUBTITLE   "set_subtitle"
#define SFMP_CMD_SET_SPEED      "set_speed"
#define SFMP_CMD_GET_INFO       "get_info"
#define SFMP_CMD_SET_USERAGENT  "set_ua"
#define SFMP_CMD_SET_HEADERS    "set_headers"
#define SFMP_CMD_RECORD_START   "record_start"
#define SFMP_CMD_RECORD_STOP    "record_stop"
#define SFMP_CMD_BUFFERSIZE     "set_bufsize"

#define SFMP_EVT_STARTED        "started"
#define SFMP_EVT_EOF            "eof"
#define SFMP_EVT_SOF            "sof"
#define SFMP_EVT_ERROR          "error"
#define SFMP_EVT_INFO           "info"
#define SFMP_EVT_POSITION       "position"
#define SFMP_EVT_BUFFERING      "buffering"
#define SFMP_EVT_BUFFER_DONE    "buffer_done"
#define SFMP_EVT_AUDIO_TRACKS   "audio_tracks"
#define SFMP_EVT_SUB_TRACKS     "sub_tracks"
#define SFMP_EVT_VIDEO_SIZE     "video_size"
#define SFMP_EVT_SUBTITLE_PAGE  "subtitle"
#define SFMP_EVT_SUBTITLE_CLEAR "subtitle_clear"
#define SFMP_EVT_RECORD_INFO    "record_info"

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
    int         id;
    int         pid;
    std::string language;
    std::string codec;
    int         channels;
    int         samplerate;
    int         bitrate;
};

struct sFfmpegSubtitleTrack
{
    int         id;
    std::string language;
    std::string codec;
    bool        bitmap;
};

struct sFfmpegVideoInfo
{
    std::string codec;
    int         width;
    int         height;
    int         aspect;
    int         framerate;
    int         bitrate;
    bool        progressive;
};

struct sFfmpegStreamInfo
{
    sFfmpegVideoInfo                   video;
    std::vector<sFfmpegAudioTrack>     audio_tracks;
    std::vector<sFfmpegSubtitleTrack>  sub_tracks;
    int64_t                            duration_ms;
    bool                               seekable;
    bool                               is_live;
    std::string                        title;
    std::string                        container;
};

/* ======================================================================
 * eStaticServiceFfmpegInfo
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
 * ====================================================================== */
class eServiceFactoryFfmpeg
    : public iServiceHandler
{
    DECLARE_REF(eServiceFactoryFfmpeg);
    ePtr<eStaticServiceFfmpegInfo> m_service_info;
public:
    eServiceFactoryFfmpeg();
    ~eServiceFactoryFfmpeg();

    enum { id = 0x1002 };

    RESULT play(const eServiceReference &, ePtr<iPlayableService> &);
    RESULT record(const eServiceReference &, ePtr<iRecordableService> &);
    RESULT list(const eServiceReference &, ePtr<iListableService> &);
    RESULT info(const eServiceReference &, ePtr<iStaticServiceInformation> &);
    RESULT offlineOperations(const eServiceReference &, ePtr<iServiceOfflineOperations> &);
};

/* ======================================================================
 * eServiceFfmpeg
 *
 * sigc++-3.0 migration:
 *   OLD: sigc::slot2<void, T*, int>              -> NEW: sigc::slot<void(T*,int)>
 *   OLD: Signal2<void, T*, int>  (Enigma2 macro) -> NEW: Signal<void(T*,int)>
 *   OLD: connectEvent signature uses slot2<>     -> NEW: uses slot<void(T*,int)>
 *
 * iPlayableService / iRecordableService new mandatory virtuals (scarthgap):
 *   setTarget, audioChannel, timeshift, tap, cueSheet, audioDelay,
 *   rdsDecoder, stream, streamed, keys, setQpipMode (iPlayableService)
 *   prepare, prepareStreaming, start(bool), stream, getFilenameExtension (iRecordableService)
 *
 * iSubtitleOutput (scarthgap):
 *   enableSubtitles(iSubtitleUser*, SubtitleTrack&)
 *   disableSubtitles()   -- no parameter
 *
 * ====================================================================== */
class eServiceFfmpeg
    : public iPlayableService
    , public iServiceInformation
    , public iPauseableService
    , public iSeekableService
    , public iAudioTrackSelection
    , public iSubtitleOutput
    , public iRecordableService
{
    DECLARE_REF(eServiceFfmpeg);

public:
    eServiceFfmpeg(eServiceReference ref);
    ~eServiceFfmpeg();

    /* --- iPlayableService --- */
    /* sigc++-3.0: slot<void(T*,int)> */
    RESULT connectEvent(const sigc::slot<void(iPlayableService*,int)> &event,
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
    /* new mandatory virtuals in scarthgap iPlayableService */
    RESULT setTarget(int target, bool noaudio = false);
    RESULT audioChannel(ePtr<iAudioChannelSelection> &ptr);
    RESULT timeshift(ePtr<iTimeshiftService> &ptr);
    RESULT tap(ePtr<iTapService> &ptr);
    RESULT cueSheet(ePtr<iCueSheet> &ptr);
    RESULT audioDelay(ePtr<iAudioDelay> &ptr);
    RESULT rdsDecoder(ePtr<iRdsDecoder> &ptr);
    RESULT stream(ePtr<iStreamableService> &ptr);
    RESULT streamed(ePtr<iStreamedService> &ptr);
    RESULT keys(ePtr<iServiceKeys> &ptr);
    void   setQpipMode(bool value, bool audio);

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

    /* --- iSubtitleOutput (scarthgap signature) --- */
    RESULT enableSubtitles(iSubtitleUser *parent, SubtitleTrack &track);
    RESULT disableSubtitles();
    RESULT getCachedSubtitle(SubtitleTrack &track);
    RESULT getSubtitleList(std::vector<SubtitleTrack> &subtitle_list);

    /* --- iRecordableService --- */
    /* sigc++-3.0 slot */
    RESULT connectEvent(const sigc::slot<void(iRecordableService*,int)> &event,
                        ePtr<eConnection> &connection);
    RESULT getError(int &error);
    RESULT subServices(ePtr<iSubserviceList> &ptr);
    /* new mandatory virtuals in scarthgap iRecordableService */
    RESULT prepare(const char *filename,
                   time_t begTime=-1, time_t endTime=-1, int eit_event_id=-1,
                   const char *name=0, const char *descr=0, const char *tags=0,
                   bool descramble=true, bool recordecm=false, int packetsize=188);
    RESULT prepareStreaming(bool descramble=true, bool includeecm=false);
    RESULT start(bool simulate=false);
    RESULT record(ePtr<iStreamableService> &ptr);
    RESULT getFilenameExtension(std::string &ext);
    RESULT frontendInfo(ePtr<iFrontendInformation> &ptr);
    RESULT stopRecord();

private:
    enum eState { stIdle, stRunning, stPaused, stStopped, stError };
    eState                      m_state;

    eServiceReference           m_ref;
    std::string                 m_useragent;
    std::string                 m_extra_headers;

    sFfmpegStreamInfo           m_stream_info;
    int                         m_current_audio_track;
    int                         m_current_sub_track;
    int                         m_cached_sub_track;

    bool                        m_paused;
    bool                        m_buffering;
    int                         m_buffer_percentage;
    pts_t                       m_last_position;
    pts_t                       m_duration;
    bool                        m_seekable;
    bool                        m_is_live;
    int                         m_trickmode_speed;

    int                         m_width, m_height, m_aspect, m_framerate;
    bool                        m_progressive;

    /* Subtitle: use iSubtitleUser* (scarthgap), no eWidget */
    iSubtitleUser              *m_subtitle_user;

    int                         m_error_code;
    std::string                 m_error_message;

    bool                        m_recording;
    std::string                 m_record_path;

    /* sigc++-3.0: Signal<void(T*,int)> */
    sigc::signal<void(iRecordableService*,int)>  m_rec_event;
    sigc::signal<void(iPlayableService*,int)>    m_event;

    ePtr<eTimer>                m_nownext_timer;
    ePtr<eServiceEvent>         m_event_now;
    ePtr<eServiceEvent>         m_event_next;

    pid_t                       m_player_pid;
    int                         m_ipc_fd;
    std::string                 m_socket_path;
    ePtr<eSocketNotifier>       m_sn_ipc;
    std::string                 m_ipc_recv_buf;

    bool    launchPlayer();
    void    killPlayer();
    bool    ipcConnect();
    bool    ipcSend(const std::string &cmd, const std::string &params = "");
    void    ipcReceive(int fd);
    void    handleEvent(const std::string &evt, const std::string &payload);
    void    updateEpgCacheNowNext();

    std::string buildUri() const;
    void        parseTrackList(const std::string &json);
    void        parseVideoInfo(const std::string &json);
    void        parseSubtitle(const std::string &json);
    pts_t       msToPts(int64_t ms) const  { return ms * 90;  }
    int64_t     ptsToMs(pts_t pts) const   { return pts / 90; }
};

#endif /* __SERVICEFFMPEG_H */
