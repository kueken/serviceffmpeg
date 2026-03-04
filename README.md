# serviceffmpeg

Pure FFmpeg Enigma2 media service plugin.
Replaces servicemp3 (GStreamer) and servicehisilicon (proprietary Hisilicon HAL).

## Architecture

```
Enigma2 process
└── libserviceffmpeg.so      (C++ Enigma2 plugin)
    implements:
      iPlayableService
      iSeekableService
      iAudioTrackSelection
      iSubtitleOutput
      iRecordableService
    │
    │ Unix Domain Socket  (line-oriented JSON, one command/event per line)
    │ e.g. /tmp/sfmp-12345.sock
    │
    └── ffmpeg-player        (external process, spawned per playback)
        │
        ├── AVFormatContext  -- demux (all containers libavformat supports)
        ├── libavcodec       -- codec info / subtitle decode
        ├── libswresample    -- audio resampling (SW path)
        ├── libass           -- ASS/SSA subtitle rendering
        │
        ├── /dev/dvb/adapter0/video0  -- ES inject (HW decode, preferred)
        ├── /dev/dvb/adapter0/audio0  -- ES inject (HW decode, preferred)
        │
        └── AVFormatContext output    -- stream recording (mpegts copy)
```

### Why external process?

1. **Crash isolation**: ffmpeg-player crash does not kill Enigma2
2. **Library isolation**: player can link its own FFmpeg version,
   independent of system libs (critical on Hisilicon where vendor
   ships fixed ffmpeg3 closed libs incompatible with newer versions)
3. **Hot update**: player binary can be updated without restarting E2
4. **No GStreamer**: eliminates the entire gst-plugins-bad/dvbmediasink
   dependency chain. FFmpeg handles all protocols and containers directly.

### Why not servicemp3 / GStreamer?

servicemp3 uses GStreamer `playbin` which:
- Dynamically discovers plugins at runtime (fragile)
- Routes through dvbmediasink (poorly maintained)
- Uses libav GStreamer plugin which wraps FFmpeg... to eventually call
  the same codecs, with 3x the abstraction layers
- Sparse stream subtitle sync bug exists since 2010 (unfixed)
- Crashes in one GStreamer plugin can destabilize Enigma2

### Why not servicehisilicon?

servicehisilicon is a thin wrapper around Hisilicon's proprietary
closed-source player. It:
- Only works on Hisilicon hardware
- Depends on specific kernel module / firmware versions
- Cannot be updated independently of the BSP
- The libeplayer3 origins (sh4/STi series) are now 15 years old

## IPC Protocol

Wire format: one JSON object per line (`\n` terminated).

### E2 → Player (commands)
```json
{"cmd":"play"}
{"cmd":"pause"}
{"cmd":"resume"}
{"cmd":"stop"}
{"cmd":"seek","pos_ms":30000}
{"cmd":"seek_rel","delta_ms":-10000}
{"cmd":"set_audio","id":2}
{"cmd":"set_subtitle","id":3}
{"cmd":"set_subtitle","id":-1}
{"cmd":"set_speed","speed":2}
{"cmd":"set_ua","ua":"Mozilla/5.0 ..."}
{"cmd":"set_headers","headers":"X-Token: abc123\r\n"}
{"cmd":"record_start","path":"/media/hdd/rec.ts"}
{"cmd":"record_stop"}
```

### Player → E2 (events)
```json
{"evt":"started"}
{"evt":"eof"}
{"evt":"sof"}
{"evt":"error","code":1,"msg":"open failed: Connection refused"}
{"evt":"info","seekable":true,"live":false,"dur_ms":5400000,
  "container":"matroska","title":"Movie",
  "vcodec":"h264","w":1920,"h":1080,"aspect":2,"fps":25000,
  "audio":[{"id":0,"pid":256,"lang":"deu","codec":"ac3","ch":6,"rate":48000}],
  "subs":[{"id":3,"lang":"eng","codec":"subrip","bitmap":false}]}
{"evt":"position","pos_ms":12345,"dur_ms":5400000}
{"evt":"buffering","pct":42}
{"evt":"buffer_done"}
{"evt":"video_size","w":1920,"h":1080,"aspect":2,"fps":25000,"progressive":true}
{"evt":"subtitle","text":"Hello World","pts_ms":12000,"dur_ms":3000}
{"evt":"subtitle_clear"}
```

## Hardware targets

### mipsel - Xtrend et9200 (BCM7325)
- DVB ES sink via `/dev/dvb/adapter0/{audio,video}0`
- Hardware MPEG2/H.264 decoder in BCM chip
- Software decode fallback via ALSA for audio-only files
- SD/HD streams work; 4K not supported by BCM7325

### ARM - Hisilicon Hi3798MV200 (Zgemma H9S etc.)
- Same DVB sink interface
- On-chip HW decoder handles H.264, HEVC, VP9
- Vendor's ffmpeg3 closed libs isolated: player links own FFmpeg 6.x
  via static linking or separate lib path (LD_LIBRARY_PATH override)

### ARM - Amlogic S905x (Dreambox One etc.)
- DVB sink same interface
- HW HEVC/VP9 decode via `/dev/amstream_*` OR via standard DVB ES sink
  (depends on driver; DVB ES path is the most portable)

## Building

### For OpenPLi / OpenEmbedded:
```sh
# Add recipe to your layer
cp serviceffmpeg.bb \
   /path/to/meta-mylayer/recipes-enigma2/serviceffmpeg/serviceffmpeg.bb

# Add to image
echo 'IMAGE_INSTALL += "serviceffmpeg"' >> conf/local.conf

# Build
bitbake serviceffmpeg
```

### For manual cross-compile (mipsel example):
```sh
source /opt/mipsel-toolchain/environment-setup

./autogen.sh

./configure \
    --host=mipsel-oe-linux \
    --with-sysroot=/opt/mipsel-sysroot \
    PKG_CONFIG_PATH=/opt/mipsel-sysroot/usr/lib/pkgconfig \
    --with-player-bin-path=/usr/bin

make -j4
make install DESTDIR=/tmp/serviceffmpeg-install
```

### For native Linux testing:
```sh
./autogen.sh
./configure ENIGMA2_CFLAGS="-I/path/to/enigma2/include"
make
# Test player standalone:
./player/ffmpeg-player --socket=/tmp/test.sock --uri=file:///path/to/video.mkv
```

## Known limitations / TODO

- [ ] Software audio decode path (ALSA) for mipsel audio-only files
- [ ] Bitmap subtitle rendering (DVB/PGS) - needs framebuffer output
- [ ] HbbTV stream support (dash/isoff)
- [ ] Cuesheet support (CUE files for audio albums)
- [ ] Speed control for reverse playback via FFmpeg seek
- [ ] Buffer size tuning for slow USB/network storage
- [ ] EPG correlation for IP TV streams

## License

GPL v2. See COPYING.

Based on concepts from:
- libeplayer3 (crow, schischu, hellmaster1024, konfetti) - GPL v2
- Taapat's Enigma2 libeplayer3 port - GPL v2
- servicemp3 (OpenPLi) - GPL v2
- libstb-hal-ddt (Duckbox-Developers) - GPL v2
