#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

#define MIN_CELLS 1024
#define BIO_PRISON_MAX_RANGE 1024
#define BIO_PRISON_MAX_RANGE_SHIFT 10

// Simplified types from Linux kernel
typedef uint64_t dm_block_t;
typedef uint32_t dm_thin_id;
typedef int gfp_t;
#define GFP_KERNEL 0

// Simplified bio structure
struct bio {
    int bi_status;
    void *bi_private;
    struct bio *bi_next;
};

struct bio_list {
    struct bio *head;
    struct bio *tail;
};

// Key structure for identifying cells
struct dm_cell_key {
    int virtual;
    dm_thin_id dev;
    dm_block_t block_begin;
    dm_block_t block_end;
};

// Red-black tree node
struct rb_node {
    struct rb_node *rb_left;
    struct rb_node *rb_right;
    struct rb_node *rb_parent;
    int rb_color;
};

struct rb_root {
    struct rb_node *rb_node;
};

// Cell structure
struct dm_bio_prison_cell {
    struct dm_cell_key key;
    struct bio *holder;
    struct bio_list bios;
    struct rb_node node;
};

// Prison region structure
struct prison_region {
    pthread_spinlock_t lock;
    struct rb_root cell;
};

// Main prison structure
struct dm_bio_prison {
    struct dm_bio_prison_cell *cell_pool;
    int cell_pool_size;
    int cell_pool_used;
    unsigned int num_locks;
    struct prison_region *regions;
};

// Helper functions
void bio_list_init(struct bio_list *bl) {
    bl->head = bl->tail = NULL;
}

void bio_list_add(struct bio_list *bl, struct bio *bio) {
    bio->bi_next = NULL;
    if (!bl->head) {
        bl->head = bl->tail = bio;
    } else {
        bl->tail->bi_next = bio;
        bl->tail = bio;
    }
}

struct bio *bio_list_pop(struct bio_list *bl) {
    struct bio *bio = bl->head;
    if (bio) {
        bl->head = bio->bi_next;
        if (!bl->head)
            bl->tail = NULL;
        bio->bi_next = NULL;
    }
    return bio;
}

// RB-tree functions (simplified)
void rb_link_node(struct rb_node *node, struct rb_node *parent, struct rb_node **rb_link) {
    node->rb_parent = parent;
    node->rb_color = 1;
    node->rb_left = node->rb_right = NULL;
    *rb_link = node;
}

// Prison functions
struct dm_bio_prison *dm_bio_prison_create(void) {
    struct dm_bio_prison *prison = calloc(1, sizeof(*prison));
    if (!prison)
        return NULL;

    prison->num_locks = 16; // Simplified from kernel version
    prison->regions = calloc(prison->num_locks, sizeof(struct prison_region));
    if (!prison->regions) {
        free(prison);
        return NULL;
    }

    prison->cell_pool_size = MIN_CELLS;
    prison->cell_pool = calloc(prison->cell_pool_size, sizeof(struct dm_bio_prison_cell));
    if (!prison->cell_pool) {
        free(prison->regions);
        free(prison);
        return NULL;
    }

    for (unsigned int i = 0; i < prison->num_locks; i++) {
        pthread_spin_init(&prison->regions[i].lock, PTHREAD_PROCESS_PRIVATE);
        prison->regions[i].cell.rb_node = NULL;
    }

    return prison;
}

void dm_bio_prison_destroy(struct dm_bio_prison *prison) {
    if (!prison)
        return;

    for (unsigned int i = 0; i < prison->num_locks; i++)
        pthread_spin_destroy(&prison->regions[i].lock);

    free(prison->regions);
    free(prison->cell_pool);
    free(prison);
}

struct dm_bio_prison_cell *dm_bio_prison_alloc_cell(struct dm_bio_prison *prison, gfp_t gfp) {
    if (prison->cell_pool_used >= prison->cell_pool_size)
        return NULL;
    return &prison->cell_pool[prison->cell_pool_used++];
}

void dm_bio_prison_free_cell(struct dm_bio_prison *prison, struct dm_bio_prison_cell *cell) {
    // In this simplified version, we don't actually free cells
    prison->cell_pool_used--;
}

static int cmp_keys(struct dm_cell_key *lhs, struct dm_cell_key *rhs) {
    if (lhs->virtual != rhs->virtual)
        return lhs->virtual - rhs->virtual;
    if (lhs->dev != rhs->dev)
        return lhs->dev - rhs->dev;
    if (lhs->block_begin != rhs->block_begin)
        return lhs->block_begin < rhs->block_begin ? -1 : 1;
    if (lhs->block_end != rhs->block_end)
        return lhs->block_end < rhs->block_end ? -1 : 1;
    return 0;
}

static void __setup_new_cell(struct dm_cell_key *key, struct bio *holder,
                           struct dm_bio_prison_cell *cell) {
    memcpy(&cell->key, key, sizeof(cell->key));
    cell->holder = holder;
    bio_list_init(&cell->bios);
}

bool dm_cell_key_has_valid_range(struct dm_cell_key *key) {
    dm_block_t range = key->block_end - key->block_begin;
    return range <= BIO_PRISON_MAX_RANGE &&
           !(key->block_begin & (BIO_PRISON_MAX_RANGE - 1));
}

// Test program
int main() {
    printf("Creating bio prison...\n");
    struct dm_bio_prison *prison = dm_bio_prison_create();
    if (!prison) {
        printf("Failed to create bio prison\n");
        return 1;
    }

    // Create a test bio
    struct bio test_bio = {
        .bi_status = 0,
        .bi_private = NULL,
        .bi_next = NULL
    };

    // Create a test key
    struct dm_cell_key key = {
        .virtual = 1,
        .dev = 0,
        .block_begin = 0,
        .block_end = 1024
    };

    printf("Allocating prison cell...\n");
    struct dm_bio_prison_cell *cell = dm_bio_prison_alloc_cell(prison, GFP_KERNEL);
    if (!cell) {
        printf("Failed to allocate cell\n");
        dm_bio_prison_destroy(prison);
        return 1;
    }

    printf("Setting up new cell...\n");
    __setup_new_cell(&key, &test_bio, cell);

    printf("Testing key range validation...\n");
    if (dm_cell_key_has_valid_range(&key)) {
        printf("Key range is valid\n");
    } else {
        printf("Key range is invalid\n");
    }

    printf("Adding additional bio to cell...\n");
    struct bio additional_bio = {
        .bi_status = 0,
        .bi_private = NULL,
        .bi_next = NULL
    };
    bio_list_add(&cell->bios, &additional_bio);

    printf("Freeing cell...\n");
    dm_bio_prison_free_cell(prison, cell);

    printf("Destroying bio prison...\n");
    dm_bio_prison_destroy(prison);

    printf("Test completed successfully\n");
    return 0;
}
