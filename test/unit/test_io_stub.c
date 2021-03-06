#include "../../include/raft.h"
#include "../../include/raft/io_stub.h"

#include "../lib/heap.h"
#include "../lib/runner.h"

TEST_MODULE(io_stub);

/*******************************************************************************
 *
 * Helpers
 *
 ******************************************************************************/

#define FIXTURE                \
    struct raft_heap heap;     \
    struct raft_io io;

#define SETUP                                           \
    int rv;                                             \
    (void)user_data;                                    \
    test_heap_setup(params, &f->heap);                  \
    rv = raft_io_stub_init(&f->io);                     \
    munit_assert_int(rv, ==, 0);                        \
    rv = f->io.init(&f->io, 1, "1");                    \
    munit_assert_int(rv, ==, 0);                        \
    rv = f->io.start(&f->io, 50, __tick_cb, __recv_cb); \
    munit_assert_int(rv, ==, 0);                        \
    f->io.data = f;

#define TEAR_DOWN                      \
    f->io.close(&f->io, NULL);         \
    raft_io_stub_close(&f->io);        \
    test_heap_tear_down(&f->heap);     \
    free(f);

struct fixture
{
    FIXTURE;
    struct raft_io_send req;
    struct
    {
        bool invoked;
    } tick_cb;
    struct
    {
        int invoked;
        int status;
    } append_cb;
    struct
    {
        bool invoked;
        int status;
    } send_cb;
    struct
    {
        bool invoked;
        struct raft_message *message;
    } recv_cb;
    struct
    {
        bool invoked;
    } stop_cb;
};

static void __tick_cb(struct raft_io *io)
{
    struct fixture *f = io->data;

    f->tick_cb.invoked = true;
}

static void __append_cb(void *data, const int status)
{
    struct fixture *f = data;

    f->append_cb.invoked++;
    f->append_cb.status = status;
}

static void __send_cb(struct raft_io_send *req, const int status)
{
    struct fixture *f = req->data;

    f->send_cb.invoked = true;
    f->send_cb.status = status;
}

static void __recv_cb(struct raft_io *io, struct raft_message *message)
{
    struct fixture *f = io->data;

    f->recv_cb.invoked = true;
    f->recv_cb.message = message;
}

static void *setup(const MunitParameter params[], void *user_data)
{
    struct fixture *f = munit_malloc(sizeof *f);
    SETUP;
    f->req.data = f;

    f->tick_cb.invoked = false;

    f->append_cb.invoked = 0;
    f->append_cb.status = -1;

    f->send_cb.invoked = false;
    f->send_cb.status = -1;

    return f;
}

static void tear_down(void *data)
{
    struct fixture *f = data;
    TEAR_DOWN;
}

/* Advance time. */
#define __advance(F, MSECS)                  \
    {                                        \
        raft_io_stub_advance(&F->io, MSECS); \
    }

/* Load the initial state from the store and check that no error occurs. */
#define __load(F)                                                       \
    {                                                                   \
        raft_term term;                                                 \
        unsigned voted_for;                                             \
        struct raft_snapshot *snapshot;                                 \
        struct raft_entry *entries;                                     \
        size_t n_entries;                                               \
        int rv;                                                         \
                                                                        \
        rv = F->io.load(&F->io, &term, &voted_for, &snapshot, &entries, \
                        &n_entries);                                    \
        munit_assert_int(rv, ==, 0);                                    \
    }

/*******************************************************************************
 *
 * raft_io->start
 *
 ******************************************************************************/

TEST_SUITE(start);
TEST_SETUP(start, setup);
TEST_TEAR_DOWN(start, tear_down);

/* When raft_io_stub_advance is called, the tick callback is invoked. */
TEST_CASE(start, tick, NULL)
{
    struct fixture *f = data;

    (void)params;

    __advance(f, 100);

    munit_assert_true(f->tick_cb.invoked);

    return MUNIT_OK;
}

/* Once the raft_io_uv instance is started, the recv callback is invoked when a
 * message is received.. */
TEST_CASE(start, recv, NULL)
{
    struct fixture *f = data;
    struct raft_message message;

    (void)params;

    message.type = RAFT_IO_REQUEST_VOTE;
    message.server_id = 2;
    message.server_address = "2";

    raft_io_stub_deliver(&f->io, &message);

    munit_assert_true(f->recv_cb.invoked);
    munit_assert_int(f->recv_cb.message->type, ==, RAFT_IO_REQUEST_VOTE);
    munit_assert_int(f->recv_cb.message->server_id, ==, 2);
    munit_assert_string_equal(f->recv_cb.message->server_address, "2");

    return MUNIT_OK;
}

/*******************************************************************************
 *
 * raft_io->load
 *
 ******************************************************************************/

TEST_SUITE(load);

TEST_SETUP(load, setup);
TEST_TEAR_DOWN(load, tear_down);

