/*
 * read.c
 *
 * Copyright (C) 2016 Aerospike, Inc.
 *
 * Portions may be licensed to Aerospike, Inc. under one or more contributor
 * license agreements.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/
 */

//==========================================================
// Includes.
//

#include "transaction/read.h"

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "citrusleaf/alloc.h"
#include "citrusleaf/cf_atomic.h"
#include "citrusleaf/cf_clock.h"

#include "dynbuf.h"
#include "fault.h"

#include "base/batch.h"
#include "base/cfg.h"
#include "base/datamodel.h"
#include "base/index.h"
#include "base/proto.h"
#include "base/transaction.h"
#include "base/transaction_policy.h"
#include "storage/storage.h"
#include "transaction/duplicate_resolve.h"
#include "transaction/proxy.h"
#include "transaction/rw_request.h"
#include "transaction/rw_request_hash.h"
#include "transaction/rw_utils.h"


//==========================================================
// Forward declarations.
//

bool start_read_dup_res(rw_request* rw, as_transaction* tr);
bool read_dup_res_cb(rw_request* rw);

void send_read_response(as_transaction* tr, as_msg_op** ops,
		as_bin** response_bins, uint16_t n_bins, cf_dyn_buf* db);
void read_timeout_cb(rw_request* rw);

transaction_status read_local(as_transaction* tr);
void read_local_done(as_transaction* tr, as_index_ref* r_ref, as_storage_rd* rd,
		int result_code);


//==========================================================
// Inlines & macros.
//

static inline void
client_read_update_stats(as_namespace* ns, uint8_t result_code)
{
	switch (result_code) {
	case AS_PROTO_RESULT_OK:
		cf_atomic64_incr(&ns->n_client_read_success);
		break;
	case AS_PROTO_RESULT_FAIL_TIMEOUT:
		cf_atomic64_incr(&ns->n_client_read_timeout);
		break;
	default:
		cf_atomic64_incr(&ns->n_client_read_error);
		break;
	case AS_PROTO_RESULT_FAIL_NOT_FOUND:
		cf_atomic64_incr(&ns->n_client_read_not_found);
		break;
	}
}

static inline void
batch_sub_read_update_stats(as_namespace* ns, uint8_t result_code)
{
	switch (result_code) {
	case AS_PROTO_RESULT_OK:
		cf_atomic64_incr(&ns->n_batch_sub_read_success);
		break;
	case AS_PROTO_RESULT_FAIL_TIMEOUT:
		cf_atomic64_incr(&ns->n_batch_sub_read_timeout);
		break;
	default:
		cf_atomic64_incr(&ns->n_batch_sub_read_error);
		break;
	case AS_PROTO_RESULT_FAIL_NOT_FOUND:
		cf_atomic64_incr(&ns->n_batch_sub_read_not_found);
		break;
	}
}


//==========================================================
// Public API.
//

transaction_status
as_read_start(as_transaction* tr)
{
	BENCHMARK_START(tr, read, FROM_CLIENT);
	BENCHMARK_START(tr, batch_sub, FROM_BATCH);

	if (! as_read_must_duplicate_resolve(tr)) {
		// No duplicates to resolve, or not configured to duplicate resolve.
		// Just read local copy - response sent to origin no matter what.
		return read_local(tr);
	}
	// else - there are duplicates, and we're configured to resolve them.

	// Create rw_request and add to hash.
	rw_request_hkey hkey = { tr->rsv.ns->id, tr->keyd };
	rw_request* rw = rw_request_create(&tr->keyd);
	transaction_status status = rw_request_hash_insert(&hkey, rw, tr);

	// If rw_request wasn't inserted in hash, transaction is finished.
	if (status != TRANS_IN_PROGRESS) {
		rw_request_release(rw);

		if (status != TRANS_WAITING) {
			send_read_response(tr, NULL, NULL, 0, NULL);
		}

		return status;
	}
	// else - rw_request is now in hash, continue...

	if (! start_read_dup_res(rw, tr)) {
		rw_request_hash_delete(&hkey, rw);
		tr->result_code = AS_PROTO_RESULT_FAIL_UNKNOWN;
		send_read_response(tr, NULL, NULL, 0, NULL);
		return TRANS_DONE_ERROR;
	}

	// Started duplicate resolution.
	return TRANS_IN_PROGRESS;
}


