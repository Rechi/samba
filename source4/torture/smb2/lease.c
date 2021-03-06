/*
   Unix SMB/CIFS implementation.

   test suite for SMB2 leases

   Copyright (C) Zachary Loafman 2009

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "includes.h"
#include <tevent.h>
#include "libcli/smb2/smb2.h"
#include "libcli/smb2/smb2_calls.h"
#include "torture/torture.h"
#include "torture/smb2/proto.h"
#include "libcli/smb/smbXcli_base.h"

#define CHECK_VAL(v, correct) do { \
	if ((v) != (correct)) { \
		torture_result(tctx, TORTURE_FAIL, "(%s): wrong value for %s got 0x%x - should be 0x%x\n", \
				__location__, #v, (int)(v), (int)(correct)); \
		ret = false; \
	}} while (0)

#define CHECK_STATUS(status, correct) do { \
	if (!NT_STATUS_EQUAL(status, correct)) { \
		torture_result(tctx, TORTURE_FAIL, __location__": Incorrect status %s - should be %s", \
		       nt_errstr(status), nt_errstr(correct)); \
		ret = false; \
		goto done; \
	}} while (0)

#define CHECK_CREATED(__io, __created, __attribute)			\
	do {								\
		CHECK_VAL((__io)->out.create_action, NTCREATEX_ACTION_ ## __created); \
		CHECK_VAL((__io)->out.alloc_size, 0);			\
		CHECK_VAL((__io)->out.size, 0);				\
		CHECK_VAL((__io)->out.file_attr, (__attribute));	\
		CHECK_VAL((__io)->out.reserved2, 0);			\
	} while(0)

#define CHECK_LEASE(__io, __state, __oplevel, __key)			\
	do {								\
		if (__oplevel) {					\
			CHECK_VAL((__io)->out.oplock_level, SMB2_OPLOCK_LEVEL_LEASE); \
			CHECK_VAL((__io)->out.lease_response.lease_key.data[0], (__key)); \
			CHECK_VAL((__io)->out.lease_response.lease_key.data[1], ~(__key)); \
			CHECK_VAL((__io)->out.lease_response.lease_state, smb2_util_lease_state(__state)); \
		} else {						\
			CHECK_VAL((__io)->out.oplock_level, SMB2_OPLOCK_LEVEL_NONE); \
			CHECK_VAL((__io)->out.lease_response.lease_key.data[0], 0); \
			CHECK_VAL((__io)->out.lease_response.lease_key.data[1], 0); \
			CHECK_VAL((__io)->out.lease_response.lease_state, 0); \
		}							\
									\
		CHECK_VAL((__io)->out.lease_response.lease_flags, 0);	\
		CHECK_VAL((__io)->out.lease_response.lease_duration, 0); \
	} while(0)

#define CHECK_LEASE_V2(__io, __state, __oplevel, __key, __flags)	\
	do {								\
		if (__oplevel) {					\
			CHECK_VAL((__io)->out.oplock_level, SMB2_OPLOCK_LEVEL_LEASE); \
			CHECK_VAL((__io)->out.lease_response_v2.lease_key.data[0], (__key)); \
			CHECK_VAL((__io)->out.lease_response_v2.lease_key.data[1], ~(__key)); \
			CHECK_VAL((__io)->out.lease_response_v2.lease_state, smb2_util_lease_state(__state)); \
		} else {						\
			CHECK_VAL((__io)->out.oplock_level, SMB2_OPLOCK_LEVEL_NONE); \
			CHECK_VAL((__io)->out.lease_response_v2.lease_key.data[0], 0); \
			CHECK_VAL((__io)->out.lease_response_v2.lease_key.data[1], 0); \
			CHECK_VAL((__io)->out.lease_response_v2.lease_state, 0); \
		}							\
									\
		CHECK_VAL((__io)->out.lease_response_v2.lease_flags, __flags); \
		CHECK_VAL((__io)->out.lease_response_v2.lease_duration, 0); \
	} while(0)

static const uint64_t LEASE1 = 0xBADC0FFEE0DDF00Dull;
static const uint64_t LEASE2 = 0xDEADBEEFFEEDBEADull;
static const uint64_t LEASE3 = 0xDAD0FFEDD00DF00Dull;
static const uint64_t LEASE4 = 0xBAD0FFEDD00DF00Dull;

#define NREQUEST_RESULTS 8
static const char *request_results[NREQUEST_RESULTS][2] = {
	{ "", "" },
	{ "R", "R" },
	{ "H", "" },
	{ "W", "" },
	{ "RH", "RH" },
	{ "RW", "RW" },
	{ "HW", "" },
	{ "RHW", "RHW" },
};

static bool test_lease_request(struct torture_context *tctx,
	                       struct smb2_tree *tree)
{
	TALLOC_CTX *mem_ctx = talloc_new(tctx);
	struct smb2_create io;
	struct smb2_lease ls;
	struct smb2_handle h1, h2;
	NTSTATUS status;
	const char *fname = "lease.dat";
	const char *fname2 = "lease2.dat";
	const char *sname = "lease.dat:stream";
	const char *dname = "lease.dir";
	bool ret = true;
	int i;
	uint32_t caps;

	caps = smb2cli_conn_server_capabilities(tree->session->transport->conn);
	if (!(caps & SMB2_CAP_LEASING)) {
		torture_skip(tctx, "leases are not supported");
	}

	smb2_util_unlink(tree, fname);
	smb2_util_unlink(tree, fname2);
	smb2_util_rmdir(tree, dname);

	/* Win7 is happy to grant RHW leases on files. */
	smb2_lease_create(&io, &ls, false, fname, LEASE1, smb2_util_lease_state("RHW"));
	status = smb2_create(tree, mem_ctx, &io);
	CHECK_STATUS(status, NT_STATUS_OK);
	h1 = io.out.file.handle;
	CHECK_CREATED(&io, CREATED, FILE_ATTRIBUTE_ARCHIVE);
	CHECK_LEASE(&io, "RHW", true, LEASE1);

	/* But will reject leases on directories. */
	smb2_lease_create(&io, &ls, true, dname, LEASE2, smb2_util_lease_state("RHW"));
	status = smb2_create(tree, mem_ctx, &io);
	CHECK_STATUS(status, NT_STATUS_OK);
	CHECK_CREATED(&io, CREATED, FILE_ATTRIBUTE_DIRECTORY);
	CHECK_LEASE(&io, "", false, 0);
	smb2_util_close(tree, io.out.file.handle);

	/* Also rejects multiple files leased under the same key. */
	smb2_lease_create(&io, &ls, true, fname2, LEASE1, smb2_util_lease_state("RHW"));
	status = smb2_create(tree, mem_ctx, &io);
	CHECK_STATUS(status, NT_STATUS_INVALID_PARAMETER);

	/* And grants leases on streams (with separate leasekey). */
	smb2_lease_create(&io, &ls, false, sname, LEASE2, smb2_util_lease_state("RHW"));
	status = smb2_create(tree, mem_ctx, &io);
	h2 = io.out.file.handle;
	CHECK_STATUS(status, NT_STATUS_OK);
	CHECK_CREATED(&io, CREATED, FILE_ATTRIBUTE_ARCHIVE);
	CHECK_LEASE(&io, "RHW", true, LEASE2);
	smb2_util_close(tree, h2);

	smb2_util_close(tree, h1);

	/* Now see what combos are actually granted. */
	for (i = 0; i < NREQUEST_RESULTS; i++) {
		torture_comment(tctx, "Requesting lease type %s(%x),"
		    " expecting %s(%x)\n",
		    request_results[i][0], smb2_util_lease_state(request_results[i][0]),
		    request_results[i][1], smb2_util_lease_state(request_results[i][1]));
		smb2_lease_create(&io, &ls, false, fname, LEASE1,
		    smb2_util_lease_state(request_results[i][0]));
		status = smb2_create(tree, mem_ctx, &io);
		h2 = io.out.file.handle;
		CHECK_STATUS(status, NT_STATUS_OK);
		CHECK_CREATED(&io, EXISTED, FILE_ATTRIBUTE_ARCHIVE);
		CHECK_LEASE(&io, request_results[i][1], true, LEASE1);
		smb2_util_close(tree, io.out.file.handle);
	}

 done:
	smb2_util_close(tree, h1);
	smb2_util_close(tree, h2);

	smb2_util_unlink(tree, fname);
	smb2_util_unlink(tree, fname2);
	smb2_util_rmdir(tree, dname);

	talloc_free(mem_ctx);

	return ret;
}

