#include "boot_ack.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifdef NDEBUG
#error "Boot acknowledgement tests require assertions (compile with -UNDEBUG)."
#endif

enum operation { READ = 1, ERASE, BLANK_CHECK, PROGRAM, VERIFY };

typedef struct {
    uint8_t flash[OMNI_BOOT_METADATA_PAGE_SIZE];
    uint8_t programmed[OMNI_BOOT_METADATA_PAGE_SIZE];
    enum operation operations[16];
    unsigned operation_count;
    enum operation fail_operation;
    int32_t failure_status;
} fake_flash;

static int32_t record_operation(fake_flash *fake, enum operation operation)
{
    assert(fake->operation_count < sizeof(fake->operations) / sizeof(fake->operations[0]));
    fake->operations[fake->operation_count++] = operation;
    return fake->fail_operation == operation ? fake->failure_status : 0;
}

static int32_t read_page(void *context, uint8_t *page)
{
    fake_flash *fake = context;
    int32_t status = record_operation(fake, READ);
    if (status == 0) memcpy(page, fake->flash, sizeof(fake->flash));
    return status;
}

static int32_t erase_page(void *context)
{
    fake_flash *fake = context;
    int32_t status = record_operation(fake, ERASE);
    if (status == 0) memset(fake->flash, 0xff, sizeof(fake->flash));
    return status;
}

static int32_t verify_erased(void *context)
{
    fake_flash *fake = context;
    int32_t status = record_operation(fake, BLANK_CHECK);
    if (status != 0) return status;
    for (unsigned i = 0; i < sizeof(fake->flash); ++i)
        if (fake->flash[i] != 0xff) return -301;
    return 0;
}

static int32_t program_page(void *context, const uint8_t *page)
{
    fake_flash *fake = context;
    int32_t status = record_operation(fake, PROGRAM);
    if (status == 0) {
        memcpy(fake->programmed, page, sizeof(fake->programmed));
        memcpy(fake->flash, page, sizeof(fake->flash));
    }
    return status;
}

static int32_t verify_page(void *context, const uint8_t *page)
{
    fake_flash *fake = context;
    int32_t status = record_operation(fake, VERIFY);
    if (status != 0) return status;
    return memcmp(fake->flash, page, sizeof(fake->flash)) == 0 ? 0 : -302;
}

static void write32(uint8_t *page, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) page[i] = (uint8_t)(value >> (8 * i));
}

static void initialize(fake_flash *fake, uint8_t eligible, uint8_t pending,
                       uint8_t acknowledged, uint32_t force)
{
    memset(fake, 0, sizeof(*fake));
    /* Every non-header byte matters: this includes byte 15 and the entire tail. */
    for (unsigned i = 0; i < sizeof(fake->flash); ++i)
        fake->flash[i] = (uint8_t)(i * 73U + 19U);
    write32(fake->flash, UINT32_C(0xb00710ad));
    write32(fake->flash + 4, force);
    write32(fake->flash + 8, UINT32_C(0xc000));
    fake->flash[12] = eligible;
    fake->flash[13] = pending;
    fake->flash[14] = acknowledged;
}

static omni_ack_result apply(fake_flash *fake, int32_t *driver_status)
{
    uint8_t scratch[OMNI_BOOT_METADATA_PAGE_SIZE];
    memset(scratch, 0xa5, sizeof(scratch));
    const omni_ack_io io = {
        .context = fake,
        .read_page = read_page,
        .erase_page = erase_page,
        .verify_erased = verify_erased,
        .program_page = program_page,
        .verify_page = verify_page,
    };
    return omni_boot_ack_apply(&io, scratch, driver_status);
}

static omni_ack_result recover(fake_flash *fake, int32_t *driver_status)
{
    uint8_t scratch[OMNI_BOOT_METADATA_PAGE_SIZE];
    const omni_ack_io io = {fake, read_page, erase_page, verify_erased, program_page, verify_page};
    return omni_boot_recovery_apply(&io, scratch, driver_status);
}

