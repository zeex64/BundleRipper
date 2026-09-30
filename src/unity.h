// Unity container formats: UnityFS asset bundles, serialized files, type trees,
// and a database that resolves object references across every loaded file.
#pragma once
#include "value.h"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace xl {

// Unity class ids used by the ripper.
enum ClassId : int {
    kGameObject = 1,
    kTransform = 4,
    kMaterial = 21,
    kMeshRenderer = 23,
    kTexture2D = 28,
    kMeshFilter = 33,
    kMesh = 43,
    kShader = 48,
    kMeshCollider = 64,
    kBoxCollider = 65,
    kLight = 108,
    kMonoBehaviour = 114,
    kMonoScript = 115,
    kSphereCollider = 135,
    kCapsuleCollider = 136,
    kSkinnedMeshRenderer = 137,
    kTerrainCollider = 154,
    kTerrainData = 156,
    kLODGroup = 205,
    kTerrain = 218,
    kRectTransform = 224,
    kTerrainLayer = 1953259897,
};

struct TypeNode {
    enum class K : uint8_t { Struct, Prim, String, Typeless, Array, Registry };
    std::string_view type, name;
    int32_t byte_size = 0;
    uint32_t meta_flag = 0;
    uint8_t level = 0;
    uint8_t type_flags = 0;
    K kind = K::Struct;
    Prim prim = Prim::None;
    std::vector<int> children;
};

struct TypeTree {
    std::vector<TypeNode> nodes;  // nodes[0] is the root
    std::vector<std::unique_ptr<std::string>> strings;  // owns local names
};

struct SerializedType {
    int class_id = 0;
    std::shared_ptr<TypeTree> tree;
};

struct ObjectInfo {
    int64_t path_id = 0;
    uint64_t offset = 0;  // from the start of the serialized file
    uint32_t size = 0;
    int type_index = -1;
    int class_id = 0;
};

// File indices below zero name Unity's built-in resource files.
constexpr int kBuiltinDefault = -2;  // "Library/unity default resources"
constexpr int kBuiltinExtra = -3;    // "Resources/unity_builtin_extra"

struct SerializedFile {
    std::string name;
    const uint8_t* data = nullptr;
    size_t size = 0;
    uint32_t version = 0;
    bool big = false;
    bool type_trees = true;
    std::string unity_version;
    int unity_major = 0;
    std::vector<SerializedType> types;
    std::vector<ObjectInfo> objects;
    std::unordered_map<int64_t, size_t> by_id;
    std::vector<std::string> externals;
    std::vector<int> external_files;  // index into Database::files, or kBuiltin*, or -1
};

struct ObjRef {
    int file = -1;
    int64_t id = 0;
    bool valid() const { return id != 0 && file != -1; }
    bool builtin() const { return file <= kBuiltinDefault; }
    bool operator==(const ObjRef& o) const { return file == o.file && id == o.id; }
    bool operator!=(const ObjRef& o) const { return !(*this == o); }
    bool operator<(const ObjRef& o) const { return file != o.file ? file < o.file : id < o.id; }
};
struct ObjRefHash {
    size_t operator()(const ObjRef& r) const { return std::hash<int64_t>()(r.id * 31 + r.file); }
};

class Database {
public:
    // Loads a bundle, a serialized file, or every Unity file in a folder.
    void load(const std::filesystem::path& path);
    void resolve_externals();

    std::vector<std::unique_ptr<SerializedFile>> files;

    ObjRef resolve(int from_file, const Value& pptr) const;
    const ObjectInfo* info(ObjRef ref) const;
    int class_of(ObjRef ref) const;
    Value read(ObjRef ref) const;
    // Bytes of a streamed resource (.resS / .resource), or {nullptr, 0}.
    std::pair<const uint8_t*, size_t> resource(const std::string& path, uint64_t offset, uint64_t size) const;
    // Every object of a class, across all files.
    std::vector<ObjRef> objects_of(int class_id) const;

    size_t bundle_count = 0;

private:
    void load_file(const std::filesystem::path& path);
    void load_bundle(std::vector<uint8_t>&& raw, const std::string& label);
    void add_serialized(const uint8_t* data, size_t size, const std::string& name);

    std::vector<std::unique_ptr<std::vector<uint8_t>>> blobs_;
    std::unordered_map<std::string, std::pair<const uint8_t*, size_t>> resources_;
};

std::string lower(std::string s);
std::string basename_of(const std::string& path);

} // namespace xl
