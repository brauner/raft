/**
 * Helpers to initialize a raft object, as if its state was loaded from disk.
 */

#ifndef TEST_RAFT_H
#define TEST_RAFT_H

#include "../../include/raft.h"

#include "fsm.h"
#include "heap.h"
#include "io.h"
#include "munit.h"

/**
 * Fields common to all fixtures setting up a raft instance.
 */
#define RAFT_FIXTURE       \
    struct raft_heap heap; \
    struct raft_io io;     \
    struct raft_fsm fsm;   \
    struct raft raft

/**
 * Setup the raft instance of a fixture.
 */
#define RAFT_SETUP(F)                                           \
    {                                                           \
        uint64_t id = 1;                                        \
        const char *address = "1";                              \
        int rv;                                                 \
        (void)user_data;                                        \
        test_heap_setup(params, &F->heap);                      \
        test_io_setup(params, &F->io);                          \
        test_fsm_setup(params, &F->fsm);                        \
        rv = raft_init(&F->raft, &F->io, &F->fsm, id, address); \
        munit_assert_int(rv, ==, 0);                            \
        F->raft.data = F;                                       \
    }

#define RAFT_TEAR_DOWN(F)              \
    {                                  \
        raft_close(&F->raft, NULL);    \
                                       \
        test_fsm_tear_down(&F->fsm);   \
        test_io_tear_down(&F->io);     \
        test_heap_tear_down(&F->heap); \
    }

/**
 * Start an instance and check that no error occurs.
 */
void test_start(struct raft *r);

void test_set_initial_snapshot(struct raft *r,
                               raft_term term,
                               raft_index index,
                               int x,
                               int y);

/**
 * Bootstrap and start raft instance.
 *
 * The initial configuration will have the given amount of servers and will be
 * saved as first entry in the log. The server IDs are assigned sequentially
 * starting from 1 up to @n_servers. Only servers with IDs in the range
 * [@voting_a, @voting_b] will be voting servers.
 */
void test_bootstrap_and_start(struct raft *r,
                              int n_servers,
                              int voting_a,
                              int voting_b);

/**
 * Make a pristine raft instance transition to the candidate state, by letting
 * the election time expire.
 */
void test_become_candidate(struct raft *r);

/**
 * Make a pristine raft instance transition to the leader state, by
 * transitioning to candidate state first and then getting votes from a majority
 * of the servers in the configuration.
 */
void test_become_leader(struct raft *r);

/**
 * Receive a valid heartbeat request from the given leader. Valid means that the
 * term of the request will match @r's current term, and the previous index/term
 * will match @r's last log entry.
 */
void test_receive_heartbeat(struct raft *r, unsigned leader_id);

#endif /* TEST_CONFIGURATION_H */
