#include <string.h>

#include "assert.h"
#include "configuration.h"
#include "error.h"
#include "log.h"
#include "logging.h"
#include "membership.h"
#include "queue.h"
#include "replication.h"
#include "snapshot.h"
#include "state.h"
#include "watch.h"

#ifndef max
#define max(a, b) ((a) < (b) ? (b) : (a))
#endif

#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif

/* Set to 1 to enable tracing. */
#if 1
#define tracef(MSG, ...) debugf(r->io, "replication: " MSG, __VA_ARGS__)
#else
#define tracef(MSG, ...)
#endif

/**
 * Hold context for a #RAFT_IO_APPEND_ENTRIES send request that was submitted.
 */
struct send_append_entries
{
    struct raft *raft;          /* Instance that has submitted the request */
    raft_index index;           /* Index of the first entry in the request. */
    struct raft_entry *entries; /* Entries referenced in the request. */
    unsigned n;                 /* Length of the entries array. */
    struct raft_io_send req;
};

struct send_install_snapshot
{
    struct raft *raft;
    struct raft_snapshot *snapshot;
    struct raft_io_snapshot_get get;
    struct raft_io_send send;
    unsigned server_id; /* ID of follower server to send the snapshot to */
};

struct recv_install_snapshot
{
    struct raft *raft;
    struct raft_snapshot snapshot;
};

/**
 * Hold context for an append request that was submitted by a leader.
 */
struct raft_replication__leader_append
{
    struct raft *raft;          /* Instance that has submitted the request */
    raft_index index;           /* Index of the first entry in the request. */
    struct raft_entry *entries; /* Entries referenced in the request. */
    unsigned n;                 /* Length of the entries array. */
};

struct raft_replication__follower_append
{
    struct raft *raft; /* Instance that has submitted the request */
    raft_index index;  /* Index of the first entry in the request. */
    struct raft_append_entries args;
};

/**
 * Callback invoked after request to send an AppendEntries RPC has completed.
 */
static void raft_replication__send_append_entries_cb(struct raft_io_send *req,
                                                     int status)
{
    struct send_append_entries *request = req->data;
    struct raft *r = request->raft;

    debugf(r->io, "send append entries completed: status %d", status);

    /* Tell the log that we're done referencing these entries. */
    log__release(&r->log, request->index, request->entries, request->n);

    raft_free(request);
}

static void send_install_snapshot_cb(struct raft_io_send *req, int status)
{
    struct send_install_snapshot *request = req->data;
    struct raft *r = request->raft;

    debugf(r->io, "send install snapshot completed: status %d", status);
    snapshot__close(request->snapshot);
    raft_free(request->snapshot);
    raft_free(request);
}

static void snapshot_get_cb(struct raft_io_snapshot_get *req,
                            struct raft_snapshot *snapshot,
                            int status)
{
    struct send_install_snapshot *request = req->data;
    struct raft *r = request->raft;
    struct raft_message message;
    struct raft_install_snapshot *args = &message.install_snapshot;
    const struct raft_server *server;
    int rv;

    if (status != 0) {
        errorf(r->io, "get snapshot %s", raft_strerror(status));
        goto err;
    }

    if (r->state != RAFT_LEADER) {
        goto err_with_snapshot;
    }

    server = configuration__get(&r->configuration, request->server_id);
    if (server == NULL) {
        /* Probably the server was removed in the meantime. */
        goto err_with_snapshot;
    }

    assert(snapshot->n_bufs == 1);

    message.type = RAFT_IO_INSTALL_SNAPSHOT;
    message.server_id = server->id;
    message.server_address = server->address;

    args->term = r->current_term;
    args->leader_id = r->id;
    args->last_index = snapshot->index;
    args->last_term = snapshot->term;
    args->conf_index = snapshot->configuration_index;
    args->conf = snapshot->configuration;
    args->data = snapshot->bufs[0];

    request->snapshot = snapshot;
    request->send.data = request;

    infof(r->io, "sending snapshot %ld to %ld", snapshot->index, server->id);

    rv = r->io->send(r->io, &request->send, &message, send_install_snapshot_cb);
    if (rv != 0) {
        goto err_with_snapshot;
    }

    return;

err_with_snapshot:
    snapshot__close(snapshot);
    raft_free(snapshot);
err:
    raft_free(request);
    return;
}

static int raft_replication__send_snapshot(struct raft *r, size_t i)
{
    struct raft_server *server = &r->configuration.servers[i];
    struct raft_replication *replication = &r->leader_state.replication[i];
    struct send_install_snapshot *request;
    int rv;

    request = raft_malloc(sizeof *request);
    if (request == NULL) {
        rv = RAFT_ENOMEM;
        goto err;
    }
    request->raft = r;
    request->server_id = server->id;
    request->get.data = request;

    replication->state = REPLICATION__SNAPSHOT;

    rv = r->io->snapshot_get(r->io, &request->get, snapshot_get_cb);
    if (rv != 0) {
        goto err_after_req_alloc;
    }

    return 0;

err_after_req_alloc:
    raft_free(request);
    replication->state = REPLICATION__PROBE;
err:
    assert(rv != 0);
    return rv;
}

