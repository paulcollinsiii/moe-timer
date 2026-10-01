cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(magtag_timer)

# ---- Build-size guard: fail the build past MAGTAG_MAX_SLOT_PCT of the slot ----
#
# The app slot is 0x1C0000 and partitions.csv freezes it: an OTA cannot
# rewrite the partition table, so the layout shipped with the first
# OTA-capable firmware is the layout those devices keep short of a serial
# cable. That makes headroom a one-way resource. ESP-IDF already errors when
# the image will not fit and prints -- exit 0 -- a notice under 5 % free
# (components/partition_table/check_sizes.py), but both alarms arrive after
# the decision that mattered: at 5 % free there is nothing left to give back.
#
# This FAILS the build rather than warning, deliberately, and the plan text
# at docs/planning/implemented/ota.plan.md:157 says "warn". A printed warning has one
# reader -- whoever happens to be watching that build -- and this repo has no
# CI to catch what they miss. A threshold you must raise in a commit, with a
# reason, converts silent erosion into a reviewed decision. 85 % reserves the
# top 15 % of the slot -- 269 KB -- and the guard fires when the image crosses
# into it. How much room that actually leaves is a property of the image, not
# of the threshold, so it moves with every build: when this landed it was
# ~63 KB, which is the granularity that makes the guard bite (one more
# Montserrat font at ~20-40 KB fitted, two did not). The pass line printed on
# every build carries the live figure; this comment cannot.
#
# MAGTAG_MAX_SLOT_PCT is the documented override. Raising it is a legitimate
# act -- it accepts a new floor -- and it belongs in its own commit whose
# message says what the bytes bought. Only 1..100 are meaningful; the tool
# rejects anything else rather than quietly disabling itself.
#
# Two-sided check when touching this, same discipline as the
# -Werror=unused-function re-arm at the bottom of main/CMakeLists.txt: the
# build must pass as-is, AND lowering MAGTAG_MAX_SLOT_PCT below the current
# occupancy must make `idf.py build` exit nonzero. A guard that only
# satisfies the first half is manufacturing confidence, not checking
# anything. The arithmetic itself is covered from both sides by
# test/test_check_slot_size/, which can drive the failure branch with
# synthetic sizes -- something no real build can do without growing the
# image by tens of KB first.
#
# Wired here rather than in main/CMakeLists.txt because the `app` target
# does not exist yet while component CMakeLists are being processed: IDF
# creates it in idf_build_executable(), which project.cmake calls after
# idf_build_process() has walked the components. The slot size is not
# repeated here either -- check_slot_size.py reads it out of the generated
# partition-table binary, so partitions.csv stays the only place it is
# stated.
#
# The threshold is a plain (non-CACHE) variable on purpose, so that a
# -D MAGTAG_MAX_SLOT_PCT=99 on the command line cannot move it: the whole
# point is that raising it costs a commit. But a shadowed cache entry is
# discarded in *silence* -- the flag lands in CMakeCache.txt, the build still
# runs with 85, and the guard fails again with no hint that the flag did
# nothing. That is the trap this catches, because "raise
# MAGTAG_MAX_SLOT_PCT" is exactly what the failure message says and -D is
# the obvious first attempt. Refuse loudly instead; the policy is unchanged.
if(DEFINED CACHE{MAGTAG_MAX_SLOT_PCT})
    message(FATAL_ERROR
        "MAGTAG_MAX_SLOT_PCT cannot be set with -D (got "
        "'$CACHE{MAGTAG_MAX_SLOT_PCT}'). The build-size guard's threshold is "
        "deliberately not a cache variable: the line below shadows any cache "
        "entry, so the flag would be ignored without warning and the build "
        "would fail at the same threshold as before.\n"
        "Raising the guard is meant to cost a commit. Edit "
        "set(MAGTAG_MAX_SLOT_PCT ...) in CMakeLists.txt (project root) as its "
        "own commit, saying in the message what the bytes bought.\n"
        "Nothing to clean up under idf.py: it deletes CMakeCache.txt "
        "whenever a configure fails, so just re-run without the flag. "
        "Driving cmake directly, the entry does persist -- clear it with "
        "cmake -U MAGTAG_MAX_SLOT_PCT -B <build dir>.")
endif()
set(MAGTAG_MAX_SLOT_PCT 85)

idf_build_get_property(magtag_python PYTHON)
idf_build_get_property(magtag_build_dir BUILD_DIR)
idf_build_get_property(magtag_project_bin PROJECT_BIN)
idf_build_get_property(magtag_table_bin PARTITION_TABLE_BIN_PATH)

# A custom target is never "up to date", so this runs on every build --
# including the no-op rebuild after a one-line change, which is exactly the
# build where an unnoticed 40 KB font would otherwise sail through. It hangs
# off `app` the same way IDF's own app_check_size does, so it is part of the
# default target and not an opt-in one nobody invokes.
add_custom_target(magtag_check_slot_size
    COMMAND ${magtag_python} ${CMAKE_CURRENT_LIST_DIR}/tools/check_slot_size.py
            --partition-table "${magtag_table_bin}"
            --image "${magtag_build_dir}/${magtag_project_bin}"
            --max-pct ${MAGTAG_MAX_SLOT_PCT}
    DEPENDS gen_project_binary partition_table_bin
    VERBATIM)
add_dependencies(app magtag_check_slot_size)
