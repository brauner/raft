#include "../include/raft.h"

#include "assert.h"
#include "configuration.h"
#include "election.h"
#include "logging.h"
#include "replication.h"
#include "state.h"
#include "watch.h"

/**
 * Number of milliseconds after which a server promotion will be aborted if the
 * server hasn't caught up with the logs yet.
 */
#define RAFT_MAX_CATCH_UP_DURATION (30 * 1000)

unsigned raft_next_timeout(struct raft *r)
{
    unsigned timeout;
    if (r->state == RAFT_LEADER) {
        timeout = r->heartbeat_timeout;
    } else {
        timeout = r->election_timeout_rand;
    }
    return timeout > r->timer ? timeout - r->timer : 0;
}

/**
 * Apply time-dependent rules for followers (Figure 3.1).
 */
static int follower_tick(struct raft *r)
{
    const struct raft_server *server;

    assert(r != NULL);
    assert(r->state == RAFT_FOLLOWER);

    server = configuration__get(&r->configuration, r->id);

    /* If we have been removed from the configuration, or maybe we didn't
     * receive one yet, just stay follower. */
    if (server == NULL) {
        return 0;
    }

    /* Check if we need to start an election.
     *
     * From Section §3.3:
     *
     *   If a follower receives no communication over a period of time called
     *   the election timeout, then it assumes there is no viable leader and
     *   begins an election to choose a new leader.
     *
     * Figure 3.1:
     *
     *   If election timeout elapses without receiving AppendEntries RPC from
     *   current leader or granting vote to candidate, convert to candidate.
     */
    if (r->timer > r->election_timeout_rand && server->voting) {
        infof(r->io, "convert to candidate and start new election");
        return raft_state__convert_to_candidate(r);
    }

    return 0;
}

/**
 * Apply time-dependent rules for candidates (Figure 3.1).
 */
static int candidate_tick(struct raft *r)
{
    assert(r != NULL);
    assert(r->state == RAFT_CANDIDATE);

    /* Check if we need to start an election.
     *
     * From Section §3.4:
     *
     *   The third possible outcome is that a candidate neither wins nor loses
     *   the election: if many followers become candidates at the same time,
     *   votes could be split so that no candidate obtains a majority. When this
     *   happens, each candidate will time out and start a new election by
     *   incrementing its term and initiating another round of RequestVote RPCs
     */
    if (r->timer > r->election_timeout_rand) {
        infof(r->io, "start new election");
        return raft_election__start(r);
    }

    return 0;
}

/**
 * Return true if the leader has been contacted by a majority of servers in the
 * last @election_timeout milliseconds.
 */
static bool leader_has_been_contacted_by_majority_of_servers(struct raft *r)
{
    raft_time now = r->io->time(r->io);
    unsigned i;
    unsigned contacts = 0;

    for (i = 0; i < r->configuration.n; i++) {
        struct raft_server *server = &r->configuration.servers[i];
        struct raft_replication *replication = &r->leader_state.replication[i];
        unsigned elapsed;

        if (!server->voting) {
            continue;
        }

        if (server->id == r->id) {
            contacts++;
            continue;
        }

        elapsed = now - replication->last_contact;

        if (elapsed <= r->election_timeout) {
            contacts++;
        } else {
            debugf(r->io,
                   "lost contact with server %d: no message since %u "
                   "msecs (last contact %u, now %u)",
                   server->id, elapsed, replication->last_contact, now);
        }
    }

    return contacts > configuration__n_voting(&r->configuration) / 2;
}

/**
 * Apply time-dependent rules for leaders (Figure 3.1).
 */
static int leader_tick(struct raft *r, const unsigned msec_since_last_tick)
{
    int rv;

    assert(r != NULL);
    assert(r->state == RAFT_LEADER);

    /* Check if we still can reach a majority of servers.
     *
     * From Section 6.2:
     *
     *   A leader in Raft steps down if an election timeout elapses without a
     *   successful round of heartbeats to a majority of its cluster; this
     *   allows clients to retry their requests with another server.
     */
    if (!leader_has_been_contacted_by_majority_of_servers(r)) {
        warnf(r->io, "unable to contact majority of cluster -> step down");
        rv = raft_state__convert_to_follower(r, r->current_term);
        return rv;
    }

    /* Check if we need to send heartbeats.
     *
     * From Figure 3.1:
     *
     *   Send empty AppendEntries RPC during idle periods to prevent election
     *   timeouts.
     */
    if (r->timer > r->heartbeat_timeout) {
        raft_replication__trigger(r, 0);
        r->timer = 0;
    }

    /* If a server is being promoted, increment the timer of the current
     * round or abort the promotion.
     *
     * From Section 4.2.1:
     *
     *   The algorithm waits a fixed number of rounds (such as 10). If the last
     *   round lasts less than an election timeout, then the leader adds the new
     *   server to the cluster, under the assumption that there are not enough
     *   unreplicated entries to create a significant availability
     *   gap. Otherwise, the leader aborts the configuration change with an
     *   error.
     */
    if (r->leader_state.promotee_id != 0) {
        unsigned id = r->leader_state.promotee_id;
        size_t server_index;
        bool is_too_slow;
        bool is_unresponsive;

        /* If a promotion is in progress, we expect that our configuration
         * contains an entry for the server being promoted, and that the server
         * is not yet considered as voting. */
        server_index = configuration__index_of(&r->configuration, id);
        assert(server_index < r->configuration.n);
        assert(!r->configuration.servers[server_index].voting);

        r->leader_state.round_duration += msec_since_last_tick;

        is_too_slow = (r->leader_state.round_number == 10 &&
                       r->leader_state.round_duration > r->election_timeout);
        is_unresponsive =
            r->leader_state.round_duration > RAFT_MAX_CATCH_UP_DURATION;

        /* Abort the promotion if we are at the 10'th round and it's still
         * taking too long, or if the server is unresponsive. */
        if (is_too_slow || is_unresponsive) {
            r->leader_state.promotee_id = 0;

            r->leader_state.round_index = 0;
            r->leader_state.round_number = 0;
            r->leader_state.round_duration = 0;

            raft_watch__promotion_aborted(r, id);
        }
    }

    return 0;
}

static int tick(struct raft *r)
{
    int rv;
    raft_time now;
    unsigned msecs_since_last_tick;

    assert(r != NULL);

    assert(r->state == RAFT_UNAVAILABLE || r->state == RAFT_FOLLOWER ||
           r->state == RAFT_CANDIDATE || r->state == RAFT_LEADER);

    /* If we are not available, let's do nothing. */
    if (r->state == RAFT_UNAVAILABLE) {
        return 0;
    }

    now = r->io->time(r->io);

    msecs_since_last_tick = now - r->last_tick;
    r->timer += msecs_since_last_tick;
    r->last_tick = now;

    switch (r->state) {
        case RAFT_FOLLOWER:
            rv = follower_tick(r);
            break;
        case RAFT_CANDIDATE:
            rv = candidate_tick(r);
            break;
        case RAFT_LEADER:
            rv = leader_tick(r, msecs_since_last_tick);
            break;
    }

    return rv;
}

void tick_cb(struct raft_io *io)
{
    struct raft *r;
    r = io->data;
    tick(r);
}

