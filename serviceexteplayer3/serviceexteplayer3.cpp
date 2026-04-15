/*
 * serviceexteplayer3.cpp  —  Enigma2 media service plugin using exteplayer3
 *
 * exteplayer3 IPC protocol (e2iplayer/exteplayer3, PLi OE-mirror build):
 *
 *   stdin  commands (E2 → ep3):
 *     q\n              stop/quit
 *     p\n              pause
 *     c\n              continue (unpause)
 *     f<N>\n           fast-forward speed N
 *     b<N>\n           fast-backward speed N
 *     m<N>\n           slow-motion speed N
 *     gf<sec>\n        seek absolute (force)
 *     kf<sec>\n        seek relative (force)
 *     j\n              query position → {"J":...}
 *     l\n              query length  → {"PLAYBACK_LENGTH":...}
 *     al\n             list audio tracks → {"al":[...]}
 *     ac\n             current audio     → {"ac":{...}}
 *     ai<idx>\n        switch audio by list-index
 *     sl\n             list subtitle tracks → {"sl":[...]}
 *     sc\n             current subtitle     → {"sc":{...}}
 *     si<idx>\n        switch subtitle by list-index  (si-1 = disable)
 *     vc\n             current video info → {"vc":{...}}
 *
 *   stderr events (ep3 → E2, one JSON object per line):
 *     {"EPLAYER3_EXTENDED":{"version":69}}
 *     {"PLAYBACK_OPEN":{"OutputName":"...","file":"...","sts":0}}
 *     {"OUTPUT_OPEN":{"sts":0}}
 *     {"PLAYBACK_PLAY":{"sts":0}}
 *     {"PLAYBACK_STOP":{"sts":0}}
 *     {"PLAYBACK_PAUSE":{"sts":0}}
 *     {"PLAYBACK_CONTINUE":{"sts":0}}
 *     {"PLAYBACK_FASTFORWARD":{"speed":N,"sts":0}}
 *     {"PLAYBACK_FASTBACKWARD":{"speed":N,"sts":0}}
 *     {"PLAYBACK_SEEK_ABS":{"sec":N,"sts":0}}
 *     {"PLAYBACK_SEEK":{"sec":N,"sts":0}}
 *     {"PLAYBACK_LENGTH":{"length":N,"sts":0}}
 *     {"J":{"ms":N}}  or  {"J":{"ms":N,"lms":N}}
 *     {"al":[{"id":N,"e":"ENC","n":"Name"},...]}
 *     {"ac":{"id":N,"e":"ENC","n":"Name"}}
 *     {"sl":[{"id":N,"e":"ENC","n":"Name"},...]}
 *     {"sc":{"id":N,"e":"ENC","n":"Name"}}
 *     {"vc":{"id":N,"e":"ENC","n":"","w":W,"h":H,"f":F,"p":P,"an":A,"ad":D}}
 *     {"v_e":{"w":W,"h":H,"a":A,"f":F,"p":P}}  (spontaneous from linuxdvb_mipsel)
 *     {"s_a":{"id":N,"s":MS,"e":MS,"t":"text"}}  (subtitle line)
 *     {"s_f":{"r":0}}                             (subtitle flush)
 *     {"FF_ERROR":{"msg":"...","code":N}}          (open error)
 *   EOF on stderr pipe → playback ended (no explicit EOT event)
 *
 * License: GPL-2.0
 */

#include "serviceexteplayer3.h"

#include <lib/base/ebase.h>
#include <lib/base/eerror.h>
#include <lib/base/init.h>
#include <lib/base/init_num.h>
#include <lib/base/nconfig.h>
#include <lib/dvb/epgcache.h>
#include <lib/dvb/dvbtime.h>
#include <lib/dvb/decoder.h>
#include <lib/gui/esubtitle.h>
#include <lib/service/service.h>

#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* DVB video ioctls needed to restore OSD after exteplayer3 */
#include <linux/netlink.h>
#include <linux/dvb/audio.h>
#include <linux/dvb/video.h>

#ifndef EXTEPLAYER3_BIN
#define EXTEPLAYER3_BIN "/usr/bin/exteplayer3"
#endif

/* =========================================================================
 * Minimal JSON helpers (no external dependency, suitable for ep3 output)
 * ======================================================================= */

/* Return value of first occurrence of "key":N (integer) */
static int64_t json_int(const std::string &s, const char *key)
{
    std::string pat = std::string("\"") + key + "\":";
    size_t p = s.find(pat);
    if (p == std::string::npos) return 0;
    p += pat.size();
    while (p < s.size() && s[p] == ' ') ++p;
    if (p >= s.size()) return 0;
    bool neg = (s[p] == '-');
    if (neg) ++p;
    int64_t v = 0;
    while (p < s.size() && isdigit((unsigned char)s[p]))
        v = v * 10 + (s[p++] - '0');
    return neg ? -v : v;
}

/* Return value of first occurrence of "key":"value" (string) */
static std::string json_str(const std::string &s, const char *key)
{
    std::string pat = std::string("\"") + key + "\":\"";
    size_t p = s.find(pat);
    if (p == std::string::npos) return "";
    p += pat.size();
    size_t e = s.find('"', p);
    if (e == std::string::npos) return "";
    return s.substr(p, e - p);
}

/* True when top-level key exists anywhere in line */
static bool json_has(const std::string &s, const char *key)
{
    return s.find(std::string("\"") + key + "\"") != std::string::npos;
}

/* Extract inner object/array string for a given key */
static std::string json_inner(const std::string &s, const char *key, char open, char close)
{
    std::string pat = std::string("\"") + key + "\":";
    size_t p = s.find(pat);
    if (p == std::string::npos) return "";
    p = s.find(open, p + pat.size());
    if (p == std::string::npos) return "";
    /* find matching close, handling nesting */
    int depth = 1;
    size_t q = p + 1;
    while (q < s.size() && depth > 0) {
        if (s[q] == open)  ++depth;
        if (s[q] == close) --depth;
        ++q;
    }
    return s.substr(p, q - p);
}

