from __future__ import annotations

import hashlib
import shutil
import sys
import zipfile
from pathlib import Path, PurePosixPath


ROOT = Path(__file__).resolve().parent
PACKAGES = ROOT / "packages"
VERSION = (ROOT / "VERSION").read_text(encoding="utf-8").strip()


def copy_file(source: Path, target: Path) -> None:
  target.parent.mkdir(parents=True, exist_ok=True)
  shutil.copy2(source, target)


def copy_tree(source: Path, target: Path) -> None:
  if target.exists():
    shutil.rmtree(target)
  shutil.copytree(source, target)


def write_text(path: Path, text: str) -> None:
  path.parent.mkdir(parents=True, exist_ok=True)
  with path.open("w", encoding="utf-8", newline="\n") as stream:
    stream.write(text)


def set_zip_modes(zip_path: Path, modes: dict[str, int]) -> None:
  tmp = zip_path.with_name(zip_path.name + ".tmp")
  with zipfile.ZipFile(zip_path, "r") as zin, zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as zout:
    for info in zin.infolist():
      data = zin.read(info.filename)
      if info.filename in modes:
        info.create_system = 3
        info.external_attr = modes[info.filename] << 16
      zout.writestr(info, data)
  tmp.replace(zip_path)


def make_zip(directory: Path, modes: dict[str, int] | None = None) -> Path:
  archive = Path(str(directory) + ".zip")
  if archive.exists():
    archive.unlink()
  shutil.make_archive(str(directory), "zip", directory)
  if modes:
    set_zip_modes(archive, modes)
  return archive


def verify_zip(archive: Path, required: set[str], executables: set[str] | None = None) -> None:
  with zipfile.ZipFile(archive, "r") as package:
    names = [info.filename for info in package.infolist()]
    if len(names) != len(set(names)) or package.testzip() is not None:
      raise RuntimeError(f"invalid or duplicate zip entries in {archive.name}")
    for name in names:
      path = PurePosixPath(name)
      if path.is_absolute() or ".." in path.parts or "\\" in name:
        raise RuntimeError(f"unsafe zip entry in {archive.name}: {name}")
    missing = required - set(names)
    if missing:
      raise RuntimeError(f"missing entries in {archive.name}: {', '.join(sorted(missing))}")
    for name in executables or set():
      mode = package.getinfo(name).external_attr >> 16
      if mode & 0o111 == 0:
        raise RuntimeError(f"Linux executable bit missing in {archive.name}: {name}")


def package_root(name: str) -> Path:
  path = PACKAGES / name
  if path.exists():
    shutil.rmtree(path)
  zip_path = Path(str(path) + ".zip")
  if zip_path.exists():
    zip_path.unlink()
  return path


def copy_common_files(out: Path) -> None:
  copy_file(ROOT / "LICENSE", out / "LICENSE")
  copy_file(ROOT / "THIRD_PARTY_NOTICES", out / "THIRD_PARTY_NOTICES")
  copy_file(ROOT / "CHANGELOG.md", out / "CHANGELOG.md")
  copy_file(ROOT / "README.md", out / "README.md")
  copy_file(ROOT / "cfg" / "cs2fow.cfg", out / "cfg" / "cs2fow.cfg")
  copy_file(ROOT / "gamedata" / "cs2fow.games.txt", out / "addons" / "cs2fow" / "gamedata" / "cs2fow.games.txt")
  write_text(out / "addons" / "cs2fow" / "data" / "maps" / ".gitkeep", "")


