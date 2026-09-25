#include <stdlib.h>
#include <string.h>
#include <unity.h>

/* Single-TU compilation of the pure chore model. Nothing is mocked
   because there is nothing to mock: chores.c touches no clock, no NVS and
   no hardware, which is the property this suite exists to keep true. */
#include "../../main/chores.c"

void setUp(void) {}
void tearDown(void) {}

/* The fixture list, and the shape the real caller stores. */
static const char kList[CHORE_MAX][CHORE_NAME_BUF] = {"Teeth", "Bed", "Dishes"};

/* Ack masks spelled out, so the C-row tests read as situations rather
   than as bit arithmetic. */
#define ACK_NONE 0x00u
#define ACK_0 0x01u
#define ACK_01 0x03u
#define ACK_ALL3 0x07u

/* A day's numbers: one hour allocated, half of it unconditional. */
#define ALLOC_SEC 3600u
#define FREE_SEC 1800u

/* ---- C1: no chores configured -> the feature is entirely inert ------- */

void test_c1_no_chores_is_inert(void) {
    /* The default for every existing device. Nothing is withheld whatever
       the allocation split says, nothing can be acked, and the release
       edge never fires - so no gate is ever armed and no pixel lights. */
    TEST_ASSERT_EQUAL_UINT32(0, chores_withheld_sec(ALLOC_SEC, FREE_SEC, ACK_NONE, 0, false));
    TEST_ASSERT_EQUAL_UINT32(0, chores_withheld_sec(ALLOC_SEC, 0, ACK_NONE, 0, false));
    TEST_ASSERT_EQUAL_UINT8(0, chores_outstanding(ACK_NONE, 0));
    TEST_ASSERT_FALSE(chores_all_acked(ACK_NONE, 0));
    TEST_ASSERT_FALSE(chores_release_due(ACK_NONE, 0, false));
    /* Even a corrupt record claiming acks cannot wake it up. */
    TEST_ASSERT_FALSE(chores_release_due(ACK_ALL3, 0, false));
    TEST_ASSERT_EQUAL_UINT32(0, chores_withheld_sec(ALLOC_SEC, FREE_SEC, ACK_ALL3, 0, false));
}

/* ---- C2: chores configured, none acked, screen IDLE ------------------ */

void test_c2_none_acked_withholds_the_remainder(void) {
    TEST_ASSERT_EQUAL_UINT32(ALLOC_SEC - FREE_SEC, chores_withheld_sec(ALLOC_SEC, FREE_SEC, ACK_NONE, 3, false));
    /* Stated the way the caller uses it: timer_start allocates
       min(allocation, chore_free). That identity holds for every
       unreleased day on which the gate is armed - including, on purpose,
       one where every chore has just been ticked but `released` has not
       been latched yet. The remainder is not handed over by the mask
       changing; it is handed over by the release, once. */
    TEST_ASSERT_EQUAL_UINT32(FREE_SEC, ALLOC_SEC - chores_withheld_sec(ALLOC_SEC, FREE_SEC, ACK_NONE, 3, false));
    /* A partial tick is still a tick short: nothing is granted piecemeal. */
    TEST_ASSERT_EQUAL_UINT32(ALLOC_SEC - FREE_SEC, chores_withheld_sec(ALLOC_SEC, FREE_SEC, ACK_01, 3, false));
    TEST_ASSERT_EQUAL_UINT8(3, chores_outstanding(ACK_NONE, 3));
    TEST_ASSERT_EQUAL_UINT8(1, chores_outstanding(ACK_01, 3));
}

/* ---- C8: a chore toggled back off AFTER release --------------------- */

void test_c8_toggle_off_after_release_stays_released(void) {
    const uint8_t after = chores_toggle_ack(ACK_ALL3, 1, 3); /* un-tick "Bed" */
    TEST_ASSERT_EQUAL_UINT8(0x05u, after);
    TEST_ASSERT_EQUAL_UINT8(1, chores_outstanding(after, 3));
    /* Nothing is clawed back: the release is a latched day flag, not a
       recomputation, so the withheld amount stays zero. */
    TEST_ASSERT_EQUAL_UINT32(0, chores_withheld_sec(ALLOC_SEC, FREE_SEC, after, 3, true));
    /* And nothing is gained either - re-ticking cannot fire a second
       release - so there is nothing to game by toggling. */
    TEST_ASSERT_FALSE(chores_release_due(after, 3, true));
    TEST_ASSERT_FALSE(chores_release_due(ACK_ALL3, 3, true));
}