/* =========================================================================
 * eStaticServiceEp3Info
 * ======================================================================= */
DEFINE_REF(eStaticServiceEp3Info);

RESULT eStaticServiceEp3Info::getName(const eServiceReference &ref, std::string &name)
{
    if (!ref.name.empty()) { name = ref.name; return 0; }
    size_t sl = ref.path.rfind('/');
    name = (sl != std::string::npos) ? ref.path.substr(sl + 1) : ref.path;
    return 0;
}

int eStaticServiceEp3Info::getLength(const eServiceReference &) { return -1; }
int eStaticServiceEp3Info::getInfo(const eServiceReference &, int)
    { return iServiceInformation::resNA; }

long long eStaticServiceEp3Info::getFileSize(const eServiceReference &ref)
{
    struct stat st;
    return (::stat(ref.path.c_str(), &st) == 0) ? (long long)st.st_size : 0LL;
}

RESULT eStaticServiceEp3Info::getEvent(const eServiceReference &,
    ePtr<eServiceEvent> &evt, time_t)
    { evt = NULL; return -1; }

/* =========================================================================
 * eServiceEp3InfoContainer
 * ======================================================================= */
DEFINE_REF(eServiceEp3InfoContainer);

eServiceEp3InfoContainer::eServiceEp3InfoContainer()
    : m_double(0), m_buf(NULL), m_buf_size(0) {}

eServiceEp3InfoContainer::~eServiceEp3InfoContainer()
    { free(m_buf); }

double eServiceEp3InfoContainer::getDouble(unsigned int) const { return m_double; }
unsigned char *eServiceEp3InfoContainer::getBuffer(unsigned int &sz) const
    { sz = m_buf_size; return m_buf; }
void eServiceEp3InfoContainer::setDouble(double v) { m_double = v; }

/* =========================================================================
 * eServiceFactoryEp3
 * ======================================================================= */
DEFINE_REF(eServiceFactoryEp3);

eServiceFactoryEp3::eServiceFactoryEp3()
{
    ePtr<eServiceCenter> sc;
    eServiceCenter::getPrivInstance(sc);
    if (!sc) return;

    std::list<std::string> ext;
    static const char *exts[] = {
        "mp4","m4v","m4a","mkv","avi","mov","wmv","flv","ts","m2ts","mts",
        "mpg","mpeg","vob","iso","mp3","aac","flac","ogg","wav","wma","ac3",
        "dts","m3u","m3u8","pls","divx","xvid","webm","ogv","3gp","3g2",
        "rmvb","rm","asf","dat","trp","tp","rec","stream",NULL
    };
    for (int i = 0; exts[i]; ++i) ext.push_back(exts[i]);

    sc->removeServiceFactory(0x1001); /* remove servicemp3 first — same pattern as servicehisilicon */
    sc->addServiceFactory(eServiceFactoryEp3::id, this, ext);
    m_service_info = new eStaticServiceEp3Info();
}

eServiceFactoryEp3::~eServiceFactoryEp3()
{
    ePtr<eServiceCenter> sc;
    eServiceCenter::getPrivInstance(sc);
    if (sc) sc->removeServiceFactory(eServiceFactoryEp3::id);
}

RESULT eServiceFactoryEp3::play(const eServiceReference &ref,
    ePtr<iPlayableService> &ptr)
    { ptr = new eServiceEp3(ref); return 0; }

RESULT eServiceFactoryEp3::record(const eServiceReference &ref,
    ePtr<iRecordableService> &ptr)
    { ptr = new eServiceEp3(ref); return 0; }

RESULT eServiceFactoryEp3::list(const eServiceReference &,
    ePtr<iListableService> &ptr)
    { ptr = NULL; return -1; }

RESULT eServiceFactoryEp3::info(const eServiceReference &,
    ePtr<iStaticServiceInformation> &ptr)
    { ptr = m_service_info; return 0; }

RESULT eServiceFactoryEp3::offlineOperations(const eServiceReference &,
    ePtr<iServiceOfflineOperations> &ptr)
    { ptr = NULL; return -1; }

/* =========================================================================
 * eServiceEp3  —  constructor / destructor
 * ======================================================================= */
DEFINE_REF(eServiceEp3);

eServiceEp3::eServiceEp3(eServiceReference ref)
    : m_state(stIdle)
    , m_ref(ref)
    , m_useragent("Enigma2 HbbTV/1.4.2 (+PVR+RTSP+DL;OpenPLi;;;)")
    , m_current_audio_idx(-1)
    , m_current_sub_idx(-1)
    , m_cached_sub_idx(-1)
    , m_last_position(0)
    , m_duration(0)
    , m_seekable(true)
    , m_is_live(false)
    , m_error_code(0)
    , m_last_poll_ms(0)
    , m_subtitle_user(NULL)    , m_player_pid(-1)
    , m_stdin_fd(-1)
    , m_stderr_fd(-1)
{
    m_video_info = sEp3VideoInfo{};

    /* Parse extra headers / User-Agent from URI fragment (#key=val&...) */
    const std::string &path = m_ref.path;
    bool isNet = (path.compare(0,4,"http")==0 || path.compare(0,4,"rtsp")==0);
    if (isNet) {
        size_t h = path.find('#');
        if (h != std::string::npos) {
            m_extra_headers = path.substr(h + 1);
            size_t ua = m_extra_headers.find("User-Agent=");
            if (ua != std::string::npos) {
                size_t s = ua + 11;
                size_t e = m_extra_headers.find('&', s);
                m_useragent = (e != std::string::npos)
                    ? m_extra_headers.substr(s, e - s)
                    : m_extra_headers.substr(s);
            }
        }
    }

    /* Optional config override */
    if (eConfigManager::getConfigBoolValue(
            "config.mediaplayer.useAlternateUserAgent"))
        m_useragent = eConfigManager::getConfigValue(
            "config.mediaplayer.alternateUserAgent");

    m_nownext_timer = eTimer::create(eApp);
    CONNECT(m_nownext_timer->timeout, eServiceEp3::updateEpgCacheNowNext);

    /* Launch player immediately in constructor — like serviceapp starts
     * exteplayer3 in ExtEplayer3::ExtEplayer3(). This way the player is
     * already running when start()/evStart are called. */
    if (!launchPlayer()) {
        m_state = stError;
        eDebug("[serviceexteplayer3] launchPlayer failed in constructor");
    }

    eDebug("[serviceexteplayer3] created: %s", m_ref.path.c_str());
}

