//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_runtime_class_qualified_surface.cpp
// Purpose: Guard runtime.def class-qualified Graphics3D/Game3D names against
//   drift from the runtime class method/property surface.
// Key invariants:
//   - runtime.def is the single source of truth and is consumed by X-macro
//     expansion, not by text scraping.
//   - Class-qualified runtime functions must resolve to a callable class member with the
//     same public member name.
//   - Direct runtime class leaf names under Zanna.* must not collide unless
//     they are intentionally reused across 2D/3D namespaces.
//   - Instance method signatures omit the leading obj receiver from RT_FUNC.
// Ownership/Lifetime:
//   - Test tables own copied runtime.def string literals for process lifetime.
//   - No runtime objects are allocated.
// Links: src/il/runtime/runtime.def
//
//===----------------------------------------------------------------------===//

#include <algorithm>
#include <cctype>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct SignatureException {
    const char *class_name;
    const char *method_name;
    const char *target_id;
    const char *reason;
};

static const std::vector<SignatureException> kSignatureExceptions = {
    {"Zanna.Graphics3D.Ray3D",
     "IntersectTriangle",
     "Ray3DIntersectTriangle",
     "ADR 0227 geometry namespace: the leading obj is the ray origin, not a receiver."},
    {"Zanna.Graphics3D.Ray3D",
     "IntersectTriangleCull",
     "Ray3DIntersectTriangleCull",
     "ADR 0227 geometry namespace: the leading obj is the ray origin, not a receiver."},
    {"Zanna.Graphics3D.Ray3D",
     "IntersectMesh",
     "Ray3DIntersectMesh",
     "ADR 0227 geometry namespace: the leading obj is the ray origin, not a receiver."},
    {"Zanna.Graphics3D.Ray3D",
     "IntersectAABB",
     "Ray3DIntersectAABB",
     "ADR 0227 geometry namespace: the leading obj is the ray origin, not a receiver."},
    {"Zanna.Graphics3D.Ray3D",
     "IntersectSphere",
     "Ray3DIntersectSphere",
     "ADR 0227 geometry namespace: the leading obj is the ray origin, not a receiver."},
    {"Zanna.Graphics3D.AABB3D",
     "Overlaps",
     "AABB3DOverlaps",
     "ADR 0227 geometry namespace: the leading obj is the first box minimum, not a receiver."},
    {"Zanna.Graphics3D.AABB3D",
     "Penetration",
     "AABB3DPenetration",
     "ADR 0227 geometry namespace: the leading obj is the first box minimum, not a receiver."},
    {"Zanna.Graphics3D.AABB3D",
     "ClosestPoint",
     "AABB3DClosestPoint",
     "ADR 0227 geometry namespace: the leading obj is the box minimum, not a receiver."},
    {"Zanna.Graphics3D.AABB3D",
     "SphereOverlaps",
     "AABB3DSphereOverlaps",
     "ADR 0227 geometry namespace: the leading obj is the box minimum, not a receiver."},
    {"Zanna.Graphics3D.Sphere3D",
     "Overlaps",
     "Sphere3DOverlaps",
     "ADR 0227 geometry namespace: the leading obj is the first sphere center, not a receiver."},
    {"Zanna.Graphics3D.Sphere3D",
     "Penetration",
     "Sphere3DPenetration",
     "ADR 0227 geometry namespace: the leading obj is the first sphere center, not a receiver."},
    {"Zanna.Graphics3D.Segment3D",
     "ClosestPoint",
     "Segment3DClosestPoint",
     "ADR 0227 geometry namespace: the leading obj is the segment start, not a receiver."},
    {"Zanna.Graphics3D.Capsule3D",
     "SphereOverlaps",
     "Capsule3DSphereOverlaps",
     "ADR 0227 geometry namespace: the leading obj is the capsule base, not a receiver."},
    {"Zanna.Graphics3D.Capsule3D",
     "AABBOverlaps",
     "Capsule3DAABBOverlaps",
     "ADR 0227 geometry namespace: the leading obj is the capsule base, not a receiver."},
    {"Zanna.Graphics3D.IKSolver3D",
     "TwoBone",
     "IKSolver3DTwoBone",
     "Static IK-solver factory; the leading obj is the skeleton input, not a receiver."},
    {"Zanna.Graphics3D.IKSolver3D",
     "LookAt",
     "IKSolver3DLookAt",
     "Static IK-solver factory; the leading obj is the skeleton input, not a receiver."},
    {"Zanna.Graphics3D.IKSolver3D",
     "FABRIK",
     "IKSolver3DFABRIK",
     "Static IK-solver factory; the leading obj is the chain input, not a receiver."},
    {"Zanna.Graphics3D.Material3D",
     "Textured",
     "Material3DTextured",
     "Static factory alias; the leading obj is the texture input, not a receiver."},
    {"Zanna.Graphics3D.Light3D",
     "Directional",
     "Light3DDirectional",
     "Static factory alias; the leading obj is the direction input, not a receiver."},
    {"Zanna.Graphics3D.Light3D",
     "Point",
     "Light3DPoint",
     "Static factory alias; the leading obj is the position input, not a receiver."},
    {"Zanna.Graphics3D.Light3D",
     "Spot",
     "Light3DSpot",
     "Static factory alias; the leading object parameters are direction and position inputs."},
    {"Zanna.Graphics3D.Light3D",
     "AreaRectangle",
     "Light3DAreaRectangle",
     "Static factory alias; the leading object parameters are position and direction inputs."},
    {"Zanna.Graphics3D.Light3D",
     "AreaSphere",
     "Light3DAreaSphere",
     "Static factory alias; the leading obj is the position input, not a receiver."},
    {"Zanna.Graphics3D.Light3D",
     "Volume",
     "Light3DVolume",
     "Static factory alias; the leading obj is the position input, not a receiver."},
};

