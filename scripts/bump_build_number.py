"""
Pre-build script: maintains a persistent build counter in scripts/.build_number,
incrementing it on every PlatformIO build and exposing it to firmware source as
the BUILD_NUMBER compile-time define (see HAXXCOUNTER_BUILD_NUMBER in counter.h).
"""
Import("env")   # noqa: F821 — SCons build variable

import os

counter_file = os.path.join(env.subst("$PROJECT_DIR"), "scripts", ".build_number")

build_number = 0
if os.path.exists(counter_file):
    with open(counter_file, "r") as f:
        try:
            build_number = int(f.read().strip())
        except ValueError:
            build_number = 0

build_number += 1

with open(counter_file, "w") as f:
    f.write(str(build_number))

env.Append(CPPDEFINES=[("BUILD_NUMBER", build_number)])
