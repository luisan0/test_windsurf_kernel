/*
 * Bluetooth Security Manager Protocol Test Program
 * This is a standalone simulation of BT SMP operations
 * Author: Cascade AI
 * Date: 2024-12-27
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

/* Constants */
#define BT_ADDR_SIZE        6
#define BT_KEY_SIZE         16
#define BT_RAND_SIZE        16
#define BT_CONFIRM_SIZE     16
#define BT_IRK_SIZE         16
#define BT_CSRK_SIZE        16
#define BT_LTK_SIZE         16
#define BT_MAX_KEYS         10

/* SMP Commands */
#define SMP_PAIRING_REQ     0x01
#define SMP_PAIRING_RSP     0x02
#define SMP_CONFIRM         0x03
#define SMP_RAND            0x04
#define SMP_PAIRING_FAIL    0x05
#define SMP_ENCRYPT_INFO    0x06
#define SMP_MASTER_IDENT    0x07
#define SMP_IDENT_INFO      0x08
#define SMP_IDENT_ADDR_INFO 0x09
#define SMP_SIGNING_INFO    0x0A

/* Pairing Methods */
#define SMP_JUST_WORKS      0x00
#define SMP_PASSKEY_ENTRY   0x01
#define SMP_NUMERIC_COMP    0x02
#define SMP_OOB             0x03

/* Error Codes */
#define SMP_SUCCESS         0x00
#define SMP_ERROR_PASSKEY   0x01
#define SMP_ERROR_OOB       0x02
#define SMP_ERROR_AUTH      0x03
#define SMP_ERROR_CONFIRM   0x04
#define SMP_ERROR_ENCRYPT   0x05
#define SMP_ERROR_RESOURCES 0x06

/* IO Capabilities */
#define SMP_IO_DISPLAY_ONLY     0x00
#define SMP_IO_DISPLAY_YESNO    0x01
#define SMP_IO_KEYBOARD_ONLY    0x02
#define SMP_IO_NO_INPUT_OUTPUT  0x03

/* Authentication Requirements */
#define SMP_AUTH_NONE           0x00
#define SMP_AUTH_BONDING        0x01
#define SMP_AUTH_MITM           0x04
#define SMP_AUTH_SC             0x08
#define SMP_AUTH_KEYPRESS       0x10

/* Device structure */
struct bt_device {
    uint8_t  addr[BT_ADDR_SIZE];
    uint8_t  addr_type;
    uint8_t  io_capability;
    uint8_t  auth_req;
    uint8_t  max_key_size;
    uint8_t  init_key_dist;
    uint8_t  resp_key_dist;
    bool     initiator;
    
    /* Temporary pairing data */
    uint8_t  confirm[BT_CONFIRM_SIZE];
    uint8_t  random[BT_RAND_SIZE];
    uint8_t  tk[BT_KEY_SIZE];
    uint32_t passkey;
    
    /* Long term keys */
    struct {
        uint8_t ltk[BT_LTK_SIZE];
        uint8_t irk[BT_IRK_SIZE];
        uint8_t csrk[BT_CSRK_SIZE];
        uint16_t ediv;
        uint8_t rand[BT_RAND_SIZE];
        bool valid;
    } keys;
};

/* SMP Context */
struct smp_context {
    struct bt_device *initiator;
    struct bt_device *responder;
    uint8_t  pairing_method;
    uint8_t  state;
    bool     encrypted;
    bool     authenticated;
};

/* Function declarations */
static struct bt_device *bt_device_create(const uint8_t *addr, uint8_t addr_type);
static void bt_device_destroy(struct bt_device *dev);
static struct smp_context *smp_context_create(struct bt_device *init, struct bt_device *resp);
static void smp_context_destroy(struct smp_context *ctx);
static int smp_send_pairing_req(struct smp_context *ctx);
static int smp_send_pairing_rsp(struct smp_context *ctx);
static int smp_generate_tk(struct smp_context *ctx);
static int smp_generate_confirm(struct bt_device *dev);
static int smp_verify_confirm(struct smp_context *ctx);
static int smp_generate_ltk(struct smp_context *ctx);
static void smp_distribute_keys(struct smp_context *ctx);
static void generate_random(uint8_t *buf, size_t len);
static void print_hex(const uint8_t *data, size_t len);
static const char *get_pairing_method_str(uint8_t method);

/* Create Bluetooth device */
static struct bt_device *bt_device_create(const uint8_t *addr, uint8_t addr_type) {
    struct bt_device *dev = calloc(1, sizeof(*dev));
    if (!dev)
        return NULL;
    
    memcpy(dev->addr, addr, BT_ADDR_SIZE);
    dev->addr_type = addr_type;
    dev->io_capability = SMP_IO_DISPLAY_YESNO;
    dev->auth_req = SMP_AUTH_BONDING | SMP_AUTH_MITM;
    dev->max_key_size = 16;
    dev->init_key_dist = 0x07;  /* All keys */
    dev->resp_key_dist = 0x07;  /* All keys */
    
    return dev;
}

/* Destroy Bluetooth device */
static void bt_device_destroy(struct bt_device *dev) {
    if (dev)
        free(dev);
}

