# Console camera bridge on Linux

Farside redirects an explicitly selected client camera to the signed-in Console
user. It publishes a PipeWire camera source. Applications that enumerate V4L2
cameras also need a `v4l2loopback` device on the **server**. A client reporting
that a camera is mapped does not prove that an application can open it.

## Configure the server

On Ubuntu, install `v4l2loopback-dkms` and `v4l2loopback-utils`. DKMS needs headers
for the running kernel; systems with Secure Boot need their normal module signing
and enrollment procedure. Preserve existing loopback devices and configuration.
Choose an unused video number; the example below uses `/dev/video10`.

For a host without an existing loopback configuration, an administrator can use:

```sh
sudo apt-get install v4l2loopback-dkms v4l2loopback-utils
sudo modprobe v4l2loopback devices=1 video_nr=10 exclusive_caps=1 \
    card_label="Farside Remote Camera"
```

For persistence, configure `v4l2loopback` in a dedicated file in
`/etc/modules-load.d/`, and the matching module options in `/etc/modprobe.d/`.
Do not overwrite another application's module configuration or unload a module
while a camera consumer or producer is using it.

In **Farside Remote Desktop → Host Settings → Console**, load settings and set
**CameraLoopbackDevice** to `/dev/video10`. Save with the normal administrator
authorization dialog. Then explicitly restart Console through **Services** when
its connections can be interrupted. Saving alone does not restart the service.
Inspect runtime settings to verify that the running service uses the chosen path.
The device must also be writable by the signed-in desktop user; normal `uaccess`
ACLs or the system's existing video-device policy should supply that permission.
Do not make the device world writable.

## Use and verify

Reconnect the client, explicitly select a camera and enable mapping. Open or
refresh the remote Camera application or browser camera selector. The V4L2 name
is **Farside Remote Camera**. With `exclusive_caps=1`, the device advertises
capture capabilities only while Farside has opened its producer; an app opened
before mapping was enabled may need a refresh. Capture is demand driven, so
mapping by itself does not mean that the client camera is recording.

Verify live video, switching/off behavior and teardown with an actual application.
A device node, runtime setting or mapped badge alone is insufficient evidence.
Check the Console worker journal for loopback open/configuration failures if the
device is still unavailable. This setup applies to **Console**. Virtual camera
loopback remains unavailable in the current device namespace; do not enable it
by copying the Console path into Virtual settings.

## Sol deployment, 2026-10-01

Sol runs server `df517e8`, with `v4l2loopback` 0.15.3 on kernel
`7.0.0-34-generic`. Dedicated `farside-camera.conf` files in both module config
directories create `/dev/video10` with `exclusive_caps=1`; the desktop user has a
normal `uaccess` read/write ACL. Only Console's loopback setting changed. The
installed settings helper saved it with TLS kept; an explicit service restart
and runtime readback verified `/dev/video10`. Real Hal-camera application
acceptance is tracked separately in the task plan and deployment evidence.
