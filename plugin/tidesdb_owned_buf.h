/*
  Copyright (c) 2026 TidesDB Corp.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/
#pragma once

/*
  Ownership of a buffer TidesDB allocated and handed to us.

  TidesDB 9 returned iterator keys and values by borrowing: tidesdb_iter_key
  assigned iter->current->key straight out of the iterator's own state, so
  there was nothing to free and freeing would have corrupted the heap. TidesDB
  10 changed both to "newly allocated ... the caller frees". The signatures did
  not change, so nothing failed to compile and every existing call site went on
  reading the key correctly and leaking it -- one key per row, on the index
  scan paths, which is most of the read traffic an OLTP workload generates.

  Freeing by hand at each site is how that becomes a double free instead: these
  sites sit inside scan loops with several early exits each (break, DBUG_RETURN
  on a key mismatch, HA_ERR_END_OF_FILE), and one missed path either leaks
  again or frees twice. A scope guard gets every path by construction.

  TdbFreeGuard attaches to the pointer variable a site already has, so the
  reads and comparisons around it stay exactly as they were -- the change at
  each site is one added line, not a rewrite of every use:

      uint8_t *ik = NULL;
      TdbFreeGuard ik_guard(&ik);          // <- added
      size_t iks = 0;
      if (tidesdb_iter_key(scan_iter, &ik, &iks) != TDB_SUCCESS) break;
      ... memcmp(ik, ...) unchanged ...

  It frees whatever the pointer holds when the scope ends and nulls it, so a
  declaration inside a loop body is freed once per iteration. For a pointer
  declared outside a loop and re-read inside it, use TdbOwnedBuf instead: its
  out() releases the previous buffer before each read, which a scope guard
  cannot do.
*/

#include <cstddef>
#include <cstdint>

/* Frees whatever an existing pointer variable holds, when the scope ends.
   The pointer is nulled too, so a later "if (p)" reads correctly. */
class TdbFreeGuard
{
   public:
    explicit TdbFreeGuard(uint8_t **pp) : pp_(pp) {}

    ~TdbFreeGuard()
    {
        if (pp_ && *pp_)
        {
            tidesdb_free(*pp_);
            *pp_ = nullptr;
        }
    }

    TdbFreeGuard(const TdbFreeGuard &) = delete;
    TdbFreeGuard &operator=(const TdbFreeGuard &) = delete;

   private:
    uint8_t **pp_;
};

class TdbOwnedBuf
{
   public:
    TdbOwnedBuf() = default;

    ~TdbOwnedBuf() { reset(); }

    /* Non-copyable: two owners would free the same buffer twice. Movable so a
       caller can hand ownership on where a scan keeps the last row. */
    TdbOwnedBuf(const TdbOwnedBuf &) = delete;
    TdbOwnedBuf &operator=(const TdbOwnedBuf &) = delete;

    TdbOwnedBuf(TdbOwnedBuf &&o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    TdbOwnedBuf &operator=(TdbOwnedBuf &&o) noexcept
    {
        if (this != &o)
        {
            reset();
            p_ = o.p_;
            o.p_ = nullptr;
        }
        return *this;
    }

    /* The out-parameter to hand the engine. Drops any buffer already held, so
       a loop that reads a key per iteration keeps exactly one alive. */
    uint8_t **out()
    {
        reset();
        return &p_;
    }

    uint8_t *get() const { return p_; }
    const char *as_char() const { return reinterpret_cast<const char *>(p_); }
    explicit operator bool() const { return p_ != nullptr; }

    void reset()
    {
        if (p_)
        {
            tidesdb_free(p_);
            p_ = nullptr;
        }
    }

    /* Give up ownership to a caller that will free it itself. */
    uint8_t *release()
    {
        uint8_t *r = p_;
        p_ = nullptr;
        return r;
    }

   private:
    uint8_t *p_ = nullptr;
};
