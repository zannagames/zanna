//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/system/rt_pty_windows_environment.inc
// Purpose: Builds strict UTF-16 environment blocks for Windows ConPTY children.
// Key invariants:
//   - Environment names use case-insensitive Windows ordinal comparison.
//   - Returned blocks are sorted, double-NUL terminated, and caller-owned.
// Ownership/Lifetime:
//   - Input runtime sequences and strings are borrowed for each call.
//   - Every temporary and returned native allocation has one explicit owner.
// Links: src/runtime/system/rt_pty.c, docs/adr/0289-pty-environment-overlays.md
//
//===----------------------------------------------------------------------===//

/// @brief Invoke the dynamically resolved Windows ordinal comparator.
/// @details Uses case-insensitive CompareStringOrdinal when available and falls
///          back to exact wide-string equality otherwise.
/// @param left First UTF-16 string.
/// @param left_len First length, or -1 for NUL-terminated input.
/// @param right Second UTF-16 string.
/// @param right_len Second length, or -1 for NUL-terminated input.
/// @return CSTR_* ordering value, CSTR_EQUAL for equal fallback inputs, or zero
///         for unequal fallback inputs.
static int pty_compare_ordinal(const wchar_t *left,
                               int left_len,
                               const wchar_t *right,
                               int right_len) {
    pty_load_conpty();
    if (pty_compare_string_ordinal)
        return pty_compare_string_ordinal(left, left_len, right, right_len, TRUE);
    if (left_len < 0 && right_len < 0)
        return wcscmp(left, right) == 0 ? CSTR_EQUAL : 0;
    return wcsncmp(left, right, (size_t)left_len) == 0 ? CSTR_EQUAL : 0;
}

/// @brief Compare complete UTF-16 environment entries using Win32 ordinal rules.
/// @details Applies case-insensitive ordinal order first and binary wide-string
///          order as a deterministic tie breaker.
/// @param left Pointer to the first wchar_t pointer.
/// @param right Pointer to the second wchar_t pointer.
/// @return Negative, zero, or positive for qsort ordering.
static int pty_compare_env_entry_wide(const void *left, const void *right) {
    const wchar_t *lhs = *(const wchar_t *const *)left;
    const wchar_t *rhs = *(const wchar_t *const *)right;
    int result = pty_compare_ordinal(lhs, -1, rhs, -1);
    if (result == CSTR_LESS_THAN)
        return -1;
    if (result == CSTR_GREATER_THAN)
        return 1;
    return wcscmp(lhs, rhs);
}

/// @brief Test whether two UTF-16 environment entries name the same variable.
/// @param lhs First NUL-terminated NAME=VALUE entry.
/// @param rhs Second NUL-terminated NAME=VALUE entry.
/// @return 1 when the nonempty names compare case-insensitively equal, otherwise 0.
static int pty_env_names_equal_wide(const wchar_t *lhs, const wchar_t *rhs) {
    size_t lhs_len = 0;
    size_t rhs_len = 0;
    while (lhs[lhs_len] && lhs[lhs_len] != L'=')
        lhs_len++;
    while (rhs[rhs_len] && rhs[rhs_len] != L'=')
        rhs_len++;
    if (lhs_len == 0 || lhs_len != rhs_len || lhs_len > INT_MAX)
        return 0;
    return pty_compare_ordinal(lhs, (int)lhs_len, rhs, (int)rhs_len) == CSTR_EQUAL;
}

/// @brief Build a strict, sorted CreateProcessW environment block.
/// @details Converts every validated NAME=VALUE entry independently, sorts
///          case-insensitively, rejects duplicate names, and emits the required
///          double-NUL terminator. A non-NULL empty sequence creates an explicit
///          empty environment block.
/// @param env Runtime environment sequence, or NULL to inherit.
/// @return Caller-owned UTF-16 block, or NULL for inheritance or on conversion,
///         duplicate-name, overflow, or allocation failure.
static wchar_t *pty_build_env_block_wide(void *env) {
    int64_t count;
    size_t total_wchars = 1;
    wchar_t **entries = NULL;
    wchar_t *block = NULL;

    if (!env)
        return NULL;
    count = rt_seq_len(env);
    if (count < 0 || (uint64_t)count > SIZE_MAX / sizeof(*entries))
        return NULL;
    if (count > 0) {
        entries = (wchar_t **)calloc((size_t)count, sizeof(*entries));
        if (!entries)
            return NULL;
    }
    for (int64_t i = 0; i < count; i++) {
        rt_string item = rt_seq_get_str(env, i);
        entries[i] = pty_widen(item ? rt_string_cstr(item) : NULL);
        rt_str_release_maybe(item);
        if (!entries[i])
            goto fail;
        size_t length = wcslen(entries[i]);
        if (length == SIZE_MAX || total_wchars > SIZE_MAX - length - 1)
            goto fail;
        total_wchars += length + 1;
    }
    if (count > 1) {
        qsort(entries, (size_t)count, sizeof(*entries), pty_compare_env_entry_wide);
        for (int64_t i = 1; i < count; i++) {
            if (pty_env_names_equal_wide(entries[i - 1], entries[i]))
                goto fail;
        }
    }
    if (total_wchars == SIZE_MAX || total_wchars + 1 > SIZE_MAX / sizeof(*block))
        goto fail;
    block = (wchar_t *)calloc(total_wchars + 1, sizeof(*block));
    if (!block)
        goto fail;
    wchar_t *out = block;
    for (int64_t i = 0; i < count; i++) {
        size_t length = wcslen(entries[i]);
        memcpy(out, entries[i], length * sizeof(*out));
        out += length + 1;
        free(entries[i]);
    }
    free(entries);
    return block;

fail:
    if (entries) {
        for (int64_t i = 0; i < count; i++)
            free(entries[i]);
    }
    free(entries);
    return NULL;
}

