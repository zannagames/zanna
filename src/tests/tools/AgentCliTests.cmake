# Tests for the agent-facing CLI surface: zanna check, zanna eval,
# zanna explain, --print-error-codes, --dump-runtime-api, --dump-opcodes.
# Validates exit-code contracts and machine-readable output shapes.
cmake_minimum_required(VERSION 3.20)

if (NOT DEFINED ZANNA_EXE)
    message(FATAL_ERROR "ZANNA_EXE must be provided")
endif ()
if (NOT DEFINED TEST_WORK_DIR)
    message(FATAL_ERROR "TEST_WORK_DIR must be provided")
endif ()

file(REMOVE_RECURSE "${TEST_WORK_DIR}")
file(MAKE_DIRECTORY "${TEST_WORK_DIR}")

# ===== zanna check: clean file exits 0 =====
set(_ok_zia "${TEST_WORK_DIR}/ok.zia")
file(WRITE "${_ok_zia}" "module T;\nbind Zanna.Terminal;\nfunc start() {\n    Say(\"hi\");\n}\n")

execute_process(
        COMMAND "${ZANNA_EXE}" check "${_ok_zia}"
        RESULT_VARIABLE _check_ok_rc
        OUTPUT_VARIABLE _check_ok_out
        ERROR_VARIABLE _check_ok_err)
if (NOT _check_ok_rc EQUAL 0)
    message(FATAL_ERROR "zanna check on a clean file should exit 0, got ${_check_ok_rc}:\n${_check_ok_err}")
endif ()

# ===== zanna check: compile errors exit 2 with structured JSON =====
set(_bad_zia "${TEST_WORK_DIR}/bad.zia")
file(WRITE "${_bad_zia}" "module T;\nbind Zanna.Terminal;\nfunc start() {\n    var count = 1;\n    SayInt(cout + count);\n}\n")

execute_process(
        COMMAND "${ZANNA_EXE}" check "${_bad_zia}" --diagnostic-format=json
        RESULT_VARIABLE _check_bad_rc
        OUTPUT_VARIABLE _check_bad_out
        ERROR_VARIABLE _check_bad_err)
if (NOT _check_bad_rc EQUAL 2)
    message(FATAL_ERROR "zanna check on a broken file should exit 2, got ${_check_bad_rc}:\n${_check_bad_err}")
endif ()
if (NOT _check_bad_err MATCHES "\"code\":\"V-ZIA-UNDEFINED\"")
    message(FATAL_ERROR "zanna check JSON did not include the V-ZIA-UNDEFINED code:\n${_check_bad_err}")
endif ()
if (NOT _check_bad_err MATCHES "\"fixits\"" OR NOT _check_bad_err MATCHES "\"replacement\":\"count\"")
    message(FATAL_ERROR "zanna check JSON did not carry the did-you-mean fix-it:\n${_check_bad_err}")
endif ()

# ===== zanna check: unresolvable target exits 1 =====
execute_process(
        COMMAND "${ZANNA_EXE}" check "${TEST_WORK_DIR}/does-not-exist.zia"
        RESULT_VARIABLE _check_missing_rc
        OUTPUT_VARIABLE _check_missing_out
        ERROR_VARIABLE _check_missing_err)
if (NOT _check_missing_rc EQUAL 1)
    message(FATAL_ERROR "zanna check on a missing target should exit 1, got ${_check_missing_rc}")
endif ()

# ===== zanna check: rejects run/build-only flags =====
execute_process(
        COMMAND "${ZANNA_EXE}" check "${_ok_zia}" -o out.il
        RESULT_VARIABLE _check_o_rc
        OUTPUT_VARIABLE _check_o_out
        ERROR_VARIABLE _check_o_err)
if (_check_o_rc EQUAL 0)
    message(FATAL_ERROR "zanna check should reject -o")
endif ()

# ===== zanna eval: expression auto-print, exit 0 =====
execute_process(
        COMMAND "${ZANNA_EXE}" eval "2 + 3 * 4"
        RESULT_VARIABLE _eval_rc
        OUTPUT_VARIABLE _eval_out
        ERROR_VARIABLE _eval_err)
if (NOT _eval_rc EQUAL 0)
    message(FATAL_ERROR "zanna eval should exit 0, got ${_eval_rc}:\n${_eval_err}")
endif ()
if (NOT _eval_out MATCHES "14")
    message(FATAL_ERROR "zanna eval '2 + 3 * 4' should print 14, got:\n${_eval_out}")
endif ()

# ===== zanna eval --json --type: structured result =====
execute_process(
        COMMAND "${ZANNA_EXE}" eval --json --type "1 + 1"
        RESULT_VARIABLE _eval_json_rc
        OUTPUT_VARIABLE _eval_json_out
        ERROR_VARIABLE _eval_json_err)
if (NOT _eval_json_rc EQUAL 0)
    message(FATAL_ERROR "zanna eval --json should exit 0, got ${_eval_json_rc}:\n${_eval_json_err}")
endif ()
if (NOT _eval_json_out MATCHES "\"success\":true" OR NOT _eval_json_out MATCHES "\"resultType\":\"Integer\"")
    message(FATAL_ERROR "zanna eval --json output malformed:\n${_eval_json_out}")
endif ()
if (NOT _eval_json_out MATCHES "\"type\":\"Integer\"")
    message(FATAL_ERROR "zanna eval --type did not report the inferred type:\n${_eval_json_out}")
endif ()

