/*
 * ffmpeg-player/main.cpp
 *
 * Korrigierte Version mit integriertem codec-spezifischem PES-Wrapping
 * entsprechend der exteplayer3/mipsel Writer-Logik.
 *
 * Kernproblem (warum Player startete aber nichts abspielte):
 *   Die bisherige simple PES-Konstruktion war falsch. Der BCM-Treiber
 *   erwartet für jeden Codec spezifisch aufgebaute PES-Pakete.
 *   Insbesondere:
 *     - H.264: AVCC→AnnexB Konvertierung + SPS/PPS als StartCode vorangestellt
 *     - H.265: HVCC→AnnexB Konvertierung + VPS/SPS/PPS
 *     - MPEG-2 Video: PES_PRIVATE_DATA flag (Byte 6 = 0x81) muss gesetzt sein
 *     - H.264/HEVC: MPEG_VIDEO_PES_START_CODE (0xE0) mit "Fake Start Code" Extension
 *     - Audio (AC3/DTS/LPCM/WMA/Opus/Vorbis): Private Stream 1 (0xBD)
 *     - MPEG-Audio (MP2/MP3/AAC): Standard Audio PES (0xC0)
 *     - PES_packet_length=0 (unbegrenzt) für Video, konkreter Wert für Audio
 *
 * Die Writer-Dateien aus exteplayer3/mipsel zeigen genau was der
 * BCM-Treiber an jedem /dev/dvb Device erwartet. Diese Logik ist hier
 * vollständig portiert.
 */

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/time.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/opt.h>
#include <libavutil/dict.h>
#include <libswresample/swresample.h>
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
#include <sys/uio.h>
#include <linux/dvb/audio.h>
#include <linux/dvb/video.h>
#include <sys/ioctl.h>

#include "bcm_ioctls.h"

#include <string>
#include <vector>
#include <atomic>

/* ======================================================================
 * Konstanten
 * ====================================================================== */
#define PLAYER_VERSION              "1.1.0-bcm"
#define DVB_AUDIO_DEVICE            "/dev/dvb/adapter0/audio0"
#define DVB_VIDEO_DEVICE            "/dev/dvb/adapter0/video0"
#define BUFFER_SIZE_DEFAULT         (4 * 1024 * 1024)
#define POSITION_REPORT_INTERVAL_MS 500

/* PES Start Codes - exakt wie in exteplayer3/mipsel/pes.h */
#define MPEG_VIDEO_PES_START_CODE   0xE0
#define MPEG_AUDIO_PES_START_CODE   0xC0
#define PRIVATE_STREAM_1            0xBD
#define PES_MAX_HEADER_SIZE         19   /* 6 fixed + flags(3) + PTS(5) + max padding */

/* Fake-Start-Code Extension für BCM Video (aus exteplayer3) */
#define PES_VERSION_FAKE_START_CODE 0x45

/* ======================================================================
 * BCM Codec Mapping (FFmpeg 6.x AVCodecID → BCM Enums)
 * ====================================================================== */
static video_stream_type_t codec_to_bcm_video(AVCodecID id)
{
    switch (id) {
    case AV_CODEC_ID_MPEG1VIDEO: return STREAMTYPE_MPEG1;
    case AV_CODEC_ID_MPEG2VIDEO: return STREAMTYPE_MPEG2;
    case AV_CODEC_ID_H264:       return STREAMTYPE_MPEG4_H264;
    case AV_CODEC_ID_HEVC:       return STREAMTYPE_MPEG4_H265;
    case AV_CODEC_ID_H263:       return STREAMTYPE_H263;
    case AV_CODEC_ID_MPEG4:      return STREAMTYPE_MPEG4_Part2;
    case AV_CODEC_ID_MSMPEG4V1:
    case AV_CODEC_ID_MSMPEG4V2:
    case AV_CODEC_ID_MSMPEG4V3:  return STREAMTYPE_DIVX311;
    case AV_CODEC_ID_VC1:        return STREAMTYPE_VC1;
    case AV_CODEC_ID_WMV3:       return STREAMTYPE_VC1_SM;
    case AV_CODEC_ID_VP6:
    case AV_CODEC_ID_VP6F:
    case AV_CODEC_ID_VP6A:       return STREAMTYPE_VB6;
    case AV_CODEC_ID_VP8:        return STREAMTYPE_VB8;
    case AV_CODEC_ID_VP9:        return STREAMTYPE_VB9;
    case AV_CODEC_ID_FLV1:       return STREAMTYPE_SPARK;
    case AV_CODEC_ID_MJPEG:      return STREAMTYPE_MJPEG;
    case AV_CODEC_ID_RV30:       return STREAMTYPE_RV30;
    case AV_CODEC_ID_RV40:       return STREAMTYPE_RV40;
    default:
        fprintf(stderr, "[ffmpeg-player] unknown video codec %s, using MPEG2\n",
                avcodec_get_name(id));
        return STREAMTYPE_MPEG2;
    }
}

static audio_stream_type_t codec_to_bcm_audio(AVCodecID id)
{
    switch (id) {
    case AV_CODEC_ID_AC3:       return AUDIOTYPE_AC3;
    case AV_CODEC_ID_EAC3:      return AUDIOTYPE_AC3_PLUS;
    case AV_CODEC_ID_MP2:       return AUDIOTYPE_MPEG;
    case AV_CODEC_ID_MP3:       return AUDIOTYPE_MP3;
    case AV_CODEC_ID_DTS:       return AUDIOTYPE_DTS;
    case AV_CODEC_ID_AAC:       return AUDIOTYPE_AAC;
    case AV_CODEC_ID_AAC_LATM:  return AUDIOTYPE_AAC_HE;
    case AV_CODEC_ID_PCM_S16LE:
    case AV_CODEC_ID_PCM_S16BE:
    case AV_CODEC_ID_PCM_S24LE:
    case AV_CODEC_ID_PCM_S24BE: return AUDIOTYPE_LPCM;
    case AV_CODEC_ID_WMAV1:
    case AV_CODEC_ID_WMAV2:     return AUDIOTYPE_WMA;
    case AV_CODEC_ID_WMAPRO:    return AUDIOTYPE_WMA_PRO;
    case AV_CODEC_ID_OPUS:      return AUDIOTYPE_OPUS;
    case AV_CODEC_ID_VORBIS:    return AUDIOTYPE_VORBIS;
    case AV_CODEC_ID_AMR_NB:
    case AV_CODEC_ID_AMR_WB:    return AUDIOTYPE_AMR;
    default:                    return AUDIOTYPE_MPEG;
    }
}

static int audio_bypass_mode(AVCodecID id)
{
    switch (id) {
    case AV_CODEC_ID_AC3:
    case AV_CODEC_ID_EAC3:      return 1;
    case AV_CODEC_ID_DTS:        return 2;
    case AV_CODEC_ID_PCM_S16LE:
    case AV_CODEC_ID_PCM_S16BE:
    case AV_CODEC_ID_PCM_S24LE:
    case AV_CODEC_ID_PCM_S24BE: return 3;
    case AV_CODEC_ID_AAC:        return 4;
    case AV_CODEC_ID_AAC_LATM:   return 5;
    default:                     return 0;
    }
}

