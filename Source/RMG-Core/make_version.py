#!/usr/bin/env python3
import os
import subprocess

def system(cmd, default):
    try:
        return subprocess.check_output(cmd.split()).decode("ascii").strip()
    except subprocess.CalledProcessError:
        return default

if __name__ == "__main__":
    hash = system("git rev-parse --short=7 HEAD", "0" * 7)  # Ensuring a 7-character fallback

    mappings = {
        "GIT_COMMIT_HASH": hash,
    }

    base_path = os.path.dirname(os.path.abspath(__file__))
    in_path = os.path.join(base_path, "VersionHash.hpp.in")
    out_path = os.path.join(base_path, "VersionHash.hpp")

    with open(in_path, "r") as f:
        version_str = f.read()

    for mapping, value in mappings.items():
        version_str = version_str.replace("@" + mapping + "@", value)

    if os.path.exists(out_path):
        with open(out_path, "r") as f:
            if f.read() == version_str:
                raise SystemExit(0)

    with open(out_path, "w") as f:
        f.write(version_str)