int raft_replication__send_append_entries(struct raft *r, size_t i)
{
    struct raft_server *server = &r->configuration.servers[i];
    struct raft_replication *replication = &r->leader_state.replication[i];
    raft_index next_index;
    struct raft_message message;
    struct raft_append_entries *args = &message.append_entries;
    struct send_append_entries *request;
    raft_time msecs_without_contact;
    int rv;

    assert(r != NULL);
    assert(r->state == RAFT_LEADER);
    assert(server != NULL);
    assert(server->id != r->id);
    assert(server->id != 0);
    assert(r->leader_state.replication != NULL);

    args->term = r->current_term;
    args->leader_id = r->id;

    /* If we have already sent a snapshot or we haven't hear back from the
     * server since a while, just send heartbeats until we hear back again from
     * the server (at that point we'll set the state back to probe). */
    msecs_without_contact = r->io->time(r->io) - replication->last_contact;
    if (replication->state == REPLICATION__SNAPSHOT ||
        msecs_without_contact > 5000 /* TODO: make this configurable */) {
        next_index = log__last_index(&r->log) + 1;
    } else {
        next_index = replication->next_index;
    }

    /* From Section §3.5:
     *
     *   When sending an AppendEntries RPC, the leader includes the index and
     *   term of the entry in its log that immediately precedes the new
     *   entries. If the follower does not find an entry in its log with the
     *   same index and term, then it refuses the new entries. The consistency
     *   check acts as an induction step: the initial empty state of the logs
     *   satisfies the Log Matching Property, and the consistency check
     *   preserves the Log Matching Property whenever logs are extended. As a
     *   result, whenever AppendEntries returns successfully, the leader knows
     *   that the follower’s log is identical to its own log up through the new
     *   entries (Log Matching Property in Figure 3.2).
     */
    if (next_index == 1) {
        /* We're including the very first log entry, so prevIndex and prevTerm
         * are null. */
        if (log__term_of(&r->log, 1) == 0) {
            return raft_replication__send_snapshot(r, i);
        }
        args->prev_log_index = 0;
        args->prev_log_term = 0;
    } else {
        /* Set prevIndex and prevTerm to the index and term of the entry at
         * next_index - 1 */
        assert(next_index > 1);

        args->prev_log_index = next_index - 1;
        args->prev_log_term = log__term_of(&r->log, next_index - 1);

        /* If the entry is not anymore in our log, check the last index of the
         * last snapshot. In case next_index - 1 is behind the snapshot last
         * index, we don't know anymore about that section of log, so we need to
         * send the whole snapshot. Otherwise if next_index - 1 is exactly the
         * snapshot last index, we need to send all the current log. */
        if (args->prev_log_term == 0) {
            assert(r->snapshot.index > 0);
            assert(next_index - 1 <= r->snapshot.index);
            if (next_index - 1 < r->snapshot.index) {
                infof(r->io, "missing entry at index %lld -> send snapshot",
                      next_index - 1);
                return raft_replication__send_snapshot(r, i);
            }
            args->prev_log_term = r->snapshot.term;
        }
    }

    rv = log__acquire(&r->log, next_index, &args->entries, &args->n_entries);
    if (rv != 0) {
        goto err;
    }

    /* From Section §3.5:
     *
     *   The leader keeps track of the highest index it knows to be committed,
     *   and it includes that index in future AppendEntries RPCs (including
     *   heartbeats) so that the other servers eventually find out. Once a
     *   follower learns that a log entry is committed, it applies the entry to
     *   its local state machine (in log order)
     */
    args->leader_commit = r->commit_index;

    tracef("send %ld entries to server %ld (log size %ld)", args->n_entries,
           server->id, log__n_entries(&r->log));

    message.type = RAFT_IO_APPEND_ENTRIES;
    message.server_id = server->id;
    message.server_address = server->address;

    request = raft_malloc(sizeof *request);
    if (request == NULL) {
        rv = RAFT_ENOMEM;
        goto err_after_entries_acquired;
    }
    request->raft = r;
    request->index = args->prev_log_index + 1;
    request->entries = args->entries;
    request->n = args->n_entries;

    request->req.data = request;
    rv = r->io->send(r->io, &request->req, &message,
                     raft_replication__send_append_entries_cb);
    if (rv != 0) {
        goto err_after_request_alloc;
    }

    return 0;

err_after_request_alloc:
    raft_free(request);

err_after_entries_acquired:
    log__release(&r->log, next_index, args->entries, args->n_entries);

err:
    assert(rv != 0);

    return rv;
}

/* Called after a successful append entries I/O request to update the index of
 * the last entry stored on disk. Return how many new entries that are still
 * present in our in-memory log were stored. */
static size_t update_last_stored(struct raft *r,
                                 raft_index first_index,
                                 struct raft_entry *entries,
                                 size_t n_entries)
{
    size_t i;

    /* Check which of these entries is still in our in-memory log */
    for (i = 0; i < n_entries; i++) {
        struct raft_entry *entry = &entries[i];
        raft_index index = first_index + i;
        raft_term local_term = log__term_of(&r->log, index);

        /* If we have no entry at this index, or if the entry we have now has a
         * different term, it means that this entry got truncated, so let's stop
         * here. */
        if (local_term == 0 || (local_term > 0 && local_term != entry->term)) {
            break;
        }

        assert(local_term != 0 && local_term == entry->term);
    }

    r->last_stored += i;
    return i;
}