/* ======================================================================
 * PES Header Konstruktion
 *
 * InsertPesHeader: Baut einen vollständigen PES-Header.
 *   - packet_len = 0 → unbegrenzt (Standard für Video in DVB)
 *   - packet_len > 0 → konkrete Länge (Audio, oder wenn bekannt)
 *   - pts = -1 → kein PTS
 *   - fake_sc > 0 → BCM "Fake Start Code" Extension für H.264/HEVC
 *
 * Gibt die Header-Länge zurück.
 * ====================================================================== */
static uint32_t InsertPesHeader(uint8_t *buf, int32_t payload_len,
                                uint8_t stream_id, int64_t pts,
                                uint32_t fake_sc)
{
    uint32_t pos = 0;

    buf[pos++] = 0x00;  /* start code prefix */
    buf[pos++] = 0x00;
    buf[pos++] = 0x01;
    buf[pos++] = stream_id;

    /* PES_packet_length: 0 = unbegrenzt, sonst payload + flags(3) + PTS(5) */
    uint16_t pes_len = 0;
    if (payload_len >= 0 && payload_len <= 0xFFE8)
        pes_len = (uint16_t)(payload_len + 8); /* +8: flags(3)+PTS(5) */
    buf[pos++] = (pes_len >> 8) & 0xFF;
    buf[pos++] = pes_len & 0xFF;

    /* flags */
    buf[pos++] = 0x80;  /* marker bits */

    if (pts != (int64_t)-1) {
        buf[pos++] = 0x80;  /* PTS_DTS_flags: PTS only */
        buf[pos++] = 0x05;  /* PES_header_data_length */
        /* PTS (ISO 13818-1 §2.4.3.7) */
        uint64_t p = (uint64_t)pts;
        buf[pos++] = (uint8_t)(0x21 | ((p >> 29) & 0x0E));
        buf[pos++] = (uint8_t)((p >> 22) & 0xFF);
        buf[pos++] = (uint8_t)(0x01 | ((p >> 14) & 0xFE));
        buf[pos++] = (uint8_t)((p >> 7)  & 0xFF);
        buf[pos++] = (uint8_t)(0x01 | ((p << 1)  & 0xFE));
    } else {
        buf[pos++] = 0x00;  /* no PTS */
        buf[pos++] = 0x00;
    }

    /* BCM "Fake Start Code" Extension (aus exteplayer3 h264.c / h265.c)
     * Signalisiert dem Treiber codec-spezifische Frame-Grenzen */
    if (fake_sc != 0) {
        buf[pos++] = 0x00;
        buf[pos++] = 0x00;
        buf[pos++] = (fake_sc >> 8) & 0xFF;
        buf[pos++] = fake_sc & 0xFF;
    }

    return pos;
}

/* PES payload-length nachträglich aktualisieren (nach Header-Aufbau) */
static void UpdatePesPayloadSize(uint8_t *buf, uint32_t total_after_6)
{
    uint16_t v = (total_after_6 <= 0xFFFF) ? (uint16_t)total_after_6 : 0;
    buf[4] = (v >> 8) & 0xFF;
    buf[5] = v & 0xFF;
}

/* ======================================================================
 * Scatter-Gather write mit Retry (wie exteplayer3/mipsel/writer.c)
 * ====================================================================== */
static ssize_t writev_retry(int fd, const struct iovec *iov, int iovcnt)
{
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        const uint8_t *buf = (const uint8_t *)iov[i].iov_base;
        ssize_t remain = (ssize_t)iov[i].iov_len;
        while (remain > 0) {
            ssize_t ret = write(fd, buf, remain);
            if (ret < 0) {
                if (errno == EINTR || errno == EAGAIN) { usleep(1000); continue; }
                return -1;
            }
            buf    += ret;
            remain -= ret;
            total  += ret;
        }
    }
    return total;
}

/* ======================================================================
 * Video PES Writers — codec-spezifisch
 * ====================================================================== */

/* --- H.264 Writer ---
 * Erwartet AVCC-Format (extradaten mit avcC magic byte == 1).
 * Konvertiert NAL-Length-Prefix → AnnexB Startcodes (0x00000001).
 * Sendet SPS/PPS (CodecData) vor dem ersten Frame.
 */
struct H264State {
    bool     initialHeader;
    uint32_t nalLengthBytes;
    uint8_t *codecData;
    uint32_t codecDataLen;
    bool     annexB;          /* true wenn Quelle schon AnnexB ist */

    H264State() : initialHeader(true), nalLengthBytes(4),
                  codecData(NULL), codecDataLen(0), annexB(false) {}
    ~H264State() { if (codecData) free(codecData); }
};
static H264State h264state;

static void h264_prepare_codec_data(const uint8_t *extra, int extra_size)
{
    if (!extra || extra_size < 8 || extra[0] != 1) return;

    /* AVCC Format: Byte 0=1, Byte 4=NAL-Length-Bytes-1, Byte 6/7=SPS-Length */
    h264state.nalLengthBytes = (extra[4] & 0x03) + 1;

    uint8_t tmp[2048];
    uint32_t tmp_len = 0;

    /* SPS */
    uint16_t sps_len = (extra[6] << 8) | extra[7];
    if (extra_size < 8 + (int)sps_len) return;
    const uint8_t startcode[] = {0,0,0,1};
    memcpy(tmp + tmp_len, startcode, 4); tmp_len += 4;
    memcpy(tmp + tmp_len, extra + 8, sps_len); tmp_len += sps_len;

    /* PPS */
    int pps_pos = 8 + sps_len;
    if (extra_size > pps_pos + 2) {
        uint16_t pps_len = (extra[pps_pos+1] << 8) | extra[pps_pos+2];
        pps_pos += 3;
        if (extra_size >= pps_pos + (int)pps_len) {
            memcpy(tmp + tmp_len, startcode, 4); tmp_len += 4;
            memcpy(tmp + tmp_len, extra + pps_pos, pps_len); tmp_len += pps_len;
        }
    }

    h264state.codecData    = (uint8_t*)malloc(tmp_len);
    h264state.codecDataLen = tmp_len;
    if (h264state.codecData) memcpy(h264state.codecData, tmp, tmp_len);
}

