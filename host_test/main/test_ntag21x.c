#include <string.h>

#include "unity.h"

#include "ndef.h"
#include "ntag21x.h"
#include "sim_ntag.h"
#include "test_util.h"

static sim_ntag_t s_sim;
static ntag_t s_tag;
static uint8_t s_msg[900];
static size_t s_msg_len;
static int s_writing_calls;

static const uint8_t PWD[4] = { 0xA1, 0xB2, 0xC3, 0xD4 };
static const uint8_t PACK[2] = { 0x12, 0x34 };
static const uint8_t OTHER_PWD[4] = { 0x01, 0x02, 0x03, 0x04 };

static void on_writing(void *ctx)
{
    (void)ctx;
    s_writing_calls++;
}

static void fresh(ntag_type_t type)
{
    sim_ntag_init(&s_sim, type);
    s_writing_calls = 0;
    s_msg_len = make_inventree_ndef(s_msg, sizeof(s_msg), "inventree.example/web/stock/location/42", "INV-SL42");
    TEST_ASSERT_EQUAL(NTAG_OK, ntag_identify(&s_tag, sim_ntag_xcvr(&s_sim)));
}

static ntag_program_t job(void)
{
    ntag_program_t p = {
        .ndef = s_msg,
        .ndef_len = s_msg_len,
        .auth0 = NTAG_AUTH0_DEFAULT,
        .on_writing = on_writing,
    };
    return p;
}

static ntag_program_t job_with_pwd(const uint8_t pwd[4])
{
    ntag_program_t p = job();
    p.have_pwd = true;
    memcpy(p.pwd, pwd, 4);
    memcpy(p.pack, PACK, 2);
    return p;
}

/* The tag's memory holds exactly the message the job carried. */
static void assert_tag_holds(const uint8_t *msg, size_t len)
{
    const uint8_t *got = NULL;
    size_t got_len = 0;
    TEST_ASSERT_TRUE(sim_ntag_get_ndef(&s_sim, &got, &got_len));
    TEST_ASSERT_EQUAL(len, got_len);
    if (len) {
        TEST_ASSERT_EQUAL_MEMORY(msg, got, len);
    }
}

