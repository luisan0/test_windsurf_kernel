#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

// Type definitions to simulate Linux kernel types
typedef uint64_t resource_size_t;
typedef unsigned long dma_addr_t;

// Red-black tree definitions
struct rb_node {
    struct rb_node *rb_left;
    struct rb_node *rb_right;
    struct rb_node *rb_parent;
    int color;
};

struct rb_root {
    struct rb_node *rb_node;
};

#define RB_RED      0
#define RB_BLACK    1

// Simulated PCI device structure
struct pci_dev {
    int vendor;
    int device;
    char name[32];
    // Simplified resource array
    struct resource {
        resource_size_t start;
        resource_size_t end;
        unsigned long flags;
    } resource[6];  // PCI has 6 BARs
};

// Simulated EEH device structure
struct eeh_dev {
    struct pci_dev *pdev;
    int pe_config_addr;
};

// Address range structure for the cache
struct pci_io_addr_range {
    struct rb_node rb_node;
    resource_size_t addr_lo;
    resource_size_t addr_hi;
    struct eeh_dev *edev;
    struct pci_dev *pcidev;
    unsigned long flags;
};

// The main cache structure
static struct pci_io_addr_cache {
    struct rb_root rb_root;
    pthread_spinlock_t piar_lock;
} pci_io_addr_cache_root;

// Helper function to create a new range
static struct pci_io_addr_range *new_range(resource_size_t alo, resource_size_t ahi,
                                         struct eeh_dev *edev, struct pci_dev *dev,
                                         unsigned long flags) {
    struct pci_io_addr_range *piar = malloc(sizeof(*piar));
    if (!piar)
        return NULL;
    
    piar->addr_lo = alo;
    piar->addr_hi = ahi;
    piar->edev = edev;
    piar->pcidev = dev;
    piar->flags = flags;
    return piar;
}

// Initialize the cache
void eeh_addr_cache_init(void) {
    pci_io_addr_cache_root.rb_root.rb_node = NULL;
    pthread_spin_init(&pci_io_addr_cache_root.piar_lock, PTHREAD_PROCESS_PRIVATE);
}

// Helper function to simulate rb_link_node
static void rb_link_node(struct rb_node *node, struct rb_node *parent,
                        struct rb_node **rb_link) {
    node->rb_parent = parent;
    node->rb_left = node->rb_right = NULL;
    node->color = RB_RED;
    *rb_link = node;
}

// Insert a range into the tree
static int __eeh_addr_cache_insert(struct pci_io_addr_range *piar) {
    struct rb_node **p = &pci_io_addr_cache_root.rb_root.rb_node;
    struct rb_node *parent = NULL;
    struct pci_io_addr_range *tmp;

    while (*p) {
        parent = *p;
        tmp = (struct pci_io_addr_range *)parent;

        if (piar->addr_hi < tmp->addr_lo)
            p = &(*p)->rb_left;
        else if (piar->addr_lo > tmp->addr_hi)
            p = &(*p)->rb_right;
        else
            return -1; // Overlap detected
    }

    rb_link_node(&piar->rb_node, parent, p);
    return 0;
}

// Get device from address
struct eeh_dev *eeh_addr_cache_get_dev(unsigned long addr) {
    struct eeh_dev *edev;
    struct rb_node *n = pci_io_addr_cache_root.rb_root.rb_node;

    pthread_spin_lock(&pci_io_addr_cache_root.piar_lock);

    while (n) {
        struct pci_io_addr_range *piar =
            (struct pci_io_addr_range *)n;

        if (addr < piar->addr_lo)
            n = n->rb_left;
        else if (addr > piar->addr_hi)
            n = n->rb_right;
        else {
            edev = piar->edev;
            pthread_spin_unlock(&pci_io_addr_cache_root.piar_lock);
            return edev;
        }
    }

    pthread_spin_unlock(&pci_io_addr_cache_root.piar_lock);
    return NULL;
}

// Debug function to print the cache contents
void eeh_addr_cache_print(void) {
    struct rb_node *n = pci_io_addr_cache_root.rb_root.rb_node;
    printf("EEH Address Cache Contents:\n");
    printf("%-20s %-20s %-20s\n", "Start Address", "End Address", "Device");
    
    while (n) {
        struct pci_io_addr_range *piar = (struct pci_io_addr_range *)n;
        printf("0x%016lx 0x%016lx %s\n",
               (unsigned long)piar->addr_lo,
               (unsigned long)piar->addr_hi,
               piar->pcidev ? piar->pcidev->name : "unknown");
        n = n->rb_right; // Simple traversal for demonstration
    }
}

// Test program
int main() {
    printf("Initializing EEH address cache...\n");
    eeh_addr_cache_init();

    // Create some test PCI devices
    struct pci_dev pdev1 = {
        .vendor = 0x1234,
        .device = 0x5678,
        .name = "Test Device 1"
    };
    pdev1.resource[0].start = 0x1000;
    pdev1.resource[0].end = 0x1FFF;

    struct pci_dev pdev2 = {
        .vendor = 0x5678,
        .device = 0x1234,
        .name = "Test Device 2"
    };
    pdev2.resource[0].start = 0x2000;
    pdev2.resource[0].end = 0x2FFF;

    // Create EEH devices
    struct eeh_dev edev1 = { .pdev = &pdev1, .pe_config_addr = 0x100 };
    struct eeh_dev edev2 = { .pdev = &pdev2, .pe_config_addr = 0x200 };

    // Insert ranges into cache
    printf("Adding devices to cache...\n");
    struct pci_io_addr_range *range1 = new_range(0x1000, 0x1FFF, &edev1, &pdev1, 0);
    struct pci_io_addr_range *range2 = new_range(0x2000, 0x2FFF, &edev2, &pdev2, 0);

    pthread_spin_lock(&pci_io_addr_cache_root.piar_lock);
    __eeh_addr_cache_insert(range1);
    __eeh_addr_cache_insert(range2);
    pthread_spin_unlock(&pci_io_addr_cache_root.piar_lock);

    // Test address lookups
    printf("\nTesting address lookups:\n");
    unsigned long test_addrs[] = { 0x1500, 0x2500, 0x3000 };
    for (int i = 0; i < 3; i++) {
        struct eeh_dev *found = eeh_addr_cache_get_dev(test_addrs[i]);
        printf("Looking up address 0x%lx: ", test_addrs[i]);
        if (found) {
            printf("Found device: %s\n", found->pdev->name);
        } else {
            printf("No device found\n");
        }
    }

    // Print cache contents
    printf("\n");
    eeh_addr_cache_print();

    // Cleanup
    pthread_spin_destroy(&pci_io_addr_cache_root.piar_lock);
    free(range1);
    free(range2);

    printf("\nTest completed successfully\n");
    return 0;
}
