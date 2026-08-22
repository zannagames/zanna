# SPDX-License-Identifier: GPL-3.0-only
# File: src/tests/golden/GoldenUpdateMode.cmake
# Purpose: Normalize how golden runner scripts learn they may refresh a golden
#          instead of failing.
# Key invariants: UPDATE_GOLDEN stays undefined unless a caller explicitly asked
#                 for refresh mode, so an ordinary CTest run can never rewrite a
#                 golden; an empty value and "0" both mean off.
# Links: scripts/update_goldens.sh,
#        src/tests/golden/basic_to_il/BasicToIlBatchRunner.cpp
#
# Refresh mode arrives either as -DUPDATE_GOLDEN=1 on the cmake command line or
# as UPDATE_GOLDEN=1 in the environment. update_goldens.sh uses the environment:
# `ctest -N -V` records every argument quoted, so splicing a -D flag into the
# recorded command line is unreliable, and the compiled batch runner has no
# command line to splice into at all.

if (NOT DEFINED UPDATE_GOLDEN)
    set(_zanna_golden_update_env "$ENV{UPDATE_GOLDEN}")
    if (NOT "${_zanna_golden_update_env}" STREQUAL "" AND
            NOT "${_zanna_golden_update_env}" STREQUAL "0")
        set(UPDATE_GOLDEN "${_zanna_golden_update_env}")
    endif ()
    unset(_zanna_golden_update_env)
endif ()
