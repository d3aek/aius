/* Host suite for lib/aius_ota's activation sequencer, reworked to the
 * verify-before-window / byte-compare-within contract (aius-ota-upgrade.md, the
 * final slice): the WHOLE digest + signature verification runs in
 * aius_ota_activate_begin() over a caller-provided linear staged image, BEFORE
 * the dark window; the dark window then only erases, programs from that staged
 * view, and byte-compares the readback against it -- no SHA-256, no chunked
 * medium reader.
 *
 * The device under the sequencer is a FAKE FLASH with erase-state modelling --
 * an erased byte is 0xFF, a program over a non-erased byte is a fault, and a
 * readback returns what the array actually holds. Its own discrimination is
 * proven first (test_fake_device_discriminates), because a fake that cannot
 * fail turns every green below into a tautology.
 *
 * The staged image is NOT a device primitive any more: it is a plain byte array
 * (g_staged) handed to begin(), standing in for the composition's PSRAM copy.
 *
 * The load-bearing technique is the OPERATION TRACE. Every primitive call the
 * sequencer makes is appended to one array with its kind, address and length,
 * so the ordering contract -- mark before any erase, the FCB erased first,
 * every body byte programmed and read back before the FCB is rewritten, a
 * watchdog feed between every pair of operations -- is checked as a property of
 * the recorded sequence rather than inferred from the final flash contents.
 *
 * Power loss is simulated by truncation: run k steps of a real activation, stop
 * calling step(), then rebuild the session from its persisted record and ask
 * the sequencer to start again. Repeated for every k from 0 (after begin, the
 * mark written, nothing erased) to the last step, so there is no boundary at
 * which an interrupted activation is resumable or the record ambiguous. */

#include "aius_ota.h"
#include "aius_ota_activate.h"
#include "aius_ota_health.h"
#include "aius_sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) check((condition), #condition, __LINE__)

static const char *current_key = "startup";

static void check(int condition, const char *expression, int line)
{
	if (condition)
		return;
	fprintf(stderr, "ota activation oracle mismatch [%s] line %d: %s\n",
	        current_key, line, expression);
	exit(1);
}

/* -------------------------------------------------------------------------
 * Fixtures
 * ---------------------------------------------------------------------- */

#define TEST_TARGET_ID "teensy41-optical"
#define TEST_REVISION "7a66153703d1"
#define TEST_BUILD_UTC UINT64_C(1753900000) /* 2025-07-30, inside the window */

/* Deliberately awkward: 12,900 = 3 whole sectors + 612 bytes, so the last
 * sector is partial AND the last programmed chunk is 100 bytes rather than
 * 256. A sector-aligned test image would never exercise either tail. */
#define TEST_IMAGE_BYTES 12900u

/* The other end of the range: one byte past the IVT, the smallest image the
 * geometry admits, whose whole body is a single 1-byte program. */
#define TEST_MIN_IMAGE_BYTES AIUS_OTA_ACTIVATE_MIN_IMAGE_BYTES

#define FAKE_FLASH_SECTORS 8u
#define FAKE_FLASH_BYTES (FAKE_FLASH_SECTORS * AIUS_OTA_FLASH_SECTOR_BYTES)
#define TRACE_CAPACITY 2048u

/* The cycle counter starts here, so it WRAPS partway through every activation
 * and the 64-bit accumulation of 32-bit differences is exercised on every run
 * rather than in one dedicated test. */
#define TEST_CYCLES_START (UINT32_MAX - 5000u)
#define TEST_CYCLES_PER_CALL 1000u

typedef enum {
	OP_FEED = 0,
	OP_ERASE,
	OP_PROGRAM,
	OP_FLASH_READ,
	OP_HEALTH_READ,
	OP_HEALTH_WRITE
} OpKind;

typedef struct {
	OpKind kind;
	uint32_t address; /* flash address */
	uint32_t length;
} Op;

typedef struct {
	uint8_t flash[FAKE_FLASH_BYTES];
	uint8_t health[AIUS_OTA_HEALTH_RECORD_BYTES];

	Op trace[TRACE_CAPACITY];
	uint32_t ops;

	uint32_t cycles;

	/* Fault injection: fail the Nth call of that kind, 1-based; 0 never. */
	uint32_t fail_erase_at;
	uint32_t fail_program_at;
	uint32_t fail_flash_read_at;
	uint32_t fail_health_read_at;
	uint32_t fail_health_write_at;
	uint32_t erases;
	uint32_t programs;
	uint32_t flash_reads;
	uint32_t health_reads;
	uint32_t health_writes;

	/* A program over a byte that is not 0xFF is a fault rather than a silent
	 * bit-AND. Real NOR would AND; this device refuses, so an ordering bug
	 * that programmed before erasing is convicted at the operation rather
	 * than three phases later. Turned off by the corruption tests, which need
	 * the write to land wrong instead of not landing. */
	bool strict_program;
	uint32_t dirty_programs;

	/* Silently flip a bit at this FLASH offset as it is programmed: a flash
	 * cell that did not take, indistinguishable from success at the driver. */
	bool corrupt_enabled;
	uint32_t corrupt_offset;
} FakeDevice;

static FakeDevice g_dev;
static uint8_t g_new_image[TEST_IMAGE_BYTES];
static uint8_t g_old_image[FAKE_FLASH_BYTES];
static uint8_t g_new_digest[AIUS_OTA_DIGEST_BYTES];
static uint8_t g_old_digest[AIUS_OTA_DIGEST_BYTES];
/* The staged image the composition would hold in PSRAM: a linear copy of the
 * new image, handed to begin(). device_reset() refreshes it; the corruption
 * tests mutate it to model a decayed staging medium. */
static uint8_t g_staged[TEST_IMAGE_BYTES];

static void fill_pattern(uint8_t *bytes, uint32_t length, uint8_t seed)
{
	uint32_t index;

	for (index = 0u; index < length; index++)
		bytes[index] = (uint8_t)((index * 31u + seed) & 0xffu);
}

static void trace_push(FakeDevice *dev, OpKind kind, uint32_t address,
	               uint32_t length)
{
	CHECK(dev->ops < TRACE_CAPACITY);
	dev->trace[dev->ops].kind = kind;
	dev->trace[dev->ops].address = address;
	dev->trace[dev->ops].length = length;
	dev->ops++;
}

/* -------------------------------------------------------------------------
 * The fake device's primitives
 * ---------------------------------------------------------------------- */

static int fake_erase(void *ctx, uint32_t address)
{
	FakeDevice *dev = (FakeDevice *)ctx;
	uint32_t offset;

	trace_push(dev, OP_ERASE, address, AIUS_OTA_FLASH_SECTOR_BYTES);
	dev->erases++;
	if (dev->fail_erase_at == dev->erases)
		return -6;
	if (address < AIUS_OTA_FLASH_BASE)
		return -1;
	offset = address - AIUS_OTA_FLASH_BASE;
	if (offset % AIUS_OTA_FLASH_SECTOR_BYTES != 0u ||
	    offset >= FAKE_FLASH_BYTES)
		return -1;
	memset(&dev->flash[offset], 0xff, AIUS_OTA_FLASH_SECTOR_BYTES);
	return 0;
}

static int fake_program(void *ctx, uint32_t address, const uint8_t *data,
	                uint32_t length)
{
	FakeDevice *dev = (FakeDevice *)ctx;
	uint32_t offset;
	uint32_t index;
	bool dirty = false;

	trace_push(dev, OP_PROGRAM, address, length);
	dev->programs++;
	if (dev->fail_program_at == dev->programs)
		return -7;
	if (address < AIUS_OTA_FLASH_BASE)
		return -1;
	offset = address - AIUS_OTA_FLASH_BASE;
	if (length == 0u || offset >= FAKE_FLASH_BYTES ||
	    length > FAKE_FLASH_BYTES - offset)
		return -1;

	for (index = 0u; index < length; index++) {
		if (dev->flash[offset + index] != 0xffu)
			dirty = true;
	}
	if (dirty) {
		dev->dirty_programs++;
		if (dev->strict_program)
			return -2;
	}
	for (index = 0u; index < length; index++) {
		uint8_t value = data[index];
		if (dev->corrupt_enabled &&
		    dev->corrupt_offset == offset + index)
			value = (uint8_t)(value ^ 0x40u);
		/* NOR programming can only clear bits. */
		dev->flash[offset + index] &= value;
	}
	return 0;
}

