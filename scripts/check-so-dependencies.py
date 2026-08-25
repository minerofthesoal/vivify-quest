#!/usr/bin/env python3
"""Check that every library libVivify.so needs will exist on a Quest.

The mod is loaded with dlopen. If the built .so carries a DT_NEEDED entry for a
library the device does not have, the load fails outright and nothing in the mod
runs -- the whole thing reports as "libVivify.so failed", with no clue as to
which library was missing.

That is not hypothetical: extern.cmake links every .so in extern/libs, and
restore-deps.py was downloading libconfig-utils_test.so for config-utils, a
headers-only dependency whose release happens to publish the repo's test binary.
It linked cleanly and shipped a mod no device could load.

Every DT_NEEDED entry must be one of:
  - a library declared in mod.json's dependencies (installed alongside the mod),
  - a library the modloader itself provides,
  - an Android system library.

Usage: scripts/check-so-dependencies.py <path-to-libVivify.so>
"""
import json
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Provided by the Scotland2 modloader / the game process itself.
MODLOADER_PROVIDED = {
    "libsl2.so",           # Scotland2 itself
    "libpaper2_scotland2.so",
    "libmain.so",
    "libil2cpp.so",
    "libunity.so",
}

# Bionic and the Android platform.
SYSTEM_PROVIDED = {
    "libc.so", "libm.so", "libdl.so", "liblog.so", "libstdc++.so",
    "libc++_shared.so", "libandroid.so", "libz.so", "libGLESv3.so",
    "libGLESv2.so", "libEGL.so", "libvulkan.so", "libOpenSLES.so",
}


def needed_libraries(path: pathlib.Path) -> list:
    for tool in ("readelf", "llvm-readelf", "aarch64-linux-android-readelf"):
        try:
            out = subprocess.run([tool, "-d", str(path)], capture_output=True,
                                 text=True, check=True).stdout
        except (FileNotFoundError, subprocess.CalledProcessError):
            continue
        return re.findall(r"\(NEEDED\).*?\[(.+?)\]", out)
    raise RuntimeError("no readelf available")


def declared_dependency_libraries() -> dict:
    """Map library file name -> mod.json dependency id.

    qpm.shared.json records the library each dependency actually installs; a
    dependency resolved headersOnly installs none.
    """
    shared = json.loads((ROOT / "qpm.shared.json").read_text())
    by_id = {}
    for entry in shared.get("restoredDependencies", []):
        dep = entry.get("dependency", {})
        extra = dep.get("additionalData") or {}
        if extra.get("headersOnly"):
            continue
        name = extra.get("overrideSoName")
        if name:
            by_id[name] = dep.get("id", "?")
    return by_id


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    target = pathlib.Path(sys.argv[1])
    if not target.is_file():
        print(f"error: {target} does not exist", file=sys.stderr)
        return 2

    needed = needed_libraries(target)
    installed = declared_dependency_libraries()
    mod = json.loads((ROOT / "mod.json").read_text())
    declared_ids = {d["id"] for d in mod.get("dependencies", [])}

    print(f"{target.name} needs {len(needed)} librar{'y' if len(needed) == 1 else 'ies'}:")
    problems = []
    warnings = []
    for name in needed:
        if name in SYSTEM_PROVIDED:
            print(f"  ok       {name:<32} (Android system)")
        elif name in MODLOADER_PROVIDED:
            print(f"  ok       {name:<32} (modloader)")
        elif name in installed:
            dep_id = installed[name]
            if dep_id in declared_ids:
                print(f"  ok       {name:<32} (mod.json dependency '{dep_id}')")
            else:
                # A real mod that some dependency of ours pulls in transitively
                # (libtinyxml2 arrives with BSML, for instance). Worth surfacing,
                # but it is normally installed, so it is not a build failure.
                print(f"  note     {name:<32} from '{dep_id}', installed transitively")
                warnings.append(
                    f"{name} comes from '{dep_id}', which mod.json does not list directly; "
                    f"it is expected to arrive as a dependency of another mod")
        else:
            print(f"  PROBLEM  {name:<32} unaccounted for")
            problems.append(
                f"{name} is not an Android or modloader library and no qpm dependency "
                f"installs it; dlopen will fail on device and the mod will not load")

    for warning in warnings:
        print(f"warning: {warning}", file=sys.stderr)

    if problems:
        print()
        print("error: libVivify.so would fail to load on a Quest:", file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        return 1

    print("\nEvery needed library is accounted for.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