# ===== zanna eval: compile errors exit 2 =====
execute_process(
        COMMAND "${ZANNA_EXE}" eval "thisIsNotDefined + 1"
        RESULT_VARIABLE _eval_bad_rc
        OUTPUT_VARIABLE _eval_bad_out
        ERROR_VARIABLE _eval_bad_err)
if (NOT _eval_bad_rc EQUAL 2)
    message(FATAL_ERROR "zanna eval on bad code should exit 2, got ${_eval_bad_rc}")
endif ()

# ===== zanna eval: runtime traps exit 3 =====
execute_process(
        COMMAND "${ZANNA_EXE}" eval "1 / 0"
        RESULT_VARIABLE _eval_trap_rc
        OUTPUT_VARIABLE _eval_trap_out
        ERROR_VARIABLE _eval_trap_err)
if (NOT _eval_trap_rc EQUAL 3)
    message(FATAL_ERROR "zanna eval on trapping code should exit 3, got ${_eval_trap_rc}")
endif ()

# ===== zanna explain: cataloged code =====
execute_process(
        COMMAND "${ZANNA_EXE}" explain V-ZIA-UNDEFINED
        RESULT_VARIABLE _explain_rc
        OUTPUT_VARIABLE _explain_out
        ERROR_VARIABLE _explain_err)
if (NOT _explain_rc EQUAL 0)
    message(FATAL_ERROR "zanna explain V-ZIA-UNDEFINED should exit 0, got ${_explain_rc}:\n${_explain_err}")
endif ()
if (NOT _explain_out MATCHES "zia-sema")
    message(FATAL_ERROR "zanna explain output missing subsystem:\n${_explain_out}")
endif ()

# ===== zanna explain --json =====
execute_process(
        COMMAND "${ZANNA_EXE}" explain W001 --json
        RESULT_VARIABLE _explain_json_rc
        OUTPUT_VARIABLE _explain_json_out
        ERROR_VARIABLE _explain_json_err)
if (NOT _explain_json_rc EQUAL 0 OR NOT _explain_json_out MATCHES "\"code\":\"W001\"")
    message(FATAL_ERROR "zanna explain W001 --json malformed:\n${_explain_json_out}")
endif ()

# ===== zanna explain: uncataloged code with known prefix still resolves =====
execute_process(
        COMMAND "${ZANNA_EXE}" explain B2113
        RESULT_VARIABLE _explain_family_rc
        OUTPUT_VARIABLE _explain_family_out
        ERROR_VARIABLE _explain_family_err)
if (NOT _explain_family_rc EQUAL 0)
    message(FATAL_ERROR "zanna explain on an uncataloged BASIC code should fall back to its family, got ${_explain_family_rc}")
endif ()

# ===== zanna explain: unknown code exits 1 =====
execute_process(
        COMMAND "${ZANNA_EXE}" explain TOTALLY-BOGUS
        RESULT_VARIABLE _explain_unknown_rc
        OUTPUT_VARIABLE _explain_unknown_out
        ERROR_VARIABLE _explain_unknown_err)
if (_explain_unknown_rc EQUAL 0)
    message(FATAL_ERROR "zanna explain on an unknown code should exit non-zero")
endif ()

# ===== --print-error-codes --json =====
execute_process(
        COMMAND "${ZANNA_EXE}" --print-error-codes --json
        RESULT_VARIABLE _codes_rc
        OUTPUT_VARIABLE _codes_out
        ERROR_VARIABLE _codes_err)
if (NOT _codes_rc EQUAL 0)
    message(FATAL_ERROR "--print-error-codes --json should exit 0, got ${_codes_rc}")
endif ()
if (NOT _codes_out MATCHES "\"code\":\"V-ZIA-TYPE-MISMATCH\"" OR NOT _codes_out MATCHES "\"code\":\"B2001\"")
    message(FATAL_ERROR "--print-error-codes catalog is missing expected entries:\n${_codes_out}")
endif ()

# ===== --dump-runtime-api =====
execute_process(
        COMMAND "${ZANNA_EXE}" --dump-runtime-api
        RESULT_VARIABLE _api_rc
        OUTPUT_VARIABLE _api_out
        ERROR_VARIABLE _api_err)

# A function row of the API dump, so per-function contract checks match inside
# one JSON object instead of backtracking `[^\n]*` across the whole single-line
# document (which took CMake's regex engine hours once the tail of the dump
# changed, ADR 0314). Sets ${out} to the substring from `"name":"<name>"` up to
# the next row.
function(zanna_api_row name out)
    string(FIND "${_api_out}" "\"name\":\"${name}\"" _row_start)
    if (_row_start LESS 0)
        set(${out} "" PARENT_SCOPE)
        return()
    endif ()
    string(SUBSTRING "${_api_out}" ${_row_start} -1 _row_tail)
    string(FIND "${_row_tail}" "{\"name\":\"" _row_end)
    if (_row_end LESS 0)
        set(${out} "${_row_tail}" PARENT_SCOPE)
    else ()
        string(SUBSTRING "${_row_tail}" 0 ${_row_end} _row_only)
        set(${out} "${_row_only}" PARENT_SCOPE)
    endif ()
endfunction()
if (NOT _api_rc EQUAL 0)
    message(FATAL_ERROR "--dump-runtime-api should exit 0, got ${_api_rc}")
endif ()
if (NOT _api_out MATCHES "\"functions\":" OR NOT _api_out MATCHES "\"classes\":")
    message(FATAL_ERROR "--dump-runtime-api missing top-level sections")