static int fake_flash_read(void *ctx, uint32_t address, uint8_t *destination,
	                   uint32_t length)
{
	FakeDevice *dev = (FakeDevice *)ctx;
	uint32_t offset;

	trace_push(dev, OP_FLASH_READ, address, length);
	dev->flash_reads++;
	if (dev->fail_flash_read_at == dev->flash_reads)
		return -8;
	if (address < AIUS_OTA_FLASH_BASE)
		return -1;
	offset = address - AIUS_OTA_FLASH_BASE;
	if (length == 0u || offset >= FAKE_FLASH_BYTES ||
	    length > FAKE_FLASH_BYTES - offset)
		return -1;
	memcpy(destination, &dev->flash[offset], length);
	return 0;
}

static void fake_watchdog(void *ctx)
{
	trace_push((FakeDevice *)ctx, OP_FEED, 0u, 0u);
}

static uint32_t fake_cycles(void *ctx)
{
	FakeDevice *dev = (FakeDevice *)ctx;
	uint32_t now = dev->cycles;

	dev->cycles += TEST_CYCLES_PER_CALL;
	return now;
}

static int fake_health_read(void *ctx, uint8_t *bytes, uint32_t length)
{
	FakeDevice *dev = (FakeDevice *)ctx;

	trace_push(dev, OP_HEALTH_READ, 0u, length);
	dev->health_reads++;
	if (dev->fail_health_read_at == dev->health_reads)
		return -9;
	if (length != AIUS_OTA_HEALTH_RECORD_BYTES)
		return -1;
	memcpy(bytes, dev->health, length);
	return 0;
}

static int fake_health_write(void *ctx, const uint8_t *bytes, uint32_t length)
{
	FakeDevice *dev = (FakeDevice *)ctx;

	trace_push(dev, OP_HEALTH_WRITE, 0u, length);
	dev->health_writes++;
	if (dev->fail_health_write_at == dev->health_writes)
		return -10;
	if (length != AIUS_OTA_HEALTH_RECORD_BYTES)
		return -1;
	memcpy(dev->health, bytes, length);
	return 0;
}

static AiusOtaActivatePrimitives primitives_over(FakeDevice *dev)
{
	AiusOtaActivatePrimitives primitives;

	primitives.ctx = dev;
	primitives.erase_sector = fake_erase;
	primitives.program = fake_program;
	primitives.flash_read = fake_flash_read;
	primitives.watchdog_feed = fake_watchdog;
	primitives.cycles = fake_cycles;
	primitives.health_read = fake_health_read;
	primitives.health_write = fake_health_write;
	return primitives;
}

/* -------------------------------------------------------------------------
 * The verifier seam, and the begin() the tests drive
 * ---------------------------------------------------------------------- */

static int accepting_verify(const uint8_t digest[AIUS_OTA_DIGEST_BYTES],
	                    const uint8_t signature[AIUS_OTA_SIGNATURE_BYTES],
	                    void *ctx)
{
	(void)digest;
	(void)signature;
	(void)ctx;
	return 0;
}

static int rejecting_verify(const uint8_t digest[AIUS_OTA_DIGEST_BYTES],
	                    const uint8_t signature[AIUS_OTA_SIGNATURE_BYTES],
	                    void *ctx)
{
	(void)digest;
	(void)signature;
	(void)ctx;
	return -42;
}

static AiusOtaVerifier g_accept = { NULL, accepting_verify };

/* The common begin(): the accepting verifier over the staged copy. */
static AiusOtaStatus begin_over(AiusOtaActivator *activator,
	                        AiusOtaSession *session,
	                        const AiusOtaActivatePrimitives *primitives,
	                        uint32_t image_length,
	                        const uint8_t *running_digest)
{
	return aius_ota_activate_begin(activator, session, primitives,
	                               &g_accept, g_staged, image_length,
	                               running_digest);
}

/* The board as it stands before an upgrade: the old image in flash, a health
 * record that committed it, and the new image staged in RAM. */
static void device_reset(FakeDevice *dev, const uint8_t *image,
	                 uint32_t image_length)
{
	AiusOtaHealth health;

	memset(dev, 0, sizeof(*dev));
	memcpy(dev->flash, g_old_image, FAKE_FLASH_BYTES);
	memcpy(g_staged, image, image_length);
	dev->cycles = TEST_CYCLES_START;
	dev->strict_program = true;

	aius_ota_health_init(&health);
	CHECK(aius_ota_health_note_activation(
		      &health, g_old_digest,
		      AIUS_OTA_HEALTH_DEFAULT_TRIAL_BOOTS) == AIUS_OTA_OK);
	CHECK(aius_ota_health_note_boot(&health, g_old_digest, NULL) ==
	      AIUS_OTA_OK);
	CHECK(aius_ota_health_mark_healthy(&health, g_old_digest) ==
	      AIUS_OTA_OK);
	CHECK(aius_ota_health_save(&health, dev->health, sizeof(dev->health)) ==
	      AIUS_OTA_OK);
}

/* -------------------------------------------------------------------------
 * Sessions
 * ---------------------------------------------------------------------- */

static void build_wire(uint8_t *wire, const uint8_t *payload,
	               uint32_t image_length)
{
	AiusOtaImageHeader header;

	memset(&header, 0, sizeof(header));
	header.magic = AIUS_OTA_MAGIC;
	header.format_version = AIUS_OTA_FORMAT_VERSION;
	header.header_length = (uint16_t)AIUS_OTA_HEADER_BYTES;
	header.image_length = image_length;
	header.signature_scheme = AIUS_OTA_SIGSCHEME_ED25519;
	header.build_utc = TEST_BUILD_UTC;
	memcpy(header.target_id, TEST_TARGET_ID, sizeof(TEST_TARGET_ID));
	memcpy(header.build_revision, TEST_REVISION, sizeof(TEST_REVISION));
	aius_sha256(payload, image_length, header.payload_digest);
	memset(header.signature, 0xa5, sizeof(header.signature));
	CHECK(aius_ota_header_serialize(&header, wire, AIUS_OTA_HEADER_BYTES) ==
	      AIUS_OTA_OK);
}

/* Drives the slice-1 machine all the way to ACTIVATION_REQUESTED through its
 * public interface, so the sequencer is always handed a session that got there
 * the way a real upgrade does. */
static void request_activation(AiusOtaSession *session, const uint8_t *payload,
	                       uint32_t image_length)
{
	uint8_t wire[AIUS_OTA_HEADER_BYTES];
	AiusOtaVerifier verifier;
	uint32_t offset = 0u;

	verifier.ctx = NULL;
	verifier.verify = accepting_verify;

	CHECK(aius_ota_init(session, TEST_TARGET_ID, strlen(TEST_TARGET_ID)) ==
	      AIUS_OTA_OK);
	CHECK(aius_ota_latch_arm(session) == AIUS_OTA_OK);
	build_wire(wire, payload, image_length);
	CHECK(aius_ota_offer(session, wire, sizeof(wire)) == AIUS_OTA_OK);
	while (offset < image_length) {
		uint32_t take = image_length - offset;
		if (take > 512u)
			take = 512u;
		CHECK(aius_ota_absorb(session, offset, &payload[offset],
		                      take) == AIUS_OTA_OK);
		offset += take;
	}
	CHECK(aius_ota_verify(session, &verifier) == AIUS_OTA_OK);
	CHECK(aius_ota_arm_image(session) == AIUS_OTA_OK);
	CHECK(aius_ota_request_activation(session) == AIUS_OTA_OK);
}

/* -------------------------------------------------------------------------
 * Trace queries
 * ---------------------------------------------------------------------- */

static uint32_t count_of(const FakeDevice *dev, OpKind kind)
{
	uint32_t index;
	uint32_t count = 0u;

	for (index = 0u; index < dev->ops; index++) {
		if (dev->trace[index].kind == kind)
			count++;
	}
	return count;
}

/* Index of the first op of `kind` whose address is in [low, high), or dev->ops
 * when there is none. */
static uint32_t first_in_range(const FakeDevice *dev, OpKind kind,
	                       uint32_t low, uint32_t high)
{
	uint32_t index;

	for (index = 0u; index < dev->ops; index++) {
		const Op *op = &dev->trace[index];
		if (op->kind == kind && op->address >= low && op->address < high)
			return index;
	}
	return dev->ops;
}

static uint32_t last_in_range(const FakeDevice *dev, OpKind kind, uint32_t low,
	                      uint32_t high)
{
	uint32_t index;
	uint32_t found = dev->ops;

	for (index = 0u; index < dev->ops; index++) {
		const Op *op = &dev->trace[index];
		if (op->kind == kind && op->address >= low && op->address < high)
			found = index;
	}
	return found;
}

