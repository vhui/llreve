#
# This file is part of
#    llreve - Automatic regression verification for LLVM programs
#
# Copyright (C) 2016 Karlsruhe Institute of Technology
#
# The system is published under a BSD license.
# See LICENSE (distributed with this file) for details.

# Try to find the GMP librairies
# GMP_FOUND - system has GMP lib
# GMP_INCLUDE_DIR - the GMP include directory
# GMP_LIBRARIES - Libraries needed to use GMP

find_path(Z3_INCLUDE_DIR NAMES z3.h )
find_library(Z3_LIBRARIES NAMES z3 libz3)

include(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(Z3 DEFAULT_MSG Z3_INCLUDE_DIR Z3_LIBRARIES)

mark_as_advanced(Z3_INCLUDE_DIR Z3_LIBRARIES)