endif ()
if (NOT _api_out MATCHES "\"schema_version\":4" OR
    NOT _api_out MATCHES "\"signature_dialect\":\"runtime-def-v1\"" OR
    NOT _api_out MATCHES "\"public_boundary\":\"registry\"" OR
    NOT _api_out MATCHES "\"c_abi_status\":\"internal-embedding\"")
    message(FATAL_ERROR "--dump-runtime-api missing schema metadata")
endif ()
if (NOT _api_out MATCHES "\"documentation\":\\{\"summary\":" OR
    NOT _api_out MATCHES "\"format\":\"markdown\"")
    message(FATAL_ERROR "--dump-runtime-api missing authored class documentation")
endif ()
if (NOT _api_out MATCHES "Provides immutable runtime string values" OR
    NOT _api_out MATCHES "`Zanna.String`")
    message(FATAL_ERROR "--dump-runtime-api did not preserve authored String documentation")
endif ()
if (NOT _api_out MATCHES "\"fallibility\":" OR NOT _api_out MATCHES "\"class_kind\":")
    message(FATAL_ERROR "--dump-runtime-api missing production contract metadata")
endif ()
# VDOC-198: IO conversion/allocation entries carry accurate trapping + owned
# contracts (previously mislabeled infallible / unknown ownership).
zanna_api_row("Zanna.IO.Stream.AsBinFile" _api_row_1)
if (NOT _api_row_1 MATCHES
        "\"name\":\"Zanna.IO.Stream.AsBinFile\"[^\n]*\"fallibility\":\"traps\"[^\n]*\"ownership\":\"owned\"")
    message(FATAL_ERROR "--dump-runtime-api Stream.AsBinFile contract regressed (expected traps/owned)")
endif ()
zanna_api_row("Zanna.IO.LineWriter.Append" _api_row_2)
if (NOT _api_row_2 MATCHES
        "\"name\":\"Zanna.IO.LineWriter.Append\"[^\n]*\"fallibility\":\"traps\"[^\n]*\"ownership\":\"owned\"")
    message(FATAL_ERROR "--dump-runtime-api LineWriter.Append contract regressed (expected traps/owned)")
endif ()
# VDOC-209: trapping/allocating Math entries carry accurate traps + owned
# contracts (were mislabeled infallible / unknown ownership).
zanna_api_row("Zanna.Math.Mat4.Inverse" _api_row_3)
if (NOT _api_row_3 MATCHES
        "\"name\":\"Zanna.Math.Mat4.Inverse\"[^\n]*\"fallibility\":\"traps\"[^\n]*\"ownership\":\"owned\"")
    message(FATAL_ERROR "--dump-runtime-api Mat4.Inverse contract regressed (expected traps/owned)")
endif ()
zanna_api_row("Zanna.Math.BigInt.Div" _api_row_4)
if (NOT _api_row_4 MATCHES
        "\"name\":\"Zanna.Math.BigInt.Div\"[^\n]*\"fallibility\":\"traps\"[^\n]*\"ownership\":\"owned\"")
    message(FATAL_ERROR "--dump-runtime-api BigInt.Div contract regressed (expected traps/owned)")
endif ()
# VDOC-222: System entries surface their hidden nulls, traps, and statuses.
# Process spawns return NULL on failure (nullable return, not a live handle),
# Unsafe releases trap on an invalid object, and PtySession.Resize returns a
# boolean status rather than an infallible void.
zanna_api_row("Zanna.System.Process.Start" _api_row_5)
if (NOT _api_row_5 MATCHES
        "\"name\":\"Zanna.System.Process.Start\"[^\n]*\"nullable\":true[^\n]*\"fallibility\":\"infallible\"[^\n]*\"ownership\":\"owned\"")
    message(FATAL_ERROR "--dump-runtime-api Process.Start contract regressed (expected nullable return / owned)")
endif ()
zanna_api_row("Zanna.Runtime.Unsafe.Release" _api_row_6)
if (NOT _api_row_6 MATCHES
        "\"name\":\"Zanna.Runtime.Unsafe.Release\"[^\n]*\"fallibility\":\"traps\"")
    message(FATAL_ERROR "--dump-runtime-api Unsafe.Release contract regressed (expected traps)")
endif ()
zanna_api_row("Zanna.System.Pty.PtySession.Resize" _api_row_7)
if (NOT _api_row_7 MATCHES
        "\"name\":\"Zanna.System.Pty.PtySession.Resize\"[^\n]*\"fallibility\":\"status\"")
    message(FATAL_ERROR "--dump-runtime-api PtySession.Resize contract regressed (expected status)")
endif ()
# VDOC-228: TimeZone.Find carries its concrete class return type so Zia can chain
# instance members, and stays "borrowed" because the handle is static, must-not-free
# data (not an owned allocation).
zanna_api_row("Zanna.Time.TimeZone.Find" _api_row_8)
if (NOT _api_row_8 MATCHES
        "\"name\":\"Zanna.Time.TimeZone.Find\"[^\n]*\"class\":\"Zanna.Time.TimeZone\"[^\n]*\"fallibility\":\"traps\"[^\n]*\"ownership\":\"borrowed\"")
    message(FATAL_ERROR "--dump-runtime-api TimeZone.Find contract regressed (expected typed handle / traps / borrowed)")