/* No two primitive calls without a watchdog feed between them. */
static void assert_feed_cadence(const FakeDevice *dev)
{
	uint32_t index;
	uint32_t since_feed = 0u;

	for (index = 0u; index < dev->ops; index++) {
		if (dev->trace[index].kind == OP_FEED) {
			since_feed = 0u;
			continue;
		}
		since_feed++;
		CHECK(since_feed <= AIUS_OTA_ACTIVATE_MAX_OPS_PER_FEED);
	}
	CHECK(count_of(dev, OP_FEED) > 0u);
}

static bool region_is_erased(const FakeDevice *dev, uint32_t offset,
	                     uint32_t length)
{
	uint32_t index;

	for (index = 0u; index < length; index++) {
		if (dev->flash[offset + index] != 0xffu)
			return false;
	}
	return true;
}

static void load_health(const FakeDevice *dev, AiusOtaHealth *health)
{
	CHECK(aius_ota_health_load(health, dev->health,
	                           AIUS_OTA_HEALTH_RECORD_BYTES) ==
	      AIUS_OTA_OK);
}

/* -------------------------------------------------------------------------
 * The fake's own negative control
 * ---------------------------------------------------------------------- */

static void test_fake_device_discriminates(void)
{
	uint8_t data[16];
	uint8_t back[16];

	current_key = "fake.erase-state";
	device_reset(&g_dev, g_new_image, TEST_IMAGE_BYTES);
	memset(data, 0x5a, sizeof(data));

	/* The flash starts holding the old image, so a program without an erase
	 * must be refused -- if this passed, every ordering assertion below
	 * would be checking a device that cannot tell erased from written. */
	CHECK(fake_program(&g_dev, AIUS_OTA_FLASH_BASE, data, sizeof(data)) !=
	      0);
	CHECK(g_dev.dirty_programs == 1u);

	CHECK(fake_erase(&g_dev, AIUS_OTA_FLASH_BASE) == 0);
	CHECK(region_is_erased(&g_dev, 0u, AIUS_OTA_FLASH_SECTOR_BYTES));
	CHECK(fake_program(&g_dev, AIUS_OTA_FLASH_BASE, data, sizeof(data)) ==
	      0);
	CHECK(fake_flash_read(&g_dev, AIUS_OTA_FLASH_BASE, back, sizeof(back)) ==
	      0);
	CHECK(memcmp(back, data, sizeof(data)) == 0);

	/* A second program over the same bytes is dirty again. */
	CHECK(fake_program(&g_dev, AIUS_OTA_FLASH_BASE, data, sizeof(data)) !=
	      0);
	CHECK(g_dev.dirty_programs == 2u);

	current_key = "fake.out-of-range";
	CHECK(fake_erase(&g_dev, AIUS_OTA_FLASH_BASE + FAKE_FLASH_BYTES) != 0);
	CHECK(fake_erase(&g_dev, AIUS_OTA_FLASH_BASE + 1u) != 0);
	CHECK(fake_program(&g_dev, AIUS_OTA_FLASH_BASE + FAKE_FLASH_BYTES, data,
	                   sizeof(data)) != 0);
	CHECK(fake_flash_read(&g_dev, AIUS_OTA_FLASH_BASE + FAKE_FLASH_BYTES,
	                      back, sizeof(back)) != 0);

	current_key = "fake.corruption";
	device_reset(&g_dev, g_new_image, TEST_IMAGE_BYTES);
	g_dev.corrupt_enabled = true;
	g_dev.corrupt_offset = 4u;
	CHECK(fake_erase(&g_dev, AIUS_OTA_FLASH_BASE) == 0);
	CHECK(fake_program(&g_dev, AIUS_OTA_FLASH_BASE, data, sizeof(data)) ==
	      0);
	CHECK(fake_flash_read(&g_dev, AIUS_OTA_FLASH_BASE, back, sizeof(back)) ==
	      0);
	CHECK(back[4] != data[4]);
	CHECK(back[3] == data[3]);
}

/* -------------------------------------------------------------------------
 * Geometry
 * ---------------------------------------------------------------------- */

static void test_geometry_constants_are_exact(void)
{
	current_key = "geometry.constants";
	/* The 0x1000-adjacent arithmetic the plan calls out: the FCB is sector
	 * 0 and the IVT is the first byte of sector 1, NOT a second structure
	 * inside sector 0. */
	CHECK(AIUS_OTA_FLASH_BASE == UINT32_C(0x60000000));
	CHECK(AIUS_OTA_FLASH_FCB_OFFSET == 0u);
	CHECK(AIUS_OTA_FLASH_IVT_OFFSET == 0x1000u);
	CHECK(AIUS_OTA_FLASH_IVT_OFFSET == AIUS_OTA_FLASH_SECTOR_BYTES);
	CHECK(AIUS_OTA_FLASH_BASE + AIUS_OTA_FLASH_IVT_OFFSET ==
	      UINT32_C(0x60001000));
	CHECK(AIUS_OTA_FLASH_HEAD_BYTES == AIUS_OTA_FLASH_SECTOR_BYTES);
	CHECK(AIUS_OTA_FLASH_USABLE_BYTES == UINT32_C(8126464));
	CHECK(AIUS_OTA_FLASH_USABLE_BYTES % AIUS_OTA_FLASH_SECTOR_BYTES == 0u);
	CHECK(AIUS_OTA_FLASH_USABLE_BYTES / AIUS_OTA_FLASH_SECTOR_BYTES ==
	      1984u);
	CHECK(AIUS_OTA_FLASH_SECTOR_BYTES % AIUS_OTA_ACTIVATE_CHUNK_BYTES == 0u);
}

static void test_bounds_and_alignment_refusals(void)
{
	const uint32_t limit =
		AIUS_OTA_FLASH_BASE + AIUS_OTA_FLASH_USABLE_BYTES;

	current_key = "geometry.erase";
	CHECK(aius_ota_activate_check_erase(AIUS_OTA_FLASH_BASE) ==
	      AIUS_OTA_OK);
	CHECK(aius_ota_activate_check_erase(AIUS_OTA_FLASH_BASE +
	                                    AIUS_OTA_FLASH_SECTOR_BYTES) ==
	      AIUS_OTA_OK);
	CHECK(aius_ota_activate_check_erase(limit -
	                                    AIUS_OTA_FLASH_SECTOR_BYTES) ==
	      AIUS_OTA_OK);
	/* Unaligned, below the window, and one sector past the end. */
	CHECK(aius_ota_activate_check_erase(AIUS_OTA_FLASH_BASE + 1u) ==
	      AIUS_OTA_GEOMETRY);
	CHECK(aius_ota_activate_check_erase(AIUS_OTA_FLASH_BASE + 2048u) ==
	      AIUS_OTA_GEOMETRY);
	CHECK(aius_ota_activate_check_erase(AIUS_OTA_FLASH_BASE - 4096u) ==
	      AIUS_OTA_GEOMETRY);
	CHECK(aius_ota_activate_check_erase(0u) == AIUS_OTA_GEOMETRY);
	CHECK(aius_ota_activate_check_erase(limit) == AIUS_OTA_GEOMETRY);

	current_key = "geometry.span";
	CHECK(aius_ota_activate_check_span(AIUS_OTA_FLASH_BASE,
	                                   AIUS_OTA_ACTIVATE_CHUNK_BYTES) ==
	      AIUS_OTA_OK);
	CHECK(aius_ota_activate_check_span(AIUS_OTA_FLASH_BASE + 4095u, 1u) ==
	      AIUS_OTA_OK);
	CHECK(aius_ota_activate_check_span(limit - 1u, 1u) == AIUS_OTA_OK);
	/* A zero-length or over-chunk span is the interface's own refusal. */
	CHECK(aius_ota_activate_check_span(AIUS_OTA_FLASH_BASE, 0u) ==
	      AIUS_OTA_INVALID_ARG);
	CHECK(aius_ota_activate_check_span(AIUS_OTA_FLASH_BASE,
	                                   AIUS_OTA_ACTIVATE_CHUNK_BYTES +
		                           1u) == AIUS_OTA_INVALID_ARG);
	/* Straddling the sector boundary, by one byte. */
	CHECK(aius_ota_activate_check_span(AIUS_OTA_FLASH_BASE + 4095u, 2u) ==
	      AIUS_OTA_GEOMETRY);
	/* The last whole chunk of a sector ends exactly on its boundary; one
	 * byte further along is the first span that would straddle. */
	CHECK(aius_ota_activate_check_span(AIUS_OTA_FLASH_BASE + 3840u, 256u) ==
	      AIUS_OTA_OK);
	CHECK(aius_ota_activate_check_span(AIUS_OTA_FLASH_BASE + 3841u, 256u) ==
	      AIUS_OTA_GEOMETRY);
	/* Past the end of the window, and below its base. */
	CHECK(aius_ota_activate_check_span(limit, 1u) == AIUS_OTA_GEOMETRY);
	CHECK(aius_ota_activate_check_span(limit - 128u, 256u) ==
	      AIUS_OTA_GEOMETRY);
	CHECK(aius_ota_activate_check_span(AIUS_OTA_FLASH_BASE - 1u, 1u) ==
	      AIUS_OTA_GEOMETRY);

	current_key = "geometry.image-length";
	CHECK(aius_ota_activate_check_image_length(0u) == AIUS_OTA_GEOMETRY);
	CHECK(aius_ota_activate_check_image_length(AIUS_OTA_FLASH_IVT_OFFSET) ==
	      AIUS_OTA_GEOMETRY);
	CHECK(aius_ota_activate_check_image_length(TEST_MIN_IMAGE_BYTES) ==
	      AIUS_OTA_OK);
	CHECK(aius_ota_activate_check_image_length(
		      AIUS_OTA_FLASH_USABLE_BYTES) == AIUS_OTA_OK);
	CHECK(aius_ota_activate_check_image_length(
		      AIUS_OTA_FLASH_USABLE_BYTES + 1u) == AIUS_OTA_GEOMETRY);

	current_key = "geometry.step-bound";
	/* Hand-computed for the awkward image: 1 head erase + 3 body erases +
	 * 35 body program-and-compare chunks + 16 head program-and-compare
	 * chunks. The mark and the verification are begin()'s work, not steps. */
	CHECK(aius_ota_activate_step_bound(TEST_IMAGE_BYTES) == 55u);
	/* The minimum image: 1 head erase + 1 body erase + 1 one-byte body chunk
	 * + 16 head chunks. */
	CHECK(aius_ota_activate_step_bound(TEST_MIN_IMAGE_BYTES) == 19u);
	CHECK(aius_ota_activate_step_bound(0u) == 0u);
	CHECK(aius_ota_activate_step_bound(AIUS_OTA_FLASH_USABLE_BYTES + 1u) ==
	      0u);
}

