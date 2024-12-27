/*
 * USB Core Test Program
 * This is a standalone simulation of USB core operations
 * Author: Cascade AI
 * Date: 2024-12-27
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

/* USB constants */
#define USB_MAXCHILDREN        31
#define USB_MAX_EP_NUM        32
#define USB_DIR_OUT           0
#define USB_DIR_IN            0x80

/* Device speeds */
#define USB_SPEED_UNKNOWN     0
#define USB_SPEED_LOW         1
#define USB_SPEED_FULL        2
#define USB_SPEED_HIGH        3
#define USB_SPEED_SUPER       4
#define USB_SPEED_SUPER_PLUS  5

/* Device states */
#define USB_STATE_NOTATTACHED 0
#define USB_STATE_ATTACHED    1
#define USB_STATE_POWERED     2
#define USB_STATE_DEFAULT     3
#define USB_STATE_ADDRESS     4
#define USB_STATE_CONFIGURED  5
#define USB_STATE_SUSPENDED   6

/* Standard requests */
#define USB_REQ_GET_STATUS        0x00
#define USB_REQ_CLEAR_FEATURE     0x01
#define USB_REQ_SET_FEATURE       0x03
#define USB_REQ_SET_ADDRESS       0x05
#define USB_REQ_GET_DESCRIPTOR    0x06
#define USB_REQ_SET_DESCRIPTOR    0x07
#define USB_REQ_GET_CONFIGURATION 0x08
#define USB_REQ_SET_CONFIGURATION 0x09

/* Descriptor types */
#define USB_DT_DEVICE            0x01
#define USB_DT_CONFIG            0x02
#define USB_DT_STRING            0x03
#define USB_DT_INTERFACE         0x04
#define USB_DT_ENDPOINT          0x05

/* Error codes */
#define USB_ERR_SUCCESS          0
#define USB_ERR_INVAL           -1
#define USB_ERR_NODEV           -2
#define USB_ERR_BUSY            -3
#define USB_ERR_TIMEOUT         -4
#define USB_ERR_OVERFLOW        -5
#define USB_ERR_PIPE            -6
#define USB_ERR_IO              -7

/* USB device descriptor */
struct usb_device_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
};

/* USB endpoint descriptor */
struct usb_endpoint_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
};

/* USB configuration descriptor */
struct usb_config_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
};

/* USB interface descriptor */
struct usb_interface_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
};

/* USB device structure */
struct usb_device {
    int                     devnum;
    char                    devpath[16];
    enum {
        USB_DEVICE_ATTACHED,
        USB_DEVICE_DETACHED
    }                      state;
    uint8_t                 speed;
    uint8_t                 config;
    uint8_t                 max_child;
    
    struct usb_device      *parent;
    struct usb_device      *children[USB_MAXCHILDREN];
    
    struct usb_device_descriptor    descriptor;
    struct usb_config_descriptor    *config_desc;
    struct usb_endpoint_descriptor  ep0;
    
    void                   *hcpriv;  /* Host controller private data */
    
    /* Statistics */
    unsigned long          tx_bytes;
    unsigned long          rx_bytes;
    unsigned int           errors;
    
    pthread_mutex_t        lock;
};

/* USB request structure */
struct usb_request {
    uint8_t  requesttype;
    uint8_t  request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
    void    *data;
    int      status;
    bool     complete;
};

/* USB host controller structure */
struct usb_hc {
    char        name[32];
    uint8_t     num_ports;
    uint8_t     power_state;
    
    struct usb_device *root_hub;
    struct usb_device *devices[128];
    int               num_devices;
    
    pthread_mutex_t   lock;
};

/* Function declarations */
static struct usb_device *usb_alloc_device(void);
static void usb_free_device(struct usb_device *udev);
static struct usb_hc *usb_alloc_hc(const char *name);
static void usb_free_hc(struct usb_hc *hc);
static int usb_enumerate_device(struct usb_hc *hc, struct usb_device *udev);
static int usb_control_request(struct usb_device *udev, struct usb_request *req);
static void usb_dump_device(struct usb_device *udev);
static const char *usb_speed_string(uint8_t speed);
static const char *usb_state_string(uint8_t state);

/* Allocate USB device */
static struct usb_device *usb_alloc_device(void) {
    struct usb_device *udev;
    
    udev = calloc(1, sizeof(*udev));
    if (!udev)
        return NULL;
    
    pthread_mutex_init(&udev->lock, NULL);
    
    /* Initialize default endpoint */
    udev->ep0.bLength = sizeof(struct usb_endpoint_descriptor);
    udev->ep0.bDescriptorType = USB_DT_ENDPOINT;
    udev->ep0.bEndpointAddress = 0;
    udev->ep0.bmAttributes = 0;
    udev->ep0.wMaxPacketSize = 64;
    udev->ep0.bInterval = 0;
    