static void test_recovery(void)
{
    for (unsigned flags=0; flags<8; ++flags) {
        fake_flash fake;
        initialize(&fake, (uint8_t)(flags&1U), (uint8_t)((flags>>1)&1U), (uint8_t)((flags>>2)&1U), 0);
        uint8_t expected[512]; memcpy(expected, fake.flash, 512);
        int32_t status=0;
        bool valid=(flags&4U) && (flags&3U);
        assert(recover(&fake,&status)==(valid?OMNI_ACK_WRITTEN:OMNI_ACK_INVALID_METADATA));
        assert(status==0);
        if (valid) write32(expected+4, UINT32_C(0xabbabaab));
        assert(memcmp(fake.flash,expected,512)==0);
        assert(fake.operation_count==(valid?5U:1U));
        if (valid) {
            fake.operation_count=0;
            assert(recover(&fake,&status)==OMNI_ACK_ALREADY_VALID);
            assert(fake.operation_count==1);
            assert(memcmp(fake.flash,expected,512)==0);
        }
    }
    const omni_ack_result failures[]={OMNI_ACK_WAITING,OMNI_ACK_READ_FAILED,OMNI_ACK_ERASE_FAILED,
        OMNI_ACK_BLANK_CHECK_FAILED,OMNI_ACK_PROGRAM_FAILED,OMNI_ACK_VERIFY_FAILED};
    for (unsigned op=1;op<=5;++op) {
        fake_flash fake; initialize(&fake,1,0,1,0);
        fake.fail_operation=(enum operation)op; fake.failure_status=-123;
        int32_t status=0;
        assert(recover(&fake,&status)==failures[op]);
        assert(status==-123 && fake.operation_count==op);
    }
    /* Every header byte is checked; reserved tail bytes are preserved above. */
    for (unsigned offset=0;offset<15;++offset) {
        fake_flash fake; initialize(&fake,1,0,1,0); fake.flash[offset]=0xff;
        int32_t status=0;
        assert(recover(&fake,&status)==OMNI_ACK_INVALID_METADATA);
        assert(fake.operation_count==1);
    }
}

static void expect_operations(const fake_flash *fake, unsigned count)
{
    assert(fake->operation_count == count);
    for (unsigned i = 0; i < count; ++i)
        assert(fake->operations[i] == (enum operation)(READ + i));
}

static void expect_read_only(fake_flash *fake, omni_ack_result expected)
{
    uint8_t original[OMNI_BOOT_METADATA_PAGE_SIZE];
    memcpy(original, fake->flash, sizeof(original));
    int32_t status = -999;
    assert(apply(fake, &status) == expected);
    assert(status == 0);
    expect_operations(fake, 1);
    assert(memcmp(fake->flash, original, sizeof(original)) == 0);
}

static void test_stock_commit_and_preservation(void)
{
    const uint32_t forces[] = {0, UINT32_C(0xabbabaab)};
    for (unsigned i = 0; i < sizeof(forces) / sizeof(forces[0]); ++i) {
        fake_flash fake;
        initialize(&fake, 0, 1, 0, forces[i]);
        uint8_t expected[OMNI_BOOT_METADATA_PAGE_SIZE];
        memcpy(expected, fake.flash, sizeof(expected));
        write32(expected + 4, 0);
        expected[14] = 1;
        int32_t status = -999;
        assert(apply(&fake, &status) == OMNI_ACK_WRITTEN);
        assert(status == 0);
        expect_operations(&fake, 5);
        assert(memcmp(fake.programmed, expected, sizeof(expected)) == 0);
        assert(memcmp(fake.flash, expected, sizeof(expected)) == 0);

        /* A subsequent startup must not consume another flash erase cycle. */
        fake.operation_count = 0;
        expect_read_only(&fake, OMNI_ACK_ALREADY_VALID);
    }
}

static void test_all_known_flag_states(void)
{
    const uint32_t forces[] = {0, UINT32_C(0xabbabaab)};
    for (uint8_t eligible = 0; eligible <= 1; ++eligible) {
        for (uint8_t pending = 0; pending <= 1; ++pending) {
            for (uint8_t acknowledged = 0; acknowledged <= 1; ++acknowledged) {
                for (unsigned f = 0; f < sizeof(forces) / sizeof(forces[0]); ++f) {
                    fake_flash fake;
                    initialize(&fake, eligible, pending, acknowledged, forces[f]);
                    if (!pending) {
                        /* Failed or ambiguous non-pending states are not repaired. */
                        expect_read_only(&fake, eligible && acknowledged && forces[f] == 0 ?
                            OMNI_ACK_ALREADY_VALID : OMNI_ACK_INVALID_METADATA);
                    } else if (acknowledged && forces[f] == 0) {
                        expect_read_only(&fake, OMNI_ACK_ALREADY_VALID);
                    } else {
                        uint8_t expected[OMNI_BOOT_METADATA_PAGE_SIZE];
                        memcpy(expected, fake.flash, sizeof(expected));
                        write32(expected + 4, 0);
                        expected[14] = 1;
                        int32_t status = -999;
                        assert(apply(&fake, &status) == OMNI_ACK_WRITTEN);
                        assert(status == 0);
                        expect_operations(&fake, 5);
                        assert(memcmp(fake.flash, expected, sizeof(expected)) == 0);
                    }
                }
            }
        }
    }
}