/* -------------------------------------------------------------------------
 * The happy path, and the ordering it promises
 * ---------------------------------------------------------------------- */

static void assert_flash_holds_image(const FakeDevice *dev,
	                             const uint8_t *image, uint32_t length)
{
	uint32_t sector_end;

	CHECK(memcmp(dev->flash, image, length) == 0);
	/* The tail of the last sector was erased and never written. */
	sector_end = ((length + AIUS_OTA_FLASH_SECTOR_BYTES - 1u) /
		      AIUS_OTA_FLASH_SECTOR_BYTES) *
		     AIUS_OTA_FLASH_SECTOR_BYTES;
	CHECK(region_is_erased(dev, length, sector_end - length));
	/* Nothing above the image was touched. */
	CHECK(memcmp(&dev->flash[sector_end], &g_old_image[sector_end],
	             FAKE_FLASH_BYTES - sector_end) == 0);
}

static void assert_head_programmed_last(const FakeDevice *dev)
{
	const uint32_t head_low = AIUS_OTA_FLASH_BASE;
	const uint32_t head_high =
		AIUS_OTA_FLASH_BASE + AIUS_OTA_FLASH_HEAD_BYTES;
	const uint32_t body_high = AIUS_OTA_FLASH_BASE + FAKE_FLASH_BYTES;
	uint32_t first_head_program;
	uint32_t index;
	uint32_t head_programs = 0u;
	uint32_t expect_offset = 0u;

	first_head_program =
		first_in_range(dev, OP_PROGRAM, head_low, head_high);
	CHECK(first_head_program < dev->ops);

	/* Everything that touches the body -- every erase, every body program,
	 * every body readback -- happens before the FCB is rewritten. */
	CHECK(last_in_range(dev, OP_ERASE, head_low, body_high) <
	      first_head_program);
	CHECK(last_in_range(dev, OP_PROGRAM, head_high, body_high) <
	      first_head_program);
	CHECK(last_in_range(dev, OP_FLASH_READ, head_high, body_high) <
	      first_head_program);

	/* And the head is programmed as one ascending contiguous run (its
	 * readbacks are interleaved but skipped by the OP_PROGRAM filter). */
	for (index = first_head_program; index < dev->ops; index++) {
		const Op *op = &dev->trace[index];
		if (op->kind != OP_PROGRAM)
			continue;
		CHECK(op->address == head_low + expect_offset);
		CHECK(op->length == AIUS_OTA_ACTIVATE_CHUNK_BYTES);
		expect_offset += op->length;
		head_programs++;
	}
	CHECK(head_programs ==
	      AIUS_OTA_FLASH_HEAD_BYTES / AIUS_OTA_ACTIVATE_CHUNK_BYTES);
	CHECK(expect_offset == AIUS_OTA_FLASH_HEAD_BYTES);
}

static void assert_erase_order(const FakeDevice *dev, uint32_t sectors)
{
	uint32_t index;
	uint32_t seen = 0u;

	for (index = 0u; index < dev->ops; index++) {
		const Op *op = &dev->trace[index];
		if (op->kind != OP_ERASE)
			continue;
		/* Sector 0 first, then ascending, each exactly once. */
		CHECK(op->address ==
		      AIUS_OTA_FLASH_BASE + seen * AIUS_OTA_FLASH_SECTOR_BYTES);
		seen++;
	}
	CHECK(seen == sectors);
}

static void test_happy_path_to_ready_to_reset(void)
{
	AiusOtaSession session;
	AiusOtaActivator activator;
	AiusOtaActivatePrimitives primitives;
	AiusOtaActivateProgress progress;
	AiusOtaHealth health;

	current_key = "happy.run";
	device_reset(&g_dev, g_new_image, TEST_IMAGE_BYTES);
	primitives = primitives_over(&g_dev);
	request_activation(&session, g_new_image, TEST_IMAGE_BYTES);

	/* begin() does the whole verify + the mark and lands POISED at the first
	 * dark step: the erase that follows is inside the window. */
	CHECK(begin_over(&activator, &session, &primitives, TEST_IMAGE_BYTES,
	                 g_old_digest) == AIUS_OTA_OK);
	CHECK(aius_ota_activate_phase(&activator) ==
	      AIUS_OTA_ACTIVATE_ERASE_HEAD);
	CHECK(aius_ota_activate_is_dark(&activator));

	CHECK(aius_ota_activate_run(&activator) == AIUS_OTA_OK);
	CHECK(aius_ota_activate_phase(&activator) ==
	      AIUS_OTA_ACTIVATE_READY_TO_RESET);
	CHECK(!aius_ota_activate_is_dark(&activator));

	current_key = "happy.progress";
	CHECK(aius_ota_activate_progress(&activator, &progress) == AIUS_OTA_OK);
	CHECK(progress.fault == AIUS_OTA_ACTIVATE_FAULT_NONE);
	CHECK(progress.primitive_fault == 0);
	CHECK(progress.image_length == TEST_IMAGE_BYTES);
	CHECK(progress.programmed_bytes == TEST_IMAGE_BYTES);
	CHECK(progress.verified_bytes == TEST_IMAGE_BYTES);
	CHECK(progress.steps_total == 55u);
	CHECK(progress.steps_taken == progress.steps_total);
	/* Every step ran inside the dark window (the mark is begin()'s, not a
	 * step), and the counter wrapped partway through -- the 64-bit
	 * accumulation of 32-bit differences is what makes this survive that. */
	CHECK(progress.max_step_cycles == TEST_CYCLES_PER_CALL);
	CHECK(progress.dark_window_cycles ==
	      (uint64_t)TEST_CYCLES_PER_CALL * 55u);
	CHECK(g_dev.cycles < TEST_CYCLES_START); /* it really did wrap */

	current_key = "happy.flash";
	assert_flash_holds_image(&g_dev, g_new_image, TEST_IMAGE_BYTES);
	CHECK(g_dev.dirty_programs == 0u);
	CHECK(g_dev.erases == 4u);
	CHECK(g_dev.programs == 35u + 16u);
	CHECK(g_dev.flash_reads == 35u + 16u);

	current_key = "happy.ordering";
	/* The whole prologue, in order: the mark is written before anything is
	 * erased, and the FCB is the first thing erased. */
	CHECK(g_dev.trace[0].kind == OP_FEED);
	CHECK(g_dev.trace[1].kind == OP_HEALTH_READ);
	CHECK(g_dev.trace[2].kind == OP_FEED);
	CHECK(g_dev.trace[3].kind == OP_HEALTH_WRITE);
	CHECK(g_dev.trace[4].kind == OP_FEED);
	CHECK(g_dev.trace[5].kind == OP_ERASE);
	CHECK(g_dev.trace[5].address == AIUS_OTA_FLASH_BASE);
	CHECK(count_of(&g_dev, OP_HEALTH_WRITE) == 1u);
	assert_erase_order(&g_dev, 4u);
	assert_head_programmed_last(&g_dev);
	assert_feed_cadence(&g_dev);

	current_key = "happy.health";
	load_health(&g_dev, &health);
	CHECK(health.state == AIUS_OTA_HEALTH_TRIAL);
	CHECK(memcmp(health.image_digest, g_new_digest,
	             AIUS_OTA_DIGEST_BYTES) == 0);
	CHECK(memcmp(health.previous_digest, g_old_digest,
	             AIUS_OTA_DIGEST_BYTES) == 0);
	CHECK(health.trial_boots_budget == AIUS_OTA_HEALTH_DEFAULT_TRIAL_BOOTS);
	CHECK(health.trial_boots_remaining ==
	      AIUS_OTA_HEALTH_DEFAULT_TRIAL_BOOTS);

	current_key = "happy.terminal";
	/* READY_TO_RESET is terminal: the reset is the caller's, and there is
	 * nothing else this sequencer will do. */
	CHECK(aius_ota_activate_step(&activator) == AIUS_OTA_STATE);
	CHECK(aius_ota_activate_run(&activator) == AIUS_OTA_OK);
	CHECK(g_dev.programs == 35u + 16u);
	/* The session stays ACTIVATION_REQUESTED: only a reset leaves it. */
	CHECK(aius_ota_state(&session) ==
	      AIUS_OTA_STATE_ACTIVATION_REQUESTED);
}