static bool test_lease_upgrade(struct torture_context *tctx,
                               struct smb2_tree *tree)
{
	TALLOC_CTX *mem_ctx = talloc_new(tctx);
	struct smb2_create io;
	struct smb2_lease ls;
	struct smb2_handle h, hnew;
	NTSTATUS status;
	const char *fname = "lease.dat";
	bool ret = true;
	uint32_t caps;

	caps = smb2cli_conn_server_capabilities(tree->session->transport->conn);
	if (!(caps & SMB2_CAP_LEASING)) {
		torture_skip(tctx, "leases are not supported");
	}

	smb2_util_unlink(tree, fname);

	/* Grab a RH lease. */
	smb2_lease_create(&io, &ls, false, fname, LEASE1, smb2_util_lease_state("RH"));
	status = smb2_create(tree, mem_ctx, &io);
	CHECK_STATUS(status, NT_STATUS_OK);
	CHECK_CREATED(&io, CREATED, FILE_ATTRIBUTE_ARCHIVE);
	CHECK_LEASE(&io, "RH", true, LEASE1);
	h = io.out.file.handle;

	/* Upgrades (sidegrades?) to RW leave us with an RH. */
	smb2_lease_create(&io, &ls, false, fname, LEASE1, smb2_util_lease_state("RW"));
	status = smb2_create(tree, mem_ctx, &io);
	CHECK_STATUS(status, NT_STATUS_OK);
	CHECK_CREATED(&io, EXISTED, FILE_ATTRIBUTE_ARCHIVE);
	CHECK_LEASE(&io, "RH", true, LEASE1);
	hnew = io.out.file.handle;

	smb2_util_close(tree, hnew);

	/* Upgrade to RHW lease. */
	smb2_lease_create(&io, &ls, false, fname, LEASE1, smb2_util_lease_state("RHW"));
	status = smb2_create(tree, mem_ctx, &io);
	CHECK_STATUS(status, NT_STATUS_OK);
	CHECK_CREATED(&io, EXISTED, FILE_ATTRIBUTE_ARCHIVE);
	CHECK_LEASE(&io, "RHW", true, LEASE1);
	hnew = io.out.file.handle;

	smb2_util_close(tree, h);
	h = hnew;

	/* Attempt to downgrade - original lease state is maintained. */
	smb2_lease_create(&io, &ls, false, fname, LEASE1, smb2_util_lease_state("RH"));
	status = smb2_create(tree, mem_ctx, &io);
	CHECK_STATUS(status, NT_STATUS_OK);
	CHECK_CREATED(&io, EXISTED, FILE_ATTRIBUTE_ARCHIVE);
	CHECK_LEASE(&io, "RHW", true, LEASE1);
	hnew = io.out.file.handle;