endif ()
# VDOC-232: Time metadata surfaces sentinel parsers, nullable object returns, and
# owned fresh allocations. ParseIso8601 signals failure with a sentinel (not a
# trap); DateOnly.FromParts / DateRange.Intersection return NULL on ordinary
# failure and own their fresh result; Stopwatch.StartNew is an owned allocation.
zanna_api_row("Zanna.Time.DateTime.ParseIso8601" _api_row_9)
if (NOT _api_row_9 MATCHES
        "\"name\":\"Zanna.Time.DateTime.ParseIso8601\"[^\n]*\"fallibility\":\"sentinel\"")
    message(FATAL_ERROR "--dump-runtime-api DateTime.ParseIso8601 contract regressed (expected sentinel)")
endif ()
zanna_api_row("Zanna.Time.DateOnly.FromParts" _api_row_10)
if (NOT _api_row_10 MATCHES
        "\"name\":\"Zanna.Time.DateOnly.FromParts\"[^\n]*\"nullable\":true[^\n]*\"ownership\":\"owned\"")
    message(FATAL_ERROR "--dump-runtime-api DateOnly.FromParts contract regressed (expected nullable / owned)")
endif ()
zanna_api_row("Zanna.Time.DateRange.Intersection" _api_row_11)
if (NOT _api_row_11 MATCHES
        "\"name\":\"Zanna.Time.DateRange.Intersection\"[^\n]*\"nullable\":true[^\n]*\"ownership\":\"owned\"")
    message(FATAL_ERROR "--dump-runtime-api DateRange.Intersection contract regressed (expected nullable / owned)")
endif ()
zanna_api_row("Zanna.Time.Stopwatch.StartNew" _api_row_12)
if (NOT _api_row_12 MATCHES
        "\"name\":\"Zanna.Time.Stopwatch.StartNew\"[^\n]*\"ownership\":\"owned\"")
    message(FATAL_ERROR "--dump-runtime-api Stopwatch.StartNew contract regressed (expected owned)")
endif ()
zanna_api_row("Zanna.Graphics3D.Canvas3D.TryCopyScreenshotTo" _api_row_13)
if (NOT _api_row_13 MATCHES
        "\"name\":\"Zanna.Graphics3D.Canvas3D.TryCopyScreenshotTo\"[^\n]*\"c_symbol\":\"rt_canvas3d_try_copy_screenshot_to\"[^\n]*\"fallibility\":\"status\"[^\n]*\"ownership\":\"value\"[^\n]*\"contract_source\":\"three-d-boundary-policy\"")
    message(FATAL_ERROR "--dump-runtime-api missing the allocation-reusing Canvas3D status contract")
endif ()
zanna_api_row("Zanna.GUI.App.New" _api_row_14)
if (NOT _api_row_14 MATCHES
        "\"name\":\"Zanna.GUI.App.New\"[^\n]*\"c_symbol\":\"rt_gui_app_new\"[^\n]*\"nullable\":true[^\n]*\"fallibility\":\"nullable\"[^\n]*\"ownership\":\"owned\"[^\n]*\"contract_source\":\"gui-boundary-policy\"")
    message(FATAL_ERROR "--dump-runtime-api GUI App.New contract regressed")
endif ()
zanna_api_row("Zanna.GUI.App.TryNew" _api_row_15)
if (NOT _api_row_15 MATCHES
        "\"name\":\"Zanna.GUI.App.TryNew\"[^\n]*\"class\":\"Zanna.Result\"[^\n]*\"nullable\":false[^\n]*\"fallibility\":\"result\"[^\n]*\"ownership\":\"owned\"[^\n]*\"contract_source\":\"gui-boundary-policy\"")
    message(FATAL_ERROR "--dump-runtime-api GUI App.TryNew Result contract regressed")
endif ()
zanna_api_row("Zanna.GUI.App.get_Root" _api_row_16)
if (NOT _api_row_16 MATCHES
        "\"name\":\"Zanna.GUI.App.get_Root\"[^\n]*\"class\":\"Zanna.GUI.Widget\"[^\n]*\"nullable\":true[^\n]*\"fallibility\":\"nullable\"[^\n]*\"ownership\":\"borrowed\"[^\n]*\"contract_source\":\"gui-boundary-policy\"")
    message(FATAL_ERROR "--dump-runtime-api GUI App.Root borrowed contract regressed")
endif ()
zanna_api_row("Zanna.GUI.App.RunFrameWithDelta" _api_row_17)
if (NOT _api_row_17 MATCHES
        "\"name\":\"Zanna.GUI.App.RunFrameWithDelta\"[^\n]*\"c_symbol\":\"rt_gui_app_run_frame_with_delta\"[^\n]*\"return_type\":\{\"raw\":\"i1\"[^\n]*\"ownership\":\"value\"[^\n]*\"contract_source\":\"gui-boundary-policy\"")
    message(FATAL_ERROR "--dump-runtime-api GUI deterministic frame contract regressed")
endif ()
if (NOT _api_out MATCHES
        "\"name\":\"GetNextDeadlineMs\"[^\n]*\"target\":\"Zanna.GUI.App.GetNextDeadlineMs\"[^\n]*\"c_symbol\":\"rt_gui_app_get_next_deadline_ms\"")
    message(FATAL_ERROR "--dump-runtime-api GUI deadline method binding regressed")
endif ()
zanna_api_row("Zanna.GUI.CodeEditor.TakeGutterClick" _api_row_19)
if (NOT _api_row_19 MATCHES
        "\"name\":\"Zanna.GUI.CodeEditor.TakeGutterClick\"[^\n]*\"class\":\"Zanna.Collections.Map\"[^\n]*\"nullable\":false[^\n]*\"ownership\":\"owned\"[^\n]*\"contract_source\":\"gui-boundary-policy\"")
    message(FATAL_ERROR "--dump-runtime-api GUI TakeGutterClick typed-map contract regressed")
