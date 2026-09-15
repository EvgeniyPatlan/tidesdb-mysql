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
  Telling a TidesDB 9 data directory from a TidesDB 10 one, before opening it.

  This has to happen first, because opening answers the question wrongly.
  TidesDB 10 does not recognise the 9.x layout; it finds nothing it knows,
  concludes the directory is a new database, and writes its own manifest into
  it. The open returns success and every column family is missing -- a server
  that starts perfectly with all the data gone from view. Refusing needs a
  signal read off the directory itself, before the engine is handed it.

  The two layouts are distinct in a way that is easy to test positively:

    TidesDB 9   one subdirectory per column family, each holding config.ini
                plus its own MANIFEST, klog, vlog and wal files. Nothing at
                the top level but LOCK.

    TidesDB 10  flat. MANIFEST, LOCK and the numbered .log / .vlog files at
                the top level, no per-family subdirectory and no config.ini
                anywhere.

  So "a subdirectory containing config.ini" means 9.x. It is a positive test
  for something 9.x always writes rather than the absence of something 10.x
  writes, which is what makes it safe to run on a directory that does not
  exist yet, is empty, or belongs to 10.x -- all of which answer false.

  A directory that 10.x has already written into keeps its 9.x subdirectories,
  so this still fires on a second start. That is deliberate: the data is still
  there, and the operator still needs to be told.

  Header-only and free of server headers on purpose -- the scan is pure, so
  tests can cover it against a fixture without linking mysqld.
*/

#include <dirent.h>
#include <sys/stat.h>

#include <string>

/* True when path names a TidesDB 9 data directory. When it does and
   out_cf_name is given, it receives the name of the column-family directory
   that proved it, so the error message can point at something real.

   False for a path that does not exist, is not a directory, is empty, or
   holds a TidesDB 10 database. */
inline bool tdb_datadir_is_tidesdb9(const std::string &path, std::string *out_cf_name = nullptr)
{
    if (path.empty()) return false;

    DIR *d = opendir(path.c_str());
    if (!d) return false;

    bool found = false;
    struct dirent *ent;
    while (!found && (ent = readdir(d)) != nullptr)
    {
        const std::string name(ent->d_name);
        if (name == "." || name == "..") continue;

        const std::string child = path + "/" + name;
        struct stat st;
        if (stat(child.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        /* d_type is not populated on every filesystem, so the directory test
           above goes through stat rather than trusting it. */
        const std::string marker = child + "/config.ini";
        struct stat mst;
        if (stat(marker.c_str(), &mst) == 0 && S_ISREG(mst.st_mode))
        {
            found = true;
            if (out_cf_name) *out_cf_name = name;
        }
    }
    closedir(d);
    return found;
}
