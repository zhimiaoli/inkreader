#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""合并 S31 (metalio47) 四个刷机文件为单个 16MB 整片 Flash 镜像。

用法（用 IDF 的 Python 环境，与编译相同）:
  D:\esp\espressif\python_env\idf6.1_py3.11_env\Scripts\python.exe tools\merge_s31_flash_image.py

可选参数:
  --package <目录>   默认 build/boards/packages/metalio47
  --output <文件>    默认 <package>/kiikoread-metalio47-full-16mb.bin
"""

import argparse
import hashlib
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PACKAGE = ROOT / "build" / "boards" / "packages" / "metalio47"

# 与 boards/metalio47/partitions.csv 及 tools/build_firmware.py 导出保持一致
PARTS = [
    ("bootloader.bin", 0x2000),
    ("partitions.bin", 0x8000),
    ("firmware.bin", 0x10000),
    ("ota_data_initial.bin", 0xD000),
]
CHIP = "esp32s31"          # boards/metalio47/board.json: idf_target
S31_CHIP_ID = 32           # board.json: image_chip_id
FLASH_MODE, FLASH_FREQ, FLASH_SIZE = "dio", "80m", "16MB"


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--package", type=Path, default=PACKAGE)
    ap.add_argument("--output", type=Path, default=None)
    args = ap.parse_args()

    pkg = args.package.resolve()
    out = args.output.resolve() if args.output else pkg / "kiikoread-metalio47-full-16mb.bin"

    for name, _ in PARTS:
        p = pkg / name
        if not p.is_file():
            print(f"缺少 {name}（请先用 tools/build_firmware.py --board metalio47 编译）")
            return 1

    boot = (pkg / "bootloader.bin").read_bytes()
    if boot[0] != 0xE9 or struct.unpack_from("<H", boot, 12)[0] != S31_CHIP_ID:
        print("bootloader.bin 不是 esp32s31 的镜像，拒绝合并")
        return 1
    table = (pkg / "partitions.bin").read_bytes()
    if struct.unpack_from("<H", table, 0)[0] != 0x50AA:
        print("partitions.bin 分区表魔数不对，拒绝合并")
        return 1

    pairs = []
    for name, offset in PARTS:
        pairs.extend([f"{offset:#x}", name])
        print(f"{offset:#09x}  {name:<24} {sha256(pkg / name)}")

    cmd = [sys.executable, "-m", "esptool", "--chip", CHIP, "merge-bin",
           "--flash-mode", FLASH_MODE, "--flash-freq", FLASH_FREQ,
           "--flash-size", FLASH_SIZE, "--fill-flash-size", FLASH_SIZE,
           "-o", str(out), *pairs]
    print("\n" + " ".join(cmd) + "\n")
    subprocess.run(cmd, check=True, cwd=pkg)

    print(f"\n生成: {out}  ({out.stat().st_size / 1024 / 1024:.1f} MB)")
    print(f"SHA256: {sha256(out)}")
    print("注意: 整片镜像包含空白填充，write-flash 会覆盖 NVS 设置、"
          "书籍 storage 分区和另一 OTA 槽；数据保留升级请继续用原 4 文件流程。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
