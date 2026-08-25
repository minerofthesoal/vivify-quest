#!/usr/bin/env python3
"""Restore this project's dependencies from GitHub, falling back to qpackages.com.

Replaces `qpm restore`, which resolves everything through qpackages.com. This
reads scripts/dependencies.json -- a self-contained manifest of repositories,
refs and release asset URLs -- and populates extern/ with the same layout qpm
produces, then regenerates extern.cmake.

    python3 scripts/restore-deps.py             # restore into ./extern
    python3 scripts/restore-deps.py --clean     # wipe extern/ first
    python3 scripts/restore-deps.py --check     # verify manifest, download nothing

Headers come from https://github.com/<repo>/archive/<ref>.tar.gz (or qpackages.com
if no repo/ref is specified) and native libraries from their GitHub release assets.
"""

import argparse
import io
import json
import pathlib
import re
import shutil
import sys
import tarfile
import urllib.error
import urllib.request

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
MANIFEST = HERE / "dependencies.json"
USER_AGENT = "vivify-quest-restore"


def fetch(url: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request) as response:
        return response.read()


def extract_headers_from_local(dep: dict, includes: pathlib.Path) -> bool:
    """Check if headers are already bundled locally and copy them."""
    dep_id = dep["id"]
    local_include_dir = ROOT / "extern" / "includes" / dep_id

    if not local_include_dir.exists():
        return False

    destination = includes / dep_id

    # The vendored headers usually already live exactly where they are wanted
    # (extern/includes/<id>), in which case source and destination are the same
    # directory. Copying would mean rmtree'ing the destination first -- which
    # is the source -- deleting the vendored tree and then failing on a copy
    # from a path that no longer exists.
    if local_include_dir.resolve() == destination.resolve():
        print(f"  headers  {dep_id:<26} (bundled locally, already in place)")
        return True

    if destination.exists():
        shutil.rmtree(destination)
    shutil.copytree(local_include_dir, destination)
    print(f"  headers  {dep_id:<26} (bundled locally)")
    return True

def library_file_name(dep: dict) -> str:
    """The file name a dependency's native library is stored under.

    Must agree between the local-bundle check and the download, or the two
    disagree about whether a library is already present. overrideSoName lives at
    the top level of a manifest entry; it was previously read from a nested
    "additionalData" key that the manifest never had, so this always fell
    through to the guess below. That guess turns hyphens into underscores, so
    for every hyphenated id (beatsaber-hook, custom-types, web-utils, ...) it
    looked for a file that does not exist and re-downloaded a library already
    sitting in extern/libs.
    """
    override = dep.get("overrideSoName") or (dep.get("additionalData") or {}).get("overrideSoName")
    if override:
        return override
    url = dep.get("soLink")
    if url:
        return url.rsplit("/", 1)[-1]
    return f"lib{dep['id'].replace('-', '_')}.so"


def extract_library_from_local(dep: dict, libs: pathlib.Path) -> bool:
    """Use a library already bundled in extern/libs, if there is one."""
    dep_id = dep["id"]
    lib_name = library_file_name(dep)
    local_lib_path = ROOT / "extern" / "libs" / lib_name

    if not local_lib_path.exists():
        return False

    destination = libs / lib_name

    # In the vendored layout the bundled library already sits exactly where it
    # is wanted, so source and destination are the same file and copy2 raises
    # SameFileError. Nothing needs doing in that case.
    if local_lib_path.resolve() == destination.resolve():
        print(f"  library  {dep_id:<26} {lib_name} (bundled locally, already in place)")
        return True

    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(local_lib_path, destination)
    print(f"  library  {dep_id:<26} {lib_name} (bundled locally)")
    return True

