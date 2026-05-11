# Copyright (c) 2026 TidesDB Corp.
# 
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
# 
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
# 
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

############# Optional Plugin-level ASAN + UBSAN #############
# You enable with cmake -DTIDESDB_WITH_ASAN=ON -DTIDESDB_WITH_UBSAN=ON ..
# This instruments only the TidesDB plugin .so, not the whole server, though its good to build the server with sanitizers too.
# At runtime, LD_PRELOAD the ASAN runtime for shared-lib mode:
#   export LD_PRELOAD=$(gcc -print-file-name=libasan.so)
#   export ASAN_OPTIONS=detect_leaks=0:abort_on_error=1
OPTION(TIDESDB_WITH_ASAN "Enable AddressSanitizer for TidesDB plugin" OFF)
OPTION(TIDESDB_WITH_UBSAN "Enable UndefinedBehaviorSanitizer for TidesDB plugin" OFF)

SET(TIDESDB_SANITIZER_COMPILE_FLAGS "")
SET(TIDESDB_SANITIZER_LINK_FLAGS "")
IF(TIDESDB_WITH_ASAN)
  LIST(APPEND TIDESDB_SANITIZER_COMPILE_FLAGS -fsanitize=address -fno-omit-frame-pointer)
  LIST(APPEND TIDESDB_SANITIZER_LINK_FLAGS -fsanitize=address)
ENDIF()
IF(TIDESDB_WITH_UBSAN)
  LIST(APPEND TIDESDB_SANITIZER_COMPILE_FLAGS -fsanitize=undefined -fno-sanitize=alignment)
  LIST(APPEND TIDESDB_SANITIZER_LINK_FLAGS -fsanitize=undefined)
ENDIF()

FIND_LIBRARY(TIDESDB_LIB
        NAMES tidesdb
        HINTS
        ENV TIDESDB_ROOT
        ENV LIBRARY_PATH
        PATHS
        /usr/local/lib
        /usr/lib
        /usr/lib/x86_64-linux-gnu
        /opt/local/lib
        /opt/homebrew/lib
        "C:/Program Files/tidesdb/lib"
        "C:/Program Files (x86)/tidesdb/lib"
        PATH_SUFFIXES lib
)

############# We find compression libraries and always use FIND_LIBRARY to get full paths.#############
############# We cannot use pkg-config because it returns library names like "zstd" which #############
############# conflict with MariaDB's own "zstd" MODULE target (provider_zstd plugin).    #############
FIND_LIBRARY(ZSTD_LIBRARY
        NAMES zstd zstd_static
        HINTS ENV LIBRARY_PATH ENV VCPKG_INSTALLED_DIR
        PATHS
        /usr/local/lib
        /usr/lib
        /usr/lib/x86_64-linux-gnu
        /opt/homebrew/lib
        "C:/vcpkg/installed/x64-windows/lib"
        "$ENV{VCPKG_INSTALLATION_ROOT}/installed/x64-windows/lib"
)

FIND_LIBRARY(LZ4_LIBRARY
        NAMES lz4
        HINTS ENV LIBRARY_PATH ENV VCPKG_INSTALLED_DIR
        PATHS
        /usr/local/lib
        /usr/lib
        /usr/lib/x86_64-linux-gnu
        /opt/homebrew/lib
        "C:/vcpkg/installed/x64-windows/lib"
        "$ENV{VCPKG_INSTALLATION_ROOT}/installed/x64-windows/lib"
)

FIND_LIBRARY(SNAPPY_LIBRARY
        NAMES snappy
        HINTS ENV LIBRARY_PATH ENV VCPKG_INSTALLED_DIR
        PATHS
        /usr/local/lib
        /usr/lib
        /usr/lib/x86_64-linux-gnu
        /opt/homebrew/lib
        "C:/vcpkg/installed/x64-windows/lib"
        "$ENV{VCPKG_INSTALLATION_ROOT}/installed/x64-windows/lib"
)

############# We find include directories #############
# We look for tidesdb/db.h to get the parent include directory
# The plugin primarily uses #include <tidesdb/db.h> so we need the parent of the tidesdb folder
FIND_PATH(TIDESDB_INCLUDE_DIR
        NAMES tidesdb/db.h
        HINTS
        ENV TIDESDB_ROOT
        ENV CPATH
        PATHS
        /usr/local/include
        /usr/include
        /opt/local/include
        /opt/homebrew/include
        "C:/tidesdb/include"
        "C:/Program Files/tidesdb/include"
        "C:/Program Files (x86)/tidesdb/include"
        PATH_SUFFIXES include
)

############# Validate required dependencies #############
IF(NOT TIDESDB_LIB)
  MESSAGE(STATUS "TidesDB: library not found - plugin will not be built. Install TidesDB or set TIDESDB_ROOT.")
  RETURN()
ENDIF()

IF(NOT TIDESDB_INCLUDE_DIR)
  MESSAGE(STATUS "TidesDB: tidesdb/db.h header not found - plugin will not be built. Install TidesDB headers or set TIDESDB_ROOT.")
  RETURN()
ENDIF()

IF(NOT ZSTD_LIBRARY)
  MESSAGE(STATUS "TidesDB: zstd library not found - plugin will not be built. Install libzstd-dev.")
  RETURN()
ENDIF()

IF(NOT LZ4_LIBRARY)
  MESSAGE(STATUS "TidesDB: lz4 library not found - plugin will not be built. Install liblz4-dev.")
  RETURN()
ENDIF()