/* Load the initial state of a pristine server. */
TEST_CASE(load, pristine, NULL)
{
    struct fixture *f = data;
    raft_term term;
    unsigned voted_for;
    struct raft_entry *entries;
    size_t n_entries;
    struct raft_snapshot *snapshot;
    int rv;

    (void)params;

    rv = f->io.load(&f->io, &term, &voted_for, &snapshot, &entries, &n_entries);
    munit_assert_int(rv, ==, 0);

    munit_assert_int(term, ==, 0);
    munit_assert_int(voted_for, ==, 0);
    munit_assert_ptr_null(snapshot);
    munit_assert_ptr_null(entries);
    munit_assert_int(n_entries, ==, 0);

    return MUNIT_OK;
}

/*******************************************************************************
 *
 * raft_io->bootstrap
 *
 ******************************************************************************/

TEST_SUITE(bootstrap);

TEST_SETUP(bootstrap, setup);
TEST_TEAR_DOWN(bootstrap, tear_down);

/* Bootstrap a pristine server. */
TEST_CASE(bootstrap, pristine, NULL)
{
    struct fixture *f = data;
    struct raft_configuration configuration;
    int rv;

    (void)params;

    __load(f);

    /* Create a configuration */
    raft_configuration_init(&configuration);

    rv = raft_configuration_add(&configuration, 1, "1", true);
    munit_assert_int(rv, ==, 0);

    rv = f->io.bootstrap(&f->io, &configuration);
    munit_assert_int(rv, ==, 0);

    raft_configuration_close(&configuration);

    return MUNIT_OK;
}

/*******************************************************************************
 *
 * raft_io->set_term
 *
 ******************************************************************************/

TEST_SUITE(set_term);

TEST_SETUP(set_term, setup);
TEST_TEAR_DOWN(set_term, tear_down);

/* Set the term on a pristine store. */
TEST_CASE(set_term, pristine, NULL)
{
    struct fixture *f = data;
    int rv;

    (void)params;

    __load(f);

    rv = f->io.set_term(&f->io, 1);
    munit_assert_int(rv, ==, 0);

    return MUNIT_OK;
}

/*******************************************************************************
 *
 * raft_io->set_vote
 *
 ******************************************************************************/

TEST_SUITE(set_vote);

TEST_SETUP(set_vote, setup);
TEST_TEAR_DOWN(set_vote, tear_down);

/* Set the vote on a pristine store. */
TEST_CASE(set_vote, pristine, NULL)
{
    struct fixture *f = data;
    int rv;

    (void)params;

    __load(f);

    rv = f->io.set_term(&f->io, 1);
    munit_assert_int(rv, ==, 0);

    rv = f->io.set_vote(&f->io, 2);
    munit_assert_int(rv, ==, 0);

    return MUNIT_OK;
}

/*******************************************************************************
 *
 * raft_io->append
 *
 ******************************************************************************/

TEST_SUITE(append);

TEST_SETUP(append, setup);
TEST_TEAR_DOWN(append, tear_down);

/* Append entries on a pristine store. */
TEST_CASE(append, pristine, NULL)
{
    struct fixture *f = data;
    struct raft_entry entry;
    int rv;

    (void)params;

    __load(f);

    entry.term = 1;
    entry.type = RAFT_COMMAND;
    entry.buf.base = munit_malloc(1);
    entry.buf.len = 1;

    ((char *)entry.buf.base)[0] = 'x';

    rv = f->io.append(&f->io, &entry, 1, f, __append_cb);
    munit_assert_int(rv, ==, 0);

    raft_io_stub_flush_all(&f->io);

    munit_assert_int(f->append_cb.invoked, ==, 1);

    free(entry.buf.base);

    return MUNIT_OK;
}

/* Make two request append entries requests concurrently. */
TEST_CASE(append, concurrent, NULL)
{
    struct fixture *f = data;
    struct raft_entry entry1;
    struct raft_entry entry2;
    int rv;

    (void)params;

    __load(f);

    entry1.term = 1;
    entry1.type = RAFT_COMMAND;
    entry1.buf.base = munit_malloc(1);
    entry1.buf.len = 1;

    entry2.term = 1;
    entry2.type = RAFT_COMMAND;
    entry2.buf.base = munit_malloc(1);
    entry2.buf.len = 1;

    rv = f->io.append(&f->io, &entry1, 1, f, __append_cb);
    munit_assert_int(rv, ==, 0);

    rv = f->io.append(&f->io, &entry1, 1, f, __append_cb);
    munit_assert_int(rv, ==, 0);

    raft_io_stub_flush_all(&f->io);

    munit_assert_int(f->append_cb.invoked, ==, 2);

    free(entry1.buf.base);
    free(entry2.buf.base);

    return MUNIT_OK;
}

/*******************************************************************************
 *
 * raft_io->send
 *
 ******************************************************************************/

TEST_SUITE(send);

