#!/usr/bin/env python3
import os
import subprocess

def system(cmd, default):
    try:
        return subprocess.check_output(cmd.split()).decode("ascii").strip()
    except subprocess.CalledProcessError:
        return default

if __name__ == "__main__":
    base_path = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.normpath(os.path.join(base_path, "..", ".."))

    short_hash = system("git rev-parse --short=7 HEAD", "0" * 7)
    version = system("git describe --tags --always", short_hash)

    version_file = os.path.join(repo_root, "VERSION")
    if version == short_hash and os.path.isfile(version_file):
        with open(version_file, "r", encoding="ascii") as f:
            version = f.read().strip() or version

    mappings = {
        "GIT_COMMIT_HASH": version,
    }

    in_path = os.path.join(base_path, "VersionHash.hpp.in")
    out_path = os.path.join(base_path, "VersionHash.hpp")

    with open(in_path, "r") as f:
        version_str = f.read()

    for mapping, value in mappings.items():
        version_str = version_str.replace("@" + mapping + "@", value)

    with open(out_path, "w") as f:
        f.write(version_str)