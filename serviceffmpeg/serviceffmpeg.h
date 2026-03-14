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

    virtual RESULT play(const eServiceReference &, ePtr<iPlayableService> &);
    virtual RESULT record(const eServiceReference &, ePtr<iRecordableService> &);
    virtual RESULT list(const eServiceReference &, ePtr<iListableService> &);
    virtual RESULT info(const eServiceReference &, ePtr<iStaticServiceInformation> &);
    virtual RESULT offlineOperations(const eServiceReference &,
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
    virtual RESULT connectEvent(const sigc::slot<void(iPlayableService*,int)> &event,
                        ePtr<eConnection> &connection);
    virtual RESULT start();
    virtual RESULT stop();
    virtual RESULT pause(ePtr<iPauseableService> &ptr);
    virtual RESULT seek(ePtr<iSeekableService> &ptr);
    virtual RESULT setSlowMotion(int ratio);
    virtual RESULT setFastForward(int ratio);
    virtual RESULT audioTracks(ePtr<iAudioTrackSelection> &ptr);
    virtual RESULT subtitle(ePtr<iSubtitleOutput> &ptr);
    virtual RESULT info(ePtr<iServiceInformation> &ptr);
    /* scarthgap mandatory stubs */
    virtual RESULT setTarget(int target, bool noaudio = false);
    virtual RESULT audioChannel(ePtr<iAudioChannelSelection> &ptr);
    virtual RESULT timeshift(ePtr<iTimeshiftService> &ptr);
    virtual RESULT tap(ePtr<iTapService> &ptr);
    virtual RESULT cueSheet(ePtr<iCueSheet> &ptr);
    virtual RESULT audioDelay(ePtr<iAudioDelay> &ptr);
    virtual RESULT rdsDecoder(ePtr<iRdsDecoder> &ptr);
    virtual RESULT stream(ePtr<iStreamableService> &ptr);
    virtual RESULT streamed(ePtr<iStreamedService> &ptr);
    virtual RESULT keys(ePtr<iServiceKeys> &ptr);
    virtual void   setQpipMode(bool value, bool audio);

    /* --- iServiceInformation --- */
    virtual RESULT getName(std::string &name);
    virtual int    getInfo(int w);
    virtual std::string getInfoString(int w);
    virtual ePtr<iServiceInfoContainer> getInfoObject(int w);

    /* --- iPauseableService --- */
    virtual RESULT pause();
    virtual RESULT unpause();

    /* --- iSeekableService --- */
    virtual RESULT getLength(pts_t &len);
    virtual RESULT seekTo(pts_t to);
    virtual RESULT seekRelative(int direction, pts_t to);
    virtual RESULT getPlayPosition(pts_t &pos);
    virtual RESULT setTrickmode(int trick);
    virtual RESULT isCurrentlySeekable();

    /* --- iAudioTrackSelection --- */
    virtual int    getNumberOfTracks();
    virtual RESULT selectTrack(unsigned int i);
    virtual RESULT getTrackInfo(struct iAudioTrackInfo &, unsigned int n);
    virtual int    getCurrentTrack();

    /* --- iSubtitleOutput --- */
    virtual RESULT enableSubtitles(iSubtitleUser *user, SubtitleTrack &track);
    virtual RESULT disableSubtitles();
    virtual RESULT getCachedSubtitle(SubtitleTrack &track);
    virtual RESULT getSubtitleList(std::vector<SubtitleTrack> &subtitle_list);

    /* --- iRecordableService (stubs) --- */
    virtual RESULT connectEvent(const sigc::slot<void(iRecordableService*,int)> &event,
                        ePtr<eConnection> &connection);
    virtual RESULT getError(int &error);
    virtual RESULT subServices(ePtr<iSubserviceList> &ptr);
    virtual RESULT prepare(const char *filename,
                   time_t begTime=-1, time_t endTime=-1, int eit_event_id=-1,
                   const char *name=0, const char *descr=0,
                   const char *tags=0, bool descramble=true,
                   bool recordecm=false, int packetsize=188);
    virtual RESULT prepareStreaming(bool descramble=true, bool includeecm=false);
    virtual RESULT start(bool simulate=false);
    virtual RESULT record(ePtr<iStreamableService> &ptr);
    virtual RESULT getFilenameExtension(std::string &ext);
    virtual RESULT frontendInfo(ePtr<iFrontendInformation> &ptr);
    virtual RESULT stopRecord();

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