static void raft_replication__leader_append_cb(void *data, int status)
{
    struct raft_replication__leader_append *request = data;
    struct raft *r = request->raft;
    size_t server_index;
    int rv;

    debugf(r->io, "write log completed on leader: status %d", status);

    update_last_stored(r, request->index, request->entries, request->n);

    /* Tell the log that we're done referencing these entries. */
    log__release(&r->log, request->index, request->entries, request->n);

    raft_free(request);

    /* If we are not leader anymore, just discard the result. */
    if (r->state != RAFT_LEADER) {
        debugf(r->io, "local server is not leader -> ignore write log result");
        return;
    }

    /* TODO: in case this is a failed disk write and we were the leader creating
     * these entries in the first place, should we truncate our log too? since
     * we have appended these entries to it. */
    if (status != 0) {
        return;
    }

    /* If Check if we have reached a quorum. */
    server_index = configuration__index_of(&r->configuration, r->id);

    /* Only update the next index if we are part of the current
     * configuration. The only case where this is not true is when we were
     * asked to remove ourselves from the cluster.
     *
     * From Section 4.2.2:
     *
     *   there will be a period of time (while it is committing Cnew) when a
     *   leader can manage a cluster that does not include itself; it
     *   replicates log entries but does not count itself in majorities.
     */
    if (server_index < r->configuration.n) {
        r->leader_state.replication[server_index].match_index = r->last_stored;
    } else {
        const struct raft_entry *entry = log__get(&r->log, r->last_stored);
        assert(entry->type == RAFT_CONFIGURATION);
    }

    /* Check if we can commit some new entries. */
    raft_replication__quorum(r, r->last_stored);

    rv = raft_replication__apply(r);
    if (rv != 0) {
        /* TODO: just log the error? */
    }
}

static int raft_replication__leader_append(struct raft *r, unsigned index)
{
    struct raft_entry *entries;
    unsigned n;
    struct raft_replication__leader_append *request;
    int rv;

    assert(r->state == RAFT_LEADER);

    if (index == 0) {
        return 0;
    }

    /* Acquire all the entries from the given index onwards. */
    rv = log__acquire(&r->log, index, &entries, &n);
    if (rv != 0) {
        goto err;
    }

    /* We expect this function to be called only when there are actually
     * some entries to write. */
    assert(n > 0);

    /* Allocate a new request. */
    request = raft_malloc(sizeof *request);
    if (request == NULL) {
        rv = RAFT_ENOMEM;
        goto err_after_entries_acquired;
    }

    request->raft = r;
    request->index = index;
    request->entries = entries;
    request->n = n;

    rv = r->io->append(r->io, entries, n, request,
                       raft_replication__leader_append_cb);
    if (rv != 0) {
        goto err_after_request_alloc;
    }

    return 0;

err_after_request_alloc:
    raft_free(request);

err_after_entries_acquired:
    log__release(&r->log, index, entries, n);

err:
    assert(rv != 0);
    return rv;
}

int raft_replication__trigger(struct raft *r, const raft_index index)
{
    raft_time now;
    size_t i;
    int rv;

    assert(r->state == RAFT_LEADER);

    rv = raft_replication__leader_append(r, index);
    if (rv != 0) {
        goto err;
    }

    /* Reset the heartbeat timer: for a full request_timeout period we'll be
     * good and we won't need to contact followers again, since this was not an
     * idle period.
     *
     * From Figure 3.1:
     *
     *   [Rules for Servers] Leaders: Upon election: send initial empty
     *   AppendEntries RPCs (heartbeat) to each server; repeat during idle
     *   periods to prevent election timeouts
     */
    r->timer = 0;

    now = r->io->time(r->io);

    /* Trigger replication for servers we didn't hear from recently. */
    for (i = 0; i < r->configuration.n; i++) {
        struct raft_server *server = &r->configuration.servers[i];
        struct raft_replication *replication = &r->leader_state.replication[i];
        int rv;

        if (server->id == r->id) {
            continue;
        }

        /* Send the heartbeat only if we were idle. */
        if (index == 0) {
            /* TODO: since we don't yet keep a last_contact array which is
             * independent from the replication array, if the value is 0 it
             * means that this is the very first heartbeat being sent after
             * election. In this case we unconditionally send a heartbeat
             * message and we set
             * last_contact to know to avoid thinking that we lost contact. */
            if (replication->last_contact == 0) {
                replication->last_contact = now;
            } else if (now - replication->last_contact <
                       r->heartbeat_timeout / 2) {
                continue;
            }
        }

        rv = raft_replication__send_append_entries(r, i);
        if (rv != 0 && rv != RAFT_ERR_IO_CONNECT) {
            /* This is not a critical failure, let's just log it. */
            warnf(r->io, "failed to send append entries to server %ld: %s (%d)",
                  server->id, raft_strerror(rv), rv);
        }
    }

    return 0;

err:
    assert(rv != 0);

    return rv;
}