TEST_SETUP(send, setup);
TEST_TEAR_DOWN(send, tear_down);

/* Send the very first message. */
TEST_CASE(send, first, NULL)
{
    struct fixture *f = data;
    struct raft_message message;
    int rv;

    (void)params;

    __load(f);

    message.type = RAFT_IO_REQUEST_VOTE;
    message.server_id = 2;
    message.server_address = "2";

    rv = f->io.send(&f->io, &f->req, &message, __send_cb);
    munit_assert_int(rv, ==, 0);

    raft_io_stub_flush_all(&f->io);

    munit_assert_true(f->send_cb.invoked);

    return MUNIT_OK;
}

/*******************************************************************************
 *
 * raft_io->snapshot_put
 *
 ******************************************************************************/

TEST_SUITE(snapshot_put);

struct put_fixture
{
    FIXTURE
    struct raft_snapshot snapshot;
    struct raft_io_snapshot_put req;
    struct raft_buffer bufs[2];
    bool invoked;
    int status;
};

static void put_cb(struct raft_io_snapshot_put *req, int status)
{
    struct put_fixture *f = req->data;
    f->invoked = true;
    f->status = status;
}

TEST_SETUP(snapshot_put)
{
    struct put_fixture *f = munit_malloc(sizeof *f);
    SETUP;
    f->bufs[0].base = raft_malloc(8);
    f->bufs[1].base = raft_malloc(8);
    f->bufs[0].len = 8;
    f->bufs[1].len = 8;
    f->snapshot.index = 8;
    f->snapshot.term = 3;
    f->snapshot.configuration_index = 2;
    f->snapshot.bufs = f->bufs;
    f->snapshot.n_bufs = 2;
    raft_configuration_init(&f->snapshot.configuration);
    rv = raft_configuration_add(&f->snapshot.configuration, 1, "1", true);
    munit_assert_int(rv, ==, 0);
    f->req.data = f;
    f->invoked = false;
    f->status = -1;
    return f;
}

TEST_TEAR_DOWN(snapshot_put)
{
    struct put_fixture *f = data;
    raft_configuration_close(&f->snapshot.configuration);
    raft_free(f->bufs[0].base);
    raft_free(f->bufs[1].base);
    TEAR_DOWN;
}

/* Invoke the snapshot_put method and check that it returns the given code. */
#define put__invoke(RV)                                                 \
    {                                                                   \
        int rv;                                                         \
        rv = f->io.snapshot_put(&f->io, &f->req, &f->snapshot, put_cb); \
        munit_assert_int(rv, ==, RV);                                   \
    }

/* Put the first snapshot. */
TEST_CASE(snapshot_put, first, NULL)
{
    struct put_fixture *f = data;

    (void)params;

    put__invoke(0);

    raft_io_stub_flush_all(&f->io);

    return MUNIT_OK;
}

/*******************************************************************************
 *
 * rat_io_stub_connect
 *
 ******************************************************************************/

TEST_SUITE(connect);

struct connect_fixture
{
    FIXTURE
    struct raft_io_send req;
    struct raft_io other;
    bool invoked;
    struct raft_message *message;
};

static void other_recv_cb(struct raft_io *io, struct raft_message *message)
{
    struct connect_fixture *f = io->data;

    munit_assert_ptr_equal(io, &f->other);
    f->invoked = true;
    f->message = message;
    munit_assert_int(f->message->type, ==, RAFT_IO_REQUEST_VOTE);
}

TEST_SETUP(connect)
{
    struct connect_fixture *f = munit_malloc(sizeof *f);
    SETUP;
    rv = raft_io_stub_init(&f->other);
    munit_assert_int(rv, ==, 0);
    rv = f->io.init(&f->other, 2, "2");
    munit_assert_int(rv, ==, 0);
    rv = f->io.start(&f->other, 50, NULL, other_recv_cb);
    munit_assert_int(rv, ==, 0);
    f->other.data = f;
    f->invoked = false;
    f->message = NULL;
    raft_io_stub_connect(&f->io, &f->other);
    return f;
}

TEST_TEAR_DOWN(connect)
{
    struct connect_fixture *f = data;
    f->other.close(&f->other, NULL);
    raft_io_stub_close(&f->other);
    TEAR_DOWN;
}

/* Put the first snapshot. */
TEST_CASE(connect, deliver, NULL)
{
    struct connect_fixture *f = data;
    struct raft_message message;
    int rv;

    (void)params;

    message.type = RAFT_IO_REQUEST_VOTE;
    message.server_id = 2;
    message.server_address = "2";

    rv = f->io.send(&f->io, &f->req, &message, NULL);
    munit_assert_int(rv, ==, 0);

    raft_io_stub_flush(&f->io);
    raft_io_stub_advance(&f->io, 0);

    munit_assert_true(f->invoked);
    munit_assert_ptr_not_null(f->message);

    return MUNIT_OK;
}
