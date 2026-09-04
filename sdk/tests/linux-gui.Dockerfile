# The Linux GUI proof.
#
# A container with the toolchain, WebKitGTK and a virtual X display, so BOTH
# Linux backends are exercised the way a Linux DAW exercises them: the X11
# peer against a real server, and the webview — a real GTK
# main loop, a real X window, a real page. Without this the Linux GUI checks
# only verify the extension CONTRACT, which a backend that never manages to
# create a view passes just as happily.
#
#   docker build -f sdk/tests/linux-gui.Dockerfile -t sonore-linux-gui sdk
#   docker run --rm sonore-linux-gui
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# libwebkit2gtk-4.1 is the current soname; the backend also accepts 4.0 at
# runtime, but a test image should pin what it actually verifies.
RUN apt-get update && apt-get install -y --no-install-recommends \
      g++ \
      libgtk-3-0 \
      libwebkit2gtk-4.1-0 \
      libx11-dev \
      xvfb \
      ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /sdk
COPY . /sdk

RUN set -eux; \
    INC="-Iinclude -Ithird_party/clap/include"; \
    g++ -std=c++17 -O2 -fPIC -shared -o /out_SonoreSaturator.clap examples/saturator/plugin.cpp $INC -ldl; \
    g++ -std=c++17 -O2 -fPIC -shared -o /out_SonoreSynth.clap examples/synth/plugin.cpp $INC -ldl; \
    g++ -std=c++17 -O2 -fPIC -shared -o /out_SonoreGuiProbe.clap examples/guiprobe/plugin.cpp $INC -ldl; \
    g++ -std=c++17 -O2 -o /out_sdk_tests tests/sdk_tests.cpp $INC; \
    g++ -std=c++17 -O2 -DSONORE_TEST_X11 -o /out_clap_host_test tests/clap_host_test.cpp $INC -ldl -lX11; \
    g++ -std=c++17 -O2 -o /out_x11_window_test tests/x11_window_test.cpp $INC -lX11 -ldl

# xvfb-run gives the container a display; without one the GUI half would skip
# and the image would pass while proving nothing.
CMD set -e; \
    /out_sdk_tests | tail -3; \
    echo '=== x11 peer ==='; \
    xvfb-run -a /out_x11_window_test | tail -4; \
    echo '=== effect ==='; \
    xvfb-run -a /out_clap_host_test /out_SonoreSaturator.clap | tail -4; \
    echo '=== instrument ==='; \
    xvfb-run -a /out_clap_host_test /out_SonoreSynth.clap | tail -4; \
    echo '=== gui bridge ==='; \
    xvfb-run -a /out_clap_host_test /out_SonoreGuiProbe.clap --expect-bridge | tail -6
