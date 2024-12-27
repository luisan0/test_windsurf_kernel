/*
 * SMB Server Authentication Test Program
 * This is a standalone simulation of SMB server authentication operations
 * Author: Cascade AI
 * Date: 2024-12-27
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>

/* Authentication mechanisms */
#define SMB_AUTH_NONE          0
#define SMB_AUTH_NTLM         1
#define SMB_AUTH_KERBEROS     2
#define SMB_AUTH_NTLMSSP      3

/* NTLM flags */
#define NTLM_NEGOTIATE_UNICODE              0x00000001
#define NTLM_NEGOTIATE_OEM                  0x00000002
#define NTLM_REQUEST_TARGET                 0x00000004
#define NTLM_NEGOTIATE_SIGN                 0x00000010
#define NTLM_NEGOTIATE_SEAL                 0x00000020
#define NTLM_NEGOTIATE_NTLM                 0x00000200
#define NTLM_NEGOTIATE_ALWAYS_SIGN          0x00008000
#define NTLM_NEGOTIATE_EXTENDED_SESSIONSEC  0x00080000
#define NTLM_NEGOTIATE_VERSION              0x02000000
#define NTLM_NEGOTIATE_128                  0x20000000
#define NTLM_NEGOTIATE_KEY_EXCH            0x40000000
#define NTLM_NEGOTIATE_56                   0x80000000

/* Error codes */
#define SMB_SUCCESS           0
#define SMB_ERR_INVAL       -1
#define SMB_ERR_NOMEM       -2
#define SMB_ERR_AUTH        -3
#define SMB_ERR_CRYPTO      -4
#define SMB_ERR_ACCESS      -5

/* Maximum sizes */
#define SMB_MAX_USERNAME    256
#define SMB_MAX_DOMAIN     256
#define SMB_MAX_PASSWORD   256
#define SMB_CHALLENGE_SIZE  8
#define SMB_HASH_SIZE     16
#define SMB_SESSION_KEY   16

/* Session flags */
#define SMB_SESSION_VALID           0x0001
#define SMB_SESSION_ENCRYPTED       0x0002
#define SMB_SESSION_SIGNED          0x0004
#define SMB_SESSION_GUEST          0x0008

/* NTLM message types */
#define NTLM_MSG_TYPE1     1
#define NTLM_MSG_TYPE2     2
#define NTLM_MSG_TYPE3     3

/* Structures */
struct smb_ntlm_challenge {
    uint8_t  challenge[SMB_CHALLENGE_SIZE];
    uint32_t server_flags;
    char     target_name[SMB_MAX_DOMAIN];
    size_t   target_name_len;
};

struct smb_session {
    uint32_t        id;
    uint32_t        flags;
    uint8_t         auth_type;
    char            username[SMB_MAX_USERNAME];
    char            domain[SMB_MAX_DOMAIN];
    uint8_t         session_key[SMB_SESSION_KEY];
    
    /* NTLM specific */
    struct smb_ntlm_challenge challenge;
    uint8_t         ntlm_hash[SMB_HASH_SIZE];
    uint32_t        ntlm_flags;
    
    /* Statistics */
    uint64_t        bytes_sent;
    uint64_t        bytes_received;
    time_t          creation_time;
    time_t          last_access;
};

struct smb_server {
    char            name[256];
    uint32_t        capabilities;
    struct smb_session *sessions;
    int             num_sessions;
    int             max_sessions;
    
    /* Security settings */
    uint32_t        auth_methods;
    bool            require_signing;
    bool            require_encryption;
    
    /* Statistics */
    uint32_t        auth_success;
    uint32_t        auth_failures;
};

/* Function declarations */
static struct smb_server *smb_alloc_server(const char *name);
static void smb_free_server(struct smb_server *server);
static struct smb_session *smb_alloc_session(struct smb_server *server);
static void smb_free_session(struct smb_session *session);
static int smb_auth_ntlm(struct smb_session *session, const char *username,
                        const char *domain, const char *password);
static void smb_generate_challenge(struct smb_ntlm_challenge *challenge);
static int smb_verify_ntlm_response(struct smb_session *session,
                                  const uint8_t *response);
static void smb_generate_session_key(struct smb_session *session);
static void smb_dump_session(struct smb_session *session);
static void smb_dump_server(struct smb_server *server);

