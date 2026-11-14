#!/usr/bin/env python3
"""Checks limits.json against the capacities in include/pdn_aprs.h and the
README, so the machine-readable list cannot drift from the library."""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
header = (ROOT / "include" / "pdn_aprs.h").read_text()
readme = (ROOT / "README.md").read_text()
limits = json.loads((ROOT / "limits.json").read_text())


def macro(name):
    m = re.search(r"#define\s+%s\s+(\d+)" % name, header)
    return int(m.group(1)) if m else None


def extra_value():
    m = re.search(r"typedef struct pdn_aprs_weather_extra \{.*?char value\[(\d+)\]", header, re.S)
    return int(m.group(1)) - 1 if m else None


want = {
    "information-field": macro("PDN_APRS_MAX_INFO"),
    "path": macro("PDN_APRS_MAX_PATH"),
    "capabilities": macro("PDN_APRS_MAX_CAPABILITIES"),
    "weather-extras": macro("PDN_APRS_MAX_WEATHER_EXTRA"),
    "weather-extra-value": extra_value(),
}
problems = 0
if limits != want:
    print(f"limits.json is {limits}, the header has {want}")
    problems += 1
for name in want:
    if "`%s`" % name not in readme:
        print(f"README.md does not name the limit {name}")
        problems += 1
print(f"limits.json checked, {problems} problems")
sys.exit(1 if problems else 0)
