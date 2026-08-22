## Accept UPDATE_GOLDEN from -D or the environment.
include(${CMAKE_CURRENT_LIST_DIR}/../GoldenUpdateMode.cmake)

if (NOT DEFINED ILC)
    message(FATAL_ERROR "ILC not set")
endif ()
if (NOT DEFINED BAS_FILE)
    message(FATAL_ERROR "BAS_FILE not set")
endif ()
if (NOT DEFINED GOLDEN)
    message(FATAL_ERROR "GOLDEN not set")
endif ()
execute_process(COMMAND ${ILC} front basic -emit-il ${BAS_FILE} OUTPUT_VARIABLE out RESULT_VARIABLE res)
if (NOT res EQUAL 0)
    message(FATAL_ERROR "emit-il failed")
endif ()
# Normalize Windows line endings
string(REPLACE "\r\n" "\n" out "${out}")
string(REPLACE "\r" "\n" out "${out}")
## Keep the verbatim compiler output for -DUPDATE_GOLDEN=1. Goldens store the
## real IL version header and the symbol names the compiler actually emits, so
## refreshing one must not bake in the comparison placeholders applied below.
set(out_raw "${out}")
file(READ ${GOLDEN} expected)
string(REPLACE "\r\n" "\n" expected "${expected}")
string(REPLACE "\r" "\n" expected "${expected}")
## Normalize IL version to avoid test churn on version bumps.
string(REGEX REPLACE "^il [0-9]+\\.[0-9]+\\.[0-9]+" "il VERSION" out "${out}")
string(REGEX REPLACE "^il [0-9]+\\.[0-9]+\\.[0-9]+" "il VERSION" expected "${expected}")
## Normalize selected BASIC helper symbols that lower to canonical runtime API calls.
set(_aliases rt_print_str;rt_print_i64;rt_print_f64;rt_str_substr;rt_trap_string;rt_trap;rt_str_concat;rt_input_line;rt_to_int;rt_to_double;rt_parse_int64;rt_parse_double;rt_int_to_str;rt_f64_to_str;rt_str_split_fields;rt_str_i16_alloc;rt_str_i32_alloc;rt_str_f_alloc)
set(_canon Zanna.Terminal.PrintStr;Zanna.Terminal.PrintI64;Zanna.Terminal.PrintF64;Zanna.String.Substring;Zanna.Diagnostics.Trap;Zanna.Diagnostics.Trap;Zanna.String.Concat;Zanna.Terminal.ReadLine;Zanna.Core.Convert.ToInt64;Zanna.Core.Convert.ToDouble;Zanna.Core.Parse.TryInt;Zanna.Core.Parse.TryDouble;Zanna.Core.Convert.ToStringInt;Zanna.Core.Convert.ToStringDouble;Zanna.String.SplitFields;Zanna.String.FromI16;Zanna.String.FromI32;Zanna.String.FromSingle)
list(LENGTH _aliases _n)
math(EXPR _last "${_n} - 1")
foreach (i RANGE 0 ${_last})
    list(GET _aliases ${i} _a)
    list(GET _canon ${i} _c)
    string(REPLACE "@${_a}(" "@${_c}(" out "${out}")
    string(REPLACE "@${_a}(" "@${_c}(" expected "${expected}")
    string(REPLACE "extern @${_a}" "extern @${_c}" out "${out}")
    string(REPLACE "extern @${_a}" "extern @${_c}" expected "${expected}")
endforeach ()
## Extern declaration order may differ across platforms (hash map iteration).
## Compare extern lines sorted, then compare the remaining IL body verbatim.
string(REGEX MATCHALL "extern @[^\n]+" _out_externs "${out}")
string(REGEX MATCHALL "extern @[^\n]+" _exp_externs "${expected}")
list(SORT _out_externs)
list(SORT _exp_externs)
set(_mismatch "")
if (NOT "${_out_externs}" STREQUAL "${_exp_externs}")
    set(_mismatch "Extern declarations mismatch\nExpected:\n${_exp_externs}\nGot:\n${_out_externs}")
endif ()
string(REGEX REPLACE "extern @[^\n]+\n?" "" _out_body "${out}")
string(REGEX REPLACE "extern @[^\n]+\n?" "" _exp_body "${expected}")
if ("${_mismatch}" STREQUAL "" AND NOT "${_out_body}" STREQUAL "${_exp_body}")
    set(_mismatch "IL body mismatch\nExpected:\n${_exp_body}\nGot:\n${_out_body}")
endif ()
## Both drift kinds route through one -DUPDATE_GOLDEN=1 branch. Extern drift is
## the common case when a runtime function becomes always-declared, so failing
## it outright would leave update_goldens.sh unable to refresh these files.
if (NOT "${_mismatch}" STREQUAL "")
    if (DEFINED UPDATE_GOLDEN)
        file(WRITE ${GOLDEN} "${out_raw}")
        message(STATUS "Updated golden: ${GOLDEN}")
    else ()
        message(FATAL_ERROR "${_mismatch}")
    endif ()
endif ()
