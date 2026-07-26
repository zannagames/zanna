//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Provides the debugging utilities responsible for breakpoint handling,
// stepping, and block parameter propagation.  These helpers keep the
// interpreter loop focused on opcode dispatch while consolidating debug
// bookkeeping in a single translation unit.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the IL VM's debugger integration helpers.
/// @details The routines here coordinate between the interpreter core, the
///          debug controller, and optional scripting front-ends.  They transfer
///          staged block parameters, evaluate breakpoints, honour step budgets,
///          and surface rich diagnostics describing why execution pauses.
///          Centralising the logic keeps the dispatch loop uncluttered and
///          ensures all debug pathways apply consistent invariants when
///          manipulating VM state.

#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "support/source_manager.hpp"
#include "zanna/vm/debug/Debug.hpp"
#include "vm/OpHandlerUtils.hpp"
#include "vm/VM.hpp"
#include "vm/VMConstants.hpp"

#include "rt_box.h"
#include "rt_collection_ids.h"
#include "rt_object.h"
#include "rt_string.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Collection element accessors used only for read-only debugger inspection of a
// paused frame. Forward-declared (rather than pulling the collection headers into
// this VM TU) to keep the debug surface minimal; signatures mirror the runtime.
extern "C" {
/// @brief Return the number of elements in a runtime List.
/// @param list Runtime list handle.
/// @return Element count, or the runtime's invalid-handle sentinel.
int64_t rt_list_len(void *list);
/// @brief Read a List element without transferring ownership.
/// @param list Runtime list handle.
/// @param index Zero-based element index.
/// @return Borrowed element handle.
void *rt_list_get(void *list, int64_t index);
/// @brief Return the number of elements in a runtime Seq.
/// @param obj Runtime sequence handle.
/// @return Element count, or the runtime's invalid-handle sentinel.
int64_t rt_seq_len(void *obj);
/// @brief Read a Seq element without transferring ownership.
/// @param obj Runtime sequence handle.
/// @param idx Zero-based element index.
/// @return Borrowed element handle.
void *rt_seq_get(void *obj, int64_t idx);
/// @brief Return the number of entries in a runtime Map.
/// @param obj Runtime map handle.
/// @return Entry count, or the runtime's invalid-handle sentinel.
int64_t rt_map_len(void *obj);
/// @brief Materialize a newly owned sequence containing the map's keys.
/// @param map Runtime map handle.
/// @return Owned key sequence, or @c nullptr on failure.
void *rt_map_keys_to_seq(void *map);
/// @brief Materialize a newly owned sequence containing the map's values.
/// @param map Runtime map handle.
/// @return Owned value sequence, or @c nullptr on failure.
void *rt_map_values_to_seq(void *map);
}

using namespace il::core;