endif ()
zanna_api_row("Zanna.GUI.Theme.GetPalette" _api_row_20)
if (NOT _api_row_20 MATCHES
        "\"name\":\"Zanna.GUI.Theme.GetPalette\"[^\n]*\"nullable\":true[^\n]*\"fallibility\":\"nullable\"[^\n]*\"ownership\":\"owned\"[^\n]*\"contract_source\":\"gui-boundary-policy\"")
    message(FATAL_ERROR "--dump-runtime-api GUI Theme.GetPalette ownership contract regressed")
endif ()
zanna_api_row("Zanna.GUI.ThemePalette.FromDark" _api_row_21)
if (NOT _api_row_21 MATCHES
        "\"name\":\"Zanna.GUI.ThemePalette.FromDark\"[^\n]*\"nullable\":true[^\n]*\"fallibility\":\"nullable\"[^\n]*\"ownership\":\"owned\"[^\n]*\"contract_source\":\"gui-boundary-policy\"")
    message(FATAL_ERROR "--dump-runtime-api GUI ThemePalette factory ownership contract regressed")
endif ()
zanna_api_row("Zanna.GUI.ThemePalette.Validate" _api_row_22)
if (NOT _api_row_22 MATCHES
        "\"name\":\"Zanna.GUI.ThemePalette.Validate\"[^\n]*\"class\":\"Zanna.Result\"[^\n]*\"nullable\":false[^\n]*\"fallibility\":\"result\"[^\n]*\"ownership\":\"owned\"[^\n]*\"contract_source\":\"gui-boundary-policy\"")
    message(FATAL_ERROR "--dump-runtime-api GUI ThemePalette.Validate Result contract regressed")
endif ()
zanna_api_row("Zanna.GUI.TreeView.GetLoadRequestedNodeOption" _api_row_23)
if (NOT _api_row_23 MATCHES
        "\"name\":\"Zanna.GUI.TreeView.GetLoadRequestedNodeOption\"[^\n]*\"class\":\"Zanna.Option\"[^\n]*\"nullable\":false[^\n]*\"fallibility\":\"option\"[^\n]*\"ownership\":\"owned\"[^\n]*\"contract_source\":\"gui-boundary-policy\"")
    message(FATAL_ERROR "--dump-runtime-api TreeView lazy-load Option contract regressed")
endif ()
zanna_api_row("Zanna.GUI.TreeView.GetActivatedNodeOption" _api_row_24)
if (NOT _api_row_24 MATCHES
        "\"name\":\"Zanna.GUI.TreeView.GetActivatedNodeOption\"[^\n]*\"class\":\"Zanna.Option\"[^\n]*\"nullable\":false[^\n]*\"fallibility\":\"option\"[^\n]*\"ownership\":\"owned\"[^\n]*\"contract_source\":\"gui-boundary-policy\"")
    message(FATAL_ERROR "--dump-runtime-api TreeView activation Option contract regressed")
endif ()
if (NOT _api_out MATCHES
        "\"name\":\"MoveTab\"[^\n]*\"target\":\"Zanna.GUI.TabBar.MoveTab\"[^\n]*\"c_symbol\":\"rt_tabbar_move_tab\"")
    message(FATAL_ERROR "--dump-runtime-api TabBar reorder method binding regressed")
endif ()
if (NOT _api_out MATCHES
        "\"name\":\"GetCollapsedSide\"[^\n]*\"target\":\"Zanna.GUI.SplitPane.GetCollapsedSide\"[^\n]*\"c_symbol\":\"rt_splitpane_get_collapsed_side\"")
    message(FATAL_ERROR "--dump-runtime-api SplitPane collapse method binding regressed")
endif ()
if (NOT _api_out MATCHES
        "\"name\":\"SetSelectedIndex\"[^\n]*\"target\":\"Zanna.GUI.RadioGroup.SetSelectedIndex\"[^\n]*\"c_symbol\":\"rt_radiogroup_set_selected_index\"")
    message(FATAL_ERROR "--dump-runtime-api RadioGroup selected-index method binding regressed")
endif ()
if (NOT _api_out MATCHES
        "\"name\":\"GetSelectedText\"[^\n]*\"target\":\"Zanna.GUI.Label.GetSelectedText\"[^\n]*\"c_symbol\":\"rt_label_get_selected_text\"")
    message(FATAL_ERROR "--dump-runtime-api selectable Label method binding regressed")
endif ()
if (NOT _api_out MATCHES
        "\"name\":\"GetColorAt\"[^\n]*\"target\":\"Zanna.GUI.ColorPalette.GetColorAt\"[^\n]*\"c_symbol\":\"rt_colorpalette_get_color_at\"")
    message(FATAL_ERROR "--dump-runtime-api ColorPalette indexed-color method binding regressed")
endif ()
if (NOT _api_out MATCHES
        "\"name\":\"SetAlphaEnabled\"[^\n]*\"target\":\"Zanna.GUI.ColorPicker.SetAlphaEnabled\"[^\n]*\"c_symbol\":\"rt_colorpicker_set_alpha_enabled\"")
    message(FATAL_ERROR "--dump-runtime-api ColorPicker alpha method binding regressed")
endif ()
if (NOT _api_out MATCHES
        "\"name\":\"SetVirtualCell\"[^\n]*\"target\":\"Zanna.GUI.Grid.SetVirtualCell\"[^\n]*\"c_symbol\":\"rt_datagrid_set_virtual_cell\"")
    message(FATAL_ERROR "--dump-runtime-api Grid sparse-cell method binding regressed")