/* ---- C9: a chore toggled off BEFORE release ------------------------- */

void test_c9_toggle_off_before_release_rearms_gate(void) {
    /* All three ticked but the flag not yet latched: this is the edge. */
    TEST_ASSERT_TRUE(chores_release_due(ACK_ALL3, 3, false));
    const uint8_t after = chores_toggle_ack(ACK_ALL3, 2, 3);
    TEST_ASSERT_EQUAL_UINT8(ACK_01, after);
    /* It was never released, so the gate simply re-arms - the remainder
       is withheld again and the edge is gone until the tick comes back. */
    TEST_ASSERT_FALSE(chores_release_due(after, 3, false));
    TEST_ASSERT_EQUAL_UINT32(ALLOC_SEC - FREE_SEC, chores_withheld_sec(ALLOC_SEC, FREE_SEC, after, 3, false));
    TEST_ASSERT_TRUE(chores_release_due(chores_toggle_ack(after, 2, 3), 3, false));
}

/* ---- C10: chore list edited (hash mismatch) -------------------------- */

void test_c10_list_edit_clears_acks_preserves_release(void) {
    const chore_ack_t stored = {.acked = ACK_01, .released = false};
    const chore_ack_t edited = chores_reconcile(stored, 0x1234, 0x5678);
    TEST_ASSERT_EQUAL_UINT8(0, edited.acked); /* positional bits are meaningless now */
    TEST_ASSERT_FALSE(edited.released);

    /* The half that matters: an edit must not re-lock a day that already
       released (C8 says a released day stays released). */
    const chore_ack_t done = {.acked = ACK_ALL3, .released = true};
    const chore_ack_t after = chores_reconcile(done, 0x1234, 0x5678);
    TEST_ASSERT_EQUAL_UINT8(0, after.acked);
    TEST_ASSERT_TRUE(after.released);
    /* ...and the cleared acks do not resurrect the gate for the rest of
       the day, because the withheld amount keys off `released`. */
    TEST_ASSERT_EQUAL_UINT32(0, chores_withheld_sec(ALLOC_SEC, FREE_SEC, after.acked, 3, after.released));
    TEST_ASSERT_FALSE(chores_release_due(after.acked, 3, after.released));
}

void test_reconcile_keeps_acks_when_the_hash_matches(void) {
    const chore_ack_t stored = {.acked = ACK_01, .released = false};
    const chore_ack_t same = chores_reconcile(stored, 0xBEEF, 0xBEEF);
    TEST_ASSERT_EQUAL_UINT8(ACK_01, same.acked);
    TEST_ASSERT_FALSE(same.released);
    const chore_ack_t released = {.acked = ACK_0, .released = true};
    const chore_ack_t kept = chores_reconcile(released, 0, 0); /* 0 is a legal hash, not "absent" */
    TEST_ASSERT_EQUAL_UINT8(ACK_0, kept.acked);
    TEST_ASSERT_TRUE(kept.released);
}

/* ---- ack-mask ops ---------------------------------------------------- */

void test_index_valid_rejects_out_of_range(void) {
    TEST_ASSERT_TRUE(chores_index_valid(0, 3));
    TEST_ASSERT_TRUE(chores_index_valid(2, 3));
    TEST_ASSERT_FALSE(chores_index_valid(3, 3)); /* one past the end */
    TEST_ASSERT_FALSE(chores_index_valid(2, 2)); /* the spare ack button */
    TEST_ASSERT_FALSE(chores_index_valid(0, 0));
    TEST_ASSERT_FALSE(chores_index_valid(200, 3));
    /* A count beyond the hardware cap cannot widen the valid range. */
    TEST_ASSERT_FALSE(chores_index_valid(CHORE_MAX, 250));
}