static bool write_video_h264(int fd, const uint8_t *data, int size, int64_t pts_90k)
{
    if (fd < 0 || !data || size <= 0) return false;

    uint8_t PesHeader[PES_MAX_HEADER_SIZE + 8];
    uint32_t fake_sc = (0x00 << 8) | PES_VERSION_FAKE_START_CODE;
    static const uint8_t startcode[] = {0,0,0,1};

    /* Prüfe ob AnnexB (Startcodes vorhanden) */
    bool is_annexb = (size > 3) &&
        (data[0]==0 && data[1]==0 && data[2]==0 && data[3]==1);

    if (is_annexb) {
        /* AnnexB: direkt schreiben, optional SPS/PPS voranstellen */
        bool has_sps = false;
        for (int i = 0; i < (int)size - 4 && i < 64; i++) {
            if (data[i]==0 && data[i+1]==0 && data[i+2]==0 && data[i+3]==1 &&
                (data[i+4] & 0x1F) == 7) { has_sps = true; break; }
        }

        struct iovec iov[4]; int ic = 0;
        iov[ic++] = {PesHeader, 0};

        if (h264state.initialHeader && !has_sps && h264state.codecData) {
            h264state.initialHeader = false;
            iov[ic++] = {h264state.codecData, h264state.codecDataLen};
        }
        iov[ic++] = {(void*)data, (size_t)size};

        int payload = 0;
        for (int i = 1; i < ic; i++) payload += (int)iov[i].iov_len;
        iov[0].iov_len = InsertPesHeader(PesHeader, -1,
                                         MPEG_VIDEO_PES_START_CODE, pts_90k, fake_sc);
        return writev_retry(fd, iov, ic) >= 0;
    }

    /* AVCC Format: NAL-Length-Prefix → AnnexB */
    if (!h264state.codecData) return false;

    struct iovec iov[64]; int ic = 0;
    iov[ic++] = {PesHeader, 0};

    if (h264state.initialHeader) {
        h264state.initialHeader = false;
        iov[ic++] = {h264state.codecData, h264state.codecDataLen};
    }

    uint32_t pos = 0;
    while (pos + h264state.nalLengthBytes < (uint32_t)size) {
        uint32_t nal_len = 0;
        for (uint32_t i = 0; i < h264state.nalLengthBytes; i++) {
            nal_len = (nal_len << 8) | data[pos + i];
        }
        pos += h264state.nalLengthBytes;
        if (pos + nal_len > (uint32_t)size) break;
        if (ic + 2 >= 64) break;
        iov[ic++] = {(void*)startcode, 4};
        iov[ic++] = {(void*)(data + pos), nal_len};
        pos += nal_len;
    }

    iov[0].iov_len = InsertPesHeader(PesHeader, -1,
                                     MPEG_VIDEO_PES_START_CODE, pts_90k, fake_sc);
    return writev_retry(fd, iov, ic) >= 0;
}

/* --- H.265 Writer ---
 * Analog zu H.264, aber mit HEVC-spezifischem extradata-Parsing.
 * Konvertiert HEVCDecoderConfigurationRecord → AnnexB.
 */
struct H265State {
    bool     initialHeader;
    uint8_t *codecData;
    uint32_t codecDataLen;

    H265State() : initialHeader(true), codecData(NULL), codecDataLen(0) {}
    ~H265State() { if (codecData) free(codecData); }
};
static H265State h265state;

static void h265_prepare_codec_data(const uint8_t *extra, int extra_size)
{
    if (!extra || extra_size < 23) return;
    /* extra[0] != 1 bedeutet AnnexB schon vorhanden */
    if (extra[0] == 0) return;

    uint8_t tmp[4096];
    uint32_t tmp_len = 0;
    static const uint8_t sc[] = {0,0,0,1};

    /* HEVCDecoderConfigurationRecord ab Byte 22 */
    int pos = 22;
    if (pos >= extra_size) return;
    int num_arrays = extra[pos++];
    for (int a = 0; a < num_arrays && pos < extra_size; a++) {
        if (pos + 3 > extra_size) break;
        /* uint8_t nal_type = extra[pos] & 0x3F; */
        pos++; /* nal_type */
        int num_nalus = (extra[pos] << 8) | extra[pos+1];
        pos += 2;
        for (int n = 0; n < num_nalus && pos < extra_size; n++) {
            if (pos + 2 > extra_size) break;
            int nal_len = (extra[pos] << 8) | extra[pos+1];
            pos += 2;
            if (pos + nal_len > extra_size) break;
            if (tmp_len + 4 + nal_len > sizeof(tmp)) break;
            memcpy(tmp + tmp_len, sc, 4); tmp_len += 4;
            memcpy(tmp + tmp_len, extra + pos, nal_len); tmp_len += nal_len;
            pos += nal_len;
        }
    }

    h265state.codecData    = (uint8_t*)malloc(tmp_len);
    h265state.codecDataLen = tmp_len;
    if (h265state.codecData) memcpy(h265state.codecData, tmp, tmp_len);
}

static bool write_video_h265(int fd, const uint8_t *data, int size, int64_t pts_90k)
{
    if (fd < 0 || !data || size <= 0) return false;

    uint8_t PesHeader[PES_MAX_HEADER_SIZE + 8];
    uint32_t fake_sc = (0x00 << 8) | PES_VERSION_FAKE_START_CODE;
    static const uint8_t startcode[] = {0,0,0,1};

    bool is_annexb = (size > 3) &&
        (data[0]==0 && data[1]==0 && data[2]==0 && data[3]==1);

    struct iovec iov[8]; int ic = 0;
    iov[ic++] = {PesHeader, 0};

    if (h265state.initialHeader && h265state.codecData) {
        h265state.initialHeader = false;
        iov[ic++] = {h265state.codecData, h265state.codecDataLen};
    }

    if (!is_annexb && size > 4) {
        /* HVCC NAL-Length-Prefix → AnnexB */
        static uint8_t sc_buf[] = {0,0,0,1};
        uint32_t pos = 0;
        while (pos + 4 < (uint32_t)size && ic + 2 < 8) {
            uint32_t nal_len = AV_RB32(data + pos); pos += 4;
            if (pos + nal_len > (uint32_t)size) break;
            iov[ic++] = {(void*)sc_buf, 4};
            iov[ic++] = {(void*)(data + pos), nal_len};
            pos += nal_len;
        }
    } else {
        iov[ic++] = {(void*)data, (size_t)size};
    }

    iov[0].iov_len = InsertPesHeader(PesHeader, -1,
                                     MPEG_VIDEO_PES_START_CODE, pts_90k, fake_sc);
    return writev_retry(fd, iov, ic) >= 0;
}

/* --- MPEG-1/2 Writer ---
 * Braucht PES_PRIVATE_DATA flag (Byte[6] |= 0x01) und
 * PES_packet_length = 0 (unbegrenzt).
 * Codec-Daten (sequence header) werden vor GOP-Startcode eingefügt.
 */
static bool mpeg2_must_send_header = true;