static void test_smallest_admissible_image(void)
{
	AiusOtaSession session;
	AiusOtaActivator activator;
	AiusOtaActivatePrimitives primitives;
	AiusOtaActivateProgress progress;

	current_key = "happy.minimum-image";
	device_reset(&g_dev, g_new_image, TEST_MIN_IMAGE_BYTES);
	primitives = primitives_over(&g_dev);
	request_activation(&session, g_new_image, TEST_MIN_IMAGE_BYTES);

	CHECK(begin_over(&activator, &session, &primitives, TEST_MIN_IMAGE_BYTES,
	                 g_old_digest) == AIUS_OTA_OK);
	CHECK(aius_ota_activate_run(&activator) == AIUS_OTA_OK);
	CHECK(aius_ota_activate_progress(&activator, &progress) == AIUS_OTA_OK);
	CHECK(progress.steps_taken == 19u);
	CHECK(progress.programmed_bytes == TEST_MIN_IMAGE_BYTES);
	assert_flash_holds_image(&g_dev, g_new_image, TEST_MIN_IMAGE_BYTES);
	assert_erase_order(&g_dev, 2u);
	assert_head_programmed_last(&g_dev);
	assert_feed_cadence(&g_dev);
	/* The whole body is one byte, programmed by one 1-byte program. */
	CHECK(g_dev.programs == 1u + 16u);
}

/* -------------------------------------------------------------------------
 * Power loss at every step boundary
 * ---------------------------------------------------------------------- */

static void test_power_loss_at_every_step_boundary(void)
{
	uint8_t record[AIUS_OTA_RECORD_BYTES];
	uint32_t steps = aius_ota_activate_step_bound(TEST_IMAGE_BYTES);
	uint32_t k;

	current_key = "power-loss.every-boundary";
	/* k == 0 is the boundary right after begin(): the mark is written and
	 * the phase is ERASE_HEAD, but nothing is erased yet. */
	for (k = 0u; k <= steps; k++) {
		AiusOtaActivatePrimitives primitives;
		AiusOtaActivator activator;
		AiusOtaActivator resumed;
		AiusOtaSession session;
		AiusOtaHealth health;
		AiusOtaActivatePhase phase;
		uint32_t step;

		device_reset(&g_dev, g_new_image, TEST_IMAGE_BYTES);
		primitives = primitives_over(&g_dev);
		request_activation(&session, g_new_image, TEST_IMAGE_BYTES);
		/* What the composition persists at the moment activation is
		 * requested; the power loss is simulated after k steps by
		 * simply not calling step() again. */
		CHECK(aius_ota_record_save(&session, record, sizeof(record)) ==
		      AIUS_OTA_OK);

		CHECK(begin_over(&activator, &session, &primitives,
		                 TEST_IMAGE_BYTES, g_old_digest) ==
		      AIUS_OTA_OK);
		for (step = 0u; step < k; step++)
			CHECK(aius_ota_activate_step(&activator) ==
			      AIUS_OTA_OK);
		phase = aius_ota_activate_phase(&activator);

		/* Until the head is reprogrammed the FCB is gone, so the ROM
		 * cannot boot ANYTHING -- whole image or half. That is the
		 * designed outcome: HalfKay is a separate chip and survives.
		 * The one boundary where the old image is still whole is the
		 * one before the first erase, which is where begin() lands:
		 * ERASE_HEAD, the mark written, nothing erased. */
		if (phase == AIUS_OTA_ACTIVATE_ERASE_HEAD) {
			CHECK(memcmp(g_dev.flash, g_old_image,
			             FAKE_FLASH_BYTES) == 0);
		} else {
			CHECK(memcmp(g_dev.flash, g_old_image,
			             AIUS_OTA_FLASH_HEAD_BYTES) != 0);
			if (phase < AIUS_OTA_ACTIVATE_PROGRAM_HEAD)
				CHECK(region_is_erased(
					&g_dev, 0u,
					AIUS_OTA_FLASH_HEAD_BYTES));
		}

		/* The reboot. The record names the interrupted activation:
		 * TRIAL for an image that is not the one running. */
		load_health(&g_dev, &health);
		CHECK(health.state == AIUS_OTA_HEALTH_TRIAL);
		CHECK(memcmp(health.image_digest, g_new_digest,
		             AIUS_OTA_DIGEST_BYTES) == 0);
		CHECK(memcmp(health.previous_digest, g_old_digest,
		             AIUS_OTA_DIGEST_BYTES) == 0);

		memset(&session, 0, sizeof(session));
		CHECK(aius_ota_init(&session, TEST_TARGET_ID,
		                    strlen(TEST_TARGET_ID)) == AIUS_OTA_OK);
		CHECK(aius_ota_latch_arm(&session) == AIUS_OTA_OK);
		CHECK(aius_ota_record_load(&session, record, sizeof(record)) ==
		      AIUS_OTA_OK);
		CHECK(aius_ota_state(&session) ==
		      AIUS_OTA_STATE_ACTIVATION_REQUESTED);

		/* And the sequencer refuses to resume into it. */
		CHECK(begin_over(&resumed, &session, &primitives,
		                 TEST_IMAGE_BYTES, g_old_digest) ==
		      AIUS_OTA_INTERRUPTED);
		CHECK(aius_ota_activate_phase(&resumed) ==
		      AIUS_OTA_ACTIVATE_FAULTED);
		CHECK(aius_ota_activate_step(&resumed) == AIUS_OTA_STATE);

		/* The way out is deliberate and destructive of the staged
		 * image: abandon, then stage again (or rescue over HalfKay). */
		CHECK(aius_ota_abandon_activation(&session) == AIUS_OTA_OK);
		CHECK(aius_ota_state(&session) == AIUS_OTA_STATE_IDLE);
		CHECK(begin_over(&resumed, &session, &primitives,
		                 TEST_IMAGE_BYTES, g_old_digest) ==
		      AIUS_OTA_STATE);
	}
}

