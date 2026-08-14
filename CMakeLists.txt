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
# at docs/planning/ota.plan.md:152 says "warn". A printed warning has one
# reader -- whoever happens to be watching that build -- and this repo has no
# CI to catch what they miss. A threshold you must raise in a commit, with a
# reason, converts silent erosion into a reviewed decision. 85 % leaves
# 63 KB: one more Montserrat font (~20-40 KB) fits, two do not, which is the
# conversation this is meant to force.
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