endif ()
if (NOT _api_out MATCHES
        "\"name\":\"CommitEdit\"[^\n]*\"target\":\"Zanna.GUI.Grid.CommitEdit\"[^\n]*\"c_symbol\":\"rt_datagrid_commit_edit\"")
    message(FATAL_ERROR "--dump-runtime-api Grid edit method binding regressed")
endif ()
zanna_api_row("Zanna.Graphics3D.Canvas3D.Screenshot" _api_row_33)
if (NOT _api_row_33 MATCHES
        "\"name\":\"Zanna.Graphics3D.Canvas3D.Screenshot\"[^\n]*\"c_symbol\":\"rt_canvas3d_screenshot\"[^\n]*\"nullable\":true[^\n]*\"fallibility\":\"nullable\"[^\n]*\"ownership\":\"owned\"")
    message(FATAL_ERROR "--dump-runtime-api missing the Canvas3D screenshot ownership contract")
endif ()
if (NOT _api_out MATCHES
        "\"name\":\"TryCopyScreenshotTo\"[^\n]*\"target\":\"Zanna.Graphics3D.Canvas3D.TryCopyScreenshotTo\"[^\n]*\"c_symbol\":\"rt_canvas3d_try_copy_screenshot_to\"")
    message(FATAL_ERROR "--dump-runtime-api class methods must resolve to backing C symbols")
endif ()
if (NOT _api_out MATCHES "Zanna.Terminal.Say")
    message(FATAL_ERROR "--dump-runtime-api missing canonical function entries")
endif ()
if (NOT _api_out MATCHES "Zanna.Diagnostics.CurrentTrap" OR
    NOT _api_out MATCHES "Zanna.Diagnostics.TrapInfo")
    message(FATAL_ERROR "--dump-runtime-api missing diagnostics trap snapshot entries")
endif ()
if (NOT _api_out MATCHES "Zanna.Runtime.Unsafe.Retain" OR
    NOT _api_out MATCHES "Zanna.Runtime.Unsafe.SetThrowMsg" OR
    NOT _api_out MATCHES "Zanna.Runtime.Unsafe.SetTrapFields" OR
    NOT _api_out MATCHES "Zanna.Runtime.GC.Collect")
    message(FATAL_ERROR "--dump-runtime-api missing runtime namespace memory entries")
endif ()
if (NOT _api_out MATCHES "Zanna.Core.Parse.TryDouble" OR
    NOT _api_out MATCHES "Zanna.Core.Parse.DoubleOr")
    message(FATAL_ERROR "--dump-runtime-api missing canonical parse double entries")
endif ()
if (NOT _api_out MATCHES "Zanna.Core.Convert.ToStringInt" OR
    NOT _api_out MATCHES "Zanna.Core.Convert.ToStringDouble")
    message(FATAL_ERROR "--dump-runtime-api missing canonical convert string entries")
endif ()
if (NOT _api_out MATCHES "Zanna.Collections.Seq.get_Capacity" OR
    NOT _api_out MATCHES "Zanna.Threads.Channel.get_Capacity" OR
    NOT _api_out MATCHES "Zanna.IO.BinaryBuffer.NewCapacity")
    message(FATAL_ERROR "--dump-runtime-api missing canonical capacity entries")
endif ()
if (NOT _api_out MATCHES "Zanna.Collections.BloomFilter.FalsePositiveRate")
    message(FATAL_ERROR "--dump-runtime-api missing canonical BloomFilter rate entry")
endif ()
if (NOT _api_out MATCHES "Zanna.Text.Fmt.Scientific" OR
    NOT _api_out MATCHES "Zanna.Text.Fmt.Percent" OR
    NOT _api_out MATCHES "Zanna.Text.Fmt.YesNo")
    message(FATAL_ERROR "--dump-runtime-api missing canonical formatting entries")
endif ()
if (NOT _api_out MATCHES "Zanna.Collections.LruCache.Set" OR
    NOT _api_out MATCHES "Zanna.Collections.BiMap.Set" OR
    NOT _api_out MATCHES "Zanna.Collections.MultiMap.Add")
    message(FATAL_ERROR "--dump-runtime-api missing canonical collection write entries")
endif ()
if (NOT _api_out MATCHES "Zanna.Graphics3D.Mesh3D.Box" OR
    NOT _api_out MATCHES "Zanna.Graphics3D.Material3D.FromColor" OR
    NOT _api_out MATCHES "Zanna.Graphics3D.Light3D.Directional" OR
    NOT _api_out MATCHES "Zanna.Graphics3D.Collider3D.Capsule" OR
    NOT _api_out MATCHES "Zanna.Game3D.World3D.WithHorizontalCamera")
    message(FATAL_ERROR "--dump-runtime-api missing canonical factory entries")