namespace il::vm {

/// @brief Request an interactive debugger pause on a VM.
/// @param vm VM that should stop at its next debug boundary.
void requestDebugPause(VM &vm) {
    vm.requestDebugPause();
}

/// @brief Replace the active stepping state with a debugger action.
/// @details Clears prior budgets and frame-depth modes, then configures
///          continue, counted step, step-over, or step-out behavior relative
///          to the current execution-stack depth.
/// @param action Action selected by the script or interactive frontend.
/// @param currentDepth Active execution-stack depth at the stop.
void VM::applyDebugAction(DebugAction action, size_t currentDepth) {
    stepBudget = 0;
    debugStepMode_ = DebugStepMode::None;
    debugStepTargetDepth_ = currentDepth;
    debugStepArmed_ = false;

    switch (action.kind) {
        case DebugActionKind::Continue:
            break;
        case DebugActionKind::Step:
            stepBudget = action.count == 0 ? 1 : action.count;
            break;
        case DebugActionKind::StepOver:
            debugStepMode_ = DebugStepMode::StepOver;
            debugStepTargetDepth_ = currentDepth;
            break;
        case DebugActionKind::StepOut:
            debugStepMode_ = DebugStepMode::StepOut;
            debugStepTargetDepth_ = currentDepth == 0 ? 0 : currentDepth - 1;
            break;
    }
}

namespace {

/// @brief Format a double compactly for variable display.
/// @param d Floating-point value to render.
/// @return Locale-dependent compact decimal representation.
std::string formatDouble(double d) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", d);
    return std::string(buf);
}

/// @brief Render an rt_string as a quoted, truncated display value. Only called
///        when the register is known to hold a valid owned string.
/// @param s Valid runtime string handle, or @c nullptr for an empty display.
/// @return Quoted value with control characters sanitized and long content truncated.
std::string quoteRtString(rt_string s) {
    std::string out = "\"";
    if (s) {
        const char *data = rt_string_cstr(s);
        size_t len = static_cast<size_t>(rt_str_len(s));
        constexpr size_t kMax = 200;
        for (size_t i = 0; i < len && i < kMax; ++i) {
            unsigned char c = static_cast<unsigned char>(data[i]);
            if (c == '\n')
                out += "\\n";
            else if (c == '\t')
                out += "\\t";
            else if (c < 0x20)
                out.push_back(' ');
            else
                out.push_back(static_cast<char>(c));
        }
        if (len > kMax)
            out += "...";
    }
    out.push_back('"');
    return out;
}

/// @brief Convert an owned runtime string to std::string and release it.
/// @details Runtime object introspection returns newly allocated strings. The
///          debugger copies the bytes immediately so the runtime handle can be
///          released before the stop snapshot is serialized.
/// @param s Owned runtime string handle, or NULL.
/// @return Byte-for-byte copy of @p s, or an empty string for NULL/invalid data.
std::string takeRtString(rt_string s) {
    if (!s)
        return {};
    std::string out;
    const char *data = rt_string_cstr(s);
    const int64_t len = rt_str_len(s);
    if (data && len > 0)
        out.assign(data, data + len);
    rt_string_unref(s);
    return out;
}

/// @brief Format a pointer value for debugger local display.
/// @details Managed runtime objects are identified by the object header and
///          displayed with their runtime type name. Raw pointers remain hidden
///          unless @p allowRaw is true, because arbitrary addresses are usually
///          compiler temporaries rather than source-level values.
/// @param value Pointer value to describe.
/// @param local Local-display record to populate.
/// @param allowRaw Whether non-managed pointers should be shown as "<ptr>".
/// @param managedOut When non-null, receives @p value if it is a managed runtime
///        object (so the caller can offer structured expansion); left untouched
///        for null/raw pointers.
/// @return True when @p local was populated.
bool formatPointerValue(void *value,
                        DebugLocalInfo &local,
                        bool allowRaw,
                        void **managedOut = nullptr) {
    if (!value) {
        local.type = "ptr";
        local.value = "null";
        return true;
    }

    const int64_t typeId = rt_obj_type_id(value);
    const std::string typeName = takeRtString(rt_obj_type_name(value));
    if (typeId != 0 || (!typeName.empty() && typeName != "Object")) {
        local.type = typeName.empty() ? "object" : typeName;
        local.value = typeId == 0 ? "<object>" : "<object #" + std::to_string(typeId) + ">";
        if (managedOut)
            *managedOut = value;
        return true;
    }

    if (!allowRaw)
        return false;
    local.type = "ptr";
    local.value = "<ptr>";
    return true;
}

/// @brief Read a scalar of @p kind from a frame-stack alloca pointer into @p local.
/// @details Bounds-checks @p ptr against the frame's fixed-size operand stack so a
///          stale or non-alloca pointer cannot fault. Only fixed-size scalars are
///          loaded; aggregate/string pointees are left to the caller.
/// @param fr Frame whose operand stack may contain @p ptr.
/// @param ptr Candidate address of an alloca-backed scalar.
/// @param kind Static pointee type used to select the read width and formatter.
/// @param [out] local Debugger value populated after a successful read.
/// @param [out] managedOut Optional destination for a managed pointer value.
/// @return True when a value was read and formatted.
bool loadScalarFromAlloca(const Frame &fr,
                          void *ptr,
                          Type::Kind kind,
                          DebugLocalInfo &local,
                          void **managedOut = nullptr) {
    if (!ptr)
        return false;
    size_t size = 0;
    switch (kind) {
        case Type::Kind::I64:
        case Type::Kind::F64:
        case Type::Kind::Ptr:
        case Type::Kind::Str:
            size = 8;
            break;
        case Type::Kind::I32:
            size = 4;
            break;
        case Type::Kind::I16:
            size = 2;
            break;
        case Type::Kind::I1:
            size = 1;
            break;
        default:
            return false; // strings/aggregates: not loaded here
    }
    const uint8_t *base = fr.stack.data();
    if (!base || fr.stack.empty())
        return false;
    const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(base);
    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(ptr);
    if (address < begin)
        return false;
    const std::uintptr_t offset = address - begin;
    if (offset > fr.stack.size() || size > fr.stack.size() - offset)
        return false; // not inside this frame's alloca region
    const uint8_t *p = base + offset;

    if (kind == Type::Kind::F64) {
        double d = 0.0;
        std::memcpy(&d, p, 8);
        local.type = "f64";
        local.value = formatDouble(d);
    } else if (kind == Type::Kind::I1) {
        uint8_t b = 0;
        std::memcpy(&b, p, 1);
        local.type = "bool";
        local.value = b ? "true" : "false";
    } else if (kind == Type::Kind::I32) {
        int32_t v = 0;
        std::memcpy(&v, p, 4);
        local.type = "i64";
        local.value = std::to_string(static_cast<int64_t>(v));
    } else if (kind == Type::Kind::I16) {
        int16_t v = 0;
        std::memcpy(&v, p, 2);
        local.type = "i64";
        local.value = std::to_string(static_cast<int64_t>(v));
    } else if (kind == Type::Kind::Str) {
        rt_string s = nullptr;
        std::memcpy(&s, p, sizeof(rt_string));
        local.type = "str";
        // rt_string_is_handle validates against the runtime's string registry and
        // is safe on garbage, so a not-yet-stored alloca (stale pooled bytes) cannot
        // deref an invalid handle.
        if (rt_string_is_handle(s))
            local.value = quoteRtString(s);
        else
            local.value = "\"\"";
    } else if (kind == Type::Kind::Ptr) {
        void *value = nullptr;
        std::memcpy(&value, p, sizeof(void *));
        formatPointerValue(value, local, true, managedOut);
    } else { // I64
        int64_t v = 0;
        std::memcpy(&v, p, 8);
        local.type = "i64";
        local.value = std::to_string(v);
    }
    return true;
}

/// @brief Format a scalar register value held directly in an SSA value.
/// @param fr Frame supplying the register and string-ownership metadata.
/// @param id Register index to display.
/// @param kind Static IL type controlling interpretation of the slot.
/// @param [out] local Debugger value populated from the register.
void formatRegScalar(const Frame &fr, size_t id, Type::Kind kind, DebugLocalInfo &local) {
    const Slot &slot = fr.regs[id];
    switch (kind) {
        case Type::Kind::F64:
            local.type = "f64";
            local.value = formatDouble(slot.f64);
            break;
        case Type::Kind::Str:
            local.type = "str";
            if (id < fr.regIsStr.size() && fr.regIsStr[id])
                local.value = quoteRtString(slot.str);
            else
                local.value = "\"\"";
            break;
        case Type::Kind::I1:
            local.type = "bool";
            local.value = slot.i64 ? "true" : "false";
            break;
        default: // integral
            local.type = "i64";
            local.value = std::to_string(slot.i64);
            break;
    }
}

/// @brief Cap on map pairs materialized eagerly for a single map value, bounding
///        the work done at a stop for very large maps (the UI pages within this).
constexpr int64_t kMaxEagerMapPairs = 2000;

/// @brief Lazy, one-level child provider backing DebugStopInfo::vars for the
///        current stop. Expands List, Seq, the canonical Map, and — when the
///        host installed a class-layout sidecar (ADR 0138) — user class
///        instances field-by-field; every other managed value is a leaf.
/// @details Safety: this runs on the paused VM thread inside onStop, so it must
///          never re-enter the interpreter. Values are formatted purely — boxed
///          primitives via rt_unbox_* (guarded by a rt_box_type tag check so they
///          cannot trap), strings quoted directly, object fields by raw reads of
///          frame-owned object memory at compiler-recorded offsets — never
///          through user ToString. List/Seq/object handles are owned by the
///          paused frame and read in place (no retain). The only allocations are
///          the transient key/value seqs used to enumerate a map, which are
///          released immediately.
class VmDebugVarStore : public DebugVarExpander {
  public:
    /// @brief Construct a variable expander for one paused VM snapshot.
    /// @param layouts Class-layout sidecar for object expansion, or null/empty
    ///        when the host installed none (objects then stay leaves).
    explicit VmDebugVarStore(const DebugClassLayoutTable *layouts) : layouts_(layouts) {}

