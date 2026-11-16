#!/usr/bin/env python3
"""
Extract game content from a retail Half-Life 2 install (Steam, steam_legacy
branch) into the local, git-ignored asset folder used for asset compilation.

Every *_dir.vpk in each game folder is unpacked to loose files:
  hl2/      hl2_misc, hl2_pak, hl2_textures, hl2_sound_misc, hl2_sound_vo_english
  episodic/ ep1_pak   (Episode One)
  ep2/      ep2_pak   (Episode Two)
  platform/ platform_misc

Loose content shipped outside the VPKs (maps/*.bsp, cfg, resource, ...) is
copied too, so each folder is the game's complete content as plain files.
Where a file is both loose and in a VPK, the VPK copy is kept, matching the
engine's search order. bin/, save/, custom/ and the .vpk files are skipped.

Files keep their VPK-internal paths, under a folder per game:
  assets/hl2/sound/ambient/...
  assets/episodic/maps/ep1_c17_00.bsp
  assets/ep2/models/...

Runs on Linux and Windows with only the Python 3 standard library.

Examples:
  python3 scripts/extract_vpk_assets.py
  python3 scripts/extract_vpk_assets.py --only ep2 --filter sound/
  python3 scripts/extract_vpk_assets.py --hl2-dir "D:/SteamLibrary/steamapps/common/Half-Life 2" --list
"""

import argparse
import os
import re
import shutil
import struct
import sys
import zlib
from pathlib import Path

VPK_SIGNATURE = 0x55AA1234
DIR_ARCHIVE_INDEX = 0x7FFF
CHUNK = 1 << 20

# Game folders whose VPKs are extracted. Every *_dir.vpk in each is unpacked.
GAMES = ["hl2", "episodic", "ep2", "platform"]

# Files whose CRC stored in the retail VPK is wrong, although the data is what
# Steam ships (confirmed with "Verify integrity of game files").
# Keyed by (path, CRC in the VPK) -> CRC of the real data. Only an exact match
# is accepted, so genuine corruption of these files is still reported.
KNOWN_BAD_VPK_CRCS = {
	("sound/vo/novaprospekt/al_pickherup.wav", 0x38D8E8B1): 0x19A18A99,
}

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_OUT = REPO_ROOT / "assets"


# --------------------------------------------------------------------------
# Locating the game
# --------------------------------------------------------------------------

