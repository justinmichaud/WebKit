# - Try to find LLDB's C++ SB API.
# Once done, this will define
#
#  LLDB_FOUND - LLDB's SB API was found
#  LLDB_INCLUDE_DIRS - the LLDB include directories
#  LLDB_LIBRARIES - link these to use LLDB.
#
# A distribution that installs LLDB under a versioned prefix, as Debian and
# Ubuntu do with /usr/lib/llvm-<n>, is searched there as well as in the default
# paths. Set LLDB_ROOT to point the search at one particular install.
#
# Copyright (C) 2026 Igalia S.L.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
# 1.  Redistributions of source code must retain the above copyright
#     notice, this list of conditions and the following disclaimer.
# 2.  Redistributions in binary form must reproduce the above copyright
#     notice, this list of conditions and the following disclaimer in the
#     documentation and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER AND ITS CONTRIBUTORS ``AS
# IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
# THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
# PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR ITS
# CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
# EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
# PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
# OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
# WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
# OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
# ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

# Newest first, so that a machine with several LLVM installs gets the one whose
# headers and library are most likely to match the rest of the toolchain.
set(LLDB_VERSIONED_PREFIXES)
foreach (_lldb_version RANGE 22 15 -1)
    list(APPEND LLDB_VERSIONED_PREFIXES "/usr/lib/llvm-${_lldb_version}")
endforeach ()

find_path(LLDB_INCLUDE_DIR
    NAMES lldb/API/LLDB.h
    HINTS ${LLDB_ROOT}
    PATHS ${LLDB_VERSIONED_PREFIXES}
    PATH_SUFFIXES include
)

# The library has to come from the same prefix as the header: liblldb makes no
# promise of ABI stability between versions, so a header from one install and a
# library from another is not a combination that can be linked.
if (LLDB_INCLUDE_DIR)
    get_filename_component(LLDB_PREFIX "${LLDB_INCLUDE_DIR}" DIRECTORY)
    find_library(LLDB_LIBRARY
        NAMES lldb
        HINTS "${LLDB_PREFIX}"
        PATH_SUFFIXES lib lib64
        NO_DEFAULT_PATH
    )
endif ()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LLDB
    REQUIRED_VARS LLDB_INCLUDE_DIR LLDB_LIBRARY
)

if (LLDB_LIBRARY AND NOT TARGET LLDB::LLDB)
    add_library(LLDB::LLDB UNKNOWN IMPORTED GLOBAL)
    set_target_properties(LLDB::LLDB PROPERTIES
        IMPORTED_LOCATION "${LLDB_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LLDB_INCLUDE_DIR}"
    )
endif ()

mark_as_advanced(
    LLDB_INCLUDE_DIR
    LLDB_LIBRARY
)

if (LLDB_FOUND)
    set(LLDB_LIBRARIES ${LLDB_LIBRARY})
    set(LLDB_INCLUDE_DIRS ${LLDB_INCLUDE_DIR})
endif ()
