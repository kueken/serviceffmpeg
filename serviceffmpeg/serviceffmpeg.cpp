/*
 * serviceffmpeg.cpp
 *
 * Enigma2 media service plugin - Enigma2-side glue
 * Spawns ffmpeg-player as external process, communicates via Unix socket.
 *
 * License: GPL v2
 */

#include "serviceffmpeg.h"

#include <lib/base/ebase.h>
#include <lib/base/eerror.h>
#include <lib/base/init.h>
#include <lib/base/init_num.h>
#include <lib/base/nconfig.h>
#include <lib/components/file_eraser.h>
#include <lib/dvb/epgcache.h>
#include <lib/dvb/dvbtime.h>
#include <lib/gui/esubtitle.h>

#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Path to the external player binary - installed alongside the .so */
#ifndef SFMP_PLAYER_BIN
#define SFMP_PLAYER_BIN  "/usr/bin/ffmpeg-player"
#endif

/* Position update interval in ms */
#define SFMP_POSITION_INTERVAL  500

/* ======================================================================
 * Simple JSON helpers (no external dependency)
 * These are intentionally minimal - just enough to build/parse our protocol
 * ====================================================================== */
static std::string json_str(const std::string &key, const std::string &val)
{
    return "\"" + key + "\":\"" + val + "\"";
}
static std::string json_int(const std::string &key, long long val)
{
    char buf[64]; snprintf(buf, sizeof(buf), "%lld", val);
    return "\"" + key + "\":" + buf;
}
static std::string json_obj(const std::string &inner)
{
    return "{" + inner + "}";
}

