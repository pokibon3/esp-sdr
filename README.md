# ESP-SDR firmware

<img src="docs/espargos-logo.png" width="40%" align="right" alt="ESPARGOS logo">

ESP-SDR turns the ESP32's built-in 2.4 GHz Wi-Fi radio into a
**software-defined radio (SDR)**. It lets you capture the radio signal itself
and process it in software, so you can view the spectrum and study signals
beyond ordinary Wi-Fi packets. No separate SDR hardware is needed. The
ESP32-C5 also supports reception in the 5 GHz band.

With the help of LLMs, we discovered an undocumented debug path that bypasses
the chip's fixed-function Wi-Fi modem. This gives software access to raw
radio samples, called **I/Q samples**, from the built-in receiver. ESP-SDR
captures short bursts of these samples and sends them to your
computer over USB or UART for analysis. It can also compute spectra on-device,
including continuous RF capture on selected chips.

<br clear="all">

![ESP32 radio architecture: an undocumented debug path connects the ADC/DAC to the CPU, bypassing the fixed-function Wi-Fi modem.](docs/sdr-bypass.png)

The diagram shows the hardware's receive and transmit paths; this firmware
currently implements reception only.

[Project overview](https://espargos.net/espsdr/) ·
[Browser SDR viewer](https://espargos.net/espsdr/app/) ·
[Browser firmware installer](https://espargos.net/espsdr/app/flash.html)

**Parts of the firmware code are AI-generated.**
While we have a very good understanding of how the IQ sampling functionality works on the ESP32-C61 chip (used in our ESPARGOS One array), making IQ sampling work on the whole range of ESP32 family chips would have been too much work without LLM support.

## Chip support

| Chip | Status | Native USB | UART0 TX / RX | Minimum flash | Special modes / firmware |
| --- | --- | --- | --- | --- | --- |
| ESP32 | ✅ | — | GPIO1 / GPIO3 | 2 MB | — |
| ESP32-C2 | ✅ (26 MHz crystal) | — | GPIO20 / GPIO19 | 2 MB | — |
| ESP32-C3 | ✅ | Serial/JTAG | GPIO21 / GPIO20 | 2 MB | — |
| ESP32-C5 | ✅ | Serial/JTAG | GPIO11 / GPIO12 | 2 MB | — |
| ESP32-C6 | ✅ | Serial/JTAG | GPIO16 / GPIO17 | 2 MB | — |
| ESP32-C61 | ✅ | Serial/JTAG | GPIO11 / GPIO10 | 2 MB | — |
| ESP32-H2 | ✅ | Serial/JTAG | GPIO24 / GPIO23 | 2 MB | — |
| ESP32-H21 | 🚧 | — | — | — | — |
| ESP32-H4 | 🚧 | — | — | — | — |
| ESP32-P4 | ❌ | — | — | — | — |
| ESP32-S2 | ✅ | USB-OTG CDC | GPIO43 / GPIO44 | 4 MB | — |
| ESP32-S3 | ✅ | Serial/JTAG | GPIO43 / GPIO44 | 2 MB | [Continuous decimated I/Q over USB (15.625–250 kSa/s)](#s3-streaming) |
| ESP32-S31 | ✅ | Serial/JTAG | GPIO58 / GPIO59 | 2 MB | [High Speed USB / Ethernet streaming at up to 40 MSa/s (experimental)](#s31-streaming) |

✅ Supported · 🚧 Not yet supported · ❌ Unsupported (no integrated radio).

USB, UART, and flash requirements above refer to the standard firmware; see each
special firmware variant for its board requirements.

## Special Chip- / Board-Specific Modes and Firmware

<a id="s31-streaming"></a>

### **ESP32-S31**: Ethernet / high-speed USB streaming

**Experimental:** The S31 streaming mode is under development. Signal quality,
including the remaining DC peak, still needs improvement.

The separate **`esp32s31-stream`** firmware targets the ESP32-S31 Function-CoreBoard
with Gigabit Ethernet and native high-speed USB. It streams receive-only I/Q to
**[SoapyESPSDR](https://github.com/ESPARGOS/SoapyESPSDR)** and includes an on-device web page for receiver controls and
status, with rates up to 20 MSa/s over USB and 40 MSa/s over Ethernet using
8-bit I plus 8-bit Q. The ordinary `esp32s31` firmware provides serial burst/FFT capture
for ESP-WebSDR. See [streaming build, architecture, and protocol](docs/s31-streaming.md).

![Gqrx displaying an LTE signal at 2.63 GHz, continuously sampled at 40 MSa/s over Ethernet with an ESP32-S31 and SoapyESPSDR.](docs/gqrx-esp-sdr.png)

<a id="s3-streaming"></a>

### **ESP32-S3**: Continuous I/Q streaming over USB

The standard **`esp32s3`** firmware includes an **`IQS`** mode for continuous,
decimated I/Q streaming over native USB Serial/JTAG. The second core filters
and decimates the 16 MSa/s capture stream by powers of two from 64 to 1024,
giving output rates from **250 down to 15.625 kSa/s**, with 4, 8 or 16 bits per
I and Q component. This mode requires native USB; UART is not supported.

Use [esp-sdr-bridge](https://github.com/z2labs/esp-sdr-bridge) to connect the
receiver to SDR++, SDR#, Gqrx or GNU Radio through SpyServer / rtl_tcp.
For custom clients, `IQS 0 64 8 6` starts a 250 kSa/s stream with 8-bit I and
8-bit Q until the host sends a byte to stop it. Frames include sample indices,
gap flags and CRC32 checksums; host stalls or processing overruns can cause
sample loss. See the [continuous I/Q protocol](docs/iq-stream.md) for command
options, sample formats, filtering and frequency-offset tuning.

<a id="s3-box-lite"></a>

### **ESP32-S3-BOX-Lite**: standalone LCD spectrum view

The optional `CONFIG_ESP_SDR_LCD_VIEW` build draws a 256-bin spectrum and
waterfall on the BOX-Lite's 320×240 ST7789 display without a host. It
repeats 40 ms SPEC captures and redraws between them, so it analyzes only
part of the signal. Each run keeps every bin's peak. The line shows the
median of the last three runs, rising within a run and decaying over about
four. The waterfall shows each run, coloured relative to the noise level.
The trace scale is fixed from −20 to −80 dBFS, as in the host viewer
(code / 2 − 84.3), with a 20 dB grid. Wi-Fi channel numbers appear below
the trace.

ENTER selects the highlighted setting: CENTER, SPAN, STEP or GAIN. PREV and
NEXT lower and raise it. CENTER moves by STEP (1, 5, 10 or 20 MHz;
default 10). SPAN is 10, 20, 40 or 80 MHz; the 10 and 20 MHz spans show the
centre of a 16 or 40 MS/s capture. GAIN is a manual gain index in steps of
10, about 1 dB per index above 50 on the tested board. The view starts at
2442 MHz (channel 7), an 80 MHz span and index 35. AGC would shift whole
spectra between the short runs.

Under the host's automatic filter, the 40 and 80 MHz spans use a 40 MHz
analog filter. The automatic filter passes only about 25 MHz, and the
widest one raised every bin with spurious energy on the tested board, so
the 80 MHz span shows roughly channels 2–12. A strong transmitter within a
few centimetres compresses the other signals. Any host command restores
the host's gain and pauses the view; it resumes when the serial lease is
released or expires after five seconds of silence. `LCDDUMP?` returns the
last run's peaks and the drawn line, and `LCDINPUT?` the raw button or touch
reading, for diagnosis. The standard `esp32s3`
image is unchanged.

```sh
idf.py -B build-s3-boxlite -DIDF_TARGET=esp32s3 -DSDKCONFIG=sdkconfig.s3-boxlite \
  "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults.esp32s3;sdkconfig.defaults.esp32s3-boxlite" build
```

The same view runs on the **M5Stack CoreS3** with
`sdkconfig.defaults.esp32s3-cores3` in place of the BOX-Lite overlay. Its
ILI9342C/E panel is powered through the AXP2101 and AW9523, and the
FT6336 touch panel replaces the buttons. Tapping a mode box selects that
setting. The three touch buttons below the screen, like the rest of the
screen, lower the setting (left), cycle it (middle) and raise it (right).

#### 使い方（日本語）

ESP32-S3-BOX-Lite（または M5Stack CoreS3）の画面に、スペクトルとウォーターフォールを PC なしで表示する版です。

**ビルドと書き込み**

[Build and flash](#build-and-flash) の手順で ESP-IDF を用意し、`export.sh` を読み込んでから実行します。ポート名は `ls /dev/cu.usbmodem*`（macOS）などで確認してください。

```sh
idf.py -B build-s3-boxlite -DIDF_TARGET=esp32s3 -DSDKCONFIG=sdkconfig.s3-boxlite \
  "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults.esp32s3;sdkconfig.defaults.esp32s3-boxlite" build
idf.py -B build-s3-boxlite -p /dev/cu.usbmodemXXXX flash
```

書き込むと、電源を入れるだけで表示が始まります。起動時は中心 2442 MHz（Wi-Fi ch7）、表示幅 80 MHz、ゲイン 35 です。

**画面**

| 場所 | 内容 |
| --- | --- |
| 上部 | CENTER / SPAN / STEP / GAIN の値。選択中の項目はオレンジで強調表示。中心が Wi-Fi チャンネルに一致すると ch 番号も表示 |
| スペクトル | −20〜−80 dBFS 固定、20 dB ごとの目盛り（値は左側）。中央の縦線が中心周波数 |
| チャンネル行 | Wi-Fi のチャンネル番号（2.4 GHz 帯 1〜14、5 GHz 帯）と中心位置の点線 |
| 周波数 | 左端・中心・右端の周波数（MHz） |
| ウォーターフォール | 上が最新。40 ms ごとの計測を色で表示 |

**ボタン**

| ボタン | 動作 |
| --- | --- |
| 中央 | 設定項目を CENTER → SPAN → STEP → GAIN の順に切り替え |
| 左 | 選択中の値を下げる |
| 右 | 選択中の値を上げる |

| 項目 | 内容 |
| --- | --- |
| CENTER | 中心周波数。STEP の幅ずつ変わり、押し続けると連続で動く（100〜6000 MHz） |
| SPAN | 表示幅 10 / 20 / 40 / 80 MHz |
| STEP | CENTER の刻み幅 1 / 5 / 10 / 20 MHz（既定 10 MHz。5 MHz にすると Wi-Fi 1 チャンネルずつ） |
| GAIN | 受信ゲインの番号。10 ずつ変わる（実測では番号 50 以上で 1 あたり約 1 dB） |

**ブラウザビューアとの併用**

PC からコマンドが届くと LCD 表示は止まり、[ブラウザ SDR ビューア](https://espargos.net/espsdr/app/) などで通常どおり使えます。PC 側が切断するか、5 秒間何も送らなければ LCD 表示に戻ります。LCD 表示中に固定していたゲインは、PC から使うときには元の設定（通常は AGC）に戻ります。

**M5Stack CoreS3**

同じ表示を M5Stack CoreS3 でも使えます。ビルド時の設定ファイルを `sdkconfig.defaults.esp32s3-cores3` に替えます。

```sh
idf.py -B build-s3-cores3 -DIDF_TARGET=esp32s3 -DSDKCONFIG=sdkconfig.s3-cores3 \
  "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults.esp32s3;sdkconfig.defaults.esp32s3-cores3" build
idf.py -B build-s3-cores3 -p /dev/cu.usbmodemXXXX flash
```

CoreS3 にはボタンがないので、タッチで操作します。

| タッチする場所 | 動作 |
| --- | --- |
| 上部の CENTER / SPAN / STEP / GAIN の枠 | その項目を選ぶ |
| 画面下のタッチボタン左（または画面の左 1/3） | 選択中の値を下げる（押し続けると連続） |
| 画面下のタッチボタン中央（または画面の中央 1/3） | 項目を順に切り替え |
| 画面下のタッチボタン右（または画面の右 1/3） | 選択中の値を上げる（押し続けると連続） |

画面の内容とブラウザビューアとの併用は BOX-Lite 版と同じです。

**注意**

- 40 ms 計測しては描画する繰り返しなので、信号の一部を取りこぼします。
- スペクトルの線は 40 ms ごとのピーク値の直近 3 回の中央値です。平均電力を表示するブラウザより数 dB 高めに出ます。
- 80 MHz 表示でも、受信部の特性で両端（およそ ch1 以下と ch13 以上）は感度が落ちます。
- 数 cm の距離に強い送信機があると受信部が飽和し、ほかの信号が小さく見えます。
- 標準の `esp32s3` ファームには影響しません。

## On-chip spectrum streaming

The firmware can compute FFTs on the device and send compact spectra instead
of raw I/Q. The viewer queries each device's supported rates, FFT sizes and
transport before offering this mode.

| Chip | Continuous RF capture with on-chip FFT | Snapshot FFT |
| --- | --- | --- |
| ESP32 | — | 256–2048 bins; 16/40/80 MS/s; UART |
| ESP32-C2 | — | 256–2048 bins; 80/40/16 MS/s; UART |
| ESP32-C3 | — | 256–2048 bins; 80 MS/s; USB or UART |
| ESP32-C5 | — | 256–2048 bins; 4/8/10/20/40/80 MS/s |
| ESP32-C6 | 256 bins; 80 MS/s; native USB | 512–2048 bins over USB; 256–2048 over UART |
| ESP32-H2 | — | 256–2048 bins; 6.4/10.667/16/32 MS/s; USB or UART |
| ESP32-C61 | 256 bins; 4/8/10/20/40/80 MS/s; native USB | 512–2048 bins over USB; 256–2048 over UART |
| ESP32-S2 | — | 256–2048 bins; 16/40/80 MS/s; USB or UART |
| ESP32-S3 | 256–2048 bins; 16/40/80 MS/s; native USB | — |
| ESP32-S31 | 256–2048 bins; 4/8/10/20/40/80 MS/s; native USB | Same FFT sizes and rates over UART |

Continuous capture keeps the RF writer running, but the CPU analyzes only
selected FFT windows. It does **not** deliver every sample or guarantee that
every short RF event will be visible. Snapshot FFT stops reception between
captures; the viewer labels these gaps explicitly. Raw I/Q capture remains
available separately.

The original S3 Turbo Mode was developed by Zoltan Doczi from
[Z2Labs](https://www.z2labs.io/). The shared implementation extends it with
C6/C61 bank rotation, S31 dual-core SIMD processing with
continuous bank rotation, and portable snapshot FFTs.
See [spectrum protocol and hardware validation](docs/spectrum.md) for the
wire format, limitations and test results. The S3 ring diagnostic host tool
is [tools/s3_ring.py](tools/s3_ring.py).

GPIO outputs can be controlled from the browser’s GPIO section or the serial
protocol. Firmware reports available pins; each supports high impedance (Z),
low (0), or high (1). See [GPIO controls](docs/rx-controls.md#gpio-outputs).

## Commands and transport

Connect over native USB or a 3.3 V USB-to-UART adapter with crossed TX/RX
and common ground, using the pins above. Both interfaces carry the same
request/response protocol: send newline-terminated ASCII commands and read
text replies. Capture replies also include a binary I/Q payload.

Query `INFO` and `CAPS` to identify the firmware and supported features.
`VERSION?` reports the Git revision and UTC build date/time; see
[firmware version reporting](docs/firmware-version.md).
`LIMITS?` reports receive-control limits, `RANGE?` reports the tuning range,
and `TRANSPORT?` identifies the active interface. Configure reception with
`FREQ <MHz>`, `BANDWIDTH <MHz>` and `GAIN` commands.

Request a snapshot with `CAP16 <samples> <rate-index>` for signed 8-bit I/Q
or `CAP20 <samples> <rate-index>` for packed signed 10-bit I/Q. The reply is
`DATA <samples> <crc32-hex> <capture-microseconds>`, followed by exactly
`ceil(samples × bits-per-component × 2 / 8)` binary bytes. Verify the
payload CRC32 before using the samples. Rate indices 0–6 select 80, 40, 20, 10,
8, 4 or 16 MS/s respectively; use only rates advertised by `LIMITS?`.

Finish reading each reply before sending another command. Failures return
`ERR <reason>`. `SYNC <nonce>` echoes the nonce to let clients resynchronize
after an incomplete transfer. One client controls the radio at a time;
`RELEASE` or five seconds of idle time releases it, while other clients
receive `ERR busy`.

## Build and flash

[firmware-targets.json](firmware-targets.json) lists the supported firmware variants and
pins their ESP-IDF commits, including the preview SDK for S31. Check out the
matching SDK, initialize its submodules, run `install.sh <target>`, and source
`export.sh`.

Use a separate build directory and configuration for each chip:

```sh
idf.py -B build-s3 -DIDF_TARGET=esp32s3 \
  -DSDKCONFIG=sdkconfig.s3 \
  -DSDKCONFIG_DEFAULTS=sdkconfig.defaults.esp32s3 build
idf.py -B build-s3 -p /dev/ttyACM0 flash
```

Substitute the target and paths for your chip. S31 also requires `idf.py --preview`.

The C2 firmware uses a **26 MHz crystal**, as on the tested ESP8684H board.
For a 40 MHz board, select `CONFIG_XTAL_FREQ_40=y` in menuconfig and rebuild;
the packaged browser image requires 26 MHz. C2 uses UART0 at 2 Mbaud by
default. With a CH340 bridge, use the viewer's **Switch to 1 Mbaud** warning
when transfers lose bytes; baud changes are session-only. C2 provides raw
IQ8/IQ10 captures, hardware/manual gain, and snapshot FFTs. Continuous capture
is not advertised; approximate analog bandwidth covers 12–20 MHz.
See [C2 backend and validation](docs/esp32c2.md).

The `esp32h2` firmware uses the Bluetooth PHY capture engine and supports
both native USB Serial/JTAG and UART0. It supports 32, 16, approximately 10.667 and 6.4 MS/s hardware sampling,
plus approximately 4–11 MHz analog bandwidth control.
See [H2 backend and validation](docs/esp32h2.md).

## Source layout

- `main/targets/<target>/`: chip receiver or adapter, tuning helpers, and the
  linker guard for its capture SRAM. CMake selects only the requested target.
- `main/targets/esp32s31/burst/`: serial IQ capture and on-chip FFT firmware.
- `main/targets/esp32s31/streaming/`: continuous USB/Ethernet IQ application for
  SoapyESPSDR. Both S31 firmware variants share `main/targets/esp32s31/tuning.h`.
- `main/families/c5_c6_c61/`: receiver shared by C5, C6, and C61; its `chip.h`
  comes from the selected target directory.
- `main/common/`: burst serial transport, gain control, limits, and bandwidth
  helpers. The gain-table wrapper is linked only for C61 and S31.
- `main/diagnostics/`: optional register probes, excluded from release exports.
- `platform/esp32s2/`: pinned ROM USB CDC compatibility component.

The application component and UART configuration stay in `main/`. Target SDK
defaults stay at the repository root for the build tools and ESP-IDF defaults
lookup. The firmware uses the burst protocol over UART/native USB; the separate
`main/targets/esp32s31/streaming/` application implements the receive-only Ethernet and
vendor USB streaming firmware.

Run `python3 -m unittest discover -s tests` for host checks. Build every firmware variant
with `tools/build_firmware.py` and its pinned SDK before distributing a change;
the CI matrix does this automatically. Preserve the target SRAM guards and
gain-table linker wrappers when moving or refactoring receiver code.

## Receive controls

Hardware AGC is the default. `GAIN MANUAL <index>` sets manual gain;
`GAIN HARDWARE` restores AGC. `LIMITS?` reports available gain indices,
bandwidths, sample rates and bit depths. `BANDWIDTH <MHz>` sets approximate
analog bandwidth; zero selects the widest setting.

All supported burst targets accept tuning attempts from **100–6000 MHz in 1 MHz steps**.
The viewer shows an informational warning outside 2400–2483.5 MHz, with
5150–5895 MHz also treated as the supported 5 GHz Wi-Fi band on C5. The warning never blocks tuning.
These are software attempt limits, not a guaranteed reception range.
ESP32, S2, S3, C2, C3 and C6 automatically use the experimental **5/6 LO mode from
1842–2209 MHz**, extending reception down to about **1.84 GHz** on the tested
boards. No extra command or browser setting is needed. An equivalent divider
mode is not yet verified on C5, C61, H2 or S31. C61 and S31 instead recover
failed low-band PLL calibration automatically, with reception verified down
to **2.18 GHz and 2.15 GHz**, respectively, on the tested boards. This uses
hardware capacitor calibration and also applies to S31 streaming; the
achievable range depends on the individual chip.

- **ESP32:** 80/40/16 MS/s.
- **C2:** 80/40/16 MS/s; up to 8,190 complex samples; approximately 12–20 MHz analog bandwidth.
- **H2:** 32/16/10.667/6.4 MS/s; approximately 4–11 MHz analog bandwidth; up to 16,380 complex samples.
- **C3:** 80 MS/s; 14–62 MHz analog bandwidth.
- **C5:** 11–48 MHz bandwidth; selects its 5 GHz RF path above 3000 MHz.
- **C61:** 80/40/20/10/8/4 MS/s; 13–54 MHz bandwidth.
- **C6:** 80 MS/s; 12–54 MHz bandwidth.
- **S2:** 80/40/16 MS/s; 15–60 MHz bandwidth; up to 12,284 complex samples.
- **S3:** 13–69 MHz bandwidth.
- **S31:** 80/40/20/10/8/4 MS/s; 13–54 MHz bandwidth.

Captures have gaps; nominal sample rates exceed sustained serial throughput.
Gain and power are uncalibrated. Extended tuning does not guarantee PLL lock
or reception; the viewer uses the ISM-band warning described above.

See [receive-control details](docs/rx-controls.md).

## Contributors

<table>
  <tr>
    <td align="center">
      <a href="https://github.com/Jeija">
        <img src="https://github.com/Jeija.png?size=160" width="80" height="80" alt="Florian Euchner"><br>
        <b>Florian Euchner</b>
      </a>
    </td>
    <td align="center">
      <a href="https://github.com/zodoczi">
        <img src="https://github.com/zodoczi.png?size=160" width="80" height="80" alt="Zoltan Doczi"><br>
        <b>Zoltan Doczi</b>
      </a>
    </td>
  </tr>
</table>

Special thanks to [h0m3us3r](https://github.com/h0m3us3r) for providing
[eSpDR](https://github.com/h0m3us3r/eSpDR), whose 5/6 LO investigation informed
our lower-frequency tuning implementation.

## License

ESP-SDR is licensed under the GNU General Public License as published by the
Free Software Foundation, either version 3 of the License, or (at your option)
any later version (`GPL-3.0-or-later`). See [LICENSE](LICENSE) for the full terms.
It is provided without any warranty, including implied warranties of
merchantability or fitness for a particular purpose.

**We chose the GPL because we want improvements to ESP-SDR to make their way
back to the community**. When you distribute modified versions, the GPL requires
you to make the corresponding source available to recipients under the GPL,
so they can study, share, and build on those improvements. We encourage you to
contribute changes upstream, but the GPL does not require upstream submissions
or publication of private modifications. See the
[GNU GPL FAQ](https://www.gnu.org/licenses/gpl-faq.html#UnreleasedMods).

The discovery of the capture mechanism itself is not protected by copyright:
copyright covers the code and other copyrightable expression, not the
underlying facts, ideas, or methods. **You are free to independently implement
the mechanism in your own projects under a license of your choice**.

Third-party components retain their own licenses and copyright notices,
including the Apache-2.0 ESP-IDF compatibility code in
`platform/esp32s2/esp_usb_cdc_rom_console/`, the pinned
[ESP-DSP component](components/esp-dsp/LICENSE), and the derived FFT kernels
in `main/targets/esp32s3/s3_fft_rnd.S` and `main/targets/esp32s31/burst/s31_fft_rnd.S`.
