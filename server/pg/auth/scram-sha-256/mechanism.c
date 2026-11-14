#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include "utils/utils.h"
#include "server/core/bytebuf.h"
#include "server/core/base64.h"
#include "server/core/config.h"
#include "server/pg/wire.h"
#include "env_parser/env.h"

#define MECHANISM_NAME "SCRAM-SHA-256"
#define GS2_HEADER "n,,"
#define CLIENT_FINAL_PREFIX "c=biws," // base64("n,,")

#define SCRAM_KEY_LEN 32 // 256 bits for SHA-256
#define SCRAM_NONCE_LEN 20

#define AUTH_SASL_CONTINUE 11
#define AUTH_SASL_FINAL 12
#define AuthenticationOk 0

typedef struct
{
    char client_nonce[SCRAM_NONCE_LEN + 1];
    char *client_first_bare; // "n=,r=<nonce>"
    char *server_first;      // exactly as received
    char *combined_nonce;    // value of r=
    uint8_t *salt;           // base64-decoded
    size_t salt_len;
    int iterations;
    char *client_final_without_proof;
    char *auth_message;
    uint8_t server_signature[SCRAM_KEY_LEN];
} ScramSession;

// --- Helpers forward declaration ---
static void scram_free(ScramSession *s);
static void generate_nonce(char *out, size_t len);
static ByteBuffer *build_sasl_initial_response(ScramSession *s);
static int server_sigs_match(ScramSession *s, const char *msg, size_t len);
static int parse_server_first(ScramSession *s, const char *msg, size_t len);
static void build_auth_message(ScramSession *s);
static int compute_proof(ScramSession *s, const char *password, uint8_t proof[SCRAM_KEY_LEN]);
static ByteBuffer *build_sasl_response(const ScramSession *s, const uint8_t proof[SCRAM_KEY_LEN]);
static void hmac256(const uint8_t *key, const char *msg, size_t msg_len, uint8_t out[SCRAM_KEY_LEN]);
static int expect_auth(DBResponse *res, uint32_t code);

// --- Main logic ---
int authenticate_scram_sha_256(int db_fd)
{
    const char *password = get_env_val("DB_PASSWORD");

    ScramSession s = {0};
    ByteBuffer *req = NULL;
    DBResponse *res = NULL;
    uint8_t proof[SCRAM_KEY_LEN];
    int rc = -1;

    // Send SASLInitialResponse:
    req = build_sasl_initial_response(&s);
    if (send_db_request(db_fd, req) < 0)
    {
        goto done;
    }
    free_ByteBuffer(req);
    req = NULL;

    // Read AuthenticationSASLContinue:
    res = read_db_response(db_fd);
    if (!expect_auth(res, AUTH_SASL_CONTINUE) ||
        parse_server_first(&s, (const char *)res->data + 4, res->len - 4) < 0)
    {
        goto done;
    }
    free_DBResponse(res);
    res = NULL;

    // Compute proof and send SASLResponse:
    build_auth_message(&s);
    if (compute_proof(&s, password, proof) < 0)
    {
        goto done;
    }

    req = build_sasl_response(&s, proof);
    if (send_db_request(db_fd, req) < 0)
    {
        fprintf(stderr, "Failed to send SASLResponse to the DB\n");
        goto done;
    }
    free_ByteBuffer(req);
    req = NULL;

    // Read AuthenticationSASLFinal:
    res = read_db_response(db_fd);
    if (!expect_auth(res, AUTH_SASL_FINAL) || (server_sigs_match(&s, (const char *)res->data + 4, res->len - 4) < 0))
    {
        goto done;
    }
    free_DBResponse(res);
    res = NULL;

    // By this point, we should get AuthenticationOk.
    res = read_db_response(db_fd);
    if (!expect_auth(res, AuthenticationOk))
    {
        goto done;
    }
    free_DBResponse(res);
    res = NULL;

    rc = 0;

done:
    if (res != NULL)
    {
        free_DBResponse(res);
    }
    free_ByteBuffer(req);
    scram_free(&s);
    return rc;
}

// --- Helpers ---
static void scram_free(ScramSession *s)
{
    free(s->client_first_bare);
    free(s->server_first);
    free(s->combined_nonce);
    free(s->salt);
    free(s->client_final_without_proof);
    free(s->auth_message);
}

// Fills out with len random alphanumeric chars and null terminates it.
static void generate_nonce(char *out, size_t len)
{
    const char charset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

    for (size_t i = 0; i < len; i++)
    {
        out[i] = charset[arc4random_uniform(sizeof(charset) - 1)];
    }
    out[len] = '\0';
}

static ByteBuffer *build_sasl_initial_response(ScramSession *s)
{
    generate_nonce(s->client_nonce, SCRAM_NONCE_LEN);

    ByteBuffer *bare = bb_create(32);
    bb_write_string(bare, "n=,r=", 5, 0);
    bb_write_string(bare, s->client_nonce, SCRAM_NONCE_LEN, 0);
    s->client_first_bare = bb_detach_cstr(bare);

    size_t first_len = strlen(GS2_HEADER) + strlen(s->client_first_bare);

    ByteBuffer *bb = msg_begin('p', 64);
    bb_write_string(bb, MECHANISM_NAME, strlen(MECHANISM_NAME), 1);
    bb_write_u32_be(bb, (uint32_t)first_len);
    bb_write_string(bb, GS2_HEADER, strlen(GS2_HEADER), 0);
    bb_write_string(bb, s->client_first_bare, strlen(s->client_first_bare), 0);
    msg_end(bb);

    return bb;
}