static void test_resume_refusal_permits_the_running_trial_image(void)
{
	AiusOtaSession session;
	AiusOtaActivator activator;
	AiusOtaActivatePrimitives primitives;
	uint8_t unknown[AIUS_OTA_DIGEST_BYTES];

	/* The discriminating control for the resume refusal: the same record
	 * state -- TRIAL naming the image about to be activated -- is NOT a
	 * refusal when that image is the one running, because then nothing is
	 * half-written. Without this the refusal could be unconditional and the
	 * suite would not notice. */
	current_key = "power-loss.same-image-running";
	device_reset(&g_dev, g_new_image, TEST_IMAGE_BYTES);
	primitives = primitives_over(&g_dev);
	request_activation(&session, g_new_image, TEST_IMAGE_BYTES);

	/* begin() writes the mark (TRIAL new) and lands at ERASE_HEAD. */
	CHECK(begin_over(&activator, &session, &primitives, TEST_IMAGE_BYTES,
	                 g_old_digest) == AIUS_OTA_OK);
	CHECK(aius_ota_activate_phase(&activator) ==
	      AIUS_OTA_ACTIVATE_ERASE_HEAD);

	/* Re-beginning with the new image itself running is permitted: the
	 * record and the running image agree and nothing is half-written. */
	CHECK(begin_over(&activator, &session, &primitives, TEST_IMAGE_BYTES,
	                 g_new_digest) == AIUS_OTA_OK);
	CHECK(aius_ota_activate_phase(&activator) ==
	      AIUS_OTA_ACTIVATE_ERASE_HEAD);

	/* An unknown running image -- the all-zero spelling -- is fail-closed,
	 * not permissive. */
	current_key = "power-loss.unknown-running-image";
	memset(unknown, 0, sizeof(unknown));
	CHECK(begin_over(&activator, &session, &primitives, TEST_IMAGE_BYTES,
	                 unknown) == AIUS_OTA_INTERRUPTED);
	CHECK(begin_over(&activator, &session, &primitives, TEST_IMAGE_BYTES,
	                 g_old_digest) == AIUS_OTA_INTERRUPTED);
}

/* -------------------------------------------------------------------------
 * Primitive faults
 * ---------------------------------------------------------------------- */

typedef struct {
	const char *key;
	uint32_t *injection_site;
	uint32_t nth;
	AiusOtaActivateFault expect_fault;
	int expect_primitive_fault;
	bool expect_flash_touched;
	/* Whether the RECORD changed, which is not the same question as whether
	 * the write was attempted: the health-write fault case attempts it and
	 * leaves the record standing. */
	bool expect_health_written;
	uint32_t expect_health_write_ops;
} FaultCase;

static void test_primitive_fault_paths(void)
{
	unsigned int index;

	for (index = 0u; index < 5u; index++) {
		AiusOtaSession session;
		AiusOtaActivator activator;
		AiusOtaActivatePrimitives primitives;
		AiusOtaActivateProgress progress;
		AiusOtaHealth health;
		FaultCase cases[5];
		FaultCase *item;
		AiusOtaStatus status;
		uint32_t ops_at_halt;

		device_reset(&g_dev, g_new_image, TEST_IMAGE_BYTES);
		primitives = primitives_over(&g_dev);

		/* health-read and health-write both fault inside begin(), before
		 * the dark window: nothing is erased. The read fault never
		 * reaches the mark; the write fault attempts it and leaves the
		 * record standing. */
		cases[0].key = "fault.health-read";
		cases[0].injection_site = &g_dev.fail_health_read_at;
		cases[0].nth = 1u;
		cases[0].expect_fault = AIUS_OTA_ACTIVATE_FAULT_HEALTH_READ;
		cases[0].expect_primitive_fault = -9;
		cases[0].expect_flash_touched = false;
		cases[0].expect_health_written = false;
		cases[0].expect_health_write_ops = 0u;

		cases[1].key = "fault.health-write";
		cases[1].injection_site = &g_dev.fail_health_write_at;
		cases[1].nth = 1u;
		cases[1].expect_fault = AIUS_OTA_ACTIVATE_FAULT_HEALTH_WRITE;
		cases[1].expect_primitive_fault = -10;
		cases[1].expect_flash_touched = false;
		cases[1].expect_health_written = false;
		cases[1].expect_health_write_ops = 1u;

		cases[2].key = "fault.erase";
		cases[2].injection_site = &g_dev.fail_erase_at;
		cases[2].nth = 2u; /* the first body sector; erase 1 is the head */
		cases[2].expect_fault = AIUS_OTA_ACTIVATE_FAULT_ERASE;
		cases[2].expect_primitive_fault = -6;
		cases[2].expect_flash_touched = true;
		cases[2].expect_health_written = true;
		cases[2].expect_health_write_ops = 1u;

		cases[3].key = "fault.program";
		cases[3].injection_site = &g_dev.fail_program_at;
		cases[3].nth = 1u; /* the first body chunk */
		cases[3].expect_fault = AIUS_OTA_ACTIVATE_FAULT_PROGRAM;
		cases[3].expect_primitive_fault = -7;
		cases[3].expect_flash_touched = true;
		cases[3].expect_health_written = true;
		cases[3].expect_health_write_ops = 1u;

		cases[4].key = "fault.flash-read";
		cases[4].injection_site = &g_dev.fail_flash_read_at;
		cases[4].nth = 1u; /* the first body readback */
		cases[4].expect_fault = AIUS_OTA_ACTIVATE_FAULT_FLASH_READ;
		cases[4].expect_primitive_fault = -8;
		cases[4].expect_flash_touched = true;
		cases[4].expect_health_written = true;
		cases[4].expect_health_write_ops = 1u;

		item = &cases[index];
		current_key = item->key;
		*item->injection_site = item->nth;

		request_activation(&session, g_new_image, TEST_IMAGE_BYTES);
		status = begin_over(&activator, &session, &primitives,
		                    TEST_IMAGE_BYTES, g_old_digest);
		if (status == AIUS_OTA_OK)
			status = aius_ota_activate_run(&activator);

		CHECK(status == AIUS_OTA_PRIMITIVE);
		CHECK(aius_ota_activate_phase(&activator) ==
		      AIUS_OTA_ACTIVATE_FAULTED);
		CHECK(aius_ota_activate_progress(&activator, &progress) ==
		      AIUS_OTA_OK);
		CHECK(progress.fault == item->expect_fault);
		CHECK(progress.primitive_fault == item->expect_primitive_fault);

		/* The halt is a halt: no retry, and not one further primitive
		 * call from either entry point. */
		ops_at_halt = g_dev.ops;
		CHECK(aius_ota_activate_step(&activator) == AIUS_OTA_STATE);
		CHECK(aius_ota_activate_run(&activator) == AIUS_OTA_STATE);
		CHECK(g_dev.ops == ops_at_halt);

		/* Nothing is erased before the mark is durably written, so a
		 * fault at or before the mark leaves a bootable board. */
		if (item->expect_flash_touched) {
			CHECK(count_of(&g_dev, OP_ERASE) > 0u);
			CHECK(region_is_erased(&g_dev, 0u,
			                       AIUS_OTA_FLASH_HEAD_BYTES));
		} else {
			CHECK(count_of(&g_dev, OP_ERASE) == 0u);
			CHECK(count_of(&g_dev, OP_PROGRAM) == 0u);
			CHECK(memcmp(g_dev.flash, g_old_image,
			             FAKE_FLASH_BYTES) == 0);
		}

		load_health(&g_dev, &health);
		if (item->expect_health_written) {
			/* The mark stands: an activation of this image began
			 * and did not complete, which is the true statement and
			 * the one the next boot resolves. */
			CHECK(health.state == AIUS_OTA_HEALTH_TRIAL);
			CHECK(memcmp(health.image_digest, g_new_digest,
			             AIUS_OTA_DIGEST_BYTES) == 0);
		} else {
			CHECK(health.state == AIUS_OTA_HEALTH_COMMITTED);
			CHECK(memcmp(health.image_digest, g_old_digest,
			             AIUS_OTA_DIGEST_BYTES) == 0);
		}
		CHECK(count_of(&g_dev, OP_HEALTH_WRITE) ==
		      item->expect_health_write_ops);
		assert_feed_cadence(&g_dev);
	}
}

/* -------------------------------------------------------------------------
 * Verification refusals
 * ---------------------------------------------------------------------- */