static void test_identify_each_type(void)
{
    const struct {
        ntag_type_t type;
        uint8_t cfg_page;
        uint16_t user_bytes;
    } cases[] = {
        { NTAG_213, 0x29, 144 }, { NTAG_215, 0x83, 504 }, { NTAG_216, 0xE3, 888 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        sim_ntag_init(&s_sim, cases[i].type);
        TEST_ASSERT_EQUAL(NTAG_OK, ntag_identify(&s_tag, sim_ntag_xcvr(&s_sim)));
        TEST_ASSERT_EQUAL(cases[i].type, s_tag.type);
        TEST_ASSERT_EQUAL_HEX8(cases[i].cfg_page, s_tag.cfg_page);
        TEST_ASSERT_EQUAL(cases[i].user_bytes, s_tag.user_bytes);
    }
}

static void test_identify_rejects_other_tags_and_absent_ones(void)
{
    sim_ntag_init(&s_sim, NTAG_215);
    s_sim.supports_version = false;
    TEST_ASSERT_EQUAL(NTAG_ERR_WRONG_TYPE, ntag_identify(&s_tag, sim_ntag_xcvr(&s_sim)));

    sim_ntag_init(&s_sim, NTAG_215);
    s_sim.present = false;
    TEST_ASSERT_EQUAL(NTAG_ERR_REMOVED, ntag_identify(&s_tag, sim_ntag_xcvr(&s_sim)));

    sim_ntag_init(&s_sim, NTAG_215);
    s_sim.reader_broken = true;
    TEST_ASSERT_EQUAL(NTAG_ERR_IO, ntag_identify(&s_tag, sim_ntag_xcvr(&s_sim)));
}

static void test_inspect_factory_tags(void)
{
    const ntag_type_t types[] = { NTAG_213, NTAG_215, NTAG_216 };
    const uint16_t capacity[] = { 144, 496, 872 };
    for (size_t i = 0; i < 3; i++) {
        fresh(types[i]);
        ntag_state_t st;
        const uint8_t *msg = NULL;
        TEST_ASSERT_EQUAL(NTAG_OK, ntag_inspect(&s_tag, &st, &msg));
        TEST_ASSERT_TRUE(st.formatted);
        TEST_ASSERT_TRUE(st.readable);
        TEST_ASSERT_FALSE(st.is_protected);
        TEST_ASSERT_FALSE(st.locked);
        TEST_ASSERT_EQUAL(capacity[i], st.capacity);
        TEST_ASSERT_TRUE(st.has_ndef);          /* past the lock control TLV on 213 and 216 */
        TEST_ASSERT_EQUAL(0, st.ndef_len);
        TEST_ASSERT_EQUAL_HEX8(0xFF, st.auth0);
        TEST_ASSERT_EQUAL(0, s_sim.writes);
    }
}

static void test_program_blank_tag(void)
{
    const ntag_type_t types[] = { NTAG_213, NTAG_215, NTAG_216 };
    for (size_t i = 0; i < 3; i++) {
        fresh(types[i]);
        ntag_program_t p = job();
        ntag_state_t st;
        TEST_ASSERT_EQUAL(NTAG_OK, ntag_program(&s_tag, &p, &st, NULL));
        TEST_ASSERT_EQUAL(1, s_writing_calls);
        TEST_ASSERT_FALSE(st.is_protected);
        assert_tag_holds(s_msg, s_msg_len);
        TEST_ASSERT_EQUAL_HEX8(0x03, sim_ntag_page(&s_sim, 4)[0]);  /* always from the first data byte */

        /* and it reads back through the same path lookup uses */
        const uint8_t *msg = NULL;
        TEST_ASSERT_EQUAL(NTAG_OK, ntag_inspect(&s_tag, &st, &msg));
        TEST_ASSERT_EQUAL(s_msg_len, st.ndef_len);
        TEST_ASSERT_EQUAL_MEMORY(s_msg, msg, s_msg_len);
        ndef_info_t info;
        TEST_ASSERT_TRUE(ndef_parse_message(msg, st.ndef_len, &info));
        TEST_ASSERT_EQUAL_STRING("INV-SL42", info.text);
    }
}

static void test_program_refuses_to_overwrite_unless_told(void)
{
    fresh(NTAG_215);
    uint8_t old[64];
    const size_t old_len = make_inventree_ndef(old, sizeof(old), "old/1", "INV-SL1");
    sim_ntag_set_ndef(&s_sim, old, old_len);

    ntag_program_t p = job();
    ntag_state_t st;
    const uint8_t *existing = NULL;
    TEST_ASSERT_EQUAL(NTAG_ERR_NOT_BLANK, ntag_program(&s_tag, &p, &st, &existing));
    TEST_ASSERT_EQUAL(0, s_sim.writes);
    TEST_ASSERT_EQUAL(0, s_writing_calls);
    TEST_ASSERT_NOT_NULL(existing);
    TEST_ASSERT_EQUAL(old_len, st.ndef_len);
    TEST_ASSERT_EQUAL_MEMORY(old, existing, old_len);

    p.overwrite = true;
    TEST_ASSERT_EQUAL(NTAG_OK, ntag_program(&s_tag, &p, &st, &existing));
    assert_tag_holds(s_msg, s_msg_len);
}

static void test_program_accepts_a_tag_a_phone_erased(void)
{
    fresh(NTAG_215);
    const uint8_t erased[] = { 0xD0, 0x00, 0x00 };
    sim_ntag_set_ndef(&s_sim, erased, sizeof(erased));

    ntag_program_t p = job();
    TEST_ASSERT_EQUAL(NTAG_OK, ntag_program(&s_tag, &p, NULL, NULL));
    assert_tag_holds(s_msg, s_msg_len);
}

static void test_program_size_limits(void)
{
    /* NTAG215 declares 496 bytes; a message of 491 takes all of it with the long TLV form. */
    fresh(NTAG_215);
    s_msg_len = make_text_ndef(s_msg, sizeof(s_msg), 491 - 10);
    TEST_ASSERT_EQUAL(491, s_msg_len);
    ntag_program_t p = job();
    TEST_ASSERT_EQUAL(NTAG_OK, ntag_program(&s_tag, &p, NULL, NULL));
    assert_tag_holds(s_msg, s_msg_len);
    TEST_ASSERT_EQUAL_HEX8(0xFF, sim_ntag_page(&s_sim, 4)[1]);

    fresh(NTAG_215);
    s_msg_len = make_text_ndef(s_msg, sizeof(s_msg), 492 - 10);
    p = job();
    TEST_ASSERT_EQUAL(NTAG_ERR_TOO_LARGE, ntag_program(&s_tag, &p, NULL, NULL));
    TEST_ASSERT_EQUAL(0, s_sim.writes);
    TEST_ASSERT_EQUAL(0, s_writing_calls);

    /* 254 is the longest message the one-byte length can describe. */
    fresh(NTAG_215);
    s_msg_len = make_text_ndef(s_msg, sizeof(s_msg), 254 - 7);
    TEST_ASSERT_EQUAL(254, s_msg_len);
    p = job();
    TEST_ASSERT_EQUAL(NTAG_OK, ntag_program(&s_tag, &p, NULL, NULL));
    assert_tag_holds(s_msg, s_msg_len);
}

/* Pull the tag away before every possible write in turn. Whatever is left must read as either
 * an empty message or the complete new one: never a broken tag, never a mix. */
static void test_torn_write_never_leaves_a_broken_tag(void)
{
    const size_t lengths[] = { 60, 300 };
    for (size_t l = 0; l < 2; l++) {
        fresh(NTAG_215);
        s_msg_len = make_text_ndef(s_msg, sizeof(s_msg), lengths[l]);
        ntag_program_t p = job();
        TEST_ASSERT_EQUAL(NTAG_OK, ntag_program(&s_tag, &p, NULL, NULL));
        const unsigned total_writes = s_sim.writes;
        TEST_ASSERT_TRUE(total_writes > 3);

        for (unsigned k = 0; k < total_writes; k++) {
            fresh(NTAG_215);
            s_msg_len = make_text_ndef(s_msg, sizeof(s_msg), lengths[l]);
            uint8_t old[64];
            const size_t old_len = make_inventree_ndef(old, sizeof(old), "old/1", "INV-SL1");
            sim_ntag_set_ndef(&s_sim, old, old_len);

            p = job();
            p.overwrite = true;
            s_sim.tear_after_writes = (int)k;
            TEST_ASSERT_EQUAL(NTAG_ERR_REMOVED, ntag_program(&s_tag, &p, NULL, NULL));
            TEST_ASSERT_EQUAL(k, s_sim.writes);

            const uint8_t *got = NULL;
            size_t got_len = 0;
            TEST_ASSERT_TRUE_MESSAGE(sim_ntag_get_ndef(&s_sim, &got, &got_len), "tag left without a readable TLV");
            if (k == 0) {
                TEST_ASSERT_EQUAL(old_len, got_len);    /* nothing was written at all */
            } else {
                TEST_ASSERT_EQUAL_MESSAGE(0, got_len, "a partial write must read as empty");
            }

            /* Presented again, the same job completes. */
            sim_ntag_present(&s_sim);
            TEST_ASSERT_EQUAL(NTAG_OK, ntag_identify(&s_tag, sim_ntag_xcvr(&s_sim)));
            TEST_ASSERT_EQUAL(NTAG_OK, ntag_program(&s_tag, &p, NULL, NULL));
            assert_tag_holds(s_msg, s_msg_len);
        }
    }
}

static void test_locked_tags_are_left_alone(void)
{
    fresh(NTAG_215);
    sim_ntag_page(&s_sim, 2)[3] = 0x01;         /* static lock bit for page 8 */
    ntag_program_t p = job();
    TEST_ASSERT_EQUAL(NTAG_ERR_LOCKED, ntag_program(&s_tag, &p, NULL, NULL));
    TEST_ASSERT_EQUAL(0, s_sim.writes);

    fresh(NTAG_215);
    sim_ntag_page(&s_sim, 0x82)[0] = 0x01;      /* a dynamic lock bit */
    TEST_ASSERT_EQUAL(NTAG_ERR_LOCKED, ntag_program(&s_tag, &p, NULL, NULL));

    fresh(NTAG_215);
    sim_ntag_page(&s_sim, 3)[3] = 0x0F;         /* capability container: read-only */
    TEST_ASSERT_EQUAL(NTAG_ERR_LOCKED, ntag_program(&s_tag, &p, NULL, NULL));

    fresh(NTAG_215);
    memcpy(sim_ntag_page(&s_sim, 3), (const uint8_t[]){ 0x12, 0x34, 0x56, 0x78 }, 4);  /* not NDEF, not blank */
    TEST_ASSERT_EQUAL(NTAG_ERR_LOCKED, ntag_program(&s_tag, &p, NULL, NULL));
    TEST_ASSERT_EQUAL(0, s_sim.writes);

    /* The lock bits themselves are never written, whatever happens. */
    fresh(NTAG_215);
    p = job_with_pwd(PWD);
    TEST_ASSERT_EQUAL(NTAG_OK, ntag_program(&s_tag, &p, NULL, NULL));
    TEST_ASSERT_EQUAL_HEX8(0, sim_ntag_page(&s_sim, 2)[2]);
    TEST_ASSERT_EQUAL_HEX8(0, sim_ntag_page(&s_sim, 2)[3]);
    TEST_ASSERT_EQUAL_HEX8(0, sim_ntag_page(&s_sim, 0x82)[0]);
    TEST_ASSERT_EQUAL_HEX8(0, sim_ntag_page(&s_sim, 0x82)[1]);
}

static void test_unformatted_tag_gets_a_capability_container(void)
{
    fresh(NTAG_215);
    memset(sim_ntag_page(&s_sim, 3), 0, 4);
    memset(sim_ntag_page(&s_sim, 4), 0, 4);

    ntag_state_t st;
    TEST_ASSERT_EQUAL(NTAG_OK, ntag_inspect(&s_tag, &st, NULL));
    TEST_ASSERT_FALSE(st.formatted);
    TEST_ASSERT_FALSE(st.locked);

    ntag_program_t p = job();
    TEST_ASSERT_EQUAL(NTAG_OK, ntag_program(&s_tag, &p, NULL, NULL));
    const uint8_t cc[4] = { 0xE1, 0x10, 0x3E, 0x00 };
    TEST_ASSERT_EQUAL_MEMORY(cc, sim_ntag_page(&s_sim, 3), 4);
    assert_tag_holds(s_msg, s_msg_len);
}

static void test_protect_sets_password_and_auth0(void)
{
    fresh(NTAG_215);
    ntag_program_t p = job_with_pwd(PWD);
    ntag_state_t st;
    TEST_ASSERT_EQUAL(NTAG_OK, ntag_program(&s_tag, &p, &st, NULL));
    TEST_ASSERT_TRUE(st.is_protected);
    assert_tag_holds(s_msg, s_msg_len);

    /* Datasheet table 8: CFG0 83h (AUTH0 in byte 3), CFG1 84h (ACCESS), PWD 85h, PACK 86h. */
    const uint8_t cfg0[4] = { 0x04, 0x00, 0x00, 0x03 };     /* the mirror byte is preserved */
    const uint8_t cfg1[4] = { 0x00, 0x05, 0x00, 0x00 };     /* PROT=0, AUTHLIM=0, CFGLCK=0 */
    TEST_ASSERT_EQUAL_MEMORY(cfg0, sim_ntag_page(&s_sim, 0x83), 4);
    TEST_ASSERT_EQUAL_MEMORY(cfg1, sim_ntag_page(&s_sim, 0x84), 4);
    TEST_ASSERT_EQUAL_MEMORY(PWD, sim_ntag_page(&s_sim, 0x85), 4);
    TEST_ASSERT_EQUAL_MEMORY(PACK, sim_ntag_page(&s_sim, 0x86), 2);

    /* A reader without the password can read it and cannot write it. */
    nfc_xcvr_t *x = sim_ntag_xcvr(&s_sim);
    TEST_ASSERT_EQUAL(NFC_OK, x->reselect(x));
    uint8_t rx[16];
    size_t n = 0;
    TEST_ASSERT_EQUAL(NFC_OK, x->transceive(x, (const uint8_t[]){ 0x30, 0x04 }, 2, rx, sizeof(rx), &n));
    TEST_ASSERT_EQUAL_HEX8(0x03, rx[0]);
    TEST_ASSERT_EQUAL(NFC_ERR_NAK, x->transceive(x, (const uint8_t[]){ 0xA2, 0x04, 0, 0, 0, 0 }, 6, rx, sizeof(rx), &n));
    TEST_ASSERT_EQUAL(NFC_OK, x->reselect(x));
    TEST_ASSERT_EQUAL(NFC_ERR_NAK, x->transceive(x, (const uint8_t[]){ 0xA2, 0x03, 0, 0, 0, 0x0F }, 6, rx, sizeof(rx), &n));
}

static void test_reprogram_protected_tag(void)
{
    fresh(NTAG_215);
    ntag_program_t p = job_with_pwd(PWD);
    TEST_ASSERT_EQUAL(NTAG_OK, ntag_program(&s_tag, &p, NULL, NULL));

    /* A new tap: a new selection, nothing authenticated. */
    sim_ntag_present(&s_sim);
    TEST_ASSERT_EQUAL(NTAG_OK, ntag_identify(&s_tag, sim_ntag_xcvr(&s_sim)));
    ntag_state_t st;
    TEST_ASSERT_EQUAL(NTAG_OK, ntag_inspect(&s_tag, &st, NULL));
    TEST_ASSERT_TRUE(st.is_protected);
    TEST_ASSERT_TRUE(st.readable);

    s_msg_len = make_inventree_ndef(s_msg, sizeof(s_msg), "inventree.example/web/stock/location/7", "INV-SL7");
    const unsigned writes_before = s_sim.writes;

    p = job();
    p.overwrite = true;
    TEST_ASSERT_EQUAL(NTAG_ERR_AUTH_REQUIRED, ntag_program(&s_tag, &p, NULL, NULL));

    p = job_with_pwd(OTHER_PWD);
    p.overwrite = true;
    TEST_ASSERT_EQUAL(NTAG_ERR_AUTH_FAILED, ntag_program(&s_tag, &p, NULL, NULL));
    TEST_ASSERT_EQUAL(writes_before, s_sim.writes);
    TEST_ASSERT_EQUAL(0, s_writing_calls - 1);      /* only the first programming announced */

    p = job_with_pwd(PWD);
    TEST_ASSERT_EQUAL(NTAG_ERR_NOT_BLANK, ntag_program(&s_tag, &p, NULL, NULL));
    TEST_ASSERT_EQUAL(writes_before, s_sim.writes);

    p.overwrite = true;
    TEST_ASSERT_EQUAL(NTAG_OK, ntag_program(&s_tag, &p, &st, NULL));
    TEST_ASSERT_TRUE(st.is_protected);
    assert_tag_holds(s_msg, s_msg_len);
}

static void test_password_rotation(void)
{
    fresh(NTAG_215);
    ntag_program_t p = job_with_pwd(PWD);
    TEST_ASSERT_EQUAL(NTAG_OK, ntag_program(&s_tag, &p, NULL, NULL));

    sim_ntag_present(&s_sim);
    TEST_ASSERT_EQUAL(NTAG_OK, ntag_identify(&s_tag, sim_ntag_xcvr(&s_sim)));
    p = job_with_pwd(OTHER_PWD);
    p.overwrite = true;
    p.have_old_pwd = true;
    memcpy(p.old_pwd, PWD, 4);
    TEST_ASSERT_EQUAL(NTAG_OK, ntag_program(&s_tag, &p, NULL, NULL));
    TEST_ASSERT_EQUAL_MEMORY(OTHER_PWD, sim_ntag_page(&s_sim, 0x85), 4);

    sim_ntag_present(&s_sim);
    TEST_ASSERT_EQUAL(NTAG_OK, ntag_identify(&s_tag, sim_ntag_xcvr(&s_sim)));
    TEST_ASSERT_EQUAL(NTAG_ERR_AUTH_FAILED, ntag_auth(&s_tag, PWD, NULL));
    uint8_t pack[2];
    TEST_ASSERT_EQUAL(NTAG_OK, ntag_auth(&s_tag, OTHER_PWD, pack));
    TEST_ASSERT_EQUAL_MEMORY(PACK, pack, 2);
}

static void test_read_protected_tag(void)
{
    fresh(NTAG_215);
    sim_ntag_set_ndef(&s_sim, s_msg, s_msg_len);
    sim_ntag_protect(&s_sim, PWD, PACK, 0x03, true);

    ntag_state_t st;
    TEST_ASSERT_EQUAL(NTAG_OK, ntag_inspect(&s_tag, &st, NULL));
    TEST_ASSERT_FALSE(st.readable);
    TEST_ASSERT_TRUE(st.is_protected);

    ntag_program_t p = job();
    p.overwrite = true;
    TEST_ASSERT_EQUAL(NTAG_ERR_AUTH_REQUIRED, ntag_program(&s_tag, &p, NULL, NULL));
    p = job_with_pwd(OTHER_PWD);
    p.overwrite = true;
    TEST_ASSERT_EQUAL(NTAG_ERR_AUTH_FAILED, ntag_program(&s_tag, &p, NULL, NULL));

    s_msg_len = make_inventree_ndef(s_msg, sizeof(s_msg), "inventree.example/web/stock/location/9", "INV-SL9");
    p = job_with_pwd(PWD);
    p.overwrite = true;
    TEST_ASSERT_EQUAL(NTAG_OK, ntag_program(&s_tag, &p, NULL, NULL));
    assert_tag_holds(s_msg, s_msg_len);
    /* Re-protected the way this firmware does it: readable by phones again. */
    TEST_ASSERT_EQUAL_HEX8(0x00, sim_ntag_page(&s_sim, 0x84)[0] & 0x80);
}

/* Overwrite a tag, open or protected, losing the frame `glitch_at` exchanges in (never, if
 * negative). Returns how many exchanges the job took. */
static unsigned overwrite_with_glitch(bool protect, int glitch_at)
{
    fresh(NTAG_215);
    if (protect) {
        sim_ntag_set_ndef(&s_sim, s_msg, s_msg_len);
        sim_ntag_protect(&s_sim, PWD, PACK, 0x03, false);
    }
    s_msg_len = make_inventree_ndef(s_msg, sizeof(s_msg), "inventree.example/web/stock/location/3", "INV-SL3");
    ntag_program_t p = protect ? job_with_pwd(PWD) : job();
    p.overwrite = true;

    s_sim.exchanges = 0;
    s_sim.silent_after = glitch_at;
    TEST_ASSERT_EQUAL_MESSAGE(NTAG_OK, ntag_program(&s_tag, &p, NULL, NULL), "a single lost frame failed the job");
    TEST_ASSERT_EQUAL(-1, s_sim.silent_after);
    assert_tag_holds(s_msg, s_msg_len);
    return s_sim.exchanges;
}

/* Lose one frame at every point of a job in turn. Each time the job must still succeed: the
 * tag is re-selected, authenticated again if it had been, and the command repeated. */
static void test_missed_exchange_is_retried(void)
{
    for (int protect = 0; protect < 2; protect++) {
        const unsigned total = overwrite_with_glitch(protect, -1);
        TEST_ASSERT_TRUE(total > 10);
        for (unsigned at = 0; at < total; at++) {
            overwrite_with_glitch(protect, (int)at);
        }
    }
}

static void test_wipe(void)
{
    fresh(NTAG_215);
    ntag_program_t p = job_with_pwd(PWD);
    TEST_ASSERT_EQUAL(NTAG_OK, ntag_program(&s_tag, &p, NULL, NULL));

    sim_ntag_present(&s_sim);
    TEST_ASSERT_EQUAL(NTAG_OK, ntag_identify(&s_tag, sim_ntag_xcvr(&s_sim)));
    s_writing_calls = 0;
    TEST_ASSERT_EQUAL(NTAG_ERR_AUTH_REQUIRED, ntag_wipe(&s_tag, NULL, on_writing, NULL));
    TEST_ASSERT_EQUAL(NTAG_ERR_AUTH_FAILED, ntag_wipe(&s_tag, OTHER_PWD, on_writing, NULL));
    TEST_ASSERT_EQUAL(0, s_writing_calls);      /* refused before anything was written */
    assert_tag_holds(s_msg, s_msg_len);

    TEST_ASSERT_EQUAL(NTAG_OK, ntag_wipe(&s_tag, PWD, on_writing, NULL));
    assert_tag_holds(NULL, 0);
    TEST_ASSERT_EQUAL_HEX8(0xFF, sim_ntag_page(&s_sim, 0x83)[3]);
    const uint8_t factory_pwd[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
    TEST_ASSERT_EQUAL_MEMORY(factory_pwd, sim_ntag_page(&s_sim, 0x85), 4);

    /* Open again: anyone can program it. */
    sim_ntag_present(&s_sim);
    TEST_ASSERT_EQUAL(NTAG_OK, ntag_identify(&s_tag, sim_ntag_xcvr(&s_sim)));
    p = job();
    TEST_ASSERT_EQUAL(NTAG_OK, ntag_program(&s_tag, &p, NULL, NULL));

    /* Wiping an open tag needs no password. */
    TEST_ASSERT_EQUAL(NTAG_OK, ntag_wipe(&s_tag, NULL, on_writing, NULL));
    assert_tag_holds(NULL, 0);
}

static void test_config_lock_is_respected(void)
{
    fresh(NTAG_215);
    sim_ntag_page(&s_sim, 0x84)[0] |= 0x40;     /* CFGLCK: AUTH0 can no longer change */
    ntag_program_t p = job_with_pwd(PWD);
    TEST_ASSERT_EQUAL(NTAG_ERR_LOCKED, ntag_program(&s_tag, &p, NULL, NULL));
    TEST_ASSERT_EQUAL_HEX8(0xFF, sim_ntag_page(&s_sim, 0x83)[3]);
    /* the message itself went on; it is the protection that could not */
    assert_tag_holds(s_msg, s_msg_len);
}

static void test_reader_failure_is_reported_as_such(void)
{
    fresh(NTAG_215);
    s_sim.reader_broken = true;
    ntag_program_t p = job();
    TEST_ASSERT_EQUAL(NTAG_ERR_IO, ntag_program(&s_tag, &p, NULL, NULL));
}

void run_ntag21x_tests(void)
{
    RUN_TEST(test_identify_each_type);
    RUN_TEST(test_identify_rejects_other_tags_and_absent_ones);
    RUN_TEST(test_inspect_factory_tags);
    RUN_TEST(test_program_blank_tag);
    RUN_TEST(test_program_refuses_to_overwrite_unless_told);
    RUN_TEST(test_program_accepts_a_tag_a_phone_erased);
    RUN_TEST(test_program_size_limits);
    RUN_TEST(test_torn_write_never_leaves_a_broken_tag);
    RUN_TEST(test_locked_tags_are_left_alone);
    RUN_TEST(test_unformatted_tag_gets_a_capability_container);
    RUN_TEST(test_protect_sets_password_and_auth0);
    RUN_TEST(test_reprogram_protected_tag);
    RUN_TEST(test_password_rotation);
    RUN_TEST(test_read_protected_tag);
    RUN_TEST(test_missed_exchange_is_retried);
    RUN_TEST(test_wipe);
    RUN_TEST(test_config_lock_is_respected);
    RUN_TEST(test_reader_failure_is_reported_as_such);
}