	smb2_util_close(tree, hnew);

 done:
	smb2_util_close(tree, h);
	smb2_util_close(tree, hnew);

	smb2_util_unlink(tree, fname);

	talloc_free(mem_ctx);

	return ret;
}

/**
 * upgrade2 test.
 * full matrix of lease upgrade combinations
 */
struct lease_upgrade2_test {
	const char *initial;
	const char *upgrade_to;
	const char *expected;
};

#define NUM_LEASE_TYPES 5
#define NUM_UPGRADE_TESTS ( NUM_LEASE_TYPES * NUM_LEASE_TYPES )
struct lease_upgrade2_test lease_upgrade2_tests[NUM_UPGRADE_TESTS] = {
	{ "", "", "" },
	{ "", "R", "R" },
	{ "", "RH", "RH" },
	{ "", "RW", "RW" },
	{ "", "RWH", "RWH" },

	{ "R", "", "R" },
	{ "R", "R", "R" },
	{ "R", "RH", "RH" },
	{ "R", "RW", "RW" },
	{ "R", "RWH", "RWH" },

	{ "RH", "", "RH" },
	{ "RH", "R", "RH" },
	{ "RH", "RH", "RH" },
	{ "RH", "RW", "RH" },
	{ "RH", "RWH", "RWH" },

	{ "RW", "", "RW" },
	{ "RW", "R", "RW" },
	{ "RW", "RH", "RW" },
	{ "RW", "RW", "RW" },
	{ "RW", "RWH", "RWH" },

	{ "RWH", "", "RWH" },
	{ "RWH", "R", "RWH" },
	{ "RWH", "RH", "RWH" },
	{ "RWH", "RW", "RWH" },
	{ "RWH", "RWH", "RWH" },
};

static bool test_lease_upgrade2(struct torture_context *tctx,
                                struct smb2_tree *tree)
{
	TALLOC_CTX *mem_ctx = talloc_new(tctx);
	struct smb2_handle h, hnew;
	NTSTATUS status;
	struct smb2_create io;
	struct smb2_lease ls;
	const char *fname = "lease.dat";
	bool ret = true;
	int i;
	uint32_t caps;

	caps = smb2cli_conn_server_capabilities(tree->session->transport->conn);
	if (!(caps & SMB2_CAP_LEASING)) {
		torture_skip(tctx, "leases are not supported");
	}

	for (i = 0; i < NUM_UPGRADE_TESTS; i++) {
		struct lease_upgrade2_test t = lease_upgrade2_tests[i];

		smb2_util_unlink(tree, fname);

		/* Grab a lease. */
		smb2_lease_create(&io, &ls, false, fname, LEASE1, smb2_util_lease_state(t.initial));
		status = smb2_create(tree, mem_ctx, &io);
		CHECK_STATUS(status, NT_STATUS_OK);
		CHECK_CREATED(&io, CREATED, FILE_ATTRIBUTE_ARCHIVE);
		CHECK_LEASE(&io, t.initial, true, LEASE1);
		h = io.out.file.handle;

		/* Upgrade. */
		smb2_lease_create(&io, &ls, false, fname, LEASE1, smb2_util_lease_state(t.upgrade_to));
		status = smb2_create(tree, mem_ctx, &io);
		CHECK_STATUS(status, NT_STATUS_OK);
		CHECK_CREATED(&io, EXISTED, FILE_ATTRIBUTE_ARCHIVE);
		CHECK_LEASE(&io, t.expected, true, LEASE1);
		hnew = io.out.file.handle;

		smb2_util_close(tree, hnew);
		smb2_util_close(tree, h);
	}

 done:
	smb2_util_close(tree, h);
	smb2_util_close(tree, hnew);

	smb2_util_unlink(tree, fname);

	talloc_free(mem_ctx);

	return ret;
}


#define CHECK_LEASE_BREAK(__lb, __oldstate, __state, __key)		\
	do {								\
		CHECK_VAL((__lb)->new_lease_state, smb2_util_lease_state(__state));	\
		CHECK_VAL((__lb)->current_lease.lease_state, smb2_util_lease_state(__oldstate)); \
		CHECK_VAL((__lb)->current_lease.lease_key.data[0], (__key)); \
		CHECK_VAL((__lb)->current_lease.lease_key.data[1], ~(__key)); \
	} while(0)

#define CHECK_LEASE_BREAK_ACK(__lba, __state, __key)			\
	do {								\
		CHECK_VAL((__lba)->out.reserved, 0);			\
		CHECK_VAL((__lba)->out.lease.lease_key.data[0], (__key)); \
		CHECK_VAL((__lba)->out.lease.lease_key.data[1], ~(__key)); \
		CHECK_VAL((__lba)->out.lease.lease_state, smb2_util_lease_state(__state)); \
		CHECK_VAL((__lba)->out.lease.lease_flags, 0);		\
		CHECK_VAL((__lba)->out.lease.lease_duration, 0);	\
	} while(0)

static struct {
	struct smb2_lease_break lease_break;
	struct smb2_lease_break_ack lease_break_ack;
	int count;
	int failures;

	struct smb2_handle oplock_handle;
	uint8_t held_oplock_level;
	uint8_t oplock_level;
	int oplock_count;
	int oplock_failures;
} break_info;

#define CHECK_BREAK_INFO(__oldstate, __state, __key)			\
	do {								\
		CHECK_VAL(break_info.failures, 0);			\
		CHECK_VAL(break_info.count, 1);				\
		CHECK_LEASE_BREAK(&break_info.lease_break, (__oldstate), \
		    (__state), (__key));				\
		if (break_info.lease_break.break_flags &		\
		    SMB2_NOTIFY_BREAK_LEASE_FLAG_ACK_REQUIRED) {	\
			CHECK_LEASE_BREAK_ACK(&break_info.lease_break_ack, \
				              (__state), (__key));	\
		}							\
	} while(0)

