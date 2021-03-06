#ifndef TEST_CLUSTER_H
#define TEST_CLUSTER_H

#include <stdlib.h>

#include "../../include/raft.h"
#include "../../include/raft/fixture.h"

#include "fsm.h"
#include "heap.h"
#include "io.h"
#include "munit.h"

#define FIXTURE_CLUSTER                             \
    FIXTURE_HEAP;                                   \
    struct raft_fsm fsms[RAFT_FIXTURE_MAX_SERVERS]; \
    struct raft_fixture cluster;

#define SETUP_CLUSTER(N)                                                   \
    SETUP_HEAP;                                                            \
    {                                                                      \
        unsigned i;                                                        \
        int rc;                                                            \
        for (i = 0; i < N; i++) {                                          \
            test_fsm_setup(NULL, &f->fsms[i]);                             \
        }                                                                  \
        rc = raft_fixture_init(&f->cluster, N, f->fsms);                   \
        munit_assert_int(rc, ==, 0);                                       \
        for (i = 0; i < N; i++) {                                          \
            raft_fixture_set_random(&f->cluster, i, munit_rand_int_range); \
        }                                                                  \
    }

#define TEAR_DOWN_CLUSTER                    \
    {                                        \
        unsigned n = CLUSTER_N;              \
        unsigned i;                          \
        raft_fixture_close(&f->cluster);     \
        for (i = 0; i < n; i++) {            \
            test_fsm_tear_down(&f->fsms[i]); \
        }                                    \
    }                                        \
    TEAR_DOWN_HEAP;

#define CLUSTER_N_PARAM "cluster-n"
#define CLUSTER_N_PARAM_GET \
    (unsigned)atoi(munit_parameters_get(params, CLUSTER_N_PARAM))

/**
 * Get the number of servers in the cluster.
 */
#define CLUSTER_N raft_fixture_n(&f->cluster)

/**
 * Index of the current leader, or CLUSTER_N if there's no leader.
 */
#define CLUSTER_LEADER raft_fixture_leader_index(&f->cluster)

/**
 * True if the cluster has a leader.
 */
#define CLUSTER_HAS_LEADER CLUSTER_LEADER < CLUSTER_N

/**
 * Get the struct raft object of the I'th server.
 */
#define CLUSTER_RAFT(I) raft_fixture_get(&f->cluster, I)

/**
 * Get the struct fsm object of the I'th server.
 */
#define CLUSTER_FSM(I) &f->fsms[I]

/**
 * Return the last applied index on the I'th server.
 */
#define CLUSTER_LAST_APPLIED(I) \
    raft_last_applied(raft_fixture_get(&f->cluster, I))

/**
 * Populate the given configuration with all servers in the fixture. All servers
 * will be voting.
 */
#define CLUSTER_CONFIGURATION(CONF)                                    \
    {                                                                  \
        int rc;                                                        \
        rc = raft_fixture_configuration(&f->cluster, CLUSTER_N, CONF); \
        munit_assert_int(rc, ==, 0);                                   \
    }

/**
 * Bootstrap all servers in the cluster. All servers will be voting.
 */
#define CLUSTER_BOOTSTRAP                                         \
    {                                                             \
        int rc;                                                   \
        struct raft_configuration configuration;                  \
        CLUSTER_CONFIGURATION(&configuration);                    \
        rc = raft_fixture_bootstrap(&f->cluster, &configuration); \
        munit_assert_int(rc, ==, 0);                              \
        raft_configuration_close(&configuration);                 \
    }

/**
 * Start all servers in the test cluster.
 */
#define CLUSTER_START                         \
    {                                         \
        int rc;                               \
        rc = raft_fixture_start(&f->cluster); \
        munit_assert_int(rc, ==, 0);          \
    }

/**
 * Step the cluster until a leader is elected or #MAX_MSECS have elapsed.
 */
#define CLUSTER_STEP_UNTIL_ELAPSED(MSECS) \
    raft_fixture_step_until_elapsed(&f->cluster, MSECS)

/**
 * Step the cluster until a leader is elected or #MAX_MSECS have elapsed.
 */
#define CLUSTER_STEP_UNTIL_HAS_LEADER(MAX_MSECS)                           \
    {                                                                      \
        bool done;                                                         \
        done = raft_fixture_step_until_has_leader(&f->cluster, MAX_MSECS); \
        munit_assert_true(done);                                           \
        munit_assert_true(CLUSTER_HAS_LEADER);                             \
    }

/**
 * Step the cluster until there's no leader or #MAX_MSECS have elapsed.
 */
