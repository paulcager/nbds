/* 
 * Written by Josh Dybnis and released to the public domain, as explained at
 * http://creativecommons.org/licenses/publicdomain
 *
 * Implementation of the lock-free skiplist data-structure created by Maurice Herlihy, Yossi Lev,
 * and Nir Shavit. See Herlihy's and Shivit's book "The Art of Multiprocessor Programming".
 * http://www.amazon.com/Art-Multiprocessor-Programming-Maurice-Herlihy/dp/0123705916/
 *
 * See also Kir Fraser's dissertation "Practical Lock Freedom".
 * www.cl.cam.ac.uk/techreports/UCAM-CL-TR-579.pdf
 *
 * This code is written for the x86 memory-model. The algorithim depends on certain stores and
 * loads being ordered. Be careful, this code won't work correctly on platforms with weaker memory
 * models if you don't add memory barriers in the right places.
 */
#include <stdio.h>
#include <string.h>

#include "common.h"
#include "runtime.h"
#include "mlocal.h"
#include "nstring.h"
#include "mem.h"
#include "tls.h"

// Setting MAX_LEVEL to 0 essentially makes this data structure the Harris-Michael lock-free list
// (in list.c).
#define MAX_LEVEL 31

typedef struct node {
    nstring_t *key;
    uint64_t val;
    int top_level;
    struct node *next[];
} node_t;

struct sl {
    node_t *head;
};

static int random_level (void) {
    unsigned r = nbd_rand();
    if (r & 1)
        return 0;
#if MAX_LEVEL < 31
    r |= 1 << (MAX_LEVEL+1);
#endif
    int n = __builtin_ctz(r)-1;
    assert(n <= MAX_LEVEL);
    return n;
}

static node_t *node_alloc (int level, const void *key_data, uint32_t key_len, uint64_t val) {
    assert(level >= 0 && level <= MAX_LEVEL);
    size_t sz = sizeof(node_t) + (level + 1) * sizeof(node_t *);
    node_t *item = (node_t *)nbd_malloc(sz);
    memset(item, 0, sz);
    // If <key_len> is -1 it indicates <key_data> is an integer and not a pointer
    item->key = (key_len == (unsigned)-1) 
              ? (void *)TAG_VALUE(key_data) 
              : ns_alloc(key_data, key_len); 
    item->val = val;
    item->top_level = level;
    return item;
}

static void node_free (node_t *item) {
    if (!IS_TAGGED(item->key)) {
        nbd_free(item->key);
    }
    nbd_free(item);
}

static void node_defer_free (node_t *item) {
    if (!IS_TAGGED(item->key)) {
        nbd_defer_free(item->key);
    }
    nbd_defer_free(item);
}

skiplist_t *sl_alloc (void) {
    skiplist_t *sl = (skiplist_t *)nbd_malloc(sizeof(skiplist_t));
    sl->head = node_alloc(MAX_LEVEL, " ", 0, 0);
    memset(sl->head->next, 0, (MAX_LEVEL+1) * sizeof(skiplist_t *));
    return sl;
}

void ll_free (list_t *ll) {
}