def steam_roots():
	"""Candidate Steam install directories for this platform."""
	roots = []
	if sys.platform == "win32":
		try:
			import winreg
			for hive, key in ((winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam"),
							  (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Valve\Steam")):
				try:
					with winreg.OpenKey(hive, key) as k:
						for name in ("SteamPath", "InstallPath"):
							try:
								roots.append(Path(winreg.QueryValueEx(k, name)[0]))
							except OSError:
								pass
				except OSError:
					pass
		except ImportError:
			pass
		roots.append(Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Steam")
	else:
		home = Path.home()
		roots += [
			home / ".local/share/Steam",
			home / ".steam/steam",
			home / ".var/app/com.valvesoftware.Steam/.local/share/Steam",  # Flatpak
			home / "Library/Application Support/Steam",                    # macOS
		]
	return roots


def steam_libraries(root):
	"""The Steam root plus any extra library folders listed in libraryfolders.vdf."""
	libs = [root]
	vdf = root / "steamapps" / "libraryfolders.vdf"
	try:
		text = vdf.read_text(encoding="utf-8", errors="replace")
	except OSError:
		return libs
	for m in re.finditer(r'"path"\s+"([^"]+)"', text):
		libs.append(Path(m.group(1).replace("\\\\", "\\")))
	return libs


def find_hl2_dir():
	seen = set()
	for root in steam_roots():
		for lib in steam_libraries(root):
			cand = lib / "steamapps" / "common" / "Half-Life 2"
			key = str(cand).lower()
			if key in seen:
				continue
			seen.add(key)
			if (cand / "hl2").is_dir():
				return cand
	return None


# --------------------------------------------------------------------------
# VPK reading (versions 1 and 2)
# --------------------------------------------------------------------------

class VpkEntry:
	__slots__ = ("path", "crc", "preload", "archive", "offset", "length")

	def __init__(self, path, crc, preload, archive, offset, length):
		self.path = path
		self.crc = crc
		self.preload = preload
		self.archive = archive
		self.offset = offset
		self.length = length

	@property
	def size(self):
		return len(self.preload) + self.length


def _read_cstr(buf, pos):
	end = buf.index(b"\0", pos)
	return buf[pos:end].decode("utf-8", errors="replace"), end + 1


def read_vpk_dir(dir_path):
	"""Parse a *_dir.vpk. Returns (entries, offset of embedded data in the dir file)."""
	with open(dir_path, "rb") as f:
		sig, version, tree_size = struct.unpack("<III", f.read(12))
		if sig != VPK_SIGNATURE:
			raise ValueError(f"{dir_path}: not a VPK file")
		if version == 1:
			header_size = 12
		elif version == 2:
			f.read(16)  # file data, archive md5, other md5, signature section sizes
			header_size = 28
		else:
			raise ValueError(f"{dir_path}: unsupported VPK version {version}")
		tree = f.read(tree_size)

	entries = []
	pos = 0
	while True:
		ext, pos = _read_cstr(tree, pos)
		if not ext:
			break
		while True:
			folder, pos = _read_cstr(tree, pos)
			if not folder:
				break
			while True:
				name, pos = _read_cstr(tree, pos)
				if not name:
					break
				crc, preload_len, archive, offset, length, term = struct.unpack_from("<IHHIIH", tree, pos)
				pos += 18
				if term != 0xFFFF:
					raise ValueError(f"{dir_path}: corrupt directory tree near byte {pos}")
				preload = tree[pos:pos + preload_len]
				pos += preload_len

				parts = []
				if folder != " ":
					parts.append(folder)
				parts.append(name if ext == " " else f"{name}.{ext}")
				entries.append(VpkEntry("/".join(parts), crc, preload, archive, offset, length))

	return entries, header_size + tree_size


def safe_dest(out_root, vpk_path):
	"""Map a VPK path to a file under out_root, refusing anything that escapes it."""
	rel = Path(*[p for p in vpk_path.replace("\\", "/").split("/") if p not in ("", ".")])
	if rel.is_absolute() or ".." in rel.parts:
		raise ValueError(f"unsafe path in VPK: {vpk_path!r}")
	return out_root / rel


def extract_vpk(dir_path, out_root, path_filter, force, verify, dry_run):
	entries, dir_data_offset = read_vpk_dir(dir_path)
	if path_filter:
		entries = [e for e in entries if any(e.path.lower().startswith(p) for p in path_filter)]

	paths = {e.path.lower() for e in entries}
	total_bytes = sum(e.size for e in entries)
	print(f"  {dir_path.name}: {len(entries)} files, {total_bytes / 2**20:.1f} MiB")
	if dry_run:
		for e in entries:
			print(f"    {e.path}  ({e.size} bytes)")
		return (len(entries), 0, 0), paths

	stem = dir_path.name[:-len("_dir.vpk")]
	handles = {}
	written = skipped = errors = 0

	def archive_file(index):
		if index not in handles:
			p = dir_path if index == DIR_ARCHIVE_INDEX else dir_path.with_name(f"{stem}_{index:03d}.vpk")
			handles[index] = open(p, "rb")
		return handles[index]

	try:
		# Sort by archive and offset so reads are sequential.
		entries.sort(key=lambda e: (e.archive, e.offset))
		for i, e in enumerate(entries, 1):
			dest = safe_dest(out_root, e.path)
			if not force and dest.is_file() and dest.stat().st_size == e.size:
				skipped += 1
				continue

			dest.parent.mkdir(parents=True, exist_ok=True)
			tmp = dest.with_name(dest.name + ".part")
			crc = zlib.crc32(e.preload)
			with open(tmp, "wb") as out:
				out.write(e.preload)
				if e.length:
					src = archive_file(e.archive)
					offset = e.offset + (dir_data_offset if e.archive == DIR_ARCHIVE_INDEX else 0)
					src.seek(offset)
					remaining = e.length
					while remaining:
						chunk = src.read(min(CHUNK, remaining))
						if not chunk:
							raise IOError(f"unexpected end of archive {e.archive} reading {e.path}")
						crc = zlib.crc32(chunk, crc)
						out.write(chunk)
						remaining -= len(chunk)

			if verify and crc != e.crc and KNOWN_BAD_VPK_CRCS.get((e.path.lower(), e.crc)) == crc:
				print(f"    note: {e.path} has a known-wrong CRC in the retail VPK; data is correct")
			elif verify and crc != e.crc:
				print(f"    CRC MISMATCH: {e.path} (expected {e.crc:08x}, got {crc:08x})", file=sys.stderr)
				tmp.unlink()
				errors += 1
				continue

			os.replace(tmp, dest)
			written += 1
			if i % 500 == 0 or i == len(entries):
				print(f"    {i}/{len(entries)}", flush=True)
	finally:
		for h in handles.values():
			h.close()

	return (written, skipped, errors), paths


# Top-level folders and file types in a game dir that are not game content.
LOOSE_SKIP_DIRS = {"bin", "save", "custom", "downloads"}
LOOSE_SKIP_SUFFIXES = (".vpk", ".cache")


def copy_loose(game_dir, out_root, vpk_paths, path_filter, force, dry_run):
	"""Copy loose content files (maps, cfg, resource, ...) from a game dir.

	The engine searches VPKs before loose folders, so a path that a VPK also
	provides is skipped: the VPK copy is what the game actually loads.
	"""
	files = []
	for src in game_dir.rglob("*"):
		if not src.is_file():
			continue
		rel = src.relative_to(game_dir).as_posix()
		if rel.split("/", 1)[0].lower() in LOOSE_SKIP_DIRS or rel.lower().endswith(LOOSE_SKIP_SUFFIXES):
			continue
		if path_filter and not any(rel.lower().startswith(p) for p in path_filter):
			continue
		if rel.lower() in vpk_paths:
			continue
		files.append((src, rel))

	total_bytes = sum(src.stat().st_size for src, _ in files)
	print(f"  loose files: {len(files)} files, {total_bytes / 2**20:.1f} MiB")
	if dry_run:
		for src, rel in files:
			print(f"    {rel}  ({src.stat().st_size} bytes)")
		return len(files), 0, 0

	written = skipped = 0
	for src, rel in files:
		dest = safe_dest(out_root, rel)
		if not force and dest.is_file() and dest.stat().st_size == src.stat().st_size:
			skipped += 1
			continue
		dest.parent.mkdir(parents=True, exist_ok=True)
		tmp = dest.with_name(dest.name + ".part")
		shutil.copyfile(src, tmp)
		os.replace(tmp, dest)
		written += 1
	return written, skipped, 0


# --------------------------------------------------------------------------

def main():
	ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	ap.add_argument("--hl2-dir", type=Path,
					help='the "Half-Life 2" install folder (auto-detected from Steam if omitted)')
	ap.add_argument("--out", type=Path, default=DEFAULT_OUT,
					help=f"output folder (default: {DEFAULT_OUT})")
	ap.add_argument("--only", metavar="GAMES",
					help=f"comma-separated games to extract: {','.join(GAMES)} (default: all)")
	ap.add_argument("--filter", metavar="PREFIX", action="append", default=[],
					help="only extract paths starting with PREFIX, e.g. sound/ (repeatable)")
	ap.add_argument("--list", action="store_true", help="list files without extracting")
	ap.add_argument("--force", action="store_true", help="overwrite files that already exist")
	ap.add_argument("--no-verify", action="store_true", help="skip CRC checks")
	args = ap.parse_args()

	hl2_dir = args.hl2_dir or find_hl2_dir()
	if not hl2_dir or not (hl2_dir / "hl2").is_dir():
		sys.exit('Could not find Half-Life 2. Pass --hl2-dir "<path to Half-Life 2>".')

	games = None
	if args.only:
		games = {g.strip().lower() for g in args.only.split(",") if g.strip()}
		unknown = games - set(GAMES)
		if unknown:
			sys.exit(f"Unknown game(s): {', '.join(sorted(unknown))}")

	path_filter = [p.replace("\\", "/").lower() for p in args.filter]

	print(f"Game:   {hl2_dir}")
	print(f"Output: {args.out.resolve()}")

	totals = [0, 0, 0]
	missing = []
	for game in GAMES:
		if games and game not in games:
			continue
		vpks = sorted((hl2_dir / game).glob("*_dir.vpk"))
		if not vpks:
			missing.append(str(hl2_dir / game))
			continue
		print(f"[{game}]")
		vpk_paths = set()
		for dir_path in vpks:
			result, paths = extract_vpk(dir_path, args.out / game, path_filter, args.force,
										not args.no_verify, args.list)
			vpk_paths |= paths
			totals = [a + b for a, b in zip(totals, result)]
		result = copy_loose(hl2_dir / game, args.out / game, vpk_paths, path_filter,
							args.force, args.list)
		totals = [a + b for a, b in zip(totals, result)]

	if missing:
		print("\nNo VPKs found in (is the game/episode installed on the steam_legacy branch?):", file=sys.stderr)
		for m in missing:
			print(f"  {m}", file=sys.stderr)

	if not args.list:
		print(f"\nDone: {totals[0]} written, {totals[1]} already up to date, {totals[2]} errors")
	return 1 if (missing or totals[2]) else 0


if __name__ == "__main__":
	sys.exit(main())