/* Simple NTLM hash function (for demonstration) */
static void ntlm_hash(const char *password, uint8_t *hash) {
    size_t len = strlen(password);
    uint32_t h = 0x67452301;
    
    for (size_t i = 0; i < len; i++) {
        h = ((h << 5) + h) + password[i];
    }
    
    for (int i = 0; i < SMB_HASH_SIZE; i++) {
        hash[i] = (h >> (i * 8)) & 0xFF;
    }
}

/* Allocate SMB server */
static struct smb_server *smb_alloc_server(const char *name) {
    struct smb_server *server;
    
    server = calloc(1, sizeof(*server));
    if (!server)
        return NULL;
    
    strncpy(server->name, name, sizeof(server->name) - 1);
    server->max_sessions = 100;
    server->sessions = calloc(server->max_sessions, sizeof(struct smb_session));
    if (!server->sessions) {
        free(server);
        return NULL;
    }
    
    server->auth_methods = (1 << SMB_AUTH_NTLM) | (1 << SMB_AUTH_NTLMSSP);
    server->require_signing = true;
    
    return server;
}

/* Free SMB server */
static void smb_free_server(struct smb_server *server) {
    if (!server)
        return;
    
    for (int i = 0; i < server->num_sessions; i++)
        smb_free_session(&server->sessions[i]);
    
    free(server->sessions);
    free(server);
}

/* Allocate SMB session */
static struct smb_session *smb_alloc_session(struct smb_server *server) {
    struct smb_session *session = NULL;
    
    if (server->num_sessions >= server->max_sessions)
        return NULL;
    
    session = &server->sessions[server->num_sessions];
    memset(session, 0, sizeof(*session));
    session->id = rand();
    session->creation_time = time(NULL);
    server->num_sessions++;
    
    return session;
}

/* Free SMB session */
static void smb_free_session(struct smb_session *session) {
    if (!session)
        return;
    
    memset(session, 0, sizeof(*session));
}

/* Generate NTLM challenge */
static void smb_generate_challenge(struct smb_ntlm_challenge *challenge) {
    for (int i = 0; i < SMB_CHALLENGE_SIZE; i++)
        challenge->challenge[i] = rand() & 0xFF;
    
    challenge->server_flags = NTLM_NEGOTIATE_UNICODE |
                            NTLM_NEGOTIATE_NTLM |
                            NTLM_NEGOTIATE_ALWAYS_SIGN |
                            NTLM_NEGOTIATE_EXTENDED_SESSIONSEC |
                            NTLM_NEGOTIATE_VERSION |
                            NTLM_NEGOTIATE_128 |
                            NTLM_NEGOTIATE_KEY_EXCH;
}

/* Verify NTLM response */
static int smb_verify_ntlm_response(struct smb_session *session,
                                  const uint8_t *response) {
    /* Simple verification (for demonstration) */
    return memcmp(session->ntlm_hash, response, SMB_HASH_SIZE) == 0 ?
           SMB_SUCCESS : SMB_ERR_AUTH;
}

/* Generate session key */
static void smb_generate_session_key(struct smb_session *session) {
    /* Simple key generation (for demonstration) */
    for (int i = 0; i < SMB_SESSION_KEY; i++)
        session->session_key[i] = rand() & 0xFF;
}

/* NTLM authentication */
static int smb_auth_ntlm(struct smb_session *session, const char *username,
                        const char *domain, const char *password) {
    uint8_t response[SMB_HASH_SIZE];
    int ret;
    
    /* Store credentials */
    strncpy(session->username, username, SMB_MAX_USERNAME - 1);
    strncpy(session->domain, domain, SMB_MAX_DOMAIN - 1);
    
    /* Generate challenge */
    smb_generate_challenge(&session->challenge);
    
    /* Calculate NTLM hash (normally this would be more complex) */
    ntlm_hash(password, session->ntlm_hash);
    
    /* Simulate client response */
    memcpy(response, session->ntlm_hash, SMB_HASH_SIZE);
    
    /* Verify response */
    ret = smb_verify_ntlm_response(session, response);
    if (ret != SMB_SUCCESS)
        return ret;
    
    /* Generate session key */
    smb_generate_session_key(session);
    
    session->flags |= SMB_SESSION_VALID;
    session->auth_type = SMB_AUTH_NTLM;
    session->last_access = time(NULL);
    
    return SMB_SUCCESS;
}