void test_ack_bits_are_positional_and_toggle(void) {
    uint8_t mask = ACK_NONE;
    mask = chores_toggle_ack(mask, 1, 3);
    TEST_ASSERT_EQUAL_UINT8(0x02u, mask);
    TEST_ASSERT_TRUE(chores_is_acked(mask, 1, 3));
    TEST_ASSERT_FALSE(chores_is_acked(mask, 0, 3));
    TEST_ASSERT_FALSE(chores_is_acked(mask, 2, 3));
    mask = chores_toggle_ack(mask, 1, 3); /* a mis-press is undone by the same button */
    TEST_ASSERT_EQUAL_UINT8(ACK_NONE, mask);
    TEST_ASSERT_FALSE(chores_is_acked(mask, 1, 3));
}

void test_toggle_out_of_range_is_a_no_op(void) {
    TEST_ASSERT_EQUAL_UINT8(ACK_01, chores_toggle_ack(ACK_01, 2, 2));
    TEST_ASSERT_EQUAL_UINT8(ACK_01, chores_toggle_ack(ACK_01, 3, 3));
    TEST_ASSERT_EQUAL_UINT8(ACK_01, chores_toggle_ack(ACK_01, 0, 0));
    /* An index from a corrupt record must not reach a shift - a shift by
       200 is undefined behaviour, which the suite's UBSan would catch. */
    TEST_ASSERT_EQUAL_UINT8(ACK_01, chores_toggle_ack(ACK_01, 200, 3));
    TEST_ASSERT_FALSE(chores_is_acked(0xFFu, 200, 3));
}

void test_outstanding_ignores_bits_above_the_count(void) {
    /* The list shrank from three chores to two with all three acked: the
       stale bit 2 must not be counted, and must not read as an ack. */
    TEST_ASSERT_EQUAL_UINT8(0, chores_outstanding(ACK_ALL3, 2));
    TEST_ASSERT_FALSE(chores_is_acked(ACK_ALL3, 2, 2));
    TEST_ASSERT_TRUE(chores_all_acked(ACK_ALL3, 2));
    /* And the same in the other direction: a mask with every bit set from
       a corrupt read still only answers for configured chores. */
    TEST_ASSERT_EQUAL_UINT8(0, chores_outstanding(0xFFu, 3));
    TEST_ASSERT_EQUAL_UINT8(2, chores_outstanding(ACK_0, 3));
}

void test_all_acked_requires_a_configured_chore(void) {
    TEST_ASSERT_TRUE(chores_all_acked(ACK_ALL3, 3));
    TEST_ASSERT_TRUE(chores_all_acked(ACK_0, 1));
    TEST_ASSERT_FALSE(chores_all_acked(ACK_01, 3));
    /* The empty list is not "all done": there is nothing to release. */
    TEST_ASSERT_FALSE(chores_all_acked(ACK_NONE, 0));
    TEST_ASSERT_FALSE(chores_all_acked(ACK_ALL3, 0));
}

/* ---- the release edge ------------------------------------------------ */

void test_release_due_only_on_the_all_acked_edge(void) {
    TEST_ASSERT_FALSE(chores_release_due(ACK_NONE, 3, false));
    TEST_ASSERT_FALSE(chores_release_due(ACK_01, 3, false));
    TEST_ASSERT_TRUE(chores_release_due(ACK_ALL3, 3, false)); /* the one true case */
    /* Latching it is what makes the release fire once: the caller sets
       `released` and the same mask stops being an edge. */
    TEST_ASSERT_FALSE(chores_release_due(ACK_ALL3, 3, true));
}

void test_release_due_false_without_chores(void) {
    TEST_ASSERT_FALSE(chores_release_due(ACK_NONE, 0, false));
    TEST_ASSERT_FALSE(chores_release_due(0xFFu, 0, false));
    TEST_ASSERT_FALSE(chores_release_due(0xFFu, 0, true));
}

/* ---- the withheld split ---------------------------------------------- */

