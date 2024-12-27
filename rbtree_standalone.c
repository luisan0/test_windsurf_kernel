#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stddef.h>

// Red-Black Tree node structure
struct rb_node {
    unsigned long  __rb_parent_color;
    struct rb_node *rb_right;
    struct rb_node *rb_left;
} __attribute__((aligned(sizeof(long))));

// Red-Black Tree root structure
struct rb_root {
    struct rb_node *rb_node;
};

#define rb_parent(r)    ((struct rb_node *)((r)->__rb_parent_color & ~3))
#define RB_RED          0
#define RB_BLACK        1
#define rb_is_red(r)    (!rb_color(r))
#define rb_is_black(r)  rb_color(r)
#define rb_color(r)     ((r)->__rb_parent_color & 1)
#define rb_set_parent(r, p)  \
    ((r)->__rb_parent_color = rb_color(r) | (unsigned long)(p))
#define rb_set_parent_color(r, p, c)  \
    ((r)->__rb_parent_color = (unsigned long)(p) | (c))
#define rb_set_color(r, c) \
    ((r)->__rb_parent_color = rb_parent(r) | (c))

// Test structure that contains the rb_node
struct test_node {
    int key;
    struct rb_node node;
};

#define container_of(ptr, type, member) ({          \
    const typeof(((type *)0)->member) * __mptr = (ptr);    \
    (type *)((char *)__mptr - offsetof(type, member)); })

static inline void rb_link_node(struct rb_node *node, struct rb_node *parent,
                              struct rb_node **rb_link)
{
    node->__rb_parent_color = (unsigned long)parent;
    node->rb_left = node->rb_right = NULL;
    *rb_link = node;
}

static inline void rb_rotate_set_parents(struct rb_node *old, struct rb_node *new,
                                       struct rb_root *root, int color)
{
    struct rb_node *parent = rb_parent(old);
    new->__rb_parent_color = old->__rb_parent_color;
    rb_set_parent_color(old, new, color);
    
    if (parent) {
        if (parent->rb_left == old)
            parent->rb_left = new;
        else
            parent->rb_right = new;
    } else
        root->rb_node = new;
}

void rb_insert_color(struct rb_node *node, struct rb_root *root)
{
    struct rb_node *parent = rb_parent(node), *gparent, *tmp;

    while (true) {
        if (!parent) {
            rb_set_parent_color(node, NULL, RB_BLACK);
            break;
        }

        if (rb_is_black(parent))
            break;

        gparent = rb_parent(parent);
        tmp = gparent->rb_right;

        if (parent != tmp) {
            if (tmp && rb_is_red(tmp)) {
                rb_set_parent_color(tmp, gparent, RB_BLACK);
                rb_set_parent_color(parent, gparent, RB_BLACK);
                node = gparent;
                parent = rb_parent(node);
                rb_set_parent_color(node, parent, RB_RED);
                continue;
            }

            tmp = parent->rb_right;
            if (node == tmp) {
                tmp = node->rb_left;
                parent->rb_right = tmp;
                node->rb_left = parent;
                if (tmp)
                    rb_set_parent_color(tmp, parent, RB_BLACK);
                rb_set_parent_color(parent, node, RB_RED);
                parent = node;
                tmp = node->rb_right;
            }

            gparent->rb_left = tmp;
            parent->rb_right = gparent;
            if (tmp)
                rb_set_parent_color(tmp, gparent, RB_BLACK);
            rb_rotate_set_parents(gparent, parent, root, RB_RED);
            break;
        } else {
            tmp = gparent->rb_left;
            if (tmp && rb_is_red(tmp)) {
                rb_set_parent_color(tmp, gparent, RB_BLACK);
                rb_set_parent_color(parent, gparent, RB_BLACK);
                node = gparent;
                parent = rb_parent(node);
                rb_set_parent_color(node, parent, RB_RED);
                continue;
            }

            tmp = parent->rb_left;
            if (node == tmp) {
                tmp = node->rb_right;
                parent->rb_left = tmp;
                node->rb_right = parent;
                if (tmp)
                    rb_set_parent_color(tmp, parent, RB_BLACK);
                rb_set_parent_color(parent, node, RB_RED);
                parent = node;
                tmp = node->rb_left;
            }

            gparent->rb_right = tmp;
            parent->rb_left = gparent;
            if (tmp)
                rb_set_parent_color(tmp, gparent, RB_BLACK);
            rb_rotate_set_parents(gparent, parent, root, RB_RED);
            break;
        }
    }
}

