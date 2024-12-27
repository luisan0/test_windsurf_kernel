/*
 * PCI Resource Management Test Program
 * This is a standalone simulation of PCI device resource management
 * Author: Cascade AI
 * Date: 2024-12-27
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <time.h>

/* PCI Configuration Space Registers */
#define PCI_COMMAND              0x04
#define PCI_COMMAND_IO          0x01
#define PCI_COMMAND_MEMORY      0x02
#define PCI_COMMAND_MASTER      0x04
#define PCI_HEADER_TYPE         0x0E
#define PCI_HEADER_TYPE_NORMAL  0x00
#define PCI_HEADER_TYPE_BRIDGE  0x01
#define PCI_CLASS_BRIDGE_HOST   0x0600

/* Resource Types */
#define IORESOURCE_IO           0x00000100
#define IORESOURCE_MEM          0x00000200
#define IORESOURCE_PREFETCH     0x00002000
#define IORESOURCE_UNSET        0x20000000
#define IORESOURCE_BUSY         0x80000000

/* PCI Resource Indices */
#define PCI_ROM_RESOURCE        6
#define PCI_BRIDGE_RESOURCES    7
#define PCI_NUM_RESOURCES       12

/* Default Alignments */
#define PCI_MIN_ALIGN           0x1000  /* 4KB */
#define PCI_DEFAULT_ALIGN       0x100000 /* 1MB */

/* Resource Structure */
struct resource {
    uint64_t start;
    uint64_t end;
    const char *name;
    unsigned long flags;
};

/* PCI Device Structure */
struct pci_dev {
    uint16_t vendor;
    uint16_t device;
    uint16_t subsystem_vendor;
    uint16_t subsystem_device;
    uint8_t revision;
    uint8_t hdr_type;
    uint32_t class;
    bool is_virtfn;
    struct resource resource[PCI_NUM_RESOURCES];
    uint16_t command;
    char name[64];
};

/* Function Declarations */
static void pci_read_config_word(struct pci_dev *dev, int where, uint16_t *val);
static void pci_write_config_word(struct pci_dev *dev, int where, uint16_t val);
static void pci_disable_bridge_window(struct pci_dev *dev);
static void pci_request_resource_alignment(struct pci_dev *dev, int bar, 
                                         uint64_t align, bool resize);
static uint64_t pci_specified_resource_alignment(struct pci_dev *dev, bool *resize);
static void pci_reassigndev_resource_alignment(struct pci_dev *dev);
static void print_resource_info(struct pci_dev *dev);

/* Helper Functions */
static void pci_read_config_word(struct pci_dev *dev, int where, uint16_t *val)
{
    /* Simulate reading from PCI config space */
    *val = dev->command;
}

static void pci_write_config_word(struct pci_dev *dev, int where, uint16_t val)
{
    /* Simulate writing to PCI config space */
    dev->command = val;
    printf("Writing config word 0x%04x to offset 0x%02x for device %s\n", 
           val, where, dev->name);
}

static void pci_disable_bridge_window(struct pci_dev *dev)
{
    if (dev->hdr_type != PCI_HEADER_TYPE_BRIDGE)
        return;

    printf("Disabling bridge windows for device %s\n", dev->name);
    for (int i = PCI_BRIDGE_RESOURCES; i < PCI_NUM_RESOURCES; i++) {
        struct resource *r = &dev->resource[i];
        if (!(r->flags & IORESOURCE_MEM))
            continue;
        r->start = 0;
        r->end = 0;
        r->flags |= IORESOURCE_UNSET;
    }
}

static uint64_t get_alignment_size(const char *str)
{
    uint64_t align = 0;
    char suffix;
    if (sscanf(str, "%lu%c", &align, &suffix) == 2) {
        switch (suffix) {
        case 'K':
        case 'k':
            align *= 1024;
            break;
        case 'M':
        case 'm':
            align *= 1024 * 1024;
            break;
        case 'G':
        case 'g':
            align *= 1024 * 1024 * 1024;
            break;
        default:
            printf("Invalid alignment suffix: %c\n", suffix);
            return 0;
        }
    }
    return align;
}

static void pci_request_resource_alignment(struct pci_dev *dev, int bar, 
                                         uint64_t align, bool resize)
{
    struct resource *r = &dev->resource[bar];
    uint64_t size;

    if (!(r->flags & (IORESOURCE_IO | IORESOURCE_MEM)))
        return;

    size = r->end - r->start + 1;
    if (size == 0)
        return;

    printf("Requesting alignment of %luB for BAR%d of device %s\n",
           align, bar, dev->name);

    /* Align the resource */
    if (align > 0) {
        uint64_t mask = align - 1;
        uint64_t aligned_start = (r->start + mask) & ~mask;
        uint64_t aligned_size = (size + mask) & ~mask;

        if (resize && aligned_size > size) {
            printf("Resizing BAR%d from %lu to %lu bytes\n",
                   bar, size, aligned_size);
            size = aligned_size;
        }

        r->start = aligned_start;
        r->end = r->start + size - 1;
        r->flags |= IORESOURCE_UNSET;

        printf("Resource aligned: start=0x%lx, end=0x%lx, size=%lu\n",
               r->start, r->end, size);
    }
}

static uint64_t pci_specified_resource_alignment(struct pci_dev *dev, bool *resize)
{
    /* Simulate alignment specification */
    *resize = true;
    return PCI_DEFAULT_ALIGN;
}

