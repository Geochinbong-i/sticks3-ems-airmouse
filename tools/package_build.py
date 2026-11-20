"""Package compiled StickS3 firmware binaries and SHA256 manifest."""
import csv
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
OUT = ROOT / "dist"
OUT.mkdir(exist_ok=True)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


flash = json.loads((BUILD / "flasher_args.json").read_text())
assert flash.get("flash_files") and flash.get("app", {}).get("file"), "Missing flash metadata"
assert json.loads((BUILD / "project_description.json").read_text())["target"] == "esp32s3"

source_sha = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
app_bin = BUILD / flash["app"]["file"]
assert app_bin.is_file(), f"Missing {app_bin}"

subprocess.run(
    [
        sys.executable,
        "-m",
        "esptool",
        "--chip",
        "esp32s3",
        "merge_bin",
        "-o",
        str(OUT / "sticks3-ems-airmouse-full.bin"),
        "@flash_args",
    ],
    cwd=BUILD,
    check=True,
)
shutil.copy2(app_bin, OUT / "sticks3-ems-airmouse-app.bin")

for offset, name in flash["flash_files"].items():
    p = BUILD / name
    assert p.is_file(), p
    shutil.copy2(p, OUT / f"{offset}-{p.name}")

for p in [
    BUILD / "flash_args",
    BUILD / "flasher_args.json",
    BUILD / "project_description.json",
    ROOT / "partitions.csv",
]:
    if p.is_file():
        shutil.copy2(p, OUT / p.name)

rows = [
    r
    for r in csv.reader((ROOT / "partitions.csv").read_text().splitlines())
    if r and not r[0].lstrip().startswith("#")
]
slot = next(r for r in rows if r[0].strip() == "ota_0")
assert int(slot[3].strip(), 0) == int(flash["app"]["offset"], 0)
assert app_bin.stat().st_size <= int(slot[4].strip(), 0), "App exceeds ota_0 slot"
assert (OUT / "sticks3-ems-airmouse-full.bin").stat().st_size <= 8 * 1024 * 1024

manifest = {
    "source_sha": source_sha,
    "idf": subprocess.check_output(["idf.py", "--version"], text=True).strip(),
    "target": "esp32s3",
    "flash_bytes": 8 * 1024 * 1024,
    "app_offset": flash["app"]["offset"],
    "app_size": app_bin.stat().st_size,
    "slot_bytes": int(slot[4].strip(), 0),
    "files": {
        p.name: sha256(p)
        for p in sorted(OUT.iterdir())
        if p.is_file() and p.name not in ("SHA256SUMS.txt", "build-manifest.json")
    },
}
(OUT / "build-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
(OUT / "SHA256SUMS.txt").write_text(
    "".join(
        f"{sha256(p)}  {p.name}\n"
        for p in sorted(OUT.iterdir())
        if p.is_file() and p.name != "SHA256SUMS.txt"
    )
)
print(json.dumps({"source_sha": source_sha, "app_bytes": app_bin.stat().st_size}))