/* Dump session info */
static void smb_dump_session(struct smb_session *session) {
    printf("\nSession Info:\n");
    printf("============\n");
    printf("ID: 0x%08x\n", session->id);
    printf("Username: %s\n", session->username);
    printf("Domain: %s\n", session->domain);
    printf("Auth Type: %d\n", session->auth_type);
    printf("Flags: 0x%08x\n", session->flags);
    printf("Creation Time: %ld\n", session->creation_time);
    printf("Last Access: %ld\n", session->last_access);
    printf("Bytes Sent: %lu\n", session->bytes_sent);
    printf("Bytes Received: %lu\n", session->bytes_received);
    
    printf("Session Key: ");
    for (int i = 0; i < SMB_SESSION_KEY; i++)
        printf("%02x", session->session_key[i]);
    printf("\n");
}

/* Dump server info */
static void smb_dump_server(struct smb_server *server) {
    printf("\nServer Info:\n");
    printf("============\n");
    printf("Name: %s\n", server->name);
    printf("Active Sessions: %d\n", server->num_sessions);
    printf("Max Sessions: %d\n", server->max_sessions);
    printf("Auth Methods: 0x%08x\n", server->auth_methods);
    printf("Require Signing: %s\n", server->require_signing ? "yes" : "no");
    printf("Require Encryption: %s\n", server->require_encryption ? "yes" : "no");
    printf("Auth Success: %u\n", server->auth_success);
    printf("Auth Failures: %u\n", server->auth_failures);
}

/* Test functions */
static void test_ntlm_auth(struct smb_server *server) {
    printf("\nTesting NTLM authentication...\n");
    
    /* Create session */
    struct smb_session *session = smb_alloc_session(server);
    if (!session) {
        printf("Failed to allocate session\n");
        return;
    }
    
    /* Test valid credentials */
    printf("Testing valid credentials...\n");
    int ret = smb_auth_ntlm(session, "testuser", "TESTDOMAIN", "password123");
    if (ret == SMB_SUCCESS) {
        printf("Authentication successful!\n");
        server->auth_success++;
        smb_dump_session(session);
    } else {
        printf("Authentication failed: %d\n", ret);
        server->auth_failures++;
    }
    
    smb_free_session(session);
    
    /* Test invalid credentials */
    printf("\nTesting invalid credentials...\n");
    session = smb_alloc_session(server);
    if (!session) {
        printf("Failed to allocate session\n");
        return;
    }
    
    ret = smb_auth_ntlm(session, "baduser", "TESTDOMAIN", "wrongpass");
    if (ret == SMB_SUCCESS) {
        printf("Authentication unexpectedly succeeded\n");
        server->auth_success++;
    } else {
        printf("Authentication failed as expected: %d\n", ret);
        server->auth_failures++;
    }
    
    smb_free_session(session);
}

static void test_session_limits(struct smb_server *server) {
    printf("\nTesting session limits...\n");
    
    /* Try to create more than max sessions */
    int original_max = server->max_sessions;
    server->max_sessions = 2;
    
    for (int i = 0; i < 3; i++) {
        struct smb_session *session = smb_alloc_session(server);
        if (session) {
            printf("Created session %d\n", i + 1);
            smb_auth_ntlm(session, "user", "DOMAIN", "pass");
            smb_free_session(session);
        } else {
            printf("Failed to create session %d (expected)\n", i + 1);
        }
    }
    
    server->max_sessions = original_max;
}

int main(void) {
    printf("SMB Server Authentication Test Program\n");
    printf("====================================\n\n");
    
    /* Seed random number generator */
    srand(time(NULL));
    
    /* Create server */
    struct smb_server *server = smb_alloc_server("TESTSERVER");
    if (!server) {
        printf("Failed to allocate server\n");
        return 1;
    }
    
    /* Run tests */
    test_ntlm_auth(server);
    test_session_limits(server);
    
    /* Display final status */
    smb_dump_server(server);
    
    /* Cleanup */
    smb_free_server(server);
    
    printf("\nTest completed successfully!\n");
    return 0;
}
