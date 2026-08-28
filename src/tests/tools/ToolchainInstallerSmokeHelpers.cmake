function(zanna_installer_smoke_host_codegen_arch out_var)
    set(_host_arch "")
    if (DEFINED ZANNA_HOST_ARCH AND NOT "${ZANNA_HOST_ARCH}" STREQUAL "")
        set(_host_arch "${ZANNA_HOST_ARCH}")
    elseif (DEFINED ENV{PROCESSOR_ARCHITECTURE} AND NOT "$ENV{PROCESSOR_ARCHITECTURE}" STREQUAL "")
        set(_host_arch "$ENV{PROCESSOR_ARCHITECTURE}")
    else ()
        execute_process(
                COMMAND uname -m
                RESULT_VARIABLE _uname_rv
                OUTPUT_VARIABLE _host_arch
                ERROR_VARIABLE _uname_err
                OUTPUT_STRIP_TRAILING_WHITESPACE)
        if (NOT _uname_rv EQUAL 0)
            message(FATAL_ERROR
                    "unable to determine host architecture for installed zanna smoke\nstderr:\n${_uname_err}")
        endif ()
    endif ()

    if (_host_arch MATCHES "^(arm64|ARM64|aarch64)$")
        set(${out_var} arm64 PARENT_SCOPE)
    else ()
        set(${out_var} x64 PARENT_SCOPE)
    endif ()
endfunction()

function(zanna_installer_smoke_required_tool_names out_var)
    set(${out_var}
            zanna
            zia
            zbasic
            ilrun
            il-verify
            il-dis
            zia-server
            zbasic-server
            basic-ast-dump
            basic-lex-dump
            zannastudio
            PARENT_SCOPE)
endfunction()

function(zanna_installer_smoke_require_listing_paths listing label)
    foreach (_path IN LISTS ARGN)
        string(FIND "${listing}" "${_path}" _path_index)
        if (_path_index EQUAL -1)
            message(FATAL_ERROR "${label}: payload listing did not contain '${_path}'\n${listing}")
        endif ()
    endforeach ()
endfunction()

function(zanna_installer_smoke_verify_installed_tools bin_dir exe_suffix label)
    zanna_installer_smoke_required_tool_names(_required_tools)
    foreach (_tool IN LISTS _required_tools)
        set(_tool_path "${bin_dir}/${_tool}${exe_suffix}")
        if (NOT EXISTS "${_tool_path}")
            message(FATAL_ERROR "${label}: missing installed tool ${_tool_path}")
        endif ()
    endforeach ()

    execute_process(
            COMMAND "${bin_dir}/zanna${exe_suffix}" --version
            RESULT_VARIABLE _zanna_version_rv
            OUTPUT_VARIABLE _zanna_version_out
            ERROR_VARIABLE _zanna_version_err)
    if (NOT _zanna_version_rv EQUAL 0)
        message(FATAL_ERROR
                "${label}: installed zanna --version failed\nstdout:\n${_zanna_version_out}\nstderr:\n${_zanna_version_err}")
    endif ()

    execute_process(
            COMMAND "${bin_dir}/zannastudio${exe_suffix}" --version
            RESULT_VARIABLE _zannastudio_version_rv
            OUTPUT_VARIABLE _zannastudio_version_out
            ERROR_VARIABLE _zannastudio_version_err)
    if (NOT _zannastudio_version_rv EQUAL 0)
        message(FATAL_ERROR
                "${label}: installed zannastudio --version failed\nstdout:\n${_zannastudio_version_out}\nstderr:\n${_zannastudio_version_err}")
    endif ()
endfunction()

