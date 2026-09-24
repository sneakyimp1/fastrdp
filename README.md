# fastrdp

A low-latency Remote Desktop (RDP) client for Linux, built so that scrolling, video and
window resizing feel as smooth as the Windows client (mstsc) does.

It uses [FreeRDP](https://github.com/FreeRDP/FreeRDP) for the protocol, and replaces
everything between the network and your screen with a GPU pipeline:

- **Hardware H.264 decoding** through VAAPI. Decoded frames go straight to OpenGL as
  dmabufs, so nothing is copied back to the CPU.
- **AVC444 is recombined on the GPU.** Windows' full-colour mode ships as two 4:2:0
  streams that fastrdp stitches back together in shaders.
- **GPU-composited graphics pipeline (RDPGFX).** Every remote surface is a GPU texture.
  Scrolls (surface-to-surface copies), cache hits and fills are GPU blits. ClearCodec,
  Planar and Progressive are decoded on the CPU, and only the changed tiles are uploaded.
- **Presents immediately.** Frames are shown as soon as they're decoded instead of
  waiting for the next vblank. On Wayland the compositor can't tear, so this is simply the
  lowest-latency option. `--vsync` restores vblank pacing.
- **Local hardware cursor.** The remote pointer image is drawn by your compositor, so
  it moves with zero network delay.
- **Smooth resizing.** While you drag, the current frame is scaled to fit. The new
  resolution is requested from the server once you stop.
- **HiDPI aware.** The remote desktop is rendered at your display's native pixel count
  and scale factor.
- **Clipboard.** Copy and paste text, formatted text (HTML) and images in both
  directions. Remote content is fetched only when you actually paste.
- **Connection manager.** Running `fastrdp` with no arguments opens a Qt window with saved
  connections. Passwords are optional and only ever stored in the system keyring
  (KWallet / Secret Service), never in config files.

### Why the other Linux clients lag

Several distributions, Fedora included, build FreeRDP without FFmpeg and VAAPI, so H.264
is decoded in software with OpenH264 on a single CPU core. The frame is then converted and
copied again on its way to the screen. That's what makes Remmina and xfreerdp feel sluggish
next to mstsc on the same link. You can check your distribution's build with
`xfreerdp /buildconfig | tr ' ' '\n' | grep -E 'FFMPEG|VAAPI|OPENH264'`.

## Building

Fedora:

```bash
sudo dnf install cmake ninja-build gcc-c++ freerdp-devel libwinpr-devel SDL3-devel \
    libepoxy-devel ffmpeg-devel libva-devel libdrm-devel qt6-qtbase-devel qtkeychain-qt6-devel
cmake -S . -B build -G Ninja
ninja -C build
```

`ffmpeg-devel` with VAAPI support comes from RPM Fusion. Install for your user (adds a
menu entry):

```bash
cmake --install build --prefix ~/.local
```

## Usage

```bash
fastrdp                                    # connection manager
fastrdp /v:my-pc /u:me                     # connect directly; prompts in the terminal
fastrdp --stats /v:my-pc /u:me /d:CORP     # print per-second performance numbers
```

Any FreeRDP argument works (`/f`, `/size:WxH`, `/sound`, `/drive:…`, `/gateway:…`).
fastrdp adds its own options too; see `fastrdp --help`. Press **Ctrl+Alt+Enter** to toggle
full screen. Windows-key shortcuts go to the remote PC while in full screen.

The window title shows the resolution, frame rate, average decode time, the codec the
server chose, and whether video is decoded on the GPU.

### Server-side tuning

On the Windows machine, enable these under *Computer Configuration → Administrative
Templates → Windows Components → Remote Desktop Services → Remote Desktop Session Host →
Remote Session Environment*:

- **Prioritize H.264/AVC 444 graphics mode for Remote Desktop Connections**
- **Configure H.264/AVC hardware encoding for Remote Desktop Connections**

## Architecture

| Thread | Work |
| --- | --- |
| FreeRDP network thread | transport, input, cursor updates |
| Dynamic-channel thread | RDPGFX parsing, CPU codecs, H.264 decode (VAAPI) → render commands |
| UI thread | SDL3 events, OpenGL ES 3 renderer, present |

- `src/gfx_pipeline.cpp`: RDPGFX callbacks; decodes and emits one command batch per frame.
- `src/h264_decoder.cpp`: FFmpeg decoder with VAAPI and DRM PRIME export (software fallback).
- `src/renderer.cpp`: surface textures, blits, YUV/AVC444 shaders, dmabuf import.
- `src/rdp_session.cpp`: FreeRDP instance, channels, pointer and input.
- `src/clipboard.cpp`: cliprdr ↔ SDL clipboard bridge with lazy remote fetch.
- `src/app.cpp`: window, event loop, resize debounce, certificate dialogs.
- `src/launcher/`: Qt connection manager, bookmarks and keyring.

## Status

Working, and tested against a Windows desktop (RDPGFX 10.7, H.264) with VAAPI on Intel graphics.

Not implemented yet:

- copying files through the clipboard
- multi-monitor
- RemoteApp
- smartcards
- Gateway consent-message dialog

## License

Apache License 2.0, see [LICENSE](LICENSE). fastrdp links against FreeRDP (Apache 2.0),
SDL3 (zlib), FFmpeg (LGPL), Qt 6 (LGPL) and QtKeychain (BSD).