/// @brief Copy one inherited UTF-16 environment entry.
static wchar_t *pty_wide_string_dup(const wchar_t *text) {
    size_t len = text ? wcslen(text) : SIZE_MAX;
    if (len == SIZE_MAX || len + 1 > SIZE_MAX / sizeof(wchar_t))
        return NULL;
    wchar_t *copy = (wchar_t *)malloc((len + 1) * sizeof(wchar_t));
    if (copy)
        memcpy(copy, text, (len + 1) * sizeof(wchar_t));
    return copy;
}

/// @brief Build a sorted environment block from inherited entries plus overrides.
static wchar_t *pty_build_env_overlay_block_wide(void *env) {
    if (!env)
        return NULL;
    wchar_t *overlay = pty_build_env_block_wide(env);
    LPWCH inherited = overlay ? GetEnvironmentStringsW() : NULL;
    if (!overlay || !inherited) {
        free(overlay);
        return NULL;
    }
    size_t inherited_count = 0, overlay_count = 0;
    for (const wchar_t *entry = inherited; *entry; entry += wcslen(entry) + 1)
        inherited_count++;
    for (const wchar_t *entry = overlay; *entry; entry += wcslen(entry) + 1)
        overlay_count++;
    if (inherited_count > SIZE_MAX - overlay_count ||
        inherited_count + overlay_count > SIZE_MAX / sizeof(wchar_t *))
        goto fail;
    size_t capacity = inherited_count + overlay_count;
    wchar_t **entries = capacity ? (wchar_t **)calloc(capacity, sizeof(*entries)) : NULL;
    if (capacity && !entries)
        goto fail;
    size_t count = 0;
    for (const wchar_t *entry = inherited; *entry; entry += wcslen(entry) + 1) {
        int shadowed = 0;
        if (*entry != L'=') {
            for (const wchar_t *candidate = overlay; *candidate;
                 candidate += wcslen(candidate) + 1) {
                if (pty_env_names_equal_wide(entry, candidate)) {
                    shadowed = 1;
                    break;
                }
            }
        }
        if (!shadowed && !(entries[count++] = pty_wide_string_dup(entry)))
            goto entries_fail;
    }
    for (const wchar_t *entry = overlay; *entry; entry += wcslen(entry) + 1) {
        if (!(entries[count++] = pty_wide_string_dup(entry)))
            goto entries_fail;
    }
    if (count > 1)
        qsort(entries, count, sizeof(*entries), pty_compare_env_entry_wide);
    size_t total = 1;
    for (size_t i = 0; i < count; i++) {
        size_t len = wcslen(entries[i]);
        if (len == SIZE_MAX || total > SIZE_MAX - len - 1)
            goto entries_fail;
        total += len + 1;
    }
    wchar_t *block =
        total < SIZE_MAX / sizeof(wchar_t) ? (wchar_t *)calloc(total + 1, sizeof(wchar_t)) : NULL;
    if (!block)
        goto entries_fail;
    wchar_t *out = block;
    for (size_t i = 0; i < count; i++) {
        size_t len = wcslen(entries[i]);
        memcpy(out, entries[i], len * sizeof(wchar_t));
        out += len + 1;
        free(entries[i]);
    }
    free(entries);
    FreeEnvironmentStringsW(inherited);
    free(overlay);
    return block;

entries_fail:
    for (size_t i = 0; i < count; i++)
        free(entries[i]);
    free(entries);
fail:
    FreeEnvironmentStringsW(inherited);
    free(overlay);
    return NULL;
}
