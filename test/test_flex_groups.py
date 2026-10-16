#!/usr/bin/env python3
"""Exact generator -> FLEX_NEXT delivery checks, not enrollment substring checks."""
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def run(command):
    return subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          text=True, check=True, timeout=60).stdout


def main():
    wrapper = shlex.split(os.environ.get("WINE_CMD", ""))
    gen = wrapper + [os.environ.get("GEN_NG", "./build/gen-ng")]
    decoder = wrapper + [os.environ.get("MULTIMON", "./build/multimon-ng"),
                         "-t", "raw", "-q", "-a", "FLEX_NEXT"]
    with tempfile.TemporaryDirectory(prefix="flex-groups-") as temp:
        raw = Path(temp) / "group.raw"
        for count in (92, 93, 126, 200, 5461):
            start = 1000000
            message = f"group {count}"
            run(gen + ["-t", "raw", "-f", message, "-G", f"{start}:{count}", str(raw)])
            output = run(decoder + [str(raw)])
            lines = [line.split("|") for line in output.splitlines()
                     if line.startswith("FLEX_NEXT|") and "|ALN|" in line]
            assert len(lines) == 1, f"{count}: expected one ALN delivery, got {len(lines)}"
            expected = [2029568] + list(range(start, start + count))
            assert lines[0][4] == " ".join(f"{cap:010d}" for cap in expected), count
            assert lines[0][5] == "T" and lines[0][-1] == message, count
            assert "K+" in lines[0][7] and "SIG+" in lines[0][7], count
            objects = [json.loads(line) for line in run(decoder + ["--json", str(raw)]).splitlines()
                       if line.startswith("{")]
            messages = [obj for obj in objects if obj.get("msg_type") == "alphanumeric"]
            assert len(messages) == 1, count
            obj = messages[0]
            assert obj["group_capcodes"] == expected[1:], count
            assert obj["capcode"] == expected[0] and obj["is_group"], count
            assert obj["message"] == message and obj["k_ok"] and obj["sig_ok"], count
            assert obj["cycle"] == 0 and obj["frame"] == (count + 42) // 43, count
            raw.unlink()
            print(f"FLEX group {count}: exact ALN member field and JSON array passed")

        # The comma-list path must enforce the same short-address range.
        run(gen + ["-t", "raw", "-f", "edges", "-G", "1,1933312", str(raw)])
        objects = [json.loads(line) for line in run(decoder + ["--json", str(raw)]).splitlines()
                   if line.startswith("{")]
        messages = [obj for obj in objects if obj.get("msg_type") == "alphanumeric"]
        assert len(messages) == 1 and messages[0]["group_capcodes"] == [1, 1933312]
        raw.unlink()
        for option, value in [
            ("-F", value) for value in ("0", "1933313", "2000000", "2029568",
                                       "4294967297", "18446744073709551616", "-1", "1junk", "")
        ] + [
            ("-G", value) for value in ("0:1", "1933312:2", "2000000:200", "1:5462",
                                       "1:22016", "4294967297:1", "1:2junk", "1:-1",
                                       "1,2000000", "1,", ",1", "1,,2", "bad", "1:1:1")
        ]:
            result = subprocess.run(gen + ["-t", "raw", "-f", "invalid", option, value, str(raw)],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            assert result.returncode == 2 and not raw.exists(), (option, value)
        for capcode in (1, 1933312):
            run(gen + ["-t", "raw", "-f", "edge", "-F", str(capcode), str(raw)])
            objects = [json.loads(line) for line in run(decoder + ["--json", str(raw)]).splitlines()
                       if line.startswith("{")]
            messages = [obj for obj in objects if obj.get("msg_type") == "alphanumeric"]
            assert len(messages) == 1 and messages[0]["capcode"] == capcode
            raw.unlink()
        print("FLEX address boundaries and invalid generator inputs passed")


if __name__ == "__main__":
    main()
