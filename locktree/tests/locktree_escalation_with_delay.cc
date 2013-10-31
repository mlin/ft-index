/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
#ident "$Id$"
/*
COPYING CONDITIONS NOTICE:

  This program is free software; you can redistribute it and/or modify
  it under the terms of version 2 of the GNU General Public License as
  published by the Free Software Foundation, and provided that the
  following conditions are met:

      * Redistributions of source code must retain this COPYING
        CONDITIONS NOTICE, the COPYRIGHT NOTICE (below), the
        DISCLAIMER (below), the UNIVERSITY PATENT NOTICE (below), the
        PATENT MARKING NOTICE (below), and the PATENT RIGHTS
        GRANT (below).

      * Redistributions in binary form must reproduce this COPYING
        CONDITIONS NOTICE, the COPYRIGHT NOTICE (below), the
        DISCLAIMER (below), the UNIVERSITY PATENT NOTICE (below), the
        PATENT MARKING NOTICE (below), and the PATENT RIGHTS
        GRANT (below) in the documentation and/or other materials
        provided with the distribution.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
  02110-1301, USA.

COPYRIGHT NOTICE:

  TokuDB, Tokutek Fractal Tree Indexing Library.
  Copyright (C) 2007-2013 Tokutek, Inc.

DISCLAIMER:

  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

UNIVERSITY PATENT NOTICE:

  The technology is licensed by the Massachusetts Institute of
  Technology, Rutgers State University of New Jersey, and the Research
  Foundation of State University of New York at Stony Brook under
  United States of America Serial No. 11/760379 and to the patents
  and/or patent applications resulting from it.

PATENT MARKING NOTICE:

  This software is covered by US Patent No. 8,185,551.
  This software is covered by US Patent No. 8,489,638.

PATENT RIGHTS GRANT:

  "THIS IMPLEMENTATION" means the copyrightable works distributed by
  Tokutek as part of the Fractal Tree project.
  "PATENT CLAIMS" means the claims of patents that are owned or
  licensable by Tokutek, both currently or in the future; and that in
  the absence of this license would be infringed by THIS
  IMPLEMENTATION or by using or running THIS IMPLEMENTATION.

  "PATENT CHALLENGE" shall mean a challenge to the validity,
  patentability, enforceability and/or non-infringement of any of the
  PATENT CLAIMS or otherwise opposing any of the PATENT CLAIMS.

  Tokutek hereby grants to you, for the term and geographical scope of
  the PATENT CLAIMS, a non-exclusive, no-charge, royalty-free,
  irrevocable (except as stated in this section) patent license to
  make, have made, use, offer to sell, sell, import, transfer, and
  otherwise run, modify, and propagate the contents of THIS
  IMPLEMENTATION, where such license applies only to the PATENT
  CLAIMS.  This grant does not include claims that would be infringed
  only as a consequence of further modifications of THIS
  IMPLEMENTATION.  If you or your agent or licensee institute or order
  or agree to the institution of patent litigation against any entity
  (including a cross-claim or counterclaim in a lawsuit) alleging that
  THIS IMPLEMENTATION constitutes direct or contributory patent
  infringement, or inducement of patent infringement, then any rights
  granted to you under this License shall terminate as of the date
  such litigation is filed.  If you or your agent or exclusive
  licensee institute or order or agree to the institution of a PATENT
  CHALLENGE, then Tokutek may terminate any rights granted to you
  under this License.
*/

#ident "Copyright (c) 2007-2013 Tokutek Inc.  All rights reserved."
#ident "The technology is licensed by the Massachusetts Institute of Technology, Rutgers State University of New Jersey, and the Research Foundation of State University of New York at Stony Brook under United States of America Serial No. 11/760379 and to the patents and/or patent applications resulting from it."

#include <stdio.h>
#include "locktree.h"
#include "test.h"

// This test verifies that a lock request does not get a premature out of locks error.
// Two big transactions are grabbing locks in their own lock trees.
// Eventually, lock escalation needs to be run.
// We delay the execution of the background escalator thread.
// The test ensures that the txn's do not get a premature out of locks error due to the delay.

using namespace toku;

static int verbose = 0;
static int killed = 0;

static void locktree_release_lock(locktree *lt, TXNID txn_id, int64_t left_k, int64_t right_k) {
    range_buffer buffer;
    buffer.create();
    DBT left; toku_fill_dbt(&left, &left_k, sizeof left_k);
    DBT right; toku_fill_dbt(&right, &right_k, sizeof right_k);
    buffer.append(&left, &right);
    lt->release_locks(txn_id, &buffer);
    buffer.destroy();
}

