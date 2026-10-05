#include <string.h>
#include <unity.h>

#include "../../main/dns_reply.c"

void setUp(void) {}
void tearDown(void) {}

static const uint8_t IP[4] = {192, 168, 4, 1};

/* A standard recursive query for `name` (already label-encoded by the
   caller), one question, no EDNS. Returns its length. */
static size_t make_query(uint8_t *buf, const uint8_t *labels, size_t labels_len, uint16_t qtype, uint16_t qclass) {
    memset(buf, 0, 12);
    buf[0] = 0x12;
    buf[1] = 0x34;
    buf[2] = 0x01; /* RD */
    buf[5] = 1;    /* QDCOUNT */
    memcpy(buf + 12, labels, labels_len);
    size_t o = 12 + labels_len;
    buf[o++] = (uint8_t)(qtype >> 8);
    buf[o++] = (uint8_t)qtype;
    buf[o++] = (uint8_t)(qclass >> 8);
    buf[o++] = (uint8_t)qclass;
    return o;
}

/* "connectivitycheck.gstatic.com" */
static const uint8_t NAME_GSTATIC[] = {17,  'c', 'o', 'n', 'n', 'e', 'c', 't', 'i', 'v', 'i', 't', 'y', 'c', 'h', 'e',
                                       'c', 'k', 7,   'g', 's', 't', 'a', 't', 'i', 'c', 3,   'c', 'o', 'm', 0};

void test_an_a_query_is_answered_with_the_given_address(void) {
    uint8_t q[128], r[DNS_REPLY_MAX];
    size_t qn = make_query(q, NAME_GSTATIC, sizeof(NAME_GSTATIC), 1, 1);
    size_t rn = dns_reply_build(q, qn, IP, r, sizeof(r));

    TEST_ASSERT_EQUAL_UINT(qn + 16, rn); /* question echoed, plus one 16-byte A record */
    TEST_ASSERT_EQUAL_UINT8(0x12, r[0]); /* id echoed */
    TEST_ASSERT_EQUAL_UINT8(0x34, r[1]);
    TEST_ASSERT_TRUE_MESSAGE(r[2] & 0x80, "QR must be set");
    TEST_ASSERT_TRUE_MESSAGE(r[2] & 0x01, "RD must be echoed");
    TEST_ASSERT_TRUE_MESSAGE(r[3] & 0x80, "RA must be set");
    TEST_ASSERT_EQUAL_UINT8(0, r[3] & 0x0F); /* NOERROR */
    TEST_ASSERT_EQUAL_UINT8(1, r[5]);        /* QDCOUNT */
    TEST_ASSERT_EQUAL_UINT8(1, r[7]);        /* ANCOUNT */
    TEST_ASSERT_EQUAL_MEMORY(q + 12, r + 12, qn - 12);
    const uint8_t expect_rr[16] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1};
    TEST_ASSERT_EQUAL_MEMORY(expect_rr, r + qn, 16);
}

void test_a_non_a_query_gets_an_empty_noerror_answer(void) {
    uint8_t q[128], r[DNS_REPLY_MAX];
    /* AAAA (28) and HTTPS (65): a phone asks for both beside every A. */
    uint16_t types[] = {28, 65, 15 /* MX */};
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        size_t qn = make_query(q, NAME_GSTATIC, sizeof(NAME_GSTATIC), types[i], 1);
        size_t rn = dns_reply_build(q, qn, IP, r, sizeof(r));
        TEST_ASSERT_EQUAL_UINT(qn, rn);
        TEST_ASSERT_TRUE(r[2] & 0x80);
        TEST_ASSERT_EQUAL_UINT8(0, r[3] & 0x0F);
        TEST_ASSERT_EQUAL_UINT8(0, r[6]);
        TEST_ASSERT_EQUAL_UINT8(0, r[7]); /* no answer */
    }
}