/* Create SMP context */
static struct smp_context *smp_context_create(struct bt_device *init,
                                            struct bt_device *resp) {
    struct smp_context *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;
    
    ctx->initiator = init;
    ctx->responder = resp;
    init->initiator = true;
    resp->initiator = false;
    
    return ctx;
}

/* Destroy SMP context */
static void smp_context_destroy(struct smp_context *ctx) {
    if (ctx)
        free(ctx);
}

/* Send pairing request */
static int smp_send_pairing_req(struct smp_context *ctx) {
    struct bt_device *dev = ctx->initiator;
    
    printf("Sending pairing request:\n");
    printf("  IO Capability: 0x%02x\n", dev->io_capability);
    printf("  Auth Requirements: 0x%02x\n", dev->auth_req);
    printf("  Max Key Size: %d\n", dev->max_key_size);
    printf("  Init Key Dist: 0x%02x\n", dev->init_key_dist);
    printf("  Resp Key Dist: 0x%02x\n", dev->resp_key_dist);
    
    return SMP_SUCCESS;
}

/* Send pairing response */
static int smp_send_pairing_rsp(struct smp_context *ctx) {
    struct bt_device *dev = ctx->responder;
    
    printf("Sending pairing response:\n");
    printf("  IO Capability: 0x%02x\n", dev->io_capability);
    printf("  Auth Requirements: 0x%02x\n", dev->auth_req);
    printf("  Max Key Size: %d\n", dev->max_key_size);
    printf("  Init Key Dist: 0x%02x\n", dev->init_key_dist);
    printf("  Resp Key Dist: 0x%02x\n", dev->resp_key_dist);
    
    /* Determine pairing method */
    if ((ctx->initiator->auth_req | ctx->responder->auth_req) & SMP_AUTH_MITM) {
        if (ctx->initiator->io_capability == SMP_IO_NO_INPUT_OUTPUT ||
            ctx->responder->io_capability == SMP_IO_NO_INPUT_OUTPUT) {
            ctx->pairing_method = SMP_JUST_WORKS;
        } else if (ctx->initiator->io_capability == SMP_IO_DISPLAY_YESNO &&
                  ctx->responder->io_capability == SMP_IO_DISPLAY_YESNO) {
            ctx->pairing_method = SMP_NUMERIC_COMP;
        } else {
            ctx->pairing_method = SMP_PASSKEY_ENTRY;
        }
    } else {
        ctx->pairing_method = SMP_JUST_WORKS;
    }
    
    printf("Selected pairing method: %s\n",
           get_pairing_method_str(ctx->pairing_method));
    
    return SMP_SUCCESS;
}

/* Generate temporary key */
static int smp_generate_tk(struct smp_context *ctx) {
    struct bt_device *init = ctx->initiator;
    struct bt_device *resp = ctx->responder;
    
    switch (ctx->pairing_method) {
    case SMP_JUST_WORKS:
        memset(init->tk, 0, BT_KEY_SIZE);
        memset(resp->tk, 0, BT_KEY_SIZE);
        break;
        
    case SMP_PASSKEY_ENTRY:
        init->passkey = rand() % 1000000;
        resp->passkey = init->passkey;
        memcpy(init->tk, &init->passkey, sizeof(init->passkey));
        memcpy(resp->tk, &resp->passkey, sizeof(resp->passkey));
        printf("Generated passkey: %06u\n", init->passkey);
        break;
        
    case SMP_NUMERIC_COMP:
        init->passkey = rand() % 1000000;
        resp->passkey = init->passkey;
        memcpy(init->tk, &init->passkey, sizeof(init->passkey));
        memcpy(resp->tk, &resp->passkey, sizeof(resp->passkey));
        printf("Numeric value: %06u\n", init->passkey);
        break;
        
    case SMP_OOB:
        generate_random(init->tk, BT_KEY_SIZE);
        memcpy(resp->tk, init->tk, BT_KEY_SIZE);
        printf("OOB data: ");
        print_hex(init->tk, BT_KEY_SIZE);
        break;
    }
    
    return SMP_SUCCESS;
}

/* Generate confirm value */
static int smp_generate_confirm(struct bt_device *dev) {
    /* In real implementation, this would use AES-CMAC */
    generate_random(dev->confirm, BT_CONFIRM_SIZE);
    generate_random(dev->random, BT_RAND_SIZE);
    
    printf("%s confirm value: ", dev->initiator ? "Initiator" : "Responder");
    print_hex(dev->confirm, BT_CONFIRM_SIZE);
    
    return SMP_SUCCESS;
}

/* Verify confirm value */
static int smp_verify_confirm(struct smp_context *ctx) {
    /* In real implementation, this would verify AES-CMAC */
    printf("Verifying confirm values...\n");
    ctx->authenticated = true;
    return SMP_SUCCESS;
}