//==========================================================
// Local helpers - transaction flow.
//

bool
start_read_dup_res(rw_request* rw, as_transaction* tr)
{
	// Finish initializing rw_request, construct and send dup-res message.

	if (! dup_res_make_message(rw, tr)) {
		return false;
	}

	pthread_mutex_lock(&rw->lock);

	dup_res_setup_rw(rw, tr, read_dup_res_cb, read_timeout_cb);
	send_rw_messages(rw);

	pthread_mutex_unlock(&rw->lock);

	return true;
}


bool
read_dup_res_cb(rw_request* rw)
{
	BENCHMARK_NEXT_DATA_POINT(rw, read, dup_res);
	BENCHMARK_NEXT_DATA_POINT(rw, batch_sub, dup_res);

	as_transaction tr;
	as_transaction_init_from_rw(&tr, rw);

	if (tr.result_code != AS_PROTO_RESULT_OK) {
		send_read_response(&tr, NULL, NULL, 0, NULL);
		return true;
	}

	// Read the local copy and respond to origin.
	read_local(&tr);

	// Finished transaction - rw_request cleans up reservation and msgp!
	return true;
}


//==========================================================
// Local helpers - transaction end.
//

void
send_read_response(as_transaction* tr, as_msg_op** ops, as_bin** response_bins,
		uint16_t n_bins, cf_dyn_buf* db)
{
	// Paranoia - shouldn't get here on losing race with timeout.
	if (! tr->from.any) {
		cf_warning(AS_RW, "transaction origin %u has null 'from'", tr->origin);
		return;
	}

	// Note - if tr was setup from rw, rw->from.any has been set null and
	// informs timeout it lost the race.

	switch (tr->origin) {
	case FROM_CLIENT:
		BENCHMARK_NEXT_DATA_POINT(tr, read, local);
		if (db && db->used_sz != 0) {
			as_msg_send_ops_reply(tr->from.proto_fd_h, db);
		}
		else {
			as_msg_send_reply(tr->from.proto_fd_h, tr->result_code,
					tr->generation, tr->void_time, ops, response_bins, n_bins,
					tr->rsv.ns, as_transaction_trid(tr));
		}
		BENCHMARK_NEXT_DATA_POINT(tr, read, response);
		HIST_TRACK_ACTIVATE_INSERT_DATA_POINT(tr, read_hist);
		client_read_update_stats(tr->rsv.ns, tr->result_code);
		break;
	case FROM_PROXY:
		if (db && db->used_sz != 0) {
			as_proxy_send_ops_response(tr->from.proxy_node,
					tr->from_data.proxy_tid, db);
		}
		else {
			as_proxy_send_response(tr->from.proxy_node, tr->from_data.proxy_tid,
					tr->result_code, tr->generation, tr->void_time, ops,
					response_bins, n_bins, tr->rsv.ns, as_transaction_trid(tr));
		}
		break;
	case FROM_BATCH:
		BENCHMARK_NEXT_DATA_POINT(tr, batch_sub, read_local);
		as_batch_add_result(tr, n_bins, response_bins, ops);
		BENCHMARK_NEXT_DATA_POINT(tr, batch_sub, response);
		batch_sub_read_update_stats(tr->rsv.ns, tr->result_code);
		break;
	default:
		cf_crash(AS_RW, "unexpected transaction origin %u", tr->origin);
		break;
	}

	tr->from.any = NULL; // pattern, not needed
}