/**
 * Helper to be invoked after a promotion of a non-voting server has been
 * requested via @raft_promote and that server has caught up with logs.
 *
 * This function changes the local configuration marking the server being
 * promoted as actually voting, appends the a RAFT_CONFIGURATION entry with
 * the new configuration to the local log and triggers its replication.
 */
static int raft_replication__trigger_promotion(struct raft *r)
{
    raft_index index;
    raft_term term = r->current_term;
    size_t server_index;
    struct raft_server *server;
    int rv;

    assert(r->state == RAFT_LEADER);
    assert(r->leader_state.promotee_id != 0);

    server_index =
        configuration__index_of(&r->configuration, r->leader_state.promotee_id);
    assert(server_index < r->configuration.n);

    server = &r->configuration.servers[server_index];

    assert(!server->voting);

    /* Update our current configuration. */
    server->voting = true;

    /* Index of the entry being appended. */
    index = log__last_index(&r->log) + 1;

    /* Encode the new configuration and append it to the log. */
    rv = log__append_configuration(&r->log, term, &r->configuration);
    if (rv != 0) {
        goto err;
    }

    /* Start writing the new log entry to disk and send it to the followers. */
    rv = raft_replication__trigger(r, index);
    if (rv != 0) {
        goto err_after_log_append;
    }

    r->leader_state.promotee_id = 0;
    r->configuration_uncommitted_index = log__last_index(&r->log);

    return 0;

err_after_log_append:
    log__truncate(&r->log, index);

err:
    server->voting = false;

    assert(rv != 0);
    return rv;
}

int raft_replication__update(struct raft *r,
                             const struct raft_server *server,
                             const struct raft_append_entries_result *result)
{
    size_t server_index;
    struct raft_replication *replication;
    raft_index last_log_index;
    bool is_being_promoted;
    int rv;

    assert(r->state == RAFT_LEADER);

    server_index = configuration__index_of(&r->configuration, server->id);
    assert(server_index < r->configuration.n);

    replication = &r->leader_state.replication[server_index];
    replication->last_contact = r->io->time(r->io);

    /* Reset the replication state to probe, as we might need to send the
     * snapshot again. */
    if (replication->state == REPLICATION__SNAPSHOT) {
        debugf(r->io, "reset replication from snapshot to probe");
        replication->state = REPLICATION__PROBE;
    }

    /* If the reported index is lower than the match index, it must be an out of
     * order response for an old append entries. Ignore it. */
    if (replication->match_index > replication->next_index - 1) {
        debugf(r->io, "match index higher than reported next index -> ignore");
        return 0;
    }

    last_log_index = log__last_index(&r->log);

    /* If the RPC failed because of a log mismatch, retry.
     *
     * From Figure 3.1:
     *
     *   [Rules for servers] Leaders:
     *
     *   - If AppendEntries fails because of log inconsistency:
     *     decrement nextIndex and retry.
     */
    if (!result->success) {
        /* If the match index is already up-to-date then the rejection must be
         * stale and come from an out of order message. */
        if (replication->match_index == replication->next_index - 1) {
            debugf(r->io, "match index is up to date -> ignore ");
            return 0;
        }

        /* If the peer reports a last index lower than what we believed was its
         * next index, decrerment the next index to whatever is shorter: our log
         * or the peer log. Otherwise just blindly decrement next_index by 1. */
        if (result->last_log_index < replication->next_index - 1) {
            replication->next_index =
                min(result->last_log_index, last_log_index);
        } else {
            replication->next_index = replication->next_index - 1;
        }

        replication->next_index = max(replication->next_index, 1);

        infof(r->io, "log mismatch -> send old entries %ld",
              replication->next_index);

        /* Retry, ignoring errors. */
        raft_replication__send_append_entries(r, server_index);

        return 0;
    }

    if (result->last_log_index <= replication->match_index) {
        /* Like above, this must be a stale response. */
        debugf(r->io, "match index is up to date -> ignore ");

        return 0;
    }

    /* In case of success the remote server is expected to send us back the
     * value of prevLogIndex + len(entriesToAppend). */
    assert(result->last_log_index <= last_log_index);

    /* If the RPC succeeded, update our counters for this server.
     *
     * From Figure 3.1:
     *
     *   [Rules for servers] Leaders:
     *
     *   If successful update nextIndex and matchIndex for follower.
     */
    replication->next_index = result->last_log_index + 1;
    replication->match_index = result->last_log_index;
    debugf(r->io, "match/next idx for server %ld: %ld/%ld", server->id,
           replication->match_index, replication->next_index);

    /* If the server is currently being promoted and is catching with logs,
     * update the information about the current catch-up round, and possibly
     * proceed with the promotion. */
    is_being_promoted = r->leader_state.promotee_id != 0 &&
                        r->leader_state.promotee_id == server->id;
    if (is_being_promoted) {
        int is_up_to_date = raft_membership__update_catch_up_round(r);
        if (is_up_to_date) {
            rv = raft_replication__trigger_promotion(r);
            if (rv != 0) {
                return rv;
            }
        }
    }

    /* TODO: switch to pipeline replication mode. */

    return 0;
}