    /// @brief If @p v is an expandable container, register it and populate the
    ///        display fields for a top-level local.
    /// @param v Managed runtime value to classify.
    /// @param [out] local Top-level local record updated on successful classification.
    /// @return @c true for a registered container or object; @c false for a leaf.
    bool classify(void *v, DebugLocalInfo &local) {
        Kind k;
        int64_t n = 0;
        const DebugClassLayout *layout = nullptr;
        if (!detect(v, k, n, layout))
            return false;
        if (k == Kind::Object) {
            local.type = layout->qname;
            local.value = objectPreview(v, *layout);
        } else {
            local.type = kindName(k);
            local.value = summaryOf(k, n);
        }
        local.varRef = registerContainer(v, k, n, layout);
        local.childCount = childCountOf(local.varRef, k, n);
        return true;
    }

    /// @brief Query whether this stop snapshot registered any expandable values.
    /// @return @c true when no variable references have been allocated.
    bool empty() const {
        return entries_.empty();
    }

    /// @brief Materialize a page of immediate children for a variable reference.
    /// @details Map children are copied from the eager snapshot, objects are
    ///          read through compiler-provided layouts, and List/Seq elements
    ///          are inspected without invoking user code.
    /// @param ref One-based variable reference returned by this store.
    /// @param start Zero-based first child index; negative values clamp to zero.
    /// @param count Maximum number of children to return.
    /// @return Available children in ascending index or field order.
    std::vector<DebugLocalInfo> expand(int64_t ref, int64_t start, int64_t count) override {
        std::vector<DebugLocalInfo> out;
        if (ref < 1 || static_cast<size_t>(ref) > entries_.size() || count <= 0)
            return out;
        // Copy the entry's fixed state before iterating: describing a nested
        // container registers it (entries_.push_back), which can reallocate the
        // vector and would dangle any reference held across the loop. The layout
        // pointer is stable (it targets the host-owned sidecar, not entries_).
        const Entry &e = entries_[static_cast<size_t>(ref) - 1];
        const Kind kind = e.kind;
        void *const handle = e.handle;
        const int64_t total = e.count;
        const DebugClassLayout *const layout = e.layout;
        if (start < 0)
            start = 0;
        if (kind == Kind::Map) {
            // Map children are pre-materialized leaves; nothing registers while
            // copying them out, so indexing entries_ fresh each pass is safe.
            for (int64_t i = start; i < start + count; ++i) {
                const Entry &me = entries_[static_cast<size_t>(ref) - 1];
                if (static_cast<size_t>(i) >= me.mapChildren.size())
                    break;
                out.push_back(me.mapChildren[static_cast<size_t>(i)]);
            }
            return out;
        }
        if (kind == Kind::Object) {
            for (int64_t i = start; i < start + count && i < total; ++i) {
                DebugLocalInfo child;
                const DebugFieldLayout &field = layout->fields[static_cast<size_t>(i)];
                child.name = field.name;
                describeField(handle, field, child, /*allowRegister=*/true);
                out.push_back(std::move(child));
            }
            return out;
        }
        for (int64_t i = start; i < start + count && i < total; ++i) {
            DebugLocalInfo child;
            child.name = "[" + std::to_string(i) + "]";
            void *elem = (kind == Kind::List) ? rt_list_get(handle, i) : rt_seq_get(handle, i);
            describeValue(elem, child, /*allowRegister=*/true);
            out.push_back(std::move(child));
        }
        return out;
    }

  private:
    /// @brief Expandable runtime-value category.
    enum class Kind { List, Seq, Map, Object };

    /// @brief Stored expansion metadata associated with one variable reference.
    struct Entry {
        Kind kind = Kind::List; ///< Backend category used during expansion.
        void *handle = nullptr; ///< Frame-owned List, Seq, or object; unused for maps.
        int64_t count = 0; ///< Available element or field count.
        const DebugClassLayout *layout = nullptr; ///< Sidecar-owned object layout.
        std::vector<DebugLocalInfo> mapChildren; ///< Eagerly materialized map entries.
    };

