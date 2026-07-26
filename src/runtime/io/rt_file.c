//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE in the project root for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/io/rt_file.c
// Purpose: Maintains the BASIC runtime's channel table that maps integer channel
//          IDs to RtFile handles. Exposes the legacy file I/O ABI (open, close,
//          read, write, EOF check) translating results into Err_* error codes.
//
// Key invariants:
//   - Channel identifiers map 1:1 to entries in a growable table; no two
//     channels share an entry.
//   - EOF state is cached eagerly on each read to replicate VM interpreter
//     behaviour.
//   - All failures are reported as Err_* enumerators; errno never escapes.
//   - Table growth doubles capacity to amortise allocations while keeping
//     existing channel handles stable.
//   - Channel 0 is reserved; valid user channels start at 1.
//
// Ownership/Lifetime:
//   - The channel table is owned by the RtContext; rt_file_state_cleanup frees
//     all open handles when the context is torn down.
//   - RtFile handles stored in the table are not reference-counted; the table
//     is their sole owner.
//
// Links: src/runtime/io/rt_file.h (public API and RtFile type),
//        src/runtime/io/rt_file_io.c (low-level read/write primitives),
//        src/runtime/io/rt_file_path.h (mode string helpers)
//
//===----------------------------------------------------------------------===//

#include "rt_file.h"

#include "rt_file_path.h"

#include "rt_context.h"
#include "rt_context_internal.h"
#include "rt_internal.h"
#include <stdbool.h>
#include <stdlib.h>

/// @brief Per-channel bookkeeping entry stored in the context's file table.
typedef struct RtFileChannelEntry {
    int32_t channel; ///< User-visible channel number (1-based; 0 is reserved).
    RtFile file;     ///< Underlying file handle; fd == -1 when not open.
    bool in_use;     ///< True when the channel is open and available for I/O.
    bool at_eof;     ///< Cached EOF flag; set after a read returns EOF.
} RtFileChannelEntry;

/// @brief Release all open channel entries and free the file table in `ctx`.
/// @param ctx Runtime context whose exclusively owned file state is being torn
/// down; `NULL` is accepted.
void rt_file_state_cleanup(RtContext *ctx) {
    if (!ctx)
        return;

    RtFileChannelEntry *entries = (RtFileChannelEntry *)ctx->file_state.entries;
    size_t count = ctx->file_state.count;
    if (entries) {
        for (size_t i = 0; i < count; ++i) {
            RtFileChannelEntry *entry = &entries[i];
            if (entry->in_use) {
                RtError err = RT_ERROR_NONE;
                (void)rt_file_close(&entry->file, &err);
                entry->in_use = false;
                entry->at_eof = false;
                rt_file_init(&entry->file);
            }
        }
        free(entries);
    }

    ctx->file_state.entries = NULL;
    ctx->file_state.count = 0;
    ctx->file_state.capacity = 0;
}

/// @brief Return a typed pointer to one locked context's channel entry array.
/// @param ctx Context whose `RT_CONTEXT_STATE_FILE` mutex is held.
/// @return Borrowed entry-array pointer, or `NULL` before allocation.
static inline RtFileChannelEntry *rtf_entries(RtContext *ctx) {
    return (RtFileChannelEntry *)ctx->file_state.entries;
}

/// @brief Return a pointer to one locked context's channel count field.
/// @param ctx Context whose file-state mutex is held.
/// @return Borrowed mutable count-field pointer.
static inline size_t *rtf_count(RtContext *ctx) {
    return &ctx->file_state.count;
}

/// @brief Return a pointer to one locked context's channel-table capacity.
/// @param ctx Context whose file-state mutex is held.
/// @return Borrowed mutable capacity-field pointer.
static inline size_t *rtf_capacity(RtContext *ctx) {
    return &ctx->file_state.capacity;
}

/// @brief Replace one locked context's channel entry array pointer.
/// @param ctx Context whose file-state mutex is held.
/// @param ptr Newly allocated table pointer.
static inline void rtf_set_entries(RtContext *ctx, RtFileChannelEntry *ptr) {
    ctx->file_state.entries = ptr;
}