############# Add include directories #############
INCLUDE_DIRECTORIES(${TIDESDB_INCLUDE_DIR})

############# Build list of libraries to link -- we use full paths from FIND_LIBRARY ############
SET(TIDESDB_LIBS ${TIDESDB_LIB} ${ZSTD_LIBRARY} ${LZ4_LIBRARY})

IF(SNAPPY_LIBRARY)
  LIST(APPEND TIDESDB_LIBS ${SNAPPY_LIBRARY})
ENDIF()

############# On Windows, we need pthreads library ############
IF(WIN32)
  FIND_LIBRARY(PTHREADS_LIBRARY
          NAMES pthreadVC3 pthreadVC2 pthread pthreads
          HINTS ENV LIBRARY_PATH ENV VCPKG_INSTALLED_DIR
          PATHS
          "C:/vcpkg/installed/x64-windows/lib"
          "$ENV{VCPKG_INSTALLATION_ROOT}/installed/x64-windows/lib"
  )
  IF(PTHREADS_LIBRARY)
    LIST(APPEND TIDESDB_LIBS ${PTHREADS_LIBRARY})
  ENDIF()
ENDIF()

############# Optional S3 object store connector #############
OPTION(TIDESDB_WITH_S3 "Enable S3-compatible object store connector" OFF)
IF(TIDESDB_WITH_S3)
  FIND_PACKAGE(CURL REQUIRED)
  FIND_PACKAGE(OpenSSL REQUIRED)
  LIST(APPEND TIDESDB_LIBS CURL::libcurl OpenSSL::SSL OpenSSL::Crypto)
  MESSAGE(STATUS "TidesDB: S3 object store connector enabled")
ENDIF()

############# Define the storage engine using MySQL's MYSQL_ADD_PLUGIN macro #############
MYSQL_ADD_PLUGIN(tidesdb ha_tidesdb.cc tidesdb_keyring_compat.cc
                 STORAGE_ENGINE MODULE_ONLY LINK_LIBRARIES ${TIDESDB_LIBS})

############# S3 compile definition for the plugin #############
IF(TIDESDB_WITH_S3)
  IF(TARGET tidesdb)
    TARGET_COMPILE_DEFINITIONS(tidesdb PRIVATE TIDESDB_WITH_S3)
  ENDIF()
ENDIF()

############# Soften strictness for the plugin target.                     #############
############# Tracked work item -- run scripts/check-strict-build.sh        #############
############# to enumerate the warnings these flags hide.                   #############
IF(TARGET tidesdb)
  # MySQL's stock build flags include -Werror which turns every warning into
  # a hard fail. We turn -Werror OFF for the plugin target only -- the rest
  # of the server keeps strict warnings. -fpermissive lets C/C++ linkage
  # mismatches on plugin var-update callbacks become warnings rather than
  # errors.
  #
  # Auditable work item (not blocked by anything; just needs a full build):
  # set TIDESDB_STRICT_BUILD=ON when configuring CMake to drop both flags
  # and see every remaining warning. Fix in batches, then make strict the
  # default.
  IF(NOT TIDESDB_STRICT_BUILD)
    TARGET_COMPILE_OPTIONS(tidesdb PRIVATE
      -Wno-error
      -fpermissive
    )
  ENDIF()
  # MySQL's data-dictionary headers (pulled in transitively from sql/handler.h
  # via sql/table.h -> sql/dd/types/foreign_key.h -> sql/dd/sdi_fwd.h) include
  # <rapidjson/fwd.h>. MYSQL_ADD_PLUGIN does not put the bundled rapidjson on
  # the plugin's include path automatically. Other engines (innobase, ndb)
  # add it explicitly the same way.
  TARGET_INCLUDE_DIRECTORIES(tidesdb PRIVATE
    ${CMAKE_SOURCE_DIR}/extra/rapidjson/include
  )
  # ABI alignment: MySQL builds rapidjson with these compile definitions
  # everywhere it's used (see cmake/rapidjson.cmake's extra::rapidjson
  # INTERFACE target). Forgetting them in the plugin causes rapidjson's
  # internal types (SizeType, std::string overloads) to mismatch between
  # the plugin's translation unit and the server's, which manifests as a
  # `stackTop_` Debug-build assertion inside rapidjson's internal Stack
  # when the plugin parses ENGINE_ATTRIBUTE JSON. Keep these in lock-step
  # with cmake/rapidjson.cmake.
  TARGET_COMPILE_DEFINITIONS(tidesdb PRIVATE
    RAPIDJSON_NO_SIZETYPEDEFINE
    RAPIDJSON_SCHEMA_USE_INTERNALREGEX=0
    RAPIDJSON_SCHEMA_USE_STDREGEX=1
    RAPIDJSON_HAS_STDSTRING=1
  )
ENDIF()

############# Apply sanitizer flags to the plugin target only #############
IF(TIDESDB_SANITIZER_COMPILE_FLAGS)
  IF(TARGET tidesdb)
    TARGET_COMPILE_OPTIONS(tidesdb PRIVATE ${TIDESDB_SANITIZER_COMPILE_FLAGS})
    TARGET_LINK_OPTIONS(tidesdb PRIVATE ${TIDESDB_SANITIZER_LINK_FLAGS})
    MESSAGE(STATUS "TidesDB: Sanitizer compile flags: ${TIDESDB_SANITIZER_COMPILE_FLAGS}")
    MESSAGE(STATUS "TidesDB: Sanitizer link flags: ${TIDESDB_SANITIZER_LINK_FLAGS}")
  ENDIF()
ENDIF()