static bool write_video_mpeg2(int fd, const uint8_t *data, int size, int64_t pts_90k,
                               const uint8_t *extra, int extra_size)
{
    if (fd < 0 || !data || size <= 0) return false;
    uint8_t PesHeader[PES_MAX_HEADER_SIZE];

    struct iovec iov[4]; int ic = 0;
    iov[ic++] = {PesHeader, 0};

    /* Sequence header vor GOP einfügen wenn vorhanden */
    if (extra && extra_size > 0 && mpeg2_must_send_header) {
        /* Suche GOP start code 0x000001B8 */
        for (int pos = 0; pos <= size - 4; pos++) {
            if (data[pos]==0 && data[pos+1]==0 && data[pos+2]==1 && data[pos+3]==0xB8) {
                mpeg2_must_send_header = false;
                iov[ic++] = {(void*)data, (size_t)pos};
                iov[ic++] = {(void*)extra, (size_t)extra_size};
                iov[ic++] = {(void*)(data+pos), (size_t)(size-pos)};
                break;
            }
        }
    }
    if (ic == 1) {
        iov[ic++] = {(void*)data, (size_t)size};
    }

    int payload = 0;
    for (int i = 1; i < ic; i++) payload += (int)iov[i].iov_len;
    iov[0].iov_len = InsertPesHeader(PesHeader, payload,
                                     MPEG_VIDEO_PES_START_CODE, pts_90k, 0);
    /* MPEG-2: PES_PRIVATE_DATA_INDICATOR flag setzen */
    PesHeader[6] |= 0x01;
    /* Update payload size nachdem flag gesetzt wurde */
    UpdatePesPayloadSize(PesHeader, (uint32_t)(payload + iov[0].iov_len - 6));
    return writev_retry(fd, iov, ic) >= 0;
}

/* --- Generischer Video Writer (MPEG4, VC1, VP6/8/9, etc.) ---
 * Einfaches PES mit optionalem extradata prefix beim ersten Frame.
 */
static bool mpeg4_initial_header = true;

static bool write_video_generic(int fd, const uint8_t *data, int size, int64_t pts_90k,
                                 const uint8_t *extra, int extra_size)
{
    if (fd < 0 || !data || size <= 0) return false;
    uint8_t PesHeader[PES_MAX_HEADER_SIZE];

    struct iovec iov[3]; int ic = 0;
    iov[ic++] = {PesHeader, 0};

    int payload = size;
    if (mpeg4_initial_header && extra && extra_size > 0) {
        mpeg4_initial_header = false;
        iov[ic++] = {(void*)extra, (size_t)extra_size};
        payload += extra_size;
    }
    iov[ic++] = {(void*)data, (size_t)size};

    iov[0].iov_len = InsertPesHeader(PesHeader, payload,
                                     MPEG_VIDEO_PES_START_CODE, pts_90k, 0);
    return writev_retry(fd, iov, ic) >= 0;
}

/* ======================================================================
 * Audio PES Writers — codec-spezifisch
 * ====================================================================== */

/* MPEG-Audio (MP2, MP3): Standard Audio PES 0xC0 */
static bool write_audio_mpeg(int fd, const uint8_t *data, int size, int64_t pts_90k)
{
    if (fd < 0 || !data || size <= 0) return false;
    uint8_t PesHeader[PES_MAX_HEADER_SIZE];
    struct iovec iov[2];
    iov[0].iov_len  = InsertPesHeader(PesHeader, size, MPEG_AUDIO_PES_START_CODE, pts_90k, 0);
    iov[0].iov_base = PesHeader;
    iov[1].iov_base = (void*)data;
    iov[1].iov_len  = size;
    return writev_retry(fd, iov, 2) >= 0;
}

/* Private Stream 1 Audio (AC3, EAC3, DTS, LPCM): 0xBD */
static bool write_audio_private(int fd, const uint8_t *data, int size, int64_t pts_90k)
{
    if (fd < 0 || !data || size <= 0) return false;
    uint8_t PesHeader[PES_MAX_HEADER_SIZE];
    struct iovec iov[2];
    iov[0].iov_len  = InsertPesHeader(PesHeader, size, PRIVATE_STREAM_1, pts_90k, 0);
    iov[0].iov_base = PesHeader;
    iov[1].iov_base = (void*)data;
    iov[1].iov_len  = size;
    return writev_retry(fd, iov, 2) >= 0;
}

/* AAC: ADTS (Standard Audio PES 0xC0).
 * Falls Quelle raw AAC (ohne ADTS-Header), ADTS-Header voranstellen. */
static const uint8_t DefaultADTSHeader[] = {0xFF,0xF1,0x50,0x80,0x00,0x1F,0xFC};

static bool write_audio_aac(int fd, const uint8_t *data, int size, int64_t pts_90k)
{
    if (fd < 0 || !data || size <= 0) return false;
    uint8_t PesHeader[PES_MAX_HEADER_SIZE];
    bool has_adts = (size > 1) && (data[0]==0xFF && (data[1]&0xF0)==0xF0);
    struct iovec iov[3]; int ic = 0;
    iov[ic++] = {PesHeader, 0};
    int payload = size;
    if (!has_adts) {
        iov[ic++] = {(void*)DefaultADTSHeader, sizeof(DefaultADTSHeader)};
        payload += sizeof(DefaultADTSHeader);
    }
    iov[ic++] = {(void*)data, (size_t)size};
    iov[0].iov_len = InsertPesHeader(PesHeader, payload, MPEG_AUDIO_PES_START_CODE, pts_90k, 0);
    return writev_retry(fd, iov, ic) >= 0;
}

/* ======================================================================
 * Codec-Dispatch: wählt den richtigen Writer
 * ====================================================================== */
static bool write_video_packet(int fd, AVCodecID cid,
                                const uint8_t *data, int size, int64_t pts_90k,
                                const uint8_t *extra, int extra_size)
{
    switch (cid) {
    case AV_CODEC_ID_H264:       return write_video_h264(fd, data, size, pts_90k);
    case AV_CODEC_ID_HEVC:       return write_video_h265(fd, data, size, pts_90k);
    case AV_CODEC_ID_MPEG1VIDEO:
    case AV_CODEC_ID_MPEG2VIDEO: return write_video_mpeg2(fd, data, size, pts_90k, extra, extra_size);
    default:                     return write_video_generic(fd, data, size, pts_90k, extra, extra_size);
    }
}

static bool write_audio_packet(int fd, AVCodecID cid,
                                const uint8_t *data, int size, int64_t pts_90k)
{
    switch (cid) {
    case AV_CODEC_ID_AAC:
    case AV_CODEC_ID_AAC_LATM:  return write_audio_aac(fd, data, size, pts_90k);
    case AV_CODEC_ID_MP2:
    case AV_CODEC_ID_MP3:       return write_audio_mpeg(fd, data, size, pts_90k);
    /* Private Stream 1 */
    case AV_CODEC_ID_AC3:
    case AV_CODEC_ID_EAC3:
    case AV_CODEC_ID_DTS:
    case AV_CODEC_ID_TRUEHD:
    case AV_CODEC_ID_PCM_S16LE:
    case AV_CODEC_ID_PCM_S16BE:
    case AV_CODEC_ID_PCM_S24LE:
    case AV_CODEC_ID_PCM_S24BE:
    case AV_CODEC_ID_WMAV1:
    case AV_CODEC_ID_WMAV2:
    case AV_CODEC_ID_WMAPRO:
    case AV_CODEC_ID_OPUS:
    case AV_CODEC_ID_VORBIS:
    case AV_CODEC_ID_AMR_NB:
    case AV_CODEC_ID_AMR_WB:    return write_audio_private(fd, data, size, pts_90k);
    default:                    return write_audio_mpeg(fd, data, size, pts_90k);
    }
}

/* ======================================================================
 * JSON helpers
 * ====================================================================== */