void test_a_query_in_a_non_internet_class_gets_no_address(void) {
    uint8_t q[128], r[DNS_REPLY_MAX];
    size_t qn = make_query(q, NAME_GSTATIC, sizeof(NAME_GSTATIC), 1, 3 /* CH */);
    size_t rn = dns_reply_build(q, qn, IP, r, sizeof(r));
    TEST_ASSERT_EQUAL_UINT(qn, rn);
    TEST_ASSERT_EQUAL_UINT8(0, r[7]);
}

void test_a_short_or_empty_packet_is_dropped(void) {
    uint8_t q[128], r[DNS_REPLY_MAX];
    size_t qn = make_query(q, NAME_GSTATIC, sizeof(NAME_GSTATIC), 1, 1);
    TEST_ASSERT_EQUAL_UINT(0, dns_reply_build(q, 0, IP, r, sizeof(r)));
    TEST_ASSERT_EQUAL_UINT(0, dns_reply_build(q, 11, IP, r, sizeof(r)));     /* shorter than a header */
    TEST_ASSERT_EQUAL_UINT(0, dns_reply_build(q, 12, IP, r, sizeof(r)));     /* header only, no question */
    TEST_ASSERT_EQUAL_UINT(0, dns_reply_build(q, qn - 1, IP, r, sizeof(r))); /* question cut inside QCLASS */
    TEST_ASSERT_EQUAL_UINT(0, dns_reply_build(q, 20, IP, r, sizeof(r)));     /* cut inside the name */
    TEST_ASSERT_EQUAL_UINT(0, dns_reply_build(NULL, qn, IP, r, sizeof(r)));
}

void test_a_name_that_never_terminates_is_dropped(void) {
    uint8_t q[64], r[DNS_REPLY_MAX];
    memset(q, 0, sizeof(q));
    q[5] = 1;
    q[12] = 63; /* a label that claims to run past the end of the packet */
    memset(q + 13, 'a', 40);
    TEST_ASSERT_EQUAL_UINT(0, dns_reply_build(q, 53, IP, r, sizeof(r)));
}

void test_an_oversize_label_or_name_is_dropped(void) {
    uint8_t labels[400], q[512], r[DNS_REPLY_MAX];

    /* a 64-byte label: the length byte's top two bits are the pointer flag, so this is not a label at all */
    size_t n = 0;
    labels[n++] = 64;
    memset(labels + n, 'a', 64);
    n += 64;
    labels[n++] = 0;
    size_t qn = make_query(q, labels, n, 1, 1);
    TEST_ASSERT_EQUAL_UINT(0, dns_reply_build(q, qn, IP, r, sizeof(r)));

    /* five 63-byte labels: a 321-byte name, over the 255-byte limit, though each label is legal */
    n = 0;
    for (int i = 0; i < 5; i++) {
        labels[n++] = 63;
        memset(labels + n, 'a', 63);
        n += 63;
    }
    labels[n++] = 0;
    qn = make_query(q, labels, n, 1, 1);
    TEST_ASSERT_EQUAL_UINT(0, dns_reply_build(q, qn, IP, r, sizeof(r)));

    /* a name exactly at the limit (255 bytes on the wire) is still answered */
    n = 0;
    for (int i = 0; i < 3; i++) {
        labels[n++] = 63;
        memset(labels + n, 'a', 63);
        n += 63;
    }
    labels[n++] = 61;
    memset(labels + n, 'b', 61);
    n += 61;
    labels[n++] = 0;
    TEST_ASSERT_EQUAL_UINT(255, n);
    qn = make_query(q, labels, n, 1, 1);
    size_t rn = dns_reply_build(q, qn, IP, r, sizeof(r));
    TEST_ASSERT_EQUAL_UINT(qn + 16, rn);
    TEST_ASSERT_TRUE(rn <= DNS_REPLY_MAX);
}

