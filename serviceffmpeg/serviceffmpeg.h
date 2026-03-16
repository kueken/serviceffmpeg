#ifndef __SERVICEFFMPEG_H
#define __SERVICEFFMPEG_H

/*
 * serviceffmpeg.h  —  Enigma2 media service plugin using exteplayer3 as backend
 *
 * Architecture:
 *   Enigma2  →  serviceffmpeg.so  →  fork/exec exteplayer3
 *
 *   IPC:
 *     stdin  pipe  (E2 → ep3):  text commands, one per line
 *     stderr pipe  (ep3 → E2):  JSON events, one per line
 *     terminate:  connect to /tmp/.exteplayerterm.socket
 *
 *   exteplayer3 source: github.com/e2iplayer/exteplayer3  (PLi OE-mirror)
 *   Binary installed to: /usr/bin/exteplayer3
 *
 * Replaces: servicemp3 (GStreamer), servicehisilicon (closed HAL)
 * License:  GPL-2.0
 */

#include <lib/base/ebase.h>
#include <lib/base/eerror.h>
#include <lib/base/object.h>
#include <lib/base/nconfig.h>
#include <lib/base/message.h>
#include <lib/dvb/epgcache.h>
#include <lib/dvb/pmt.h>
#include <lib/dvb/subtitle.h>
#include <lib/dvb/teletext.h>
#include <lib/service/iservice.h>
#include <lib/gui/esubtitle.h>

#include <string>
#include <vector>

/* -------------------------------------------------------------------------
 * Track info structures (filled from ep3 JSON: al / sl / vc / v_e)
 * ----------------------------------------------------------------------- */
struct sFfmpegAudioTrack {
    int         id;
    std::string encoding;   /* e.g. "AC3", "AAC", "MP3", "DTS", "EAC3" */
    std::string name;       /* language name or empty string */
};

struct sFfmpegSubtitleTrack {
    int         id;
    std::string encoding;   /* e.g. "SUBRIP", "ASS", "WEBVTT", "DVB", "PGS" */
    std::string name;
};

struct sFfmpegVideoInfo {
    std::string encoding;     /* e.g. "H264", "HEVC", "MPEG2VIDEO" */
    int         width         = 0;
    int         height        = 0;
    int         framerate     = 0; /* fps * 1000, from vc response */
    int         progressive   = 0;
    int         aspect_num    = 0; /* from vc (an field) */
    int         aspect_den    = 0; /* from vc (ad field) */
    int         aspect_ratio  = 0; /* raw from v_e (a field, ETSI/DVB value) */
};

/* -------------------------------------------------------------------------
 * eStaticServiceFfmpegInfo
 * ----------------------------------------------------------------------- */
class eStaticServiceFfmpegInfo : public iStaticServiceInformation
{
    DECLARE_REF(eStaticServiceFfmpegInfo);
public:
    eStaticServiceFfmpegInfo() {}
    RESULT getName(const eServiceReference &ref, std::string &name);
    int    getLength(const eServiceReference &ref);
    int    getInfo(const eServiceReference &ref, int w);
    long long getFileSize(const eServiceReference &ref);
    RESULT getEvent(const eServiceReference &ref, ePtr<eServiceEvent> &evt,
                    time_t start_time);
};

/* -------------------------------------------------------------------------
 * eServiceFfmpegInfoContainer
 * ----------------------------------------------------------------------- */
class eServiceFfmpegInfoContainer : public iServiceInfoContainer
{
    DECLARE_REF(eServiceFfmpegInfoContainer);
    double         m_double;
    unsigned char *m_buf;
    unsigned int   m_buf_size;
public:
    eServiceFfmpegInfoContainer();
    ~eServiceFfmpegInfoContainer();
    double         getDouble(unsigned int idx) const;
    unsigned char *getBuffer(unsigned int &size) const;
    void           setDouble(double v);
};

/* -------------------------------------------------------------------------
 * eServiceFactoryFfmpeg
 * ----------------------------------------------------------------------- */
class eServiceFactoryFfmpeg : public iServiceHandler
{
    DECLARE_REF(eServiceFactoryFfmpeg);
    ePtr<eStaticServiceFfmpegInfo> m_service_info;
public:
    eServiceFactoryFfmpeg();
    virtual ~eServiceFactoryFfmpeg();

    enum { id = 0x1001 };  /* same as servicemp3 — transparent replacement */

    RESULT play(const eServiceReference &, ePtr<iPlayableService> &);
    RESULT record(const eServiceReference &, ePtr<iRecordableService> &);
    RESULT list(const eServiceReference &, ePtr<iListableService> &);
    RESULT info(const eServiceReference &, ePtr<iStaticServiceInformation> &);
    RESULT offlineOperations(const eServiceReference &,
                             ePtr<iServiceOfflineOperations> &);
};