static std::string jstr(const std::string &k, const std::string &v)
{ return "\""+k+"\":\""+v+"\""; }
static std::string jint(const std::string &k, long long v)
{ char b[64]; snprintf(b,sizeof(b),"%lld",v); return "\""+k+"\":"+b; }
static std::string jbool(const std::string &k, bool v)
{ return "\""+k+"\":" + (v?"true":"false"); }
static std::string json_get_str(const std::string &j, const std::string &k)
{
    std::string n="\""+k+"\":\"";
    size_t p=j.find(n); if(p==std::string::npos) return "";
    p+=n.length(); size_t e=j.find('"',p);
    return e==std::string::npos?"":j.substr(p,e-p);
}
static long long json_get_int(const std::string &j, const std::string &k)
{
    std::string n="\""+k+"\":";
    size_t p=j.find(n); if(p==std::string::npos) return 0;
    return strtoll(j.c_str()+p+n.length(),NULL,10);
}

/* ======================================================================
 * Track descriptors / State
 * ====================================================================== */
struct AudioTrack { int stream_idx,pid,channels,samplerate,bitrate; std::string lang,codec; AVCodecID codec_id; };
struct SubTrack   { int stream_idx; std::string lang,codec; bool bitmap; };

struct PlayerState {
    int ipc_fd; std::string recv_buf,uri,useragent,extra_headers;
    int buffer_size;
    AVFormatContext *fmt_ctx;
    int video_stream_idx, audio_stream_idx, sub_stream_idx;
    std::vector<AudioTrack> audio_tracks;
    std::vector<SubTrack>   sub_tracks;
    int active_audio_track;
    std::atomic<bool>    running,paused,stop_requested;
    std::atomic<int>     speed;
    std::atomic<int64_t> seek_target_ms;
    pthread_mutex_t seek_mutex;
    int dvb_video_fd, dvb_audio_fd;
    bool hw_sink_available;
    AVCodecID active_video_codec_id, active_audio_codec_id;
    /* extradata cache für Video */
    uint8_t  *video_extra;
    int       video_extra_size;
    AVFormatContext *record_ctx; bool recording;
    pthread_mutex_t record_mutex;
    int64_t duration_ms, position_ms;
    bool is_live, seekable;

    PlayerState()
        : ipc_fd(-1), buffer_size(BUFFER_SIZE_DEFAULT)
        , fmt_ctx(NULL)
        , video_stream_idx(-1), audio_stream_idx(-1), sub_stream_idx(-1)
        , active_audio_track(-1)
        , running(false), paused(false), stop_requested(false)
        , speed(0), seek_target_ms(-1)
        , dvb_video_fd(-1), dvb_audio_fd(-1), hw_sink_available(false)
        , active_video_codec_id(AV_CODEC_ID_NONE)
        , active_audio_codec_id(AV_CODEC_ID_NONE)
        , video_extra(NULL), video_extra_size(0)
        , record_ctx(NULL), recording(false)
        , duration_ms(0), position_ms(0), is_live(false), seekable(false)
    { pthread_mutex_init(&seek_mutex,NULL); pthread_mutex_init(&record_mutex,NULL); }
};
static PlayerState G;

/* ======================================================================
 * IPC
 * ====================================================================== */
static void ipc_send(const std::string &evt, const std::string &params="")
{
    if(G.ipc_fd<0) return;
    std::string msg="{\"evt\":\""+evt+"\"";
    if(!params.empty()) msg+=","+params;
    msg+="}\n";
    write(G.ipc_fd,msg.c_str(),msg.length());
}
static void ipc_send_error(int code,const std::string &msg)
{ ipc_send("error",jint("code",code)+","+jstr("msg",msg)); }

/* ======================================================================
 * DVB Sink
 * ====================================================================== */
static bool open_dvb_sink()
{
    G.dvb_audio_fd = open(DVB_AUDIO_DEVICE, O_RDWR|O_CLOEXEC);
    G.dvb_video_fd = open(DVB_VIDEO_DEVICE, O_RDWR|O_CLOEXEC);
    if(G.dvb_audio_fd<0||G.dvb_video_fd<0) {
        if(G.dvb_audio_fd>=0){close(G.dvb_audio_fd);G.dvb_audio_fd=-1;}
        if(G.dvb_video_fd>=0){close(G.dvb_video_fd);G.dvb_video_fd=-1;}
        return false;
    }
    ioctl(G.dvb_audio_fd, AUDIO_SET_BYPASS_MODE, 0);
    return true;
}

static void configure_dvb_video_codec()
{
    if(G.dvb_video_fd<0||G.video_stream_idx<0) return;
    AVStream *vs = G.fmt_ctx->streams[G.video_stream_idx];
    AVCodecID cid = vs->codecpar->codec_id;
    G.active_video_codec_id = cid;

    /* Extradata cachen */
    if(vs->codecpar->extradata && vs->codecpar->extradata_size>0) {
        G.video_extra = vs->codecpar->extradata;
        G.video_extra_size = vs->codecpar->extradata_size;
    }

    video_stream_type_t t = codec_to_bcm_video(cid);
    ioctl(G.dvb_video_fd, VIDEO_SET_STREAMTYPE, (int)t);
    fprintf(stderr,"[ffmpeg-player] VIDEO_SET_STREAMTYPE=%d (%s)\n",(int)t,avcodec_get_name(cid));

    /* Codec-spezifische Initialisierung */
    if(cid==AV_CODEC_ID_H264 && G.video_extra) {
        h264_prepare_codec_data(G.video_extra, G.video_extra_size);
        fprintf(stderr,"[ffmpeg-player] H.264: AVCC parsed, NAL-Length=%u, CodecData=%u bytes\n",
                h264state.nalLengthBytes, h264state.codecDataLen);
    } else if(cid==AV_CODEC_ID_HEVC && G.video_extra) {
        h265_prepare_codec_data(G.video_extra, G.video_extra_size);
        fprintf(stderr,"[ffmpeg-player] HEVC: HEVCDecoderCfg parsed, CodecData=%u bytes\n",
                h265state.codecDataLen);
    } else if((cid==AV_CODEC_ID_H264||cid==AV_CODEC_ID_HEVC) && G.video_extra) {
        /* VIDEO_SET_CODEC_DATA als Fallback für ältere Treiber */
        video_codec_data_t cd; cd.length=G.video_extra_size; cd.data=G.video_extra;
        ioctl(G.dvb_video_fd, VIDEO_SET_CODEC_DATA, &cd);
    }

    mpeg4_initial_header = true;
    mpeg2_must_send_header = true;

    ioctl(G.dvb_video_fd, VIDEO_PLAY);
}

static void configure_dvb_audio_codec(AVCodecID cid)
{
    if(G.dvb_audio_fd<0) return;
    G.active_audio_codec_id = cid;
    int bypass = audio_bypass_mode(cid);
    ioctl(G.dvb_audio_fd, AUDIO_SET_BYPASS_MODE, bypass);
    fprintf(stderr,"[ffmpeg-player] AUDIO_SET_BYPASS_MODE=%d (%s)\n",bypass,avcodec_get_name(cid));
    ioctl(G.dvb_audio_fd, AUDIO_PLAY);
}