endif ()
if (NOT _api_out MATCHES "Zanna.Collections.Queue.TryPop" OR
    NOT _api_out MATCHES "Zanna.Collections.Heap.TryPeek" OR
    NOT _api_out MATCHES "Zanna.Collections.Deque.TryPopBack" OR
    NOT _api_out MATCHES "Zanna.Threads.Promise.GetFuture.*obj<Zanna.Threads.Future>" OR
    NOT _api_out MATCHES "Zanna.Threads.Async.Delay.*obj<Zanna.Threads.Future>" OR
    NOT _api_out MATCHES "Zanna.Threads.Channel.TryRecv" OR
    NOT _api_out MATCHES "Zanna.Threads.Future.TryGet" OR
    NOT _api_out MATCHES "Zanna.Time.DateTime.TryParse" OR
    NOT _api_out MATCHES "Zanna.Localization.MessageBundle.TryGet" OR
    NOT _api_out MATCHES "Zanna.Localization.MessageBundle.GetOr" OR
    NOT _api_out MATCHES "Zanna.Localization.Locale.TryParse" OR
    NOT _api_out MATCHES "Zanna.Collections.Bytes.Find" OR
    NOT _api_out MATCHES "Zanna.Collections.UnionFind.FindRoot" OR
    NOT _api_out MATCHES "Zanna.Collections.Seq.FindWhereOption" OR
    NOT _api_out MATCHES "Zanna.Text.Pattern.Find" OR
    NOT _api_out MATCHES "Zanna.Data.Xml.Find" OR
    NOT _api_out MATCHES "Zanna.Graphics2D.SceneGraph.Find" OR
    NOT _api_out MATCHES "Zanna.Audio.Mixer.FindGroup" OR
    NOT _api_out MATCHES "Zanna.Graphics3D.SceneGraph.Find" OR
    NOT _api_out MATCHES "Zanna.Graphics3D.SceneNode.Find" OR
    NOT _api_out MATCHES "Zanna.Graphics3D.Skeleton3D.FindBoneOption" OR
    NOT _api_out MATCHES "Zanna.Graphics3D.SceneAsset.FindNode" OR
    NOT _api_out MATCHES "Zanna.Game3D.World3D.FindNode" OR
    NOT _api_out MATCHES "Zanna.Game3D.World3D.FindEntity" OR
    NOT _api_out MATCHES "Zanna.Graphics3D.NavMesh3D.FindPathOption" OR
    NOT _api_out MATCHES "Zanna.GUI.FindBar.FindNext" OR
    NOT _api_out MATCHES "Zanna.GUI.TestHarness.FindById" OR
    NOT _api_out MATCHES "Zanna.GUI.CommandRegistry.Find" OR
    NOT _api_out MATCHES "Zanna.Game.UI.HudTableClickResult.RowOption" OR
    NOT _api_out MATCHES "Zanna.Game.UI.HudTableClickResult.ColumnOption")
    message(FATAL_ERROR "--dump-runtime-api missing modern Option-returning failure entries")
endif ()
if (NOT _api_out MATCHES "Zanna.Graphics.Canvas.SetMaxDeltaTime" OR
    NOT _api_out MATCHES "Zanna.Graphics3D.Canvas3D.SetMaxDeltaTime")
    message(FATAL_ERROR "--dump-runtime-api missing canonical frame delta entries")
endif ()
if (NOT _api_out MATCHES "Zanna.Input.Key" OR
    NOT _api_out MATCHES "Zanna.Input.Key.get_A" OR
    NOT _api_out MATCHES "Zanna.Input.Key.get_LeftShift" OR
    NOT _api_out MATCHES "Zanna.Input.Key.get_NumpadDecimal")
    message(FATAL_ERROR "--dump-runtime-api missing canonical input key entries")
endif ()
zanna_api_row("Zanna.Math.Random.Chance" _api_row_35)
if (NOT _api_row_35 MATCHES "\"name\":\"Zanna.Math.Random.Chance\"[^\\n]*\"signature\":\"i1\\(f64\\)\"")
    message(FATAL_ERROR "--dump-runtime-api missing boolean Random.Chance row")
endif ()
if (NOT _api_out MATCHES "Zanna.Crypto.Compliance.EnableApprovedModeForProcess" OR
    NOT _api_out MATCHES "Zanna.Crypto.Compliance.DisableApprovedModeForProcess" OR
    NOT _api_out MATCHES "Zanna.Crypto.Compliance.IsApprovedModeForProcess")
    message(FATAL_ERROR "--dump-runtime-api missing process-scoped Crypto.Compliance policy rows")
endif ()
if (NOT _api_out MATCHES "Zanna.Runtime.Unsafe.ValueType" OR
    NOT _api_out MATCHES "Zanna.Runtime.Unsafe.ValueTypeAddField")
    message(FATAL_ERROR "--dump-runtime-api missing unsafe boxed value-type rows")
endif ()
if (NOT _api_out MATCHES "Zanna.Game3D.Prefab.Load" OR
    NOT _api_out MATCHES "Zanna.Game3D.Prefab.LoadAsset" OR
    NOT _api_out MATCHES "Zanna.Game3D.AssetHandle3D.GetPrefab")
    message(FATAL_ERROR "--dump-runtime-api missing canonical Game3D prefab loading rows")
endif ()
if (NOT _api_out MATCHES "Zanna.Network.HttpReq.SendResult" OR
    NOT _api_out MATCHES "Zanna.Network.RestClient.GetResult" OR
    NOT _api_out MATCHES "Zanna.Network.RestClient.PostResult" OR
    NOT _api_out MATCHES "Zanna.Network.SmtpClient.SendResult" OR
    NOT _api_out MATCHES "Zanna.Network.SmtpClient.SendHtmlResult" OR
    NOT _api_out MATCHES "Zanna.Network.RestClient.HeadResult")
    message(FATAL_ERROR "--dump-runtime-api missing network Result-returning HTTP rows")
endif ()
if (NOT _api_out MATCHES "Zanna.Data.JsonStream.NextResult" OR
    NOT _api_out MATCHES "Zanna.Data.Xml.ParseResult" OR
    NOT _api_out MATCHES "Zanna.Data.Yaml.ParseResult" OR
    NOT _api_out MATCHES "Zanna.Data.Serialize.ParseResult" OR
    NOT _api_out MATCHES "Zanna.Data.Serialize.AutoParseResult")
    message(FATAL_ERROR "--dump-runtime-api missing Result-returning data format parse rows")