static void raft_replication__follower_respond_cb(struct raft_io_send *req,
                                                  int status)
{
    (void)status;
    raft_free(req);
}

static void raft_replication__follower_append_cb(void *data, int status)
{
    struct raft_replication__follower_append *request = data;
    struct raft *r = request->raft;
    struct raft_append_entries *args = &request->args;
    struct raft_message message;
    struct raft_append_entries_result *result = &message.append_entries_result;
    struct raft_io_send *req;
    size_t i;
    size_t j;
    int rv;

    debugf(r->io, "I/O completed on follower: status %d", status);

    assert(args->leader_id > 0);
    assert(args->entries != NULL);
    assert(args->n_entries > 0);

    i = update_last_stored(r, request->index, args->entries, args->n_entries);

    /* If we are not followers anymore, just discard the result. */
    if (r->state != RAFT_FOLLOWER) {
        debugf(r->io, "local server is not follower -> ignore I/O result");
        goto out;
    }

    if (status != 0) {
        result->success = false;
        goto respond;
    }

    result->term = r->current_term;

    /* If none of the entries that we persisted is present anymore in our
     * in-memory log, there's nothing to report or to do. We just discard
     * them. */
    if (i == 0) {
        goto out;
    }

    /* Possibly apply configuration changes. */
    for (j = 0; j < i; j++) {
        struct raft_entry *entry = &args->entries[j];
        raft_index index = request->index + j;
        raft_term local_term = log__term_of(&r->log, index);

        assert(local_term != 0 && local_term == entry->term);

        /* If this is a configuration change entry, check if the change is about
         * promoting a non-voting server to voting, and in that case update our
         * configuration cache. */
        if (entry->type == RAFT_CONFIGURATION) {
            rv = raft_membership__apply(r, index, entry);
            if (rv != 0) {
                goto out;
            }
        }
    }

    /* From Figure 3.1:
     *
     *   AppendEntries RPC: Receiver implementation: If leaderCommit >
     *   commitIndex, set commitIndex = min(leaderCommit, index of last new
     *   entry).
     */
    if (args->leader_commit > r->commit_index) {
        r->commit_index = min(args->leader_commit, r->last_stored);
        rv = raft_replication__apply(r);
        if (rv != 0) {
            goto out;
        }
    }

    result->success = true;

respond:
    result->last_log_index = r->last_stored;

    message.type = RAFT_IO_APPEND_ENTRIES_RESULT;
    message.server_id = r->follower_state.current_leader.id;
    message.server_address = r->follower_state.current_leader.address;

    req = raft_malloc(sizeof *req);
    if (req == NULL) {
        goto out;
    }

    rv = r->io->send(r->io, req, &message,
                     raft_replication__follower_respond_cb);
    if (rv != 0) {
        raft_free(req);
        goto out;
    }

out:
    log__release(&r->log, request->index, request->args.entries,
                 request->args.n_entries);

    raft_free(request);
}

/**
 * Check that the log matching property against an incoming AppendEntries
 * request.
 *
 * From Figure 3.1:
 *
 *   [AppendEntries RPC] Receiver implementation:
 *
 *   2. Reply false if log doesn't contain an entry at prevLogIndex whose
 *   term matches prevLogTerm.
 *
 * Return 0 if the check passed.
 *
 * Return 1 if the check did not pass and the request needs to be rejected.
 *
 * Return -1 if there's a conflict and we need to shutdown.
 */
static int raft_replication__check_prev_log_entry(
    struct raft *r,
    const struct raft_append_entries *args)
{
    raft_term local_prev_term;

    /* If this is the very first entry, there's nothing to check. */
    if (args->prev_log_index == 0) {
        return 0;
    }

    if (r->snapshot.index == args->prev_log_index) {
        local_prev_term = r->snapshot.term;
    } else {
        local_prev_term = log__term_of(&r->log, args->prev_log_index);
    }

    if (local_prev_term == 0) {
        debugf(r->io, "no entry at previous index -> reject");
        return 1;
    }

    if (local_prev_term != args->prev_log_term) {
        if (args->prev_log_index <= r->commit_index) {
            /* Should never happen; something is seriously wrong! */
            errorf(r->io,
                   "previous index conflicts with "
                   "committed entry -> shutdown");
            return -1;
        }
        debugf(r->io, "previous term mismatch -> reject");
        return 1;
    }

    return 0;
}

/**
 * Delete from our log all entries that conflict with the ones in the given
 * AppendEntries request.
 *
 * From Figure 3.1:
 *
 *   [AppendEntries RPC] Receiver implementation:
 *
 *   3. If an existing entry conflicts with a new one (same index but
 *   different terms), delete the existing entry and all that follow it.
 *
 * The @i parameter will be set to the array index of the first new log entry
 * that we don't have yet in our log, among the ones included in the given
 * AppendEntries request.
 */