eServiceEp3::~eServiceEp3()
{
    stop();
    m_nownext_timer->stop();
    eDebug("[serviceexteplayer3] destroyed");
}

/* =========================================================================
 * Player process management
 * ======================================================================= */
bool eServiceEp3::launchPlayer()
{
    /* Hand over the HW video decoder to exteplayer3.
     *
     * E2's DVB service leaves the decoder in VIDEO_SOURCE_DEMUX mode with
     * vpid=-1 ("clean handover"). On BCM7xxx that means the HW decoder is
     * still owned by the DVB pipeline — exteplayer3 cannot acquire it and
     * the mainloop spins waiting for a decoder event that never comes.
     *
     * Neutrino solves this by switching VIDEO_SELECT_SOURCE to
     * VIDEO_SOURCE_MEMORY before starting the external player.  We do the
     * same here: switch the source so the DVB demux pipeline releases the
     * decoder, then call VIDEO_PLAY + VIDEO_CONTINUE so the hardware is
     * in a defined "running from memory" state that exteplayer3 expects.
     */
    {
        int vfd = ::open("/dev/dvb/adapter0/video0", O_RDWR | O_NONBLOCK);
        if (vfd >= 0) {
            ::ioctl(vfd, VIDEO_SELECT_SOURCE, VIDEO_SOURCE_MEMORY);
            ::ioctl(vfd, VIDEO_PLAY);
            ::ioctl(vfd, VIDEO_CONTINUE);
            ::close(vfd);
            eDebug("[serviceexteplayer3] launchPlayer: VIDEO_SOURCE_MEMORY handover done");
        } else {
            eDebug("[serviceexteplayer3] launchPlayer: cannot open video0: %s", strerror(errno));
        }

        /* Same for audio — release audio decoder from DVB pipeline */
        int afd = ::open("/dev/dvb/adapter0/audio0", O_RDWR | O_NONBLOCK);
        if (afd >= 0) {
            ::ioctl(afd, AUDIO_SELECT_SOURCE, AUDIO_SOURCE_MEMORY);
            ::ioctl(afd, AUDIO_PLAY);
            ::close(afd);
            eDebug("[serviceexteplayer3] launchPlayer: AUDIO_SOURCE_MEMORY handover done");
        }
    }

    int stdin_pipe[2], stderr_pipe[2];
    if (pipe(stdin_pipe) < 0 || pipe(stderr_pipe) < 0) {
        eDebug("[serviceexteplayer3] pipe() failed: %s", strerror(errno));
        return false;
    }

    m_player_pid = fork();
    if (m_player_pid < 0) {
        eDebug("[serviceexteplayer3] fork() failed: %s", strerror(errno));
        close(stdin_pipe[0]);  close(stdin_pipe[1]);
        close(stderr_pipe[0]); close(stderr_pipe[1]);
        return false;
    }

    if (m_player_pid == 0) {
        /* ---- child ---- */
        /* Own process group so kill(-pid) kills all children too */
        setsid();

        dup2(stdin_pipe[0],  STDIN_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);

        /* Close all pipe ends in child — only keep the duped ones */
        close(stdin_pipe[0]);  close(stdin_pipe[1]);
        close(stderr_pipe[0]); close(stderr_pipe[1]);

        /* Close all other inherited fds (E2 has many open sockets/pipes)
         * so none of them accidentally keep parent's pipe ends alive */
        for (int i = 3; i < 256; ++i)
            close(i);

        /* Build argument vector — safe to use stack after setsid/close loop */
        char uri_buf[4096];
        char ua_buf[512];
        char hdr_buf[2048];

        strncpy(uri_buf, buildUri().c_str(), sizeof(uri_buf)-1);
        strncpy(ua_buf,  m_useragent.c_str(), sizeof(ua_buf)-1);

        const char *argv[24];
        int ac = 0;
        argv[ac++] = EXTEPLAYER3_BIN;

        if (!m_useragent.empty()) {
            argv[ac++] = "-u";
            argv[ac++] = ua_buf;
        }

        /* Pass raw fragment as HTTP headers string if present */
        if (!m_extra_headers.empty()) {
            strncpy(hdr_buf, m_extra_headers.c_str(), sizeof(hdr_buf)-1);
            argv[ac++] = "-h";
            argv[ac++] = hdr_buf;
        }

        /* Live-TS mode for network streams without a known duration */
        if (m_is_live) argv[ac++] = "-v";

        argv[ac++] = uri_buf;
        argv[ac]   = NULL;

        execvp(EXTEPLAYER3_BIN, (char * const *)argv);
        _exit(127);
    }

    /* ---- parent ---- */
    close(stdin_pipe[0]);
    close(stderr_pipe[1]);

    m_stdin_fd  = stdin_pipe[1];
    m_stderr_fd = stderr_pipe[0];

    /* Non-blocking read on stderr — watch both data and hangup */
    int flags = fcntl(m_stderr_fd, F_GETFL, 0);
    fcntl(m_stderr_fd, F_SETFL, flags | O_NONBLOCK);

    m_sn_read = eSocketNotifier::create(eApp, m_stderr_fd,
        eSocketNotifier::Read | eSocketNotifier::Priority | eSocketNotifier::Hungup);
    CONNECT(m_sn_read->activated, eServiceEp3::onStderrData);

    m_state = stRunning;
    eDebug("[serviceexteplayer3] exteplayer3 pid=%d", (int)m_player_pid);
    return true;
}