    return udev;
}

/* Free USB device */
static void usb_free_device(struct usb_device *udev) {
    if (!udev)
        return;
    
    free(udev->config_desc);
    pthread_mutex_destroy(&udev->lock);
    free(udev);
}

/* Allocate USB host controller */
static struct usb_hc *usb_alloc_hc(const char *name) {
    struct usb_hc *hc;
    
    hc = calloc(1, sizeof(*hc));
    if (!hc)
        return NULL;
    
    strncpy(hc->name, name, sizeof(hc->name) - 1);
    pthread_mutex_init(&hc->lock, NULL);
    
    /* Create root hub */
    hc->root_hub = usb_alloc_device();
    if (!hc->root_hub) {
        free(hc);
        return NULL;
    }
    
    hc->root_hub->descriptor.bDeviceClass = 9; /* Hub */
    hc->root_hub->speed = USB_SPEED_HIGH;
    hc->num_ports = 4;
    
    return hc;
}

/* Free USB host controller */
static void usb_free_hc(struct usb_hc *hc) {
    int i;
    
    if (!hc)
        return;
    
    for (i = 0; i < hc->num_devices; i++)
        usb_free_device(hc->devices[i]);
    
    usb_free_device(hc->root_hub);
    pthread_mutex_destroy(&hc->lock);
    free(hc);
}

/* Enumerate USB device */
static int usb_enumerate_device(struct usb_hc *hc, struct usb_device *udev) {
    struct usb_request req;
    uint8_t desc[18];
    int ret;
    
    /* Read device descriptor */
    memset(&req, 0, sizeof(req));
    req.requesttype = USB_DIR_IN;
    req.request = USB_REQ_GET_DESCRIPTOR;
    req.value = (USB_DT_DEVICE << 8) + 0;
    req.index = 0;
    req.length = sizeof(desc);
    req.data = desc;
    
    ret = usb_control_request(udev, &req);
    if (ret < 0)
        return ret;
    
    /* Copy descriptor */
    memcpy(&udev->descriptor, desc, sizeof(udev->descriptor));
    
    /* Set device address */
    memset(&req, 0, sizeof(req));
    req.request = USB_REQ_SET_ADDRESS;
    req.value = hc->num_devices + 1;
    
    ret = usb_control_request(udev, &req);
    if (ret < 0)
        return ret;
    
    udev->devnum = req.value;
    
    /* Add to device list */
    pthread_mutex_lock(&hc->lock);
    hc->devices[hc->num_devices++] = udev;
    pthread_mutex_unlock(&hc->lock);
    
    return USB_ERR_SUCCESS;
}

/* Process control request */
static int usb_control_request(struct usb_device *udev, struct usb_request *req) {
    /* Simulate request processing */
    usleep(1000);  /* 1ms delay */
    
    switch (req->request) {
    case USB_REQ_GET_DESCRIPTOR:
        if ((req->value >> 8) == USB_DT_DEVICE) {
            /* Simulate device descriptor */
            struct usb_device_descriptor *desc = req->data;
            desc->bLength = sizeof(*desc);
            desc->bDescriptorType = USB_DT_DEVICE;
            desc->bcdUSB = 0x0200;  /* USB 2.0 */
            desc->bDeviceClass = 0;
            desc->bDeviceSubClass = 0;
            desc->bDeviceProtocol = 0;
            desc->bMaxPacketSize0 = 64;
            desc->idVendor = 0x0483;  /* Example vendor ID */
            desc->idProduct = 0x5740;  /* Example product ID */
            desc->bcdDevice = 0x0100;
            desc->iManufacturer = 1;
            desc->iProduct = 2;
            desc->iSerialNumber = 3;
            desc->bNumConfigurations = 1;
        }
        break;
        
    case USB_REQ_SET_ADDRESS:
        /* Simulate address setting */
        udev->state = USB_STATE_ADDRESS;
        break;
        
    case USB_REQ_SET_CONFIGURATION:
        /* Simulate configuration setting */
        udev->config = req->value;
        udev->state = USB_STATE_CONFIGURED;
        break;
        
    default:
        return USB_ERR_INVAL;
    }
    
    req->status = 0;
    req->complete = true;
    return USB_ERR_SUCCESS;
}

/* Get USB speed string */
static const char *usb_speed_string(uint8_t speed) {
    switch (speed) {
    case USB_SPEED_UNKNOWN:     return "UNKNOWN";
    case USB_SPEED_LOW:         return "low-speed";
    case USB_SPEED_FULL:        return "full-speed";
    case USB_SPEED_HIGH:        return "high-speed";
    case USB_SPEED_SUPER:       return "super-speed";
    case USB_SPEED_SUPER_PLUS:  return "super-speed+";
    default:                    return "INVALID";
    }
}