void test_withheld_is_zero_only_once_released(void) {
    TEST_ASSERT_EQUAL_UINT32(0, chores_withheld_sec(ALLOC_SEC, FREE_SEC, ACK_NONE, 3, true));
    /* All acked with the flag not yet latched is NOT zero. This is the
       release instant, and the number it returns is the amount the
       release owes - see
       test_release_composes_to_exactly_the_allocation for why answering
       0 here breaks both callers. */
    TEST_ASSERT_EQUAL_UINT32(ALLOC_SEC - FREE_SEC, chores_withheld_sec(ALLOC_SEC, FREE_SEC, ACK_ALL3, 3, false));
    /* The mask is not a term in the formula at all, so stale high bits
       and partial ticks give the same answer as none. */
    TEST_ASSERT_EQUAL_UINT32(ALLOC_SEC - FREE_SEC, chores_withheld_sec(ALLOC_SEC, FREE_SEC, 0x04u, 3, false));
    TEST_ASSERT_EQUAL_UINT32(ALLOC_SEC - FREE_SEC, chores_withheld_sec(ALLOC_SEC, FREE_SEC, 0xFFu, 3, false));
}

void test_release_composes_to_exactly_the_allocation(void) {
    /* What this pins is not a value but a SUM, which is why it exists
       alongside the single-value assertions above. Walk the whole
       sequence - unreleased, last tick lands, release fires, `released`
       latched - and total what the day actually made available. It must
       come to ALLOC_SEC: granted once, never zero, never twice.

       The hazard is specific and was real. A short-circuit returning 0
       from chores_withheld_sec() as soon as every chore is acked passes
       every isolated assertion in this file and still breaks both
       callers: sizing the one-shot release from the withheld amount
       would grant 0 seconds, and using (alloc - withheld) as a live cap
       while also adding the remainder on the edge would grant the
       remainder twice. */

    /* Before the last tick: the free half is available, the rest held. */
    uint8_t mask = ACK_01;
    bool released = false;
    TEST_ASSERT_EQUAL_UINT32(ALLOC_SEC - FREE_SEC, chores_withheld_sec(ALLOC_SEC, FREE_SEC, mask, 3, released));
    TEST_ASSERT_EQUAL_UINT32(FREE_SEC, ALLOC_SEC - chores_withheld_sec(ALLOC_SEC, FREE_SEC, mask, 3, released));

    /* The last tick lands. The edge is armed, and the amount owed is
       readable BEFORE the latch - that ordering is the caller contract
       in chores.h, and this is the read it describes. */
    mask = chores_toggle_ack(mask, 2, 3);
    TEST_ASSERT_EQUAL_UINT8(ACK_ALL3, mask);
    TEST_ASSERT_TRUE(chores_release_due(mask, 3, released));
    const uint32_t release_amount = chores_withheld_sec(ALLOC_SEC, FREE_SEC, mask, 3, released);
    TEST_ASSERT_EQUAL_UINT32(ALLOC_SEC - FREE_SEC, release_amount);

    /* The caller grants that and latches immediately. Shape A totals the
       free half handed over up front plus this release, fired once. */
    released = true;
    TEST_ASSERT_EQUAL_UINT32(ALLOC_SEC, FREE_SEC + release_amount);

    /* Shape B carries (alloc - withheld) as a live cap. After the latch
       that cap is the FULL allocation, so the remainder must not also be
       added again: the edge is gone and nothing is withheld. */
    TEST_ASSERT_EQUAL_UINT32(ALLOC_SEC, ALLOC_SEC - chores_withheld_sec(ALLOC_SEC, FREE_SEC, mask, 3, released));
    TEST_ASSERT_FALSE(chores_release_due(mask, 3, released));
    TEST_ASSERT_EQUAL_UINT32(0, chores_withheld_sec(ALLOC_SEC, FREE_SEC, mask, 3, released));

    /* And a second wake on the same latched day adds nothing under
       either shape - the one-shot stays one-shot. */
    TEST_ASSERT_EQUAL_UINT32(0, chores_withheld_sec(ALLOC_SEC, FREE_SEC, mask, 3, released));
    TEST_ASSERT_FALSE(chores_release_due(mask, 3, released));
}

void test_free_equals_allocation_withholds_nothing(void) {
    /* Design 3.3: chore_free == allocation means the gate is simply off
       for that day type - there is no remainder to withhold, so no gate
       is armed even though chores are configured and outstanding. */
    TEST_ASSERT_EQUAL_UINT32(0, chores_withheld_sec(ALLOC_SEC, ALLOC_SEC, ACK_NONE, 3, false));
    TEST_ASSERT_EQUAL_UINT32(0, chores_withheld_sec(0, 0, ACK_NONE, 3, false));
}