/* Extract a string value from flat JSON: "key":"value" */
static std::string json_get_str(const std::string &json, const std::string &key)
{
    std::string needle = "\"" + key + "\":\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos += needle.length();
    size_t end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

/* Extract an integer value from flat JSON: "key":12345 */
static long long json_get_int(const std::string &json, const std::string &key)
{
    std::string needle = "\"" + key + "\":";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return 0;
    pos += needle.length();
    return strtoll(json.c_str() + pos, NULL, 10);
}

/* ======================================================================
 * eStaticServiceFfmpegInfo
 * ====================================================================== */
DEFINE_REF(eStaticServiceFfmpegInfo);

eStaticServiceFfmpegInfo::eStaticServiceFfmpegInfo() {}

RESULT eStaticServiceFfmpegInfo::getName(const eServiceReference &ref, std::string &name)
{
    if (ref.name.length())
        name = ref.name;
    else
    {
        size_t last = ref.path.rfind('/');
        name = (last != std::string::npos) ? ref.path.substr(last + 1) : ref.path;
    }
    return 0;
}

int eStaticServiceFfmpegInfo::getLength(const eServiceReference &ref)
{
    return -1;
}

int eStaticServiceFfmpegInfo::getInfo(const eServiceReference &ref, int w)
{
    struct stat s;
    switch (w)
    {
    case iServiceInformation::sTimeCreate:
        if (stat(ref.path.c_str(), &s) == 0) return (int)s.st_mtime;
        break;
    case iServiceInformation::sFileSize:
        if (stat(ref.path.c_str(), &s) == 0) return (int)s.st_size;
        break;
    }
    return iServiceInformation::resNA;
}

long long eStaticServiceFfmpegInfo::getFileSize(const eServiceReference &ref)
{
    struct stat s;
    if (stat(ref.path.c_str(), &s) == 0) return s.st_size;
    return 0;
}

RESULT eStaticServiceFfmpegInfo::getEvent(const eServiceReference &ref,
                                           ePtr<eServiceEvent> &evt, time_t start_time)
{
    if (ref.path.find("://") != std::string::npos)
    {
        eServiceReference eq(ref);
        eq.type = eServiceFactoryFfmpeg::id;
        eq.path.clear();
        return eEPGCache::getInstance()->lookupEventTime(eq, start_time, evt);
    }
    evt = 0;
    return -1;
}

/* ======================================================================
 * eServiceFfmpegInfoContainer
 * ====================================================================== */
DEFINE_REF(eServiceFfmpegInfoContainer);

eServiceFfmpegInfoContainer::eServiceFfmpegInfoContainer()
    : m_double(0.0), m_buf(NULL), m_buf_size(0) {}

eServiceFfmpegInfoContainer::~eServiceFfmpegInfoContainer()
{
    delete[] m_buf;
}

double eServiceFfmpegInfoContainer::getDouble(unsigned int) const { return m_double; }
unsigned char *eServiceFfmpegInfoContainer::getBuffer(unsigned int &size) const
{
    size = m_buf_size; return m_buf;
}
void eServiceFfmpegInfoContainer::setDouble(double v) { m_double = v; }

/* ======================================================================
 * eServiceFfmpegOfflineOperations
 * ====================================================================== */
class eServiceFfmpegOfflineOperations : public iServiceOfflineOperations
{
    DECLARE_REF(eServiceFfmpegOfflineOperations);
    eServiceReference m_ref;
public:
    eServiceFfmpegOfflineOperations(const eServiceReference &ref) : m_ref(ref) {}
    RESULT deleteFromDisk(int simulate)
    {
        if (!simulate)
        {
            std::list<std::string> files;
            getListOfFilenames(files);
            eBackgroundFileEraser *eraser = eBackgroundFileEraser::getInstance();
            for (auto &f : files)
            {
                if (eraser) eraser->erase(f.c_str());
                else        ::unlink(f.c_str());
            }
        }
        return 0;
    }
    RESULT getListOfFilenames(std::list<std::string> &res)
    {
        res.clear();
        res.push_back(m_ref.path);
        return 0;
    }
    RESULT reindex() { return -1; }
};
DEFINE_REF(eServiceFfmpegOfflineOperations);

/* ======================================================================
 * eServiceFactoryFfmpeg
 * ====================================================================== */
DEFINE_REF(eServiceFactoryFfmpeg);

eServiceFactoryFfmpeg::eServiceFactoryFfmpeg()
{
    ePtr<eServiceCenter> sc;
    eServiceCenter::getPrivInstance(sc);
    if (sc)
    {
        std::list<std::string> ext;
        /* Audio */
        ext.push_back("mp3");  ext.push_back("mp2");  ext.push_back("m2a");
        ext.push_back("ogg");  ext.push_back("oga");  ext.push_back("flac");
        ext.push_back("wav");  ext.push_back("wave"); ext.push_back("aac");
        ext.push_back("m4a");  ext.push_back("wma");  ext.push_back("ac3");
        ext.push_back("dts");  ext.push_back("mka");  ext.push_back("ape");
        ext.push_back("alac"); ext.push_back("opus"); ext.push_back("amr");
        ext.push_back("au");   ext.push_back("wv");   ext.push_back("mid");
        ext.push_back("aif");  ext.push_back("aiff");
        /* Video */
        ext.push_back("mkv");  ext.push_back("mp4");  ext.push_back("mov");
        ext.push_back("avi");  ext.push_back("mpg");  ext.push_back("mpeg");
        ext.push_back("mpe");  ext.push_back("m4v");  ext.push_back("vob");
        ext.push_back("divx"); ext.push_back("flv");  ext.push_back("wmv");
        ext.push_back("asf");  ext.push_back("3gp");  ext.push_back("3g2");
        ext.push_back("rm");   ext.push_back("rmvb"); ext.push_back("ogm");
        ext.push_back("ogv");  ext.push_back("webm"); ext.push_back("ts");
        ext.push_back("m2ts"); ext.push_back("mts");  ext.push_back("dat");
        ext.push_back("pva");  ext.push_back("wtv");  ext.push_back("stream");
        ext.push_back("hevc"); ext.push_back("h264"); ext.push_back("h265");
        ext.push_back("264");  ext.push_back("265");
        /* Playlists / streams */
        ext.push_back("m3u");  ext.push_back("m3u8"); ext.push_back("pls");

        /* Replace servicemp3 (0x1001) as the default handler */
        sc->removeServiceFactory(0x1001);
        sc->addServiceFactory(eServiceFactoryFfmpeg::id, this, ext);
    }
    m_service_info = new eStaticServiceFfmpegInfo();
    eDebug("[serviceffmpeg] registered, replacing servicemp3");
}

eServiceFactoryFfmpeg::~eServiceFactoryFfmpeg()
{
    ePtr<eServiceCenter> sc;
    eServiceCenter::getPrivInstance(sc);
    if (sc) sc->removeServiceFactory(eServiceFactoryFfmpeg::id);
}

RESULT eServiceFactoryFfmpeg::play(const eServiceReference &ref, ePtr<iPlayableService> &ptr)
{
    ptr = new eServiceFfmpeg(ref);
    return 0;
}

RESULT eServiceFactoryFfmpeg::record(const eServiceReference &ref, ePtr<iRecordableService> &ptr)
{
    ptr = new eServiceFfmpeg(ref);
    return 0;
}

RESULT eServiceFactoryFfmpeg::list(const eServiceReference &, ePtr<iListableService> &ptr)
{
    ptr = 0; return -1;
}

RESULT eServiceFactoryFfmpeg::info(const eServiceReference &ref, ePtr<iStaticServiceInformation> &ptr)
{
    ptr = m_service_info; return 0;
}

RESULT eServiceFactoryFfmpeg::offlineOperations(const eServiceReference &ref,
                                                  ePtr<iServiceOfflineOperations> &ptr)
{
    ptr = new eServiceFfmpegOfflineOperations(ref); return 0;
}

/* ======================================================================
 * eServiceFfmpeg - Constructor / Destructor
 * ====================================================================== */
DEFINE_REF(eServiceFfmpeg);

eServiceFfmpeg::eServiceFfmpeg(eServiceReference ref)
    : m_state(stIdle)
    , m_ref(ref)
    , m_useragent("Enigma2 HbbTV/1.1.7 (+PVR+RTSP+DL;OpenPLi;;;)")
    , m_extra_headers("")
    , m_current_audio_track(-1)
    , m_current_sub_track(-1)
    , m_cached_sub_track(-2)
    , m_paused(false)
    , m_buffering(false)
    , m_buffer_percentage(0)
    , m_last_position(0)
    , m_duration(0)
    , m_seekable(false)
    , m_is_live(false)
    , m_trickmode_speed(0)
    , m_width(-1), m_height(-1), m_aspect(-1), m_framerate(-1)
    , m_progressive(false)
    , m_subtitle_user(NULL)
    , m_error_code(0)
    , m_recording(false)
    , m_player_pid(-1)
    , m_ipc_fd(-1)
{
    m_nownext_timer = eTimer::create(eApp);
    CONNECT(m_nownext_timer->timeout, eServiceFfmpeg::updateEpgCacheNowNext);

    /* Check for alternate user agent config */
    if (eConfigManager::getConfigBoolValue("config.mediaplayer.useAlternateUserAgent"))
        m_useragent = eConfigManager::getConfigValue("config.mediaplayer.alternateUserAgent");

    /* Parse extra headers from URL fragment: url#key=val&key2=val2 */
    size_t hash = m_ref.path.find('#');
    if (hash != std::string::npos &&
        (m_ref.path.compare(0, 4, "http") == 0 || m_ref.path.compare(0, 4, "rtsp") == 0))
    {
        m_extra_headers = m_ref.path.substr(hash + 1);
        /* Extract User-Agent if present */
        size_t ua_pos = m_extra_headers.find("User-Agent=");
        if (ua_pos != std::string::npos)
        {
            size_t start = ua_pos + 11;
            size_t end   = m_extra_headers.find('&', start);
            m_useragent  = (end != std::string::npos)
                           ? m_extra_headers.substr(start, end - start)
                           : m_extra_headers.substr(start);
        }
    }

    eDebug("[serviceffmpeg] created for uri=%s", m_ref.path.c_str());
}

eServiceFfmpeg::~eServiceFfmpeg()
{
    m_current_sub_track = -1;
    
    stop();
    m_nownext_timer->stop();
}

/* ======================================================================
 * Player process management
 * ====================================================================== */
bool eServiceFfmpeg::launchPlayer()
{
    /* Build socket path using our PID so multiple instances don't collide */
    char sock_path[128];
    snprintf(sock_path, sizeof(sock_path), SFMP_SOCKET_PATH, (int)getpid());
    m_socket_path = sock_path;

    /* Create the Unix domain socket server side BEFORE fork,
     * so the player can connect as soon as it's ready */
    int srv_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv_fd < 0) { eDebug("[serviceffmpeg] socket() failed: %m"); return false; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, m_socket_path.c_str(), sizeof(addr.sun_path) - 1);
    unlink(m_socket_path.c_str());

    if (bind(srv_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        eDebug("[serviceffmpeg] bind() failed: %m");
        close(srv_fd); return false;
    }
    listen(srv_fd, 1);

    /* Build command line for ffmpeg-player */
    std::string uri = buildUri();

    pid_t pid = fork();
    if (pid < 0) { eDebug("[serviceffmpeg] fork() failed: %m"); close(srv_fd); return false; }

    if (pid == 0)
    {
        /* Child process */
        /* Close all E2 file descriptors except stdin/stdout/stderr */
        int maxfd = sysconf(_SC_OPEN_MAX);
        for (int fd = 3; fd < maxfd; fd++)
        {
            if (fd != srv_fd) close(fd);
        }

        char sock_arg[128];
        snprintf(sock_arg, sizeof(sock_arg), "--socket=%s", m_socket_path.c_str());

        execl(SFMP_PLAYER_BIN, SFMP_PLAYER_BIN,
              sock_arg,
              "--uri", uri.c_str(),
              NULL);

        /* If execl fails */
        fprintf(stderr, "[ffmpeg-player] execl failed: %s\n", strerror(errno));
        _exit(127);
    }

    /* Parent */
    m_player_pid = pid;
    eDebug("[serviceffmpeg] launched player pid=%d", (int)pid);

    /* Accept connection from player (with timeout) */
    struct timeval tv = { 5, 0 };
    fd_set rset;
    FD_ZERO(&rset); FD_SET(srv_fd, &rset);
    int sel = select(srv_fd + 1, &rset, NULL, NULL, &tv);
    if (sel <= 0)
    {
        eDebug("[serviceffmpeg] player did not connect within 5s");
        close(srv_fd);
        killPlayer();
        return false;
    }

    m_ipc_fd = accept(srv_fd, NULL, NULL);
    close(srv_fd);  /* Done with listen socket */

    if (m_ipc_fd < 0) { eDebug("[serviceffmpeg] accept() failed: %m"); killPlayer(); return false; }

    /* Make non-blocking for E2 event loop integration */
    int flags = fcntl(m_ipc_fd, F_GETFL, 0);
    fcntl(m_ipc_fd, F_SETFL, flags | O_NONBLOCK);

    /* Register socket notifier so E2 calls ipcReceive() when data arrives */
    m_sn_ipc = eSocketNotifier::create(eApp, m_ipc_fd, eSocketNotifier::Read);
    CONNECT(m_sn_ipc->activated, eServiceFfmpeg::ipcReceive);

    eDebug("[serviceffmpeg] IPC connected");
    return true;
}

void eServiceFfmpeg::killPlayer()
{
    if (m_sn_ipc) { m_sn_ipc->stop(); m_sn_ipc = NULL; }
    if (m_ipc_fd >= 0) { close(m_ipc_fd); m_ipc_fd = -1; }

    if (m_player_pid > 0)
    {
        kill(m_player_pid, SIGTERM);
        int status;
        /* Give it 2 seconds then SIGKILL */
        for (int i = 0; i < 20; i++)
        {
            usleep(100000);
            if (waitpid(m_player_pid, &status, WNOHANG) == m_player_pid)
                goto done;
        }
        kill(m_player_pid, SIGKILL);
        waitpid(m_player_pid, &status, 0);
done:
        eDebug("[serviceffmpeg] player pid=%d reaped", (int)m_player_pid);
        m_player_pid = -1;
    }
    if (!m_socket_path.empty()) { unlink(m_socket_path.c_str()); m_socket_path.clear(); }
}

std::string eServiceFfmpeg::buildUri() const
{
    std::string uri = m_ref.path;
    /* Strip fragment (handled separately as headers) */
    size_t hash = uri.find('#');
    if (hash != std::string::npos) uri = uri.substr(0, hash);
    /* Strip &suburi= (external subtitle, TODO) */
    size_t sub = uri.find("&suburi=");
    if (sub != std::string::npos) uri = uri.substr(0, sub);
    return uri;
}

/* ======================================================================
 * IPC send/receive
 * Wire format: one JSON object per line (newline terminated)
 * {"cmd":"play"}\n
 * {"evt":"started"}\n
 * {"evt":"position","pos_ms":12345,"dur_ms":98765}\n
 * ====================================================================== */
bool eServiceFfmpeg::ipcSend(const std::string &cmd, const std::string &params)
{
    if (m_ipc_fd < 0) return false;
    std::string msg = "{\"cmd\":\"" + cmd + "\"";
    if (!params.empty()) msg += "," + params;
    msg += "}\n";

    ssize_t sent = write(m_ipc_fd, msg.c_str(), msg.length());
    if (sent < 0) { eDebug("[serviceffmpeg] ipcSend write error: %m"); return false; }
    return true;
}

void eServiceFfmpeg::ipcReceive(int)
{
    /* Read available data into line buffer */
    char buf[4096];
    ssize_t n = read(m_ipc_fd, buf, sizeof(buf) - 1);
    if (n <= 0)
    {
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        eDebug("[serviceffmpeg] IPC pipe closed (player exited)");
        /* Player died unexpectedly - signal EOF so E2 can react */
        if (m_state == stRunning || m_state == stPaused)
            m_event((iPlayableService*)this, iPlayableService::evEOF);
        return;
    }
    buf[n] = 0;
    m_ipc_recv_buf += buf;

    /* Process complete lines */
    size_t pos;
    while ((pos = m_ipc_recv_buf.find('\n')) != std::string::npos)
    {
        std::string line = m_ipc_recv_buf.substr(0, pos);
        m_ipc_recv_buf   = m_ipc_recv_buf.substr(pos + 1);

        if (line.empty()) continue;

        std::string evt     = json_get_str(line, "evt");
        std::string payload = line;   /* full JSON for further parsing */
        handleEvent(evt, payload);
    }
}

/* ======================================================================
 * Event dispatcher
 * ====================================================================== */
void eServiceFfmpeg::handleEvent(const std::string &evt, const std::string &payload)
{
    if (evt == SFMP_EVT_STARTED)
    {
        eDebug("[serviceffmpeg] evStarted");
        m_state = stRunning;
        m_event((iPlayableService*)this, iPlayableService::evStart);
        m_nownext_timer->startLongTimer(3);
    }
    else if (evt == SFMP_EVT_EOF)
    {
        eDebug("[serviceffmpeg] evEOF");
        m_event((iPlayableService*)this, iPlayableService::evEOF);
    }
    else if (evt == SFMP_EVT_SOF)
    {
        eDebug("[serviceffmpeg] evSOF");
        m_event((iPlayableService*)this, iPlayableService::evSOF);
    }
    else if (evt == SFMP_EVT_ERROR)
    {
        m_error_code    = (int)json_get_int(payload, "code");
        m_error_message = json_get_str(payload, "msg");
        eDebug("[serviceffmpeg] error %d: %s", m_error_code, m_error_message.c_str());
        m_event((iPlayableService*)this, iPlayableService::evUser + 12);
    }
    else if (evt == SFMP_EVT_BUFFERING)
    {
        int pct = (int)json_get_int(payload, "pct");
        m_buffer_percentage = pct;
        if (!m_buffering)
        {
            m_buffering = true;
            m_event((iPlayableService*)this, iPlayableService::evBuffering);
        }
    }
    else if (evt == SFMP_EVT_BUFFER_DONE)
    {
        if (m_buffering)
        {
            m_buffering = false;
            m_buffer_percentage = 100;
            m_event((iPlayableService*)this, iPlayableService::evUpdatedInfo); /* reuse existing event */
        }
    }
    else if (evt == SFMP_EVT_POSITION)
    {
        m_last_position = msToPts(json_get_int(payload, "pos_ms"));
        m_duration      = msToPts(json_get_int(payload, "dur_ms"));
        /* evUpdatedInfo would spam too much, position is pulled via getPlayPosition() */
    }
    else if (evt == SFMP_EVT_INFO)
    {
        /* Stream info arrived - parse tracks, video info, etc. */
        m_seekable   = (bool)json_get_int(payload, "seekable");
        m_is_live    = (bool)json_get_int(payload, "live");
        m_duration   = msToPts(json_get_int(payload, "dur_ms"));
        m_stream_info.container = json_get_str(payload, "container");
        m_stream_info.title     = json_get_str(payload, "title");
        parseTrackList(payload);
        parseVideoInfo(payload);
        m_event((iPlayableService*)this, iPlayableService::evUpdatedInfo);
    }
    else if (evt == SFMP_EVT_AUDIO_TRACKS)
    {
        parseTrackList(payload);
        m_event((iPlayableService*)this, iPlayableService::evUpdatedInfo);
    }
    else if (evt == SFMP_EVT_VIDEO_SIZE)
    {
        m_width       = (int)json_get_int(payload, "w");
        m_height      = (int)json_get_int(payload, "h");
        m_aspect      = (int)json_get_int(payload, "aspect");
        m_framerate   = (int)json_get_int(payload, "fps");
        m_progressive = (bool)json_get_int(payload, "progressive");
        eDebug("[serviceffmpeg] video %dx%d aspect=%d fps=%d",
               m_width, m_height, m_aspect, m_framerate);
        m_event((iPlayableService*)this, iPlayableService::evVideoSizeChanged);
        m_event((iPlayableService*)this, iPlayableService::evVideoSizeChanged);
    }
    else if (evt == SFMP_EVT_SUBTITLE_PAGE)
    {
        parseSubtitle(payload);
    }
    else if (evt == SFMP_EVT_SUBTITLE_CLEAR)
    {
        if (m_subtitle_user && m_current_sub_track >= 0)
        {
            ePangoSubtitlePage page;
            page.m_show_pts = 0;
            page.m_timeout  = 0;
            m_subtitle_user->setPage(page);
        }
    }
    else if (evt == SFMP_EVT_RECORD_INFO)
    {
        /* recording progress - can be forwarded if needed */
    }
    else
    {
        eDebug("[serviceffmpeg] unknown event: %s", evt.c_str());
    }
}

/* ======================================================================
 * Track list parsing
 * Audio tracks JSON fragment: "audio":[{"id":0,"lang":"deu","codec":"ac3","ch":2},...] 
 * ====================================================================== */
void eServiceFfmpeg::parseTrackList(const std::string &json)
{
    /* Simple linear scan - not a full JSON parser but sufficient for our protocol */
    m_stream_info.audio_tracks.clear();
    m_stream_info.sub_tracks.clear();

    /* Parse audio tracks */
    size_t apos = json.find("\"audio\":[");
    if (apos != std::string::npos)
    {
        size_t start = json.find('[', apos);
        size_t end   = json.find(']', start);
        std::string arr = json.substr(start, end - start + 1);
        size_t obj_start = 0;
        while ((obj_start = arr.find('{', obj_start)) != std::string::npos)
        {
            size_t obj_end = arr.find('}', obj_start);
            std::string obj = arr.substr(obj_start, obj_end - obj_start + 1);
            sFfmpegAudioTrack t;
            t.id         = (int)json_get_int(obj, "id");
            t.pid        = (int)json_get_int(obj, "pid");
            t.language   = json_get_str(obj, "lang");
            t.codec      = json_get_str(obj, "codec");
            t.channels   = (int)json_get_int(obj, "ch");
            t.samplerate = (int)json_get_int(obj, "rate");
            t.bitrate    = (int)json_get_int(obj, "bps");
            m_stream_info.audio_tracks.push_back(t);
            obj_start = obj_end + 1;
        }
    }

    /* Parse subtitle tracks */
    size_t spos = json.find("\"subs\":[");
    if (spos != std::string::npos)
    {
        size_t start = json.find('[', spos);
        size_t end   = json.find(']', start);
        std::string arr = json.substr(start, end - start + 1);
        size_t obj_start = 0;
        while ((obj_start = arr.find('{', obj_start)) != std::string::npos)
        {
            size_t obj_end = arr.find('}', obj_start);
            std::string obj = arr.substr(obj_start, obj_end - obj_start + 1);
            sFfmpegSubtitleTrack t;
            t.id       = (int)json_get_int(obj, "id");
            t.language = json_get_str(obj, "lang");
            t.codec    = json_get_str(obj, "codec");
            t.bitmap   = (bool)json_get_int(obj, "bitmap");
            m_stream_info.sub_tracks.push_back(t);
            obj_start = obj_end + 1;
        }
    }

    if (m_current_audio_track < 0 && !m_stream_info.audio_tracks.empty())
        m_current_audio_track = 0;

    eDebug("[serviceffmpeg] parsed %zu audio tracks, %zu subtitle tracks",
           m_stream_info.audio_tracks.size(), m_stream_info.sub_tracks.size());
}

void eServiceFfmpeg::parseVideoInfo(const std::string &json)
{
    m_stream_info.video.codec       = json_get_str(json, "vcodec");
    m_stream_info.video.width       = (int)json_get_int(json, "w");
    m_stream_info.video.height      = (int)json_get_int(json, "h");
    m_stream_info.video.aspect      = (int)json_get_int(json, "aspect");
    m_stream_info.video.framerate   = (int)json_get_int(json, "fps");
    m_stream_info.video.bitrate     = (int)json_get_int(json, "vbps");
    m_stream_info.video.progressive = (bool)json_get_int(json, "progressive");
}

void eServiceFfmpeg::parseSubtitle(const std::string &json)
{
    if (!m_subtitle_user || m_current_sub_track < 0) return;
    std::string text     = json_get_str(json, "text");
    int64_t pts_ms       = json_get_int(json, "pts_ms");
    int64_t duration_ms  = json_get_int(json, "dur_ms");
    if (duration_ms <= 0) duration_ms = 5000;

    ePangoSubtitlePage page;
    gRGB color(0xD0, 0xD0, 0xD0);
    page.m_elements.push_back(ePangoSubtitlePageElement(color, text.c_str()));
    page.m_show_pts = msToPts(pts_ms);
    page.m_timeout  = (int)duration_ms;
    m_subtitle_user->setPage(page);
}

/* ======================================================================
 * EPG/NowNext
 * ====================================================================== */
void eServiceFfmpeg::updateEpgCacheNowNext()
{
    bool update = false;
    ePtr<eServiceEvent> ptr = 0;
    eServiceReference ref(m_ref);
    ref.type = eServiceFactoryFfmpeg::id;
    ref.path.clear();

    if (eEPGCache::getInstance() &&
        eEPGCache::getInstance()->lookupEventTime(ref, -1, ptr) >= 0)
    {
        ePtr<eServiceEvent> cur = m_event_now;
        if (!cur || !ptr || cur->getEventId() != ptr->getEventId())
        {
            update = true;
            m_event_now = ptr;
            time_t next_time = ptr->getBeginTime() + ptr->getDuration();
            ePtr<eServiceEvent> nxt = 0;
            if (eEPGCache::getInstance()->lookupEventTime(ref, next_time, nxt) >= 0)
                m_event_next = nxt;
        }
    }

    int refresh = 60;
    if (m_event_next)
    {
        time_t now = eDVBLocalTimeHandler::getInstance()->nowTime();
        refresh = (int)(m_event_next->getBeginTime() - now) + 3;
        if (refresh <= 0 || refresh > 60) refresh = 60;
    }
    m_nownext_timer->startLongTimer(refresh);
    if (update) m_event((iPlayableService*)this, iPlayableService::evUpdatedEventInfo);
}

/* ======================================================================
 * iPlayableService implementation
 * ====================================================================== */
RESULT eServiceFfmpeg::connectEvent(
    const sigc::slot<void(iPlayableService*,int)> &event,
    ePtr<eConnection> &connection)
{
    connection = new eConnection((iPlayableService*)this, m_event.connect(event));
    return 0;
}

RESULT eServiceFfmpeg::start()
{
    ASSERT(m_state == stIdle);
    eDebug("[serviceffmpeg] start %s", m_ref.path.c_str());

    if (!launchPlayer())
    {
        m_state = stError;
        m_error_code    = SFMP_ERR_OPEN_FAILED;
        m_error_message = "failed to launch ffmpeg-player";
        m_event((iPlayableService*)this, iPlayableService::evUser + 12);
        return -1;
    }

    /* Send initial configuration to player */
    if (!m_useragent.empty())
        ipcSend(SFMP_CMD_SET_USERAGENT,
                json_str("ua", m_useragent));
    if (!m_extra_headers.empty())
        ipcSend(SFMP_CMD_SET_HEADERS,
                json_str("headers", m_extra_headers));

    ipcSend(SFMP_CMD_PLAY);
    return 0;
}

RESULT eServiceFfmpeg::stop()
{
    if (m_state == stStopped) return -1;
    eDebug("[serviceffmpeg] stop");
    if (m_ipc_fd >= 0) ipcSend(SFMP_CMD_STOP);
    killPlayer();
    m_state = stStopped;
    m_nownext_timer->stop();
    return 0;
}

RESULT eServiceFfmpeg::pause(ePtr<iPauseableService> &ptr)
{
    ptr = this; return 0;
}

RESULT eServiceFfmpeg::seek(ePtr<iSeekableService> &ptr)
{
    ptr = this; return 0;
}

RESULT eServiceFfmpeg::audioTracks(ePtr<iAudioTrackSelection> &ptr)
{
    ptr = this; return 0;
}

RESULT eServiceFfmpeg::subtitle(ePtr<iSubtitleOutput> &ptr)
{
    ptr = this; return 0;
}

RESULT eServiceFfmpeg::info(ePtr<iServiceInformation> &ptr)
{
    ptr = this; return 0;
}

RESULT eServiceFfmpeg::setSlowMotion(int ratio)
{
    if (!ratio) return 0;
    eDebug("[serviceffmpeg] slowmotion ratio=%d", ratio);
    return ipcSend(SFMP_CMD_SET_SPEED,
                   json_int("speed", ratio > 0 ? 1 : -1)) ? 0 : -1;
}

RESULT eServiceFfmpeg::setFastForward(int ratio)
{
    eDebug("[serviceffmpeg] fastforward ratio=%d", ratio);
    m_trickmode_speed = ratio;
    return ipcSend(SFMP_CMD_SET_SPEED,
                   json_int("speed", ratio)) ? 0 : -1;
}

/* ======================================================================
 * iPauseableService
 * ====================================================================== */
RESULT eServiceFfmpeg::pause()
{
    if (m_state != stRunning) return -1;
    eDebug("[serviceffmpeg] pause");
    m_paused = true;
    m_state  = stPaused;
    return ipcSend(SFMP_CMD_PAUSE) ? 0 : -1;
}

RESULT eServiceFfmpeg::unpause()
{
    if (m_state != stPaused) return -1;
    eDebug("[serviceffmpeg] unpause");
    m_paused = false;
    m_state  = stRunning;
    return ipcSend(SFMP_CMD_RESUME) ? 0 : -1;
}

/* ======================================================================
 * iSeekableService
 * ====================================================================== */
RESULT eServiceFfmpeg::getLength(pts_t &len)
{
    len = m_duration;
    return (m_duration > 0) ? 0 : -1;
}

RESULT eServiceFfmpeg::seekTo(pts_t to)
{
    int64_t ms = ptsToMs(to);
    eDebug("[serviceffmpeg] seekTo %lld ms", (long long)ms);
    return ipcSend(SFMP_CMD_SEEK, json_int("pos_ms", ms)) ? 0 : -1;
}

RESULT eServiceFfmpeg::seekRelative(int direction, pts_t to)
{
    int64_t delta_ms = ptsToMs(to) * direction;
    eDebug("[serviceffmpeg] seekRelative %lld ms", (long long)delta_ms);
    return ipcSend(SFMP_CMD_SEEK_RELATIVE, json_int("delta_ms", delta_ms)) ? 0 : -1;
}

RESULT eServiceFfmpeg::getPlayPosition(pts_t &pos)
{
    pos = m_last_position;
    return 0;
}

RESULT eServiceFfmpeg::setTrickmode(int trick)
{
    return setFastForward(trick);
}

RESULT eServiceFfmpeg::isCurrentlySeekable()
{
    return m_seekable ? 1 : 0;
}

/* ======================================================================
 * iAudioTrackSelection
 * ====================================================================== */
int eServiceFfmpeg::getNumberOfTracks()
{
    return (int)m_stream_info.audio_tracks.size();
}

RESULT eServiceFfmpeg::selectTrack(unsigned int i)
{
    if (i >= m_stream_info.audio_tracks.size()) return -1;
    m_current_audio_track = (int)i;
    int stream_id = m_stream_info.audio_tracks[i].id;
    eDebug("[serviceffmpeg] select audio track %u (stream_id=%d)", i, stream_id);
    return ipcSend(SFMP_CMD_SET_AUDIO, json_int("id", stream_id)) ? 0 : -1;
}

RESULT eServiceFfmpeg::getTrackInfo(struct iAudioTrackInfo &info, unsigned int n)
{
    if (n >= m_stream_info.audio_tracks.size()) return -1;
    const sFfmpegAudioTrack &t = m_stream_info.audio_tracks[n];
    info.m_language    = t.language;
    info.m_description = t.codec;
    return 0;
}

int eServiceFfmpeg::getCurrentTrack()
{
    return m_current_audio_track;
}

/* ======================================================================
 * iSubtitleOutput
 * ====================================================================== */
RESULT eServiceFfmpeg::enableSubtitles(iSubtitleUser *parent, SubtitleTrack &track)
{
    disableSubtitles();
    m_subtitle_user = parent;

    /* Find the track by language/type */
    int stream_id = -1;
    for (size_t i = 0; i < m_stream_info.sub_tracks.size(); i++)
    {
        if (m_stream_info.sub_tracks[i].language == track.language_code ||
            (int)i == track.pid)
        {
            stream_id = m_stream_info.sub_tracks[i].id;
            m_current_sub_track = (int)i;
            break;
        }
    }
    if (stream_id < 0)
    {
        eDebug("[serviceffmpeg] subtitle track not found");
        return -1;
    }

    
    

    eDebug("[serviceffmpeg] enable subtitles stream_id=%d", stream_id);
    ipcSend(SFMP_CMD_SET_SUBTITLE, json_int("id", stream_id));
    m_cached_sub_track = m_current_sub_track;
    return 0;
}

RESULT eServiceFfmpeg::disableSubtitles()
{
    m_subtitle_user = NULL;
    m_current_sub_track = -1;
    ipcSend(SFMP_CMD_SET_SUBTITLE, json_int("id", -1));
    return 0;
}

RESULT eServiceFfmpeg::getCachedSubtitle(SubtitleTrack &track)
{
    if (m_cached_sub_track < 0 ||
        m_cached_sub_track >= (int)m_stream_info.sub_tracks.size())
        return -1;
    const sFfmpegSubtitleTrack &t = m_stream_info.sub_tracks[m_cached_sub_track];
    track.language_code = t.language;
    track.pid           = t.id;
    track.type          = t.bitmap ? 1 : 0;
    return 0;
}

RESULT eServiceFfmpeg::getSubtitleList(std::vector<SubtitleTrack> &list)
{
    list.clear();
    for (size_t i = 0; i < m_stream_info.sub_tracks.size(); i++)
    {
        const sFfmpegSubtitleTrack &t = m_stream_info.sub_tracks[i];
        SubtitleTrack st;
        st.language_code = t.language;
        st.pid           = t.id;
        st.type          = t.bitmap ? 1 : 0;
        list.push_back(st);
    }
    return 0;
}

/* ======================================================================
 * iRecordableService
 * ====================================================================== */
/* record() was removed from iRecordableService in scarthgap.
 * Use prepare(filename) + start() instead (stub implementations below).
 * This helper is kept for internal use. */

RESULT eServiceFfmpeg::stopRecord()
{
    if (!m_recording) return -1;
    m_recording = false;
    return ipcSend(SFMP_CMD_RECORD_STOP) ? 0 : -1;
}

RESULT eServiceFfmpeg::frontendInfo(ePtr<iFrontendInformation> &ptr)
{
    ptr = 0; return -1;
}

RESULT eServiceFfmpeg::connectEvent(
    const sigc::slot<void(iRecordableService*,int)> &event,
    ePtr<eConnection> &connection)
{
    connection = new eConnection((iRecordableService*)this, m_rec_event.connect(event));
    return 0;
}

RESULT eServiceFfmpeg::getError(int &error)
{
    error = m_error_code; return 0;
}

RESULT eServiceFfmpeg::subServices(ePtr<iSubserviceList> &ptr)
{
    ptr = 0; return -1;
}

/* ======================================================================
 * iServiceInformation
 * ====================================================================== */
RESULT eServiceFfmpeg::getName(std::string &name)
{
    if (!m_stream_info.title.empty())    { name = m_stream_info.title; return 0; }
    if (m_ref.name.length())             { name = m_ref.name; return 0; }
    size_t last = m_ref.path.rfind('/');
    name = (last != std::string::npos) ? m_ref.path.substr(last + 1) : m_ref.path;
    return 0;
}

int eServiceFfmpeg::getInfo(int w)
{
    switch (w)
    {
    case iServiceInformation::sVideoWidth:      return m_width;
    case iServiceInformation::sVideoHeight:     return m_height;
    case iServiceInformation::sAspect:          return m_aspect;
    case iServiceInformation::sFrameRate:       return m_framerate;
    case iServiceInformation::sProgressive:     return m_progressive ? 1 : 0;
    case iServiceInformation::sCurrentTitle:    return m_current_audio_track;
    case iServiceInformation::sTotalTitles:     return (int)m_stream_info.audio_tracks.size();
    case iServiceInformation::sBuffer:          return m_buffer_percentage;
    }
    return iServiceInformation::resNA;
}

std::string eServiceFfmpeg::getInfoString(int w)
{
    switch (w)
    {
    case iServiceInformation::sServiceref:  return m_ref.toString();
    case iServiceInformation::sUser + 12:   return m_error_message;
    }
    if (!m_stream_info.audio_tracks.empty() && m_current_audio_track >= 0 &&
        m_current_audio_track < (int)m_stream_info.audio_tracks.size())
    {
        if (w == (iServiceInformation::sUser + 30))
            return m_stream_info.audio_tracks[m_current_audio_track].codec;
    }
    if (w == (iServiceInformation::sUser + 31)) return m_stream_info.video.codec;
    return "";
}

ePtr<iServiceInfoContainer> eServiceFfmpeg::getInfoObject(int w)
{
    (void)w;
    return NULL;
}


/* ======================================================================
 * Stub implementations for mandatory scarthgap virtuals
 * ====================================================================== */

/* iPlayableService stubs */
RESULT eServiceFfmpeg::setTarget(int, bool)             { return -1; }
RESULT eServiceFfmpeg::audioChannel(ePtr<iAudioChannelSelection> &p) { p=0; return -1; }
RESULT eServiceFfmpeg::timeshift(ePtr<iTimeshiftService> &p)         { p=0; return -1; }
RESULT eServiceFfmpeg::tap(ePtr<iTapService> &p)                     { p=0; return -1; }
RESULT eServiceFfmpeg::cueSheet(ePtr<iCueSheet> &p)                  { p=0; return -1; }
RESULT eServiceFfmpeg::audioDelay(ePtr<iAudioDelay> &p)              { p=0; return -1; }
RESULT eServiceFfmpeg::rdsDecoder(ePtr<iRdsDecoder> &p)              { p=0; return -1; }
RESULT eServiceFfmpeg::stream(ePtr<iStreamableService> &p)           { p=0; return -1; }
RESULT eServiceFfmpeg::streamed(ePtr<iStreamedService> &p)           { p=0; return -1; }
RESULT eServiceFfmpeg::keys(ePtr<iServiceKeys> &p)                   { p=0; return -1; }
void   eServiceFfmpeg::setQpipMode(bool, bool)                       { }

/* iRecordableService stubs */
RESULT eServiceFfmpeg::prepare(const char *filename, time_t, time_t, int,
                                const char*, const char*, const char*,
                                bool, bool, int)
{
    m_record_path = filename ? filename : "";
    return 0;
}
RESULT eServiceFfmpeg::prepareStreaming(bool, bool) { return -1; }
RESULT eServiceFfmpeg::start(bool simulate)
{
    /* iRecordableService::start - triggers recording after prepare() */
    if (simulate) return 0;
    if (m_record_path.empty()) return -1;
    if (m_recording) return -1;
    eDebug("[serviceffmpeg] recording start: %s", m_record_path.c_str());
    m_recording = true;
    return ipcSend(SFMP_CMD_RECORD_START, json_str("path", m_record_path)) ? 0 : -1;
}
RESULT eServiceFfmpeg::record(ePtr<iStreamableService> &p) { p=0; return -1; }
RESULT eServiceFfmpeg::getFilenameExtension(std::string &ext) { ext = "ts"; return 0; }

/* ======================================================================
 * Module registration
 * ====================================================================== */
eAutoInitPtr<eServiceFactoryFfmpeg> init_eServiceFactoryFfmpeg(
    eAutoInitNumbers::service + 1, "eServiceFactoryFfmpeg");