void eServiceEp3::stopPlayer()
{
    if (m_player_pid <= 0) return;

    pid_t pid = m_player_pid;
    m_player_pid = -1;

    /* Detach notifier and close pipes first so no more callbacks fire */
    m_sn_read = NULL;
    if (m_stdin_fd  >= 0) { close(m_stdin_fd);  m_stdin_fd  = -1; }
    if (m_stderr_fd >= 0) { close(m_stderr_fd); m_stderr_fd = -1; }

    /* Send polite stop via terminate socket (non-blocking connect) */
    int sock = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock >= 0) {
        /* Set non-blocking so connect() never hangs */
        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, "/tmp/.exteplayerterm.socket",
                sizeof(addr.sun_path)-1);
        ::connect(sock, (struct sockaddr*)&addr, sizeof(addr));
        ::close(sock);
    }

    /* SIGTERM to whole process group first, then SIGKILL */
    kill(-pid, SIGTERM);

    int status;
    if (waitpid(pid, &status, WNOHANG) == 0) {
        kill(-pid, SIGKILL);
        waitpid(pid, &status, WNOHANG);
    }

    eDebug("[serviceexteplayer3] stopPlayer pid=%d done", (int)pid);

    /* Restore OSD / Enigma2 GUI layer after exteplayer3.
     *
     * On BCM7xxx the hardware video layer sits on top of the OSD.
     * exteplayer3 activates it via VIDEO_PLAY but never calls VIDEO_SET_BLANK
     * on exit, so the last video frame stays visible and blocks the E2 GUI.
     *
     * We replicate what LinuxDvbStop() does plus add VIDEO_SET_BLANK so the
     * hardware compositor hands control back to the framebuffer/OSD layer.
     */
    int vfd = open("/dev/dvb/adapter0/video0", O_RDWR | O_NONBLOCK);
    if (vfd >= 0) {
        ioctl(vfd, VIDEO_SELECT_SOURCE, VIDEO_SOURCE_DEMUX);
        ioctl(vfd, VIDEO_STOP);
        /* Blank = 1: show still picture / hand layer back to OSD */
        int blank = 1;
        ioctl(vfd, VIDEO_SET_BLANK, &blank);
        close(vfd);
        eDebug("[serviceexteplayer3] OSD restore: VIDEO_STOP + VIDEO_SET_BLANK done");
    } else {
        eDebug("[serviceexteplayer3] OSD restore: cannot open video0: %s", strerror(errno));
    }
}

void eServiceEp3::sendCmd(const char *cmd)
{
    if (m_stdin_fd < 0) return;
    ssize_t r = write(m_stdin_fd, cmd, strlen(cmd));
    (void)r;
    eDebug("[serviceexteplayer3] → %s", cmd);
}

std::string eServiceEp3::buildUri() const
{
    std::string path = m_ref.path;
    /* Strip fragment from network URIs */
    if (path.compare(0,4,"http")==0 || path.compare(0,4,"rtsp")==0) {
        size_t h = path.find('#');
        if (h != std::string::npos) path = path.substr(0, h);
    }
    return path;
}

/* =========================================================================
 * stderr pipe → JSON line dispatcher
 * ======================================================================= */
void eServiceEp3::onStderrData(int fd)
{
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf)-1);
    if (n <= 0) {
        if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
            /* Real EOF or error (not just "no data yet") —
             * exteplayer3 has exited. Deregister notifier FIRST
             * so the mainloop stops polling this fd (fixes GUI hang
             * on old kernels that signal POLLHUP without POLLIN). */
            eDebug("[serviceexteplayer3] stderr EOF/error (errno=%d) — player exited", errno);
            m_sn_read = NULL;
            if (m_state == stRunning || m_state == stPaused) {
                m_state = stStopped;
                m_event((iPlayableService*)this, iPlayableService::evStopped);
                m_event((iPlayableService*)this, iPlayableService::evEOF);
            }
        }
        return;
    }
    buf[n] = '\0';
    m_read_buf += buf;

    size_t pos;
    while ((pos = m_read_buf.find('\n')) != std::string::npos) {
        std::string line = m_read_buf.substr(0, pos);
        m_read_buf.erase(0, pos + 1);
        if (!line.empty())
            processLine(line);
    }
}

void eServiceEp3::processLine(const std::string &line)
{
    eDebug("[serviceexteplayer3] ← %s", line.c_str());

    /* Dispatch on first unique key in line.
     * Keys verified against real exteplayer3 stderr output:
     *   {"v_c":{...}}  {"a_l":[...]}  {"a_c":{...}}
     *   {"s_l":[...]}  {"s_c":{...}}  {"s_a":{...}}  {"s_f":{...}}
     *   {"v_e":{...}}  (spontaneous video info from linuxdvb_mipsel)
     */
    if (json_has(line, "EPLAYER3_EXTENDED"))  { onEplayer3Extended(line); return; }
    if (json_has(line, "PLAYBACK_OPEN"))      { onPlaybackOpen(line);     return; }
    if (json_has(line, "PLAYBACK_PLAY"))      { onPlaybackPlay(line);     return; }
    if (json_has(line, "PLAYBACK_STOP"))      { onPlaybackStop(line);     return; }
    if (json_has(line, "PLAYBACK_PAUSE"))     { onPlaybackPause(line);    return; }
    if (json_has(line, "PLAYBACK_CONTINUE"))  { onPlaybackContinue(line); return; }
    if (json_has(line, "PLAYBACK_LENGTH"))    { onPlaybackLength(line);   return; }
    if (json_has(line, "\"J\""))              { onPositionUpdate(line);   return; }
    if (json_has(line, "\"a_l\""))            { onAudioList(line);        return; }
    if (json_has(line, "\"a_c\""))            { onAudioCurrent(line);     return; }
    if (json_has(line, "\"s_l\""))            { onSubtitleList(line);     return; }
    if (json_has(line, "\"s_c\""))            { onSubtitleCurrent(line);  return; }
    if (json_has(line, "\"s_a\""))            { onSubtitleData(line);     return; }
    if (json_has(line, "\"s_f\""))            { onSubtitleFlush(line);    return; }
    if (json_has(line, "\"v_c\""))            { onVideoInfoVc(line);      return; }
    if (json_has(line, "\"v_e\""))            { onVideoInfoVe(line);      return; }
    if (json_has(line, "FF_ERROR"))           { onFfError(line);          return; }
    /* PLAYBACK_FASTFORWARD / PLAYBACK_FASTBACKWARD / PLAYBACK_SEEK* ignored */
}