/// @brief Locate an existing channel entry without modifying the table.
/// @details Performs a linear scan over the populated prefix of the table so
///          channel identity remains stable across table reallocations.
///          Non-positive identifiers are rejected immediately. The returned
///          pointer remains valid only while the file-state lock is held and
///          no table-growing helper is invoked.
/// @param ctx Locked runtime context containing the channel table.
/// @param channel Positive BASIC channel identifier.
/// @return Borrowed matching entry, or `NULL` when absent/invalid.
static RtFileChannelEntry *rt_file_find_channel(RtContext *ctx, int32_t channel) {
    if (channel <= 0)
        return NULL;
    RtFileChannelEntry *entries = rtf_entries(ctx);
    size_t count = entries ? *rtf_count(ctx) : 0;
    for (size_t i = 0; i < count; ++i) {
        if (entries[i].channel == channel)
            return &entries[i];
    }
    return NULL;
}

/// @brief Maximum number of open file channels.
/// @details Prevents unbounded resource allocation from untrusted input.
static const size_t kMaxOpenChannels = 1024;

/// @brief Ensure a table entry exists for @p channel, allocating if necessary.
/// @details Reuses an existing entry when one already tracks the identifier.
///          Otherwise the table grows geometrically, new slots are initialised
///          via @ref rt_file_init, and the freshly provisioned entry is returned
///          to the caller.  Allocation failures bubble up as @c NULL so callers
///          can surface @ref Err_RuntimeError.
/// @param ctx Locked runtime context whose table may be reallocated.
/// @param channel Positive BASIC channel identifier.
/// @return Borrowed existing/new entry, or `NULL` for invalid input,
/// capacity exhaustion, or allocation failure.
static RtFileChannelEntry *rt_file_prepare_channel(RtContext *ctx, int32_t channel) {
    if (channel <= 0)
        return NULL;
    RtFileChannelEntry *entry = rt_file_find_channel(ctx, channel);
    if (entry)
        return entry;

    RtFileChannelEntry *entries = rtf_entries(ctx);
    size_t *pcount = rtf_count(ctx);
    size_t *pcap = rtf_capacity(ctx);
    if (!pcount || !pcap)
        return NULL;
    if (*pcount >= kMaxOpenChannels)
        return NULL;
    if (*pcount == *pcap) {
        size_t new_capacity = *pcap ? (*pcap) * 2 : 4;
        RtFileChannelEntry *new_entries =
            (RtFileChannelEntry *)realloc(entries, new_capacity * sizeof(*new_entries));
        if (!new_entries)
            return NULL;
        for (size_t i = *pcap; i < new_capacity; ++i) {
            new_entries[i].channel = 0;
            new_entries[i].in_use = false;
            new_entries[i].at_eof = false;
            rt_file_init(&new_entries[i].file);
        }
        rtf_set_entries(ctx, new_entries);
        *pcap = new_capacity;
    }
    entries = rtf_entries(ctx);
    entry = &entries[(*pcount)++];
    entry->channel = channel;
    entry->in_use = false;
    entry->at_eof = false;
    rt_file_init(&entry->file);
    return entry;
}

/// @brief Resolve @p channel to an open entry, returning a runtime error code.
/// @details Validates the channel identifier, ensures the entry is actively in
///          use, and confirms that the cached @ref RtFile still owns a live
///          descriptor.  When successful the resolved entry is stored in
///          @p out_entry so callers can perform further operations without a
///          second lookup.
/// @param ctx Locked runtime context containing the channel table.
/// @param channel Positive channel identifier to resolve.
/// @param out_entry Receives a borrowed open entry on success and `NULL` on
/// failure; may itself be `NULL`.
/// @return 0 on success, `Err_InvalidOperation` for an invalid/closed channel,
/// or `Err_IOError` for inconsistent descriptor state.
static int32_t rt_file_resolve_channel(RtContext *ctx,
                                       int32_t channel,
                                       RtFileChannelEntry **out_entry) {
    if (out_entry)
        *out_entry = NULL;
    if (channel <= 0)
        return (int32_t)Err_InvalidOperation;
    RtFileChannelEntry *entry = rt_file_find_channel(ctx, channel);
    if (!entry || !entry->in_use)
        return (int32_t)Err_InvalidOperation;
    if (entry->file.fd < 0)
        return (int32_t)Err_IOError;
    if (out_entry)
        *out_entry = entry;
    return 0;
}

