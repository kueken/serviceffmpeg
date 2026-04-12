#ifndef __serviceexteplayer3_h
#define __serviceexteplayer3_h

#include <lib/base/ebase.h>
#include <lib/base/eerror.h>
#include <lib/base/object.h>
#include <lib/base/nconfig.h>
#include <lib/dvb/epgcache.h>
#include <lib/dvb/pmt.h>
#include <lib/dvb/subtitle.h>
#include <lib/service/iservice.h>
#include <lib/gui/esubtitle.h>

#include <string>
#include <vector>

/* Track info structures */
struct sEp3AudioTrack {
    int         id;
    std::string encoding;
    std::string name;
};

struct sEp3SubtitleTrack {
    int         id;
    std::string encoding;
    std::string name;
};

struct sEp3VideoInfo {
    std::string encoding;
    int width        = 0;
    int height       = 0;
    int framerate    = 0;
    int progressive  = 0;
    int aspect_num   = 0;
    int aspect_den   = 0;
    int aspect_ratio = 0;
};

/* eStaticServiceEp3Info */
class eStaticServiceEp3Info : public iStaticServiceInformation
{
    DECLARE_REF(eStaticServiceEp3Info);
public:
    eStaticServiceEp3Info() {}
    RESULT getName(const eServiceReference &ref, std::string &name);
    int    getLength(const eServiceReference &ref);
    int    getInfo(const eServiceReference &ref, int w);
    long long getFileSize(const eServiceReference &ref);
    RESULT getEvent(const eServiceReference &ref, ePtr<eServiceEvent> &evt, time_t start_time);
};

/* eServiceEp3InfoContainer */
class eServiceEp3InfoContainer : public iServiceInfoContainer
{
    DECLARE_REF(eServiceEp3InfoContainer);
    double         m_double;
    unsigned char *m_buf;
    unsigned int   m_buf_size;
public:
    eServiceEp3InfoContainer();
    ~eServiceEp3InfoContainer();
    double         getDouble(unsigned int idx) const;
    unsigned char *getBuffer(unsigned int &size) const;
    void           setDouble(double v);
};

/* eServiceFactoryEp3 */
class eServiceFactoryEp3 : public iServiceHandler
{
    DECLARE_REF(eServiceFactoryEp3);
    ePtr<eStaticServiceEp3Info> m_service_info;
public:
    eServiceFactoryEp3();
    virtual ~eServiceFactoryEp3();
    enum { id = 0x1001 };
    RESULT play(const eServiceReference &, ePtr<iPlayableService> &);
    RESULT record(const eServiceReference &, ePtr<iRecordableService> &);
    RESULT list(const eServiceReference &, ePtr<iListableService> &);
    RESULT info(const eServiceReference &, ePtr<iStaticServiceInformation> &);
    RESULT offlineOperations(const eServiceReference &, ePtr<iServiceOfflineOperations> &);
};