    /// @brief Return the debugger type label for a collection category.
    /// @param k Collection category to name.
    /// @return Static display name; non-List/Seq categories map to "Map".
    static const char *kindName(Kind k) {
        switch (k) {
            case Kind::List:
                return "List";
            case Kind::Seq:
                return "Seq";
            default:
                return "Map";
        }
    }

    /// @brief Build a compact collection summary containing its size.
    /// @param k Collection category to name.
    /// @param n Reported runtime size; negative values are displayed as zero.
    /// @return Display text such as @c "List(3)".
    static std::string summaryOf(Kind k, int64_t n) {
        return std::string(kindName(k)) + "(" + std::to_string(n < 0 ? 0 : n) + ")";
    }

    /// @brief Determine the child count exposed for a registered value.
    /// @param ref One-based reference of the registered entry.
    /// @param k Entry category.
    /// @param n Runtime-provided collection or field count.
    /// @return Materialized map child count or nonnegative @p n.
    int64_t childCountOf(int64_t ref, Kind k, int64_t n) const {
        if (k == Kind::Map)
            return static_cast<int64_t>(entries_[static_cast<size_t>(ref) - 1].mapChildren.size());
        return n < 0 ? 0 : n;
    }

    /// @brief Classify a managed runtime value as an expandable collection or object.
    /// @param v Managed value to inspect.
    /// @param [out] k Detected expansion category.
    /// @param [out] n Detected element or field count.
    /// @param [out] layout Matching class layout for objects, otherwise @c nullptr.
    /// @return @c true when @p v has a supported expansion representation.
    bool detect(void *v, Kind &k, int64_t &n, const DebugClassLayout *&layout) const {
        layout = nullptr;
        if (rt_obj_is_instance(v, RT_LIST_CLASS_ID, 0)) {
            k = Kind::List;
            n = rt_list_len(v);
            return true;
        }
        if (rt_obj_is_instance(v, RT_SEQ_CLASS_ID, 0)) {
            k = Kind::Seq;
            n = rt_seq_len(v);
            return true;
        }
        if (rt_obj_is_instance(v, RT_MAP_CLASS_ID, 0)) {
            k = Kind::Map;
            n = rt_map_len(v);
            return true;
        }
        // User class instance with a known layout: ids are positive and
        // compiler-assigned; builtin runtime ids are negative, so a table hit
        // can only be the debugged module's own class (ADR 0138).
        if (layouts_ && !layouts_->empty()) {
            const int64_t typeId = rt_obj_type_id(v);
            if (typeId > 0) {
                auto it = layouts_->find(typeId);
                if (it != layouts_->end()) {
                    k = Kind::Object;
                    n = static_cast<int64_t>(it->second.fields.size());
                    layout = &it->second;
                    return true;
                }
            }
        }
        return false;
    }

    /// @brief Allocate a one-based variable reference for an expandable value.
    /// @param v Frame-owned runtime handle.
    /// @param k Expansion category.
    /// @param n Runtime-provided element or field count.
    /// @param layout Sidecar-owned layout for objects, otherwise @c nullptr.
    /// @return Newly assigned one-based variable reference.
    int64_t registerContainer(void *v, Kind k, int64_t n, const DebugClassLayout *layout) {
        Entry e;
        e.kind = k;
        e.handle = v;
        e.count = n < 0 ? 0 : n;
        e.layout = layout;
        if (k == Kind::Map)
            materializeMap(v, e);
        entries_.push_back(std::move(e));
        return static_cast<int64_t>(entries_.size()); // 1-based ref
    }

    /// @brief Snapshot a bounded number of map key/value pairs as debugger leaves.
    /// @param map Runtime map handle to enumerate.
    /// @param [out] e Entry receiving materialized children.
    void materializeMap(void *map, Entry &e) {
        void *keys = rt_map_keys_to_seq(map);
        void *vals = rt_map_values_to_seq(map);
        if (keys && vals) {
            const int64_t kn = rt_seq_len(keys);
            const int64_t vn = rt_seq_len(vals);
            const int64_t n = kn < vn ? kn : vn;
            const int64_t limit = n < kMaxEagerMapPairs ? n : kMaxEagerMapPairs;
            for (int64_t i = 0; i < limit; ++i) {
                DebugLocalInfo keyView;
                describeValue(rt_seq_get(keys, i), keyView, /*allowRegister=*/false);
                DebugLocalInfo child;
                child.name = unquote(keyView.value);
                // Map values are shown one level deep as leaf summaries to keep the
                // transient key/value seqs' lifetime trivial (no nested registration).
                describeValue(rt_seq_get(vals, i), child, /*allowRegister=*/false);
                e.mapChildren.push_back(std::move(child));
            }
        }
        if (keys)
            rt_memory_release(keys);
        if (vals)
            rt_memory_release(vals);
    }

    /// @brief Strip one pair of surrounding quotes so a string key reads cleanly
    ///        as a child name.
    /// @param s Candidate quoted display value.
    /// @return Unquoted content when both quotes are present, otherwise @p s.
    static std::string unquote(const std::string &s) {
        if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
            return s.substr(1, s.size() - 2);
        return s;
    }