/* -------------------------------------------------------------------------
 * eServiceFfmpeg  —  main service class
 * ----------------------------------------------------------------------- */
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
    virtual ~eServiceFfmpeg();

    /* --- iPlayableService --- */
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
    /* scarthgap mandatory stubs */
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

    /* --- iSubtitleOutput --- */
    RESULT enableSubtitles(iSubtitleUser *user, SubtitleTrack &track);
    RESULT disableSubtitles();
    RESULT getCachedSubtitle(SubtitleTrack &track);
    RESULT getSubtitleList(std::vector<SubtitleTrack> &subtitle_list);

    /* --- iRecordableService (stubs) --- */
    RESULT connectEvent(const sigc::slot<void(iRecordableService*,int)> &event,
                        ePtr<eConnection> &connection);
    RESULT getError(int &error);
    RESULT subServices(ePtr<iSubserviceList> &ptr);
    RESULT prepare(const char *filename,
                   time_t begTime=-1, time_t endTime=-1, int eit_event_id=-1,
                   const char *name=0, const char *descr=0,
                   const char *tags=0, bool descramble=true,
                   bool recordecm=false, int packetsize=188);
    RESULT prepareStreaming(bool descramble=true, bool includeecm=false);
    RESULT start(bool simulate=false);
    RESULT record(ePtr<iStreamableService> &ptr);
    RESULT getFilenameExtension(std::string &ext);
    RESULT frontendInfo(ePtr<iFrontendInformation> &ptr);
    RESULT stopRecord();

private:
    enum eState { stIdle, stRunning, stPaused, stStopped, stError };
    eState              m_state;

    eServiceReference   m_ref;
    std::string         m_useragent;
    std::string         m_extra_headers;

    /* Track info — filled from ep3 JSON after PLAYBACK_PLAY */
    std::vector<sFfmpegAudioTrack>    m_audio_tracks;
    std::vector<sFfmpegSubtitleTrack> m_sub_tracks;
    sFfmpegVideoInfo                  m_video_info;
    int  m_current_audio_idx;   /* index into m_audio_tracks */
    int  m_current_sub_idx;     /* index into m_sub_tracks, -1 = none */
    int  m_cached_sub_idx;      /* last explicitly enabled sub track */

    /* Playback state */
    pts_t  m_last_position;     /* last known position in PTS units */
    pts_t  m_duration;          /* total duration in PTS units */
    bool   m_seekable;
    bool   m_is_live;
    int    m_error_code;
    int64_t m_last_poll_ms;     /* timestamp of last j\n poll (ms, CLOCK_MONOTONIC) */

    /* Subtitle output */
    iSubtitleUser  *m_subtitle_user;

    /* exteplayer3 process */
    pid_t  m_player_pid;
    int    m_stdin_fd;           /* write-end → ep3 stdin  */
    int    m_stderr_fd;          /* read-end  ← ep3 stderr */
    ePtr<eSocketNotifier> m_sn_read;
    std::string           m_read_buf;  /* partial-line accumulator */

    /* EPG */
    ePtr<eTimer>        m_nownext_timer;
    ePtr<eServiceEvent> m_event_now;
    ePtr<eServiceEvent> m_event_next;

    /* Enigma2 event signals */
    sigc::signal<void(iPlayableService*,int)>   m_event;
    sigc::signal<void(iRecordableService*,int)> m_rec_event;

    /* Helpers */
    bool        launchPlayer();
    void        stopPlayer();
    void        sendCmd(const char *cmd);
    std::string buildUri() const;

    void  onStderrData(int fd);
    void  processLine(const std::string &line);

    /* JSON event handlers */
    void  onEplayer3Extended(const std::string &line);
    void  onPlaybackOpen(const std::string &line);
    void  onPlaybackPlay(const std::string &line);
    void  onPlaybackStop(const std::string &line);
    void  onPlaybackPause(const std::string &line);
    void  onPlaybackContinue(const std::string &line);
    void  onPlaybackLength(const std::string &line);
    void  onPositionUpdate(const std::string &line);   /* J */
    void  onAudioList(const std::string &line);        /* a_l */
    void  onAudioCurrent(const std::string &line);     /* a_c */
    void  onSubtitleList(const std::string &line);     /* s_l */
    void  onSubtitleCurrent(const std::string &line);  /* s_c */
    void  onSubtitleData(const std::string &line);     /* s_a */
    void  onSubtitleFlush(const std::string &line);    /* s_f */
    void  onVideoInfoVc(const std::string &line);      /* v_c — spontaneous */
    void  onVideoInfoVe(const std::string &line);      /* v_e — spontaneous from decoder */
    void  onFfError(const std::string &line);          /* FF_ERROR */

    /* Track parsers */
    void  parseAudioList(const std::string &arr);
    void  parseSubtitleList(const std::string &arr);

    /* EPG */
    void  updateEpgCacheNowNext();

    /* Unit conversions */
    static pts_t   secToPts(int64_t sec) { return sec * 90000LL; }
    static int64_t ptsToSec(pts_t pts)   { return pts / 90000LL; }
    static pts_t   msToPts(int64_t ms)   { return ms  * 90LL;    }
};

#endif /* __SERVICEFFMPEG_H */