static void torture_lease_break_callback(struct smb2_request *req)
{
	NTSTATUS status;

	status = smb2_lease_break_ack_recv(req, &break_info.lease_break_ack);
	if (!NT_STATUS_IS_OK(status))
		break_info.failures++;

	return;
}

/* a lease break request handler */
static bool torture_lease_handler(struct smb2_transport *transport,
				  const struct smb2_lease_break *lb,
				  void *private_data)
{
	struct smb2_tree *tree = private_data;
	struct smb2_lease_break_ack io;
	struct smb2_request *req;

	break_info.lease_break = *lb;
	break_info.count++;

	if (lb->break_flags & SMB2_NOTIFY_BREAK_LEASE_FLAG_ACK_REQUIRED) {
		ZERO_STRUCT(io);
		io.in.lease.lease_key = lb->current_lease.lease_key;
		io.in.lease.lease_state = lb->new_lease_state;

		req = smb2_lease_break_ack_send(tree, &io);
		req->async.fn = torture_lease_break_callback;
		req->async.private_data = NULL;
	}

	return true;
}

/*
   Timer handler function notifies the registering function that time is up
*/
static void timeout_cb(struct tevent_context *ev,
		       struct tevent_timer *te,
		       struct timeval current_time,
		       void *private_data)
{
	bool *timesup = (bool *)private_data;
	*timesup = true;
	return;
}

/*
   Wait a short period of time to receive a single oplock break request
*/
static void torture_wait_for_lease_break(struct torture_context *tctx)
{
	TALLOC_CTX *tmp_ctx = talloc_new(NULL);
	struct tevent_timer *te = NULL;
	struct timeval ne;
	bool timesup = false;
	int old_count = break_info.count;

	/* Wait .1 seconds for an lease break */
	ne = tevent_timeval_current_ofs(0, 100000);

	te = tevent_add_timer(tctx->ev, tmp_ctx, ne, timeout_cb, &timesup);
	if (te == NULL) {
		torture_comment(tctx, "Failed to wait for an oplock break. "
				      "test results may not be accurate.");
		goto done;
	}

	while (!timesup && break_info.count < old_count + 1) {
		if (tevent_loop_once(tctx->ev) != 0) {
			torture_comment(tctx, "Failed to wait for an oplock "
					      "break. test results may not be "
					      "accurate.");
			goto done;
		}
	}

done:
	/* We don't know if the timed event fired and was freed, we received
	 * our oplock break, or some other event triggered the loop.  Thus,
	 * we create a tmp_ctx to be able to safely free/remove the timed
	 * event in all 3 cases. */
	talloc_free(tmp_ctx);

	return;
}

/*
  break_results should be read as "held lease, new lease, hold broken to, new
  grant", i.e. { "RH", "RW", "RH", "R" } means that if key1 holds RH and key2
  tries for RW, key1 will be broken to RH (in this case, not broken at all)
  and key2 will be granted R.

  Note: break_results only includes things that Win7 will actually grant (see
  request_results above).
 */
#define NBREAK_RESULTS 16
static const char *break_results[NBREAK_RESULTS][4] = {
	{"R",	"R",	"R",	"R"},
	{"R",	"RH",	"R",	"RH"},
	{"R",	"RW",	"R",	"R"},
	{"R",	"RHW",	"R",	"RH"},

	{"RH",	"R",	"RH",	"R"},
	{"RH",	"RH",	"RH",	"RH"},
	{"RH",	"RW",	"RH",	"R"},
	{"RH",	"RHW",	"RH",	"RH"},

	{"RW",	"R",	"R",	"R"},
	{"RW",	"RH",	"R",	"RH"},
	{"RW",	"RW",	"R",	"R"},
	{"RW",	"RHW",	"R",	"RH"},

	{"RHW",	"R",	"RH",	"R"},
	{"RHW",	"RH",	"RH",	"RH"},
	{"RHW",	"RW",	"RH",	"R"},
	{"RHW", "RHW",	"RH",	"RH"},
};

static bool test_lease_break(struct torture_context *tctx,
                               struct smb2_tree *tree)
{
	TALLOC_CTX *mem_ctx = talloc_new(tctx);
	struct smb2_create io;
	struct smb2_lease ls;
	struct smb2_handle h, h2, h3;
	NTSTATUS status;
	const char *fname = "lease.dat";
	bool ret = true;
	int i;
	uint32_t caps;

	caps = smb2cli_conn_server_capabilities(tree->session->transport->conn);
	if (!(caps & SMB2_CAP_LEASING)) {
		torture_skip(tctx, "leases are not supported");
	}

	tree->session->transport->lease.handler	= torture_lease_handler;
	tree->session->transport->lease.private_data = tree;

	smb2_util_unlink(tree, fname);

