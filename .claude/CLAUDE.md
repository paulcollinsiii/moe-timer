# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Reference docs (read when the task touches the area, not preemptively):
- `docs/developer_setup.md` — container setup, configuration and credentials, build/flash/monitor, pre-commit hooks, project layout, running tests
- `docs/architecture.md` — overview, the wake end to end, and the system-invariants table; `docs/architecture/` has one page per subsystem (hardware, layers and modules, state, wake cycle, display, network and HA, OTA, peripherals)
- `docs/product_overview.md` + `docs/behavior/` — what the device does, one feature per page (timers and schedule, screen breaks, chores, locks, power and sync)
- `docs/home_assistant/` — HA setup runbook, dashboard, configuring, reference (topics, entities), troubleshooting
- `docs/agent_notes/` — agent-facing notes (layering edges, RTC memory and reboots, HA internals); start at its `README.md`

## Process Rules

- TDD is required — write unit tests before implementing modules. Priority modules: `timer.c`, `schedule.c`, `nvs_config.c`, `display_layout.c`, `components/ssd1680/ssd1680_guard.c`
- All development on feature branches; never commit directly to `main`. Git worktrees only when a task splits across multiple agents in parallel — a single agent uses a plain feature branch
- Planning uses the superpowers skill (`superpowers:writing-plans`) when that plugin is enabled
- Commit messages follow Conventional Commits, enforced by the commit-msg hook: `<type>[(<scope>)][!]: <description>` with types `feat|fix|docs|refactor|test|chore|perf|style|ci|build|revert`
- `.c`/`.h` files must pass `clang-format --dry-run --Werror` (vendored `test/unity/` and `lib/cJSON/` excluded); auto-fix with `clang-format -i <file>`
- **`main/main.c` is under adversarial review.** Any diff that adds a line to it must justify, in the review, why that line cannot live in a host-tested module. main.c is the only large file with no test coverage; 12 of its 20 historical `fix:` commits were ordering defects in the wake orchestration that a host test would have caught. It also refilled from 1118 to 1342 lines in the twelve days after the last extraction round, because nothing pushed back.

  The only admissible reasons for residency are:
  1. **Boot ordering is a hardware contract** — the NeoPixel gate, NVS init, TZ, `timer_defs_install()` sequence in `app_main`.
  2. **It runs in an ISR or `esp_timer` context** where the module API is not safe (e.g. the awake failsafe).
  3. **It owns an ESP-IDF handle with no module home** (`esp_timer_handle_t`). A *handle* means live state this file allocates and must keep — not merely an ESP-IDF call. Bare calls are explicitly **not** covered: a device call can live in a module and be stubbed by that module's host test, which is what every other driver call in this tree already does. `gpio_hold_en` and `esp_deep_sleep_start` were once listed here as examples; they are bare calls, they were read as licence for "it touches ESP-IDF, so it stays", and that is the escape hatch this rule exists to close. The test is testability: if a host test could cover the line by stubbing the call, the line moves.
  4. **It is a ≤3-line thunk** adapting a module ABI to another module's callback signature, containing no branch.

  "It's only a few lines", "it's obviously correct" and "it's just plumbing" are **not** admissible. An `if` in main.c that is not a null-guard on an injected pointer is a review blocker. If the reviewer cannot name which of the four reasons applies, the code moves to a module with a host test. Extraction targets and the seam design: `docs/planning/implemented/20260729.refactormain.plan.md`.

## Build & Test (quick reference)

```bash
source ~/esp/esp-idf/export.sh && idf.py build            # firmware
cmake -S test -B test/build && cmake --build test/build \
  && ctest --test-dir test/build --output-on-failure      # host tests (no ESP-IDF)
```

`sdkconfig` is generated from `sdkconfig.defaults` and gitignored, and it holds every hand-set menuconfig value, so **never delete it**. To pick up new Kconfig options, snapshot it (`cp sdkconfig sdkconfig.bak`), run `idf.py reconfigure` before building, then diff against the copy. To pick up one changed default, set that value in menuconfig: whether an existing `sdkconfig` or a changed `sdkconfig.defaults` line wins is symbol-dependent; measure it with the probe in `docs/developer_setup.md` ("`sdkconfig` holds your hand-set values").

## Development rules

Rules for whoever changes the code. Breaking one breaks the device or the test contract:

- **Pure C** — no C++ translation units.
- **IDF 6 component names** go in `REQUIRES`: `main` needs `esp_driver_gpio`, `esp_driver_dac` and `esp_driver_rmt`; `components/ssd1680` needs `esp_driver_spi` and `esp_driver_gpio`. The `driver` umbrella no longer pulls them in.
- **`timer_defs_install()` runs every boot before any `timer_*` call that reads a definition** (only `timer_rtc_state_guard()` precedes it). Host tests inject their own table with `timer_set_defs()`.
- **`APP_MODE_TIMERS` must stay 0**: `timer_reset`'s memset is how the rollover returns the device to Timers (`include/timer.h`, the comment above `app_mode_t`).
- **Keep refresh policy and panel protection separate.** Partial/full cadence is `display.c` policy; BUSY serialization and the 1 s guard belong to the ssd1680 driver. Don't merge them.
- **Display goldens are the layout contract**: screens byte-compare against `test/test_display_render/golden/*.bin`. Regenerate them (`MAGTAG_WRITE_GOLDEN=1`) only for an intentional layout change, never to pass an unexpected diff. The flag rewrites every golden, so check out the ones you did not mean to change.
- **Bump the layout version on any layout change**: `TIMER_SNAPSHOT_VERSION` for `timer_snapshot_t`, `RTC_STATE_VERSION` for `rtc_state_t` (`include/timer.h`).
- **A new or renamed HA entity bumps `STATS_JSON_DISC_SCHEMA_VER`** (`include/stats_json.h`), or HA never sees it.
- **A field settable from an HA entity must also be expressible in the bulk config document.** A `set/<key>` command is cleared once applied, so an entity-only field has no copy off the device, and it is lost whenever NVS is rebuilt from the document (a defaults reseed, or an unreadable timer table). See the durability-rule comment above `FIELDS` in `main/ha_config.c`.

System invariants (the NeoPixel gate, clock steps and expiry, locks, OTA) are in `docs/architecture.md#system-invariants`.
