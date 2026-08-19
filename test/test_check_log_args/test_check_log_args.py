#!/usr/bin/env python3
"""Host tests for scripts/check-log-args.py -- the HAZ-1 log-argument gate.

Why this suite is Python and most suites in test/ are C/Unity: the code under
test is a pre-commit tool, not firmware, exactly like tools/check_slot_size.py
next door. See the comment above its add_test() in test/CMakeLists.txt.

Why it exists at all: the gate is a hand-rolled C lexer plus two classifiers,
and a gate that passes silently converts a known hazard into a believed-solved
one. The first version of the scanner shipped with two silent holes -- a
definition whose opening brace fell outside a fixed 40-character window was
never checked, and the purity tripwire missed `<<=`, indexed writes, member
writes and any file-scope name without an `s_`/`g_` prefix. Both were found by
review rather than by a test, which is the argument for this file. Every case
below that is marked NEGATIVE reproduces something that once slipped through
or once fired wrongly.
"""

import os
import subprocess
import sys
import tempfile
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.normpath(os.path.join(_HERE, "..", ".."))
_SCANNER = os.path.join(_ROOT, "scripts", "check-log-args.py")

PROLOGUE = """\
#include "esp_log.h"
#include "esp_check.h"
static const char *TAG = "t";
int get_x(void);
int get_a(void);
int get_b(void);
int get_tag(void);
esp_err_t doit(void);
int pick_code(void);
"""


class ScannerCase(unittest.TestCase):
    def scan(self, body, name="probe.c", prologue=True):
        """Run the scanner over a synthetic translation unit."""
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, name)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w") as fh:
                fh.write((PROLOGUE if prologue else "") + body)
            r = subprocess.run([sys.executable, _SCANNER, path],
                               capture_output=True, text=True)
        return r.returncode, r.stdout + r.stderr

    def assertClean(self, body, **kw):
        rc, out = self.scan(body, **kw)
        self.assertEqual(rc, 0, "expected no finding, got:\n" + out)

    def assertFlags(self, body, *needles, **kw):
        rc, out = self.scan(body, **kw)
        self.assertEqual(rc, 1, "expected a finding, got none:\n" + out)
        for n in needles:
            self.assertIn(n, out)
        return out


# ---------------------------------------------------------------------------
class TestCallsInLogArguments(ScannerCase):
    def test_plain_call_is_flagged(self):
        self.assertFlags('void f(void) { ESP_LOGI(TAG, "%d", get_x()); }',
                         "get_x()", "ESP_LOGI")

    def test_every_level_is_covered(self):
        for lvl in ("E", "W", "I", "D", "V"):
            self.assertFlags('void f(void) { ESP_LOG%s(TAG, "%%d", get_x()); }' % lvl,
                             "ESP_LOG" + lvl)

    def test_call_nested_inside_an_allowlisted_formatter_is_flagged(self):
        # The allowlist covers the formatter, never its arguments.
        self.assertFlags(
            'void f(void) { ESP_LOGI(TAG, "%s", '
            'wake_flow_reset_reason_str(esp_reset_reason())); }',
            "esp_reset_reason()")

    def test_call_in_the_tag_slot_is_flagged(self):
        self.assertFlags('void f(void) { ESP_LOGW(get_tag(), "hi"); }', "get_tag()")

    def test_ternary_and_comma_and_nesting_are_reached(self):
        out = self.assertFlags(
            'void f(void) {\n'
            '    ESP_LOGI(TAG, "%d", get_x() ? get_a() : get_b());\n'
            '    ESP_LOGI(TAG, "%d", (get_x(), 1));\n'
            '    ESP_LOGI(TAG, "%d", ((((get_a())))));\n'
            '}')
        for n in ("get_x()", "get_a()", "get_b()"):
            self.assertIn(n, out)

    def test_multi_line_statement_is_reached(self):
        self.assertFlags('void f(void) {\n    ESP_LOGI(TAG,\n'
                         '             "%d",\n             get_x());\n}', "get_x()")

    def test_buffer_hexdump_is_covered(self):
        self.assertFlags('void f(void) { ESP_LOG_BUFFER_HEXDUMP(TAG, get_a(), 16, 3); }',
                         "get_a()")

    def test_reported_line_is_the_log_statement(self):
        _, out = self.scan('void f(void) {\n\n\n    ESP_LOGI(TAG, "%d", get_x());\n}')
        self.assertIn("probe.c:%d:" % (PROLOGUE.count("\n") + 4), out)

    # -- NEGATIVE: things that must NOT be reported --------------------------
    def test_a_word_in_a_format_string_is_not_a_call(self):
        self.assertClean('void f(int d) {\n'
                         '    ESP_LOGI(TAG, "button C swap unavailable (state %d)", d);\n}')

    def test_a_call_written_in_a_comment_is_not_a_call(self):
        self.assertClean('void f(void) {\n'
                         '    /* was: ESP_LOGI(TAG, "%d", get_x()); */\n'
                         '    ESP_LOGI(TAG, "none");\n}')

    def test_slashes_and_comment_openers_inside_a_string_are_harmless(self):
        self.assertClean('void f(int d) { ESP_LOGI(TAG, "http://x /* y %d", d); }')

    def test_an_allowlisted_formatter_alone_is_clean(self):
        self.assertClean('void f(int e) { ESP_LOGE(TAG, "%s", esp_err_to_name(e)); }')
        self.assertClean('void f(int r) { ESP_LOGI(TAG, "%s", ota_policy_reason_str(r)); }')

    def test_keywords_are_not_calls(self):
        self.assertClean('void f(void) { ESP_LOGI(TAG, "%u", (unsigned)sizeof(int)); }')