static void close_dvb_sink()
{
    if(G.dvb_video_fd>=0){ioctl(G.dvb_video_fd,VIDEO_STOP);close(G.dvb_video_fd);G.dvb_video_fd=-1;}
    if(G.dvb_audio_fd>=0){ioctl(G.dvb_audio_fd,AUDIO_STOP);close(G.dvb_audio_fd);G.dvb_audio_fd=-1;}
}

/* ======================================================================
 * open_input
 * ====================================================================== */
static bool open_input()
{
    AVDictionary *opts=NULL;
    if(!G.useragent.empty())     av_dict_set(&opts,"user_agent",G.useragent.c_str(),0);
    if(!G.extra_headers.empty()) av_dict_set(&opts,"headers",G.extra_headers.c_str(),0);
    av_dict_set_int(&opts,"buffer_size",G.buffer_size,0);
    av_dict_set(&opts,"reconnect","1",0);
    av_dict_set(&opts,"reconnect_streamed","1",0);
    av_dict_set(&opts,"reconnect_delay_max","5",0);
    av_dict_set(&opts,"timeout","30000000",0);
    av_dict_set(&opts,"rw_timeout","30000000",0);

    G.fmt_ctx=avformat_alloc_context();
    if(!G.fmt_ctx){ipc_send_error(1,"alloc failed");return false;}

    int ret=avformat_open_input(&G.fmt_ctx,G.uri.c_str(),NULL,&opts);
    av_dict_free(&opts);
    if(ret<0){
        char eb[256];av_strerror(ret,eb,sizeof(eb));
        ipc_send_error(1,"open failed: "+std::string(eb));
        avformat_free_context(G.fmt_ctx);G.fmt_ctx=NULL;return false;
    }
    avformat_find_stream_info(G.fmt_ctx,NULL);

    if(G.fmt_ctx->duration>0) G.duration_ms=G.fmt_ctx->duration/1000;
    G.is_live=(G.fmt_ctx->duration<=0||!G.fmt_ctx->pb||
               (G.fmt_ctx->ctx_flags&AVFMTCTX_NOHEADER));
    G.seekable=!G.is_live||(G.fmt_ctx->pb&&G.fmt_ctx->pb->seekable);

    G.video_stream_idx=av_find_best_stream(G.fmt_ctx,AVMEDIA_TYPE_VIDEO,-1,-1,NULL,0);
    G.audio_stream_idx=av_find_best_stream(G.fmt_ctx,AVMEDIA_TYPE_AUDIO,-1,-1,NULL,0);

    G.audio_tracks.clear(); G.sub_tracks.clear();
    for(unsigned i=0;i<G.fmt_ctx->nb_streams;i++){
        AVStream *st=G.fmt_ctx->streams[i];
        AVDictionaryEntry *lang=av_dict_get(st->metadata,"language",NULL,0);
        std::string ls=lang?lang->value:"und";
        if(st->codecpar->codec_type==AVMEDIA_TYPE_AUDIO){
            AudioTrack t;
            t.stream_idx=i;t.pid=st->id;t.lang=ls;
            t.codec=avcodec_get_name(st->codecpar->codec_id);
            t.codec_id=st->codecpar->codec_id;
            t.channels=st->codecpar->ch_layout.nb_channels;
            t.samplerate=st->codecpar->sample_rate;
            t.bitrate=(int)st->codecpar->bit_rate;
            G.audio_tracks.push_back(t);
            if((int)i==G.audio_stream_idx)
                G.active_audio_track=(int)G.audio_tracks.size()-1;
        } else if(st->codecpar->codec_type==AVMEDIA_TYPE_SUBTITLE){
            SubTrack t;t.stream_idx=i;t.lang=ls;
            t.codec=avcodec_get_name(st->codecpar->codec_id);
            t.bitmap=(st->codecpar->codec_id==AV_CODEC_ID_DVD_SUBTITLE||
                      st->codecpar->codec_id==AV_CODEC_ID_DVB_SUBTITLE||
                      st->codecpar->codec_id==AV_CODEC_ID_HDMV_PGS_SUBTITLE);
            G.sub_tracks.push_back(t);
        }
    }
    for(unsigned i=0;i<G.fmt_ctx->nb_streams;i++)
        G.fmt_ctx->streams[i]->discard=AVDISCARD_ALL;
    if(G.video_stream_idx>=0)
        G.fmt_ctx->streams[G.video_stream_idx]->discard=AVDISCARD_DEFAULT;
    if(G.audio_stream_idx>=0)
        G.fmt_ctx->streams[G.audio_stream_idx]->discard=AVDISCARD_DEFAULT;

    fprintf(stderr,"[ffmpeg-player] opened: video=%d(%s) audio=%d(%s) dur=%lldms\n",
        G.video_stream_idx,
        G.video_stream_idx>=0?avcodec_get_name(G.fmt_ctx->streams[G.video_stream_idx]->codecpar->codec_id):"none",
        G.audio_stream_idx,
        G.audio_stream_idx>=0?avcodec_get_name(G.fmt_ctx->streams[G.audio_stream_idx]->codecpar->codec_id):"none",
        (long long)G.duration_ms);
    return true;
}

/* ======================================================================
 * send_track_info, Recording, process_packet, handle_command, poll_ipc
 * ====================================================================== */
static void send_track_info()
{
    std::string aa="[";
    for(size_t i=0;i<G.audio_tracks.size();i++){
        const AudioTrack &t=G.audio_tracks[i];
        if(i>0) aa+=",";
        aa+="{"+jint("id",t.stream_idx)+","+jint("pid",t.pid)+","+
            jstr("lang",t.lang)+","+jstr("codec",t.codec)+","+
            jint("ch",t.channels)+","+jint("rate",t.samplerate)+"}";
    }
    aa+="]";
    std::string sa="[";
    for(size_t i=0;i<G.sub_tracks.size();i++){
        const SubTrack &t=G.sub_tracks[i];
        if(i>0) sa+=",";
        sa+="{"+jint("id",t.stream_idx)+","+jstr("lang",t.lang)+","+
            jstr("codec",t.codec)+","+jbool("bitmap",t.bitmap)+"}";
    }
    sa+="]";
    AVStream *vs=G.video_stream_idx>=0?G.fmt_ctx->streams[G.video_stream_idx]:NULL;
    std::string p=jbool("seekable",G.seekable)+","+jbool("live",G.is_live)+","+
                  jint("dur_ms",G.duration_ms)+","+
                  jstr("container",G.fmt_ctx->iformat?G.fmt_ctx->iformat->name:"")+",";
    AVDictionaryEntry *title=av_dict_get(G.fmt_ctx->metadata,"title",NULL,0);
    p+=jstr("title",title?title->value:"")+",";
    if(vs&&vs->codecpar){
        p+=jstr("vcodec",avcodec_get_name(vs->codecpar->codec_id))+","+
           jint("w",vs->codecpar->width)+","+jint("h",vs->codecpar->height)+",";
        int fps=vs->avg_frame_rate.den>0?(int)(1000.0*vs->avg_frame_rate.num/vs->avg_frame_rate.den):0;
        p+=jint("fps",fps)+",";
    }
    p+="\"audio\":"+aa+",\"subs\":"+sa;
    ipc_send("info",p);
}

