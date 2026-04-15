# Esparagus Snapclient (dlsHome Fork)

> **This is a fork of [sonocotta/esparagus-snapclient](https://github.com/sonocotta/esparagus-snapclient).**
>
> The original project was created and is maintained by [Sonocotta](https://github.com/sonocotta). All credit for the original Esparagus Snapclient design, architecture, and implementation belongs to the original author and contributors. This fork builds on their excellent work and is released under the same [BSD 3-Clause License](LICENSE) as the upstream project.

## About the Original Project

[Esparagus Snapclient](https://github.com/sonocotta/esparagus-snapclient) is a synchronous multiroom audio streaming client for [Snapcast](https://github.com/CarlosDerSeher/snapclient) designed for ESP32-based audio devices, with particular focus on the [Sonocotta Esparagus](https://www.tindie.com/stores/sonocotta/) series of audio boards. The upstream project supports a wide range of hardware including the Esparagus HiFi, Loud, Amped, and Louder boards, Espressif LyraT development boards, and AI-Thinker boards. For the full list of supported hardware and the web installer, see the [upstream repository](https://github.com/sonocotta/esparagus-snapclient).

## Fork Purpose

This fork exists to support the **dlsHome** custom DAC board based on the `PCM5102A`, and to keep that target building and running reliably on an `ESP32-S3` with `16 MB flash` and `PSRAM`.

The maintained target in this fork is the **ESP32-S3 + PSRAM + PCM5102A** platform. The code has also been updated to behave correctly with higher audio sample rates and modern Snapcast playback use on that hardware.

## Fork Scope

- Primary hardware target: custom `dlsHome` board with `PCM5102A`
- Primary MCU target: `ESP32-S3`
- Memory target: `16 MB flash` with `PSRAM`
- Supported network modes: `wifi` and `ethernet`
- Supported build entry point: [`build_pipeline.sh`](build_pipeline.sh)

This fork keeps a single codebase. The only intentional build-time switch between the two supported pipelines is the network transport:

- `wifi`
- `ethernet`

ESP32 does not use Wi-Fi and Ethernet at the same time in this project. The selected pipeline chooses the appropriate configuration overlay at build time.

## What Changed In This Fork

- Updated the project to compile and run well on `ESP32-S3` hardware with `PSRAM`
- Added a maintained build flow for both `wifi` and `ethernet`
- Added a local-only Wi-Fi credentials overlay so SSID and password do not need to live in tracked config files
- Added support for the `dlsHome` board's dedicated DAC/AMP power-switch GPIOs
- Added support for the `dlsHome` board's dedicated dual status LEDs
- Improved playback behavior and recovery for modern use on the maintained target
- Improved handling of higher audio sample rates on the maintained hardware path
- Kept legacy configuration files only as reference under [`legacy/`](legacy/)

## Support Status

Supported and maintained:

- `ESP-IDF v5.1.1`
- `ESP32-S3`
- `PCM5102A` custom board flow
- `wifi` pipeline
- `ethernet` pipeline

Not supported in this fork:

- Old board-specific build presets from upstream
- Legacy `sdkconfig.*` presets now moved to [`legacy/`](legacy/)
- Older compile/flash instructions based on directly calling `idf.py build flash monitor` against the repo root config

The old build pipelines are not tested and should be treated as historical reference only.

## Repository Layout

- [`build_pipeline.sh`](build_pipeline.sh): supported build, flash, and monitor entry point
- [`sdkconfig`](sdkconfig): base project config
- [`sdkconfig.overlay.wifi`](sdkconfig.overlay.wifi): Wi-Fi pipeline overlay
- [`sdkconfig.overlay.ethernet`](sdkconfig.overlay.ethernet): Ethernet pipeline overlay
- [`sdkconfig.local.wifi.example`](sdkconfig.local.wifi.example): template for local Wi-Fi credentials
- [`legacy/`](legacy/): old board-specific `sdkconfig` presets retained only for reference

## Setup

Clone the repository and its submodules:

```bash
git clone --recursive https://github.com/dlsnet/esparagus-snapclient.git
cd esparagus-snapclient
```

If you already cloned without submodules:

```bash
git submodule update --init --recursive
```

Install and use `ESP-IDF v5.1.1`.

Typical local environment:

```bash
export IDF_PATH="$HOME/esp/esp-idf"
source "$IDF_PATH/export.sh"
```

`build_pipeline.sh` also tries to use these default locations:

- `IDF_PATH=$HOME/esp/esp-idf`
- `IDF_PYTHON_ENV_PATH=$HOME/.espressif/python_env/idf5.1_py3.14_env`

## Wi-Fi Credentials

For the `wifi` pipeline, create a local credentials file:

```bash
cp sdkconfig.local.wifi.example sdkconfig.local.wifi
```

Then edit `sdkconfig.local.wifi` and set:

- `CONFIG_WIFI_SSID`
- `CONFIG_WIFI_PASSWORD`

That file is intentionally ignored by git and is only applied to the `wifi` pipeline.

## Build, Flash, And Monitor

Use [`build_pipeline.sh`](build_pipeline.sh) for all supported builds.

If you run it without arguments, it will prompt for the pipeline:

```bash
./build_pipeline.sh
```

Build Wi-Fi firmware:

```bash
./build_pipeline.sh wifi build
```

Build Ethernet firmware:

```bash
./build_pipeline.sh ethernet build
```

Flash Wi-Fi firmware:

```bash
./build_pipeline.sh wifi -p /dev/cu.usbmodemXXXX flash
```

Monitor Wi-Fi firmware:

```bash
./build_pipeline.sh wifi -p /dev/cu.usbmodemXXXX monitor
```

Flash Ethernet firmware:

```bash
./build_pipeline.sh ethernet -p /dev/cu.usbmodemXXXX flash
```

Monitor Ethernet firmware:

```bash
./build_pipeline.sh ethernet -p /dev/cu.usbmodemXXXX monitor
```

The script writes outputs to pipeline-specific build directories:

- `build-wifi/`
- `build-ethernet/`

Use the matching pipeline when monitoring so `idf.py` reads the correct ELF file.

## Ethernet Notes

The maintained Ethernet overlay currently targets a `W5500` SPI Ethernet module with the pin assignment defined in [`sdkconfig.overlay.ethernet`](sdkconfig.overlay.ethernet).

If your hardware differs, update that overlay before building the `ethernet` pipeline.

## Audio Notes

This fork is maintained around the `PCM5102A` playback path on the `dlsHome` board.

The maintained path has been updated for better behavior with higher sample-rate playback on the supported hardware. Legacy board combinations and legacy audio configurations have not been revalidated in this fork.

## dlsHome Board Specifics

This fork contains `dlsHome`-specific support for discrete audio power control and separate idle/playback LEDs.

Audio power-switch GPIOs used by this fork:

- `GPIO17`: DAC power / DAC analog enable
- `GPIO16`: amplifier power / AMP enable

Status LED GPIOs used by this fork:

- `GPIO2`: idle/status LED on the `dlsHome` board
- `GPIO5`: playback LED on the `dlsHome` board

On the `dlsHome` board these LEDs are used as:

- Solid red: device is powered on, has found the Snapserver, and is waiting for an audio feed
- Solid green: device is actively playing an audio feed
- Flashing red: device failed to find the Snapserver or has a connectivity issue

The current implementation also uses a faster red flash as a critical-failure indication immediately before an automatic reboot after repeated recovery failures.

## Legacy Configurations

The old root-level `sdkconfig.*` presets have been moved into [`legacy/`](legacy/) to keep the active build flow clear.

Those files are kept only as reference material. They are not tested and are not supported.

## Acknowledgments

This project is a fork of [sonocotta/esparagus-snapclient](https://github.com/sonocotta/esparagus-snapclient), created by [Sonocotta](https://github.com/sonocotta). The original project provides an excellent ESP32-based Snapcast client with broad hardware support and a web installer. We are grateful for their work in making multiroom audio accessible on ESP32 platforms.

The Snapcast ESP32 client itself is based on the work by [CarlosDerSeher/snapclient](https://github.com/CarlosDerSeher/snapclient).

## Links

- [Original Esparagus Snapclient](https://github.com/sonocotta/esparagus-snapclient) by Sonocotta
- [Snapcast ESP32 port](https://github.com/CarlosDerSeher/snapclient) by CarlosDerSeher
- [Sonocotta Hardware](https://www.tindie.com/stores/sonocotta/)
- [ESP-IDF Documentation](https://docs.espressif.com/projects/esp-idf/)

## License

This project is licensed under the **BSD 3-Clause License**, the same license as the [original upstream project](https://github.com/sonocotta/esparagus-snapclient).

See the [LICENSE](LICENSE) file for the full license text.

```
BSD 3-Clause License

Copyright (c) 2025, sonocotta
Copyright (c) 2025-2026, dlsnet (fork modifications)

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```
