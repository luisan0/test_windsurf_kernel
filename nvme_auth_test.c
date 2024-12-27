/*
 * NVMe Authentication Test Program
 * This is a standalone simulation of NVMe authentication operations
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

/* Simple SHA-256 implementation */
#define SHA256_BLOCK_SIZE 32

typedef struct {
    uint8_t data[64];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[8];
} SHA256_CTX;

#define ROTRIGHT(word,bits) (((word) >> (bits)) | ((word) << (32-(bits))))
#define CH(x,y,z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTRIGHT(x,2) ^ ROTRIGHT(x,13) ^ ROTRIGHT(x,22))
#define EP1(x) (ROTRIGHT(x,6) ^ ROTRIGHT(x,11) ^ ROTRIGHT(x,25))
#define SIG0(x) (ROTRIGHT(x,7) ^ ROTRIGHT(x,18) ^ ((x) >> 3))
#define SIG1(x) (ROTRIGHT(x,17) ^ ROTRIGHT(x,19) ^ ((x) >> 10))

static const uint32_t k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static void sha256_transform(SHA256_CTX *ctx, const uint8_t data[]) {
    uint32_t a, b, c, d, e, f, g, h, i, j, t1, t2, m[64];

    for (i = 0, j = 0; i < 16; ++i, j += 4)
        m[i] = (data[j] << 24) | (data[j + 1] << 16) | (data[j + 2] << 8) | (data[j + 3]);
    for (; i < 64; ++i)
        m[i] = SIG1(m[i - 2]) + m[i - 7] + SIG0(m[i - 15]) + m[i - 16];

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (i = 0; i < 64; ++i) {
        t1 = h + EP1(e) + CH(e,f,g) + k[i] + m[i];
        t2 = EP0(a) + MAJ(a,b,c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void sha256_init(SHA256_CTX *ctx) {
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
}

static void sha256_update(SHA256_CTX *ctx, const uint8_t data[], size_t len) {
    uint32_t i;

    for (i = 0; i < len; ++i) {
        ctx->data[ctx->datalen] = data[i];
        ctx->datalen++;
        if (ctx->datalen == 64) {
            sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

static void sha256_final(SHA256_CTX *ctx, uint8_t hash[]) {
    uint32_t i;

    i = ctx->datalen;

    if (ctx->datalen < 56) {
        ctx->data[i++] = 0x80;
        while (i < 56)
            ctx->data[i++] = 0x00;
    }
    else {
        ctx->data[i++] = 0x80;
        while (i < 64)
            ctx->data[i++] = 0x00;
        sha256_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56);
    }

    ctx->bitlen += ctx->datalen * 8;
    ctx->data[63] = ctx->bitlen;
    ctx->data[62] = ctx->bitlen >> 8;
    ctx->data[61] = ctx->bitlen >> 16;
    ctx->data[60] = ctx->bitlen >> 24;
    ctx->data[59] = ctx->bitlen >> 32;
    ctx->data[58] = ctx->bitlen >> 40;
    ctx->data[57] = ctx->bitlen >> 48;
    ctx->data[56] = ctx->bitlen >> 56;
    sha256_transform(ctx, ctx->data);

    for (i = 0; i < 4; ++i) {
        hash[i]      = (ctx->state[0] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 4]  = (ctx->state[1] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 8]  = (ctx->state[2] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 12] = (ctx->state[3] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 16] = (ctx->state[4] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 20] = (ctx->state[5] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 24] = (ctx->state[6] >> (24 - i * 8)) & 0x000000ff;
        hash[i + 28] = (ctx->state[7] >> (24 - i * 8)) & 0x000000ff;
    }
}

/* Authentication protocol definitions */
#define NVME_AUTH_PROTO_NONE     0
#define NVME_AUTH_PROTO_DHHC     1
#define NVME_AUTH_PROTO_DHCHAP   2

/* Authentication hash algorithms */
#define NVME_AUTH_HASH_SHA256    0
#define NVME_AUTH_HASH_SHA384    1
#define NVME_AUTH_HASH_SHA512    2

/* Authentication states */
#define NVME_AUTH_STATE_NONE     0
#define NVME_AUTH_STATE_NEGOTIATE 1
#define NVME_AUTH_STATE_CHALLENGE 2
#define NVME_AUTH_STATE_RESPONSE  3
#define NVME_AUTH_STATE_SUCCESS   4
#define NVME_AUTH_STATE_FAILED    5

/* Maximum sizes */
#define NVME_AUTH_NONCE_SIZE     32
#define NVME_AUTH_KEY_SIZE       64
#define NVME_AUTH_HASH_SIZE      64
#define NVME_AUTH_NAME_SIZE      32
#define NVME_AUTH_MAX_DHGROUPS   8
#define NVME_AUTH_MAX_RETRIES    3

/* Error codes */
#define NVME_AUTH_ERR_NONE       0
#define NVME_AUTH_ERR_PROTO      1
#define NVME_AUTH_ERR_HASH       2
#define NVME_AUTH_ERR_KEY        3
#define NVME_AUTH_ERR_STATE      4
#define NVME_AUTH_ERR_PARAM      5
#define NVME_AUTH_ERR_VERIFY     6

/* Authentication transaction structure */
struct nvme_auth_trans {
    uint8_t protocol;
    uint8_t hash_algo;
    uint8_t state;
    uint8_t retries;
    
    /* DH parameters */
    uint8_t dh_group;
    uint8_t *dh_key;
    size_t dh_key_len;
    
    /* Challenge/Response */
    uint8_t challenge[NVME_AUTH_NONCE_SIZE];
    uint8_t response[NVME_AUTH_HASH_SIZE];
    uint8_t verify[NVME_AUTH_HASH_SIZE];
    
    /* Session info */
    char host_id[NVME_AUTH_NAME_SIZE];
    char ctrl_id[NVME_AUTH_NAME_SIZE];
    uint8_t session_key[NVME_AUTH_KEY_SIZE];
    size_t session_key_len;
    
    /* Status */
    int error;
    bool complete;
};

/* Authentication context structure */
struct nvme_auth_ctx {
    uint8_t supported_protos;
    uint8_t supported_hashes;
    uint8_t supported_dhgroups;
    
    /* Keys */
    uint8_t host_key[NVME_AUTH_KEY_SIZE];
    uint8_t ctrl_key[NVME_AUTH_KEY_SIZE];
    size_t key_len;
    
    /* Active transaction */
    struct nvme_auth_trans *trans;
    
    /* Statistics */
    uint32_t auth_attempts;
    uint32_t auth_success;
    uint32_t auth_failures;
    
    /* Lock */
    pthread_mutex_t lock;
};

/* Function declarations */
static struct nvme_auth_ctx *nvme_auth_alloc_ctx(void);
static void nvme_auth_free_ctx(struct nvme_auth_ctx *ctx);
static struct nvme_auth_trans *nvme_auth_alloc_trans(void);
static void nvme_auth_free_trans(struct nvme_auth_trans *trans);
static int nvme_auth_start(struct nvme_auth_ctx *ctx, struct nvme_auth_trans *trans);
static int nvme_auth_negotiate(struct nvme_auth_ctx *ctx, struct nvme_auth_trans *trans);
static int nvme_auth_challenge(struct nvme_auth_ctx *ctx, struct nvme_auth_trans *trans);
static int nvme_auth_response(struct nvme_auth_ctx *ctx, struct nvme_auth_trans *trans);
static int nvme_auth_verify(struct nvme_auth_ctx *ctx, struct nvme_auth_trans *trans);
static void nvme_auth_generate_nonce(uint8_t *nonce, size_t len);
static int nvme_auth_compute_hash(uint8_t algo, const uint8_t *data, size_t len,
                                uint8_t *hash, size_t *hash_len);
static void nvme_auth_dump_status(struct nvme_auth_ctx *ctx);

/* Allocate authentication context */
static struct nvme_auth_ctx *nvme_auth_alloc_ctx(void) {
    struct nvme_auth_ctx *ctx;
    
    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;
    
    /* Set supported protocols and algorithms */
    ctx->supported_protos = (1 << NVME_AUTH_PROTO_DHCHAP);
    ctx->supported_hashes = (1 << NVME_AUTH_HASH_SHA256);
    ctx->supported_dhgroups = 0xff;  /* All groups */
    
    pthread_mutex_init(&ctx->lock, NULL);
    
    return ctx;
}

/* Free authentication context */
static void nvme_auth_free_ctx(struct nvme_auth_ctx *ctx) {
    if (!ctx)
        return;
    
    nvme_auth_free_trans(ctx->trans);
    pthread_mutex_destroy(&ctx->lock);
    free(ctx);
}

/* Allocate authentication transaction */
static struct nvme_auth_trans *nvme_auth_alloc_trans(void) {
    struct nvme_auth_trans *trans;
    
    trans = calloc(1, sizeof(*trans));
    if (!trans)
        return NULL;
    
    trans->dh_key = malloc(NVME_AUTH_KEY_SIZE);
    if (!trans->dh_key) {
        free(trans);
        return NULL;
    }
    
    return trans;
}

/* Free authentication transaction */
static void nvme_auth_free_trans(struct nvme_auth_trans *trans) {
    if (!trans)
        return;
    
    free(trans->dh_key);
    free(trans);
}

/* Generate random nonce */
static void nvme_auth_generate_nonce(uint8_t *nonce, size_t len) {
    for (size_t i = 0; i < len; i++)
        nonce[i] = rand() & 0xff;
}

/* Compute hash */
static int nvme_auth_compute_hash(uint8_t algo, const uint8_t *data, size_t len,
                                uint8_t *hash, size_t *hash_len) {
    SHA256_CTX ctx;
    
    if (algo != NVME_AUTH_HASH_SHA256)
        return NVME_AUTH_ERR_HASH;
    
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, hash);
    
    *hash_len = SHA256_BLOCK_SIZE;
    return NVME_AUTH_ERR_NONE;
}

/* Start authentication */
static int nvme_auth_start(struct nvme_auth_ctx *ctx, struct nvme_auth_trans *trans) {
    pthread_mutex_lock(&ctx->lock);
    
    if (ctx->trans) {
        pthread_mutex_unlock(&ctx->lock);
        return NVME_AUTH_ERR_STATE;
    }
    
    /* Initialize transaction */
    trans->protocol = NVME_AUTH_PROTO_DHCHAP;
    trans->hash_algo = NVME_AUTH_HASH_SHA256;
    trans->state = NVME_AUTH_STATE_NEGOTIATE;
    trans->retries = 0;
    
    ctx->trans = trans;
    ctx->auth_attempts++;
    
    pthread_mutex_unlock(&ctx->lock);
    return NVME_AUTH_ERR_NONE;
}

/* Negotiate parameters */
static int nvme_auth_negotiate(struct nvme_auth_ctx *ctx, struct nvme_auth_trans *trans) {
    if (trans->state != NVME_AUTH_STATE_NEGOTIATE)
        return NVME_AUTH_ERR_STATE;
    
    /* Check protocol support */
    if (!(ctx->supported_protos & (1 << trans->protocol)))
        return NVME_AUTH_ERR_PROTO;
    
    /* Check hash algorithm support */
    if (!(ctx->supported_hashes & (1 << trans->hash_algo)))
        return NVME_AUTH_ERR_HASH;
    
    /* Generate session key */
    nvme_auth_generate_nonce(trans->session_key, NVME_AUTH_KEY_SIZE);
    trans->session_key_len = NVME_AUTH_KEY_SIZE;
    
    trans->state = NVME_AUTH_STATE_CHALLENGE;
    return NVME_AUTH_ERR_NONE;
}

/* Generate challenge */
static int nvme_auth_challenge(struct nvme_auth_ctx *ctx, struct nvme_auth_trans *trans) {
    if (trans->state != NVME_AUTH_STATE_CHALLENGE)
        return NVME_AUTH_ERR_STATE;
    
    /* Generate challenge nonce */
    nvme_auth_generate_nonce(trans->challenge, NVME_AUTH_NONCE_SIZE);
    
    /* Compute expected response */
    uint8_t buffer[NVME_AUTH_KEY_SIZE + NVME_AUTH_NONCE_SIZE];
    memcpy(buffer, trans->session_key, trans->session_key_len);
    memcpy(buffer + trans->session_key_len, trans->challenge, NVME_AUTH_NONCE_SIZE);
    
    size_t hash_len;
    int ret = nvme_auth_compute_hash(trans->hash_algo, buffer,
                                   trans->session_key_len + NVME_AUTH_NONCE_SIZE,
                                   trans->verify, &hash_len);
    if (ret)
        return ret;
    
    trans->state = NVME_AUTH_STATE_RESPONSE;
    return NVME_AUTH_ERR_NONE;
}

/* Process response */
static int nvme_auth_response(struct nvme_auth_ctx *ctx, struct nvme_auth_trans *trans) {
    if (trans->state != NVME_AUTH_STATE_RESPONSE)
        return NVME_AUTH_ERR_STATE;
    
    /* Compute response hash */
    uint8_t buffer[NVME_AUTH_KEY_SIZE + NVME_AUTH_NONCE_SIZE];
    memcpy(buffer, trans->session_key, trans->session_key_len);
    memcpy(buffer + trans->session_key_len, trans->challenge, NVME_AUTH_NONCE_SIZE);
    
    size_t hash_len;
    int ret = nvme_auth_compute_hash(trans->hash_algo, buffer,
                                   trans->session_key_len + NVME_AUTH_NONCE_SIZE,
                                   trans->response, &hash_len);
    if (ret)
        return ret;
    
    trans->state = NVME_AUTH_STATE_SUCCESS;
    return NVME_AUTH_ERR_NONE;
}

/* Verify authentication */
static int nvme_auth_verify(struct nvme_auth_ctx *ctx, struct nvme_auth_trans *trans) {
    if (trans->state != NVME_AUTH_STATE_SUCCESS)
        return NVME_AUTH_ERR_STATE;
    
    /* Compare response with expected value */
    if (memcmp(trans->response, trans->verify, NVME_AUTH_HASH_SIZE) != 0) {
        trans->state = NVME_AUTH_STATE_FAILED;
        ctx->auth_failures++;
        return NVME_AUTH_ERR_VERIFY;
    }
    
    ctx->auth_success++;
    trans->complete = true;
    return NVME_AUTH_ERR_NONE;
}

/* Dump authentication status */
static void nvme_auth_dump_status(struct nvme_auth_ctx *ctx) {
    printf("\nNVMe Authentication Status:\n");
    printf("=========================\n");
    printf("Supported protocols: 0x%02x\n", ctx->supported_protos);
    printf("Supported hashes: 0x%02x\n", ctx->supported_hashes);
    printf("Supported DH groups: 0x%02x\n", ctx->supported_dhgroups);
    printf("Authentication attempts: %u\n", ctx->auth_attempts);
    printf("Successful authentications: %u\n", ctx->auth_success);
    printf("Failed authentications: %u\n", ctx->auth_failures);
    
    if (ctx->trans) {
        printf("\nCurrent transaction:\n");
        printf("Protocol: %u\n", ctx->trans->protocol);
        printf("Hash algorithm: %u\n", ctx->trans->hash_algo);
        printf("State: %u\n", ctx->trans->state);
        printf("Retries: %u\n", ctx->trans->retries);
        printf("Complete: %s\n", ctx->trans->complete ? "yes" : "no");
        printf("Error: %d\n", ctx->trans->error);
    }
}

/* Test functions */
static void test_basic_auth(struct nvme_auth_ctx *ctx) {
    printf("\nTesting basic authentication...\n");
    
    /* Create transaction */
    struct nvme_auth_trans *trans = nvme_auth_alloc_trans();
    if (!trans) {
        printf("Failed to allocate transaction\n");
        return;
    }
    
    /* Start authentication */
    printf("Starting authentication...\n");
    int ret = nvme_auth_start(ctx, trans);
    if (ret) {
        printf("Failed to start authentication: %d\n", ret);
        nvme_auth_free_trans(trans);
        return;
    }
    
    /* Negotiate parameters */
    printf("Negotiating parameters...\n");
    ret = nvme_auth_negotiate(ctx, trans);
    if (ret) {
        printf("Negotiation failed: %d\n", ret);
        return;
    }
    
    /* Generate challenge */
    printf("Generating challenge...\n");
    ret = nvme_auth_challenge(ctx, trans);
    if (ret) {
        printf("Challenge generation failed: %d\n", ret);
        return;
    }
    
    /* Process response */
    printf("Processing response...\n");
    ret = nvme_auth_response(ctx, trans);
    if (ret) {
        printf("Response processing failed: %d\n", ret);
        return;
    }
    
    /* Verify authentication */
    printf("Verifying authentication...\n");
    ret = nvme_auth_verify(ctx, trans);
    if (ret)
        printf("Authentication failed: %d\n", ret);
    else
        printf("Authentication successful!\n");
}

static void test_error_handling(struct nvme_auth_ctx *ctx) {
    printf("\nTesting error handling...\n");
    
    /* Test invalid protocol */
    struct nvme_auth_trans *trans = nvme_auth_alloc_trans();
    if (!trans) {
        printf("Failed to allocate transaction\n");
        return;
    }
    
    printf("Testing invalid protocol...\n");
    trans->protocol = 0xff;
    int ret = nvme_auth_start(ctx, trans);
    if (ret)
        printf("Invalid protocol rejected as expected: %d\n", ret);
    else
        printf("Invalid protocol unexpectedly accepted\n");
    
    nvme_auth_free_trans(trans);
    
    /* Test invalid state transition */
    trans = nvme_auth_alloc_trans();
    if (!trans) {
        printf("Failed to allocate transaction\n");
        return;
    }
    
    printf("Testing invalid state transition...\n");
    trans->state = NVME_AUTH_STATE_SUCCESS;
    ret = nvme_auth_challenge(ctx, trans);
    if (ret)
        printf("Invalid state transition rejected as expected: %d\n", ret);
    else
        printf("Invalid state transition unexpectedly accepted\n");
    
    nvme_auth_free_trans(trans);
}

int main(void) {
    printf("NVMe Authentication Test Program\n");
    printf("===============================\n\n");
    
    /* Seed random number generator */
    srand(time(NULL));
    
    /* Create authentication context */
    struct nvme_auth_ctx *ctx = nvme_auth_alloc_ctx();
    if (!ctx) {
        printf("Failed to allocate context\n");
        return 1;
    }
    
    /* Run tests */
    test_basic_auth(ctx);
    test_error_handling(ctx);
    
    /* Display status */
    nvme_auth_dump_status(ctx);
    
    /* Cleanup */
    nvme_auth_free_ctx(ctx);
    
    printf("\nTest completed successfully!\n");
    return 0;
}