static bool start_recording(const std::string &path)
{
    pthread_mutex_lock(&G.record_mutex);
    if(G.recording||!G.fmt_ctx){pthread_mutex_unlock(&G.record_mutex);return false;}
    int ret=avformat_alloc_output_context2(&G.record_ctx,NULL,"mpegts",path.c_str());
    if(ret<0||!G.record_ctx){pthread_mutex_unlock(&G.record_mutex);return false;}
    for(unsigned i=0;i<G.fmt_ctx->nb_streams;i++){
        AVStream *s=G.fmt_ctx->streams[i];
        if(s->codecpar->codec_type!=AVMEDIA_TYPE_VIDEO&&
           s->codecpar->codec_type!=AVMEDIA_TYPE_AUDIO) continue;
        AVStream *os=avformat_new_stream(G.record_ctx,NULL);
        if(!os) continue;
        avcodec_parameters_copy(os->codecpar,s->codecpar);
        os->codecpar->codec_tag=0; os->time_base=s->time_base;
    }
    if(!(G.record_ctx->oformat->flags&AVFMT_NOFILE))
        if(avio_open(&G.record_ctx->pb,path.c_str(),AVIO_FLAG_WRITE)<0)
        { avformat_free_context(G.record_ctx);G.record_ctx=NULL;pthread_mutex_unlock(&G.record_mutex);return false; }
    if(avformat_write_header(G.record_ctx,NULL)<0)
    { avio_closep(&G.record_ctx->pb);avformat_free_context(G.record_ctx);G.record_ctx=NULL;pthread_mutex_unlock(&G.record_mutex);return false; }
    G.recording=true;
    pthread_mutex_unlock(&G.record_mutex);
    return true;
}

static void stop_recording()
{
    pthread_mutex_lock(&G.record_mutex);
    if(G.recording&&G.record_ctx){
        av_write_trailer(G.record_ctx);
        if(!(G.record_ctx->oformat->flags&AVFMT_NOFILE)) avio_closep(&G.record_ctx->pb);
        avformat_free_context(G.record_ctx); G.record_ctx=NULL; G.recording=false;
    }
    pthread_mutex_unlock(&G.record_mutex);
}

static void process_packet(AVPacket *pkt)
{
    AVStream *st=G.fmt_ctx->streams[pkt->stream_index];
    int64_t pts_90k=AV_NOPTS_VALUE;
    if(pkt->pts!=AV_NOPTS_VALUE)
        pts_90k=av_rescale_q(pkt->pts,st->time_base,{1,90000});
    if(pkt->pts!=AV_NOPTS_VALUE){
        int64_t pm=av_rescale_q(pkt->pts,st->time_base,{1,1000});
        if(pm>0) G.position_ms=pm;
    }

    if(G.hw_sink_available){
        if(pkt->stream_index==G.video_stream_idx)
            write_video_packet(G.dvb_video_fd, G.active_video_codec_id,
                               pkt->data, pkt->size, pts_90k,
                               G.video_extra, G.video_extra_size);
        else if(pkt->stream_index==G.audio_stream_idx)
            write_audio_packet(G.dvb_audio_fd, G.active_audio_codec_id,
                               pkt->data, pkt->size, pts_90k);
    }

    if(G.recording&&G.record_ctx){
        pthread_mutex_lock(&G.record_mutex);
        if(G.recording&&G.record_ctx)
            for(unsigned oi=0;oi<G.record_ctx->nb_streams;oi++)
                if(G.record_ctx->streams[oi]->codecpar->codec_id==
                   G.fmt_ctx->streams[pkt->stream_index]->codecpar->codec_id){
                    AVPacket op; av_packet_ref(&op,pkt);
                    op.stream_index=oi;
                    av_packet_rescale_ts(&op,
                        G.fmt_ctx->streams[pkt->stream_index]->time_base,
                        G.record_ctx->streams[oi]->time_base);
                    av_interleaved_write_frame(G.record_ctx,&op);
                    av_packet_unref(&op); break;
                }
        pthread_mutex_unlock(&G.record_mutex);
    }
}

static void handle_command(const std::string &cmd, const std::string &payload)
{
    fprintf(stderr,"[ffmpeg-player] cmd: %s\n",cmd.c_str());
    if(cmd=="pause"){G.paused=true;if(G.dvb_video_fd>=0)ioctl(G.dvb_video_fd,VIDEO_FREEZE);if(G.dvb_audio_fd>=0)ioctl(G.dvb_audio_fd,AUDIO_PAUSE);}
    else if(cmd=="resume"){G.paused=false;if(G.dvb_video_fd>=0)ioctl(G.dvb_video_fd,VIDEO_CONTINUE);if(G.dvb_audio_fd>=0)ioctl(G.dvb_audio_fd,AUDIO_CONTINUE);}
    else if(cmd=="stop"){G.stop_requested=true;}
    else if(cmd=="seek"){pthread_mutex_lock(&G.seek_mutex);G.seek_target_ms=json_get_int(payload,"pos_ms");pthread_mutex_unlock(&G.seek_mutex);}
    else if(cmd=="seek_rel"){pthread_mutex_lock(&G.seek_mutex);G.seek_target_ms=G.position_ms+json_get_int(payload,"delta_ms");pthread_mutex_unlock(&G.seek_mutex);}
    else if(cmd=="set_speed"){
        G.speed=(int)json_get_int(payload,"speed");
        if(G.dvb_video_fd>=0){
            if(G.speed<=1){ioctl(G.dvb_video_fd,VIDEO_FAST_FORWARD,0);ioctl(G.dvb_video_fd,VIDEO_CONTINUE);}
            else ioctl(G.dvb_video_fd,VIDEO_FAST_FORWARD,G.speed.load());
        }
    }
    else if(cmd=="set_audio"){
        int id=(int)json_get_int(payload,"id");
        for(size_t i=0;i<G.audio_tracks.size();i++){
            if(G.audio_tracks[i].stream_idx!=id) continue;
            if(G.audio_stream_idx>=0) G.fmt_ctx->streams[G.audio_stream_idx]->discard=AVDISCARD_ALL;
            G.audio_stream_idx=id; G.active_audio_track=(int)i;
            G.fmt_ctx->streams[id]->discard=AVDISCARD_DEFAULT;
            if(G.dvb_audio_fd>=0){ioctl(G.dvb_audio_fd,AUDIO_PAUSE);ioctl(G.dvb_audio_fd,AUDIO_CLEAR_BUFFER);}
            configure_dvb_audio_codec(G.audio_tracks[i].codec_id); break;
        }
    }
    else if(cmd=="set_subtitle"){
        int id=(int)json_get_int(payload,"id");
        if(id<0){if(G.sub_stream_idx>=0)G.fmt_ctx->streams[G.sub_stream_idx]->discard=AVDISCARD_ALL;G.sub_stream_idx=-1;}
        else{if(G.sub_stream_idx>=0)G.fmt_ctx->streams[G.sub_stream_idx]->discard=AVDISCARD_ALL;G.sub_stream_idx=id;G.fmt_ctx->streams[id]->discard=AVDISCARD_DEFAULT;}
    }
    else if(cmd=="set_ua"){G.useragent=json_get_str(payload,"ua");}
    else if(cmd=="set_headers"){G.extra_headers=json_get_str(payload,"headers");}
    else if(cmd=="record_start"){start_recording(json_get_str(payload,"path"));}
    else if(cmd=="record_stop"){stop_recording();}
    else if(cmd=="set_bufsize"){G.buffer_size=(int)json_get_int(payload,"bytes");}
    else if(cmd=="get_info"){if(G.fmt_ctx)send_track_info();}
}