void test_a_reply_that_would_not_fit_the_buffer_is_dropped(void) {
    uint8_t q[128], r[DNS_REPLY_MAX];
    size_t qn = make_query(q, NAME_GSTATIC, sizeof(NAME_GSTATIC), 1, 1);
    TEST_ASSERT_EQUAL_UINT(0, dns_reply_build(q, qn, IP, r, qn + 15));
    TEST_ASSERT_EQUAL_UINT(qn + 16, dns_reply_build(q, qn, IP, r, qn + 16));
    TEST_ASSERT_EQUAL_UINT(0, dns_reply_build(q, qn, IP, NULL, sizeof(r)));
}

void test_a_response_or_a_non_query_opcode_is_not_answered(void) {
    uint8_t q[128], r[DNS_REPLY_MAX];
    size_t qn = make_query(q, NAME_GSTATIC, sizeof(NAME_GSTATIC), 1, 1);

    q[2] |= 0x80; /* QR: someone else's response; answering it would start a reflection loop */
    TEST_ASSERT_EQUAL_UINT(0, dns_reply_build(q, qn, IP, r, sizeof(r)));
    q[2] = 0x01 | (2 << 3); /* opcode STATUS */
    TEST_ASSERT_EQUAL_UINT(0, dns_reply_build(q, qn, IP, r, sizeof(r)));
}

void test_only_a_single_question_is_answered(void) {
    uint8_t q[128], r[DNS_REPLY_MAX];
    size_t qn = make_query(q, NAME_GSTATIC, sizeof(NAME_GSTATIC), 1, 1);
    q[5] = 2;
    TEST_ASSERT_EQUAL_UINT(0, dns_reply_build(q, qn, IP, r, sizeof(r)));
    q[5] = 0;
    TEST_ASSERT_EQUAL_UINT(0, dns_reply_build(q, qn, IP, r, sizeof(r)));
}

void test_a_compression_pointer_in_the_question_is_dropped(void) {
    uint8_t q[64], r[DNS_REPLY_MAX];
    memset(q, 0, sizeof(q));
    q[5] = 1;
    q[12] = 0xC0; /* a question's name has nothing earlier to point at */
    q[13] = 0x0C;
    q[14] = 0;
    q[15] = 1;
    q[16] = 0;
    q[17] = 1;
    TEST_ASSERT_EQUAL_UINT(0, dns_reply_build(q, 18, IP, r, sizeof(r)));
}

void test_an_edns_additional_record_is_not_echoed(void) {
    uint8_t q[160], r[DNS_REPLY_MAX];
    size_t qn = make_query(q, NAME_GSTATIC, sizeof(NAME_GSTATIC), 1, 1);
    q[11] = 1; /* ARCOUNT, followed by an OPT record */
    const uint8_t opt[] = {0, 0, 41, 0x10, 0, 0, 0, 0, 0, 0, 0};
    memcpy(q + qn, opt, sizeof(opt));
    size_t rn = dns_reply_build(q, qn + sizeof(opt), IP, r, sizeof(r));
    TEST_ASSERT_EQUAL_UINT(qn + 16, rn);
    TEST_ASSERT_EQUAL_UINT8(0, r[10]); /* ARCOUNT zeroed: the OPT record is not in the reply */
    TEST_ASSERT_EQUAL_UINT8(0, r[11]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_an_a_query_is_answered_with_the_given_address);
    RUN_TEST(test_a_non_a_query_gets_an_empty_noerror_answer);
    RUN_TEST(test_a_query_in_a_non_internet_class_gets_no_address);
    RUN_TEST(test_a_short_or_empty_packet_is_dropped);
    RUN_TEST(test_a_name_that_never_terminates_is_dropped);
    RUN_TEST(test_an_oversize_label_or_name_is_dropped);
    RUN_TEST(test_a_reply_that_would_not_fit_the_buffer_is_dropped);
    RUN_TEST(test_a_response_or_a_non_query_opcode_is_not_answered);
    RUN_TEST(test_only_a_single_question_is_answered);
    RUN_TEST(test_a_compression_pointer_in_the_question_is_dropped);
    RUN_TEST(test_an_edns_additional_record_is_not_echoed);
    return UNITY_END();
}
