# CrossPoint Pal

**CrossPoint Pal is a downstream fork of [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader)**
(`crosspoint-reader/crosspoint-reader`), open-source e-reader firmware for small
e-ink devices. It is not the official upstream project and not affiliated with
Xteink or any device manufacturer.

The fork follows a different product scope from upstream: reading stays
first-class, and the firmware also uses the strengths of e-ink (persistent
image, very low static power draw) for features upstream does not target —
currently **enhanced image rendering** and a **low-power photo-frame /
slideshow mode** that runs from deep sleep on a timer. The product direction
is described in the [fork manifesto](./MANIFESTO.md).

We regularly sync from upstream `develop` and keep upstream's reader core
intact. Reusable, product-neutral pieces (pure policy modules, host-tested
rendering primitives) are kept separable from downstream-only features so they
can be upstreamed later.

![CrossPoint Pal running on Xteink device](./docs/images/cover.jpg)

## Quick answers

| Question | Answer |
| --- | --- |
| What is this? | Downstream CrossPoint firmware (CrossPoint Pal) with enhanced image rendering and a low-power slideshow. |
| How does it differ from upstream? | Same reader core, plus image render controls (brightness / gamma / contrast / quantizer), persisted render profiles, and the slideshow feature set below. See [What's different](#whats-different-from-upstream). |
| Which device has a hardware-tested binary? | **XTEINK X4 Classic only.** Other targets may compile from source but are NOT hardware-verified here. |
| Where to download? | [Releases](https://github.com/kivarun/crosspoint-pal/releases) in this repository. |
| How to install? | [Installation](#install-firmware) below — custom .bin web flasher or `esptool`. |
| How to go back to upstream? | [Back to official upstream firmware](#back-to-official-upstream-firmware) below. |

## What's different from upstream

Two finished feature groups are ahead of upstream `develop`; everything else
matches the upstream reader core this fork is based on.

### Image viewer and image rendering

- Improved BMP/image viewer with an options modal (tone controls, image info,
  delete, sleep-cover assignment).
- Tone render controls for BMP decoding: brightness, gamma, contrast, and
  4-level quantizer selection, with exact pass-through identity at defaults.
- Persisted **viewer render profile** (your tone settings survive restarts)
  and a separate persisted **sleep render profile** ("Use for sleep
  rendering").
- Grayscale-aware BMP presentation on capable panels, driven by explicit
  grayscale-capability policy instead of one-size-fits-all transfers.

### Low-power slideshow

- **Image Viewer slideshow**: start a timed slideshow from the currently open
  image; each next frame is shown from a deep-sleep timer wake.
- **Sleep Screen = Slideshow**: the sleep screen itself becomes a slideshow
  from `/.sleep` (legacy `/sleep` fallback).
- Shared slideshow interval: 1 / 5 / 10 / 30 minutes.
- Frame **Order**: Forward (A→B→C→A), Reverse (C→B→A→C), Random (every image
  is shown once in randomized order before a new cycle begins; no immediate
  repeat across cycles). Adapted from CrossPoint upstream PR #3841 by
  @gkaindl.
- **Low-battery cutoff**: at ≤10% charge without external power the sleep
  slideshow stops wake-looping and falls back to an ordinary static sleep
  screen.
- Auto-sleep timeout starts the slideshow too (Sleep Screen = Slideshow takes
  priority over the after-timeout Quick Resume option).
- Grayscale-safe presentation on X4 Classic (UC8279): every timer-wake frame
  scrubs the panel with an absolute refresh base before loading gray planes,
  so frames replace — not stack on — the retained physical image.

## Supported hardware policy

- **XTEINK X4 Classic**: hardware-tested. Release binaries are published for
  this device only.
- Other upstream targets (X3, original X4, X4 Pro, Sticky, PaperMono, …) may
  still **build from source** (`pio run -e <env>`), but nothing here claims
  they work on real hardware: we have not run our hardware UAT on them. Source
  that builds is not support.

## ⚠️ USB-locked devices: read before flashing

Some Xteink units ship with USB flashing locked and are unlocked/re-locked
through Xteink's own unlock tool. The re-lock flow only treats a small set of
official firmwares as known. This fork is a **separate downstream firmware**:
if you install it on a USB-locked (or re-locked) device, you may be left with
**no way to return** — without a verified recovery path you could permanently
lose the ability to reflash that device.

**Do not install this fork on a USB-locked / re-locked XTEINK device unless
you already have a verified recovery path.** Unlocked devices (e.g. bought
directly from xteink.com) can always be re-flashed normally.

## Install firmware

> Only for unlocked devices or devices whose recovery path you have verified
> yourself. See the [USB-locked warning](#️-usb-locked-devices-read-before-flashing) above.

### Custom .bin web flasher

1. Connect the X4 Classic via USB-C and wake the device.
2. Download `crosspoint-pal-<version>-x4c.bin` from
   [Releases](https://github.com/kivarun/crosspoint-pal/releases).
3. Open the CrossPoint web flasher (https://crosspointreader.com/#flash-tools),
   select **X4 Classic**, and use **Custom .bin** with the downloaded file.

The fork's releases are not part of the official release picker — always use
the **Custom .bin** flow with the file you downloaded here.

### esptool (application image)

The release asset is an **application image** written at offset `0x10000`
(no bootloader / partition flashing required):

```bash
pip install esptool
esptool --chip esp32s3 --port /dev/ttyACM0 \
  write-flash 0x10000 crosspoint-pal-0.1.0-x4c.bin
```

### OTA updates

Installed Pal builds check **this repository's** releases only
(`kivarun/crosspoint-pal`). They never offer official upstream firmware.

### Back to official upstream firmware

Flash any official CrossPoint release for the X4 Classic with the same
custom .bin flow or `esptool` command above, using the upstream binary from
https://github.com/crosspoint-reader/crosspoint-reader/releases.

## Fork versioning and updates

Fork releases are `0.1.0`, `0.1.1`, `0.2.0`, … (bare semver, no `v` prefix —
the OTA plumbing keys release assets by `tag_name`). Each release notes its
exact upstream `develop` base.

If you were running an earlier **development** build (self-identifying as
upstream `1.6.5`-derived versions), the OTA comparator will not offer the
0.1.0 release as an "upgrade": flash `0.1.0` once manually, after which
later fork releases update over the air normally.

## Branch model

- `main` — released/stable fork state (planned; default branch after review).
- `fork/develop` — downstream integration for the next release.
- `feature/*` — feature work.
- `upstream/develop` — the upstream tracking source this fork rebases on.

## Upstream features

The reader core below is inherited from upstream CrossPoint (this fork tracks
upstream `develop`); upstream remains the better source for reader-core
improvements.

## Custom SD-card fonts

On devices with external RAM enabled in CrossPoint, copy `.ttf`, `.otf`, or `.ttc` files to the SD card and select them as reader fonts. Put one file in `/fonts/` or `/.fonts/`, or put one family's files in a subfolder. See the [SD card font guide](./docs/sd-card-fonts.md) for the folder layout and styles.

On other devices, convert the font to `.cpfont` first. `.cpfont` files also work on devices with external RAM enabled and have better performance. No firmware reflash is needed to add fonts.

To make `.cpfont` files:

1. Go to https://crosspointreader.com/fonts and open the "SD-card font builder" form.
2. Upload up to four styles (regular, bold, italic, bold-italic), set the family name, point sizes, and Unicode range.
3. Download the generated `.cpfont` files.
4. Copy them to your SD card under `/fonts/YourFont/` (or `/.fonts/YourFont/` to hide the folder).
5. Select the font on the device from the font settings.

Conversion runs the firmware repo's `lib/EpdFont/scripts/fontconvert_sdcard.py` script unmodified, so output matches a local host build.

---

## Documentation

- [User Guide](./USER_GUIDE.md)
- [Web server usage](./docs/webserver.md)
- [Web server endpoints](./docs/webserver-endpoints.md)
- [Project scope](./SCOPE.md)
- [Contributing docs](./docs/contributing/README.md)
- [Touch and UI development](./docs/contributing/touch-and-ui.md) - how to build new screens on the FreeInkUI activity bases (UiListActivity and friends), plus build envs for the non-Xteink touch devices

---

## Development quick start

### Prerequisites

- [pioarduino PlatformIO Core](https://github.com/pioarduino/platformio-core) or [VS Code + pioarduino IDE](https://github.com/pioarduino/pioarduino-vscode-ide)
- Python 3.8+
- `clang-format` 21
- USB-C cable supporting data transfer

### Setup

```bash
git clone --recursive https://github.com/kivarun/crosspoint-pal
cd crosspoint-pal

# if cloned without --recursive:
git submodule update --init --recursive
```

### Nix/NixOS

Nix/NixOS users can enter the development shell with either `nix develop` (flakes) or `nix-shell`:

```bash
nix develop -f nix
# or
nix-shell nix
```

To flash a connected ESP32-C3 device, enable PlatformIO's udev rules in your NixOS configuration:

```nix
services.udev.packages = with pkgs; [ platformio-core.udev ];
```

After rebuilding the system configuration, reconnect the device or reload udev rules.

### Build / flash / monitor

```bash
pio run --target upload
```

### Contributor pre-PR checks

```bash
./bin/clang-format-fix
pio check -e default
pio run -e default
```

### Debugging

After flashing the new features, it’s recommended to capture detailed logs from the serial port.

First, make sure all required Python packages are installed:

```python
python3 -m pip install pyserial colorama matplotlib
```

After that run the script:

```sh
# For Linux
# This was tested on Debian and should work on most Linux systems.
python3 scripts/debugging_monitor.py

# For macOS
python3 scripts/debugging_monitor.py /dev/cu.usbmodem2101
```

Minor adjustments may be required for Windows.

---

## Internals

CrossPoint Reader is pretty aggressive about caching data down to the SD card to minimise RAM usage. The ESP32-C3 only has ~380KB of usable RAM, so we have to be careful. A lot of the decisions made in the design of the firmware were based on this constraint.

### Data caching

The first time chapters of a book are loaded, they are cached to the SD card. Subsequent loads are served from the
cache. This cache directory exists at `.crosspoint` on the SD card. The structure is as follows:

```text
.crosspoint/
├── epub_<hash>/         # one directory per book, named by content hash
│   ├── progress.bin     # reading position (chapter, page, etc.)
│   ├── cover.bmp        # generated cover image
│   ├── book.bin         # metadata: title, author, spine, TOC
│   ├── css_rules.cache  # parsed CSS rule cache
│   ├── img_*            # rendered image cache files
│   └── sections/        # per-chapter layout cache
│       ├── 0.bin
│       ├── 1.bin
│       └── ...
├── settings.json        # device settings
├── state.json           # resume/runtime state
└── recent.json          # recent books list
```

Removing `/.crosspoint` clears all cached metadata and forces a full regeneration on next open. Book deletes, overwrites, and moves done through the firmware or web UI clear or re-key matching caches; manual SD-card edits may leave stale cache directories behind.

For more details on the internal file structures, see the [file formats document](./docs/file-formats.md).

---

## Contributing

Contributions are welcome. For things to work on, see the [fork manifesto](./MANIFESTO.md) (the product
boundary) and the [contributing docs](./docs/contributing/README.md) (inherited upstream engineering
guides). Please open pull requests against this fork.

---

## Attribution and license

Fork of [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) — all credit for the
reader core, the FreeInk SDK ecosystem and the original engineering goes to the upstream CrossPoint
contributors. This fork tracks upstream `develop` and keeps the MIT
[LICENSE](./LICENSE); modifications are published under the same terms.

CrossPoint Reader is **not affiliated with Xteink or any device manufacturer**.