	for (i = 0; i < NBREAK_RESULTS; i++) {
		const char *held = break_results[i][0];
		const char *contend = break_results[i][1];
		const char *brokento = break_results[i][2];
		const char *granted = break_results[i][3];
		torture_comment(tctx, "Hold %s(%x), requesting %s(%x), "
		    "expecting break to %s(%x) and grant of %s(%x)\n",
		    held, smb2_util_lease_state(held), contend, smb2_util_lease_state(contend),
		    brokento, smb2_util_lease_state(brokento), granted, smb2_util_lease_state(granted));

		ZERO_STRUCT(break_info);

		/* Grab lease. */
		smb2_lease_create(&io, &ls, false, fname, LEASE1, smb2_util_lease_state(held));
		status = smb2_create(tree, mem_ctx, &io);
		CHECK_STATUS(status, NT_STATUS_OK);
		h = io.out.file.handle;
		CHECK_CREATED(&io, CREATED, FILE_ATTRIBUTE_ARCHIVE);
		CHECK_LEASE(&io, held, true, LEASE1);

		/* Possibly contend lease. */
		smb2_lease_create(&io, &ls, false, fname, LEASE2, smb2_util_lease_state(contend));
		status = smb2_create(tree, mem_ctx, &io);
		CHECK_STATUS(status, NT_STATUS_OK);
		h2 = io.out.file.handle;
		CHECK_CREATED(&io, EXISTED, FILE_ATTRIBUTE_ARCHIVE);
		CHECK_LEASE(&io, granted, true, LEASE2);

		if (smb2_util_lease_state(held) != smb2_util_lease_state(brokento)) {
			CHECK_BREAK_INFO(held, brokento, LEASE1);
		} else {
			CHECK_VAL(break_info.count, 0);
			CHECK_VAL(break_info.failures, 0);
		}

		ZERO_STRUCT(break_info);

		/*
		  Now verify that an attempt to upgrade LEASE1 results in no
		  break and no change in LEASE1.
		 */
		smb2_lease_create(&io, &ls, false, fname, LEASE1, smb2_util_lease_state("RHW"));
		status = smb2_create(tree, mem_ctx, &io);
		CHECK_STATUS(status, NT_STATUS_OK);
		h3 = io.out.file.handle;
		CHECK_CREATED(&io, EXISTED, FILE_ATTRIBUTE_ARCHIVE);
		CHECK_LEASE(&io, brokento, true, LEASE1);
		CHECK_VAL(break_info.count, 0);
		CHECK_VAL(break_info.failures, 0);

		smb2_util_close(tree, h);
		smb2_util_close(tree, h2);
		smb2_util_close(tree, h3);

		status = smb2_util_unlink(tree, fname);
		CHECK_STATUS(status, NT_STATUS_OK);
	}

 done:
	smb2_util_close(tree, h);
	smb2_util_close(tree, h2);

	smb2_util_unlink(tree, fname);

	talloc_free(mem_ctx);

	return ret;
}

static void torture_oplock_break_callback(struct smb2_request *req)
{
	NTSTATUS status;
	struct smb2_break br;

	ZERO_STRUCT(br);
	status = smb2_break_recv(req, &br);
	if (!NT_STATUS_IS_OK(status))
		break_info.oplock_failures++;

	return;
}

/* a oplock break request handler */
static bool torture_oplock_handler(struct smb2_transport *transport,
				   const struct smb2_handle *handle,
				   uint8_t level, void *private_data)
{
	struct smb2_tree *tree = private_data;
	struct smb2_request *req;
	struct smb2_break br;

	break_info.oplock_handle = *handle;
	break_info.oplock_level	= level;
	break_info.oplock_count++;

	ZERO_STRUCT(br);
	br.in.file.handle = *handle;
	br.in.oplock_level = level;

	if (break_info.held_oplock_level > SMB2_OPLOCK_LEVEL_II) {
		req = smb2_break_send(tree, &br);
		req->async.fn = torture_oplock_break_callback;
		req->async.private_data = NULL;
	}
	break_info.held_oplock_level = level;

	return true;
}

#define NOPLOCK_RESULTS 12
static const char *oplock_results[NOPLOCK_RESULTS][4] = {
	{"R",	"s",	"R",	"s"},
	{"R",	"x",	"R",	"s"},
	{"R",	"b",	"R",	"s"},

	{"RH",	"s",	"RH",	""},
	{"RH",	"x",	"RH",	""},
	{"RH",	"b",	"RH",	""},

	{"RW",	"s",	"R",	"s"},
	{"RW",	"x",	"R",	"s"},
	{"RW",	"b",	"R",	"s"},

	{"RHW",	"s",	"RH",	""},
	{"RHW",	"x",	"RH",	""},
	{"RHW",	"b",	"RH",	""},
};

static const char *oplock_results_2[NOPLOCK_RESULTS][4] = {
	{"s",	"R",	"s",	"R"},
	{"s",	"RH",	"s",	"R"},
	{"s",	"RW",	"s",	"R"},
	{"s",	"RHW",	"s",	"R"},

	{"x",	"R",	"s",	"R"},
	{"x",	"RH",	"s",	"R"},
	{"x",	"RW",	"s",	"R"},
	{"x",	"RHW",	"s",	"R"},

	{"b",	"R",	"s",	"R"},
	{"b",	"RH",	"s",	"R"},
	{"b",	"RW",	"s",	"R"},
	{"b",	"RHW",	"s",	"R"},
};

static bool test_lease_oplock(struct torture_context *tctx,
                              struct smb2_tree *tree)
{
	TALLOC_CTX *mem_ctx = talloc_new(tctx);
	struct smb2_create io;
	struct smb2_lease ls;
	struct smb2_handle h, h2;
	NTSTATUS status;
	const char *fname = "lease.dat";
	bool ret = true;
	int i;
	uint32_t caps;

	caps = smb2cli_conn_server_capabilities(tree->session->transport->conn);
	if (!(caps & SMB2_CAP_LEASING)) {
		torture_skip(tctx, "leases are not supported");
	}

	tree->session->transport->lease.handler	= torture_lease_handler;
	tree->session->transport->lease.private_data = tree;
	tree->session->transport->oplock.handler = torture_oplock_handler;
	tree->session->transport->oplock.private_data = tree;