/* Generate long term key */
static int smp_generate_ltk(struct smp_context *ctx) {
    struct bt_device *init = ctx->initiator;
    struct bt_device *resp = ctx->responder;
    
    /* Generate LTK */
    generate_random(init->keys.ltk, BT_LTK_SIZE);
    memcpy(resp->keys.ltk, init->keys.ltk, BT_LTK_SIZE);
    
    /* Generate EDIV and Rand */
    init->keys.ediv = rand() & 0xFFFF;
    resp->keys.ediv = init->keys.ediv;
    generate_random(init->keys.rand, BT_RAND_SIZE);
    memcpy(resp->keys.rand, init->keys.rand, BT_RAND_SIZE);
    
    init->keys.valid = true;
    resp->keys.valid = true;
    ctx->encrypted = true;
    
    printf("Generated LTK: ");
    print_hex(init->keys.ltk, BT_LTK_SIZE);
    
    return SMP_SUCCESS;
}

/* Distribute keys */
static void smp_distribute_keys(struct smp_context *ctx) {
    struct bt_device *init = ctx->initiator;
    struct bt_device *resp = ctx->responder;
    
    printf("Distributing keys...\n");
    
    if (init->init_key_dist & 0x01) {
        generate_random(init->keys.irk, BT_IRK_SIZE);
        printf("Initiator IRK: ");
        print_hex(init->keys.irk, BT_IRK_SIZE);
    }
    
    if (init->init_key_dist & 0x02) {
        generate_random(init->keys.csrk, BT_CSRK_SIZE);
        printf("Initiator CSRK: ");
        print_hex(init->keys.csrk, BT_CSRK_SIZE);
    }
    
    if (resp->resp_key_dist & 0x01) {
        generate_random(resp->keys.irk, BT_IRK_SIZE);
        printf("Responder IRK: ");
        print_hex(resp->keys.irk, BT_IRK_SIZE);
    }
    
    if (resp->resp_key_dist & 0x02) {
        generate_random(resp->keys.csrk, BT_CSRK_SIZE);
        printf("Responder CSRK: ");
        print_hex(resp->keys.csrk, BT_CSRK_SIZE);
    }
}

/* Generate random data */
static void generate_random(uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++)
        buf[i] = rand() & 0xFF;
}

/* Print hex data */
static void print_hex(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++)
        printf("%02x", data[i]);
    printf("\n");
}

/* Get pairing method string */
static const char *get_pairing_method_str(uint8_t method) {
    switch (method) {
    case SMP_JUST_WORKS:
        return "Just Works";
    case SMP_PASSKEY_ENTRY:
        return "Passkey Entry";
    case SMP_NUMERIC_COMP:
        return "Numeric Comparison";
    case SMP_OOB:
        return "Out of Band";
    default:
        return "Unknown";
    }
}

/* Test functions */
static void test_just_works_pairing(void) {
    printf("\nTesting Just Works pairing...\n");
    printf("==============================\n");
    
    /* Create devices */
    uint8_t init_addr[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    uint8_t resp_addr[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    
    struct bt_device *initiator = bt_device_create(init_addr, 0);
    struct bt_device *responder = bt_device_create(resp_addr, 0);
    
    initiator->io_capability = SMP_IO_NO_INPUT_OUTPUT;
    responder->io_capability = SMP_IO_NO_INPUT_OUTPUT;
    
    /* Create SMP context */
    struct smp_context *ctx = smp_context_create(initiator, responder);
    
    /* Perform pairing */
    smp_send_pairing_req(ctx);
    smp_send_pairing_rsp(ctx);
    smp_generate_tk(ctx);
    smp_generate_confirm(initiator);
    smp_generate_confirm(responder);
    smp_verify_confirm(ctx);
    smp_generate_ltk(ctx);
    smp_distribute_keys(ctx);
    
    /* Cleanup */
    smp_context_destroy(ctx);
    bt_device_destroy(initiator);
    bt_device_destroy(responder);
}

static void test_passkey_pairing(void) {
    printf("\nTesting Passkey Entry pairing...\n");
    printf("================================\n");
    
    /* Create devices */
    uint8_t init_addr[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    uint8_t resp_addr[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    
    struct bt_device *initiator = bt_device_create(init_addr, 0);
    struct bt_device *responder = bt_device_create(resp_addr, 0);
    
    initiator->io_capability = SMP_IO_DISPLAY_ONLY;
    responder->io_capability = SMP_IO_KEYBOARD_ONLY;
    
    /* Create SMP context */
    struct smp_context *ctx = smp_context_create(initiator, responder);
    
    /* Perform pairing */
    smp_send_pairing_req(ctx);
    smp_send_pairing_rsp(ctx);
    smp_generate_tk(ctx);
    smp_generate_confirm(initiator);
    smp_generate_confirm(responder);
    smp_verify_confirm(ctx);
    smp_generate_ltk(ctx);
    smp_distribute_keys(ctx);
    
    /* Cleanup */
    smp_context_destroy(ctx);
    bt_device_destroy(initiator);
    bt_device_destroy(responder);
}

int main(void) {
    printf("Bluetooth SMP Test Program\n");
    printf("=========================\n");
    
    /* Seed random number generator */
    srand(time(NULL));
    
    /* Run tests */
    test_just_works_pairing();
    test_passkey_pairing();
    
    printf("\nTest completed successfully!\n");
    return 0;
}