void test_free_one_second_under_allocation_withholds_one(void) {
    /* The neighbouring case, one character apart in the config. */
    TEST_ASSERT_EQUAL_UINT32(1, chores_withheld_sec(ALLOC_SEC, ALLOC_SEC - 1, ACK_NONE, 3, false));
    TEST_ASSERT_EQUAL_UINT32(ALLOC_SEC, chores_withheld_sec(ALLOC_SEC, 0, ACK_NONE, 3, false));
}

void test_free_over_allocation_saturates_to_zero(void) {
    /* A config error (caught where the config is validated), but this
       function must not turn it into a ~136-year withholding by wrapping. */
    TEST_ASSERT_EQUAL_UINT32(0, chores_withheld_sec(ALLOC_SEC, ALLOC_SEC + 1, ACK_NONE, 3, false));
    TEST_ASSERT_EQUAL_UINT32(0, chores_withheld_sec(0, ALLOC_SEC, ACK_NONE, 3, false));
    TEST_ASSERT_EQUAL_UINT32(0, chores_withheld_sec(1, UINT32_MAX, ACK_NONE, 3, false));
}

/* ---- the list hash --------------------------------------------------- */

/* The pinned values below come from an independent reference
   implementation of the documented algorithm (FNV-1a 32-bit over the
   count byte then each name's bytes plus a NUL separator, xor-folded to
   16 bits), not from this module's output. They are here because the
   value is PERSISTED: changing the algorithm silently invalidates every
   stored hash in the field, which would clear one day's acks on every
   device at once. If a change is deliberate, these constants are where
   it is acknowledged. */
void test_hash_matches_reference_fnv1a(void) {
    TEST_ASSERT_EQUAL_HEX16(0x984B, chores_list_hash(kList, 3));
    TEST_ASSERT_EQUAL_HEX16(0x5D70, chores_list_hash(kList, 2));
    TEST_ASSERT_EQUAL_HEX16(0x9090, chores_list_hash(kList, 1));
    TEST_ASSERT_EQUAL_HEX16(0x5813, chores_list_hash(kList, 0));
}

void test_hash_is_order_sensitive(void) {
    /* The property a sum-style checksum cannot give: swapping two names
       changes nothing about the bytes present, only about where they are. */
    static const char swapped[CHORE_MAX][CHORE_NAME_BUF] = {"Bed", "Teeth", "Dishes"};
    TEST_ASSERT_EQUAL_HEX16(0xA0AA, chores_list_hash(swapped, 3));
    TEST_ASSERT_NOT_EQUAL_HEX16(chores_list_hash(kList, 3), chores_list_hash(swapped, 3));
    /* Nor can a checksum separate these: the same bytes, split
       differently. The NUL between names is what does it. */
    static const char ab_c[CHORE_MAX][CHORE_NAME_BUF] = {"ab", "c", ""};
    static const char a_bc[CHORE_MAX][CHORE_NAME_BUF] = {"a", "bc", ""};
    TEST_ASSERT_NOT_EQUAL_HEX16(chores_list_hash(ab_c, 2), chores_list_hash(a_bc, 2));
}

void test_hash_changes_with_any_name_edit(void) {
    static const char typo[CHORE_MAX][CHORE_NAME_BUF] = {"Teeth", "Bed", "Dishos"};
    TEST_ASSERT_EQUAL_HEX16(0x508A, chores_list_hash(typo, 3));
    TEST_ASSERT_NOT_EQUAL_HEX16(chores_list_hash(kList, 3), chores_list_hash(typo, 3));
    /* A name filling the whole 20-character budget, differing only in the
       last usable character: the hash must read all of it. */
    static const char long_a[CHORE_MAX][CHORE_NAME_BUF] = {"Practise the piano a"};
    static const char long_b[CHORE_MAX][CHORE_NAME_BUF] = {"Practise the piano b"};
    TEST_ASSERT_EQUAL_size_t(CHORE_NAME_MAX, strlen(long_a[0]));
    TEST_ASSERT_NOT_EQUAL_HEX16(chores_list_hash(long_a, 1), chores_list_hash(long_b, 1));
}