/* =========================================================================
 * JSON event handlers
 * ======================================================================= */
void eServiceEp3::onEplayer3Extended(const std::string &)
{
    /* Player started and ready — request initial track/length info */
    /* (actual info arrives after PLAYBACK_PLAY) */
}

void eServiceEp3::onPlaybackOpen(const std::string &line)
{
    if (json_int(line, "sts") < 0) {
        eDebug("[serviceexteplayer3] PLAYBACK_OPEN failed");
        m_state = stError;
        m_event((iPlayableService*)this, iPlayableService::evUser + 12);
    }
}

void eServiceEp3::onPlaybackPlay(const std::string &line)
{
    if (json_int(line, "sts") == 0) {
        m_state = stRunning;
        /* evStart already fired in start() — just signal updated info */
        m_event((iPlayableService*)this, iPlayableService::evUpdatedInfo);
        m_nownext_timer->startLongTimer(3);
        /* Request track lists and initial length */
        sendCmd("al\n");
        sendCmd("sl\n");
        sendCmd("vc\n");
        sendCmd("l\n");
    } else {
        m_state = stError;
        m_event((iPlayableService*)this, iPlayableService::evUser + 12);
    }
}

void eServiceEp3::onPlaybackStop(const std::string &)
{
    m_state = stStopped;
    m_event((iPlayableService*)this, iPlayableService::evEOF);
}

void eServiceEp3::onPlaybackPause(const std::string &)
{
    m_state = stPaused;
    m_event((iPlayableService*)this, iPlayableService::evUpdatedInfo);
}

void eServiceEp3::onPlaybackContinue(const std::string &)
{
    m_state = stRunning;
    m_event((iPlayableService*)this, iPlayableService::evUpdatedInfo);
}

void eServiceEp3::onPlaybackLength(const std::string &line)
{
    int64_t len = json_int(line, "length");
    if (len > 0) {
        m_duration = secToPts(len);
        m_seekable = true;
    }
}

void eServiceEp3::onPositionUpdate(const std::string &line)
{
    /* {"J":{"ms":N}}  or  {"J":{"ms":N,"lms":N}} */
    std::string inner = json_inner(line, "J", '{', '}');
    if (inner.empty()) return;
    int64_t ms  = json_int(inner, "ms");
    int64_t lms = json_int(inner, "lms");
    m_last_position = msToPts(ms);
    if (lms > 0) m_duration = msToPts(lms);
}

void eServiceEp3::onAudioList(const std::string &line)
{
    std::string arr = json_inner(line, "a_l", '[', ']');
    if (!arr.empty()) {
        parseAudioList(arr);
        m_event((iPlayableService*)this, iPlayableService::evUpdatedInfo);
    }
}

void eServiceEp3::onAudioCurrent(const std::string &line)
{
    /* {"a_c":{"id":N,...}} — find matching index in m_audio_tracks */
    std::string inner = json_inner(line, "a_c", '{', '}');
    if (inner.empty()) return;
    int tid = (int)json_int(inner, "id");
    for (int i = 0; i < (int)m_audio_tracks.size(); ++i) {
        if (m_audio_tracks[i].id == tid) { m_current_audio_idx = i; break; }
    }
}

void eServiceEp3::onSubtitleList(const std::string &line)
{
    std::string arr = json_inner(line, "s_l", '[', ']');
    if (!arr.empty()) {
        parseSubtitleList(arr);
        m_event((iPlayableService*)this, iPlayableService::evUpdatedInfo);
    }
}

void eServiceEp3::onSubtitleCurrent(const std::string &line)
{
    /* {"s_c":{"id":N,"e":"ENC","n":"Name"}} — currently active subtitle track */
    std::string inner = json_inner(line, "s_c", '{', '}');
    if (inner.empty()) return;
    int tid = (int)json_int(inner, "id");
    m_current_sub_idx = -1;
    for (int i = 0; i < (int)m_sub_tracks.size(); ++i) {
        if (m_sub_tracks[i].id == tid) { m_current_sub_idx = i; break; }
    }
}

void eServiceEp3::onSubtitleData(const std::string &line)
{
    /* {"s_a":{"id":N,"s":MS_start,"e":MS_end,"t":"text"}} */
    if (!m_subtitle_user || m_current_sub_idx < 0) return;

    std::string inner = json_inner(line, "s_a", '{', '}');
    if (inner.empty()) return;

    std::string text    = json_str(inner, "t");
    int64_t     startMs = json_int(inner, "s");
    int64_t     endMs   = json_int(inner, "e");

    if (text.empty()) return;

    ePangoSubtitlePage page;
    gRGB color(0xD0, 0xD0, 0xD0);
    page.m_elements.push_back(ePangoSubtitlePageElement(color, text.c_str()));
    page.m_show_pts = msToPts(startMs);
    page.m_timeout  = (int)(endMs - startMs);
    if (page.m_timeout <= 0) page.m_timeout = 5000;
    m_subtitle_user->setPage(page);
}

void eServiceEp3::onSubtitleFlush(const std::string &)
{
    /* {"s_f":{"r":0}} — subtitle track reset, clear display */
    if (m_subtitle_user) {
        ePangoSubtitlePage page;
        m_subtitle_user->setPage(page);
    }
}