    /// @brief Fill @p out from a runtime value without ever invoking user code.
    ///        Containers are registered for expansion when @p allowRegister is true.
    /// @param v Runtime value to inspect.
    /// @param [out] out Debugger record populated with type, summary, and optional reference.
    /// @param allowRegister Whether nested expandable values receive variable references.
    void describeValue(void *v, DebugLocalInfo &out, bool allowRegister) {
        if (!v) {
            out.type = "ptr";
            out.value = "null";
            return;
        }
        if (rt_string_is_handle(v)) {
            out.type = "str";
            out.value = quoteRtString(static_cast<rt_string>(v));
            return;
        }
        const int64_t tag = rt_box_type(v);
        if (tag == RT_BOX_I64) {
            out.type = "i64";
            out.value = std::to_string(rt_unbox_i64(v));
            return;
        }
        if (tag == RT_BOX_F64) {
            out.type = "f64";
            out.value = formatDouble(rt_unbox_f64(v));
            return;
        }
        if (tag == RT_BOX_I1) {
            out.type = "bool";
            out.value = rt_unbox_i1(v) ? "true" : "false";
            return;
        }
        if (tag == RT_BOX_STR) {
            rt_string s = nullptr;
            if (rt_box_try_to_str(v, &s)) {
                out.type = "str";
                out.value = quoteRtString(s);
                rt_string_unref(s);
            } else {
                out.type = "str";
                out.value = "\"\"";
            }
            return;
        }
        Kind k;
        int64_t n = 0;
        const DebugClassLayout *layout = nullptr;
        if (detect(v, k, n, layout)) {
            if (k == Kind::Object) {
                out.type = layout->qname;
                // Nested previews are depth-capped so cyclic object graphs
                // (a.next == a) terminate; expansion itself stays user-driven
                // one level per request and needs no guard.
                out.value = previewDepth_ < 2 ? objectPreview(v, *layout)
                                              : "<" + layout->qname + ">";
            } else {
                out.type = kindName(k);
                out.value = summaryOf(k, n);
            }
            if (allowRegister) {
                out.varRef = registerContainer(v, k, n, layout);
                out.childCount = childCountOf(out.varRef, k, n);
            }
            return;
        }
        std::string tn = takeRtString(rt_obj_type_name(v));
        if (tn.empty())
            tn = "object";
        out.type = tn;
        out.value = "<" + tn + ">";
    }

    /// @brief Fill @p out from one instance field of the object at @p base by a
    ///        raw read of frame-owned memory at the compiler-recorded offset.
    ///        Managed fields recurse through describeValue so nested objects and
    ///        collections stay expandable; weak slots resolve via rt_weak_load
    ///        (non-retaining, may be null after collection).
    /// @param base Runtime object whose field storage is read.
    /// @param field Compiler-provided field offset and storage description.
    /// @param [out] out Debugger record populated from the field.
    /// @param allowRegister Whether expandable field values receive references.
    void describeField(void *base,
                       const DebugFieldLayout &field,
                       DebugLocalInfo &out,
                       bool allowRegister) {
        char *addr = static_cast<char *>(base) + field.offset;
        switch (field.storage) {
            case DebugFieldStorage::I64: {
                int64_t v = 0;
                std::memcpy(&v, addr, sizeof(v));
                out.type = field.typeName.empty() ? "i64" : field.typeName;
                out.value = field.boolDisplay ? (v != 0 ? "true" : "false") : std::to_string(v);
                return;
            }
            case DebugFieldStorage::I32: {
                int32_t v = 0;
                std::memcpy(&v, addr, sizeof(v));
                out.type = field.typeName.empty() ? "i32" : field.typeName;
                out.value = field.boolDisplay ? (v != 0 ? "true" : "false") : std::to_string(v);
                return;
            }
            case DebugFieldStorage::I16: {
                int16_t v = 0;
                std::memcpy(&v, addr, sizeof(v));
                out.type = field.typeName.empty() ? "i16" : field.typeName;
                out.value = std::to_string(v);
                return;
            }
            case DebugFieldStorage::I1: {
                unsigned char v = 0;
                std::memcpy(&v, addr, sizeof(v));
                out.type = field.typeName.empty() ? "bool" : field.typeName;
                out.value = v != 0 ? "true" : "false";
                return;
            }
            case DebugFieldStorage::F64: {
                double v = 0.0;
                std::memcpy(&v, addr, sizeof(v));
                out.type = field.typeName.empty() ? "f64" : field.typeName;
                out.value = formatDouble(v);
                return;
            }
            case DebugFieldStorage::Str: {
                void *p = nullptr;
                std::memcpy(&p, addr, sizeof(p));
                out.type = field.typeName.empty() ? "str" : field.typeName;
                out.value = p ? quoteRtString(static_cast<rt_string>(p)) : "null";
                return;
            }
            case DebugFieldStorage::Managed: {
                void *p = nullptr;
                std::memcpy(&p, addr, sizeof(p));
                if (!p) {
                    out.type = field.typeName.empty() ? "object" : field.typeName;
                    out.value = "null";
                    return;
                }
                describeValue(p, out, allowRegister);
                if (out.type.empty())
                    out.type = field.typeName;
                return;
            }
            case DebugFieldStorage::Weak: {
                void *p = rt_weak_load(reinterpret_cast<void **>(addr));
                if (!p) {
                    out.type = field.typeName.empty() ? "weak" : field.typeName;
                    out.value = "null";
                    return;
                }
                describeValue(p, out, allowRegister);
                if (out.type.empty())
                    out.type = field.typeName;
                return;
            }
            case DebugFieldStorage::Raw: {
                out.type = field.typeName.empty() ? "ptr" : field.typeName;
                out.value = "<ptr>";
                return;
            }
            case DebugFieldStorage::Opaque:
            default: {
                out.type = field.typeName.empty() ? "object" : field.typeName;
                out.value = "<" + out.type + ">";
                return;
            }
        }
    }