/// @brief Write @p len bytes to the channel, updating EOF tracking.
/// @details Validates pointers, forwards the call to @ref rt_file_write, clears
///          the cached EOF state when the write succeeds, and translates any
///          failure into the corresponding Err_* value.
/// @param entry Open channel entry.
/// @param data Source bytes; may be `NULL` only when @p len is zero.
/// @param len Number of bytes to write.
/// @return 0 on success/no-op, or an `Err` code on invalid input or I/O
/// failure.
static int32_t rt_file_write_entry(RtFileChannelEntry *entry, const uint8_t *data, size_t len) {
    if (!entry || len == 0)
        return 0;
    if (!data)
        return (int32_t)Err_InvalidOperation;
    RtError err = RT_ERROR_NONE;
    if (!rt_file_write(&entry->file, data, len, &err))
        return (int32_t)err.kind;
    entry->at_eof = false;
    return 0;
}

/// @brief Open a BASIC channel for the path stored in a runtime string.
/// @details Translates the BASIC mode enum into a host mode string, converts the
///          runtime string path into a filesystem path, allocates or reuses a
///          channel entry, and invokes @ref rt_file_open.  When the open
///          succeeds the entry is flagged in-use and its EOF indicator cleared;
///          failures propagate the error kind from the lower layer.
/// @param path Runtime filesystem path.
/// @param mode @ref RtFileMode value.
/// @param channel Positive BASIC channel identifier.
/// @return 0 on success, or an `Err` code for invalid input, duplicate/open
/// channel, context/allocation failure, or filesystem failure.
int32_t rt_open_err_vstr(ZannaString *path, int32_t mode, int32_t channel) {
    const char *mode_str = rt_file_mode_string(mode);
    const char *path_str = NULL;
    if (!mode_str || !rt_file_path_from_vstr(path, &path_str) || channel <= 0)
        return (int32_t)Err_InvalidOperation;

    RtContext *ctx = rt_context_acquire_state(RT_CONTEXT_STATE_FILE, NULL);
    if (!ctx)
        return (int32_t)Err_RuntimeError;
    RtFileChannelEntry *entry = rt_file_prepare_channel(ctx, channel);
    if (!entry) {
        rt_context_release_state(ctx, RT_CONTEXT_STATE_FILE);
        return (int32_t)Err_RuntimeError;
    }
    if (entry->in_use) {
        rt_context_release_state(ctx, RT_CONTEXT_STATE_FILE);
        return (int32_t)Err_InvalidOperation;
    }

    rt_file_init(&entry->file);
    RtError err = RT_ERROR_NONE;
    if (!rt_file_open(&entry->file, path_str, mode_str, mode, &err)) {
        entry->in_use = false;
        rt_context_release_state(ctx, RT_CONTEXT_STATE_FILE);
        return (int32_t)err.kind;
    }

    entry->in_use = true;
    entry->at_eof = false;
    rt_context_release_state(ctx, RT_CONTEXT_STATE_FILE);
    return 0;
}

/// @brief Close the file associated with @p channel.
/// @details Validates that the channel exists and is open, closes the underlying
///          descriptor, and resets bookkeeping state.  Returns zero on success or
///          propagates the runtime error code raised by @ref rt_file_close.
/// @param channel Positive BASIC channel identifier.
/// @return 0 on success, or an `Err` code for invalid/closed channel, missing
/// context, or host close failure.
int32_t rt_close_err(int32_t channel) {
    if (channel <= 0)
        return (int32_t)Err_InvalidOperation;
    RtContext *ctx = rt_context_acquire_state(RT_CONTEXT_STATE_FILE, NULL);
    if (!ctx)
        return (int32_t)Err_RuntimeError;
    RtFileChannelEntry *entry = rt_file_find_channel(ctx, channel);
    if (!entry || !entry->in_use) {
        rt_context_release_state(ctx, RT_CONTEXT_STATE_FILE);
        return (int32_t)Err_InvalidOperation;
    }

    RtError err = RT_ERROR_NONE;
    if (!rt_file_close(&entry->file, &err)) {
        entry->in_use = false;
        entry->at_eof = false;
        rt_file_init(&entry->file);
        rt_context_release_state(ctx, RT_CONTEXT_STATE_FILE);
        return (int32_t)err.kind;
    }

    entry->in_use = false;
    entry->at_eof = false;
    rt_file_init(&entry->file);
    rt_context_release_state(ctx, RT_CONTEXT_STATE_FILE);
    return 0;
}