# ---------------------------------------------------------------------------
class TestMacroExemption(ScannerCase):
    """NEGATIVE: a macro expanding to a pure expression carries no hazard.

    Rejecting these would block legitimate commits, and the scanner's advice
    would push the author at the ALLOWLIST -- whose criterion is "returns a
    display string" and fits none of them, diluting the one list that has to
    stay meaningful.
    """

    def test_all_caps_macros_are_exempt(self):
        for expr in ("MIN(1, 2)", "MAX(1, 2)", "BIT(3)", "TIMER_MIN(4)",
                     "NUM_HHMM(5)", "FOLD_U16(6)", "TEXT(7)"):
            self.assertClean('void f(void) { ESP_LOGI(TAG, "%%d", %s); }' % expr)

    def test_vendor_macros_with_a_short_lowercase_prefix_are_exempt(self):
        for expr in ("pdMS_TO_TICKS(50)", "pdTICKS_TO_MS(50)", "portTICK_PERIOD_MS"):
            self.assertClean('void f(void) { ESP_LOGI(TAG, "%%d", (int)%s); }' % expr)

    def test_a_real_function_is_not_mistaken_for_a_macro(self):
        for fn in ("uxTaskGetStackHighWaterMark(NULL)", "esp_get_free_heap_size()",
                   "timer_get_state()", "xTaskGetTickCount()"):
            self.assertFlags('void f(void) { ESP_LOGI(TAG, "%%d", (int)%s); }' % fn,
                             fn.split("(")[0])


