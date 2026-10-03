# Changelog

CrossPoint Pal releases only; upstream base is noted per release. Versioning
is bare semver without a `v` prefix (the OTA plumbing keys release assets by
`tag_name`).

## CrossPoint Pal 0.1.0 — Image Viewer & Low-Power Slideshow

First public release. Based on upstream `crosspoint-reader/crosspoint-reader`
`develop` @ `5ab0f290a5d88cb54fb7e2906c6bc0ab829770f0`.

Hardware-verified binary: **XTEINK X4 Classic** only.

### Image viewer

- Improved BMP/PNG viewer with an options modal: render controls, image info,
  delete (fail-closed contract), and sleep-cover assignment.
- BMP tone render controls: brightness (70–110%), gamma (0.70–1.30), contrast
  (80–130%), and 4-level quantizer selection (Default / Legacy / Canonical).
  Identity at defaults is an exact pass-through.
- Persisted viewer render profile (survives restarts) plus a separate
  persisted sleep render profile ("Use for sleep rendering").
- Grayscale-aware BMP presentation driven by explicit grayscale-capability
  policy (panel- and controller-aware) instead of uniform transfers.

### Low-power slideshow

- Image Viewer slideshow: start from the open image; continuation frames are
  shown from deep-sleep timer wakes.
- Sleep Screen = Slideshow: the sleep screen becomes a slideshow sourced from
  `/.sleep` (legacy `/sleep` fallback).
- Shared slideshow interval: 1 / 5 / 10 / 30 minutes.
- Frame order: Forward, Reverse, Random (every image is shown once in
  randomized order before a new cycle begins; no immediate repeat across
  cycles). Adapted from CrossPoint upstream PR #3841 by @gkaindl.
- Low-battery cutoff: at ≤10% charge without external power the sleep
  slideshow stops waking the device and falls back to an ordinary static
  sleep screen.
- Auto-sleep timeout starts the slideshow as well (Sleep Screen = Slideshow
  takes priority over the after-timeout Quick Resume option).
- Grayscale-safe X4 Classic presentation (UC8279): every timer-wake frame
  scrubs the panel with an absolute refresh base before loading gray planes.
- Reworked slideshow page controls (Start / Interval / Order) with
  device-capability-driven layout and non-stale button hints.