/// @brief Write the contents of @p s to a channel without appending a newline.
/// @details Resolves the channel, obtains a byte slice via
///          @ref rt_file_string_view, and then calls
///          @ref rt_file_write_entry so EOF caching and error translation remain
///          centralised in one helper.
/// @param channel Positive open channel identifier.
/// @param s Runtime string whose exact bytes are written.
/// @return 0 on success, or an `Err` code for invalid state/input or I/O
/// failure.
int32_t rt_write_ch_err(int32_t channel, ZannaString *s) {
    RtContext *ctx = rt_context_acquire_state(RT_CONTEXT_STATE_FILE, NULL);
    if (!ctx)
        return (int32_t)Err_RuntimeError;
    RtFileChannelEntry *entry = NULL;
    int32_t status = rt_file_resolve_channel(ctx, channel, &entry);
    if (status != 0) {
        rt_context_release_state(ctx, RT_CONTEXT_STATE_FILE);
        return status;
    }

    const uint8_t *data = NULL;
    size_t len = rt_file_string_view(s, &data);
    if (!data && len == 0) {
        rt_context_release_state(ctx, RT_CONTEXT_STATE_FILE);
        return (int32_t)Err_InvalidOperation;
    }
    status = rt_file_write_entry(entry, data, len);
    rt_context_release_state(ctx, RT_CONTEXT_STATE_FILE);
    return status;
}

/// @brief Write @p s followed by a newline to the specified channel.
/// @details Resolves the channel, writes the provided bytes, and finally emits a
///          single newline so the behaviour matches PRINT without a trailing
///          semicolon in traditional BASIC.
/// @param channel Positive open channel identifier.
/// @param s Runtime string written before the newline.
/// @return 0 on success, or an `Err` code for invalid state/input or either
/// write failure.
int32_t rt_println_ch_err(int32_t channel, ZannaString *s) {
    RtContext *ctx = rt_context_acquire_state(RT_CONTEXT_STATE_FILE, NULL);
    if (!ctx)
        return (int32_t)Err_RuntimeError;
    RtFileChannelEntry *entry = NULL;
    int32_t status = rt_file_resolve_channel(ctx, channel, &entry);
    if (status != 0) {
        rt_context_release_state(ctx, RT_CONTEXT_STATE_FILE);
        return status;
    }

    const uint8_t *data = NULL;
    size_t len = rt_file_string_view(s, &data);
    if (!data && len == 0) {
        rt_context_release_state(ctx, RT_CONTEXT_STATE_FILE);
        return (int32_t)Err_InvalidOperation;
    }
    status = rt_file_write_entry(entry, data, len);
    if (status != 0) {
        rt_context_release_state(ctx, RT_CONTEXT_STATE_FILE);
        return status;
    }

    const uint8_t newline = (uint8_t)'\n';
    status = rt_file_write_entry(entry, &newline, 1);
    rt_context_release_state(ctx, RT_CONTEXT_STATE_FILE);
    return status;
}