endif ()
if (NOT _api_out MATCHES "Zanna.Crypto.Tls.ConnectResult" OR
    NOT _api_out MATCHES "Zanna.Crypto.Tls.ConnectForResult" OR
    NOT _api_out MATCHES "Zanna.Crypto.Tls.ConnectOptionsResult" OR
    NOT _api_out MATCHES "Zanna.System.Pty.OpenResult" OR
    NOT _api_out MATCHES "Zanna.Zia.SemanticJob.Error" OR
    NOT _api_out MATCHES "Zanna.Graphics3D.SceneAsset.LoadResult" OR
    NOT _api_out MATCHES "Zanna.Graphics3D.SceneAsset.LoadAssetResult" OR
    NOT _api_out MATCHES "Zanna.Graphics3D.SceneAsset.LoadAnimationResult" OR
    NOT _api_out MATCHES "Zanna.Graphics3D.SceneAsset.LoadAnimationAssetResult" OR
    NOT _api_out MATCHES "Zanna.Graphics3D.SceneAsset.LoadNodeAnimationResult" OR
    NOT _api_out MATCHES "Zanna.Graphics3D.SceneAsset.LoadNodeAnimationAssetResult" OR
    NOT _api_out MATCHES "Zanna.Game2D.SceneDocument.LoadResult" OR
    NOT _api_out MATCHES "Zanna.Game2D.SceneDocument.LoadJsonResult")
    message(FATAL_ERROR "--dump-runtime-api missing Result-returning connect/open/load rows")
endif ()
if (NOT _api_out MATCHES "Zanna.Crypto.Cipher.DecryptResult" OR
    NOT _api_out MATCHES "Zanna.Crypto.Cipher.TryDecryptWithKeyAad" OR
    NOT _api_out MATCHES "Zanna.Crypto.Aes.DecryptAuthResult" OR
    NOT _api_out MATCHES "Zanna.Crypto.Aes.TryDecryptStr" OR
    NOT _api_out MATCHES "Zanna.Network.HttpReq.AllowInsecureCertificatesForTesting" OR
    NOT _api_out MATCHES "Zanna.Crypto.Legacy.Hash.Md5" OR
    NOT _api_out MATCHES "Zanna.Crypto.Legacy.Aes.DecryptCbcResult")
    message(FATAL_ERROR "--dump-runtime-api missing modern crypto failure/legacy entries")
endif ()
if (NOT _api_out MATCHES "Zanna.Math.Bits.CountLeadingZeros" OR
    NOT _api_out MATCHES "Zanna.Math.Bits.RotateLeft" OR
    NOT _api_out MATCHES "Zanna.Math.Bits.ShiftRightLogical")
    message(FATAL_ERROR "--dump-runtime-api missing canonical math bit entries")
endif ()
if (NOT _api_out MATCHES "Zanna.Terminal.TryReadLine" OR
    NOT _api_out MATCHES "Zanna.Terminal.ReadLineResult")
    message(FATAL_ERROR "--dump-runtime-api missing modern terminal input entries")
endif ()
if (NOT _api_out MATCHES "Zanna.Game.Pathfinder.FindPath" OR
    NOT _api_out MATCHES "Zanna.Game.PathResult.get_Found" OR
    NOT _api_out MATCHES "Zanna.Game.PathResult.get_StepCount" OR
    NOT _api_out MATCHES "Zanna.Game.Quadtree.QueryRect" OR
    NOT _api_out MATCHES "Zanna.Game.QueryResult.GetId" OR
    NOT _api_out MATCHES "Zanna.Game.Quadtree.QueryPairs" OR
    NOT _api_out MATCHES "Zanna.Game.QuadtreePairResult.First" OR
    NOT _api_out MATCHES "Zanna.Game.UI.HudTable.HandleClick" OR
    NOT _api_out MATCHES "Zanna.Game.UI.HudTableClickResult.get_IsHeader" OR
    NOT _api_out MATCHES "Zanna.Game.AnimStateMachine.PollEvents" OR
    NOT _api_out MATCHES "Zanna.Game.AnimationEventBatch.GetId")
    message(FATAL_ERROR "--dump-runtime-api missing game result snapshot entries")
endif ()
if (_api_out MATCHES "str\\?")
    message(FATAL_ERROR "--dump-runtime-api should not expose undocumented nullable suffix signatures")
endif ()
if (NOT _api_out MATCHES "\"fallibility\":\"option\"" OR
    NOT _api_out MATCHES "\"fallibility\":\"result\"")
    message(FATAL_ERROR "--dump-runtime-api missing Option/Result fallibility metadata")
endif ()

# ===== --dump-opcodes =====
execute_process(
        COMMAND "${ZANNA_EXE}" --dump-opcodes
        RESULT_VARIABLE _ops_rc
        OUTPUT_VARIABLE _ops_out
        ERROR_VARIABLE _ops_err)
if (NOT _ops_rc EQUAL 0)
    message(FATAL_ERROR "--dump-opcodes should exit 0, got ${_ops_rc}")
endif ()
if (NOT _ops_out MATCHES "\"mnemonic\":\"add\"" OR NOT _ops_out MATCHES "\"opcodes\":")
    message(FATAL_ERROR "--dump-opcodes output malformed:\n${_ops_out}")
endif ()

message(STATUS "Agent CLI tests passed")
