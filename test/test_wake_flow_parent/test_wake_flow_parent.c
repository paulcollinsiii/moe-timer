/* The same suite, built for a ParentTesting=y firmware.

   Why this exists as a second binary rather than as more cases in the
   first one: PARENT_TESTING is a compile-time Kconfig bool, so one
   translation unit can only ever hold one value of it. The default host
   build has it FALSE, which means `timer_reload_allowed(PARENT_TESTING)`
   and `timer_reload_allowed(false)` are the same call there — a swap
   between them is invisible, while on a =y device (which is how this
   project's sdkconfig actually ships) it silently removes the parent
   escape that lets a non-reloadable slot be reset at all.

   Mutation testing is what surfaced it: the "gate asked with false"
   mutant escaped the default build while "gate asked with true" was
   caught, so the flag's forwarding was pinned in one direction only.
   Building the identical cases against the other configuration closes
   the pair. test_wake_flow.c's Button B cases are written to hold under
   both values (the RUNNING refusal dominates either way, and the
   build-flag assertion is expressed in terms of PARENT_TESTING itself),
   so no case here needs to differ.

   CONFIG_MAGTAG_PARENT_TESTING is set by the build (EXTRA_DEFS in
   test/CMakeLists.txt) rather than defined here, so the macro arrives
   exactly the way sdkconfig.h delivers it on firmware. */
// clang-format off
#include "../test_wake_flow/test_wake_flow.c"
// clang-format on