/// @brief Read a line of text from @p channel, allocating a runtime string.
/// @details Resolves the channel, delegates to @ref rt_file_read_line to perform
///          the blocking read, marks the cached EOF flag when the helper reports
///          end-of-file, and on success transfers ownership of the allocated
///          runtime string to @p out.
/// @param channel Positive channel opened for input.
/// @param out Receives a newly allocated runtime string on success and `NULL`
/// on failure.
/// @return 0 on success, `Err_EOF` at end-of-file, or another `Err` code for
/// invalid state/input or I/O failure.
int32_t rt_line_input_ch_err(int32_t channel, ZannaString **out) {
    if (!out)
        return (int32_t)Err_InvalidOperation;
    *out = NULL;

    RtContext *ctx = rt_context_acquire_state(RT_CONTEXT_STATE_FILE, NULL);
    if (!ctx)
        return (int32_t)Err_RuntimeError;
    RtFileChannelEntry *entry = NULL;
    int32_t status = rt_file_resolve_channel(ctx, channel, &entry);
    if (status != 0) {
        rt_context_release_state(ctx, RT_CONTEXT_STATE_FILE);
        return status;
    }

    rt_string line = NULL;
    RtError err = RT_ERROR_NONE;
    if (!rt_file_read_line(&entry->file, &line, &err)) {
        if (err.kind == Err_EOF)
            entry->at_eof = true;
        rt_context_release_state(ctx, RT_CONTEXT_STATE_FILE);
        return (int32_t)err.kind;
    }

    entry->at_eof = false;

    *out = line;
    rt_context_release_state(ctx, RT_CONTEXT_STATE_FILE);
    return 0;
}

/// @brief Retrieve the host file descriptor associated with @p channel.
/// @details Resolves the channel and copies the descriptor into @p out_fd, if
///          provided, so embedders can integrate with poll/select loops using the
///          underlying OS handle.
/// @param channel Positive open channel identifier.
/// @param out_fd Receives the borrowed native descriptor when non-`NULL`.
/// @return 0 on success, or an `Err` code when the context/channel is invalid.
int32_t rt_file_channel_fd(int32_t channel, int *out_fd) {
    RtContext *ctx = rt_context_acquire_state(RT_CONTEXT_STATE_FILE, NULL);
    if (!ctx)
        return (int32_t)Err_RuntimeError;
    RtFileChannelEntry *entry = NULL;
    int32_t status = rt_file_resolve_channel(ctx, channel, &entry);
    if (status != 0) {
        rt_context_release_state(ctx, RT_CONTEXT_STATE_FILE);
        return status;
    }
    if (out_fd)
        *out_fd = entry->file.fd;
    rt_context_release_state(ctx, RT_CONTEXT_STATE_FILE);
    return 0;
}

/// @brief Query whether @p channel is currently positioned at EOF.
/// @details Resolves the channel and exposes the cached EOF flag maintained by
///          read helpers, mirroring the VM's "sticky" EOF semantics.
/// @param channel Positive open channel identifier.
/// @param out_at_eof Receives 0 or 1 when non-`NULL`.
/// @return 0 on success, or an `Err` code when the context/channel is invalid.
int32_t rt_file_channel_get_eof(int32_t channel, int8_t *out_at_eof) {
    RtContext *ctx = rt_context_acquire_state(RT_CONTEXT_STATE_FILE, NULL);
    if (!ctx)
        return (int32_t)Err_RuntimeError;
    RtFileChannelEntry *entry = NULL;
    int32_t status = rt_file_resolve_channel(ctx, channel, &entry);
    if (status != 0) {
        rt_context_release_state(ctx, RT_CONTEXT_STATE_FILE);
        return status;
    }
    if (out_at_eof)
        *out_at_eof = entry->at_eof;
    rt_context_release_state(ctx, RT_CONTEXT_STATE_FILE);
    return 0;
}

/// @brief Mutate the cached EOF state for @p channel.
/// @details Resolves the channel and updates the cached flag, enabling seek
///          helpers to force EOF on or off without performing another read.
/// @param channel Positive open channel identifier.
/// @param at_eof New cached value; stored as supplied.
/// @return 0 on success, or an `Err` code when the context/channel is invalid.
int32_t rt_file_channel_set_eof(int32_t channel, int8_t at_eof) {
    RtContext *ctx = rt_context_acquire_state(RT_CONTEXT_STATE_FILE, NULL);
    if (!ctx)
        return (int32_t)Err_RuntimeError;
    RtFileChannelEntry *entry = NULL;
    int32_t status = rt_file_resolve_channel(ctx, channel, &entry);
    if (status != 0) {
        rt_context_release_state(ctx, RT_CONTEXT_STATE_FILE);
        return status;
    }
    entry->at_eof = at_eof;
    rt_context_release_state(ctx, RT_CONTEXT_STATE_FILE);
    return 0;
}