    /// @brief Compact `{a=1, b="x", ...}` summary of an object's leading fields
    ///        for the Variables panel row, truncated to stay scannable. Fields
    ///        are described without registration so previews never mint varRefs.
    /// @param base Runtime object whose leading fields are summarized.
    /// @param layout Compiler-provided layout for @p base.
    /// @return Bounded object preview suitable for one debugger row.
    std::string objectPreview(void *base, const DebugClassLayout &layout) {
        constexpr size_t kMaxPreviewFields = 3;
        constexpr size_t kMaxPreviewLen = 80;
        ++previewDepth_;
        std::string out = "{";
        size_t shown = 0;
        for (const auto &field : layout.fields) {
            if (shown == kMaxPreviewFields || out.size() > kMaxPreviewLen)
                break;
            DebugLocalInfo tmp;
            describeField(base, field, tmp, /*allowRegister=*/false);
            if (shown)
                out += ", ";
            out += field.name + "=" + tmp.value;
            ++shown;
        }
        if (shown < layout.fields.size())
            out += shown ? ", ..." : "...";
        out += "}";
        --previewDepth_;
        return out;
    }

    const DebugClassLayoutTable *layouts_ = nullptr; ///< Non-owning class-layout sidecar.
    int previewDepth_ = 0; ///< Recursion guard for nested object previews.
    std::vector<Entry> entries_; ///< One entry per one-based variable reference.
};

/// @brief Collect source-named locals of @p fr (skipping SSA temporaries),
///        loading mutable variables through their frame-stack allocas and showing
///        immutable SSA values directly. Dedups by source name, preferring the
///        alloca-backed (current) value. All memory reads are bounds-checked.
/// @param fr Frame whose source-visible values are collected.
/// @param [out] out Ordered debugger-local records.
/// @param outPtrs Parallel to @p out: the managed runtime handle for each local
///        that is one (else null), so the caller can offer structured expansion.
void collectFrameLocals(const Frame &fr,
                        std::vector<DebugLocalInfo> &out,
                        std::vector<void *> &outPtrs) {
    const Function *fn = fr.func;
    if (!fn)
        return;
    const size_t count = fn->valueNames.size();

    // Per-id static type, and the pointee type of any alloca written by a store.
    std::vector<Type::Kind> kinds(count, Type::Kind::Void);
    std::vector<Type::Kind> allocaPointee(count, Type::Kind::Void);
    std::vector<char> isAlloca(count, 0);
    /// @brief Record a static type kind for a valid SSA value identifier.
    /// @param id Value identifier to update.
    /// @param k Type kind assigned to the value.
    auto setKind = [&](unsigned id, Type::Kind k) {
        if (id < count)
            kinds[id] = k;
    };
    for (const auto &bb : fn->blocks) {
        for (const auto &p : bb.params)
            setKind(p.id, p.type.kind);
        for (const auto &in : bb.instructions) {
            if (in.result)
                setKind(*in.result, in.type.kind);
            if (in.op == Opcode::Store && in.operands.size() >= 2 &&
                in.operands[0].kind == Value::Kind::Temp) {
                const unsigned pid = in.operands[0].id;
                if (pid < count) {
                    allocaPointee[pid] = in.type.kind;
                    isAlloca[pid] = 1;
                }
            }
        }
    }

    // Insertion-ordered dedup by display name; alloca-backed values are authoritative.
    struct OrderedLocal {
        std::string name;
        DebugLocalInfo info;
        void *managed; // runtime object handle when this local is one, else null
    };

    std::vector<OrderedLocal> ordered;
    /// @brief Insert or authoritatively replace one source-visible debugger local.
    /// @param name Display name used for ordered de-duplication.
    /// @param info Formatted local metadata.
    /// @param managed Managed runtime handle, or `nullptr`.
    /// @param authoritative Whether an existing entry may be replaced.
    auto upsert = [&](std::string name, DebugLocalInfo info, void *managed, bool authoritative) {
        for (auto &e : ordered) {
            if (e.name == name) {
                if (authoritative) {
                    e.info = std::move(info);
                    e.managed = managed;
                }
                return;
            }
        }
        ordered.push_back({std::move(name), std::move(info), managed});
    };

    for (size_t id = 0; id < count; ++id) {
        const std::string &name = fn->valueNames[id];
        if (name.empty() || name[0] == '%') // skip unnamed SSA temporaries ("%tN")
            continue;
        if (id >= fr.regs.size())
            continue;

        DebugLocalInfo local;
        const auto dollar = name.find('$'); // strip "$id" shadow-disambiguation suffix
        local.name = dollar == std::string::npos ? name : name.substr(0, dollar);

        bool authoritative = false;
        void *managed = nullptr;
        if (isAlloca[id]) {
            if (loadScalarFromAlloca(fr, fr.regs[id].ptr, allocaPointee[id], local, &managed))
                authoritative = true;
            else
                continue; // string/aggregate alloca or out-of-bounds: skip for now
        } else if (kinds[id] == Type::Kind::Ptr) {
            if (!formatPointerValue(fr.regs[id].ptr, local, false, &managed))
                continue; // raw non-alloca pointer: not user-meaningful
        } else {
            formatRegScalar(fr, id, kinds[id], local);
        }
        std::string displayName = local.name;
        upsert(std::move(displayName), std::move(local), managed, authoritative);
    }

    out.reserve(ordered.size());
    outPtrs.reserve(ordered.size());
    for (auto &e : ordered) {
        out.push_back(std::move(e.info));
        outPtrs.push_back(e.managed);
    }
}

} // namespace

