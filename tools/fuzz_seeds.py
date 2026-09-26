#!/usr/bin/env python3
"""Writes a seed corpus for the fuzz target from the conformance vectors:
every case's input as a TNC2 line or AX.25 frame, one file each.

  python3 tools/fuzz_seeds.py vectors fuzz-corpus
"""
import hashlib
import json
import sys
from pathlib import Path


def main():
    vectors, out = Path(sys.argv[1]), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    count = 0
    for f in sorted((vectors / "cases").glob("*.json")):
        for case in json.loads(f.read_text(encoding="utf-8"))["cases"]:
            i = case["input"]
            if "tnc2" in i:
                data = i["tnc2"].encode("utf-8")
            elif "tnc2_hex" in i:
                data = bytes.fromhex(i["tnc2_hex"])
            elif "ax25_hex" in i:
                data = bytes.fromhex(i["ax25_hex"])
            elif "info" in i or "info_hex" in i:
                info = i["info"].encode("utf-8") if "info" in i else bytes.fromhex(i["info_hex"])
                header = "%s>%s" % (i.get("source", "N0CALL"), i.get("destination", "APZ001"))
                for p in i.get("path", []):
                    header += "," + p
                data = header.encode() + b":" + info
            else:
                continue
            (out / hashlib.sha1(data).hexdigest()).write_bytes(data)
            count += 1
    print(f"{count} seeds in {out}")


if __name__ == "__main__":
    main()