void test_hash_changes_with_the_count(void) {
    TEST_ASSERT_NOT_EQUAL_HEX16(chores_list_hash(kList, 3), chores_list_hash(kList, 2));
    TEST_ASSERT_NOT_EQUAL_HEX16(chores_list_hash(kList, 1), chores_list_hash(kList, 0));
    /* An empty list and a list holding one empty name are different
       configurations and must hash differently - the count is in the
       stream, not just implied by the bytes. */
    static const char one_empty[CHORE_MAX][CHORE_NAME_BUF] = {""};
    TEST_ASSERT_EQUAL_HEX16(0xF610, chores_list_hash(one_empty, 1));
    TEST_ASSERT_NOT_EQUAL_HEX16(chores_list_hash(one_empty, 0), chores_list_hash(one_empty, 1));
}

void test_hash_clamps_a_count_above_chore_max(void) {
    /* Only CHORE_MAX rows exist to read. A larger count comes from a
       corrupt record and is clamped rather than followed off the end of
       the array (ASan would catch the alternative). */
    TEST_ASSERT_EQUAL_HEX16(chores_list_hash(kList, CHORE_MAX), chores_list_hash(kList, 200));
}

void test_hash_ignores_bytes_past_the_terminator(void) {
    /* The caller's buffers are fixed-size and need not be zero-filled
       past the NUL - a name shortened in place leaves the old tail
       behind. Two lists with the same names must hash the same
       regardless, or an edit would be reported where none happened. */
    char clean[CHORE_MAX][CHORE_NAME_BUF];
    char littered[CHORE_MAX][CHORE_NAME_BUF];
    memset(clean, 0x00, sizeof(clean));
    memset(littered, 0xAA, sizeof(littered));
    for (uint8_t i = 0; i < CHORE_MAX; i++) {
        strcpy(clean[i], kList[i]);
        strcpy(littered[i], kList[i]); /* writes the NUL; the 0xAA tail survives */
    }
    TEST_ASSERT_EQUAL_HEX16(chores_list_hash(clean, 3), chores_list_hash(littered, 3));
    /* Also the shape the firmware actually stores: a mutable buffer,
       passed to a const parameter. */
    TEST_ASSERT_EQUAL_HEX16(0x984B, chores_list_hash(clean, 3));
}

void test_hash_stops_at_the_cap_on_an_unterminated_row(void) {
    /* The module's one memory-safety claim, and the only test that can
       reach it: a row with NO terminator anywhere. The littered-tail
       test above cannot - it writes a NUL and then dirties what follows,
       which strlen stops at just as strnlen does, so strlen passes it.
       Here every byte of the row is 'W'.

       Heap, not a stack array, deliberately: ASan puts a redzone around
       a malloc'd block, so reading past it is a hard failure instead of
       a quiet read of whatever the neighbouring stack slot holds. The
       block is exactly CHORE_NAME_BUF bytes, so byte 22 is already
       outside it and strlen would walk straight into the redzone. */
    char *row = malloc(CHORE_NAME_BUF);
    TEST_ASSERT_NOT_NULL(row);
    memset(row, 'W', CHORE_NAME_BUF);
    const char(*rows)[CHORE_NAME_BUF] = (const char(*)[CHORE_NAME_BUF])row;

    const uint16_t h = chores_list_hash(rows, 1);

    /* Pinned, not merely "it did not crash", because the CAP is a
       contract too and ASan cannot see it: capping at CHORE_NAME_BUF
       instead of CHORE_NAME_MAX reads one byte more, stays inside the
       allocation, and would slip past a crash-only test while changing
       every stored hash in the field. Reference value, from the
       documented algorithm over count=1, twenty 'W' bytes and the
       separator - the same derivation as the goldens above, not a value
       read back from this module. */
    TEST_ASSERT_EQUAL_HEX16(0x6A1E, h);

    /* The cap sits exactly where the terminator would be, which is the
       claim that makes it the right cap: a properly terminated 20-byte
       name hashes identically, because the 21st byte is outside the
       value either way. */
    static const char terminated[CHORE_MAX][CHORE_NAME_BUF] = {"WWWWWWWWWWWWWWWWWWWW"};
    TEST_ASSERT_EQUAL_size_t(CHORE_NAME_MAX, strlen(terminated[0]));
    TEST_ASSERT_EQUAL_HEX16(h, chores_list_hash(terminated, 1));

    free(row);
}