# ---------------------------------------------------------------------------
class TestEspCheckFamily(ScannerCase):
    """esp_check.h forwards its variadic args straight into ESP_LOGE."""

    def test_format_varargs_are_covered(self):
        for stmt, macro in (
            ('ESP_RETURN_ON_ERROR(doit(), TAG, "f %d", get_x());',
             "ESP_RETURN_ON_ERROR"),
            ('ESP_GOTO_ON_ERROR(doit(), done, TAG, "f %d", get_x());',
             "ESP_GOTO_ON_ERROR"),
            ('ESP_RETURN_ON_FALSE(get_a() > 0, ESP_FAIL, TAG, "f %d", get_x());',
             "ESP_RETURN_ON_FALSE"),
            ('ESP_GOTO_ON_FALSE(get_a() > 0, ESP_FAIL, done, TAG, "f %d", get_x());',
             "ESP_GOTO_ON_FALSE"),
        ):
            self.assertFlags("void f(void) { %s\ndone: return; }" % stmt,
                             "get_x()", macro)

    def test_isr_variants_are_covered(self):
        self.assertFlags('void f(void) { ESP_RETURN_ON_ERROR_ISR(doit(), TAG, '
                         '"f %d", get_x()); }', "ESP_RETURN_ON_ERROR_ISR")

    # -- NEGATIVE ------------------------------------------------------------
    def test_leading_arguments_are_evaluated_outside_the_log(self):
        # The condition is assigned to err_rc_ and tested outside the log; the
        # error code sits in the return statement. Neither is level-dependent.
        self.assertClean('void f(void) { ESP_RETURN_ON_ERROR(doit(), TAG, "plain"); }')
        self.assertClean('void f(void) { ESP_RETURN_ON_FALSE(get_a() > 0, '
                         'pick_code(), TAG, "plain"); }')
        self.assertClean('void f(void) { ESP_GOTO_ON_FALSE(get_a() > 0, pick_code(), '
                         'done, TAG, "plain");\ndone: return; }')

    def test_the_real_component_using_this_family_is_clean(self):
        src = os.path.join(_ROOT, "components", "ssd1680", "ssd1680.c")
        r = subprocess.run([sys.executable, _SCANNER, src],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


# ---------------------------------------------------------------------------
DEFS_PROLOGUE = """\
typedef enum { OTA_REASON_A, OTA_REASON_COUNT } ota_reason_t;
typedef enum { OTA_REDIRECT_FOLLOW } ota_redirect_t;
typedef int esp_reset_reason_t;
struct cfg { int v; };
static int s_calls;
static int s_flags;
static char s_buf[16];
static int *s_ptr;
static struct cfg s_cfg;
static int plain_counter;
static const char *TABLE[2];
"""


class TestAllowlistPurityTripwire(ScannerCase):
    """An allowlisted repo-local formatter must stay a pure formatter."""

    def defs(self, body, name="probe.c"):
        return self.scan(DEFS_PROLOGUE + body, name=name, prologue=False)

    def flagDef(self, body, *needles, **kw):
        rc, out = self.defs(body, **kw)
        self.assertEqual(rc, 1, "expected a finding, got none:\n" + out)
        for n in needles:
            self.assertIn(n, out)
        return out

    def cleanDef(self, body, **kw):
        rc, out = self.defs(body, **kw)
        self.assertEqual(rc, 0, "expected no finding, got:\n" + out)

    def test_a_call_in_the_body_is_flagged(self):
        self.flagDef('const char *ota_url_redirect_str(ota_redirect_t r) {\n'
                     '    log_it();\n    return "x";\n}', "calls log_it()")

    def test_bare_assignment_to_a_static_is_flagged(self):
        self.flagDef('const char *ota_policy_reason_str(ota_reason_t r) {\n'
                     '    s_calls = 1;\n    return "a";\n}', "assigns to `s_calls`")

    def test_increment_is_flagged(self):
        self.flagDef('const char *ota_policy_reason_str(ota_reason_t r) {\n'
                     '    s_calls++;\n    return "a";\n}', "uses ++/--")

    # -- NEGATIVE: every one of these walked through the first version --------
    def test_shift_assign_is_flagged(self):
        self.flagDef('const char *ota_url_redirect_str(ota_redirect_t r) {\n'
                     '    s_flags <<= 1;\n    return "x";\n}', "assigns to `s_flags`")
        self.flagDef('const char *ota_url_redirect_str(ota_redirect_t r) {\n'
                     '    s_flags >>= 1;\n    return "x";\n}', "assigns to `s_flags`")

    def test_indexed_write_is_flagged(self):
        # The most likely way one of these stops being pure: format into a
        # static buffer and return a pointer to it.
        self.flagDef('const char *ota_policy_reason_str(ota_reason_t r) {\n'
                     '    s_buf[0] = 0x78;\n    return s_buf;\n}',
                     "writes through `s_buf[0]`")

    def test_member_write_is_flagged(self):
        self.flagDef('const char *ota_policy_reason_str(ota_reason_t r) {\n'
                     '    s_cfg.v = 1;\n    return "a";\n}', "writes through `s_cfg.v`")

    def test_pointer_write_is_flagged(self):
        self.flagDef('const char *ota_policy_reason_str(ota_reason_t r) {\n'
                     '    *s_ptr = 3;\n    return "a";\n}', "writes through `*s_ptr`")

    def test_file_scope_name_without_the_s_prefix_is_flagged(self):
        # The first version keyed on an `s_`/`g_` naming convention.
        self.flagDef('const char *ota_policy_reason_str(ota_reason_t r) {\n'
                     '    plain_counter = 7;\n    return "a";\n}',
                     "assigns to `plain_counter`")

    def test_function_local_static_is_flagged(self):
        self.flagDef('const char *ota_url_redirect_str(ota_redirect_t r) {\n'
                     '    static int seen;\n    return "x";\n}',
                     "declares a function-local static")

    def test_a_long_trailing_comment_does_not_hide_the_body(self):
        # F1: the brace fell outside a fixed 40-character window and the whole
        # definition was skipped in silence.
        self.flagDef(
            'const char *wake_flow_reset_reason_str(esp_reset_reason_t reason)'
            ' /* this trailing comment is comfortably longer than forty characters */\n'
            '{\n    s_flags = reason;\n    return "poisoned";\n}',
            "assigns to `s_flags`")

    def test_a_multi_line_signature_does_not_hide_the_body(self):
        self.flagDef('const char *ota_policy_reason_str(\n        ota_reason_t r)\n'
                     '{\n    s_calls = 1;\n    return "a";\n}', "assigns to `s_calls`")

    def test_an_attribute_between_signature_and_brace_is_tolerated(self):
        self.flagDef('const char *ota_url_redirect_str(ota_redirect_t r) '
                     '__attribute__((noinline));\n'
                     'const char *ota_url_redirect_str(ota_redirect_t r) {\n'
                     '    s_calls = 1;\n    return "x";\n}', "assigns to `s_calls`")

    # -- NEGATIVE: the genuine shape must NOT be flagged ---------------------
    def test_a_real_pure_formatter_is_clean(self):
        self.cleanDef('const char *ota_policy_reason_str(ota_reason_t reason) {\n'
                      '    if ((unsigned)reason >= (unsigned)OTA_REASON_COUNT)\n'
                      '        return "";\n'
                      '    const char *s = TABLE[reason];\n'
                      '    return (s != NULL) ? s : "";\n}')

    def test_a_pure_switch_is_clean(self):
        self.cleanDef('const char *ota_url_redirect_str(ota_redirect_t r) {\n'
                      '    switch (r) {\n'
                      '        case OTA_REDIRECT_FOLLOW:\n            return "";\n'
                      '        default:\n            break;\n    }\n    return "";\n}')

    def test_comparisons_are_not_assignments(self):
        self.cleanDef('const char *ota_policy_reason_str(ota_reason_t r) {\n'
                      '    if (r == OTA_REASON_A && r != OTA_REASON_COUNT &&\n'
                      '        (int)r <= 1 && (int)r >= 0)\n        return "a";\n'
                      '    return "";\n}')

    def test_a_prototype_is_not_a_definition(self):
        self.cleanDef('const char *ota_policy_reason_str(ota_reason_t r);\n')

    def test_a_call_site_is_not_a_definition(self):
        self.cleanDef('const char *use(ota_reason_t r) {\n'
                      '    return ota_policy_reason_str(r);\n}')


# ---------------------------------------------------------------------------
class TestDefinitionBackstop(ScannerCase):
    """A body check that silently finds nothing must not read as a pass."""

    def test_the_recorded_defining_file_must_actually_define_it(self):
        rc, out = self.scan("/* the definition was moved away */\n",
                            name=os.path.join("main", "ota_policy.c"),
                            prologue=False)
        self.assertEqual(rc, 1, out)
        self.assertIn("no file-scope definition was found", out)

    def test_an_unrelated_path_is_not_subject_to_the_backstop(self):
        self.assertClean("/* nothing here */\n", name="scratch.c", prologue=False)

    def test_the_header_holding_only_a_prototype_is_clean(self):
        for h in ("ota_policy.h", "ota_url.h", "wake_flow.h"):
            src = os.path.join(_ROOT, "include", h)
            r = subprocess.run([sys.executable, _SCANNER, src],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


# ---------------------------------------------------------------------------
class TestAgainstTheRealTree(ScannerCase):
    """The regression that matters: the shipped code passes its own gate."""

    def test_the_three_allowlisted_definitions_pass_purity(self):
        for rel in ("main/ota_policy.c", "main/ota_url.c", "main/wake_flow.c"):
            src = os.path.join(_ROOT, *rel.split("/"))
            r = subprocess.run([sys.executable, _SCANNER, src],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, rel + ":\n" + r.stdout + r.stderr)

    def test_the_whole_tree_is_clean(self):
        r = subprocess.run([sys.executable, _SCANNER], cwd=_ROOT,
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
