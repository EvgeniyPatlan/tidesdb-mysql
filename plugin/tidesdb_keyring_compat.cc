/* Copyright (c) 2026 TidesDB Corp.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

/*
 * Master-key implementation of the MariaDB-style encryption API the rest
 * of the engine calls. The plugin has its own row-level encrypt/decrypt
 * code (tidesdb_encrypt_row_into / tidesdb_decrypt_row in ha_tidesdb.cc)
 * that calls these four functions. We back them with a 32-byte AES-256
 * master key loaded once at plugin init from a file path the DBA sets
 * via the tidesdb_master_key_file system variable.
 *
 * Real key management (key rotation, per-table keys, integration with
 * MySQL's keyring_aes component) is follow-up work; this gives real
 * AES-256-CBC encryption-at-rest with a single master key, which is
 * what the existing tidesdb_encryption MTR test exercises.
 */

#include "my_aes.h"
#include "sql/sql_class.h"  /* pulls in sql_print_error etc. */

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "tidesdb_compat.h"

namespace {

constexpr unsigned int TIDESDB_MASTER_KEY_LEN = 32;  /* AES-256 */
constexpr unsigned int TIDESDB_AES_BLOCK_LEN  = 16;

/* Master key state. Set once at plugin init from a file the DBA points to;
   read concurrently by encrypt/decrypt paths. We use std::atomic<bool> for
   the loaded flag so the read path doesn't take a mutex on every row.

   `g_master_key_gen` is bumped on every load / clear; encryption_key_get
   compares it against a thread_local snapshot to know whether its
   TLS-cached copy of the key is still valid. This lets the steady-state
   read path (the by-far-common case: key loaded once at startup, then
   used by row encrypt/decrypt forever) skip the mutex entirely. */
std::mutex g_master_key_mu;
unsigned char g_master_key[TIDESDB_MASTER_KEY_LEN] = {0};
std::atomic<bool> g_master_key_loaded{false};
std::atomic<uint64_t> g_master_key_gen{0};

}  /* unnamed namespace */

bool tidesdb_master_key_load_from_file(const char *path) {
    if (!path || !path[0]) return true;

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        sql_print_error("[TIDESDB] master key: cannot open '%s' (errno=%d)", path, errno);
        return true;
    }

    unsigned char buf[TIDESDB_MASTER_KEY_LEN];
    size_t n = fread(buf, 1, sizeof(buf), fp);
    int extra = fgetc(fp);  /* must be EOF for the file to be exactly 32 bytes */
    fclose(fp);

    if (n != TIDESDB_MASTER_KEY_LEN || extra != EOF) {
        sql_print_error("[TIDESDB] master key file '%s' must be exactly %u bytes; "
                        "got %zu bytes (extra=%d)",
                        path, TIDESDB_MASTER_KEY_LEN, n, extra);
        memset(buf, 0, sizeof(buf));
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(g_master_key_mu);
        memcpy(g_master_key, buf, TIDESDB_MASTER_KEY_LEN);
        g_master_key_loaded.store(true, std::memory_order_release);
        /* Bump generation so any thread's TLS-cached copy is invalidated
           on next encryption_key_get. */
        g_master_key_gen.fetch_add(1, std::memory_order_acq_rel);
    }
    memset(buf, 0, sizeof(buf));
    sql_print_information("[TIDESDB] master key loaded from '%s' (%u bytes)",
                          path, TIDESDB_MASTER_KEY_LEN);
    return false;
}

void tidesdb_master_key_clear() {
    std::lock_guard<std::mutex> lock(g_master_key_mu);
    memset(g_master_key, 0, sizeof(g_master_key));
    g_master_key_loaded.store(false, std::memory_order_release);
    /* Bump generation so TLS caches invalidate. Threads that already
       had the old key cached and call encryption_key_get again will
       see the generation mismatch, take the mutex, re-check loaded
       (now false), and return -1. */
    g_master_key_gen.fetch_add(1, std::memory_order_acq_rel);
}

unsigned int encryption_key_get_latest_version(unsigned int /*key_id*/) {
    /* Single master key, no rotation. Loaded -> version 1, missing -> invalid. */
    return g_master_key_loaded.load(std::memory_order_acquire)
               ? 1u
               : ENCRYPTION_KEY_VERSION_INVALID;
}

int encryption_key_get(unsigned int /*key_id*/, unsigned int /*key_version*/,
                       unsigned char *key, unsigned int *key_len) {
    /* TLS-cached copy of the master key, valid as long as tls_gen matches
       the global g_master_key_gen. Once the key is loaded (and not
       cleared), the global gen stops changing, so this fast path skips
       the mutex on every subsequent call. Without this cache, every
       encrypted-row read or write serialized through g_master_key_mu --
       which blocks concurrent decrypts from different connections. */
    thread_local unsigned char tls_master_key[TIDESDB_MASTER_KEY_LEN];
    thread_local uint64_t tls_gen = 0;

    uint64_t cur_gen = g_master_key_gen.load(std::memory_order_acquire);
    if (cur_gen != 0 && tls_gen == cur_gen) {
        /* Fast path: TLS cache valid. No mutex. */
        memcpy(key, tls_master_key, TIDESDB_MASTER_KEY_LEN);
        *key_len = TIDESDB_MASTER_KEY_LEN;
        return 0;
    }

    /* Slow path: cold TLS, or generation changed (load/clear happened). */
    std::lock_guard<std::mutex> lock(g_master_key_mu);
    /* Re-check under the lock; another thread may have cleared. */
    if (!g_master_key_loaded.load(std::memory_order_acquire)) {
        if (key_len) *key_len = 0;
        /* Wipe the now-stale TLS copy so it can't be reused after clear. */
        memset(tls_master_key, 0, sizeof(tls_master_key));
        tls_gen = 0;
        return -1;
    }
    memcpy(tls_master_key, g_master_key, TIDESDB_MASTER_KEY_LEN);
    tls_gen = g_master_key_gen.load(std::memory_order_acquire);
    memcpy(key, tls_master_key, TIDESDB_MASTER_KEY_LEN);
    *key_len = TIDESDB_MASTER_KEY_LEN;
    return 0;
}

unsigned int encryption_encrypted_length(unsigned int src_len,
                                         unsigned int /*key_id*/,
                                         unsigned int /*key_version*/) {
    /* AES-256-CBC with PKCS#7 padding always pads to the next 16-byte
       multiple, including a full padding block when src_len is already
       aligned. */
    return ((src_len / TIDESDB_AES_BLOCK_LEN) + 1) * TIDESDB_AES_BLOCK_LEN;
}

int encryption_crypt(const unsigned char *src, unsigned int src_len,
                     unsigned char *dst, unsigned int *dst_len,
                     const unsigned char *key, unsigned int key_len,
                     const unsigned char *iv, unsigned int /*iv_len*/,
                     int flags, unsigned int /*key_id*/,
                     unsigned int /*key_version*/) {
    if (!g_master_key_loaded.load(std::memory_order_acquire)) return -1;
    if (key_len != TIDESDB_MASTER_KEY_LEN) return -1;

    /* my_aes_encrypt/decrypt return the produced byte count, or negative
       on error. They handle PKCS#7 padding when padding=true (default). */
    int rc;
    if (flags == ENCRYPTION_FLAG_ENCRYPT) {
        rc = my_aes_encrypt(src, src_len, dst, key, key_len,
                            my_aes_256_cbc, iv);
    } else {
        rc = my_aes_decrypt(src, src_len, dst, key, key_len,
                            my_aes_256_cbc, iv);
    }
    if (rc < 0) return -1;
    *dst_len = (unsigned int)rc;
    return 0;
}