void
read_timeout_cb(rw_request* rw)
{
	if (! rw->from.any) {
		return; // lost race against dup-res callback
	}

	switch (rw->origin) {
	case FROM_CLIENT:
		as_msg_send_reply(rw->from.proto_fd_h, AS_PROTO_RESULT_FAIL_TIMEOUT, 0,
				0, NULL, NULL, 0, rw->rsv.ns, rw_request_trid(rw));
		// Timeouts aren't included in histograms.
		client_read_update_stats(rw->rsv.ns, AS_PROTO_RESULT_FAIL_TIMEOUT);
		break;
	case FROM_PROXY:
		break;
	case FROM_BATCH:
		as_batch_add_error(rw->from.batch_shared, rw->from_data.batch_index,
				AS_PROTO_RESULT_FAIL_TIMEOUT);
		// Timeouts aren't included in histograms.
		batch_sub_read_update_stats(rw->rsv.ns, AS_PROTO_RESULT_FAIL_TIMEOUT);
		break;
	default:
		cf_crash(AS_RW, "unexpected transaction origin %u", rw->origin);
		break;
	}

	rw->from.any = NULL; // inform other callback it lost the race
}


//==========================================================
// Local helpers - read local.
//

transaction_status
read_local(as_transaction* tr)
{
	as_msg* m = &tr->msgp->msg;
	as_namespace* ns = tr->rsv.ns;

	as_index_ref r_ref;
	r_ref.skip_lock = false;

	if (as_record_get_live(tr->rsv.tree, &tr->keyd, &r_ref, ns) != 0) {
		read_local_done(tr, NULL, NULL, AS_PROTO_RESULT_FAIL_NOT_FOUND);
		return TRANS_DONE_ERROR;
	}

	as_record* r = r_ref.r;

	// Check if it's an expired or truncated record.
	if (as_record_is_doomed(r, ns)) {
		read_local_done(tr, &r_ref, NULL, AS_PROTO_RESULT_FAIL_NOT_FOUND);
		return TRANS_DONE_ERROR;
	}

	as_storage_rd rd;

	as_storage_record_open(ns, r, &rd);

	// Check the key if required.
	// Note - for data-not-in-memory "exists" ops, key check is expensive!
	if (as_transaction_has_key(tr) &&
			as_storage_record_get_key(&rd) && ! check_msg_key(m, &rd)) {
		read_local_done(tr, &r_ref, &rd, AS_PROTO_RESULT_FAIL_KEY_MISMATCH);
		return TRANS_DONE_ERROR;
	}

	if ((m->info1 & AS_MSG_INFO1_GET_NO_BINS) != 0) {
		tr->generation = r->generation;
		tr->void_time = r->void_time;
		tr->last_update_time = r->last_update_time;

		read_local_done(tr, &r_ref, &rd, AS_PROTO_RESULT_OK);
		return TRANS_DONE_SUCCESS;
	}

	int result = as_storage_rd_load_n_bins(&rd);

	if (result < 0) {
		cf_warning_digest(AS_RW, &tr->keyd, "{%s} read_local: failed as_storage_rd_load_n_bins() ", ns->name);
		read_local_done(tr, &r_ref, &rd, -result);
		return TRANS_DONE_ERROR;
	}

	as_bin stack_bins[ns->storage_data_in_memory ? 0 : rd.n_bins];

	if ((result = as_storage_rd_load_bins(&rd, stack_bins)) < 0) {
		cf_warning_digest(AS_RW, &tr->keyd, "{%s} read_local: failed as_storage_rd_load_bins() ", ns->name);
		read_local_done(tr, &r_ref, &rd, -result);
		return TRANS_DONE_ERROR;
	}

	if (! as_bin_inuse_has(&rd)) {
		cf_warning_digest(AS_RW, &tr->keyd, "{%s} read_local: found record with no bins ", ns->name);
		read_local_done(tr, &r_ref, &rd, AS_PROTO_RESULT_FAIL_UNKNOWN);
		return TRANS_DONE_ERROR;
	}

	uint32_t bin_count = (m->info1 & AS_MSG_INFO1_GET_ALL) != 0 ?
			rd.n_bins : m->n_ops;

	as_msg_op* ops[bin_count];
	as_msg_op** p_ops = ops;
	as_bin* response_bins[bin_count];
	uint16_t n_bins = 0;

	as_bin result_bins[bin_count];
	uint32_t n_result_bins = 0;

	if ((m->info1 & AS_MSG_INFO1_GET_ALL) != 0) {
		p_ops = NULL;
		n_bins = as_bin_inuse_count(&rd);
		as_bin_get_all_p(&rd, response_bins);
	}
	else {
		if (m->n_ops == 0) {
			cf_warning_digest(AS_RW, &tr->keyd, "{%s} read_local: bin op(s) expected, none present ", ns->name);
			read_local_done(tr, &r_ref, &rd, AS_PROTO_RESULT_FAIL_PARAMETER);
			return TRANS_DONE_ERROR;
		}

		bool respond_all_ops = (m->info2 & AS_MSG_INFO2_RESPOND_ALL_OPS) != 0;

		as_msg_op* op = 0;
		int n = 0;

		while ((op = as_msg_op_iterate(m, op, &n)) != NULL) {
			if (op->op == AS_MSG_OP_READ) {
				as_bin* b = as_bin_get_from_buf(&rd, op->name, op->name_sz);

				if (b || respond_all_ops) {
					ops[n_bins] = op;
					response_bins[n_bins++] = b;
				}
			}
			else if (op->op == AS_MSG_OP_CDT_READ) {
				as_bin* b = as_bin_get_from_buf(&rd, op->name, op->name_sz);

				if (b) {
					as_bin* rb = &result_bins[n_result_bins];
					as_bin_set_empty(rb);

					if ((result = as_bin_cdt_read_from_client(b, op, rb)) < 0) {
						cf_warning_digest(AS_RW, &tr->keyd, "{%s} read_local: failed as_bin_cdt_read_from_client() ", ns->name);
						destroy_stack_bins(result_bins, n_result_bins);
						read_local_done(tr, &r_ref, &rd, -result);
						return TRANS_DONE_ERROR;
					}

					if (as_bin_inuse(rb)) {
						n_result_bins++;
						ops[n_bins] = op;
						response_bins[n_bins++] = rb;
					}
					else if (respond_all_ops) {
						ops[n_bins] = op;
						response_bins[n_bins++] = NULL;
					}
				}
				else if (respond_all_ops) {
					ops[n_bins] = op;
					response_bins[n_bins++] = NULL;
				}
			}
			else {
				cf_warning_digest(AS_RW, &tr->keyd, "{%s} read_local: unexpected bin op %u ", ns->name, op->op);
				destroy_stack_bins(result_bins, n_result_bins);
				read_local_done(tr, &r_ref, &rd, AS_PROTO_RESULT_FAIL_PARAMETER);
				return TRANS_DONE_ERROR;
			}
		}
	}

	cf_dyn_buf_define_size(db, 16 * 1024);

	if (tr->origin != FROM_BATCH) {
		db.used_sz = db.alloc_sz;
		db.buf = (uint8_t*)as_msg_make_response_msg(tr->result_code,
				r->generation, r->void_time, p_ops, response_bins, n_bins, ns,
				(cl_msg*)dyn_bufdb, &db.used_sz, as_transaction_trid(tr));

		db.is_stack = db.buf == dyn_bufdb;
		// Note - not bothering to correct alloc_sz if buf was allocated.
	}
	else {
		tr->generation = r->generation;
		tr->void_time = r->void_time;
		tr->last_update_time = r->last_update_time;

		// Since as_batch_add_result() constructs response directly in shared
		// buffer to avoid extra copies, can't use db.
		send_read_response(tr, p_ops, response_bins, n_bins, NULL);
	}

	destroy_stack_bins(result_bins, n_result_bins);
	as_storage_record_close(&rd);
	as_record_done(&r_ref, ns);

	// Now that we're not under the record lock, send the message we just built.
	if (db.used_sz != 0) {
		send_read_response(tr, NULL, NULL, 0, &db);

		cf_dyn_buf_free(&db);
		tr->from.proto_fd_h = NULL;
	}

	return TRANS_DONE_SUCCESS;
}


void
read_local_done(as_transaction* tr, as_index_ref* r_ref, as_storage_rd* rd,
		int result_code)
{
	if (r_ref) {
		if (rd) {
			as_storage_record_close(rd);
		}

		as_record_done(r_ref, tr->rsv.ns);
	}

	tr->result_code = (uint8_t)result_code;

	send_read_response(tr, NULL, NULL, 0, NULL);
}