static void test_in_window_compare_catches_a_flash_cell_that_did_not_take(void)
{
	AiusOtaSession session;
	AiusOtaActivator activator;
	AiusOtaActivatePrimitives primitives;
	AiusOtaActivateProgress progress;

	current_key = "verify.body-readback";
	device_reset(&g_dev, g_new_image, TEST_IMAGE_BYTES);
	/* The program reports success and one BODY byte lands wrong: exactly
	 * what the in-window byte-compare exists to catch, and nothing else
	 * would, because the staged copy digested clean before the window. */
	g_dev.strict_program = false;
	g_dev.corrupt_enabled = true;
	g_dev.corrupt_offset = 9000u;
	primitives = primitives_over(&g_dev);
	request_activation(&session, g_new_image, TEST_IMAGE_BYTES);

	CHECK(begin_over(&activator, &session, &primitives, TEST_IMAGE_BYTES,
	                 g_old_digest) == AIUS_OTA_OK);
	CHECK(aius_ota_activate_run(&activator) == AIUS_OTA_VERIFY);
	CHECK(aius_ota_activate_progress(&activator, &progress) == AIUS_OTA_OK);
	CHECK(progress.fault == AIUS_OTA_ACTIVATE_FAULT_VERIFY);
	CHECK(progress.phase == AIUS_OTA_ACTIVATE_FAULTED);
	/* Caught mid-body, before the FCB was rewritten, so the board is a
	 * HalfKay case rather than a board that boots a corrupt image. */
	CHECK(region_is_erased(&g_dev, 0u, AIUS_OTA_FLASH_HEAD_BYTES));
	CHECK(first_in_range(&g_dev, OP_PROGRAM, AIUS_OTA_FLASH_BASE,
	                     AIUS_OTA_FLASH_BASE + AIUS_OTA_FLASH_HEAD_BYTES) ==
	      g_dev.ops);
}

static void test_head_compare_catches_a_bad_fcb(void)
{
	AiusOtaSession session;
	AiusOtaActivator activator;
	AiusOtaActivatePrimitives primitives;
	AiusOtaActivateProgress progress;

	current_key = "verify.head-readback";
	device_reset(&g_dev, g_new_image, TEST_IMAGE_BYTES);
	g_dev.strict_program = false;
	g_dev.corrupt_enabled = true;
	g_dev.corrupt_offset = 100u; /* inside the FCB, programmed last */
	primitives = primitives_over(&g_dev);
	request_activation(&session, g_new_image, TEST_IMAGE_BYTES);

	CHECK(begin_over(&activator, &session, &primitives, TEST_IMAGE_BYTES,
	                 g_old_digest) == AIUS_OTA_OK);
	CHECK(aius_ota_activate_run(&activator) == AIUS_OTA_VERIFY);
	CHECK(aius_ota_activate_progress(&activator, &progress) == AIUS_OTA_OK);
	/* The body pass could not have seen this -- the head was still erased
	 * -- so the head's own program-and-compare is the only thing that can
	 * convict, and it does, with the head already partly written. */
	CHECK(progress.fault == AIUS_OTA_ACTIVATE_FAULT_VERIFY);
	CHECK(first_in_range(&g_dev, OP_PROGRAM, AIUS_OTA_FLASH_BASE,
	                     AIUS_OTA_FLASH_BASE + AIUS_OTA_FLASH_HEAD_BYTES) <
	      g_dev.ops);
}

static void test_digest_mismatch_aborts_before_any_flash(void)
{
	AiusOtaSession session;
	AiusOtaActivator activator;
	AiusOtaActivatePrimitives primitives;
	AiusOtaActivateProgress progress;

	current_key = "verify.digest-mismatch";
	device_reset(&g_dev, g_new_image, TEST_IMAGE_BYTES);
	primitives = primitives_over(&g_dev);
	request_activation(&session, g_new_image, TEST_IMAGE_BYTES);

	/* The staging medium decays AFTER the image was staged, verified and
	 * armed. In the reworked contract the whole image is re-digested in
	 * begin(), BEFORE the dark window, so the mismatch aborts with nothing
	 * erased, no mark written, and the board still bootable. */
	g_staged[7000u] ^= 0x01u;

	CHECK(begin_over(&activator, &session, &primitives, TEST_IMAGE_BYTES,
	                 g_old_digest) == AIUS_OTA_DIGEST);
	CHECK(aius_ota_activate_progress(&activator, &progress) == AIUS_OTA_OK);
	CHECK(progress.fault == AIUS_OTA_ACTIVATE_FAULT_DIGEST);
	CHECK(progress.phase == AIUS_OTA_ACTIVATE_FAULTED);
	CHECK(count_of(&g_dev, OP_ERASE) == 0u);
	CHECK(count_of(&g_dev, OP_HEALTH_WRITE) == 0u);
	CHECK(memcmp(g_dev.flash, g_old_image, FAKE_FLASH_BYTES) == 0);
}

static void test_signature_refusal_aborts_before_any_flash(void)
{
	AiusOtaSession session;
	AiusOtaActivator activator;
	AiusOtaActivatePrimitives primitives;
	AiusOtaActivateProgress progress;
	AiusOtaVerifier reject = { NULL, rejecting_verify };

	current_key = "verify.signature-refusal";
	device_reset(&g_dev, g_new_image, TEST_IMAGE_BYTES);
	primitives = primitives_over(&g_dev);
	request_activation(&session, g_new_image, TEST_IMAGE_BYTES);

	/* The digest of the staged copy is clean, but the injected verifier
	 * refuses the header's authentication digest. The re-verify runs in
	 * begin(), so this too aborts before the dark window with nothing
	 * flashed, and latches the verifier's own return value. */
	CHECK(aius_ota_activate_begin(&activator, &session, &primitives,
	                              &reject, g_staged, TEST_IMAGE_BYTES,
	                              g_old_digest) == AIUS_OTA_SIGNATURE);
	CHECK(aius_ota_activate_progress(&activator, &progress) == AIUS_OTA_OK);
	CHECK(progress.fault == AIUS_OTA_ACTIVATE_FAULT_SIGNATURE);
	CHECK(progress.primitive_fault == -42);
	CHECK(progress.phase == AIUS_OTA_ACTIVATE_FAULTED);
	CHECK(count_of(&g_dev, OP_ERASE) == 0u);
	CHECK(count_of(&g_dev, OP_HEALTH_WRITE) == 0u);
	CHECK(memcmp(g_dev.flash, g_old_image, FAKE_FLASH_BYTES) == 0);
}

/* -------------------------------------------------------------------------
 * Preconditions and the session interlock
 * ---------------------------------------------------------------------- */

static void test_begin_preconditions(void)
{
	AiusOtaSession session;
	AiusOtaActivator activator;
	AiusOtaActivatePrimitives primitives;
	AiusOtaActivatePrimitives incomplete;
	uint8_t wire[AIUS_OTA_HEADER_BYTES];

	current_key = "begin.arguments";
	device_reset(&g_dev, g_new_image, TEST_IMAGE_BYTES);
	primitives = primitives_over(&g_dev);
	request_activation(&session, g_new_image, TEST_IMAGE_BYTES);

	CHECK(aius_ota_activate_begin(NULL, &session, &primitives, &g_accept,
	                              g_staged, TEST_IMAGE_BYTES,
	                              g_old_digest) == AIUS_OTA_INVALID_ARG);
	CHECK(aius_ota_activate_begin(&activator, NULL, &primitives, &g_accept,
	                              g_staged, TEST_IMAGE_BYTES,
	                              g_old_digest) == AIUS_OTA_INVALID_ARG);
	CHECK(aius_ota_activate_begin(&activator, &session, NULL, &g_accept,
	                              g_staged, TEST_IMAGE_BYTES,
	                              g_old_digest) == AIUS_OTA_INVALID_ARG);
	/* No verifier is never treated as an accepting one. */
	CHECK(aius_ota_activate_begin(&activator, &session, &primitives, NULL,
	                              g_staged, TEST_IMAGE_BYTES,
	                              g_old_digest) == AIUS_OTA_INVALID_ARG);
	/* No staged view: there is nothing to program from or verify. */
	CHECK(aius_ota_activate_begin(&activator, &session, &primitives,
	                              &g_accept, NULL, TEST_IMAGE_BYTES,
	                              g_old_digest) == AIUS_OTA_INVALID_ARG);
	CHECK(aius_ota_activate_begin(&activator, &session, &primitives,
	                              &g_accept, g_staged, TEST_IMAGE_BYTES,
	                              NULL) == AIUS_OTA_INVALID_ARG);
	/* A refused begin leaves an activator that refuses everything. */
	CHECK(aius_ota_activate_phase(&activator) == AIUS_OTA_ACTIVATE_IDLE);
	CHECK(aius_ota_activate_step(&activator) == AIUS_OTA_STATE);
	CHECK(aius_ota_activate_run(&activator) == AIUS_OTA_STATE);

	current_key = "begin.every-callback-required";
	incomplete = primitives;
	incomplete.watchdog_feed = NULL;
	CHECK(begin_over(&activator, &session, &incomplete, TEST_IMAGE_BYTES,
	                 g_old_digest) == AIUS_OTA_INVALID_ARG);
	incomplete = primitives;
	incomplete.cycles = NULL;
	CHECK(begin_over(&activator, &session, &incomplete, TEST_IMAGE_BYTES,
	                 g_old_digest) == AIUS_OTA_INVALID_ARG);
	incomplete = primitives;
	incomplete.health_write = NULL;
	CHECK(begin_over(&activator, &session, &incomplete, TEST_IMAGE_BYTES,
	                 g_old_digest) == AIUS_OTA_INVALID_ARG);
	incomplete = primitives;
	incomplete.flash_read = NULL;
	CHECK(begin_over(&activator, &session, &incomplete, TEST_IMAGE_BYTES,
	                 g_old_digest) == AIUS_OTA_INVALID_ARG);
	CHECK(g_dev.ops == 0u);

	current_key = "begin.staged-length-mismatch";
	/* A caller-declared staged length that is not the image length would
	 * program past the staged view or leave the image short: STATE. */
	CHECK(aius_ota_activate_begin(&activator, &session, &primitives,
	                              &g_accept, g_staged,
	                              TEST_IMAGE_BYTES - 1u,
	                              g_old_digest) == AIUS_OTA_STATE);
	CHECK(g_dev.ops == 0u);

	current_key = "begin.session-state";
	/* ARMED is one transition short of ACTIVATION_REQUESTED. */
	memset(&session, 0, sizeof(session));
	CHECK(aius_ota_init(&session, TEST_TARGET_ID,
	                    strlen(TEST_TARGET_ID)) == AIUS_OTA_OK);
	CHECK(aius_ota_latch_arm(&session) == AIUS_OTA_OK);
	CHECK(begin_over(&activator, &session, &primitives, TEST_IMAGE_BYTES,
	                 g_old_digest) == AIUS_OTA_STATE);
	build_wire(wire, g_new_image, TEST_IMAGE_BYTES);
	CHECK(aius_ota_offer(&session, wire, sizeof(wire)) == AIUS_OTA_OK);
	CHECK(begin_over(&activator, &session, &primitives, TEST_IMAGE_BYTES,
	                 g_old_digest) == AIUS_OTA_STATE);

	current_key = "begin.disarmed";
	request_activation(&session, g_new_image, TEST_IMAGE_BYTES);
	CHECK(aius_ota_latch_disarm(&session) == AIUS_OTA_OK);
	/* Disarm deliberately leaves ACTIVATION_REQUESTED standing, so this is
	 * the latch refusing rather than the state. */
	CHECK(aius_ota_state(&session) ==
	      AIUS_OTA_STATE_ACTIVATION_REQUESTED);
	CHECK(begin_over(&activator, &session, &primitives, TEST_IMAGE_BYTES,
	                 g_old_digest) == AIUS_OTA_DISARMED);
	CHECK(g_dev.ops == 0u);
}