/// @brief Capture an interactive debugger stop snapshot.
/// @details Resolves the stop location, builds a most-recent-first backtrace,
///          collects source-visible locals from the top frame, and attaches a
///          one-stop variable expander for supported containers and objects.
/// @param reason Frontend-neutral reason for the pause.
/// @param loc Source location at the pause boundary.
/// @return Self-contained stop information passed to a debugger frontend.
DebugStopInfo VM::buildStopInfo(std::string_view reason, const il::support::SourceLoc &loc) const {
    DebugStopInfo info;
    info.reason = std::string(reason);
    info.line = loc.line;
    info.column = loc.column;
    const il::support::SourceManager *sm = debug.getSourceManager();
    if (sm && loc.hasFile())
        info.path = std::string(sm->getPath(loc.file_id));

    for (const auto &f : buildBacktrace()) {
        DebugFrameInfo df;
        df.function = f.function;
        df.path = f.file;
        df.line = f.line > 0 ? static_cast<uint32_t>(f.line) : 0;
        info.frames.push_back(std::move(df));
    }

    if (!execStack.empty()) {
        const ExecState *top = execStack.back();
        if (top) {
            std::vector<void *> managed;
            collectFrameLocals(top->fr, info.locals, managed);
            // Offer structured expansion for any local that is an inspectable
            // container. The store owns the varRefs; it lives on the stop info and
            // dies when execution resumes, invalidating those refs (DAP semantics).
            auto store = std::make_shared<VmDebugVarStore>(&debug.classLayouts());
            for (size_t i = 0; i < info.locals.size() && i < managed.size(); ++i) {
                if (managed[i])
                    store->classify(managed[i], info.locals[i]);
            }
            if (!store->empty())
                info.vars = std::move(store);
        }
    }
    return info;
}

/// @brief Hand a pause to the interactive frontend or scripted debugger.
/// @details Interactive frontends receive a rich stop snapshot and choose the
///          next action. Without a frontend, the method prints a legacy break
///          notification, returns a pause sentinel when no scripted action
///          remains, or applies the next queued action.
/// @param st Execution state stopped at the current debug boundary.
/// @param reason Frontend-neutral stop reason.
/// @return Pause sentinel when execution must return to the host, otherwise
///         @c std::nullopt after a next action is installed.
std::optional<Slot> VM::pauseOrAdvanceDebugScript(ExecState &st, std::string_view reason) {
    if (frontend_) {
        il::support::SourceLoc loc{};
        if (st.bb && st.ip < st.bb->instructions.size())
            loc = st.bb->instructions[st.ip].loc;
        applyDebugAction(frontend_->onStop(buildStopInfo(reason, loc)), execStack.size());
        st.skipBreakOnce = true;
        return std::nullopt;
    }
    std::cerr << "[BREAK] fn=@" << st.fr.func->name << " blk=" << st.bb->label
              << " reason=" << reason << "\n";
    if (!script || script->empty()) {
        Slot s{};
        s.i64 = kDebugBreakpointSentinel;
        return s;
    }

    applyDebugAction(script->nextAction(), execStack.size());
    st.skipBreakOnce = true;
    return std::nullopt;
}

/// @brief Apply pending block parameter transfers for the given block.
/// @details Any arguments staged by a predecessor terminator are copied into the
///          frame's register file and announced to the debug controller.  The
///          routine grows the register vector when necessary, materialises a
///          pseudo-instruction so existing store helpers can marshal the value,
///          and releases transient string handles once consumed.  Parameters are
///          cleared after transfer so repeated calls are harmless when no
///          updates remain.
/// @param fr Current frame whose registers receive parameter values.
/// @param bb Basic block that has just become active.
void VM::transferBlockParams(Frame &fr, const BasicBlock &bb) {
    for (const auto &p : bb.params) {
        if (p.id >= fr.params.size() || p.id >= fr.paramsSet.size()) {
            RuntimeBridge::trap(TrapKind::InvalidOperation,
                                "block parameter ID out of range",
                                {},
                                fr.func ? fr.func->name : std::string{},
                                bb.label);
            continue;
        }
        if (!fr.paramsSet[p.id])
            continue;
        if (fr.regs.size() <= p.id)
            fr.regs.resize(p.id + 1);
        if (fr.regIsStr.size() <= p.id)
            fr.regIsStr.resize(p.id + 1, 0);

        Instr pseudo;
        pseudo.result = p.id;
        pseudo.type = p.type;
        detail::ops::storeResult(fr, pseudo, fr.params[p.id]);
        debug.onStore(
            p.name, p.type.kind, fr.regs[p.id].i64, fr.regs[p.id].f64, fr.func->name, bb.label, 0);
        if (p.type.kind == Type::Kind::Str)
            rt_str_release_maybe(fr.params[p.id].str);
        fr.paramsSet[p.id] = 0;
    }
}

