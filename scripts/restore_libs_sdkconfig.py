"""Restore the shared Arduino-libs sdkconfig that hybrid builds overwrite.

The pioarduino hybrid rebuild resolves its sdkconfig and copies the result
back over framework-arduinoespressif32-libs/<chip>/sdkconfig and the
package-level sdkconfig (espidf.py copies $PROJECT_DIR/sdkconfig.<env> there
during the libs export). The pristine original is preserved as
<chip>/sdkconfig.orig on first use.

This script runs when PlatformIO loads the extra_scripts of a pass (it is
wired to the x4c rebuilt-core diagnostic envs via their extra_scripts; the script
itself only acts on envs declaring custom_sdkconfig). The nested child pass that performs
the Arduino compile starts after the parent's copy-back, so its load-time restore
returns the shared files to the pristine base before that pass builds.
Between passes the shared files hold the last pass's resolved config; a
production env build reinstalls the package wholesale on the next
custom_sdkconfig hash mismatch. The guarantee this provides: every hybrid
build starts from the pristine base plus only its own declared overrides,
not from another env's leftovers.
"""

import hashlib
import os
import shutil
from pathlib import Path

Import("env")  # noqa: F821


def _sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def post(source, target, env):
    try:
        env.GetProjectOption("custom_sdkconfig")
    except Exception:
        return

    platform = env.PioPlatform()
    libs_dir = platform.get_package_dir("framework-arduinoespressif32-libs")
    if not libs_dir:
        return

    chip = env.BoardConfig().get("build.chip_variant", "") or env.BoardConfig().get(
        "build.mcu", "esp32"
    ).lower()
    chip_dir = Path(libs_dir) / chip

    # A missing sdkconfig.orig is normally the very first hybrid build for this
    # chip (the pristine file is moved aside by the first copy-back). But when
    # a previous hybrid build already recorded its request in
    # sdkconfig.crosspoint and .orig then went missing (deleted by hand or by a
    # partial package reinstall), the chip-level sdkconfig may still hold that
    # other env's resolved config — restoring would silently skip and this
    # build would start from another env's leftovers. The request file starts
    # with the env's own custom_sdkconfig text (see patch_pioarduino_cache.py),
    # so a matching prefix proves the recorded state belongs to this env and a
    # skip is safe; anything else fails the build loudly instead.
    request_file = chip_dir / "sdkconfig.crosspoint"
    if not (chip_dir / "sdkconfig.orig").is_file() and request_file.is_file():
        requested = env.GetProjectOption("custom_sdkconfig", "").strip()
        recorded = request_file.read_text(encoding="utf-8").strip()
        if not recorded.startswith(requested):
            raise EnvironmentError(
                f"restore_libs_sdkconfig[{env['PIOENV']}]: {chip}/sdkconfig.orig is missing while "
                f"{request_file} records a different env's build; delete {request_file} (or reinstall "
                "the framework-arduinoespressif32-libs package) and rebuild"
            )

    pairs = [
        (chip_dir / "sdkconfig", chip_dir / "sdkconfig.orig"),
        (Path(libs_dir) / "sdkconfig", chip_dir / "sdkconfig.orig"),
    ]
    for target_path, orig_path in pairs:
        if not orig_path.is_file():
            print(f"restore_libs_sdkconfig[{env['PIOENV']}]: no {orig_path.name} for {chip}, skipping")
            continue
        if not target_path.is_file():
            continue
        if _sha256(target_path) == _sha256(orig_path):
            continue
        shutil.copyfile(orig_path, target_path)
        print(
            f"restore_libs_sdkconfig[{env['PIOENV']}]: restored {target_path} "
            f"from {orig_path.name} (sha256 {_sha256(target_path)})"
        )


post(None, None, env)