static void rb_erase_color(struct rb_node *parent, struct rb_root *root)
{
    struct rb_node *node = NULL, *sibling, *tmp1, *tmp2;

    while (true) {
        sibling = parent->rb_right;
        if (node != sibling) {
            if (rb_is_red(sibling)) {
                tmp1 = sibling->rb_left;
                parent->rb_right = tmp1;
                sibling->rb_left = parent;
                rb_set_parent_color(tmp1, parent, RB_BLACK);
                rb_rotate_set_parents(parent, sibling, root, RB_RED);
                sibling = tmp1;
            }
            tmp1 = sibling->rb_right;
            if (!tmp1 || rb_is_black(tmp1)) {
                tmp2 = sibling->rb_left;
                if (!tmp2 || rb_is_black(tmp2)) {
                    rb_set_parent_color(sibling, parent, RB_RED);
                    if (rb_is_red(parent)) {
                        rb_set_black(parent);
                    } else {
                        node = parent;
                        parent = rb_parent(node);
                        if (parent)
                            continue;
                    }
                    break;
                }
                tmp1 = tmp2;
                sibling->rb_left = tmp2->rb_right;
                tmp2->rb_right = sibling;
                parent->rb_right = tmp2;
                if (tmp1)
                    rb_set_parent_color(tmp1, sibling, RB_BLACK);
                tmp2 = sibling;
                sibling = tmp1;
            }
            sibling->rb_right = tmp1;
            parent->rb_right = sibling;
            rb_set_parent_color(tmp1, sibling, RB_BLACK);
            tmp2 = sibling->rb_left;
            sibling->rb_left = parent;
            rb_set_parent_color(tmp2, parent, RB_BLACK);
            rb_rotate_set_parents(parent, sibling, root, RB_BLACK);
            break;
        } else {
            sibling = parent->rb_left;
            if (rb_is_red(sibling)) {
                tmp1 = sibling->rb_right;
                parent->rb_left = tmp1;
                sibling->rb_right = parent;
                rb_set_parent_color(tmp1, parent, RB_BLACK);
                rb_rotate_set_parents(parent, sibling, root, RB_RED);
                sibling = tmp1;
            }
            tmp1 = sibling->rb_left;
            if (!tmp1 || rb_is_black(tmp1)) {
                tmp2 = sibling->rb_right;
                if (!tmp2 || rb_is_black(tmp2)) {
                    rb_set_parent_color(sibling, parent, RB_RED);
                    if (rb_is_red(parent))
                        rb_set_black(parent);
                    else {
                        node = parent;
                        parent = rb_parent(node);
                        if (parent)
                            continue;
                    }
                    break;
                }
                tmp1 = tmp2;
                sibling->rb_right = tmp2->rb_left;
                tmp2->rb_left = sibling;
                parent->rb_left = tmp2;
                if (tmp1)
                    rb_set_parent_color(tmp1, sibling, RB_BLACK);
                tmp2 = sibling;
                sibling = tmp1;
            }
            sibling->rb_left = tmp1;
            parent->rb_left = sibling;
            rb_set_parent_color(tmp1, sibling, RB_BLACK);
            tmp2 = sibling->rb_right;
            sibling->rb_right = parent;
            rb_set_parent_color(tmp2, parent, RB_BLACK);
            rb_rotate_set_parents(parent, sibling, root, RB_BLACK);
            break;
        }
    }
}

// Insert a new node into the tree
struct test_node *insert_node(struct rb_root *root, int key)
{
    struct rb_node **new = &(root->rb_node), *parent = NULL;
    struct test_node *test_node = malloc(sizeof(struct test_node));
    
    test_node->key = key;

    // Figure out where to put new node
    while (*new) {
        struct test_node *this = container_of(*new, struct test_node, node);
        parent = *new;
        if (key < this->key)
            new = &((*new)->rb_left);
        else if (key > this->key)
            new = &((*new)->rb_right);
        else
            return NULL;
    }

    // Add new node and rebalance tree
    rb_link_node(&test_node->node, parent, new);
    rb_insert_color(&test_node->node, root);

    return test_node;
}

// Find a node in the tree
struct test_node *search_node(struct rb_root *root, int key)
{
    struct rb_node *node = root->rb_node;

    while (node) {
        struct test_node *data = container_of(node, struct test_node, node);

        if (key < data->key)
            node = node->rb_left;
        else if (key > data->key)
            node = node->rb_right;
        else
            return data;
    }
    return NULL;
}

// Inorder traversal of the tree
void inorder(struct rb_node *node)
{
    if (node) {
        inorder(node->rb_left);
        struct test_node *test_node = container_of(node, struct test_node, node);
        printf("%d ", test_node->key);
        inorder(node->rb_right);
    }
}

int main()
{
    struct rb_root root = {NULL};
    
    // Insert some test values
    printf("Inserting values: 10, 20, 30, 15, 25, 5\n");
    insert_node(&root, 10);
    insert_node(&root, 20);
    insert_node(&root, 30);
    insert_node(&root, 15);
    insert_node(&root, 25);
    insert_node(&root, 5);

    // Print the tree in-order
    printf("Tree in-order traversal: ");
    inorder(root.rb_node);
    printf("\n");

    // Test searching
    int search_key = 15;
    struct test_node *found = search_node(&root, search_key);
    if (found)
        printf("Found key %d in the tree\n", search_key);
    else
        printf("Key %d not found in the tree\n", search_key);

    search_key = 40;
    found = search_node(&root, search_key);
    if (found)
        printf("Found key %d in the tree\n", search_key);
    else
        printf("Key %d not found in the tree\n", search_key);

    return 0;
}
