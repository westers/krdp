# KRdp

Library and examples for creating an RDP server.


# Remote Desktop KCM

![Remote Desktop Settings Window](https://cdn.kde.org/screenshots/krdp/krdp-settings.png)

Remote Desktop System Settings page (KCM) that lives in the Networking category.

Features:
- User can toggle the server (running the`krdpserver` binary) on and off using a toggle switch.
- The server can be set to auto-start at session login.
- The KCM uses SystemD DBus messages to toggle the server on and off and auto-start it.
- User can easily add, modify, and remove usernames and passwords that are allowed to connect to the server.
- User can change the port of the server.
    - Do note that the address is currently set to `0.0.0.0`, which means any interface that accepts connections for `krdpserver` will work.
- Certificates can be auto-generated (this is done by default), or the user can supply their own certificates.
- Video quality can be changed between responsiveness and quality.
    - Do note that in software encoding mode, the quality slider might not necessarily do anything. This seems to be an encoder issue.
- Display target can be set to stream the full workspace, primary monitor only, or a specific monitor by ID.
    - A read-only monitor ID map shows which ID corresponds to which screen.
- VAAPI hardware encoder driver can be configured (automatic, disabled, AMD radeonsi, or Intel iHD).
- The KCM will do some basic sanity-checking and warn the user about the following issues:
    - Password manager inaccessible (for KRDP user passwords)
    - No supported H264 encoder
    - Failures with generating certificates

Not all setting changes require a server restart. Settings that take effect
immediately without disconnecting active sessions:
- Video quality
- Display target, monitor ID
- VAAPI driver mode
- Autostart on login

Settings that require a server restart (the KCM will show a warning banner):
- Listening port
- User credentials (add/modify/remove)
- System user authentication toggle
- Certificate configuration (auto-generate toggle, certificate paths)

# Running the example server

The example server requires a username and password to be provided on the command line, which will be used when connecting from an RDP client. They can be provided using the `-u` and `-p` command line parameters, respectively. For example:

```
krdpserver -u user -p test
```

The server will then listen on all interfaces on port 3389, and clients can connect with the username "user" and the password "pass".

# Connecting to the example server

To connect to the server, make sure to pass the username and password the server was started with. Note that the username is case-sensitive; this may be especially unexpected for those using Microsoft Windows RDP clients to connect, as system usernames on that platform are generally not case-sensitive.

Currently, the main client that has been used for testing and is confirmed to work is the FreeRDP client. Launch the FreeRDP client with the following command: `xfreerdp /u:<username> /p:<password> -clipboard /v:<ip_address>:3389`, filling in the username, password and IP address as appropriate. If testing locally, substitute `localhost` for an IP address.

# Security considerations

In addition, a valid TLS certificate and key are required to encrypt the communication between client and server. The server will look for a file called `server.crt` and `server.key` in the current working directory, but a different path can be provided using the `--certificate` and `--certificate-key` command line parameters. If no valid certificate is found using any of these methods, the server will internally generate a self-signed certificate and use that.

# Command Line Options

The following command line options are available for the example server:

<dl>
    <dt>-u, --username <username></dt>
    <dd>The username to use when a client tries to login. Required.</dd>
    <dt>-p, --password <password></dt>
    <dd>The password to require when a client tries to login. Required.</dd>
    <dt>--port <port></dt>
    <dd>The port to listen on for connections. Defaults to 3389.</dd>
    <dt>--certificate <certificate></dt>
    <dd>The path to a TLS certificate file to use. If not supplied or it cannot be found a temporary self-signed certificate will be generated.</dd>
    <dt>--certificate-key <certificate-key></dt>  
    <dd>The path to the TLS certificate key that matches the provided certificate.</dd>
    <dt>--monitor <monitor></dt>The index of the monitor to use for streaming video. If not supplied the whole workspace is used.</dd>
    <dt>--quality <quality></dt>
    <dd>Set the video quality, from 0 (lowest) to 100 (highest).</dd>
</dl>

When `--monitor` is not supplied, KRDP uses persisted config keys:

- `General/MonitorMode=workspace|primary|specific|multi`
- `General/MonitorIndex=<id>` (used when mode is `specific`)

`multi` gives every monitor its own capture stream, encoder and RDPGFX surface,
so a client that negotiates multi-monitor (e.g. `sdl-freerdp /multimon`) sees
them as separate remote monitors, and a client that does not sees their union as
one desktop. The 4096-px hardware H.264 limit applies to each monitor on its
own, not to the union, so `multi` is how to stream a workspace whose combined
width exceeds 4096 px. A monitor larger than 4096 px in either direction is left
out because the encoder cannot take it; when fewer than two monitors remain, the
server falls back to `specific` on the primary. The server always streams its
own monitors: a client's declared layout and `/size:` are ignored.

`MonitorMode` is applied live, so switching needs no restart:

```bash
kwriteconfig6 --file krdpserverrc --group General --key MonitorMode multi --notify
```

The KDE Remote Desktop settings page exposes this as **Display target** and
**Monitor ID**, and shows the current monitor ID map (`0: <screen name>`, etc.).

# Known Working and Not-Working Clients

The following clients are known to work with the server:

- XFreeRDP and wlFreeRDP from the FreeRDP project.
- Remmina, a remote desktop client for Gnome.
- Thincast Remote Desktop Client
- Windows Remote Desktop client, at least as shipped with a recent Windows 10.

The following clients are known not to work:

- Microsoft's Remote Desktop client for Android. While it should support H.264
it seems to not enable it.

# Known Issues and Limitations

- Only video streaming and remote input is supported.
- Only the NLA security type of RDP is supported.
- Only one username and password combination is supported for login.
- Only the "Graphics Pipeline" extension of the RDP protocol is
implemented for video streaming. This extension allows using H.264 for video
streaming, but it means only clients supporting that extension are supported.
- H.264 encoding is done using hardware encoding if possible, but currently we
only support using VAAPI for this. Most notably this means hardware encoding on
NVidia hardware can not be used and software encoding will be used instead.
Additionally, on certain hardware there are limits to what size of frame can be
encoded by the hardware. In both cases, encoding will fall back to software
encoding.
- KDE's implementation of the Remote Desktop portal is rather limited as
shipped with Plasma 5.27. Most notably it does not allow selecting which screen
to stream, nor does it have an option to remember the setup and reuse it when
the same application requests a new connection. As a workaround, the server
will open a remote desktop session on startup and reuse that session for all
RDP connections. Additionally, monitor selection can be done using the
`--monitor` command line option.
- Input on a high DPI screen may be offset incorrectly. This is due to a bug in
the Remote Desktop Portal that has been fixed in the meantime. The fix will be 
released with KDE Plasma 5.27.8.

# CLI

While currently not very well supported it is possible to configure
KRdp purely from a CLI. This is particularly useful when you have access over
SSH but haven't yet configured KRdp.

```bash
# Authorize the krdpserver for remote desktop access
flatpak permission-set kde-authorized remote-desktop org.kde.krdpserver yes

# Generate a server certificate
mkdir --parents "$HOME/.local/share/krdpserver"
certificatePath="$HOME/.local/share/krdpserver/krdp.crt"
certificateKeyPath="$HOME/.local/share/krdpserver/krdp.key"
openssl req -nodes -new -x509 -keyout "$certificateKeyPath" -out "$certificatePath" -days 1 -batch

# Configure the certificate and enable system user authentication
kwriteconfig6 --file krdpserverrc --group General --key Certificate "$certificatePath"
kwriteconfig6 --file krdpserverrc --group General --key CertificateKey "$certificateKeyPath"
kwriteconfig6 --file krdpserverrc --group General --key SystemUserEnabled true
# Optional: display target (workspace|primary|specific|multi) and monitor ID
# (applied live; add --notify to change it without restarting the service)
kwriteconfig6 --file krdpserverrc --group General --key MonitorMode workspace
kwriteconfig6 --file krdpserverrc --group General --key MonitorIndex 0
# Optional: VAAPI driver mode (auto|off|radeonsi|iHD)
kwriteconfig6 --file krdpserverrc --group General --key VaapiDriverMode auto
# Optional: wake the display on connect and keep it awake while streaming (true|false)
kwriteconfig6 --file krdpserverrc --group General --key WakeDisplayOnConnect true

# Enable/restart the systemd service
systemctl --user enable --now app-org.kde.krdpserver.service
systemctl --user restart app-org.kde.krdpserver.service
```

## Running A Local Build Under systemd

When testing changes from your local checkout, use a user service drop-in and a
matching desktop entry override so Wayland privilege checks (`fake_input` and
`zkde_screencast_unstable_v1`) apply to your local binary.

```bash
# Build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j"$(nproc)"

# Desktop entry override (required when ExecStart points to the build tree)
mkdir -p "$HOME/.local/share/applications"
cp /usr/share/applications/org.kde.krdpserver.desktop "$HOME/.local/share/applications/org.kde.krdpserver.desktop"
sed -i "s#^Exec=.*#Exec=$PWD/build/bin/krdpserver#" "$HOME/.local/share/applications/org.kde.krdpserver.desktop"
kbuildsycoca6 --noincremental

# systemd user drop-in
mkdir -p "$HOME/.config/systemd/user/app-org.kde.krdpserver.service.d"
cat > "$HOME/.config/systemd/user/app-org.kde.krdpserver.service.d/zz-krdp-plasma.conf" <<EOF
[Service]
UnsetEnvironment=LIBVA_DRIVER_NAME
Environment=XDG_DATA_DIRS=%h/.local/share:/usr/local/share:/usr/share
ExecStart=
ExecStart=$PWD/build/bin/krdpserver --plasma --monitor 0
EOF

# Reload and restart
systemctl --user daemon-reload
systemctl --user restart plasma-xdg-desktop-portal-kde app-org.kde.krdpserver

# Verify active command
systemctl --user show app-org.kde.krdpserver.service -p ExecStart
```

To switch back to the packaged binary:

```bash
cat > "$HOME/.config/systemd/user/app-org.kde.krdpserver.service.d/zz-krdp-plasma.conf" <<EOF
[Service]
UnsetEnvironment=LIBVA_DRIVER_NAME
ExecStart=
ExecStart=/usr/bin/krdpserver --plasma --monitor 0
EOF
systemctl --user daemon-reload
systemctl --user restart plasma-xdg-desktop-portal-kde app-org.kde.krdpserver
```

Useful runtime log command:

```bash
journalctl --user -f -o cat -u app-org.kde.krdpserver -u plasma-xdg-desktop-portal-kde
```

### Private KPipeWire

KRDP links against a patched KPipeWire built into `.deps/kpipewire` (source: `~/dev/kpipewire`,
branch `westers/opt-015`; patches exported to `~/dev/rdp/kpipewire-vaapi-fix/patches-6.6.4/`).
`scripts/build-kpipewire.sh` builds/installs it and relinks KRDP; `scripts/check-kpipewire-link.sh`
verifies `build/bin/krdpserver` resolves `libKPipeWire*` there. Re-run the build script after any
KPipeWire edit or after an apt upgrade of Qt/KF6/FFmpeg/PipeWire. To go back to the system
library, reconfigure with `-UKPipeWire_DIR -DCMAKE_PREFIX_PATH=`. `scripts/check-stock-build.sh`
confirms the tree still builds `krdpplasmastreamer` against the system KPipeWire on its own.

The `krdpplasmastreamer` harness has two offset-list flags for exercising the private
KPipeWire's encoder-reopen behavior without a real client: `--keyframe-at <s[,s…]>` calls
`requestKeyFrame()` at the given offsets, and `--quality-at <s:q[,s:q…]>` (e.g. `4:40,8:100`)
changes the session quality at the given offsets — each quality change reopens `h264_vaapi`
at the new QP (an IDR), so watch for one `Reopened h264_vaapi` log line and matching `fixed QP`
line per entry.

### VAAPI Driver Auto-Selection

On mixed-GPU systems, KRDP now attempts to avoid decode-only VAAPI backends by
auto-selecting a non-NVIDIA `LIBVA_DRIVER_NAME` when possible.

The persisted KCM/config key is `General/VaapiDriverMode` with these values:

- `auto` (default): automatic mixed-GPU selection.
- `off`: disable KRDP VAAPI driver auto-selection.
- `radeonsi`: force AMD VAAPI driver.
- `iHD`: force Intel VAAPI driver (runtime fallback to `i965` remains available).

Note for NVIDIA-only systems: current KRDP hardware encode integration is
VAAPI-based. NVIDIA acceleration typically uses NVENC instead, so KRDP falls
back to software (`libx264`) via KPipeWire's own internal encoder fallback
unless a non-NVIDIA VAAPI encode path is present. This is an API-path
limitation, not raw GPU compute performance.

Manual environment override examples:

```bash
systemctl --user set-environment KRDP_AUTO_VAAPI_DRIVER=0
systemctl --user set-environment KRDP_FORCE_VAAPI_DRIVER=radeonsi
```

### Display Wake On Connect

KWin does not render outputs that are DPMS-off, so connecting to a locked
session whose monitors have gone to sleep yields a screencast with no frames
and a blank remote surface. When the first client starts streaming, KRDP now
asks PowerDevil to wake the display (`org.kde.Solid.PowerManagement.wakeup`,
falling back to `org.freedesktop.ScreenSaver.SimulateUserActivity`) and takes
an `org.freedesktop.ScreenSaver` inhibition so the display stays on for as long
as any session is streaming. The inhibition is released when the last session
ends or the server shuts down.

The persisted config key is `General/WakeDisplayOnConnect`:

- `true` (default): wake and inhibit as described above.
- `false`: leave display power management alone.

The key is applied live like the other `General` settings; the startup summary
line reports it as `wakeDisplay=1|0`.

### KPipeWire Patch (Damage Metadata)

`patches/kpipewire/0001-damage-metadata-encoded-stream.patch` remains in the
tree for reference but is **no longer consumed** by KRDP: the damage-metadata
pairing it fed was removed (KPipeWire does not pair damage 1:1 with encoded
packets, so it mis-paired rects; see the 2026-09-15 clean-up). The encoded
path now sends a full-surface update per frame, matching upstream. The patch
would need to attach sequence/PTS-matched damage to `Packet` before it is worth
re-wiring.

### Performance Notes

The encoded-frame path mirrors upstream KRDP: one full-surface AVC420 region
per frame, and the pending-send queue is cleared only when a new keyframe
arrives (encoded P-frames are never dropped — dropping one corrupts the
client's decode until the next keyframe). Fork-specific value kept over
upstream:

- H.264 **Main** profile when the encoder offers it (CABAC, better quality/bit).
- Automatic VAAPI driver selection on mixed-GPU systems (see above).
- Multi-monitor `ResetGraphics` layout for workspace/output streams.
- Correct pointer mapping and input marshalling for non-origin outputs.

The single most effective bandwidth lever is the `Quality` setting: it maps to
the encoder's CQP. With the private KPipeWire, `Quality` maps to `h264_vaapi`
CQP QP = 40 − 0.28·Quality (`100`→QP 12, `80`→QP 18, `50`→QP 26); with stock
KPipeWire the old map applies (`Quality=100`→QP 1). ~80 is a good default.

`AdaptiveQuality` (kcfg `General/AdaptiveQuality`, `Bool`, default `true`) turns
`Quality` into a cap instead of a fixed value: while streaming, quality steps
in +5/−10 increments (at most once every 1.5 s) from measured goodput and RTT
(`KRdp::AdaptiveQuality::step()`, unit-tested in `AdaptiveQualityTest`). With
the private KPipeWire, each quality change reopens the `h264_vaapi` codec at
the new QP, forcing an IDR so the client resyncs immediately; the RDPGFX
metablock reports the actual QP in use. Set `AdaptiveQuality=false` to pin
quality to the `Quality` cap.

Goodput (`NetworkDetection::bandwidth()`) is measured in a scheduled window —
500 ms every 2 s — independent of frame sends; bracketing the measurement
around a single frame (the original approach) produced nonsense samples like
"10 bytes in 1 ms" or "416 bytes in 1 ms", which read as either near-zero or
many-hundred-Mbit/s goodput and drove the adaptive loop to oscillate wildly.
A sample shorter than 100 ms or smaller than 4 KB is discarded before it
reaches the smoothing filter (`NetworkDetection::onBandwidthMeasureResults()`),
and the adaptive loop itself waits for at least three accepted samples
(`NetworkDetection::validBandwidthSamples()`) before it starts steering
quality — a fresh connection stays at the `Quality` cap until the estimate is
real. `AdaptiveQuality`/`Quality` changes made while a session is connected
(e.g. `kwriteconfig6 --notify --file krdpserverrc --group General --key
AdaptiveQuality false`) take effect within a couple of seconds; a
`Runtime config applied: ...` line in the journal confirms the reload ran.

Useful debug markers:

```bash
journalctl --user -f -o cat -u app-org.kde.krdpserver | \
  rg -i 'Reset graphics monitor layout|GFX channel reset|Selected caps|VAAPI driver'
```

## SDDM Autologin

Since SDDM currently has no RDP support, you either need to already be logged in,
or let SDDM auto log in to the user.

**Mind that your session starts physically unlocked on your PC.**
**Also when you are remote logging into your session it will be physically unlocked.**

```bash
# Configure autologin for your current user
user=$(whoami)
sudo kwriteconfig6 --file /etc/sddm.conf.d/kde_settings.conf --group Autologin --key User $user

# Enable automatic relogin on logout to avoid scenarios where the system is stuck on sddm
sudo kwriteconfig6 --file /etc/sddm.conf.d/kde_settings.conf --group Autologin --key Relogin true

# Restart sddm to force an autologin (don't run this if you are already logged in :D)
## systemctl restart sddm.service
```

## Smoke Test Script

Use the bundled smoke test helper to rebuild KRDP, restart services, and watch
the key log markers used during tuning:

```bash
./smoke-test.sh
```

Useful flags:

```bash
./smoke-test.sh --no-build --watch-seconds 180
./smoke-test.sh --no-watch
./smoke-test.sh --no-build --assert-encoder vaapi
./smoke-test.sh --no-build --assert-encoder software
```