static node_t *find_preds (node_t **preds, node_t **succs, int n, skiplist_t *sl, const void *key_data, uint32_t key_len, int help_remove) {
    node_t *pred = sl->head;
    node_t *item = NULL;
    TRACE("s2", "find_preds: searching for key %p in sl (head is %p)", key_data, pred);
    int d;
    int start_level = MAX_LEVEL;
#if MAX_LEVEL > 2
    // Optimization for small lists. No need to traverse empty higher levels.
    start_level = 2;
    while (pred->next[start_level+1] != NULL) {
        start_level += start_level - 1;
        if (EXPECT_FALSE(start_level >= MAX_LEVEL)) {
            start_level = MAX_LEVEL;
            break;
        }
    }
    if (EXPECT_FALSE(start_level < n)) {
        start_level = n;
    }
#endif

    // Traverse the levels of <sl> from the top level to the bottom
    for (int level = start_level; level >= 0; --level) {
        TRACE("s3", "find_preds: level %llu", level, 0);
        item = pred->next[level];
        if (EXPECT_FALSE(IS_TAGGED(item))) {
            TRACE("s2", "find_preds: pred %p is marked for removal (item %p); retry", pred, item);
            return find_preds(preds, succs, n, sl, key_data, key_len, help_remove); // retry
        }
        while (item != NULL) {
            node_t *next = item->next[level];

            // A tag means an item is logically removed but not physically unlinked yet.
            while (EXPECT_FALSE(IS_TAGGED(next))) {

                // Skip over logically removed items.
                if (!help_remove) {
                    item = (node_t *)STRIP_TAG(item->next);
                    if (EXPECT_FALSE(item == NULL))
                        break;
                    TRACE("s3", "find_preds: skipping marked item %p (next is %p)", item, next);
                    next = item->next[level];
                    continue;
                }

                // Unlink logically removed items.
                node_t *other;
                TRACE("s3", "find_preds: unlinking marked item %p; next is %p", item, next);
                if ((other = SYNC_CAS(&pred->next[level], item, STRIP_TAG(next))) == item) {
                    item = (node_t *)STRIP_TAG(next);
                    if (EXPECT_FALSE(item == NULL))
                        break;
                    next = item->next[level];
                    TRACE("s3", "find_preds: now the current item is %p next is %p", item, next);

                    // The thread that completes the unlink should free the memory.
                    if (level == 0) { node_defer_free(other); }
                } else {
                    TRACE("s2", "find_preds: lost race to unlink item %p from pred %p", item, pred);
                    TRACE("s2", "find_preds: pred's link changed to %p", other, 0);
                    if (IS_TAGGED(other))
                        return find_preds(preds, succs, n, sl, key_data, key_len, help_remove); // retry
                    item = other;
                    if (EXPECT_FALSE(item == NULL))
                        break;
                    next = item->next[level];
                }
            }

            if (EXPECT_FALSE(item == NULL))
                break;

            TRACE("s3", "find_preds: visiting item %p (next is %p)", item, next);
            TRACE("s4", "find_preds: key %p val %p", STRIP_TAG(item->key), item->val);

            // A tagged key is an integer, otherwise it is a pointer to a string
            if (IS_TAGGED(item->key)) {
                d = (STRIP_TAG(item->key) - (uint64_t)key_data);
            } else {
                int item_key_len = item->key->len;
                int len = (key_len < item_key_len) ? key_len : item_key_len;
                d = memcmp(item->key->data, key_data, len);
                if (d == 0) { d = item_key_len - key_len; }
            }

            if (d >= 0) {
                TRACE("s2", "find_preds: found pred %p item %p", pred, item);
                break;
            }

            pred = item;
            item = next;
        }

        // The cast to unsigned is for the case when n is -1.
        if ((unsigned)level <= (unsigned)n) { 
            if (preds != NULL) {
                preds[level] = pred;
            }
            if (succs != NULL) {
                succs[level] = item;
            }
        }
    }

    // fill in empty levels
    if (n == -1 && item != NULL) {
        for (int level = start_level + 1; level <= item->top_level; ++level) {
            preds[level] = sl->head;
        }
    }

    if (d == 0) {
        TRACE("s2", "find_preds: found matching item %p in skiplist, pred is %p", item, pred);
        return item;
    }
    TRACE("s2", "find_preds: found proper place for key %p in skiplist, pred is %p. returning null", key_data, pred);
    return NULL;
}

// Fast find that does not help unlink partially removed nodes and does not return the node's predecessors.
uint64_t sl_lookup (skiplist_t *sl, const void *key_data, uint32_t key_len) {
    TRACE("s1", "sl_lookup: searching for key %p in skiplist %p", key_data, sl);
    node_t *item = find_preds(NULL, NULL, 0, sl, key_data, key_len, FALSE);

    // If we found an <item> matching the <key> return its value.
    if (item != NULL) {
        uint64_t val = item->val;
        if (val != DOES_NOT_EXIST) {
            TRACE("s1", "sl_lookup: found item %p. val %p. returning item", item, item->val);
            return val;
        }
    }

    TRACE("l1", "sl_lookup: no item in the skiplist matched the key", 0, 0);
    return DOES_NOT_EXIST;
}

