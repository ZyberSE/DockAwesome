"""Reserve a local build revision and generate the linked version strings."""

import fcntl
import sys
from datetime import date
from pathlib import Path


def next_revision():
    state = Path(".build-revision")
    with state.with_suffix(".lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        try:
            previous = int(state.read_text(encoding="ascii"))
        except FileNotFoundError:
            previous = 0
        if previous < 0:
            raise ValueError("Build revision cannot be negative")
        revision = previous + 1
        pending = state.with_suffix(".new")
        pending.write_text(f"{revision}\n", encoding="ascii")
        pending.replace(state)
    return revision


# Usage: build-version.py name output [release]
# A release such as "1.0 beta 1" replaces the local build counter. $VER keeps
# the number before the date, where the Version command reads it. A local
# build is not a version: About shows "Build N", and $VER carries 0.N for
# the Version command plus "build N" after the date.
name, output, *rest = sys.argv[1:]
release = rest[0].strip() if rest and rest[0].strip() else ""
built = date.today()
if release:
    label = f"Version {release}"
    number, _, suffix = release.partition(" ")
    tag = f"{name} {number} ({built:%d.%m.%Y})" + (f" {suffix}" if suffix else "")
else:
    revision = next_revision()
    major, minor = divmod(revision, 65536)
    label = f"Build {revision}"
    tag = f"{name} {major}.{minor} ({built:%d.%m.%Y}) build {revision}"
Path(output).write_text(
    '#include "amiga_version.h"\n'
    f'const char dock_build_version[] = "{label}";\n'
    f'const char dock_build_date[] = "Built {built.isoformat()}";\n'
    f'const char dock_build_tag[] = DOCK_BUILD_TAG_PREFIX "{tag}";\n',
    encoding="ascii",
)