static void test_invalid_metadata_is_never_erased(void)
{
    const uint32_t bad_magic[] = {0, UINT32_MAX, UINT32_C(0xb00710ac)};
    const uint32_t bad_base[] = {0, UINT32_C(0x8000), UINT32_C(0xc001), UINT32_MAX};
    const uint32_t bad_force[] = {1, UINT32_C(0xabbabaaa), UINT32_MAX};
    fake_flash fake;
    for (unsigned i = 0; i < sizeof(bad_magic) / sizeof(bad_magic[0]); ++i) {
        initialize(&fake, 0, 1, 0, 0);
        write32(fake.flash, bad_magic[i]);
        expect_read_only(&fake, OMNI_ACK_INVALID_METADATA);
    }
    for (unsigned i = 0; i < sizeof(bad_base) / sizeof(bad_base[0]); ++i) {
        initialize(&fake, 0, 1, 0, 0);
        write32(fake.flash + 8, bad_base[i]);
        expect_read_only(&fake, OMNI_ACK_INVALID_METADATA);
    }
    for (unsigned i = 0; i < sizeof(bad_force) / sizeof(bad_force[0]); ++i) {
        initialize(&fake, 0, 1, 0, 0);
        write32(fake.flash + 4, bad_force[i]);
        expect_read_only(&fake, OMNI_ACK_INVALID_METADATA);
    }
    for (unsigned offset = 12; offset <= 14; ++offset) {
        /* Exercise all unsupported byte values, including erased flags. */
        for (unsigned value = 2; value <= 255; ++value) {
            initialize(&fake, 0, 1, 0, 0);
            fake.flash[offset] = (uint8_t)value;
            expect_read_only(&fake, OMNI_ACK_INVALID_METADATA);
        }
    }
    initialize(&fake, 0, 1, 0, 0);
    memset(fake.flash, 0xff, sizeof(fake.flash));
    expect_read_only(&fake, OMNI_ACK_INVALID_METADATA);
}

static void test_each_io_failure_stops_immediately(void)
{
    static const omni_ack_result results[] = {
        OMNI_ACK_WAITING,
        OMNI_ACK_READ_FAILED,
        OMNI_ACK_ERASE_FAILED,
        OMNI_ACK_BLANK_CHECK_FAILED,
        OMNI_ACK_PROGRAM_FAILED,
        OMNI_ACK_VERIFY_FAILED,
    };
    /* Driver status values are opaque; both positive and negative errors survive. */
    const int32_t statuses[] = {-1234, 101};
    for (enum operation op = READ; op <= VERIFY; op = (enum operation)(op + 1)) {
        for (unsigned s = 0; s < sizeof(statuses) / sizeof(statuses[0]); ++s) {
            fake_flash fake;
            initialize(&fake, 0, 1, 0, UINT32_C(0xabbabaab));
            uint8_t original[OMNI_BOOT_METADATA_PAGE_SIZE];
            memcpy(original, fake.flash, sizeof(original));
            fake.fail_operation = op;
            fake.failure_status = statuses[s];
            int32_t status = -999;
            assert(apply(&fake, &status) == results[op]);
            assert(status == statuses[s]);
            expect_operations(&fake, (unsigned)op);
            if (op <= ERASE) assert(memcmp(fake.flash, original, sizeof(original)) == 0);
            if (op == BLANK_CHECK || op == PROGRAM)
                for (unsigned i = 0; i < sizeof(fake.flash); ++i)
                    assert(fake.flash[i] == 0xff);
        }
    }
}

static void test_power_loss_after_erase_is_not_claimed_atomic(void)
{
    fake_flash fake;
    initialize(&fake, 0, 1, 0, UINT32_C(0xabbabaab));
    /* Stop after erasure to model the persistent state left by losing power. */
    fake.fail_operation = BLANK_CHECK;
    fake.failure_status = -777;
    int32_t status = 0;
    assert(apply(&fake, &status) == OMNI_ACK_BLANK_CHECK_FAILED);
    assert(status == -777);
    expect_operations(&fake, 3);
    for (unsigned i = 0; i < sizeof(fake.flash); ++i) assert(fake.flash[i] == 0xff);

    /* New startup: no previous RAM copy is available to invent a repair from. */
    fake.operation_count = 0;
    fake.fail_operation = 0;
    fake.failure_status = 0;
    expect_read_only(&fake, OMNI_ACK_INVALID_METADATA);
}

int main(void)
{
    test_recovery();
    _Static_assert(OMNI_BOOT_METADATA_ADDRESS == UINT32_C(0x7f800), "Metadata address changed");
    _Static_assert(OMNI_BOOT_METADATA_PAGE_SIZE == 512U, "Metadata page size changed");
    test_stock_commit_and_preservation();
    test_all_known_flag_states();
    test_invalid_metadata_is_never_erased();
    test_each_io_failure_stops_immediately();
    test_power_loss_after_erase_is_not_claimed_atomic();
    puts("boot acknowledgement policy tests passed");
    return 0;
}