static void print_resource_info(struct pci_dev *dev)
{
    printf("\nDevice: %s\n", dev->name);
    printf("Command register: 0x%04x\n", dev->command);
    printf("Resources:\n");

    for (int i = 0; i < PCI_NUM_RESOURCES; i++) {
        struct resource *r = &dev->resource[i];
        if (!(r->flags & (IORESOURCE_IO | IORESOURCE_MEM)))
            continue;

        printf("BAR%d: ", i);
        printf("start=0x%lx, end=0x%lx, size=%lu, flags=0x%lx\n",
               r->start, r->end, r->end - r->start + 1, r->flags);
    }
    printf("\n");
}

static void pci_reassigndev_resource_alignment(struct pci_dev *dev)
{
    uint64_t align;
    bool resize;
    uint16_t command;

    if (dev->is_virtfn)
        return;

    align = pci_specified_resource_alignment(dev, &resize);
    if (!align)
        return;

    if (dev->hdr_type == PCI_HEADER_TYPE_NORMAL &&
        (dev->class >> 8) == PCI_CLASS_BRIDGE_HOST) {
        printf("Can't reassign resources to host bridge %s\n", dev->name);
        return;
    }

    /* Disable memory decoding */
    pci_read_config_word(dev, PCI_COMMAND, &command);
    command &= ~PCI_COMMAND_MEMORY;
    pci_write_config_word(dev, PCI_COMMAND, command);

    /* Request alignment for all resources */
    for (int i = 0; i <= PCI_ROM_RESOURCE; i++)
        pci_request_resource_alignment(dev, i, align, resize);

    /* Handle bridge resources */
    if (dev->hdr_type == PCI_HEADER_TYPE_BRIDGE) {
        struct resource *r;
        for (int i = PCI_BRIDGE_RESOURCES; i < PCI_NUM_RESOURCES; i++) {
            r = &dev->resource[i];
            if (!(r->flags & IORESOURCE_MEM))
                continue;
            r->flags |= IORESOURCE_UNSET;
            r->end = r->end - r->start;
            r->start = 0;
        }
        pci_disable_bridge_window(dev);
    }
}

/* Test Scenarios */
static void test_normal_device(void)
{
    struct pci_dev dev = {
        .vendor = 0x1234,
        .device = 0x5678,
        .hdr_type = PCI_HEADER_TYPE_NORMAL,
        .class = 0x030000, /* VGA compatible controller */
        .is_virtfn = false,
        .command = PCI_COMMAND_MEMORY | PCI_COMMAND_IO,
    };
    snprintf(dev.name, sizeof(dev.name), "Test VGA Device");

    /* Initialize resources */
    dev.resource[0] = (struct resource){
        .start = 0x1000,
        .end = 0x1fff,
        .name = "BAR0",
        .flags = IORESOURCE_MEM
    };
    dev.resource[1] = (struct resource){
        .start = 0x2000,
        .end = 0x2fff,
        .name = "BAR1",
        .flags = IORESOURCE_IO
    };

    printf("Testing normal PCI device resource alignment\n");
    printf("===========================================\n");
    
    printf("Initial state:\n");
    print_resource_info(&dev);

    printf("Performing resource alignment...\n");
    pci_reassigndev_resource_alignment(&dev);

    printf("Final state:\n");
    print_resource_info(&dev);
}

static void test_bridge_device(void)
{
    struct pci_dev dev = {
        .vendor = 0x9ABC,
        .device = 0xDEF0,
        .hdr_type = PCI_HEADER_TYPE_BRIDGE,
        .class = 0x060400, /* PCI Bridge */
        .is_virtfn = false,
        .command = PCI_COMMAND_MEMORY | PCI_COMMAND_IO,
    };
    snprintf(dev.name, sizeof(dev.name), "Test PCI Bridge");

    /* Initialize bridge resources */
    dev.resource[0] = (struct resource){
        .start = 0x10000,
        .end = 0x1ffff,
        .name = "Bridge BAR0",
        .flags = IORESOURCE_MEM
    };
    dev.resource[PCI_BRIDGE_RESOURCES] = (struct resource){
        .start = 0x20000,
        .end = 0x2ffff,
        .name = "Bridge Window",
        .flags = IORESOURCE_MEM
    };

    printf("\nTesting PCI bridge resource alignment\n");
    printf("=====================================\n");
    
    printf("Initial state:\n");
    print_resource_info(&dev);

    printf("Performing resource alignment...\n");
    pci_reassigndev_resource_alignment(&dev);

    printf("Final state:\n");
    print_resource_info(&dev);
}

static void test_host_bridge(void)
{
    struct pci_dev dev = {
        .vendor = 0x5555,
        .device = 0x6666,
        .hdr_type = PCI_HEADER_TYPE_NORMAL,
        .class = PCI_CLASS_BRIDGE_HOST << 8,
        .is_virtfn = false,
        .command = PCI_COMMAND_MEMORY | PCI_COMMAND_IO,
    };
    snprintf(dev.name, sizeof(dev.name), "Test Host Bridge");

    /* Initialize resources */
    dev.resource[0] = (struct resource){
        .start = 0x100000,
        .end = 0x1fffff,
        .name = "Host Bridge BAR0",
        .flags = IORESOURCE_MEM
    };

    printf("\nTesting host bridge resource alignment\n");
    printf("======================================\n");
    
    printf("Initial state:\n");
    print_resource_info(&dev);

    printf("Attempting resource alignment (should be rejected)...\n");
    pci_reassigndev_resource_alignment(&dev);

    printf("Final state:\n");
    print_resource_info(&dev);
}

int main(void)
{
    printf("PCI Resource Management Test Program\n");
    printf("===================================\n\n");

    /* Test different device types */
    test_normal_device();
    test_bridge_device();
    test_host_bridge();

    return 0;
}