	smb2_util_unlink(tree, fname);

	for (i = 0; i < NOPLOCK_RESULTS; i++) {
		const char *held = oplock_results[i][0];
		const char *contend = oplock_results[i][1];
		const char *brokento = oplock_results[i][2];
		const char *granted = oplock_results[i][3];
		torture_comment(tctx, "Hold %s(%x), requesting %s(%x), "
		    "expecting break to %s(%x) and grant of %s(%x)\n",
		    held, smb2_util_lease_state(held), contend, smb2_util_oplock_level(contend),
		    brokento, smb2_util_lease_state(brokento), granted, smb2_util_oplock_level(granted));

		ZERO_STRUCT(break_info);

		/* Grab lease. */
		smb2_lease_create(&io, &ls, false, fname, LEASE1, smb2_util_lease_state(held));
		status = smb2_create(tree, mem_ctx, &io);
		CHECK_STATUS(status, NT_STATUS_OK);
		h = io.out.file.handle;
		CHECK_CREATED(&io, CREATED, FILE_ATTRIBUTE_ARCHIVE);
		CHECK_LEASE(&io, held, true, LEASE1);

		/* Does an oplock contend the lease? */
		smb2_oplock_create(&io, fname, smb2_util_oplock_level(contend));
		status = smb2_create(tree, mem_ctx, &io);
		CHECK_STATUS(status, NT_STATUS_OK);
		h2 = io.out.file.handle;
		CHECK_CREATED(&io, EXISTED, FILE_ATTRIBUTE_ARCHIVE);
		CHECK_VAL(io.out.oplock_level, smb2_util_oplock_level(granted));
		break_info.held_oplock_level = io.out.oplock_level;

		if (smb2_util_lease_state(held) != smb2_util_lease_state(brokento)) {
			CHECK_BREAK_INFO(held, brokento, LEASE1);
		} else {
			CHECK_VAL(break_info.count, 0);
			CHECK_VAL(break_info.failures, 0);
		}

		smb2_util_close(tree, h);
		smb2_util_close(tree, h2);

		status = smb2_util_unlink(tree, fname);
		CHECK_STATUS(status, NT_STATUS_OK);
	}

	for (i = 0; i < NOPLOCK_RESULTS; i++) {
		const char *held = oplock_results_2[i][0];
		const char *contend = oplock_results_2[i][1];
		const char *brokento = oplock_results_2[i][2];
		const char *granted = oplock_results_2[i][3];
		torture_comment(tctx, "Hold %s(%x), requesting %s(%x), "
		    "expecting break to %s(%x) and grant of %s(%x)\n",
		    held, smb2_util_oplock_level(held), contend, smb2_util_lease_state(contend),
		    brokento, smb2_util_oplock_level(brokento), granted, smb2_util_lease_state(granted));

		ZERO_STRUCT(break_info);

		/* Grab an oplock. */
		smb2_oplock_create(&io, fname, smb2_util_oplock_level(held));
		status = smb2_create(tree, mem_ctx, &io);
		CHECK_STATUS(status, NT_STATUS_OK);
		h = io.out.file.handle;
		CHECK_CREATED(&io, CREATED, FILE_ATTRIBUTE_ARCHIVE);
		CHECK_VAL(io.out.oplock_level, smb2_util_oplock_level(held));
		break_info.held_oplock_level = io.out.oplock_level;

		/* Grab lease. */
		smb2_lease_create(&io, &ls, false, fname, LEASE1, smb2_util_lease_state(contend));
		status = smb2_create(tree, mem_ctx, &io);
		CHECK_STATUS(status, NT_STATUS_OK);
		h2 = io.out.file.handle;
		CHECK_CREATED(&io, EXISTED, FILE_ATTRIBUTE_ARCHIVE);
		CHECK_LEASE(&io, granted, true, LEASE1);

		if (smb2_util_oplock_level(held) != smb2_util_oplock_level(brokento)) {
			CHECK_VAL(break_info.oplock_count, 1);
			CHECK_VAL(break_info.oplock_failures, 0);
			CHECK_VAL(break_info.oplock_level, smb2_util_oplock_level(brokento));
			break_info.held_oplock_level = break_info.oplock_level;
		} else {
			CHECK_VAL(break_info.oplock_count, 0);
			CHECK_VAL(break_info.oplock_failures, 0);
		}

		smb2_util_close(tree, h);
		smb2_util_close(tree, h2);

		status = smb2_util_unlink(tree, fname);
		CHECK_STATUS(status, NT_STATUS_OK);
	}

 done:
	smb2_util_close(tree, h);
	smb2_util_close(tree, h2);

	smb2_util_unlink(tree, fname);

	talloc_free(mem_ctx);

	return ret;
}

static bool test_lease_multibreak(struct torture_context *tctx,
                                  struct smb2_tree *tree)
{
	TALLOC_CTX *mem_ctx = talloc_new(tctx);
	struct smb2_create io;
	struct smb2_lease ls;
	struct smb2_handle h, h2, h3;
	struct smb2_write w;
	NTSTATUS status;
	const char *fname = "lease.dat";
	bool ret = true;
	uint32_t caps;

	caps = smb2cli_conn_server_capabilities(tree->session->transport->conn);
	if (!(caps & SMB2_CAP_LEASING)) {
		torture_skip(tctx, "leases are not supported");
	}

	tree->session->transport->lease.handler	= torture_lease_handler;
	tree->session->transport->lease.private_data = tree;
	tree->session->transport->oplock.handler = torture_oplock_handler;
	tree->session->transport->oplock.private_data = tree;

	smb2_util_unlink(tree, fname);