void eServiceEp3::onVideoInfoVc(const std::string &line)
{
    /* {"v_c":{"id":N,"e":"V_MPEG4/ISO/AVC","n":"und","w":W,"h":H,"f":F,"p":P,"an":A,"ad":D}} */
    std::string inner = json_inner(line, "v_c", '{', '}');
    if (inner.empty()) return;

    m_video_info.encoding    = json_str(inner, "e");
    m_video_info.width       = (int)json_int(inner, "w");
    m_video_info.height      = (int)json_int(inner, "h");
    m_video_info.framerate   = (int)json_int(inner, "f");
    m_video_info.progressive = (int)json_int(inner, "p");
    m_video_info.aspect_num  = (int)json_int(inner, "an");
    m_video_info.aspect_den  = (int)json_int(inner, "ad");

    eDebug("[serviceexteplayer3] vc: %s %dx%d fps=%d progressive=%d",
        m_video_info.encoding.c_str(),
        m_video_info.width, m_video_info.height,
        m_video_info.framerate, m_video_info.progressive);

    m_event((iPlayableService*)this, iPlayableService::evVideoSizeChanged);
}

void eServiceEp3::onVideoInfoVe(const std::string &line)
{
    /* {"v_e":{"w":W,"h":H,"a":A,"f":F,"p":P}} — spontaneous from linuxdvb_mipsel
     * Only update fields that v_e provides; keep encoding from vc */
    std::string inner = json_inner(line, "v_e", '{', '}');
    if (inner.empty()) return;

    int w = (int)json_int(inner, "w");
    int h = (int)json_int(inner, "h");
    int f = (int)json_int(inner, "f");
    int p = (int)json_int(inner, "p");
    int a = (int)json_int(inner, "a");

    bool changed = (w != m_video_info.width || h != m_video_info.height);

    if (w > 0) m_video_info.width       = w;
    if (h > 0) m_video_info.height      = h;
    if (f > 0) m_video_info.framerate   = f;
    m_video_info.progressive  = p;
    m_video_info.aspect_ratio = a;

    eDebug("[serviceexteplayer3] v_e: %dx%d fps=%d aspect=%d progressive=%d",
        m_video_info.width, m_video_info.height,
        m_video_info.framerate, a, p);

    if (changed)
        m_event((iPlayableService*)this, iPlayableService::evVideoSizeChanged);
}

void eServiceEp3::onFfError(const std::string &line)
{
    /* {"FF_ERROR":{"msg":"No such file or directory","code":-2}} */
    std::string inner = json_inner(line, "FF_ERROR", '{', '}');
    std::string msg   = json_str(inner, "msg");
    int         code  = (int)json_int(inner, "code");
    eDebug("[serviceexteplayer3] FF_ERROR code=%d msg=%s", code, msg.c_str());
    m_error_code = code ? code : -1;
    m_state = stError;
    m_event((iPlayableService*)this, iPlayableService::evUser + 12);
}

/* =========================================================================
 * Track parsers
 * ======================================================================= */
void eServiceEp3::parseAudioList(const std::string &arr)
{
    m_audio_tracks.clear();
    size_t pos = 0;
    while ((pos = arr.find('{', pos)) != std::string::npos) {
        size_t e = arr.find('}', pos);
        if (e == std::string::npos) break;
        std::string item = arr.substr(pos, e - pos + 1);
        sEp3AudioTrack t;
        t.id       = (int)json_int(item, "id");
        t.encoding = json_str(item, "e");
        t.name     = json_str(item, "n");
        m_audio_tracks.push_back(t);
        pos = e + 1;
    }
    eDebug("[serviceexteplayer3] audio tracks: %zu", m_audio_tracks.size());
}

void eServiceEp3::parseSubtitleList(const std::string &arr)
{
    m_sub_tracks.clear();
    size_t pos = 0;
    while ((pos = arr.find('{', pos)) != std::string::npos) {
        size_t e = arr.find('}', pos);
        if (e == std::string::npos) break;
        std::string item = arr.substr(pos, e - pos + 1);
        sEp3SubtitleTrack t;
        t.id       = (int)json_int(item, "id");
        t.encoding = json_str(item, "e");
        t.name     = json_str(item, "n");
        m_sub_tracks.push_back(t);
        pos = e + 1;
    }
    eDebug("[serviceexteplayer3] subtitle tracks: %zu", m_sub_tracks.size());
}

/* =========================================================================
 * iPlayableService
 * ======================================================================= */
RESULT eServiceEp3::connectEvent(
    const sigc::slot<void(iPlayableService*,int)> &event,
    ePtr<eConnection> &connection)
{
    connection = new eConnection((iPlayableService*)this, m_event.connect(event));
    return 0;
}

RESULT eServiceEp3::start()
{
    if (m_state == stError) return -1;
    if (m_state != stIdle) return -1;

    /* Fire evStart — player already launched in constructor like serviceapp.
     * ServiceEventTracker stack is correct at this point. */
    m_event((iPlayableService*)this, iPlayableService::evUpdatedEventInfo);
    m_event((iPlayableService*)this, iPlayableService::evStart);
    return 0;
}

RESULT eServiceEp3::stop()
{
    if (m_state == stIdle || m_state == stStopped) return 0;
    eDebug("[serviceexteplayer3] stop()");
    m_state = stStopped;
    /* Fire evStopped BEFORE killing player so mediaplayer can proceed
     * with UI cleanup immediately without waiting for process exit */
    m_event((iPlayableService*)this, iPlayableService::evStopped);
    stopPlayer();
    return 0;
}

RESULT eServiceEp3::pause(ePtr<iPauseableService> &ptr) { ptr = this; return 0; }
RESULT eServiceEp3::seek(ePtr<iSeekableService> &ptr)   { ptr = this; return 0; }
RESULT eServiceEp3::audioTracks(ePtr<iAudioTrackSelection> &ptr) { ptr=this; return 0; }
RESULT eServiceEp3::subtitle(ePtr<iSubtitleOutput> &ptr)         { ptr=this; return 0; }
RESULT eServiceEp3::info(ePtr<iServiceInformation> &ptr)         { ptr=this; return 0; }