uint64_t sl_cas (skiplist_t *sl, const void *key_data, uint32_t key_len, uint64_t expectation, uint64_t new_val) {
    TRACE("s1", "sl_cas: key %p skiplist %p", key_data, sl);
    TRACE("s1", "sl_cas: expectation %p new value %p", expectation, new_val);
    ASSERT((int64_t)new_val > 0);

    node_t *preds[MAX_LEVEL+1];
    node_t *nexts[MAX_LEVEL+1];
    node_t *new_item = NULL;
    int n = random_level();
    do {
        node_t *old_item = find_preds(preds, nexts, n, sl, key_data, key_len, TRUE);
        if (old_item == NULL) {

            // There was not an item in the skiplist that matches the key. 
            if (EXPECT_FALSE((int64_t)expectation > 0 || expectation == CAS_EXPECT_EXISTS)) {
                TRACE("l1", "sl_cas: the expectation was not met, the skiplist was not changed", 0, 0);
                return DOES_NOT_EXIST; // failure
            }

            ASSERT(expectation == CAS_EXPECT_DOES_NOT_EXIST || expectation == CAS_EXPECT_WHATEVER);

            // First insert <new_item> into the bottom level.
            TRACE("s3", "sl_cas: attempting to insert item between %p and %p", preds[0], nexts[0]);
            new_item = node_alloc(n, key_data, key_len, new_val);
            node_t *pred = preds[0];
            node_t *next = new_item->next[0] = nexts[0];
            for (int level = 1; level <= new_item->top_level; ++level) {
                new_item->next[level] = nexts[level];
            }
            node_t *other = SYNC_CAS(&pred->next[0], next, new_item);
            if (other == next) {
                TRACE("s3", "sl_cas: successfully inserted item %p at level 0", new_item, 0);
                break; // success
            }
            TRACE("s3", "sl_cas: failed to change pred's link: expected %p found %p", next, other);
            node_free(new_item);
            continue;
        }

        // Found an item in the skiplist that matches the key.
        uint64_t old_item_val = old_item->val;
        do {
            // If the item's value is DOES_NOT_EXIST it means another thread removed the node out from under us.
            if (EXPECT_FALSE(old_item_val == DOES_NOT_EXIST)) {
                TRACE("s2", "sl_cas: lost a race, found an item but another thread removed it. retry", 0, 0);
                break; // retry
            }

            if (EXPECT_FALSE(expectation == CAS_EXPECT_DOES_NOT_EXIST)) {
                TRACE("s1", "sl_cas: found an item %p in the skiplist that matched the key. the expectation was "
                        "not met, the skiplist was not changed", old_item, old_item_val);
                return old_item_val; // failure
            }

            // Use a CAS and not a SWAP. If the node is in the process of being removed and we used a SWAP, we could
            // replace DOES_NOT_EXIST with our value. Then another thread that is updating the value could think it
            // succeeded and return our value even though we indicated that the node has been removed. If the CAS 
            // fails it means another thread either removed the node or updated its value.
            uint64_t ret_val = SYNC_CAS(&old_item->val, old_item_val, new_val);
            if (ret_val == old_item_val) {
                TRACE("s1", "sl_cas: the CAS succeeded. updated the value of the item", 0, 0);
                return ret_val; // success
            }
            TRACE("s2", "sl_cas: lost a race. the CAS failed. another thread changed the item's value", 0, 0);

            old_item_val = ret_val;
        } while (1);
    } while (1);

    // Link <new_item> into <sl> from the bottom up.
    for (int level = 1; level <= new_item->top_level; ++level) {
        node_t *pred = preds[level];
        node_t *next = nexts[level];
        do {
            TRACE("s3", "sl_cas: attempting to insert item between %p and %p", pred, next);
            node_t *other = SYNC_CAS(&pred->next[level], next, new_item);
            if (other == next) {
                TRACE("s3", "sl_cas: successfully inserted item %p at level %llu", new_item, level);
                break; // success
            }
            TRACE("s3", "sl_cas: failed to change pred's link: expected %p found %p", next, other);
            find_preds(preds, nexts, new_item->top_level, sl, key_data, key_len, TRUE);
            pred = preds[level];
            next = nexts[level];

            // Update <new_item>'s next pointer
            do {
                // There in no need to continue linking in the item if another thread removed it.
                node_t *old_next = ((volatile node_t *)new_item)->next[level];
                if (IS_TAGGED(old_next))
                    return new_val;

                // Use a CAS so we do not inadvertantly stomp on a mark another thread placed on the item.
                if (old_next == next || SYNC_CAS(&new_item->next[level], old_next, next) == old_next)
                    break;
            } while (1);
        } while (1);
    }
    return new_val;
}