// grab a write range lock on int64 keys bounded by left_k and right_k
static int locktree_write_lock(locktree *lt, TXNID txn_id, int64_t left_k, int64_t right_k) {
    DBT left; toku_fill_dbt(&left, &left_k, sizeof left_k);
    DBT right; toku_fill_dbt(&right, &right_k, sizeof right_k);
    return lt->acquire_write_lock(txn_id, &left, &right, nullptr);
}

static void run_big_txn(locktree::manager *mgr UU(), locktree *lt, TXNID txn_id) {
    int64_t last_i = -1;
    for (int64_t i = 0; !killed; i++) {
        uint64_t t_start = toku_current_time_microsec();
        int r = locktree_write_lock(lt, txn_id, i, i);
        assert(r == 0);
        last_i = i;
        uint64_t t_end = toku_current_time_microsec();
        uint64_t t_duration = t_end - t_start;
        if (t_duration > 100000) {
            printf("%u %s %" PRId64 " %" PRIu64 "\n", toku_os_gettid(), __FUNCTION__, i, t_duration);
        }
        toku_pthread_yield();
    }
    if (last_i != -1)
        locktree_release_lock(lt, txn_id, 0, last_i); // release the range 0 .. last_i
}

struct arg {
    locktree::manager *mgr;
    locktree *lt;
    TXNID txn_id;
    int64_t k;
};

static void *big_f(void *_arg) {
    struct arg *arg = (struct arg *) _arg;
    run_big_txn(arg->mgr, arg->lt, arg->txn_id);
    return arg;
}

static void e_callback(TXNID txnid, const locktree *lt, const range_buffer &buffer, void *extra) {
    if (verbose)
        printf("%u %s %" PRIu64 " %p %d %p\n", toku_os_gettid(), __FUNCTION__, txnid, lt, buffer.get_num_ranges(), extra);
}

static uint64_t get_escalation_count(locktree::manager &mgr) {
    LTM_STATUS_S ltm_status;
    mgr.get_status(&ltm_status);

    TOKU_ENGINE_STATUS_ROW key_status = NULL;
    // lookup keyname in status
    for (int i = 0; ; i++) {
        TOKU_ENGINE_STATUS_ROW status = &ltm_status.status[i];
        if (status->keyname == NULL)
            break;
        if (strcmp(status->keyname, "LTM_ESCALATION_COUNT") == 0) {
            key_status = status;
            break;
        }
    }
    assert(key_status);
    return key_status->value.num;
}

int main(int argc, const char *argv[]) {
    int n_big = 2;
    uint64_t stalls = 0;
    uint64_t max_lock_memory = 100000000;
    uint64_t delay = 500000;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose++;
            continue;
        }
        if (strcmp(argv[i], "--n_big") == 0 && i+1 < argc) {
            n_big = atoi(argv[++i]);
            continue;
        }
        if (strcmp(argv[i], "--stalls") == 0 && i+1 < argc) {
            stalls = atoll(argv[++i]);
            continue;
        }
        if (strcmp(argv[i], "--max_lock_memory") == 0 && i+1 < argc) {
            max_lock_memory = atoll(argv[++i]);
            continue;
        }
        if (strcmp(argv[i], "--delay") == 0 && i+1 < argc) {
            delay = atoll(argv[++i]);
            continue;
        }
    }

    int r;

    // create a manager
    locktree::manager mgr;
    mgr.create(nullptr, nullptr, e_callback, nullptr);
    mgr.set_max_lock_memory(max_lock_memory);
    mgr.set_escalator_delay(delay);
    if (verbose)
        mgr.set_escalator_verbose(true);

    // create lock trees
    DESCRIPTOR desc[n_big];
    DICTIONARY_ID dict_id[n_big];
    locktree *lt[n_big];
    for (int i = 0; i < n_big; i++) {
        desc[i] = nullptr;
        dict_id[i] = { (uint64_t)i };
        lt[i] = mgr.get_lt(dict_id[i], desc[i], compare_dbts, nullptr);
    }

    // create the worker threads
    struct arg big_arg[n_big];
    pthread_t big_ids[n_big];
    for (int i = 0; i < n_big; i++) {
        big_arg[i] = { &mgr, lt[i], (TXNID)(1000+i) };
        r = toku_pthread_create(&big_ids[i], nullptr, big_f, &big_arg);
        assert(r == 0);
    }

    // wait for some escalations to occur
    while (get_escalation_count(mgr) < stalls) {
        sleep(1);
    }
    killed = 1;

    // cleanup
    for (int i = 0; i < n_big; i++) {
        void *ret;
        r = toku_pthread_join(big_ids[i], &ret);
        assert(r == 0);
    }
    for (int i = 0; i < n_big ; i++) {
        mgr.release_lt(lt[i]);
    }
    mgr.destroy();

    return 0;
}