RESULT eServiceEp3::setSlowMotion(int ratio)
{
    char cmd[32]; snprintf(cmd, sizeof(cmd), "m%d\n", ratio);
    sendCmd(cmd); return 0;
}

RESULT eServiceEp3::setFastForward(int ratio)
{
    if (ratio < 0)  { char cmd[32]; snprintf(cmd,sizeof(cmd),"b%d\n",-ratio); sendCmd(cmd); }
    else if (ratio > 1) { char cmd[32]; snprintf(cmd,sizeof(cmd),"f%d\n",ratio); sendCmd(cmd); }
    else sendCmd("c\n");
    return 0;
}

/* scarthgap mandatory stubs */
RESULT eServiceEp3::setTarget(int, bool)                          { return -1; }
RESULT eServiceEp3::audioChannel(ePtr<iAudioChannelSelection> &p) { p=0; return -1; }
RESULT eServiceEp3::timeshift(ePtr<iTimeshiftService> &p)         { p=0; return -1; }
RESULT eServiceEp3::tap(ePtr<iTapService> &p)                     { p=0; return -1; }
RESULT eServiceEp3::cueSheet(ePtr<iCueSheet> &p)                  { p=0; return -1; }
RESULT eServiceEp3::audioDelay(ePtr<iAudioDelay> &p)              { p=0; return -1; }
RESULT eServiceEp3::rdsDecoder(ePtr<iRdsDecoder> &p)              { p=0; return -1; }
RESULT eServiceEp3::stream(ePtr<iStreamableService> &p)           { p=0; return -1; }
RESULT eServiceEp3::streamed(ePtr<iStreamedService> &p)           { p=0; return -1; }
RESULT eServiceEp3::keys(ePtr<iServiceKeys> &p)                   { p=0; return -1; }
void   eServiceEp3::setQpipMode(bool, bool)                       {}

/* =========================================================================
 * iPauseableService
 * ======================================================================= */
RESULT eServiceEp3::pause()
{
    if (m_state == stRunning) {
        m_state = stPaused;
        sendCmd("p\n");
    }
    return 0;
}

RESULT eServiceEp3::unpause()
{
    /* Only send continue command after exteplayer3 has confirmed
     * PLAYBACK_PLAY. Before that the player is still initialising
     * and sending c\n too early puts it in an undefined state which
     * causes the mainloop to spin waiting for an event that never comes. */
    if (m_state == stPaused) {
        m_state = stRunning;
        sendCmd("c\n");
    }
    return 0;
}

/* =========================================================================
 * iSeekableService
 * ======================================================================= */
RESULT eServiceEp3::getLength(pts_t &len)
{
    len = m_duration;
    if (m_duration == 0 && m_state == stRunning)
        sendCmd("l\n");  /* async refresh */
    return 0;
}

RESULT eServiceEp3::seekTo(pts_t to)
{
    /* gf = force-seek absolute */
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "gf%lld\n", (long long)ptsToSec(to));
    sendCmd(cmd);
    return 0;
}

RESULT eServiceEp3::seekRelative(int direction, pts_t to)
{
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "kf%lld\n",
             (long long)(ptsToSec(to) * direction));
    sendCmd(cmd);
    return 0;
}

RESULT eServiceEp3::getPlayPosition(pts_t &pos)
{
    pos = m_last_position;
    if (m_state == stRunning) {
        /* Rate-limit position polls to max 1 per second.
         * Without this, E2 calls getPlayPosition() so frequently that
         * the j\n -> J response loop floods the mainloop and causes
         * the spinner / unresponsive UI. */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t now_ms = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
        if (now_ms - m_last_poll_ms >= 1000) {
            m_last_poll_ms = now_ms;
            sendCmd("j\n");
        }
    }
    return 0;
}

RESULT eServiceEp3::setTrickmode(int trick)  { return setFastForward(trick); }

RESULT eServiceEp3::isCurrentlySeekable()
{
    /* 1=fwd seekable, 2=bwd seekable, 3=both */
    return m_seekable ? 3 : 0;
}

/* =========================================================================
 * iAudioTrackSelection
 * ======================================================================= */
int eServiceEp3::getNumberOfTracks()    { return (int)m_audio_tracks.size(); }
int eServiceEp3::getCurrentTrack()      { return m_current_audio_idx; }

RESULT eServiceEp3::selectTrack(unsigned int i)
{
    if (i >= m_audio_tracks.size()) return -1;
    char cmd[32]; snprintf(cmd, sizeof(cmd), "ai%u\n", i);
    sendCmd(cmd);
    m_current_audio_idx = (int)i;
    return 0;
}

RESULT eServiceEp3::getTrackInfo(struct iAudioTrackInfo &info, unsigned int n)
{
    if (n >= m_audio_tracks.size()) return -1;
    const sEp3AudioTrack &t = m_audio_tracks[n];
    info.m_language    = t.name.empty() ? t.encoding : t.name;
    info.m_description = t.encoding;
    return 0;
}

/* =========================================================================
 * iSubtitleOutput
 * ======================================================================= */
RESULT eServiceEp3::enableSubtitles(iSubtitleUser *user, SubtitleTrack &track)
{
    disableSubtitles();
    m_subtitle_user = user;

    /* Match track by language_code or pid (= ep3 track id) */
    for (int i = 0; i < (int)m_sub_tracks.size(); ++i) {
        const sEp3SubtitleTrack &t = m_sub_tracks[i];
        if (t.name == track.language_code || t.id == track.pid) {
            m_current_sub_idx = i;
            m_cached_sub_idx  = i;
            char cmd[32]; snprintf(cmd, sizeof(cmd), "si%d\n", i);
            sendCmd(cmd);
            return 0;
        }
    }
    return -1;
}

RESULT eServiceEp3::disableSubtitles()
{
    m_subtitle_user = NULL;
    if (m_current_sub_idx >= 0) {
        sendCmd("si-1\n");
        m_current_sub_idx = -1;
    }
    return 0;
}