uint64_t sl_remove (skiplist_t *sl, const void *key_data, uint32_t key_len) {
    TRACE("s1", "sl_remove: removing item with key %p from skiplist %p", key_data, sl);
    node_t *preds[MAX_LEVEL+1];
    node_t *item = find_preds(preds, NULL, -1, sl, key_data, key_len, TRUE);
    if (item == NULL) {
        TRACE("s3", "sl_remove: remove failed, an item with a matching key does not exist in the skiplist", 0, 0);
        return DOES_NOT_EXIST;
    }

    // Mark <item> removed at each level of <sl> from the top down. This must be atomic. If multiple threads
    // try to remove the same item only one of them should succeed. Marking the bottom level establishes which of 
    // them succeeds.
    for (int level = item->top_level; level >= 0; --level) {
        if (EXPECT_FALSE(IS_TAGGED(item->next[level]))) {
            TRACE("s3", "sl_remove: %p is already marked for removal by another thread", item, 0);
            if (level == 0)
                return DOES_NOT_EXIST;
            continue;
        }
        node_t *next;
        node_t *old_next = item->next[level];
        do {
            next = old_next;
            old_next = SYNC_CAS(&item->next[level], next, TAG_VALUE(next));
            if (IS_TAGGED(old_next)) {
                TRACE("s2", "sl_remove: lost race -- %p is already marked for removal by another thread", item, 0);
                if (level == 0)
                    return DOES_NOT_EXIST;
            }
        } while (!IS_TAGGED(old_next) || next != old_next);
    }

    // This has to be an atomic swap in case another thread is updating the item while we are removing it. 
    uint64_t val = SYNC_SWAP(&item->val, DOES_NOT_EXIST); 
    TRACE("s2", "sl_remove: replaced item's val %p with DOES_NOT_EXIT", val, 0);

    // Unlink <item> from <sl>. If we lose a race to another thread just back off. It is safe to leave the
    // item partially unlinked for a later call (or some other thread) to physically unlink. By marking the
    // item earlier, we logically removed it. 
    int level = item->top_level;
    while (level >= 0) {
        node_t *pred = preds[level];
        node_t *next = item->next[level];
        TRACE("s2", "sl_remove: unlink the item by linking its pred %p to it's successor %p", pred, STRIP_TAG(next));
        node_t *other = NULL;
        if ((other = SYNC_CAS(&pred->next[level], item, STRIP_TAG(next))) != item) {
            TRACE("s1", "sl_remove: unlink failed; pred's link changed from %p to %p", item, other);
            return val;
        }
        --level; 
    }

    // The thread that completes the unlink should free the memory.
    TRACE("s1", "sl_remove: successfully unlinked item %p from the skiplist", item, 0);
    node_defer_free(item); 
    return val;
}

void sl_print (skiplist_t *sl) {
    for (int level = MAX_LEVEL; level >= 0; --level) {
        node_t *item = sl->head;
        if (item->next[level] == NULL)
            continue;
        printf("(%d) ", level);
        while (item) {
            node_t *next = item->next[level];
            printf("%s%p ", IS_TAGGED(next) ? "*" : "", item);
            item = (node_t *)STRIP_TAG(next);
        }
        printf("\n");
        fflush(stdout);
    }

    printf("\n");
    node_t *item = sl->head;
    while (item) {
        int is_marked = IS_TAGGED(item->next[0]);

        if (IS_TAGGED(item->key)) {
            printf("%s%p:%llx ", is_marked ? "*" : "", item, STRIP_TAG(item->key));
        } else {
            printf("%s%p:%s ", is_marked ? "*" : "", item, (char *)item->key->data);
        }
        if (item != sl->head) {
            printf("[%d]", item->top_level);
        } else {
            printf("[*]");
        }
        for (int level = 1; level <= item->top_level; ++level) {
            node_t *next = (node_t *)STRIP_TAG(item->next[level]);
            is_marked = IS_TAGGED(item->next[0]);
            printf(" %p%s", next, is_marked ? "*" : "");
            if (item == sl->head && item->next[level] == NULL)
                break;
        }
        printf("\n");
        fflush(stdout);
        item = (node_t *)STRIP_TAG(item->next[0]);
    }
}