	ZERO_STRUCT(break_info);

	/* Grab lease, upgrade to RHW .. */
	smb2_lease_create(&io, &ls, false, fname, LEASE1, smb2_util_lease_state("RH"));
	status = smb2_create(tree, mem_ctx, &io);
	CHECK_STATUS(status, NT_STATUS_OK);
	h = io.out.file.handle;
	CHECK_CREATED(&io, CREATED, FILE_ATTRIBUTE_ARCHIVE);
	CHECK_LEASE(&io, "RH", true, LEASE1);

	smb2_lease_create(&io, &ls, false, fname, LEASE1, smb2_util_lease_state("RHW"));
	status = smb2_create(tree, mem_ctx, &io);
	CHECK_STATUS(status, NT_STATUS_OK);
	h2 = io.out.file.handle;
	CHECK_CREATED(&io, EXISTED, FILE_ATTRIBUTE_ARCHIVE);
	CHECK_LEASE(&io, "RHW", true, LEASE1);

	/* Contend with LEASE2. */
	smb2_lease_create(&io, &ls, false, fname, LEASE2, smb2_util_lease_state("RHW"));
	status = smb2_create(tree, mem_ctx, &io);
	CHECK_STATUS(status, NT_STATUS_OK);
	h3 = io.out.file.handle;
	CHECK_CREATED(&io, EXISTED, FILE_ATTRIBUTE_ARCHIVE);
	CHECK_LEASE(&io, "RH", true, LEASE2);

	/* Verify that we were only sent one break. */
	CHECK_BREAK_INFO("RHW", "RH", LEASE1);

	/* Drop LEASE1 / LEASE2 */
	status = smb2_util_close(tree, h);
	CHECK_STATUS(status, NT_STATUS_OK);
	status = smb2_util_close(tree, h2);
	CHECK_STATUS(status, NT_STATUS_OK);
	status = smb2_util_close(tree, h3);
	CHECK_STATUS(status, NT_STATUS_OK);

	ZERO_STRUCT(break_info);

	/* Grab an R lease. */
	smb2_lease_create(&io, &ls, false, fname, LEASE1, smb2_util_lease_state("R"));
	status = smb2_create(tree, mem_ctx, &io);
	CHECK_STATUS(status, NT_STATUS_OK);
	h = io.out.file.handle;
	CHECK_CREATED(&io, EXISTED, FILE_ATTRIBUTE_ARCHIVE);
	CHECK_LEASE(&io, "R", true, LEASE1);

	/* Grab a level-II oplock. */
	smb2_oplock_create(&io, fname, smb2_util_oplock_level("s"));
	status = smb2_create(tree, mem_ctx, &io);
	CHECK_STATUS(status, NT_STATUS_OK);
	h2 = io.out.file.handle;
	CHECK_CREATED(&io, EXISTED, FILE_ATTRIBUTE_ARCHIVE);
	CHECK_VAL(io.out.oplock_level, smb2_util_oplock_level("s"));
	break_info.held_oplock_level = io.out.oplock_level;

	/* Verify no breaks. */
	CHECK_VAL(break_info.count, 0);
	CHECK_VAL(break_info.failures, 0);

	/* Open for truncate, force a break. */
	smb2_generic_create(&io, NULL, false, fname,
	    NTCREATEX_DISP_OVERWRITE_IF, smb2_util_oplock_level(""), 0, 0);
	status = smb2_create(tree, mem_ctx, &io);
	CHECK_STATUS(status, NT_STATUS_OK);
	h3 = io.out.file.handle;
	CHECK_CREATED(&io, TRUNCATED, FILE_ATTRIBUTE_ARCHIVE);
	CHECK_VAL(io.out.oplock_level, smb2_util_oplock_level(""));
	break_info.held_oplock_level = io.out.oplock_level;

	/* Sleep, use a write to clear the recv queue. */
	smb_msleep(250);
	ZERO_STRUCT(w);
	w.in.file.handle = h3;
	w.in.offset      = 0;
	w.in.data        = data_blob_talloc(mem_ctx, NULL, 4096);
	memset(w.in.data.data, 'o', w.in.data.length);
	status = smb2_write(tree, &w);
	CHECK_STATUS(status, NT_STATUS_OK);

	/* Verify one oplock break, one lease break. */
	CHECK_VAL(break_info.oplock_count, 1);
	CHECK_VAL(break_info.oplock_failures, 0);
	CHECK_VAL(break_info.oplock_level, smb2_util_oplock_level(""));
	CHECK_BREAK_INFO("R", "", LEASE1);

 done:
	smb2_util_close(tree, h);
	smb2_util_close(tree, h2);
	smb2_util_close(tree, h3);

	smb2_util_unlink(tree, fname);

	talloc_free(mem_ctx);

	return ret;
}

static bool test_lease_v2_request(struct torture_context *tctx,
				  struct smb2_tree *tree)
{
	TALLOC_CTX *mem_ctx = talloc_new(tctx);
	struct smb2_create io;
	struct smb2_lease ls;
	struct smb2_handle h1, h2, h3, h4, h5;
	struct smb2_write w;
	NTSTATUS status;
	const char *fname = "lease.dat";
	const char *dname = "lease.dir";
	const char *dnamefname = "lease.dir\\lease.dat";
	const char *dnamefname2 = "lease.dir\\lease2.dat";
	bool ret = true;

	smb2_util_unlink(tree, fname);
	smb2_deltree(tree, dname);

	tree->session->transport->lease.handler	= torture_lease_handler;
	tree->session->transport->lease.private_data = tree;
	tree->session->transport->oplock.handler = torture_oplock_handler;
	tree->session->transport->oplock.private_data = tree;