struct RuntimeFunc {
    std::string id;
    std::string c_symbol;
    std::string canonical;
    std::string signature;
    bool public_surface;
};

struct RuntimeProp {
    std::string name;
    std::string type;
    std::string getter_id;
    std::string setter_id;
};

struct RuntimeMethod {
    std::string name;
    std::string signature;
    std::string target_id;
};

struct RuntimeClass {
    std::string name;
    std::string type_id;
    std::string layout;
    std::string ctor_id;
    std::vector<RuntimeProp> props;
    std::vector<RuntimeMethod> methods;
};

struct RuntimeSurface {
    std::vector<RuntimeFunc> funcs;
    std::vector<RuntimeClass> classes;
    RuntimeClass *current_class = nullptr;

    void add_func(const char *id,
                  const char *c_symbol,
                  const char *canonical,
                  const char *signature,
                  bool public_surface = true) {
        funcs.push_back({id, c_symbol, canonical, signature, public_surface});
    }

    void begin_class(const char *name,
                     const char *type_id,
                     const char *layout,
                     const char *ctor_id) {
        classes.push_back({name, type_id, layout, ctor_id, {}, {}});
        current_class = &classes.back();
    }

    void add_prop(const char *name,
                  const char *type,
                  const char *getter_id,
                  const char *setter_id) {
        if (current_class)
            current_class->props.push_back({name, type, getter_id, setter_id});
    }

    void add_method(const char *name, const char *signature, const char *target_id) {
        if (current_class)
            current_class->methods.push_back({name, signature, target_id});
    }

    void end_class() {
        current_class = nullptr;
    }
};