def extract_headers_from_github(dep: dict, includes: pathlib.Path) -> None:
    """Download <repo> at <ref> from GitHub and unpack it to includes/<id>/."""
    repo, ref, dep_id = dep["repo"], dep["ref"], dep["id"]
    url = f"https://github.com/{repo}/archive/{ref}.tar.gz"
    print(f"  headers  {dep_id:<26} {repo}@{ref} (GitHub)")
    blob = fetch(url)

    destination = includes / dep_id
    if destination.exists():
        shutil.rmtree(destination)
    destination.mkdir(parents=True)

    with tarfile.open(fileobj=io.BytesIO(blob), mode="r:gz") as archive:
        members = archive.getmembers()
        # GitHub wraps the tree in a single <repo>-<ref> directory; strip it so
        # includes/<id>/shared/... matches what #include paths expect.
        root = members[0].name.split("/", 1)[0] + "/" if members else ""
        for member in members:
            if not member.name.startswith(root):
                continue
            relative = member.name[len(root):]
            if not relative:
                continue
            # Refuse anything that would escape the destination directory.
            target = (destination / relative).resolve()
            if not str(target).startswith(str(destination.resolve())):
                raise RuntimeError(f"{dep_id}: archive entry escapes destination: {member.name}")
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            elif member.isreg():
                target.parent.mkdir(parents=True, exist_ok=True)
                source = archive.extractfile(member)
                if source is not None:
                    target.write_bytes(source.read())


def extract_headers_from_qpackages(dep: dict, includes: pathlib.Path) -> None:
    """Download headers from qpackages.com and unpack to includes/<id>/."""
    dep_id, version = dep["id"], dep["version"]
    
    # First try to get metadata from qpackages.com to find the actual download URL
    metadata_url = f"https://qpackages.com/{dep_id}/{version}"
    print(f"  headers  {dep_id:<26} {version} (qpackages.com)")
    
    try:
        metadata_blob = fetch(metadata_url)
        metadata = json.loads(metadata_blob.decode('utf-8'))
        
        # Extract the actual download URL from metadata
        download_url = metadata.get('config', {}).get('info', {}).get('url')
        if not download_url:
            raise RuntimeError(f"No download URL found in qpackages metadata for {dep_id}@{version}")
        
        print(f"           Downloading from: {download_url}")
        blob = fetch(download_url)
    except Exception as e:
        print(f"  FAILED   {dep_id:<26} qpackages.com metadata error: {e}")
        raise

    destination = includes / dep_id
    if destination.exists():
        shutil.rmtree(destination)
    destination.mkdir(parents=True)

    # qpackages may return either .tar.gz or .zip files
    # Try tar.gz first, then zip if that fails
    import zipfile
    
    extracted = False
    
    # Try tar.gz format first
    try:
        with tarfile.open(fileobj=io.BytesIO(blob), mode="r:gz") as archive:
            members = archive.getmembers()
            # qpackages may wrap the tree; strip the root directory if present
            root = members[0].name.split("/", 1)[0] + "/" if members else ""
            for member in members:
                if not member.name.startswith(root):
                    continue
                relative = member.name[len(root):]
                if not relative:
                    continue
                # Refuse anything that would escape the destination directory.
                target = (destination / relative).resolve()
                if not str(target).startswith(str(destination.resolve())):
                    raise RuntimeError(f"{dep_id}: archive entry escapes destination: {member.name}")
                if member.isdir():
                    target.mkdir(parents=True, exist_ok=True)
                elif member.isreg():
                    target.parent.mkdir(parents=True, exist_ok=True)
                    source = archive.extractfile(member)
                    if source is not None:
                        target.write_bytes(source.read())
        extracted = True
    except tarfile.ReadError:
        # Not a tar.gz, try zip format
        pass
    
    if not extracted:
        # Try zip format
        try:
            with zipfile.ZipFile(io.BytesIO(blob)) as zip_ref:
                # Find root directory
                names = zip_ref.namelist()
                root = names[0].split("/", 1)[0] + "/" if names else ""
                
                for name in names:
                    if not name.startswith(root):
                        continue
                    relative = name[len(root):]
                    if not relative:
                        continue
                    
                    target = destination / relative
                    if name.endswith('/'):
                        target.mkdir(parents=True, exist_ok=True)
                    else:
                        target.parent.mkdir(parents=True, exist_ok=True)
                        target.write_bytes(zip_ref.read(name))
            extracted = True
        except zipfile.BadZipFile:
            raise RuntimeError(f"Downloaded file for {dep_id}@{version} is neither valid tar.gz nor zip")
    
    if not extracted:
        raise RuntimeError(f"Failed to extract archive for {dep_id}@{version}")