static void poll_ipc()
{
    if(G.ipc_fd<0) return;
    char buf[4096]; ssize_t n=read(G.ipc_fd,buf,sizeof(buf)-1);
    if(n<=0) return; buf[n]=0; G.recv_buf+=buf;
    size_t pos;
    while((pos=G.recv_buf.find('\n'))!=std::string::npos){
        std::string line=G.recv_buf.substr(0,pos); G.recv_buf=G.recv_buf.substr(pos+1);
        if(!line.empty()) handle_command(json_get_str(line,"cmd"),line);
    }
}

/* ======================================================================
 * Playback loop
 * ====================================================================== */
static void playback_loop()
{
    AVPacket *pkt=av_packet_alloc();
    if(!pkt){ipc_send_error(1,"av_packet_alloc failed");return;}

    ipc_send("started");
    send_track_info();
    if(G.video_stream_idx>=0){
        AVStream *vs=G.fmt_ctx->streams[G.video_stream_idx];
        int fps=vs->avg_frame_rate.den>0?(int)(1000.0*vs->avg_frame_rate.num/vs->avg_frame_rate.den):0;
        ipc_send("video_size",jint("w",vs->codecpar->width)+","+jint("h",vs->codecpar->height)+","+
                 jint("aspect",2)+","+jint("fps",fps)+","+jbool("progressive",true));
    }

    int64_t last_pos=av_gettime_relative()/1000;

    while(!G.stop_requested){
        pthread_mutex_lock(&G.seek_mutex);
        int64_t seek_t=G.seek_target_ms.load();
        if(seek_t>=0) G.seek_target_ms=-1;
        pthread_mutex_unlock(&G.seek_mutex);

        if(seek_t>=0){
            if(av_seek_frame(G.fmt_ctx,-1,seek_t*1000,
                             seek_t<G.position_ms?AVSEEK_FLAG_BACKWARD:0)>=0){
                G.position_ms=seek_t;
                if(G.dvb_video_fd>=0) ioctl(G.dvb_video_fd,VIDEO_CLEAR_BUFFER);
                if(G.dvb_audio_fd>=0) ioctl(G.dvb_audio_fd,AUDIO_CLEAR_BUFFER);
                /* Reset codec state nach Seek */
                h264state.initialHeader=true;
                h265state.initialHeader=true;
                mpeg2_must_send_header=true;
                mpeg4_initial_header=true;
            }
        }

        if(G.paused){poll_ipc();usleep(20000);continue;}
        poll_ipc();

        int ret=av_read_frame(G.fmt_ctx,pkt);
        if(ret==AVERROR_EOF){ipc_send("eof");break;}
        if(ret==AVERROR(EAGAIN)){usleep(5000);continue;}
        if(ret<0){
            char eb[256];av_strerror(ret,eb,sizeof(eb));
            if(G.is_live){usleep(500000);av_seek_frame(G.fmt_ctx,-1,0,AVSEEK_FLAG_BACKWARD);continue;}
            ipc_send_error(4,eb); break;
        }

        if(pkt->stream_index==G.video_stream_idx||
           pkt->stream_index==G.audio_stream_idx||
           pkt->stream_index==G.sub_stream_idx)
            process_packet(pkt);
        av_packet_unref(pkt);

        int64_t now=av_gettime_relative()/1000;
        if(now-last_pos>=POSITION_REPORT_INTERVAL_MS){
            last_pos=now;
            ipc_send("position",jint("pos_ms",G.position_ms)+","+jint("dur_ms",G.duration_ms));
        }
    }
    av_packet_free(&pkt);
}

/* ======================================================================
 * Signal / main
 * ====================================================================== */
static void sig_handler(int){ G.stop_requested=true; }

int main(int argc, char *argv[])
{
    std::string socket_path;
    for(int i=1;i<argc;i++){
        std::string a=argv[i];
        if(a.substr(0,9)=="--socket=") socket_path=a.substr(9);
        else if(a=="--uri"&&i+1<argc) G.uri=argv[++i];
    }
    if(socket_path.empty()||G.uri.empty()){
        fprintf(stderr,"Usage: %s --socket=PATH --uri URI\n",argv[0]);return 1;}

    fprintf(stderr,"[ffmpeg-player] v%s uri=%s\n",PLAYER_VERSION,G.uri.c_str());
    signal(SIGTERM,sig_handler); signal(SIGINT,sig_handler); signal(SIGPIPE,SIG_IGN);

    {
        int fd=socket(AF_UNIX,SOCK_STREAM,0);
        if(fd<0){perror("socket");return 1;}
        struct sockaddr_un addr; memset(&addr,0,sizeof(addr));
        addr.sun_family=AF_UNIX;
        strncpy(addr.sun_path,socket_path.c_str(),sizeof(addr.sun_path)-1);
        int ok=0;
        for(int i=0;i<30&&!ok;i++){
            if(connect(fd,(struct sockaddr*)&addr,sizeof(addr))==0)ok=1;
            else usleep(100000);
        }
        if(!ok){fprintf(stderr,"[ffmpeg-player] IPC connect failed\n");return 1;}
        fcntl(fd,F_SETFL,fcntl(fd,F_GETFL,0)|O_NONBLOCK);
        G.ipc_fd=fd;
    }

#if LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(58,9,100)
    av_register_all(); avcodec_register_all();
#endif
    avformat_network_init();

    G.hw_sink_available=open_dvb_sink();

    {fd_set r;FD_ZERO(&r);FD_SET(G.ipc_fd,&r);struct timeval tv={10,0};select(G.ipc_fd+1,&r,NULL,NULL,&tv);poll_ipc();}

    if(!open_input()){close(G.ipc_fd);return 1;}

    if(G.hw_sink_available){
        configure_dvb_video_codec();
        if(G.audio_stream_idx>=0)
            configure_dvb_audio_codec(G.fmt_ctx->streams[G.audio_stream_idx]->codecpar->codec_id);
    }

    G.running=true;
    playback_loop();

    stop_recording();
    close_dvb_sink();
    if(G.fmt_ctx){avformat_close_input(&G.fmt_ctx);G.fmt_ctx=NULL;}
    avformat_network_deinit();
    if(G.ipc_fd>=0) close(G.ipc_fd);
    return 0;
}