#define CLUSTER_STEP_UNTIL_HAS_NO_LEADER(MAX_MSECS)                           \
    {                                                                         \
        bool done;                                                            \
        done = raft_fixture_step_until_has_no_leader(&f->cluster, MAX_MSECS); \
        munit_assert_true(done);                                              \
        munit_assert_false(CLUSTER_HAS_LEADER);                               \
    }

/**
 * Step the cluster until the given index was applied by the given server (or
 * all if N) or #MAX_MSECS have elapsed.
 */
#define CLUSTER_STEP_UNTIL_APPLIED(I, INDEX, MAX_MSECS)                        \
    {                                                                          \
        bool done;                                                             \
        done =                                                                 \
            raft_fixture_step_until_applied(&f->cluster, I, INDEX, MAX_MSECS); \
        munit_assert_true(done);                                               \
    }

/**
 * Request to apply an FSM command to add the given value to x.
 */
#define CLUSTER_APPLY_ADD_X(REQ, VALUE, CB)                   \
    {                                                         \
        struct raft_buffer buf;                               \
        struct raft *raft;                                    \
        int rc;                                               \
        munit_assert_int(CLUSTER_LEADER, !=, CLUSTER_N);      \
        test_fsm_encode_add_x(VALUE, &buf);                   \
        raft = raft_fixture_get(&f->cluster, CLUSTER_LEADER); \
        rc = raft_apply(raft, REQ, &buf, 1, CB);              \
        munit_assert_int(rc, ==, 0);                          \
    }

/**
 * Kill the I'th server.
 */
#define CLUSTER_KILL(I) raft_fixture_kill(&f->cluster, I);

/**
 * Kill the leader.
 */
#define CLUSTER_KILL_LEADER CLUSTER_KILL(CLUSTER_LEADER)

/**
 * Kill a majority of servers, except the leader (if there is one).
 */
#define CLUSTER_KILL_MAJORITY                              \
    {                                                      \
        size_t i;                                          \
        size_t n;                                          \
        for (i = 0, n = 0; n < (CLUSTER_N / 2) + 1; i++) { \
            if (i == CLUSTER_LEADER) {                     \
                continue;                                  \
            }                                              \
            CLUSTER_KILL(i)                                \
            n++;                                           \
        }                                                  \
    }

/**
 * Add a new pristine server to the cluster, connected to all others. Then
 * submit a request to add it to the configuration as non-voting server.
 */
#define CLUSTER_ADD                                                  \
    {                                                                \
        int rc;                                                      \
        struct raft *raft;                                           \
        test_fsm_setup(NULL, &f->fsms[CLUSTER_N]);                   \
        rc = raft_fixture_grow(&f->cluster, &f->fsms[CLUSTER_N]);    \
        munit_assert_int(rc, ==, 0);                                 \
        raft_fixture_set_random(&f->cluster, CLUSTER_N - 1,          \
                                munit_rand_int_range);               \
        raft = CLUSTER_RAFT(CLUSTER_N - 1);                          \
        rc = raft_add_server(CLUSTER_RAFT(CLUSTER_LEADER), raft->id, \
                             raft->address);                         \
        munit_assert_int(rc, ==, 0);                                 \
    }

/**
 * Promote the server that was added last.
 */
#define CLUSTER_PROMOTE                                      \
    {                                                        \
        unsigned id;                                         \
        int rc;                                              \
        id = CLUSTER_N; /* Last server that was added. */    \
        rc = raft_promote(CLUSTER_RAFT(CLUSTER_LEADER), id); \
        munit_assert_int(rc, ==, 0);                         \
    }

/**
 * Elect the I'th server.
 */
#define CLUSTER_ELECT(I) raft_fixture_elect(&f->fixture, I)

/**
 * Set the term persisted on the I'th server. This must be called before
 * starting the cluster.
 */
#define CLUSTER_SET_TERM(I, TERM) raft_fixture_set_term(&f->cluster, I, TERM)

/**
 * Set the snapshot persisted on the I'th server. This must be called before
 * starting the cluster.
 */
#define CLUSTER_SET_SNAPSHOT(I, LAST_INDEX, LAST_TERM, CONF_INDEX, X, Y) \
    {                                                                    \
        struct raft_configuration configuration;                         \
        struct raft_snapshot *snapshot;                                  \
        CLUSTER_CONFIGURATION(&configuration);                           \
        CREATE_SNAPSHOT(snapshot, LAST_INDEX, LAST_TERM, &configuration, \
                        CONF_INDEX, X, Y);                               \
        raft_configuration_close(&configuration);                        \
        raft_fixture_set_snapshot(&f->cluster, I, snapshot);             \
    }

#endif /* TEST_CLUSTER_H */