def extract_headers(dep: dict, includes: pathlib.Path, allow_qpackages: bool = True) -> None:
    """Download headers from local bundle first, then GitHub, then qpackages.com.

    With allow_qpackages=False the qpackages.com fallback is refused outright,
    so a dependency the manifest cannot source from GitHub fails the restore
    instead of quietly reaching for the registry.
    """
    # First try local bundled packages
    if extract_headers_from_local(dep, includes):
        return

    # Then try GitHub if repo/ref specified
    if dep.get("repo") and dep.get("ref"):
        try:
            extract_headers_from_github(dep, includes)
            return
        except (urllib.error.URLError, urllib.error.HTTPError) as e:
            if not allow_qpackages:
                raise RuntimeError(f"GitHub source failed and --no-qpackages is set: {e}") from e
            print(f"  WARNING  {dep['id']:<26} GitHub failed: {e}, trying qpackages.com")

    if not allow_qpackages:
        raise RuntimeError("no GitHub repo/ref in the manifest and --no-qpackages is set")

    # Fall back to qpackages.com
    extract_headers_from_qpackages(dep, includes)


def download_library(dep: dict, libs: pathlib.Path) -> None:
    # First try local bundled libraries
    if extract_library_from_local(dep, libs):
        return
    
    url = dep.get("soLink")
    if not url:
        return
    name = library_file_name(dep)
    print(f"  library  {dep['id']:<26} {name} (GitHub)")
    (libs / name).write_bytes(fetch(url))


def project_version() -> str:
    """The mod version qpm.json declares, which qpm_defines.cmake must mirror."""
    return str(json.loads((ROOT / "qpm.json").read_text())["info"]["version"])


def declared_include_paths(dep: dict):
    """Every include directory the manifest promises this dependency provides."""
    options = dep.get("compileOptions") or {}
    for path in options.get("includePaths", []):
        yield "includePaths", path
    for path in options.get("systemIncludes", []):
        yield "systemIncludes", path


def verify_include_paths(manifest: dict, includes: pathlib.Path) -> list:
    """Check that every declared include directory actually landed on disk.

    A path that does not exist is silently accepted by CMake and only surfaces
    much later as a pile of "file not found" compiler errors, which is how a
    stale "fmt/include/" (the qpm wrapper layout, one "fmt/" too many) survived
    in this manifest. Fail at restore time instead, naming the directory.
    """
    missing = []
    for dep in manifest["dependencies"]:
        for key, path in declared_include_paths(dep):
            candidate = includes / dep["id"] / path
            if not candidate.is_dir():
                missing.append(f"{dep['id']} {key} '{path}' -> {candidate} does not exist")
    return missing


def sync_mod_version(manifest_version: str) -> None:
    """Keep MOD_VERSION in qpm_defines.cmake in step with qpm.json.

    qpm normally rewrites that file on restore. This script replaces qpm, so
    without this the compiled binary keeps reporting whatever version was
    current the last time qpm ran (0.4.2, long after qpm.json reached 0.8.2).
    """
    defines = ROOT / "qpm_defines.cmake"
    if not defines.exists():
        return
    text = defines.read_text()
    updated, count = re.subn(r'set\(MOD_VERSION "[^"]*"\)',
                             f'set(MOD_VERSION "{manifest_version}")', text, count=1)
    if count and updated != text:
        defines.write_text(updated)
        print(f"  version  qpm_defines.cmake MOD_VERSION -> {manifest_version}")