// Checks that res is an Authentication ('R') message with the given code.
static int expect_auth(DBResponse *res, uint32_t code)
{
    if (res == NULL || res->type != 'R' || res->len < 4 || res->len > MAXIMUM_ALLOWED_PAYLOAD)
    {
        return 0;
    }
    return (uint32_t)parse_be32_to_int32_t(res->data) == code;
}

// Verifies the server sent signature matches what we computed.
// returns 0 if they match, otherwise -1.
static int server_sigs_match(ScramSession *s, const char *msg, size_t len)
{
    if (memcmp(base64_encode(s->server_signature, SCRAM_KEY_LEN), msg + 2, len - 2) == 0) // +2 in order to skip 'v='
    {
        return 0;
    }

    return -1;
}

// Parses "r=<nonce>,s=<salt>,i=<iterations>".
static int parse_server_first(ScramSession *s, const char *msg, size_t len)
{
    s->server_first = dup_str(msg, len);

    const char *p = s->server_first;
    const char *end;

    // r= (combined nonce)
    if (strncmp(p, "r=", 2) != 0)
    {
        goto malformed;
    }
    p += 2;
    if ((end = strchr(p, ',')) == NULL || end == p)
    {
        goto malformed;
    }
    s->combined_nonce = dup_str(p, end - p);
    p = end + 1;

    // s= (salt)
    if (strncmp(p, "s=", 2) != 0)
    {
        goto malformed;
    }
    p += 2;
    if ((end = strchr(p, ',')) == NULL || end == p)
    {
        goto malformed;
    }
    s->salt = base64_decode(p, end - p, &s->salt_len);
    if (s->salt == NULL)
    {
        goto malformed;
    }
    p = end + 1;

    // i= (iteration count)
    if (strncmp(p, "i=", 2) != 0)
    {
        goto malformed;
    }
    p += 2;
    char *num_end;
    long iterations = strtol(p, &num_end, 10);
    if (num_end == p || *num_end != '\0' || iterations <= 0 || iterations > INT_MAX)
    {
        goto malformed;
    }
    s->iterations = (int)iterations;

    // The server nonce must extend ours.
    if (strncmp(s->combined_nonce, s->client_nonce, SCRAM_NONCE_LEN) != 0)
    {
        fprintf(stderr, "Server nonce does not start with the client nonce\n");
        return -1;
    }

    return 0;

malformed:
    fprintf(stderr, "Received malformed AuthenticationSASLContinue\n");
    return -1;
}

// auth_message = client-first-bare "," server-first "," client-final-without-proof
static void build_auth_message(ScramSession *s)
{
    ByteBuffer *cf = bb_create(64);
    bb_write_string(cf, CLIENT_FINAL_PREFIX "r=", strlen(CLIENT_FINAL_PREFIX) + 2, 0);
    bb_write_string(cf, s->combined_nonce, strlen(s->combined_nonce), 0);
    s->client_final_without_proof = bb_detach_cstr(cf);

    ByteBuffer *am = bb_create(256);
    bb_write_string(am, s->client_first_bare, strlen(s->client_first_bare), 0);
    bb_write_u8(am, ',');
    bb_write_string(am, s->server_first, strlen(s->server_first), 0);
    bb_write_u8(am, ',');
    bb_write_string(am, s->client_final_without_proof, strlen(s->client_final_without_proof), 0);
    s->auth_message = bb_detach_cstr(am);
}

static void hmac256(const uint8_t *key, const char *msg, size_t msg_len, uint8_t out[SCRAM_KEY_LEN])
{
    unsigned int len;
    HMAC(EVP_sha256(), key, SCRAM_KEY_LEN, (const unsigned char *)msg, msg_len, out, &len);
}

// Computes the client proof and stores the expected server signature in s.
static int compute_proof(ScramSession *s, const char *password, uint8_t proof[SCRAM_KEY_LEN])
{
    uint8_t salted[SCRAM_KEY_LEN], client_key[SCRAM_KEY_LEN], stored_key[SCRAM_KEY_LEN];
    uint8_t server_key[SCRAM_KEY_LEN], client_sig[SCRAM_KEY_LEN];

    if (PKCS5_PBKDF2_HMAC(password, strlen(password),
                          s->salt, (int)s->salt_len,
                          s->iterations, EVP_sha256(),
                          sizeof(salted), salted) != 1)
    {
        return -1;
    }

    size_t am_len = strlen(s->auth_message);

    hmac256(salted, "Client Key", 10, client_key);
    SHA256(client_key, SCRAM_KEY_LEN, stored_key);
    hmac256(salted, "Server Key", 10, server_key);

    hmac256(stored_key, s->auth_message, am_len, client_sig);
    hmac256(server_key, s->auth_message, am_len, s->server_signature);

    for (int i = 0; i < SCRAM_KEY_LEN; i++)
    {
        proof[i] = client_key[i] ^ client_sig[i];
    }

    return 0;
}

// SASLResponse: client-final-without-proof ",p=" base64(proof)
static ByteBuffer *build_sasl_response(const ScramSession *s, const uint8_t proof[SCRAM_KEY_LEN])
{
    char *proof_b64 = base64_encode(proof, SCRAM_KEY_LEN);

    ByteBuffer *bb = msg_begin('p', 64);
    bb_write_string(bb, s->client_final_without_proof, strlen(s->client_final_without_proof), 0);
    bb_write_string(bb, ",p=", 3, 0);
    bb_write_string(bb, proof_b64, strlen(proof_b64), 0);
    msg_end(bb);

    free(proof_b64);
    return bb;
}
