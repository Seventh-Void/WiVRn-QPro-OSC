# Quest Pro eye-camera headset binaries

`questpro-camera-injector` loads `libquestpro-camera-streamer-v8.so` into the headset camera provider (root), and `questpro-camera-relay-v8` serves the frames as `QPLIVE3` on headset `127.0.0.1:27273`; `camera_link.cpp` pushes and runs them over adb.
The C sources are byte-identical copies from upstream https://github.com/Fwooffy/Qpro-Enhanced-FT-Wireless (`src/`), MIT licensed (QproFaceTracking contributors, see `LICENSE`); the binaries are aarch64 NDK builds of them.
Rebuild with `./build.sh` (Android NDK, `--target=aarch64-linux-android28`, `STREAM_PORT=27273`).