RuntimeSurface load_runtime_surface() {
    RuntimeSurface surface;

#define RT_FUNC(id, c_symbol, canonical, signature, ...)                                           \
    surface.add_func(#id, #c_symbol, canonical, signature);
#define RT_INTERNAL_FUNC(id, c_symbol, canonical, signature, ...)                                  \
    surface.add_func(#id, #c_symbol, canonical, signature, false);
#define RT_BRIDGE(target_id, roles)
#define RT_CLASS_BEGIN(name, type_id, layout, ctor_id, ...)                                        \
    surface.begin_class(name, #type_id, layout, #ctor_id);
#define RT_PROP(name, type, getter_id, setter_id)                                                  \
    surface.add_prop(name, type, #getter_id, #setter_id);
#define RT_METHOD(name, signature, target_id) surface.add_method(name, signature, #target_id);
#define RT_CLASS_END() surface.end_class();
#include "il/runtime/runtime.def"
#undef RT_CLASS_END
#undef RT_METHOD
#undef RT_PROP
#undef RT_CLASS_BEGIN
#undef RT_BRIDGE
#undef RT_INTERNAL_FUNC
#undef RT_FUNC

    return surface;
}

bool starts_with(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool is_scoped_class(std::string_view class_name) {
    return starts_with(class_name, "Zanna.Graphics3D.") || starts_with(class_name, "Zanna.Game3D.");
}

std::string lower_ascii(std::string_view value) {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

std::vector<std::string> split_signature_args(std::string_view args) {
    std::vector<std::string> out;
    std::string current;
    int angle_depth = 0;
    for (char c : args) {
        if (c == '<') {
            ++angle_depth;
            current.push_back(c);
        } else if (c == '>') {
            --angle_depth;
            current.push_back(c);
        } else if (c == ',' && angle_depth == 0) {
            out.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty())
        out.push_back(current);
    return out;
}

std::string join_signature(std::string_view ret, const std::vector<std::string> &args) {
    std::ostringstream out;
    out << ret << "(";
    for (size_t i = 0; i < args.size(); ++i) {
        if (i)
            out << ",";
        out << args[i];
    }
    out << ")";
    return out.str();
}

bool is_object_token(std::string_view token) {
    return token == "obj" || starts_with(token, "obj<");
}

std::string normalize_object_token(const std::string &token) {
    return is_object_token(token) ? std::string("obj") : token;
}

std::string normalized_signature(std::string_view signature) {
    size_t open = signature.find('(');
    size_t close = signature.rfind(')');
    if (open == std::string_view::npos || close == std::string_view::npos || close < open)
        return std::string(signature);

    std::string ret = normalize_object_token(std::string(signature.substr(0, open)));
    std::vector<std::string> args =
        split_signature_args(signature.substr(open + 1, close - open - 1));
    for (std::string &arg : args)
        arg = normalize_object_token(arg);
    return join_signature(ret, args);
}

std::string signature_return_type(std::string_view signature) {
    size_t open = signature.find('(');
    return std::string(open == std::string_view::npos ? signature : signature.substr(0, open));
}

std::string object_type_name(std::string_view token) {
    constexpr std::string_view prefix = "obj<";
    if (!starts_with(token, prefix) || token.size() <= prefix.size() || token.back() != '>')
        return {};
    return std::string(token.substr(prefix.size(), token.size() - prefix.size() - 1));
}

bool returns_own_runtime_object(const RuntimeClass &runtime_class, const RuntimeFunc &func) {
    std::string ret = signature_return_type(func.signature);
    return ret == "obj" || object_type_name(ret) == runtime_class.name;
}

std::string leaf_name(std::string_view canonical) {
    size_t dot = canonical.rfind('.');
    return std::string(dot == std::string_view::npos ? canonical : canonical.substr(dot + 1));
}

bool starts_with_bool_prefix(std::string_view name, std::string_view prefix) {
    if (!starts_with(name, prefix))
        return false;
    if (name.size() == prefix.size())
        return true;
    return std::isupper(static_cast<unsigned char>(name[prefix.size()])) != 0;
}

bool is_boolean_probe_name(std::string_view name) {
    return starts_with_bool_prefix(name, "Is") || starts_with_bool_prefix(name, "Has") ||
           starts_with_bool_prefix(name, "Can") || starts_with_bool_prefix(name, "Was") ||
           starts_with_bool_prefix(name, "get_Is") || starts_with_bool_prefix(name, "get_Has") ||
           starts_with_bool_prefix(name, "get_Can");
}

bool is_boolean_setter_name(std::string_view name) {
    static const std::set<std::string_view> names = {
        "SetAltScreen",
        "SetAutoClose",
        "SetAutoFoldDetection",
        "SetAutoScroll",
        "SetCaseSensitive",
        "SetCheckable",
        "SetChecked",
        "SetCursorVisible",
        "SetDraggable",
        "SetDropTarget",
        "SetEnabled",
        "SetFocused",
        "SetGlobalEnabled",
        "SetIndeterminate",
        "SetInsertSpaces",
        "SetIntegerScaling",
        "SetLoop",
        "SetLoopable",
        "SetModified",
        "SetMultiSelect",
        "SetMultiple",
        "SetReadOnly",
        "SetRegex",
        "SetReplaceMode",
        "SetShowFoldGutter",
        "SetShowIndentGuides",
        "SetShowLineNumbers",
        "SetShowSlider",
        "SetTerminalMode",
        "SetToggled",
        "SetVisible",
        "SetWholeWord",
        "SetWordWrap",
    };
    return names.count(name) != 0;
}

std::string signature_last_arg(std::string_view signature) {
    size_t open = signature.find('(');
    size_t close = signature.rfind(')');
    if (open == std::string_view::npos || close == std::string_view::npos || close < open)
        return {};
    std::vector<std::string> args =
        split_signature_args(signature.substr(open + 1, close - open - 1));
    return args.empty() ? std::string{} : args.back();
}

bool is_static_factory_method(const RuntimeClass &runtime_class, const RuntimeMethod &method) {
    return runtime_class.ctor_id == method.target_id || starts_with(method.name, "New") ||
           starts_with(method.name, "From") || method.name == "Of";
}

std::string method_signature_for_target(const RuntimeClass &runtime_class,
                                        const RuntimeMethod &method,
                                        std::string_view target_signature) {
    size_t open = target_signature.find('(');
    size_t close = target_signature.rfind(')');
    if (open == std::string_view::npos || close == std::string_view::npos || close < open)
        return std::string(target_signature);

    std::string_view ret = target_signature.substr(0, open);
    std::vector<std::string> args =
        split_signature_args(target_signature.substr(open + 1, close - open - 1));
    if (!args.empty() && args.front() == "obj" &&
        !is_static_factory_method(runtime_class, method)) {
        args.erase(args.begin());
    }
    return join_signature(ret, args);
}

bool signature_exception_applies(std::string_view class_name,
                                 std::string_view method_name,
                                 std::string_view target_id) {
    for (const SignatureException &exception : kSignatureExceptions) {
        if (class_name == exception.class_name && method_name == exception.method_name &&
            target_id == exception.target_id) {
            return true;
        }
    }
    return false;
}

struct RuntimeIndex {
    std::map<std::string, const RuntimeFunc *> funcs_by_id;
    std::map<std::string, const RuntimeFunc *> funcs_by_canonical;
    std::map<std::string, const RuntimeClass *> classes_by_name;
};

RuntimeIndex build_index(const RuntimeSurface &surface) {
    RuntimeIndex index;
    for (const RuntimeFunc &func : surface.funcs) {
        index.funcs_by_id.emplace(func.id, &func);
        index.funcs_by_canonical.emplace(func.canonical, &func);
    }
    for (const RuntimeClass &runtime_class : surface.classes)
        index.classes_by_name.emplace(runtime_class.name, &runtime_class);
    return index;
}

const RuntimeFunc *resolve_func(const RuntimeIndex &index, const std::string &id_or_canonical) {
    if (id_or_canonical.empty() || id_or_canonical == "none")
        return nullptr;

    auto id_it = index.funcs_by_id.find(id_or_canonical);
    if (id_it != index.funcs_by_id.end())
        return id_it->second;

    auto canonical_it = index.funcs_by_canonical.find(id_or_canonical);
    if (canonical_it != index.funcs_by_canonical.end())
        return canonical_it->second;

    return nullptr;
}

const RuntimeClass *find_owner_class(const RuntimeIndex &index, std::string_view canonical) {
    const RuntimeClass *best = nullptr;
    for (const auto &entry : index.classes_by_name) {
        const RuntimeClass *runtime_class = entry.second;
        if (!is_scoped_class(runtime_class->name))
            continue;
        std::string prefix = runtime_class->name + ".";
        if (starts_with(canonical, prefix) &&
            (!best || runtime_class->name.size() > best->name.size())) {
            best = runtime_class;
        }
    }
    return best;
}

void add_reachable(std::map<std::string, std::set<std::string>> &reachable,
                   const std::string &canonical,
                   const RuntimeFunc *target) {
    if (target)
        reachable[canonical].insert(target->id);
}

struct ReachableSurface {
    std::map<std::string, std::set<std::string>> reachable;
    std::map<std::string, std::set<std::string>> explicit_methods;
};

ReachableSurface build_reachable_names(const RuntimeSurface &surface, const RuntimeIndex &index) {
    ReachableSurface out;
    for (const RuntimeClass &runtime_class : surface.classes) {
        if (!is_scoped_class(runtime_class.name))
            continue;

        const RuntimeFunc *ctor = resolve_func(index, runtime_class.ctor_id);
        if (ctor)
            add_reachable(out.reachable, ctor->canonical, ctor);

        for (const RuntimeProp &prop : runtime_class.props) {
            const RuntimeFunc *getter = resolve_func(index, prop.getter_id);
            const RuntimeFunc *setter = resolve_func(index, prop.setter_id);
            if (getter)
                add_reachable(out.reachable, getter->canonical, getter);
            if (setter)
                add_reachable(out.reachable, setter->canonical, setter);
        }

        for (const RuntimeMethod &method : runtime_class.methods) {
            const RuntimeFunc *target = resolve_func(index, method.target_id);
            add_reachable(out.reachable, runtime_class.name + "." + method.name, target);
            if (target) {
                out.explicit_methods[runtime_class.name + "." + method.name].insert(target->id);
            }
        }
    }
    return out;
}

bool is_reachable(const ReachableSurface &reachable,
                  const std::string &canonical,
                  const RuntimeFunc *target) {
    auto it = reachable.reachable.find(canonical);
    return target && it != reachable.reachable.end() && it->second.count(target->id) != 0;
}

bool is_explicit_method(const ReachableSurface &reachable,
                        const std::string &canonical,
                        const RuntimeFunc *target) {
    auto it = reachable.explicit_methods.find(canonical);
    return target && it != reachable.explicit_methods.end() && it->second.count(target->id) != 0;
}

bool check_coverage(const RuntimeSurface &surface, const RuntimeIndex &index) {
    ReachableSurface reachable = build_reachable_names(surface, index);
    std::vector<std::string> failures;

    for (const RuntimeFunc &func : surface.funcs) {
        const RuntimeClass *owner = find_owner_class(index, func.canonical);
        if (!owner)
            continue;
        if (!is_reachable(reachable, func.canonical, &func)) {
            failures.push_back(func.canonical + " -> " + func.id);
        }
    }

    if (failures.empty())
        return true;

    std::cerr << "Class-qualified runtime names are not reachable through RT_CLASS members:\n";
    for (const std::string &failure : failures)
        std::cerr << "  " << failure << "\n";
    return false;
}

bool has_getter_name(const RuntimeClass &runtime_class,
                     const RuntimeIndex &index,
                     const RuntimeProp &prop) {
    if (prop.getter_id == "none")
        return true;
    const RuntimeFunc *getter = resolve_func(index, prop.getter_id);
    return getter && getter->canonical == runtime_class.name + ".get_" + prop.name;
}

bool has_setter_name(const RuntimeClass &runtime_class,
                     const RuntimeIndex &index,
                     const RuntimeProp &prop) {
    if (prop.setter_id == "none")
        return true;

    const RuntimeFunc *setter = resolve_func(index, prop.setter_id);
    return setter && setter->canonical == runtime_class.name + ".set_" + prop.name;
}

bool check_naming_symmetry(const RuntimeSurface &surface, const RuntimeIndex &index) {
    std::vector<std::string> failures;
    for (const RuntimeClass &runtime_class : surface.classes) {
        if (!is_scoped_class(runtime_class.name))
            continue;
        for (const RuntimeProp &prop : runtime_class.props) {
            if (!has_getter_name(runtime_class, index, prop))
                failures.push_back(runtime_class.name + "." + prop.name +
                                   " is missing qualified getter " + runtime_class.name + ".get_" +
                                   prop.name);
            if (!has_setter_name(runtime_class, index, prop))
                failures.push_back(runtime_class.name + "." + prop.name +
                                   " is missing qualified setter " + runtime_class.name + ".set_" +
                                   prop.name);
        }
    }

    if (failures.empty())
        return true;

    std::cerr << "Runtime property naming symmetry violations:\n";
    for (const std::string &failure : failures)
        std::cerr << "  " << failure << "\n";
    return false;
}

bool check_method_signatures(const RuntimeSurface &surface, const RuntimeIndex &index) {
    std::vector<std::string> failures;
    for (const RuntimeClass &runtime_class : surface.classes) {
        if (!is_scoped_class(runtime_class.name))
            continue;
        for (const RuntimeMethod &method : runtime_class.methods) {
            const RuntimeFunc *target = resolve_func(index, method.target_id);
            if (!target) {
                failures.push_back(runtime_class.name + "." + method.name +
                                   " targets missing runtime function " + method.target_id);
                continue;
            }
            if (signature_exception_applies(runtime_class.name, method.name, method.target_id))
                continue;

            std::string expected =
                method_signature_for_target(runtime_class, method, target->signature);
            bool matches = normalized_signature(method.signature) == normalized_signature(expected);
            if (!matches && runtime_class.layout == "none") {
                matches = normalized_signature(method.signature) ==
                          normalized_signature(target->signature);
            }
            if (!matches) {
                failures.push_back(runtime_class.name + "." + method.name + " targets " +
                                   method.target_id + ": method signature " + method.signature +
                                   " should be " + expected + " from RT_FUNC " + target->signature);
            }
        }
    }

    if (failures.empty())
        return true;

    std::cerr << "Runtime method signature violations:\n";
    for (const std::string &failure : failures)
        std::cerr << "  " << failure << "\n";
    return false;
}

std::string direct_leaf_name(std::string_view class_name) {
    constexpr std::string_view kZannaPrefix = "Zanna.";
    if (!starts_with(class_name, kZannaPrefix))
        return {};

    std::string_view tail = class_name.substr(kZannaPrefix.size());
    size_t dot = tail.rfind('.');
    if (dot == std::string_view::npos)
        return {};
    return std::string(tail.substr(dot + 1));
}

bool check_runtime_class_leaf_names(const RuntimeSurface &surface) {
    std::map<std::string, std::set<std::string>> owners_by_leaf;

    for (const RuntimeClass &runtime_class : surface.classes) {
        std::string leaf = direct_leaf_name(runtime_class.name);
        if (!leaf.empty())
            owners_by_leaf[leaf].insert(runtime_class.name);
    }

    std::vector<std::string> failures;
    const std::set<std::string> allowed_dimensional_reuse = {
        "Aes",
        "GC",
        "Hash",
        "SceneGraph",
        "SceneNode",
    };

    for (const auto &entry : owners_by_leaf) {
        if (entry.second.size() > 1 && allowed_dimensional_reuse.count(entry.first) == 0) {
            std::ostringstream line;
            line << entry.first << " is exported by";
            for (const std::string &owner : entry.second)
                line << " " << owner;
            failures.push_back(line.str());
        }
    }

    if (failures.empty())
        return true;

    std::cerr << "Runtime class leaf-name collisions:\n";
    for (const std::string &failure : failures)
        std::cerr << "  " << failure << "\n";
    return false;
}

bool check_property_method_collisions(const RuntimeSurface &surface) {
    std::vector<std::string> failures;
    for (const RuntimeClass &runtime_class : surface.classes) {
        std::map<std::string, std::string> props_by_lower;
        for (const RuntimeProp &prop : runtime_class.props)
            props_by_lower.emplace(lower_ascii(prop.name), prop.name);

        for (const RuntimeMethod &method : runtime_class.methods) {
            auto prop_it = props_by_lower.find(lower_ascii(method.name));
            if (prop_it != props_by_lower.end()) {
                failures.push_back(runtime_class.name + "." + prop_it->second +
                                   " collides with method " + method.name);
            }
        }
    }

    if (failures.empty())
        return true;

    std::cerr << "Runtime property/method name collisions:\n";
    for (const std::string &failure : failures)
        std::cerr << "  " << failure << "\n";
    return false;
}

bool check_redundant_property_accessor_methods(const RuntimeSurface &surface) {
    std::vector<std::string> failures;
    for (const RuntimeClass &runtime_class : surface.classes) {
        for (const RuntimeProp &prop : runtime_class.props) {
            for (const RuntimeMethod &method : runtime_class.methods) {
                if (prop.getter_id != "none" && method.target_id == prop.getter_id &&
                    method.name == "Get" + prop.name) {
                    failures.push_back(runtime_class.name + "." + method.name +
                                       " duplicates property getter " + prop.name);
                }
                if (prop.setter_id != "none" && method.name == "Set" + prop.name) {
                    failures.push_back(runtime_class.name + "." + method.name +
                                       " duplicates writable property " + prop.name);
                }
            }
        }
    }

    if (failures.empty())
        return true;

    std::cerr << "Redundant runtime property accessor methods:\n";
    for (const std::string &failure : failures)
        std::cerr << "  " << failure << "\n";
    return false;
}

bool check_property_accessor_types(const RuntimeSurface &surface, const RuntimeIndex &index) {
    std::vector<std::string> failures;
    for (const RuntimeClass &runtime_class : surface.classes) {
        for (const RuntimeProp &prop : runtime_class.props) {
            const RuntimeFunc *getter = resolve_func(index, prop.getter_id);
            if (getter) {
                size_t open = getter->signature.find('(');
                std::string getter_ret = open == std::string::npos
                                             ? getter->signature
                                             : getter->signature.substr(0, open);
                if (getter_ret != prop.type) {
                    failures.push_back(runtime_class.name + "." + prop.name + " getter returns " +
                                       getter_ret + " but property type is " + prop.type);
                }
            }

            const RuntimeFunc *setter = resolve_func(index, prop.setter_id);
            if (setter) {
                size_t open = setter->signature.find('(');
                size_t close = setter->signature.rfind(')');
                std::vector<std::string> args;
                std::string setter_ret = setter->signature;
                if (open != std::string::npos && close != std::string::npos && close > open) {
                    setter_ret = setter->signature.substr(0, open);
                    args = split_signature_args(
                        std::string_view(setter->signature).substr(open + 1, close - open - 1));
                }
                if (setter_ret != "void" || args.empty() || args.back() != prop.type) {
                    failures.push_back(runtime_class.name + "." + prop.name + " setter signature " +
                                       setter->signature + " does not set property type " +
                                       prop.type);
                }
            }
        }
    }

    if (failures.empty())
        return true;

    std::cerr << "Runtime property accessor type violations:\n";
    for (const std::string &failure : failures)
        std::cerr << "  " << failure << "\n";
    return false;
}

std::string duplicate_symbol(std::string_view key) {
    size_t pipe = key.find('|');
    return std::string(pipe == std::string_view::npos ? key : key.substr(0, pipe));
}

bool all_names_start_with_one_of(const std::vector<std::string> &names,
                                 const std::vector<std::string_view> &prefixes) {
    for (const std::string &name : names) {
        bool matched = false;
        for (std::string_view prefix : prefixes) {
            if (starts_with(name, prefix)) {
                matched = true;
                break;
            }
        }
        if (!matched)
            return false;
    }
    return true;
}

bool any_name_starts_with(const std::vector<std::string> &names, std::string_view prefix) {
    for (const std::string &name : names) {
        if (starts_with(name, prefix))
            return true;
    }
    return false;
}

bool is_allowed_duplicate_function_export(std::string_view key,
                                          const std::vector<std::string> &names) {
    const std::string symbol = duplicate_symbol(key);

    if (starts_with(symbol, "rt_keyboard_key_")) {
        return all_names_start_with_one_of(names,
                                           {"Zanna.Input.Keyboard.get_", "Zanna.Input.Key.get_"});
    }

    if (starts_with(symbol, "rt_hash_") || starts_with(symbol, "rt_aes_")) {
        return any_name_starts_with(names, "Zanna.Crypto.Legacy.") &&
               all_names_start_with_one_of(names,
                                           {"Zanna.Crypto.Hash.",
                                            "Zanna.Crypto.Aes.",
                                            "Zanna.Crypto.Legacy.Hash.",
                                            "Zanna.Crypto.Legacy.Aes."});
    }

    if (starts_with(symbol, "rt_gc_")) {
        return all_names_start_with_one_of(names, {"Zanna.Runtime.GC.", "Zanna.Runtime.GC."});
    }

    if (starts_with(symbol, "rt_memory_")) {
        return all_names_start_with_one_of(names, {"Zanna.Memory.", "Zanna.Runtime.Unsafe."});
    }

    if (starts_with(symbol, "rt_throw_msg_") || starts_with(symbol, "rt_trap_")) {
        return all_names_start_with_one_of(names, {"Zanna.Error.", "Zanna.Runtime.Unsafe."});
    }

    if (starts_with(symbol, "rt_game3d_assets_load_model_template") ||
        symbol == "rt_game3d_asset_handle_get_template") {
        return all_names_start_with_one_of(
            names,
            {"Zanna.Game3D.Prefab.", "Zanna.Game3D.Assets3D.", "Zanna.Game3D.AssetHandle3D."});
    }

    if (starts_with(symbol, "rt_game3d_world_new_with")) {
        return all_names_start_with_one_of(names, {"Zanna.Game3D.World3D."});
    }

    if (starts_with(symbol, "rt_mesh3d_new_")) {
        return all_names_start_with_one_of(names, {"Zanna.Graphics3D.Mesh3D."});
    }

    if (starts_with(symbol, "rt_collider3d_new_")) {
        return all_names_start_with_one_of(names, {"Zanna.Graphics3D.Collider3D."});
    }

    if (starts_with(symbol, "rt_light3d_new_")) {
        return all_names_start_with_one_of(names, {"Zanna.Graphics3D.Light3D."});
    }

    if (starts_with(symbol, "rt_material3d_new_")) {
        return all_names_start_with_one_of(names, {"Zanna.Graphics3D.Material3D."});
    }

    static const std::set<std::string_view> allowed_symbols = {
        "rt_bimap_put",
        "rt_binbuf_new_cap",
        "rt_bits_leadz",
        "rt_bits_rotl",
        "rt_bits_rotr",
        "rt_bits_trailz",
        "rt_bits_ushr",
        "rt_bloomfilter_fpr",
        "rt_box_value_type",
        "rt_box_value_type_add_field",
        "rt_canvas3d_set_dt_max",
        "rt_canvas_set_dt_max",
        "rt_channel_get_cap",
        "rt_crypto_module_disable_approved_mode",
        "rt_crypto_module_enable_approved_mode",
        "rt_crypto_module_is_approved_mode_zanna",
        "rt_deque_cap",
        "rt_f64_to_str",
        "rt_fmt_bool_yn",
        "rt_fmt_num_pct",
        "rt_fmt_num_sci",
        "rt_int_to_str",
        "rt_lrucache_cap",
        "rt_lrucache_put",
        "rt_multimap_put",
        "rt_parse_double_option",
        "rt_parse_num_or",
        "rt_ring_cap",
        "rt_seq_cap",
    };
    return allowed_symbols.count(symbol) != 0;
}

bool check_duplicate_function_symbol_signatures(const RuntimeSurface &surface) {
    std::map<std::string, std::vector<std::string>> names_by_symbol_signature;
    for (const RuntimeFunc &func : surface.funcs) {
        if (!func.public_surface)
            continue;
        names_by_symbol_signature[func.c_symbol + "|" + func.signature].push_back(func.canonical);
    }

    std::vector<std::string> failures;
    for (const auto &entry : names_by_symbol_signature) {
        if (entry.second.size() <= 1)
            continue;
        if (is_allowed_duplicate_function_export(entry.first, entry.second))
            continue;
        std::ostringstream line;
        line << entry.first << " exported as";
        for (const std::string &canonical : entry.second)
            line << " " << canonical;
        failures.push_back(line.str());
    }

    if (failures.empty())
        return true;

    std::cerr << "Duplicate runtime function symbol/signature exports:\n";
    for (const std::string &failure : failures)
        std::cerr << "  " << failure << "\n";
    return false;
}

bool check_boolean_probe_signatures(const RuntimeSurface &surface) {
    std::vector<std::string> failures;

    for (const RuntimeFunc &func : surface.funcs) {
        if (!func.public_surface)
            continue;
        const std::string leaf = leaf_name(func.canonical);
        if (is_boolean_probe_name(leaf) && signature_return_type(func.signature) != "i1")
            failures.push_back(func.canonical + " has non-i1 signature " + func.signature);
    }

    for (const RuntimeClass &runtime_class : surface.classes) {
        for (const RuntimeMethod &method : runtime_class.methods) {
            if (is_boolean_probe_name(method.name) &&
                signature_return_type(method.signature) != "i1") {
                failures.push_back(runtime_class.name + "." + method.name +
                                   " has non-i1 signature " + method.signature);
            }
        }
    }

    if (failures.empty())
        return true;

    std::cerr << "Boolean runtime probe signatures must return i1:\n";
    for (const std::string &failure : failures)
        std::cerr << "  " << failure << "\n";
    return false;
}

bool check_boolean_setter_signatures(const RuntimeSurface &surface) {
    std::vector<std::string> failures;

    for (const RuntimeFunc &func : surface.funcs) {
        if (!func.public_surface)
            continue;
        const std::string leaf = leaf_name(func.canonical);
        if (is_boolean_setter_name(leaf) && signature_last_arg(func.signature) != "i1")
            failures.push_back(func.canonical + " has non-i1 signature " + func.signature);
    }

    for (const RuntimeClass &runtime_class : surface.classes) {
        for (const RuntimeMethod &method : runtime_class.methods) {
            if (is_boolean_setter_name(method.name) &&
                signature_last_arg(method.signature) != "i1") {
                failures.push_back(runtime_class.name + "." + method.name +
                                   " has non-i1 signature " + method.signature);
            }
        }
    }

    if (failures.empty())
        return true;

    std::cerr << "Boolean runtime setter signatures must use i1:\n";
    for (const std::string &failure : failures)
        std::cerr << "  " << failure << "\n";
    return false;
}

bool check_no_copied_gui_widget_methods(const RuntimeSurface &surface, const RuntimeIndex &index) {
    std::vector<std::string> failures;

    for (const RuntimeClass &runtime_class : surface.classes) {
        if (!starts_with(runtime_class.name, "Zanna.GUI.") ||
            runtime_class.name == "Zanna.GUI.Widget") {
            continue;
        }

        for (const RuntimeMethod &method : runtime_class.methods) {
            const RuntimeFunc *target = resolve_func(index, method.target_id);
            if (target && starts_with(target->canonical, "Zanna.GUI.Widget.")) {
                failures.push_back(runtime_class.name + "." + method.name +
                                   " copies base widget target " + target->canonical);
            }
        }
    }

    if (failures.empty())
        return true;

    std::cerr << "Concrete GUI classes must not copy Zanna.GUI.Widget methods:\n";
    for (const std::string &failure : failures)
        std::cerr << "  " << failure << "\n";
    return false;
}

bool check_self_returning_new_methods_have_constructor_metadata(const RuntimeSurface &surface,
                                                                const RuntimeIndex &index) {
    std::vector<std::string> failures;

    for (const RuntimeClass &runtime_class : surface.classes) {
        for (const RuntimeMethod &method : runtime_class.methods) {
            if (method.name != "New")
                continue;

            const RuntimeFunc *target = resolve_func(index, method.target_id);
            if (!target || !returns_own_runtime_object(runtime_class, *target))
                continue;

            const RuntimeFunc *ctor = resolve_func(index, runtime_class.ctor_id);
            if (!ctor || ctor->canonical != target->canonical) {
                failures.push_back(runtime_class.name + ".New returns " + target->canonical +
                                   " but constructor metadata is " + runtime_class.ctor_id);
            }
        }
    }

    if (failures.empty())
        return true;

    std::cerr << "Self-returning New methods must be class constructor metadata:\n";
    for (const std::string &failure : failures)
        std::cerr << "  " << failure << "\n";
    return false;
}

bool check_constructor_targets_are_canonical_new(const RuntimeSurface &surface,
                                                 const RuntimeIndex &index) {
    std::vector<std::string> failures;

    for (const RuntimeClass &runtime_class : surface.classes) {
        const RuntimeFunc *ctor = resolve_func(index, runtime_class.ctor_id);
        if (!ctor)
            continue;

        std::string expected = runtime_class.name + ".New";
        if (ctor->canonical != expected) {
            failures.push_back(runtime_class.name + " constructor target is " + ctor->canonical +
                               ", expected " + expected + " or no constructor metadata");
        }
    }

    if (failures.empty())
        return true;

    std::cerr << "Runtime constructor metadata must target canonical .New functions:\n";
    for (const std::string &failure : failures)
        std::cerr << "  " << failure << "\n";
    return false;
}

bool is_allowed_length_property(std::string_view qualified_name) {
    static const std::set<std::string_view> allowed = {
        "Zanna.Collections.BitSet.Length",
        "Zanna.Collections.Bytes.Length",
        "Zanna.Collections.F64Buffer.Length",
        "Zanna.Collections.I64Buffer.Length",
        "Zanna.Game.Physics2D.DistanceJoint.Length",
        "Zanna.Graphics3D.Path3D.Length",
        "Zanna.IO.BinaryBuffer.Length",
        "Zanna.IO.MemStream.Length",
        "Zanna.IO.Stream.Length",
        "Zanna.Audio.MusicGen.Length",
        "Zanna.String.Length",
        "Zanna.Text.Scanner.Length",
        "Zanna.Text.StringBuilder.Length",
    };
    return allowed.count(qualified_name) != 0;
}

bool is_legacy_length_alias(std::string_view qualified_name) {
    static const std::set<std::string_view> allowed = {
        "Zanna.Game.PathResult.Length",
    };
    return allowed.count(qualified_name) != 0;
}

bool check_length_properties_are_semantic_lengths(const RuntimeSurface &surface) {
    std::vector<std::string> failures;

    for (const RuntimeClass &runtime_class : surface.classes) {
        for (const RuntimeProp &prop : runtime_class.props) {
            if (prop.name != "Length")
                continue;

            std::string qualified = runtime_class.name + "." + prop.name;
            if (!is_allowed_length_property(qualified) && !is_legacy_length_alias(qualified))
                failures.push_back(qualified);
        }
    }

    for (const RuntimeFunc &func : surface.funcs) {
        constexpr std::string_view suffix = ".get_Length";
        if (!func.public_surface || !starts_with(func.canonical, "Zanna.") ||
            func.canonical.size() < suffix.size() ||
            func.canonical.substr(func.canonical.size() - suffix.size()) != suffix) {
            continue;
        }

        std::string qualified =
            func.canonical.substr(0, func.canonical.size() - suffix.size()) + ".Length";
        if (!is_allowed_length_property(qualified) && !is_legacy_length_alias(qualified))
            failures.push_back(func.canonical);
    }

    if (failures.empty())
        return true;

    std::cerr << "Runtime Length properties must represent semantic length, not item count:\n";
    for (const std::string &failure : failures)
        std::cerr << "  " << failure << "\n";
    return false;
}

} // namespace

int main() {
    RuntimeSurface surface = load_runtime_surface();
    RuntimeIndex index = build_index(surface);

    bool ok = true;
    ok = check_coverage(surface, index) && ok;
    ok = check_naming_symmetry(surface, index) && ok;
    ok = check_method_signatures(surface, index) && ok;
    ok = check_runtime_class_leaf_names(surface) && ok;
    ok = check_property_method_collisions(surface) && ok;
    ok = check_redundant_property_accessor_methods(surface) && ok;
    ok = check_property_accessor_types(surface, index) && ok;
    ok = check_duplicate_function_symbol_signatures(surface) && ok;
    ok = check_boolean_probe_signatures(surface) && ok;
    ok = check_boolean_setter_signatures(surface) && ok;
    ok = check_no_copied_gui_widget_methods(surface, index) && ok;
    ok = check_self_returning_new_methods_have_constructor_metadata(surface, index) && ok;
    ok = check_constructor_targets_are_canonical_new(surface, index) && ok;
    ok = check_length_properties_are_semantic_lengths(surface) && ok;

    if (!ok)
        return 1;

    std::cout << "runtime class-qualified surface guardrails passed\n";
    return 0;
}