static void test_begin_refuses_an_image_the_geometry_forbids(void)
{
	AiusOtaSession session;
	AiusOtaActivator activator;
	AiusOtaActivatePrimitives primitives;
	AiusOtaActivateProgress progress;

	current_key = "begin.image-too-short";
	device_reset(&g_dev, g_new_image, AIUS_OTA_FLASH_IVT_OFFSET);
	primitives = primitives_over(&g_dev);
	/* A well-formed, correctly signed container the flash geometry still
	 * refuses: it does not reach its own IVT. */
	request_activation(&session, g_new_image, AIUS_OTA_FLASH_IVT_OFFSET);

	CHECK(begin_over(&activator, &session, &primitives,
	                 AIUS_OTA_FLASH_IVT_OFFSET, g_old_digest) ==
	      AIUS_OTA_GEOMETRY);
	CHECK(aius_ota_activate_phase(&activator) == AIUS_OTA_ACTIVATE_FAULTED);
	CHECK(aius_ota_activate_progress(&activator, &progress) == AIUS_OTA_OK);
	CHECK(progress.fault == AIUS_OTA_ACTIVATE_FAULT_GEOMETRY);
	CHECK(count_of(&g_dev, OP_ERASE) == 0u);
	CHECK(count_of(&g_dev, OP_HEALTH_WRITE) == 0u);
}

static void test_session_withdrawn_mid_window(void)
{
	AiusOtaSession session;
	AiusOtaActivator activator;
	AiusOtaActivatePrimitives primitives;
	AiusOtaActivateProgress progress;
	uint32_t ops_at_withdrawal;
	unsigned int step;

	current_key = "interlock.session-withdrawn";
	device_reset(&g_dev, g_new_image, TEST_IMAGE_BYTES);
	primitives = primitives_over(&g_dev);
	request_activation(&session, g_new_image, TEST_IMAGE_BYTES);

	CHECK(begin_over(&activator, &session, &primitives, TEST_IMAGE_BYTES,
	                 g_old_digest) == AIUS_OTA_OK);
	for (step = 0u; step < 8u; step++)
		CHECK(aius_ota_activate_step(&activator) == AIUS_OTA_OK);
	CHECK(aius_ota_activate_phase(&activator) == AIUS_OTA_ACTIVATE_BODY);

	/* The authorization is withdrawn under a live sequence. The sequencer
	 * stops rather than programming flash on it. */
	CHECK(aius_ota_abandon_activation(&session) == AIUS_OTA_OK);
	ops_at_withdrawal = g_dev.ops;
	CHECK(aius_ota_activate_step(&activator) == AIUS_OTA_STATE);
	CHECK(aius_ota_activate_progress(&activator, &progress) == AIUS_OTA_OK);
	CHECK(progress.fault == AIUS_OTA_ACTIVATE_FAULT_SESSION);
	CHECK(progress.phase == AIUS_OTA_ACTIVATE_FAULTED);
	CHECK(g_dev.ops == ops_at_withdrawal);
}

static void test_abandon_activation_is_the_only_exit(void)
{
	AiusOtaSession session;
	uint8_t wire[AIUS_OTA_HEADER_BYTES];

	current_key = "abandon.transitions";
	CHECK(aius_ota_abandon_activation(NULL) == AIUS_OTA_INVALID_ARG);

	memset(&session, 0, sizeof(session));
	CHECK(aius_ota_init(&session, TEST_TARGET_ID,
	                    strlen(TEST_TARGET_ID)) == AIUS_OTA_OK);
	/* Disarmed refuses, as every mutating entry point does. */
	CHECK(aius_ota_abandon_activation(&session) == AIUS_OTA_DISARMED);
	CHECK(aius_ota_latch_arm(&session) == AIUS_OTA_OK);
	/* And it is not a second spelling of abort(): only one state. */
	CHECK(aius_ota_abandon_activation(&session) == AIUS_OTA_STATE);
	build_wire(wire, g_new_image, TEST_IMAGE_BYTES);
	CHECK(aius_ota_offer(&session, wire, sizeof(wire)) == AIUS_OTA_OK);
	CHECK(aius_ota_abandon_activation(&session) == AIUS_OTA_STATE);

	request_activation(&session, g_new_image, TEST_IMAGE_BYTES);
	CHECK(aius_ota_abort(&session) == AIUS_OTA_STATE);
	CHECK(aius_ota_abandon_activation(&session) == AIUS_OTA_OK);
	CHECK(aius_ota_state(&session) == AIUS_OTA_STATE_IDLE);
	CHECK(aius_ota_next_offset(&session) == 0u);
	/* IDLE is a working state, not a dead end: a fresh upgrade can start. */
	CHECK(aius_ota_offer(&session, wire, sizeof(wire)) == AIUS_OTA_OK);
}

int main(void)
{
	fill_pattern(g_new_image, TEST_IMAGE_BYTES, 0x11u);
	fill_pattern(g_old_image, FAKE_FLASH_BYTES, 0x93u);
	aius_sha256(g_new_image, TEST_IMAGE_BYTES, g_new_digest);
	aius_sha256(g_old_image, TEST_IMAGE_BYTES, g_old_digest);

	test_fake_device_discriminates();
	test_geometry_constants_are_exact();
	test_bounds_and_alignment_refusals();
	test_happy_path_to_ready_to_reset();
	test_smallest_admissible_image();
	test_power_loss_at_every_step_boundary();
	test_resume_refusal_permits_the_running_trial_image();
	test_primitive_fault_paths();
	test_in_window_compare_catches_a_flash_cell_that_did_not_take();
	test_head_compare_catches_a_bad_fcb();
	test_digest_mismatch_aborts_before_any_flash();
	test_signature_refusal_aborts_before_any_flash();
	test_begin_preconditions();
	test_begin_refuses_an_image_the_geometry_forbids();
	test_session_withdrawn_mid_window();
	test_abandon_activation_is_the_only_exit();
	puts("ota activation tests passed");
	return 0;
}
