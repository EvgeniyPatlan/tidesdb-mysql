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
  ENGINE_ATTRIBUTE keys that no longer reach the engine.

  An engine version can retire a per-table option in two different ways, and
  the difference matters to whoever has to fix the DDL:

    Relocated -- the setting still exists, but as a server-level system
                 variable rather than something a table carries. The error
                 names the variable that took over.
    Removed   -- the setting is gone because the behaviour it selected is
                 gone. There is nothing to point at, so the error says what
                 happened instead. An error that invents a replacement is
                 worse than one that admits there is none.

  Accepting a retired key silently is the outcome worth avoiding: a CREATE
  TABLE that used to tune a table keeps succeeding while doing nothing, and
  nothing in the output says so.

  The parser this backs looks up only keys it already knows about
  (FindMember per option), so it cannot notice a key it no longer handles.
  This table is what gives it something to notice.

  Header-only and free of server headers on purpose -- the lookup is pure, so
  plugin/tests can cover the table without linking mysqld.
*/

#include <cstddef>
#include <cstring>

enum class TdbLegacyDisposition
{
    Relocated, /* replacement names the sysvar that supersedes it */
    Removed    /* reason says why the behaviour no longer exists */
};

struct TdbLegacyOption
{
    const char *key;
    TdbLegacyDisposition disposition;
    const char *replacement; /* set iff Relocated */
    const char *reason;      /* set iff Removed   */
};

/*
  Sentinel-terminated so the table can legitimately be empty; a zero-length
  array is not valid C++, and this list is empty until an engine version
  actually retires something.
*/
inline constexpr TdbLegacyOption tdb_legacy_options[] = {
    {nullptr, TdbLegacyDisposition::Removed, nullptr, nullptr},
};

/* Returns the entry for a retired key, or nullptr when the key is still live
   (or was never ours -- the parser ignores unknown keys, and so do we).

   The table is a parameter so the search itself can be tested against a
   fixture. Testing only the shipped table would prove nothing while that
   table is empty, and would keep proving nothing if a later edit broke the
   search rather than the data. */
inline const TdbLegacyOption *tdb_legacy_option_lookup(const char *key,
                                                       const TdbLegacyOption *table)
{
    if (key == nullptr || table == nullptr) return nullptr;
    for (const TdbLegacyOption *e = table; e->key != nullptr; e++)
    {
        if (std::strcmp(e->key, key) == 0) return e;
    }
    return nullptr;
}

inline const TdbLegacyOption *tdb_legacy_option_lookup(const char *key)
{
    return tdb_legacy_option_lookup(key, tdb_legacy_options);
}

/* True when the table carries no entries -- the state before an engine
   version retires anything. Lets a caller skip the member walk entirely. */
inline constexpr bool tdb_legacy_options_empty()
{
    return tdb_legacy_options[0].key == nullptr;
}