/* eServiceEp3 — main service class */
class eServiceEp3
    : public iPlayableService
    , public iServiceInformation
    , public iPauseableService
    , public iSeekableService
    , public iAudioTrackSelection
    , public iSubtitleOutput
    , public iRecordableService
{
    DECLARE_REF(eServiceEp3);

public:
    eServiceEp3(eServiceReference ref);
    virtual ~eServiceEp3();

    /* iPlayableService */
    RESULT connectEvent(const sigc::slot<void(iPlayableService*,int)> &event, ePtr<eConnection> &connection);
    RESULT start();
    RESULT stop();
    RESULT pause(ePtr<iPauseableService> &ptr);
    RESULT seek(ePtr<iSeekableService> &ptr);
    RESULT setSlowMotion(int ratio);
    RESULT setFastForward(int ratio);
    RESULT audioTracks(ePtr<iAudioTrackSelection> &ptr);
    RESULT subtitle(ePtr<iSubtitleOutput> &ptr);
    RESULT info(ePtr<iServiceInformation> &ptr);
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

    /* iServiceInformation */
    RESULT getName(std::string &name);
    int    getInfo(int w);
    std::string getInfoString(int w);
    ePtr<iServiceInfoContainer> getInfoObject(int w);

    /* iPauseableService */
    RESULT pause();
    RESULT unpause();

    /* iSeekableService */
    RESULT getLength(pts_t &len);
    RESULT seekTo(pts_t to);
    RESULT seekRelative(int direction, pts_t to);
    RESULT getPlayPosition(pts_t &pos);
    RESULT setTrickmode(int trick);
    RESULT isCurrentlySeekable();

    /* iAudioTrackSelection */
    int    getNumberOfTracks();
    RESULT selectTrack(unsigned int i);
    RESULT getTrackInfo(struct iAudioTrackInfo &, unsigned int n);
    int    getCurrentTrack();

    /* iSubtitleOutput */
    RESULT enableSubtitles(iSubtitleUser *user, SubtitleTrack &track);
    RESULT disableSubtitles();
    RESULT getCachedSubtitle(SubtitleTrack &track);
    RESULT getSubtitleList(std::vector<SubtitleTrack> &subtitle_list);

    /* iRecordableService stubs */
    RESULT connectEvent(const sigc::slot<void(iRecordableService*,int)> &event, ePtr<eConnection> &connection);
    RESULT getError(int &error);
    RESULT subServices(ePtr<iSubserviceList> &ptr);
    RESULT prepare(const char *filename, time_t begTime=-1, time_t endTime=-1,
                   int eit_event_id=-1, const char *name=0, const char *descr=0,
                   const char *tags=0, bool descramble=true, bool recordecm=false,
                   int packetsize=188);
    RESULT prepareStreaming(bool descramble=true, bool includeecm=false);
    RESULT start(bool simulate=false);
    RESULT record(ePtr<iStreamableService> &ptr);
    RESULT getFilenameExtension(std::string &ext);
    RESULT frontendInfo(ePtr<iFrontendInformation> &ptr);
    RESULT stopRecord();

private:
    enum eState { stIdle, stRunning, stPaused, stStopped, stError };
    eState            m_state;
    eServiceReference m_ref;
    std::string       m_useragent;
    std::string       m_extra_headers;

    std::vector<sEp3AudioTrack>    m_audio_tracks;
    std::vector<sEp3SubtitleTrack> m_sub_tracks;
    sEp3VideoInfo                  m_video_info;
    int  m_current_audio_idx;
    int  m_current_sub_idx;
    int  m_cached_sub_idx;

    pts_t   m_last_position;
    pts_t   m_duration;
    bool    m_seekable;
    bool    m_is_live;
    int     m_error_code;
    int64_t m_last_poll_ms;

    iSubtitleUser *m_subtitle_user;

    pid_t  m_player_pid;
    int    m_stdin_fd;
    int    m_stderr_fd;
    ePtr<eSocketNotifier> m_sn_read;
    std::string           m_read_buf;

    ePtr<eTimer>        m_nownext_timer;
    ePtr<eServiceEvent> m_event_now;
    ePtr<eServiceEvent> m_event_next;

    sigc::signal<void(iPlayableService*,int)>   m_event;
    sigc::signal<void(iRecordableService*,int)> m_rec_event;

    bool        launchPlayer();
    void        stopPlayer();
    void        sendCmd(const char *cmd);
    std::string buildUri() const;
    void        onStderrData(int fd);
    void        processLine(const std::string &line);
    void        onPlaybackPlay(const std::string &line);
    void        onPlaybackStop(const std::string &line);
    void        onPlaybackLength(const std::string &line);
    void        onPositionUpdate(const std::string &line);
    void        onAudioList(const std::string &line);
    void        onAudioCurrent(const std::string &line);
    void        onSubtitleList(const std::string &line);
    void        onSubtitleCurrent(const std::string &line);
    void        onSubtitleData(const std::string &line);
    void        onSubtitleFlush(const std::string &line);
    void        onVideoInfoVc(const std::string &line);
    void        onVideoInfoVe(const std::string &line);
    void        onFfError(const std::string &line);
    void        parseAudioList(const std::string &arr);
    void        parseSubtitleList(const std::string &arr);
    void        updateEpgCacheNowNext();

    static pts_t   secToPts(int64_t sec) { return sec * 90000LL; }
    static int64_t ptsToSec(pts_t pts)   { return pts / 90000LL; }
    static pts_t   msToPts(int64_t ms)   { return ms  * 90LL;    }
};

#endif