static int raft_replication__delete_conflicting_entries(
    struct raft *r,
    const struct raft_append_entries *args,
    size_t *i)
{
    size_t j;
    int rv;

    for (j = 0; j < args->n_entries; j++) {
        struct raft_entry *entry = &args->entries[j];
        raft_index entry_index = args->prev_log_index + 1 + j;
        raft_term local_term = log__term_of(&r->log, entry_index);

        if (local_term > 0 && local_term != entry->term) {
            if (entry_index <= r->commit_index) {
                /* Should never happen; something is seriously wrong! */
                errorf(r->io,
                       "new index conflicts with "
                       "committed entry -> shutdown");

                return RAFT_ERR_SHUTDOWN;
            }

            debugf(r->io, "log mismatch -> truncate (%ld)", entry_index);

            /* Discard any uncommitted voting change. */
            rv = raft_membership__rollback(r);
            if (rv != 0) {
                return rv;
            }

            /* Delete all entries from this index on because they don't match */
            rv = r->io->truncate(r->io, entry_index);
            if (rv != 0) {
                return rv;
            }
            log__truncate(&r->log, entry_index);
            r->last_stored = entry_index - 1;

            /* We want to append all entries from here on, replacing anything
             * that we had before. */
            break;
        } else if (local_term == 0) {
            /* We don't have an entry at this index, so we want to append this
             * new one and all the subsequent ones. */
            break;
        }
    }

    *i = j;

    return 0;
}

int raft_replication__append(struct raft *r,
                             const struct raft_append_entries *args,
                             bool *success,
                             bool *async)
{
    struct raft_replication__follower_append *request;
    int match;
    size_t n;
    size_t i;
    size_t j;
    int rv;

    assert(r != NULL);
    assert(args != NULL);
    assert(success != NULL);
    assert(async != NULL);

    assert(r->state == RAFT_FOLLOWER);

    *success = false;
    *async = false;

    /* Check the log matching property. */
    match = raft_replication__check_prev_log_entry(r, args);
    if (match != 0) {
        assert(match == 1 || match == -1);
        return match == 1 ? 0 : RAFT_ERR_SHUTDOWN;
    }
    rv = raft_replication__delete_conflicting_entries(r, args, &i);
    if (rv != 0) {
        return rv;
    }

    *success = true;

    n = args->n_entries - i; /* Number of new entries */

    /* This is an empty AppendEntries, there's nothing to write. However we
     * still want to check if we can commit some entry.
     *
     * From Figure 3.1:
     *
     *   AppendEntries RPC: Receiver implementation: If leaderCommit >
     *   commitIndex, set commitIndex = min(leaderCommit, index of last new
     *   entry).
     */
    if (n == 0) {
        if (args->leader_commit > r->commit_index) {
            raft_index last_index = log__last_index(&r->log);
            r->commit_index = min(args->leader_commit, last_index);
            rv = raft_replication__apply(r);
            if (rv != 0) {
                return rv;
            }
        }

        return 0;
    }

    *async = true;

    request = raft_malloc(sizeof *request);
    if (request == NULL) {
        rv = RAFT_ENOMEM;
        goto err;
    }

    request->raft = r;
    request->args = *args;
    request->index = args->prev_log_index + 1 + i;

    /* Update our in-memory log to reflect that we received these entries. We'll
     * notify the leader of a successful append once the write entries request
     * that we issue below actually completes.  */
    for (j = 0; j < n; j++) {
        struct raft_entry *entry = &args->entries[i + j];

        rv = log__append(&r->log, entry->term, entry->type, &entry->buf,
                         entry->batch);
        if (rv != 0) {
            /* TODO: we should revert any changes we made to the log */
            goto err_after_request_alloc;
        }
    }

    /* Acquire the relevant entries from the log. */
    rv = log__acquire(&r->log, request->index, &request->args.entries,
                      &request->args.n_entries);
    if (rv != 0) {
        goto err_after_request_alloc;
    }

    assert(request->args.n_entries == n);

    rv = r->io->append(r->io, request->args.entries, request->args.n_entries,
                       request, raft_replication__follower_append_cb);
    if (rv != 0) {
        goto err_after_acquire_entries;
    }

    *success = true;

    raft_free(args->entries);

    return 0;

err_after_acquire_entries:
    log__release(&r->log, request->index, request->args.entries,
                 request->args.n_entries);

err_after_request_alloc:
    raft_free(request);

err:
    assert(rv != 0);
    return rv;
}

