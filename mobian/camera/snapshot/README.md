# GNOME Snapshot (the Camera) with pinch-to-zoom

`neo-pinch-zoom.patch` (quilt, on Debian's gnome-snapshot 51.0 source): a pinch on the viewfinder sets
camerabin's digital zoom (1x-8x; wrappercamerabinsrc crops and scales the source, so the viewfinder,
pictures and videos all get it), a "2.0×" pill shows in the middle for a moment, and switching cameras
goes back to 1x.

Build on the phone (no Debian build-deps: cargo fetches the crates of Cargo.lock):

    cd /usr/src/neo/snapshot && apt-get source gnome-snapshot && cd gnome-snapshot-51.0
    QUILT_PATCHES=debian/patches quilt import .../neo-pinch-zoom.patch && quilt push -a
    meson setup _build --prefix=/usr --buildtype=release && ninja -C _build     # ~9 min
    # dev packages: rustc cargo meson libgtk-4-dev libadwaita-1-dev libglycin-2-dev libglycin-gtk4-2-dev
    #   libgstreamer1.0-dev libgstreamer-plugins-{base,bad}1.0-dev libseccomp-dev

Then repack Debian's binary deb with `_build/src/snapshot` and `_build/data/resources/resources.gresource`
as `51.0-2+neo1` (the binary reads its UI from the installed resources.gresource: a new binary with the
old bundle panics on the missing template children). Held on the phone; build-debs.sh collects it.

## 60 fps (neo-60fps.patch, 51.0-2+neo5)

Snapshot caps the camera at `MAXIMUM_RATE` 30 fps once a camera advertises frame rates. With PipeWire's libcamera
plugin patched to advertise them (`../pipewire-libcamera`) every POCO camera offers 1-60 fps, so the cap is 60.
Without that plugin patch the hardware encoder (v4l2h264enc) could not negotiate with the camera at all: recordings
with hardware encoding came out as 0-byte files.