function(zanna_installer_smoke_verify_cmake_consumer cmake_bin src_dir build_dir config_name label)
    file(REMOVE_RECURSE "${src_dir}" "${build_dir}")
    file(MAKE_DIRECTORY "${src_dir}")
    file(WRITE "${src_dir}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.20)
project(zanna_installed_package_consumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Zanna CONFIG REQUIRED)
foreach(_required_target IN ITEMS
    zanna::runtime
    zanna::rt_base
    zanna::rt_arrays
    zanna::rt_oop
    zanna::rt_collections
    zanna::rt_game
    zanna::rt_text
    zanna::rt_io_fs
    zanna::rt_exec
    zanna::rt_threads
    zanna::rt_graphics
    zanna::rt_audio
    zanna::rt_network)
  if (NOT TARGET ${_required_target})
    message(FATAL_ERROR "Installed Zanna config is missing imported target ${_required_target}")
  endif ()
endforeach()
add_executable(zanna_installed_package_consumer main.cpp)
target_link_libraries(zanna_installed_package_consumer PRIVATE zanna::il_core zanna::il_io)
]=])

    file(WRITE "${src_dir}/main.cpp" [=[
#include <sstream>
#include <zanna/il/core/Module.hpp>
#include <zanna/il/io/Serializer.hpp>

int main() {
    il::core::Module module;
    std::ostringstream out;
    il::io::Serializer::write(module, out);
    return out.str().empty() ? 1 : 0;
}
]=])

    execute_process(
            COMMAND "${cmake_bin}" -S "${src_dir}" -B "${build_dir}"
            RESULT_VARIABLE _cfg_rv
            OUTPUT_VARIABLE _cfg_out
            ERROR_VARIABLE _cfg_err)
    if (NOT _cfg_rv EQUAL 0)
        message(FATAL_ERROR
                "${label}: installed Zanna config was not discoverable by CMake\nstdout:\n${_cfg_out}\nstderr:\n${_cfg_err}")
    endif ()

    set(_consumer_build_cmd "${cmake_bin}" --build "${build_dir}")
    if (NOT "${config_name}" STREQUAL "")
        list(APPEND _consumer_build_cmd --config "${config_name}")
    endif ()
    execute_process(
            COMMAND ${_consumer_build_cmd}
            RESULT_VARIABLE _build_rv
            OUTPUT_VARIABLE _build_out
            ERROR_VARIABLE _build_err)
    if (NOT _build_rv EQUAL 0)
        message(FATAL_ERROR
                "${label}: installed Zanna consumer build failed\nstdout:\n${_build_out}\nstderr:\n${_build_err}")
    endif ()

    if (WIN32)
        set(_exe_suffix ".exe")
    else ()
        set(_exe_suffix "")
    endif ()
    set(_exe_path "${build_dir}/zanna_installed_package_consumer${_exe_suffix}")
    if (NOT "${config_name}" STREQUAL "" AND EXISTS "${build_dir}/${config_name}/zanna_installed_package_consumer${_exe_suffix}")
        set(_exe_path "${build_dir}/${config_name}/zanna_installed_package_consumer${_exe_suffix}")
    endif ()
    if (NOT EXISTS "${_exe_path}")
        message(FATAL_ERROR "${label}: consumer build did not produce ${_exe_path}")
    endif ()

    execute_process(
            COMMAND "${_exe_path}"
            RESULT_VARIABLE _run_rv
            OUTPUT_VARIABLE _run_out
            ERROR_VARIABLE _run_err)
    if (NOT _run_rv EQUAL 0)
        message(FATAL_ERROR
                "${label}: installed Zanna consumer executable failed\nstdout:\n${_run_out}\nstderr:\n${_run_err}")
    endif ()
endfunction()

function(zanna_installer_smoke_verify_native_codegen cmake_bin zanna_bin tmp_root label)
    if (DEFINED ZANNA_RUN_NATIVE_CODEGEN AND NOT ZANNA_RUN_NATIVE_CODEGEN)
        message(STATUS "Skipping installed native codegen smoke; native link is disabled")
        return()
    endif ()

    zanna_installer_smoke_host_codegen_arch(_installed_codegen_arch)
    if (WIN32)
        set(_exe_suffix ".exe")
    else ()
        set(_exe_suffix "")
    endif ()
    set(_installed_il "${tmp_root}/installed_runtime_smoke.il")
    set(_installed_exe "${tmp_root}/installed_runtime_smoke${_exe_suffix}")

    file(WRITE "${_installed_il}" [=[
il 0.3.0

extern @Zanna.Terminal.PrintStr(str) -> void
global const str @.msg = "Hello, installed Zanna!"

func @main() -> i64 {
entry:
  %msg = const_str @.msg
  call @Zanna.Terminal.PrintStr(%msg)
  ret 0
}
]=])

    execute_process(
            COMMAND "${cmake_bin}" -E env --unset=ZANNA_LIB_PATH "${zanna_bin}" codegen "${_installed_codegen_arch}" "${_installed_il}" -o "${_installed_exe}"
            WORKING_DIRECTORY "${tmp_root}"
            RESULT_VARIABLE _codegen_rv
            OUTPUT_VARIABLE _codegen_out
            ERROR_VARIABLE _codegen_err)
    if (NOT _codegen_rv EQUAL 0)
        message(FATAL_ERROR
                "${label}: installed zanna failed to compile native executable outside the build tree\nstdout:\n${_codegen_out}\nstderr:\n${_codegen_err}")
    endif ()
    if (NOT EXISTS "${_installed_exe}")
        message(FATAL_ERROR "${label}: installed zanna did not produce native smoke executable: ${_installed_exe}")
    endif ()

    execute_process(
            COMMAND "${_installed_exe}"
            WORKING_DIRECTORY "${tmp_root}"
            RESULT_VARIABLE _run_rv
            OUTPUT_VARIABLE _run_out
            ERROR_VARIABLE _run_err)
    if (NOT _run_rv EQUAL 0)
        message(FATAL_ERROR
                "${label}: native executable built by installed zanna failed\nstdout:\n${_run_out}\nstderr:\n${_run_err}")
    endif ()
    if (NOT _run_out MATCHES "Hello, installed Zanna!")
        message(FATAL_ERROR
                "${label}: native executable built by installed zanna produced unexpected output\nstdout:\n${_run_out}\nstderr:\n${_run_err}")
    endif ()
endfunction()