/* Get USB state string */
static const char *usb_state_string(uint8_t state) {
    switch (state) {
    case USB_STATE_NOTATTACHED: return "NOT ATTACHED";
    case USB_STATE_ATTACHED:    return "ATTACHED";
    case USB_STATE_POWERED:     return "POWERED";
    case USB_STATE_DEFAULT:     return "DEFAULT";
    case USB_STATE_ADDRESS:     return "ADDRESS";
    case USB_STATE_CONFIGURED:  return "CONFIGURED";
    case USB_STATE_SUSPENDED:   return "SUSPENDED";
    default:                    return "INVALID";
    }
}

/* Dump USB device info */
static void usb_dump_device(struct usb_device *udev) {
    printf("\nUSB Device Info:\n");
    printf("===============\n");
    printf("Address: %d\n", udev->devnum);
    printf("Speed: %s\n", usb_speed_string(udev->speed));
    printf("State: %s\n", usb_state_string(udev->state));
    printf("Vendor ID: 0x%04x\n", udev->descriptor.idVendor);
    printf("Product ID: 0x%04x\n", udev->descriptor.idProduct);
    printf("Device Class: 0x%02x\n", udev->descriptor.bDeviceClass);
    printf("Configurations: %d\n", udev->descriptor.bNumConfigurations);
    printf("Max Packet Size: %d\n", udev->descriptor.bMaxPacketSize0);
    printf("Statistics:\n");
    printf("  TX bytes: %lu\n", udev->tx_bytes);
    printf("  RX bytes: %lu\n", udev->rx_bytes);
    printf("  Errors: %u\n", udev->errors);
}

/* Test functions */
static void test_device_enumeration(struct usb_hc *hc) {
    printf("\nTesting device enumeration...\n");
    
    /* Create test device */
    struct usb_device *dev = usb_alloc_device();
    if (!dev) {
        printf("Failed to allocate device\n");
        return;
    }
    
    dev->speed = USB_SPEED_HIGH;
    dev->state = USB_STATE_DEFAULT;
    
    /* Enumerate device */
    printf("Enumerating device...\n");
    int ret = usb_enumerate_device(hc, dev);
    if (ret < 0) {
        printf("Enumeration failed: %d\n", ret);
        usb_free_device(dev);
        return;
    }
    
    printf("Device enumerated successfully!\n");
    usb_dump_device(dev);
}

static void test_control_transfers(struct usb_hc *hc) {
    printf("\nTesting control transfers...\n");
    
    if (hc->num_devices == 0) {
        printf("No devices available\n");
        return;
    }
    
    struct usb_device *dev = hc->devices[0];
    struct usb_request req;
    
    /* Test GET_DESCRIPTOR request */
    printf("Sending GET_DESCRIPTOR request...\n");
    memset(&req, 0, sizeof(req));
    req.requesttype = USB_DIR_IN;
    req.request = USB_REQ_GET_DESCRIPTOR;
    req.value = (USB_DT_DEVICE << 8) + 0;
    req.length = sizeof(struct usb_device_descriptor);
    
    uint8_t desc[18];
    req.data = desc;
    
    int ret = usb_control_request(dev, &req);
    if (ret < 0)
        printf("GET_DESCRIPTOR failed: %d\n", ret);
    else
        printf("GET_DESCRIPTOR successful\n");
    
    /* Test SET_CONFIGURATION request */
    printf("Sending SET_CONFIGURATION request...\n");
    memset(&req, 0, sizeof(req));
    req.request = USB_REQ_SET_CONFIGURATION;
    req.value = 1;
    
    ret = usb_control_request(dev, &req);
    if (ret < 0)
        printf("SET_CONFIGURATION failed: %d\n", ret);
    else
        printf("SET_CONFIGURATION successful\n");
    
    /* Update statistics */
    dev->tx_bytes += sizeof(req);
    dev->rx_bytes += sizeof(desc);
}

int main(void) {
    printf("USB Core Test Program\n");
    printf("===================\n\n");
    
    /* Create host controller */
    struct usb_hc *hc = usb_alloc_hc("test_hc");
    if (!hc) {
        printf("Failed to allocate host controller\n");
        return 1;
    }
    
    /* Run tests */
    test_device_enumeration(hc);
    test_control_transfers(hc);
    
    /* Cleanup */
    usb_free_hc(hc);
    
    printf("\nTest completed successfully!\n");
    return 0;
}