static void put_snapshot_cb(struct raft_io_snapshot_put *req, int status)
{
    struct recv_install_snapshot *request = req->data;
    struct raft *r = request->raft;
    struct raft_snapshot *snapshot = &request->snapshot;
    /* raft_term local_term; */
    raft_index local_first_index;
    int rv;

    r->snapshot.put.data = NULL;

    if (status != 0) {
        errorf(r->io, "save snapshot %d: %s", snapshot->index,
               raft_strerror(status));
        goto err;
    }

    /* From Figure 5.3:
     *
     *   6. If existing entry has same index and term as lastIndex and lastTerm,
     *      discard log up through lastIndex (but retain any following entries)
     *      and reply.
     */
    /* TODO: at the moment we just ignore snapshots of entries that we have */
    /* local_term = log__term_of(&r->log, snapshot->index); */
    /* if (local_term == snapshot->term) { */
    /*     log__shift(&r->log, snapshot->index); */
    /*     goto err; */
    /* } */

    /* From Figure 5.3:
     *
     *   7. Discard the entire log
     *   8. Reset state machine using snapshot contents (and load lastConfig
     *      as cluster configuration).
     */
    local_first_index = log__first_index(&r->log);
    assert(local_first_index == 0);
    /* if (local_first_index > 0) { */
    /*     log__truncate(&r->log, local_first_index); */
    /* } */
    log__set_offset(&r->log, snapshot->index);

    r->snapshot.index = snapshot->index;
    r->snapshot.term = snapshot->term;
    r->last_stored = snapshot->index;

    rv = r->fsm->restore(r->fsm, &snapshot->bufs[0]);
    if (rv != 0) {
        errorf(r->io, "restore snapshot %d: %s", snapshot->index,
               raft_strerror(status));
        goto err;
    }

    raft_configuration_close(&r->configuration);
    r->configuration = snapshot->configuration;
    r->configuration_index = snapshot->configuration_index;

    goto out;

err:
    /* In case of error we must also free the snapshot data buffer and free the
     * configuration. */
    raft_free(snapshot->bufs[0].base);
    raft_configuration_close(&snapshot->configuration);

out:
    /* Don't free the snapshot data buffer, as ownership has been trasfered to
     * the fsm. */
    raft_free(snapshot->bufs);
    raft_free(request);
}

int raft_replication__install_snapshot(struct raft *r,
                                       const struct raft_install_snapshot *args,
                                       bool *success,
                                       bool *async)
{
    struct recv_install_snapshot *request;
    struct raft_snapshot *snapshot;
    raft_term local_term;
    int rv;

    assert(r->state == RAFT_FOLLOWER);

    *success = false;
    *async = false;

    /* If we are taking a snapshot ourselves or installing a snapshot, ignore
     * the request, the leader will weventually retry. TODO: we should do
     * something smarter. */
    if (r->snapshot.pending.term != 0 || r->snapshot.put.data != NULL) {
        *async = true;
        return 0;
    }

    /* If our last snapshot is more up-to-date, this is a no-op */
    if (r->snapshot.index >= args->last_index) {
        *success = true;
        return 0;
    }

    /* If we already have all entries in the snapshot, this is a no-op */
    local_term = log__term_of(&r->log, args->last_index);
    if (local_term != 0 && local_term >= args->last_term) {
        *success = true;
        return 0;
    }

    *async = true;

    /* Premptively update our in-memory state.
     * TODO: we should roll this back in case of failure, or something. */
    r->last_applied = args->last_index;

    /* We need to truncate our entire log */
    log__truncate(&r->log, 1);
    rv = r->io->truncate(r->io, 1);
    if (rv != 0) {
        goto err;
    }
    r->last_stored = 0;

    request = raft_malloc(sizeof *request);
    if (request == NULL) {
        rv = RAFT_ENOMEM;
        goto err;
    }
    request->raft = r;

    snapshot = &request->snapshot;
    snapshot->term = args->last_term;
    snapshot->index = args->last_index;
    snapshot->configuration_index = args->conf_index;
    snapshot->configuration = args->conf;

    snapshot->bufs = raft_malloc(sizeof *snapshot->bufs);
    if (snapshot->bufs == NULL) {
        rv = RAFT_ENOMEM;
        goto err_after_request_alloc;
    }
    snapshot->bufs[0] = args->data;
    snapshot->n_bufs = 1;

    /* TODO: we should truncate the in-memory log immediately */
    assert(r->snapshot.put.data == NULL);
    r->snapshot.put.data = request;
    rv =
        r->io->snapshot_put(r->io, &r->snapshot.put, snapshot, put_snapshot_cb);
    if (rv != 0) {
        goto err_after_bufs_alloc;
    }

    return 0;

err_after_bufs_alloc:
    raft_free(snapshot->bufs);
    r->snapshot.put.data = NULL;
err_after_request_alloc:
    raft_free(request);
err:
    assert(rv != 0);
    return rv;
}

/**
 * Apply a RAFT_CONFIGURATION entry that has been committed.
 */
static void raft_replication__apply_configuration(struct raft *r,
                                                  const raft_index index)
{
    assert(index > 0);

    /* If this is an uncommitted configuration that we had already applied when
     * submitting the configuration change (for leaders) or upon receiving it
     * via an AppendEntries RPC (for followers), then reset the uncommitted
     * index, since that uncommitted configuration is now committed. */
    if (r->configuration_uncommitted_index == index) {
        r->configuration_uncommitted_index = 0;
    }

    r->configuration_index = index;

    /* If we are leader but not part of this new configuration, step down.
     *
     * From Section 4.2.2:
     *
     *   In this approach, a leader that is removed from the configuration steps
     *   down once the Cnew entry is committed.
     */
    if (r->state == RAFT_LEADER &&
        configuration__get(&r->configuration, r->id) == NULL) {
        /* Ignore the return value, since we can't fail in this case (no actual
         * write for the new term will be done) */
        raft_state__convert_to_follower(r, r->current_term);
    }

    raft_watch__configuration_applied(r);
}

/**
 * Apply a RAFT_COMMAND entry that has been committed.
 */