RESULT eServiceEp3::getCachedSubtitle(SubtitleTrack &track)
{
    if (m_cached_sub_idx < 0 ||
        m_cached_sub_idx >= (int)m_sub_tracks.size()) return -1;
    const sEp3SubtitleTrack &t = m_sub_tracks[m_cached_sub_idx];
    track.language_code = t.name.empty() ? t.encoding : t.name;
    track.pid           = t.id;
    track.type          = 0;
    return 0;
}

RESULT eServiceEp3::getSubtitleList(std::vector<SubtitleTrack> &list)
{
    list.clear();
    for (const auto &t : m_sub_tracks) {
        SubtitleTrack st;
        st.language_code = t.name.empty() ? t.encoding : t.name;
        st.pid           = t.id;
        st.type          = 0;
        list.push_back(st);
    }
    return 0;
}

/* =========================================================================
 * iServiceInformation
 * ======================================================================= */
RESULT eServiceEp3::getName(std::string &name)
{
    if (!m_ref.name.empty()) { name = m_ref.name; return 0; }
    size_t sl = m_ref.path.rfind('/');
    name = (sl != std::string::npos) ? m_ref.path.substr(sl+1) : m_ref.path;
    return 0;
}

int eServiceEp3::getInfo(int w)
{
    switch (w) {
    case iServiceInformation::sVideoWidth:
        return m_video_info.width  > 0 ? m_video_info.width  : iServiceInformation::resNA;
    case iServiceInformation::sVideoHeight:
        return m_video_info.height > 0 ? m_video_info.height : iServiceInformation::resNA;
    case iServiceInformation::sFrameRate:
        return m_video_info.framerate > 0 ? m_video_info.framerate : iServiceInformation::resNA;
    case iServiceInformation::sProgressive:
        return m_video_info.progressive;
    case iServiceInformation::sAspect: {
        /* Prefer raw DVB aspect value from v_e if available */
        if (m_video_info.aspect_ratio > 0) return m_video_info.aspect_ratio;
        /* Fall back to aspect_num/den from vc */
        if (m_video_info.aspect_den > 0) {
            float r = (float)m_video_info.aspect_num / m_video_info.aspect_den;
            if (r > 2.0f)  return 3;  /* 2.21:1 */
            if (r > 1.5f)  return 2;  /* 16:9   */
            return 1;                  /* 4:3    */
        }
        return iServiceInformation::resNA;
    }
    case iServiceInformation::sCurrentTitle:
        return m_current_audio_idx;
    case iServiceInformation::sTotalTitles:
        return (int)m_audio_tracks.size();
    }
    return iServiceInformation::resNA;
}

std::string eServiceEp3::getInfoString(int w)
{
    switch (w) {
    case iServiceInformation::sServiceref:
        return m_ref.toString();
    default:
        /* Audio codec: sUser+30 */
        if (w == iServiceInformation::sUser + 30 &&
            m_current_audio_idx >= 0 &&
            m_current_audio_idx < (int)m_audio_tracks.size())
            return m_audio_tracks[m_current_audio_idx].encoding;
        /* Video codec: sUser+31 */
        if (w == iServiceInformation::sUser + 31)
            return m_video_info.encoding;
    }
    return "";
}

ePtr<iServiceInfoContainer> eServiceEp3::getInfoObject(int) { return NULL; }

/* =========================================================================
 * iRecordableService  (stubs — recording not supported via exteplayer3)
 * ======================================================================= */
RESULT eServiceEp3::connectEvent(
    const sigc::slot<void(iRecordableService*,int)> &event,
    ePtr<eConnection> &connection)
{
    connection = new eConnection((iRecordableService*)this,
                                 m_rec_event.connect(event));
    return 0;
}

RESULT eServiceEp3::getError(int &error)              { error = m_error_code; return 0; }
RESULT eServiceEp3::subServices(ePtr<iSubserviceList> &p)  { p=0; return -1; }
RESULT eServiceEp3::frontendInfo(ePtr<iFrontendInformation> &p) { p=0; return -1; }
RESULT eServiceEp3::prepare(const char*,time_t,time_t,int,
    const char*,const char*,const char*,bool,bool,int)   { return -1; }
RESULT eServiceEp3::prepareStreaming(bool,bool)        { return -1; }
RESULT eServiceEp3::start(bool simulate)              { return simulate ? 0 : -1; }
RESULT eServiceEp3::record(ePtr<iStreamableService> &p){ p=0; return -1; }
RESULT eServiceEp3::getFilenameExtension(std::string &ext) { ext="ts"; return 0; }
RESULT eServiceEp3::stopRecord()                      { return -1; }

/* =========================================================================
 * EPG  NowNext
 * ======================================================================= */
void eServiceEp3::updateEpgCacheNowNext()
{
    bool update = false;
    time_t now  = eDVBLocalTimeHandler::getInstance()->nowTime();
    ePtr<eServiceEvent> cur, nxt;

    if (eEPGCache::getInstance()->lookupEventTime(m_ref, now, cur) >= 0) {
        if (!m_event_now ||
            m_event_now->getEventId() != cur->getEventId()) {
            m_event_now = cur;
            update = true;
        }
    }
    if (eEPGCache::getInstance()->lookupEventTime(m_ref, now, nxt, 1) >= 0) {
        if (!m_event_next ||
            m_event_next->getEventId() != nxt->getEventId()) {
            m_event_next = nxt;
            update = true;
        }
    }

    int refresh = 60;
    if (m_event_next) {
        int r = (int)(m_event_next->getBeginTime() - now) + 3;
        if (r > 0) refresh = r;
    }
    m_nownext_timer->startLongTimer(refresh);

    if (update)
        m_event((iPlayableService*)this,
                iPlayableService::evUpdatedEventInfo);
}

/* =========================================================================
 * Module init
 * ======================================================================= */
eAutoInitPtr<eServiceFactoryEp3> init_eServiceFactoryEp3(
    eAutoInitNumbers::service + 1, "eServiceFactoryEp3");