def build_core_package(platform: str, plugin_name: str, baker_name: str, vrf_dir: str) -> Path:
  out = package_root(f"cs2fow-{VERSION}-{platform}")
  copy_common_files(out)
  copy_tree(ROOT / "tools" / "vrf" / vrf_dir, out / "tools" / "vrf" / vrf_dir)
  if vrf_dir == "linux64":
    (out / "tools" / "vrf" / "linux64" / "libTinyEXRNative.so").unlink(missing_ok=True)
  copy_file(ROOT / plugin_name, out / "addons" / "cs2fow" / "bin" / Path(plugin_name).name)
  copy_file(ROOT / baker_name, out / "tools" / Path(baker_name).name)
  write_text(
    out / "addons" / "metamod" / "cs2fow.vdf",
    '"Metamod Plugin"\n{\n  "alias"  "cs2fow"\n  "file"   "addons/cs2fow/bin/cs2fow"\n}\n',
  )

  modes: dict[str, int] = {}
  if platform.startswith("linux"):
    (out / "tools" / "cs2fow_baker").chmod(0o755)
    (out / "tools" / "vrf" / "linux64" / "Source2Viewer-CLI").chmod(0o755)
    modes = {
      "tools/cs2fow_baker": 0o755,
      "tools/vrf/linux64/Source2Viewer-CLI": 0o755,
    }
  archive = make_zip(out, modes)
  required = {
    "addons/cs2fow/bin/" + Path(plugin_name).name,
    "addons/cs2fow/gamedata/cs2fow.games.txt",
    "addons/metamod/cs2fow.vdf",
    "cfg/cs2fow.cfg",
    "tools/" + Path(baker_name).name,
  }
  verify_zip(archive, required, set(modes))
  return archive


def build_official_maps_package() -> Path | None:
  maps = sorted((ROOT / "data" / "maps").glob("*"))
  maps = [path for path in maps if path.suffix in {".bvh8", ".json"}]
  if not maps:
    return None
  if {path.stem for path in maps if path.suffix == ".bvh8"} != {path.stem for path in maps if path.suffix == ".json"}:
    raise RuntimeError("official map bakes and reports do not match")

  out = package_root(f"cs2fow-{VERSION}-official-maps")
  copy_file(ROOT / "DATA_NOTICE", out / "DATA_NOTICE")
  for path in maps:
    copy_file(path, out / "addons" / "cs2fow" / "data" / "maps" / path.name)
  archive = make_zip(out)
  verify_zip(archive, {"DATA_NOTICE"} | {f"addons/cs2fow/data/maps/{path.name}" for path in maps})
  return archive


def sha256(path: Path) -> str:
  digest = hashlib.sha256()
  with path.open("rb") as stream:
    for chunk in iter(lambda: stream.read(1024 * 1024), b""):
      digest.update(chunk)
  return digest.hexdigest()


def write_checksums(archives: list[Path]) -> None:
  lines = [f"{sha256(path)}  {path.name}" for path in sorted(archives)]
  write_text(PACKAGES / "SHA256SUMS.txt", "\n".join(lines) + "\n")
  if (PACKAGES / "SHA256SUMS.txt").read_text(encoding="utf-8").splitlines() != lines:
    raise RuntimeError("checksum file verification failed")


def main() -> None:
  PACKAGES.mkdir(exist_ok=True)
  targets = sys.argv[1:] or ["windows-x86_64", "linux-x86_64", "official-maps"]
  archives: list[Path] = []
  if "windows-x86_64" in targets:
    archives.append(build_core_package(
      "windows-x86_64",
      "build/cs2fow/windows-x86_64/cs2fow.dll",
      "build/cs2fow_baker/windows-x86_64/cs2fow_baker.exe",
      "win64",
    ))
  if "linux-x86_64" in targets:
    archives.append(build_core_package(
      "linux-x86_64",
      "build-linux/cs2fow/linux-x86_64/cs2fow.so",
      "build-linux/cs2fow_baker/linux-x86_64/cs2fow_baker",
      "linux64",
    ))
  if "official-maps" in targets:
    maps_archive = build_official_maps_package()
    if maps_archive is not None:
      archives.append(maps_archive)
  if not archives:
    raise SystemExit(f"no packages built for targets: {' '.join(targets)}")
  write_checksums(archives)


if __name__ == "__main__":
  main()
