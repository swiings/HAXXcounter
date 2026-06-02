"""
Pre-build script: adds the project's include/ directory to the global CPPPATH
so that LVGL (and any other library) can find lv_conf.h via #include "lv_conf.h".

PlatformIO does not automatically add the project include/ directory to
third-party library compilation environments; this script bridges that gap.
"""
Import("env")   # noqa: F821 — SCons build variable

import os

include_dir = os.path.join(env.subst("$PROJECT_DIR"), "include")
env.Append(CPPPATH=[include_dir])
