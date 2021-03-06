#include "../../src/queue.h"

#include "../lib/runner.h"

TEST_MODULE(queue);

/**
 * Helpers
 */

struct fixture
{
    void *queue[2];
};

static void *setup(const MunitParameter params[], void *user_data)
{
    struct fixture *f = munit_malloc(sizeof *f);

    (void)params;
    (void)user_data;

    RAFT__QUEUE_INIT(&f->queue);

    return f;
}

static void tear_down(void *data)
{
    struct fixture *f = data;

    free(f);
}

struct __item
{
    int value;
    void *queue[2];
};

/**
 * Initialize and push the given items to the queue. Each item will have a value
 * equal to its index plus one.
 */
#define __push(F, ITEMS)                               \
    {                                                  \
        int n = sizeof ITEMS / sizeof ITEMS[0];        \
        int i;                                         \
                                                       \
        for (i = 0; i < n; i++) {                      \
            struct __item *item = &items[i];           \
            item->value = i + 1;                       \
            RAFT__QUEUE_PUSH(&F->queue, &item->queue); \
        }                                              \
    }

/**
 * Remove the i'th item among the given ones.
 */
#define __remove(ITEMS, I)                   \
    {                                        \
        RAFT__QUEUE_REMOVE(&ITEMS[I].queue); \
    }

/**
 * Assert that the item at the head of the queue has the given value.
 */
#define __assert_head(F, VALUE)                              \
    {                                                        \
        raft__queue *head = RAFT__QUEUE_HEAD(&F->queue);     \
        struct __item *item;                                 \
                                                             \
        item = RAFT__QUEUE_DATA(head, struct __item, queue); \
        munit_assert_int(item->value, ==, VALUE);            \
    }

/**
 * Assert that the item at the tail of the queue has the given value.
 */
#define __assert_tail(F, VALUE)                              \
    {                                                        \
        raft__queue *tail = RAFT__QUEUE_TAIL(&F->queue);     \
        struct __item *item;                                 \
                                                             \
        item = RAFT__QUEUE_DATA(tail, struct __item, queue); \
        munit_assert_int(item->value, ==, VALUE);            \
    }

/**
 * Assert that the queue is empty.
 */
#define __assert_is_empty(F)                                \
    {                                                       \
        munit_assert_true(RAFT__QUEUE_IS_EMPTY(&F->queue)); \
    }

/**
 * Assert that the queue is not empty.
 */
#define __assert_is_not_empty(F)                             \
    {                                                        \
        munit_assert_false(RAFT__QUEUE_IS_EMPTY(&F->queue)); \
    }

/**
 * RAFT__QUEUE_IS_EMPTY
 */

TEST_SUITE(is_empty);

TEST_SETUP(is_empty, setup);
TEST_TEAR_DOWN(is_empty, tear_down);

TEST_GROUP(is_empty, success);

TEST_CASE(is_empty, success, yes, NULL)
{
    struct fixture *f = data;

    (void)params;

    __assert_is_empty(f);

    return MUNIT_OK;
}

TEST_CASE(is_empty, success, no, NULL)
{
    struct fixture *f = data;
    struct __item items[1];

    (void)params;

    __push(f, items);

    __assert_is_not_empty(f);

    return MUNIT_OK;
}

/**
 * RAFT__QUEUE_PUSH
 */

TEST_SUITE(push);

TEST_SETUP(push, setup);
TEST_TEAR_DOWN(push, tear_down);

TEST_GROUP(push, success);

TEST_CASE(push, success, one, NULL)
{
    struct fixture *f = data;
    struct __item items[1];

    (void)params;

    __push(f, items);

    __assert_head(f, 1);

    return MUNIT_OK;
}

TEST_CASE(push, success, two, NULL)
{
    struct fixture *f = data;
    struct __item items[2];
    int i;

    (void)params;

    __push(f, items);

    for (i = 0; i < 2; i++) {
        __assert_head(f, i + 1);

        __remove(items, i);
    }

    __assert_is_empty(f);

    return MUNIT_OK;
}

/**
 * RAFT__QUEUE_REMOVE
 */

TEST_SUITE(remove);

TEST_SETUP(remove, setup);
TEST_TEAR_DOWN(remove, tear_down);

TEST_GROUP(remove, success);

TEST_CASE(remove, success, first, NULL)
{
    struct fixture *f = data;
    struct __item items[3];

    (void)params;

    __push(f, items);

    __remove(items, 0);

    __assert_head(f, 2);

    return MUNIT_OK;
}

TEST_CASE(remove, success, second, NULL)
{
    struct fixture *f = data;
    struct __item items[3];

    (void)params;

    __push(f, items);

    __remove(items, 1);

    __assert_head(f, 1);

    return MUNIT_OK;
}

TEST_CASE(remove, success, third, NULL)
{
    struct fixture *f = data;
    struct __item items[3];

    (void)params;

    __push(f, items);

    __remove(items, 2);

    __assert_head(f, 1);

    return MUNIT_OK;
}

/**
 * RAFT__QUEUE_TAIL
 */

TEST_SUITE(tail);

TEST_SETUP(tail, setup);
TEST_TEAR_DOWN(tail, tear_down);

TEST_GROUP(tail, success);

TEST_CASE(tail, success, one, NULL)
{
    struct fixture *f = data;
    struct __item items[1];

    (void)params;

    __push(f, items);

    __assert_tail(f, 1);

    return MUNIT_OK;
}

TEST_CASE(tail, success, two, NULL)
{
    struct fixture *f = data;
    struct __item items[2];

    (void)params;

    __push(f, items);

    __assert_tail(f, 2);

    return MUNIT_OK;
}

TEST_CASE(tail, success, three, NULL)
{
    struct fixture *f = data;
    struct __item items[3];

    (void)params;

    __push(f, items);

    __assert_tail(f, 3);

    return MUNIT_OK;
}

/**
 * RAFT__QUEUE_FOREACH
 */

TEST_SUITE(foreach);

TEST_SETUP(foreach, setup);
TEST_TEAR_DOWN(foreach, tear_down);

/* Loop through a queue of zero items. */
TEST_CASE(foreach, zero, NULL)
{
    struct fixture *f = data;
    raft__queue *head;
    int count = 0;

    (void)params;

    RAFT__QUEUE_FOREACH(head, &f->queue) {
      count++;
    }

    munit_assert_int(count, ==, 0);

    return MUNIT_OK;
}

/* Loop through a queue of one item. */
TEST_CASE(foreach, one, NULL)
{
    struct fixture *f = data;
    struct __item items[1];
    raft__queue *head;
    int count = 0;

    (void)params;

    __push(f, items);

    RAFT__QUEUE_FOREACH(head, &f->queue) {
      count++;
    }

    munit_assert_int(count, ==, 1);

    return MUNIT_OK;
}

/* Loop through a queue of two items. */
TEST_CASE(foreach, two, NULL)
{
    struct fixture *f = data;
    struct __item items[2];
    raft__queue *head;
    int count = 0;

    (void)params;

    __push(f, items);

    RAFT__QUEUE_FOREACH(head, &f->queue) {
      count++;
    }

    munit_assert_int(count, ==, 2);

    return MUNIT_OK;
}