def generate_extern_cmake(manifest: dict) -> str:
    lines = [
        "# Generated by scripts/restore-deps.py from scripts/dependencies.json.",
        "# Do not edit by hand; re-run the script instead.",
        "target_include_directories(${COMPILE_ID} PRIVATE ${EXTERN_DIR}/includes)",
        "",
    ]
    for dep in manifest["dependencies"]:
        options = dep.get("compileOptions") or {}
        dep_id = dep["id"]
        for path in options.get("includePaths", []):
            lines.append(f"target_include_directories(${{COMPILE_ID}} PRIVATE ${{EXTERN_DIR}}/includes/{dep_id}/{path})")
        for path in options.get("systemIncludes", []):
            lines.append(
                f"target_include_directories(${{COMPILE_ID}} SYSTEM PRIVATE ${{EXTERN_DIR}}/includes/{dep_id}/{path})")
        for flag in options.get("cppFlags", []):
            lines.append(f"target_compile_options(${{COMPILE_ID}} PRIVATE {flag})")
    lines += [
        "",
        "target_link_directories(${COMPILE_ID} PRIVATE ${EXTERN_DIR}/libs)",
        "RECURSE_FILES(so_list ${EXTERN_DIR}/libs/*.so)",
        "RECURSE_FILES(a_list ${EXTERN_DIR}/libs/*.a)",
        "",
        "target_link_libraries(${COMPILE_ID} PRIVATE",
        "\t${so_list}",
        "\t${a_list}",
        ")",
        "",
    ]
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--clean", action="store_true", help="remove extern/ before restoring")
    parser.add_argument("--check", action="store_true", help="validate the manifest without downloading")
    parser.add_argument("--no-qpackages", action="store_true",
                        help="refuse the qpackages.com fallback; fail if a dependency is not on GitHub")
    args = parser.parse_args()

    manifest = json.loads(MANIFEST.read_text())
    dependencies = manifest["dependencies"]

    # Now we allow dependencies without repo/ref - they will be fetched from qpackages.com
    # No need to error out anymore for missing repo/ref

    needs_registry = [d["id"] for d in dependencies if not (d.get("repo") and d.get("ref"))]

    # A ref carrying none of the pinned version's digits usually means the entry
    # points at an upstream project rather than the qpm-packaged fork of it --
    # the mistake that put fmtlib/fmt, sc2ad/ConditionalDependencies and
    # z4kn4fein/cpp-semver in this manifest, none of which has the ref
    # qpm.shared.json pins. Restoring the wrong package fails far more
    # confusingly than saying so up front.
    suspicious = []
    for dep in dependencies:
        if not dep.get("repo"):
            continue
        digits = str(dep.get("version", "")).replace(".", "")
        ref_digits = "".join(c for c in str(dep.get("ref", "")) if c.isdigit())
        if digits and ref_digits and digits not in ref_digits:
            suspicious.append(f"{dep['id']}: version {dep['version']} but ref {dep['ref']}")

    for warning in suspicious:
        print(f"warning: {warning}", file=sys.stderr)

    if args.check:
        github_deps = len(dependencies) - len(needs_registry)
        print(f"Manifest: {len(dependencies)} dependencies.")
        print(f"  GitHub sources: {github_deps}")
        print(f"  qpackages.com fallback: {len(needs_registry)}")
        print(f"Native libraries: {sum(1 for d in dependencies if d.get('soLink'))}")
        includes = ROOT / manifest.get("externDir", "extern") / "includes"
        if includes.is_dir():
            missing = verify_include_paths(manifest, includes)
            for entry in missing:
                print(f"error: {entry}", file=sys.stderr)
            if missing:
                return 1
        if args.no_qpackages and needs_registry:
            print("\nerror: --no-qpackages, but these have no GitHub repo/ref:", file=sys.stderr)
            for dep_id in needs_registry:
                print(f"  - {dep_id}", file=sys.stderr)
            print("\nRun scripts/discover-deps.py to complete the manifest.", file=sys.stderr)
            return 1
        return 0

    if args.no_qpackages and needs_registry:
        print("error: --no-qpackages, but these have no GitHub repo/ref: "
              + ", ".join(needs_registry), file=sys.stderr)
        return 1

    extern = ROOT / manifest.get("externDir", "extern")
    if args.clean and extern.exists():
        shutil.rmtree(extern)
    includes = extern / "includes"
    libs = extern / "libs"
    includes.mkdir(parents=True, exist_ok=True)
    libs.mkdir(parents=True, exist_ok=True)

    failures = []
    for dep in dependencies:
        try:
            extract_headers(dep, includes, allow_qpackages=not args.no_qpackages)
            download_library(dep, libs)
        except (urllib.error.URLError, urllib.error.HTTPError, RuntimeError, tarfile.TarError, OSError) as error:
            failures.append(f"{dep['id']}: {error}")
            print(f"  FAILED   {dep['id']:<26} {error}", flush=True)

    if failures:
        print(f"\n{len(failures)} dependency/dependencies failed to restore.", file=sys.stderr)
        return 1

    missing = verify_include_paths(manifest, includes)
    if missing:
        print("\nerror: declared include directories are missing after restore:", file=sys.stderr)
        for entry in missing:
            print(f"  - {entry}", file=sys.stderr)
        print("Fix the paths in scripts/dependencies.json to match the restored layout.",
              file=sys.stderr)
        return 1

    (ROOT / "extern.cmake").write_text(generate_extern_cmake(manifest))
    sync_mod_version(project_version())
    print(f"\nRestored {len(dependencies)} dependencies into {extern} and regenerated extern.cmake.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