void test_hash_reads_bytes_above_0x7f_unchanged(void) {
    /* Chore names arrive from an MQTT config payload as UTF-8, so a
       non-ASCII name is bytes above 0x7F, and the hash is a PERSISTED
       field compared across firmware versions. Anything that changes how
       those bytes are mixed in - a mask, a sign extension, a signed char
       reaching a wider parameter - clears one day's acks on every device
       carrying such a name, silently and fleet-wide. No other fixture in
       this file contains a byte above 0x7F, so nothing else here can
       see it happen. */
    static const char umlaut[CHORE_MAX][CHORE_NAME_BUF] = {"Räum dein Zimmer"};
    /* 16 characters in 17 bytes: the 'ä' is C3 A4. That inequality is
       why CHORE_NAME_MAX is documented as a BYTE budget. */
    TEST_ASSERT_EQUAL_size_t(17, strlen(umlaut[0]));
    TEST_ASSERT_EQUAL_HEX8(0xC3u, (uint8_t)umlaut[0][1]);
    TEST_ASSERT_EQUAL_HEX8(0xA4u, (uint8_t)umlaut[0][2]);
    /* Reference value over those 17 bytes, derived from the documented
       algorithm rather than read back from the module. Masking the high
       bit off each byte would give 0x5DB1 instead. */
    TEST_ASSERT_EQUAL_HEX16(0x5AFE, chores_list_hash(umlaut, 1));
    /* The ASCII spelling of the same name is a different list, which is
       exactly the edit the hash exists to notice. */
    static const char ascii[CHORE_MAX][CHORE_NAME_BUF] = {"Raum dein Zimmer"};
    TEST_ASSERT_EQUAL_HEX16(0xFF83, chores_list_hash(ascii, 1));
    TEST_ASSERT_NOT_EQUAL_HEX16(chores_list_hash(umlaut, 1), chores_list_hash(ascii, 1));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_c1_no_chores_is_inert);
    RUN_TEST(test_c2_none_acked_withholds_the_remainder);
    RUN_TEST(test_c8_toggle_off_after_release_stays_released);
    RUN_TEST(test_c9_toggle_off_before_release_rearms_gate);
    RUN_TEST(test_c10_list_edit_clears_acks_preserves_release);
    RUN_TEST(test_reconcile_keeps_acks_when_the_hash_matches);
    RUN_TEST(test_index_valid_rejects_out_of_range);
    RUN_TEST(test_ack_bits_are_positional_and_toggle);
    RUN_TEST(test_toggle_out_of_range_is_a_no_op);
    RUN_TEST(test_outstanding_ignores_bits_above_the_count);
    RUN_TEST(test_all_acked_requires_a_configured_chore);
    RUN_TEST(test_release_due_only_on_the_all_acked_edge);
    RUN_TEST(test_release_due_false_without_chores);
    RUN_TEST(test_withheld_is_zero_only_once_released);
    RUN_TEST(test_release_composes_to_exactly_the_allocation);
    RUN_TEST(test_free_equals_allocation_withholds_nothing);
    RUN_TEST(test_free_one_second_under_allocation_withholds_one);
    RUN_TEST(test_free_over_allocation_saturates_to_zero);
    RUN_TEST(test_hash_matches_reference_fnv1a);
    RUN_TEST(test_hash_is_order_sensitive);
    RUN_TEST(test_hash_changes_with_any_name_edit);
    RUN_TEST(test_hash_changes_with_the_count);
    RUN_TEST(test_hash_clamps_a_count_above_chore_max);
    RUN_TEST(test_hash_ignores_bytes_past_the_terminator);
    RUN_TEST(test_hash_stops_at_the_cap_on_an_unterminated_row);
    RUN_TEST(test_hash_reads_bytes_above_0x7f_unchanged);
    return UNITY_END();
}
