/* Copyright (c) 2026 TidesDB Corp.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details. */

/* tidesdb_master_key.h
 *
 * Master-key + at-rest encryption subsystem for the TidesDB MySQL plugin.
 * Promoted out of tidesdb_compat.h (where it had been sitting alongside
 * unrelated MariaDB porting shims) as the first step of the architectural
 * extraction recommended in docs/code-review-followup-report.md.
 *
 * The plugin holds a single 32-byte AES-256 master key in process memory,
 * loaded once at plugin init from a file the DBA points to via
 * `tidesdb_master_key_file`. The key drives AES-256-CBC encrypt / decrypt
 * for any table created with `ENGINE_ATTRIBUTE='{"encrypted":true}'`.
 *
 * Real implementations live in tidesdb_master_key.cc -- this header is
 * the public surface every caller (plugin init, row codec) sees.
 *
 * Hardening applied since the original port:
 *   - Stack-key wipe via tdb_secure_zero / explicit_bzero (C-1).
 *   - Page-level mlock + MADV_DONTDUMP on the master key (L-5).
 *   - Generation-counter-invalidated per-thread cache so the hot decrypt
 *     path skips the global mutex (M-8).
 *   - IV-length assertion + size-overflow guard (L-2, L-3). */

#pragma once

/* -----------------------------------------------------------------------
   Flag constants. ENCRYPTION_FLAG_{DECRYPT,ENCRYPT} match the MariaDB
   public API the plugin originally adapted to; ENCRYPTION_KEY_VERSION_INVALID
   is the sentinel `encryption_key_get_latest_version` returns when no
   master key is loaded.
   ----------------------------------------------------------------------- */
#ifndef ENCRYPTION_FLAG_DECRYPT
#define ENCRYPTION_FLAG_DECRYPT 0
#endif
#ifndef ENCRYPTION_FLAG_ENCRYPT
#define ENCRYPTION_FLAG_ENCRYPT 1
#endif
#ifndef ENCRYPTION_KEY_VERSION_INVALID
#define ENCRYPTION_KEY_VERSION_INVALID ((unsigned int)-1)
#endif

/* -----------------------------------------------------------------------
   AES-256-CBC encrypt / decrypt. `flags` is ENCRYPTION_FLAG_ENCRYPT or
   ENCRYPTION_FLAG_DECRYPT. Returns 0 on success, -1 on failure (master
   key not loaded, wrong key length, wrong IV length, cipher error).
   The IV must be TIDESDB_AES_BLOCK_LEN (16) bytes -- static_assert in
   the .cc enforces.
   ----------------------------------------------------------------------- */
int encryption_crypt(const unsigned char *src, unsigned int src_len,
                     unsigned char *dst, unsigned int *dst_len,
                     const unsigned char *key, unsigned int key_len,
                     const unsigned char *iv, unsigned int iv_len,
                     int flags, unsigned int key_id,
                     unsigned int key_version);

/* Returns the byte length needed to hold an AES-256-CBC-PKCS#7 encryption
   of `src_len` bytes. Always rounds up to the next 16-byte boundary, with
   a trailing block when src_len is already aligned. */
unsigned int encryption_encrypted_length(unsigned int src_len,
                                         unsigned int key_id,
                                         unsigned int key_version);

/* Fetch the master key into the caller-provided buffer. Returns 0 on
   success, -1 if no master key is loaded. The caller should secure-zero
   the buffer after use (the encrypt/decrypt helpers in ha_tidesdb.cc do
   this via tdb_secure_zero on every exit path). */
int encryption_key_get(unsigned int key_id, unsigned int key_version,
                       unsigned char *key, unsigned int *key_len);

/* Returns 1 if the master key is loaded, ENCRYPTION_KEY_VERSION_INVALID
   otherwise. The plugin has a single master key with no rotation today;
   `key_id` is accepted for API compatibility but ignored. */
unsigned int encryption_key_get_latest_version(unsigned int key_id);

/* Master-key bootstrap. Called from plugin init with the path the DBA
   set in tidesdb_master_key_file. Returns false on success, true on
   failure (file missing, wrong size, IO error). When the master key is
   not loaded, encryption_key_get returns -1 and any CREATE TABLE that
   asks for encryption fails with a clear error. */
bool tidesdb_master_key_load_from_file(const char *path);

/* TEST-ONLY: clears the loaded key. Bumps the generation counter so
   per-thread TLS caches invalidate on next encryption_key_get. NOT a
   production key-revocation primitive -- worker threads idle between
   encrypt operations retain their TLS copy of the plaintext key until
   they next call encryption_key_get. See the contract block in the
   .cc for the full reasoning. */
void tidesdb_master_key_clear();