	ZERO_STRUCT(break_info);

	ZERO_STRUCT(io);
	smb2_lease_v2_create_share(&io, &ls, false, fname,
				   smb2_util_share_access("RWD"),
				   LEASE1, NULL,
				   smb2_util_lease_state("RHW"),
				   0);

	status = smb2_create(tree, mem_ctx, &io);
	CHECK_STATUS(status, NT_STATUS_OK);
	h1 = io.out.file.handle;
	CHECK_CREATED(&io, CREATED, FILE_ATTRIBUTE_ARCHIVE);
	CHECK_LEASE_V2(&io, "RHW", true, LEASE1, 0);

	ZERO_STRUCT(io);
	smb2_lease_v2_create_share(&io, &ls, true, dname,
				   smb2_util_share_access("RWD"),
				   LEASE2, NULL,
				   smb2_util_lease_state("RHW"),
				   0);
	status = smb2_create(tree, mem_ctx, &io);
	CHECK_STATUS(status, NT_STATUS_OK);
	h2 = io.out.file.handle;
	CHECK_CREATED(&io, CREATED, FILE_ATTRIBUTE_DIRECTORY);
	CHECK_LEASE_V2(&io, "RH", true, LEASE2, 0);

	ZERO_STRUCT(io);
	smb2_lease_v2_create_share(&io, &ls, false, dnamefname,
				   smb2_util_share_access("RWD"),
				   LEASE3, &LEASE2,
				   smb2_util_lease_state("RHW"),
				   0);
	status = smb2_create(tree, mem_ctx, &io);
	CHECK_STATUS(status, NT_STATUS_OK);
	h3 = io.out.file.handle;
	CHECK_CREATED(&io, CREATED, FILE_ATTRIBUTE_ARCHIVE);
	CHECK_LEASE_V2(&io, "RHW", true, LEASE3,
		       SMB2_LEASE_FLAG_PARENT_LEASE_KEY_SET);

	torture_wait_for_lease_break(tctx);
	CHECK_VAL(break_info.count, 0);
	CHECK_VAL(break_info.failures, 0);

	ZERO_STRUCT(io);
	smb2_lease_v2_create_share(&io, &ls, false, dnamefname2,
				   smb2_util_share_access("RWD"),
				   LEASE4, NULL,
				   smb2_util_lease_state("RHW"),
				   0);
	status = smb2_create(tree, mem_ctx, &io);
	CHECK_STATUS(status, NT_STATUS_OK);
	h4 = io.out.file.handle;
	CHECK_CREATED(&io, CREATED, FILE_ATTRIBUTE_ARCHIVE);
	CHECK_LEASE_V2(&io, "RHW", true, LEASE4, 0);

	torture_wait_for_lease_break(tctx);
	torture_wait_for_lease_break(tctx);
	CHECK_BREAK_INFO("RH", "", LEASE2);
	torture_wait_for_lease_break(tctx);

	ZERO_STRUCT(break_info);

	ZERO_STRUCT(io);
	smb2_lease_v2_create_share(&io, &ls, true, dname,
				   smb2_util_share_access("RWD"),
				   LEASE2, NULL,
				   smb2_util_lease_state("RHW"),
				   0);
	io.in.create_disposition = NTCREATEX_DISP_OPEN;
	status = smb2_create(tree, mem_ctx, &io);
	CHECK_STATUS(status, NT_STATUS_OK);
	h5 = io.out.file.handle;
	CHECK_CREATED(&io, EXISTED, FILE_ATTRIBUTE_DIRECTORY);
	CHECK_LEASE_V2(&io, "RH", true, LEASE2, 0);
	smb2_util_close(tree, h5);

	ZERO_STRUCT(w);
	w.in.file.handle = h4;
	w.in.offset      = 0;
	w.in.data        = data_blob_talloc(mem_ctx, NULL, 4096);
	memset(w.in.data.data, 'o', w.in.data.length);
	status = smb2_write(tree, &w);
	CHECK_STATUS(status, NT_STATUS_OK);

	smb_msleep(2000);
	torture_wait_for_lease_break(tctx);
	CHECK_VAL(break_info.count, 0);
	CHECK_VAL(break_info.failures, 0);

	smb2_util_close(tree, h4);
	torture_wait_for_lease_break(tctx);
	torture_wait_for_lease_break(tctx);
	CHECK_BREAK_INFO("RH", "", LEASE2);
	torture_wait_for_lease_break(tctx);

 done:
	smb2_util_close(tree, h1);
	smb2_util_close(tree, h2);
	smb2_util_close(tree, h3);
	smb2_util_close(tree, h4);
	smb2_util_close(tree, h5);

	smb2_util_unlink(tree, fname);
	smb2_deltree(tree, dname);

	talloc_free(mem_ctx);

	return ret;
}

struct torture_suite *torture_smb2_lease_init(void)
{
	struct torture_suite *suite =
	    torture_suite_create(talloc_autofree_context(), "lease");

	torture_suite_add_1smb2_test(suite, "request", test_lease_request);
	torture_suite_add_1smb2_test(suite, "upgrade", test_lease_upgrade);
	torture_suite_add_1smb2_test(suite, "upgrade2", test_lease_upgrade2);
	torture_suite_add_1smb2_test(suite, "break", test_lease_break);
	torture_suite_add_1smb2_test(suite, "oplock", test_lease_oplock);
	torture_suite_add_1smb2_test(suite, "multibreak", test_lease_multibreak);
	torture_suite_add_1smb2_test(suite, "v2_request", test_lease_v2_request);

	suite->description = talloc_strdup(suite, "SMB2-LEASE tests");

	return suite;
}