/// @brief Manage a potential debug break before or after executing an instruction.
/// @details The helper first considers block-level breakpoints, honouring the
///          single-step skip flag by deferring only the next break opportunity.
///          When a break triggers and no script is present, a synthetic slot is
///          returned to suspend the interpreter; otherwise the current debug
///          script dictates how many instructions to step before resuming.
///          Source line breakpoints are processed when @p in is non-null so the
///          debugger can halt on specific instructions even when control stays
///          within the same block.
/// @param fr            Current frame.
/// @param bb            Current basic block.
/// @param ip            Instruction index within @p bb.
/// @param skipBreakOnce Internal flag used to skip a single break when stepping.
/// @param in            Optional instruction for source line breakpoints.
/// @return @c std::nullopt to continue or a @c Slot signalling a pause.
std::optional<Slot> VM::handleDebugBreak(
    Frame &fr, const BasicBlock &bb, size_t ip, bool &skipBreakOnce, const Instr *in) {
    if (!in) {
        if (debug.shouldBreak(bb)) {
            if (frontend_) {
                il::support::SourceLoc loc{};
                if (ip < bb.instructions.size())
                    loc = bb.instructions[ip].loc;
                applyDebugAction(frontend_->onStop(buildStopInfo("breakpoint", loc)),
                                 execStack.size());
                skipBreakOnce = true;
                return std::nullopt;
            }
            std::cerr << "[BREAK] fn=@" << fr.func->name << " blk=" << bb.label
                      << " reason=label\n";
            if (!script || script->empty()) {
                Slot s{};
                s.i64 = kDebugBreakpointSentinel;
                return s;
            }
            applyDebugAction(script->nextAction(), execStack.size());
            skipBreakOnce = true;
        }
        return std::nullopt;
    }
    if (debug.hasSrcLineBPs() && debug.shouldBreakOn(*in)) {
        if (frontend_) {
            applyDebugAction(frontend_->onStop(buildStopInfo("breakpoint", in->loc)),
                             execStack.size());
            skipBreakOnce = true;
            return std::nullopt;
        }
        const auto *sm = debug.getSourceManager();
        std::string path;
        if (sm && in->loc.hasFile())
            path = std::filesystem::path(sm->getPath(in->loc.file_id)).filename().string();
        std::cerr << "[BREAK] src=" << path;
        if (in->loc.hasLine()) {
            std::cerr << ':' << in->loc.line;
            if (in->loc.hasColumn())
                std::cerr << ':' << in->loc.column;
        }
        std::cerr << " fn=@" << fr.func->name << " blk=" << bb.label << " ip=#" << ip << "\n";
        if (!script || script->empty()) {
            Slot s{};
            s.i64 = kDebugBreakpointSentinel;
            return s;
        }
        applyDebugAction(script->nextAction(), execStack.size());
        skipBreakOnce = true;
    }
    return std::nullopt;
}

/// @brief Handle debugging-related bookkeeping before or after an instruction executes.
/// @details Prior to execution the helper enforces the global step limit,
///          performs parameter transfers when entering a block, and consults
///          @ref handleDebugBreak to honour label and source breakpoints.  After
///          execution it decrements the remaining step budget, halting when it
///          reaches zero and optionally re-arming the debugger script.  Whenever
///          a pause is requested a dedicated slot value is returned so the
///          interpreter loop can unwind gracefully.
/// @param st      Current execution state.
/// @param in      Instruction being processed, if any.
/// @param postExec Set to true when invoked after executing @p in.
/// @return Optional slot causing execution to pause; @c std::nullopt otherwise.
std::optional<Slot> VM::processDebugControl(ExecState &st, const Instr *in, bool postExec) {
    if (!postExec) {
        // Debug mode runs on the dispatch slow path, where the driver's own poll
        // (DISPATCH_AFTER) does not run; drive it here so a poll callback can drain
        // adapter commands and request an interactive Pause.
        if (pollCallback_ && pollEveryN_ && (++debugPollTick_ % pollEveryN_) == 0)
            (void)pollCallback_(*this);

        if (debugPauseRequested_ && frontend_) {
            debugPauseRequested_ = false;
            // Interactive Pause: surface a stop at the current instruction. The
            // frontend handles it like any other stop (emit + wait for a command).
            (void)pauseOrAdvanceDebugScript(st, "pause");
        }
        if (maxSteps && instrCount >= maxSteps) {
            std::cerr << "VM: step limit exceeded (" << maxSteps << "); aborting.\n";
            Slot s{};
            s.i64 = kDebugPauseSentinel;
            return s;
        }
        if (st.ip == 0 && st.bb) {
            transferBlockParams(st.fr, *st.bb);
            // Refresh the pre-resolved operand cache for the newly active block.
            st.blockCache = getOrBuildBlockCache(st.fr.func, st.bb);
        }
        if (st.ip == 0 && stepBudget == 0 && !st.skipBreakOnce) {
            if (auto br = handleDebugBreak(st.fr, *st.bb, st.ip, st.skipBreakOnce, nullptr))
                return br;
        } else if (st.skipBreakOnce) {
            st.skipBreakOnce = false;
        }
        if (in)
            if (auto br = handleDebugBreak(st.fr, *st.bb, st.ip, st.skipBreakOnce, in))
                return br;
        return std::nullopt;
    }
    if (stepBudget > 0) {
        --stepBudget;
        if (stepBudget == 0) {
            if (auto pause = pauseOrAdvanceDebugScript(st, "step"))
                return pause;
            return std::nullopt;
        }
    }
    if (debugStepMode_ != DebugStepMode::None) {
        debugStepArmed_ = true;
        const size_t depth = execStack.size();
        const bool shouldPause =
            (debugStepMode_ == DebugStepMode::StepOver && depth <= debugStepTargetDepth_) ||
            (debugStepMode_ == DebugStepMode::StepOut && depth <= debugStepTargetDepth_);
        if (debugStepArmed_ && shouldPause) {
            const std::string_view reason =
                debugStepMode_ == DebugStepMode::StepOver ? "step-over" : "step-out";
            debugStepMode_ = DebugStepMode::None;
            debugStepArmed_ = false;
            if (auto pause = pauseOrAdvanceDebugScript(st, reason))
                return pause;
        }
    }
    return std::nullopt;
}

} // namespace il::vm