static int raft_replication__apply_command(struct raft *r,
                                           const raft_index index,
                                           const struct raft_buffer *buf)
{
    int rv;
    raft__queue *head;

    rv = r->fsm->apply(r->fsm, buf);
    if (rv != 0) {
        return rv;
    }

    if (r->state == RAFT_LEADER) {
        struct raft_apply *req;
        RAFT__QUEUE_FOREACH(head, &r->leader_state.apply_reqs)
        {
            req = RAFT__QUEUE_DATA(head, struct raft_apply, queue);
            if (req->index == index) {
                RAFT__QUEUE_REMOVE(head);
                if (req->cb != NULL) {
                    req->cb(req, 0);
                }
                break;
            }
        }
    }

    raft_watch__command_applied(r, index);

    return 0;
}

static bool should_take_snapshot(struct raft *r)
{
    /* If a snapshot is already in progress, we don't want to start another
     *  one. */
    if (r->snapshot.pending.term != 0) {
        return false;
    };

    /* If we didn't reach the threshold yet, do nothing. */
    if (r->last_applied - r->snapshot.index < r->snapshot.threshold) {
        return false;
    }

    return true;
}

static void snapshot_put_cb(struct raft_io_snapshot_put *req, int status)
{
    struct raft *r = req->data;
    struct raft_snapshot *snapshot;
    size_t n_entries;
    raft_index last_index;
    raft_index shift_index;

    r->snapshot.put.data = NULL;
    snapshot = &r->snapshot.pending;

    if (status != 0) {
        debugf(r->io, "snapshot %lld at term %lld: %s", snapshot->index,
               snapshot->term, raft_strerror(status));
        goto out;
    }

    r->snapshot.term = snapshot->term;
    r->snapshot.index = snapshot->index;
    /* TODO: make the number of trailing entries configurable */
    n_entries = log__n_entries(&r->log);
    last_index = log__last_index(&r->log);
    if (n_entries > 100) {
        shift_index = last_index - 100;
        if (snapshot->index < shift_index) {
            shift_index = snapshot->index;
        }
        log__shift(&r->log, shift_index);
    }

out:
    snapshot__close(&r->snapshot.pending);
    r->snapshot.pending.term = 0;
}

static int take_snapshot(struct raft *r)
{
    struct raft_snapshot *snapshot;
    unsigned i;
    int rv;

    snapshot = &r->snapshot.pending;
    snapshot->index = r->last_applied;
    snapshot->term = log__term_of(&r->log, r->last_applied);

    raft_configuration_init(&snapshot->configuration);
    rv = configuration__copy(&r->configuration, &snapshot->configuration);
    if (rv != 0) {
        goto err;
    }

    snapshot->configuration_index = r->configuration_index;

    rv = r->fsm->snapshot(r->fsm, &snapshot->bufs, &snapshot->n_bufs);
    if (rv != 0) {
        goto err_after_config_copy;
    }

    assert(r->snapshot.put.data == NULL);
    r->snapshot.put.data = r;
    rv =
        r->io->snapshot_put(r->io, &r->snapshot.put, snapshot, snapshot_put_cb);
    if (rv != 0) {
        goto err_after_fsm_snapshot;
    }

    return 0;

err_after_fsm_snapshot:
    for (i = 0; i < snapshot->n_bufs; i++) {
        raft_free(snapshot->bufs[i].base);
    }
    raft_free(snapshot->bufs);
err_after_config_copy:
    raft_configuration_close(&snapshot->configuration);
err:
    r->snapshot.pending.term = 0;
    assert(rv);
    return rv;
}

int raft_replication__apply(struct raft *r)
{
    raft_index index;
    int rv;

    assert(r->state == RAFT_LEADER || r->state == RAFT_FOLLOWER);
    assert(r->last_applied <= r->commit_index);

    if (r->last_applied == r->commit_index) {
        /* Nothing to do. */
        return 0;
    }

    for (index = r->last_applied + 1; index <= r->commit_index; index++) {
        const struct raft_entry *entry = log__get(&r->log, index);

        assert(entry->type == RAFT_COMMAND ||
               entry->type == RAFT_CONFIGURATION);

        switch (entry->type) {
            case RAFT_COMMAND:
                rv = raft_replication__apply_command(r, index, &entry->buf);
                break;
            case RAFT_CONFIGURATION:
                raft_replication__apply_configuration(r, index);
                rv = 0;
                break;
        }

        if (rv != 0) {
            break;
        }

        r->last_applied = index;
    }

    if (should_take_snapshot(r)) {
        rv = take_snapshot(r);
    }

    return rv;
}

void raft_replication__quorum(struct raft *r, const raft_index index)
{
    size_t votes = 0;
    size_t i;

    assert(r->state == RAFT_LEADER);

    if (index <= r->commit_index) {
        return;
    }

    if (log__term_of(&r->log, index) != r->current_term) {
        return;
    }

    for (i = 0; i < r->configuration.n; i++) {
        struct raft_server *server = &r->configuration.servers[i];
        if (!server->voting) {
            continue;
        }
        if (r->leader_state.replication[i].match_index >= index) {
            votes++;
        }
    }

    if (votes > configuration__n_voting(&r->configuration) / 2) {
        r->commit_index = index;

        tracef("new commit index %ld", r->commit_index);
    }

    return;
}
