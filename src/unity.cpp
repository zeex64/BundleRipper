#include "unity.h"
#include "binary.h"
#include "log.h"

#include <lz4.h>
extern "C" {
#include <LzmaDec.h>
}

#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <thread>

namespace fs = std::filesystem;

namespace xl {

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string basename_of(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// ---------------------------------------------------------------------------
// Decompression

static void* lzma_alloc(ISzAllocPtr, size_t size) { return std::malloc(size); }
static void lzma_free(ISzAllocPtr, void* p) { std::free(p); }

static void decompress(int method, const uint8_t* src, size_t src_size, uint8_t* dst, size_t dst_size) {
    switch (method) {
    case 0:
        if (src_size != dst_size) throw std::runtime_error("stored block size mismatch");
        std::memcpy(dst, src, dst_size);
        return;
    case 1: {  // LZMA: 5 property bytes, then the raw stream
        if (src_size < LZMA_PROPS_SIZE) throw std::runtime_error("LZMA block too small");
        SizeT out_len = dst_size, in_len = src_size - LZMA_PROPS_SIZE;
        ELzmaStatus status;
        ISzAlloc alloc = {lzma_alloc, lzma_free};
        SRes res = LzmaDecode(dst, &out_len, src + LZMA_PROPS_SIZE, &in_len, src, LZMA_PROPS_SIZE,
                              LZMA_FINISH_ANY, &status, &alloc);
        if (res != SZ_OK || out_len != dst_size) throw std::runtime_error("LZMA decode failed");
        return;
    }
    case 2:
    case 3: {  // LZ4 / LZ4HC
        int n = LZ4_decompress_safe((const char*)src, (char*)dst, (int)src_size, (int)dst_size);
        if (n != (int)dst_size) throw std::runtime_error("LZ4 decode failed");
        return;
    }
    default:
        throw std::runtime_error("unsupported bundle compression " + std::to_string(method) +
                                 " (LZHAM or encrypted bundle)");
    }
}

// ---------------------------------------------------------------------------
// Type trees

static const char kCommonStrings[] =
    "AABB\0AnimationClip\0AnimationCurve\0AnimationState\0Array\0Base\0BitField\0bitset\0bool\0char\0"
    "ColorRGBA\0Component\0data\0deque\0double\0dynamic_array\0FastPropertyName\0first\0float\0Font\0"
    "GameObject\0Generic Mono\0GradientNEW\0GUID\0GUIStyle\0int\0list\0long long\0map\0Matrix4x4f\0"
    "MdFour\0MonoBehaviour\0MonoScript\0m_ByteSize\0m_Curve\0m_EditorClassIdentifier\0"
    "m_EditorHideFlags\0m_Enabled\0m_ExtensionPtr\0m_GameObject\0m_Index\0m_IsArray\0m_IsStatic\0"
    "m_MetaFlag\0m_Name\0m_ObjectHideFlags\0m_PrefabInternal\0m_PrefabParentObject\0m_Script\0"
    "m_StaticEditorFlags\0m_Type\0m_Version\0Object\0pair\0PPtr<Component>\0PPtr<GameObject>\0"
    "PPtr<Material>\0PPtr<MonoBehaviour>\0PPtr<MonoScript>\0PPtr<Object>\0PPtr<Prefab>\0"
    "PPtr<Sprite>\0PPtr<TextAsset>\0PPtr<Texture>\0PPtr<Texture2D>\0PPtr<Transform>\0Prefab\0"
    "Quaternionf\0Rectf\0RectInt\0RectOffset\0second\0set\0short\0size\0SInt16\0SInt32\0SInt64\0SInt8\0"
    "staticvector\0string\0TextAsset\0TextMesh\0Texture\0Texture2D\0Transform\0TypelessData\0UInt16\0"
    "UInt32\0UInt64\0UInt8\0unsigned int\0unsigned long long\0unsigned short\0vector\0Vector2f\0"
    "Vector3f\0Vector4f\0m_ScriptingClassIdentifier\0Gradient\0Type*\0int2_storage\0int3_storage\0"
    "BoundsInt\0m_CorrespondingSourceObject\0m_PrefabInstance\0m_PrefabAsset\0FileSize\0Hash128\0"
    "RenderingLayerMask\0fixed_array\0EntityId\0LoadableObjectId\0LoadableSceneId\0";

static std::string_view common_string(uint32_t offset) {
    if (offset >= sizeof(kCommonStrings) - 1) return {};
    return std::string_view(kCommonStrings + offset);
}

static Prim prim_of(std::string_view t) {
    if (t == "SInt8") return Prim::I8;
    if (t == "UInt8" || t == "char") return Prim::U8;
    if (t == "SInt16" || t == "short") return Prim::I16;
    if (t == "UInt16" || t == "unsigned short") return Prim::U16;
    if (t == "SInt32" || t == "int" || t == "Type*") return Prim::I32;
    if (t == "UInt32" || t == "unsigned int") return Prim::U32;
    if (t == "SInt64" || t == "long long") return Prim::I64;
    if (t == "UInt64" || t == "unsigned long long" || t == "FileSize") return Prim::U64;
    if (t == "float") return Prim::F32;
    if (t == "double") return Prim::F64;
    if (t == "bool") return Prim::Bool;
    return Prim::None;
}

static void classify(TypeTree& tt) {
    for (auto& n : tt.nodes) {
        n.prim = n.children.empty() ? prim_of(n.type) : Prim::None;
        if (n.type == "string" && !n.children.empty()) n.kind = TypeNode::K::String;
        else if (n.type == "TypelessData") n.kind = TypeNode::K::Typeless;
        else if (n.type == "ManagedReferencesRegistry") n.kind = TypeNode::K::Registry;
        else if (!n.children.empty() && tt.nodes[n.children[0]].type == "Array") n.kind = TypeNode::K::Array;
        else if (n.prim != Prim::None) n.kind = TypeNode::K::Prim;
        else if (n.type == "string") n.kind = TypeNode::K::String;
        else n.kind = TypeNode::K::Struct;
    }
}

// Links nodes listed depth-first with levels into a tree.
static void link_levels(TypeTree& tt) {
    std::vector<int> stack;
    for (int i = 0; i < (int)tt.nodes.size(); ++i) {
        int level = tt.nodes[i].level;
        while (!stack.empty() && tt.nodes[stack.back()].level >= level) stack.pop_back();
        if (!stack.empty()) tt.nodes[stack.back()].children.push_back(i);
        stack.push_back(i);
    }
}

static std::shared_ptr<TypeTree> parse_blob(Reader& r, uint32_t version) {
    auto tt = std::make_shared<TypeTree>();
    int32_t count = r.i32();
    int32_t strings_size = r.i32();
    if (count <= 0 || strings_size < 0) throw std::runtime_error("bad type tree");
    size_t node_size = version >= 19 ? 32 : 24;
    const uint8_t* node_data = r.bytes(node_size * (size_t)count);
    const uint8_t* str_data = r.bytes((size_t)strings_size);
    tt->strings.push_back(std::make_unique<std::string>((const char*)str_data, (size_t)strings_size));
    const std::string& local = *tt->strings.back();
    auto name_at = [&](uint32_t off) -> std::string_view {
        if (off & 0x80000000u) return common_string(off & 0x7fffffffu);
        if (off >= local.size()) return {};
        return std::string_view(local.c_str() + off);
    };
    Reader nr(node_data, node_size * (size_t)count, r.big);
    tt->nodes.resize((size_t)count);
    for (int i = 0; i < count; ++i) {
        auto& n = tt->nodes[i];
        nr.u16();
        n.level = nr.u8();
        n.type_flags = nr.u8();
        n.type = name_at(nr.u32());
        n.name = name_at(nr.u32());
        n.byte_size = nr.i32();
        nr.i32();
        n.meta_flag = nr.u32();
        if (version >= 19) nr.u64();
    }
    link_levels(*tt);
    classify(*tt);
    return tt;
}

static void parse_legacy_node(Reader& r, uint32_t version, TypeTree& tt, int level) {
    TypeNode n;
    tt.strings.push_back(std::make_unique<std::string>(r.cstr()));
    n.type = *tt.strings.back();
    tt.strings.push_back(std::make_unique<std::string>(r.cstr()));
    n.name = *tt.strings.back();
    n.byte_size = r.i32();
    if (version == 2) r.i32();
    if (version != 3) r.i32();
    n.type_flags = (uint8_t)r.i32();
    r.i32();
    if (version != 3) n.meta_flag = r.u32();
    n.level = (uint8_t)level;
    tt.nodes.push_back(n);
    int children = r.i32();
    for (int i = 0; i < children; ++i) parse_legacy_node(r, version, tt, level + 1);
}

// ---------------------------------------------------------------------------
// Serialized files

static SerializedType read_type(Reader& r, uint32_t version, bool type_trees, bool is_ref) {
    SerializedType t;
    t.class_id = r.i32();
    if (version >= 16) r.u8();
    int16_t script_index = -1;
    if (version >= 17) script_index = r.i16();
    if (version >= 13) {
        if ((is_ref && script_index >= 0) || (version < 16 && t.class_id < 0) || (version >= 16 && t.class_id == 114))
            r.skip(16);
        r.skip(16);
    }
    if (type_trees) {
        if (version >= 12 || version == 10) {
            t.tree = parse_blob(r, version);
        } else {
            t.tree = std::make_shared<TypeTree>();
            parse_legacy_node(r, version, *t.tree, 0);
            link_levels(*t.tree);
            classify(*t.tree);
        }
        if (version >= 21) {
            if (is_ref) {
                r.cstr();
                r.cstr();
                r.cstr();
            } else {
                int32_t n = r.i32();
                r.skip(4 * (size_t)std::max(0, n));
            }
        }
    }
    return t;
}

static bool looks_serialized(const uint8_t* data, size_t size) {
    if (size < 48) return false;
    Reader r(data, size, true);
    uint32_t metadata_size = r.u32();
    uint32_t file_size = r.u32();
    uint32_t version = r.u32();
    uint32_t data_offset = r.u32();
    if (version < 9 || version > 50) return false;
    if (version >= 22) return true;
    return file_size == size && data_offset <= size && metadata_size < size;
}

void Database::add_serialized(const uint8_t* data, size_t size, const std::string& name) {
    auto f = std::make_unique<SerializedFile>();
    f->name = name;
    f->data = data;
    f->size = size;
    Reader r(data, size, true);
    uint32_t metadata_size = r.u32();
    uint64_t file_size = r.u32();
    f->version = r.u32();
    uint64_t data_offset = r.u32();
    uint32_t v = f->version;
    if (v >= 9) {
        f->big = r.u8() != 0;
        r.skip(3);
        if (v >= 22) {
            metadata_size = r.u32();
            file_size = r.u64();
            data_offset = r.u64();
            r.skip(8);
        }
    } else {
        r.seek(file_size - metadata_size);
        f->big = r.u8() != 0;
    }
    (void)metadata_size;
    r.big = f->big;
    if (v >= 7) f->unity_version = r.cstr();
    f->unity_major = std::atoi(f->unity_version.c_str());
    if (v >= 8) r.i32();
    if (v >= 13) f->type_trees = r.boolean();
    int32_t type_count = r.i32();
    for (int i = 0; i < type_count; ++i) f->types.push_back(read_type(r, v, f->type_trees, false));
    bool big_ids = false;
    if (v >= 7 && v < 14) big_ids = r.i32() != 0;
    int32_t object_count = r.i32();
    f->objects.reserve((size_t)std::max(0, object_count));
    for (int i = 0; i < object_count; ++i) {
        ObjectInfo o;
        if (big_ids) o.path_id = r.i64();
        else if (v < 14) o.path_id = r.i32();
        else { r.align(4); o.path_id = r.i64(); }
        o.offset = (v >= 22 ? r.u64() : r.u32()) + data_offset;
        o.size = r.u32();
        int32_t type_id = r.i32();
        if (v < 16) {
            o.class_id = r.u16();
            for (size_t t = 0; t < f->types.size(); ++t)
                if (f->types[t].class_id == type_id) { o.type_index = (int)t; break; }
        } else {
            o.type_index = type_id;
            if (type_id >= 0 && type_id < (int)f->types.size()) o.class_id = f->types[type_id].class_id;
        }
        if (v < 11) r.u16();
        if (v >= 11 && v < 17) r.i16();
        if (v == 15 || v == 16) r.u8();
        f->by_id[o.path_id] = f->objects.size();
        f->objects.push_back(o);
    }
    if (v >= 11) {
        int32_t script_count = r.i32();
        for (int i = 0; i < script_count; ++i) {
            r.i32();
            if (v < 14) r.i32();
            else { r.align(4); r.i64(); }
        }
    }
    int32_t external_count = r.i32();
    for (int i = 0; i < external_count; ++i) {
        if (v >= 6) r.cstr();
        if (v >= 5) { r.skip(16); r.i32(); }
        f->externals.push_back(r.cstr());
    }
    log_verbose("  serialized file '%s': Unity %s, format %u, %zu objects%s", name.c_str(),
                f->unity_version.c_str(), v, f->objects.size(), f->type_trees ? "" : " (NO TYPE TREES)");
    files.push_back(std::move(f));
}

// ---------------------------------------------------------------------------
// Bundles

void Database::load_bundle(std::vector<uint8_t>&& raw, const std::string& label) {
    Reader r(raw.data(), raw.size(), true);
    std::string signature = r.cstr();
    if (signature != "UnityFS") throw std::runtime_error("unsupported bundle signature '" + signature + "'");
    uint32_t format = r.u32();
    r.cstr();
    std::string engine = r.cstr();
    r.i64();
    uint32_t info_packed = r.u32();
    uint32_t info_size = r.u32();
    uint32_t flags = r.u32();
    int major = std::atoi(engine.c_str());
    int minor = 0, patch = 0;
    std::sscanf(engine.c_str(), "%d.%d.%d", &major, &minor, &patch);
    // The block-alignment fix reused bit 0x200 (UnityCN encryption before it).
    bool new_flags = major > 2022 || (major == 2022 && (minor > 1 || (minor == 1 && patch >= 1))) ||
                     (major == 2021 && (minor > 3 || (minor == 3 && patch >= 2))) ||
                     (major == 2020 && (minor > 3 || (minor == 3 && patch >= 34)));
    if (!new_flags && (flags & 0x200)) throw std::runtime_error("encrypted (UnityCN) bundles are not supported");
    if (format >= 7 || (major == 2019 && (minor > 4 || (minor == 4 && patch >= 15)))) r.align(16);
    size_t start = r.pos;
    const uint8_t* info_src;
    if (flags & 0x80) {
        info_src = raw.data() + raw.size() - info_packed;
    } else {
        info_src = r.bytes(info_packed);
    }
    std::vector<uint8_t> info(info_size);
    decompress(flags & 0x3f, info_src, info_packed, info.data(), info_size);
    Reader ir(info.data(), info.size(), true);
    ir.skip(16);
    int32_t block_count = ir.i32();
    struct Block { uint32_t usize, csize; uint16_t flags; };
    std::vector<Block> blocks((size_t)block_count);
    uint64_t total = 0;
    for (auto& b : blocks) {
        b.usize = ir.u32();
        b.csize = ir.u32();
        b.flags = ir.u16();
        total += b.usize;
    }
    struct Node { int64_t offset, size; uint32_t flags; std::string path; };
    int32_t node_count = ir.i32();
    std::vector<Node> nodes((size_t)node_count);
    for (auto& n : nodes) {
        n.offset = ir.i64();
        n.size = ir.i64();
        n.flags = ir.u32();
        n.path = ir.cstr();
    }
    if (flags & 0x80) r.seek(start);
    if (new_flags && (flags & 0x200)) r.align(16);

    auto blob = std::make_unique<std::vector<uint8_t>>((size_t)total);
    std::vector<size_t> src_offsets(blocks.size()), dst_offsets(blocks.size());
    size_t src = r.pos, dst = 0;
    for (size_t i = 0; i < blocks.size(); ++i) {
        src_offsets[i] = src;
        dst_offsets[i] = dst;
        src += blocks[i].csize;
        dst += blocks[i].usize;
    }
    if (src > raw.size()) throw std::runtime_error("bundle is truncated");
    std::atomic<size_t> next{0};
    std::atomic<bool> failed{false};
    std::string error;
    auto worker = [&] {
        for (size_t i; (i = next++) < blocks.size() && !failed;) {
            try {
                decompress(blocks[i].flags & 0x3f, raw.data() + src_offsets[i], blocks[i].csize,
                           blob->data() + dst_offsets[i], blocks[i].usize);
            } catch (const std::exception& e) {
                if (!failed.exchange(true)) error = e.what();
            }
        }
    };
    unsigned threads = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; ++t) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
    if (failed) throw std::runtime_error("bundle block: " + error);
    raw.clear();
    raw.shrink_to_fit();

    log_verbose("bundle '%s': Unity %s, %zu blocks, %zu entries, %.1f MB unpacked", label.c_str(), engine.c_str(),
                blocks.size(), nodes.size(), total / 1048576.0);
    const uint8_t* base = blob->data();
    blobs_.push_back(std::move(blob));
    ++bundle_count;
    for (auto& n : nodes) {
        if (n.offset < 0 || n.size < 0 || (uint64_t)(n.offset + n.size) > total) continue;
        const uint8_t* p = base + n.offset;
        std::string name = basename_of(n.path);
        if ((n.flags & 4) && looks_serialized(p, (size_t)n.size)) add_serialized(p, (size_t)n.size, name);
        else resources_[lower(name)] = {p, (size_t)n.size};
    }
}

void Database::load_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    std::streamsize n = in.tellg();
    if (n < 16) return;
    in.seekg(0);
    char head[8] = {};
    in.read(head, 8);
    in.seekg(0);
    if (std::memcmp(head, "UnityFS", 7) == 0) {
        std::vector<uint8_t> raw((size_t)n);
        in.read((char*)raw.data(), n);
        load_bundle(std::move(raw), path.filename().string());
        return;
    }
    auto blob = std::make_unique<std::vector<uint8_t>>((size_t)n);
    in.read((char*)blob->data(), n);
    if (looks_serialized(blob->data(), blob->size())) {
        add_serialized(blob->data(), blob->size(), path.filename().string());
        blobs_.push_back(std::move(blob));
    } else {
        std::string ext = lower(path.extension().string());
        if (ext == ".ress" || ext == ".resource") {
            resources_[lower(path.filename().string())] = {blob->data(), blob->size()};
            blobs_.push_back(std::move(blob));
        }
    }
}

void Database::load(const fs::path& path) {
    if (fs::is_directory(path)) {
        std::vector<fs::path> list;
        for (auto& e : fs::recursive_directory_iterator(path, fs::directory_options::skip_permission_denied))
            if (e.is_regular_file()) list.push_back(e.path());
        std::sort(list.begin(), list.end());
        for (auto& p : list) {
            std::ifstream in(p, std::ios::binary);
            char head[8] = {};
            in.read(head, 8);
            std::string ext = lower(p.extension().string());
            if (std::memcmp(head, "UnityFS", 7) == 0 || ext == ".assets" || ext == ".ress" || ext == ".resource" ||
                ext.empty())
                load_file(p);
        }
    } else {
        load_file(path);
    }
    resolve_externals();
}

void Database::resolve_externals() {
    std::unordered_map<std::string, int> by_name;
    for (int i = 0; i < (int)files.size(); ++i) by_name[lower(basename_of(files[i]->name))] = i;
    for (auto& f : files) {
        f->external_files.clear();
        for (auto& e : f->externals) {
            std::string l = lower(e);
            if (l.find("unity default resources") != std::string::npos) f->external_files.push_back(kBuiltinDefault);
            else if (l.find("unity_builtin_extra") != std::string::npos) f->external_files.push_back(kBuiltinExtra);
            else {
                auto it = by_name.find(lower(basename_of(e)));
                f->external_files.push_back(it == by_name.end() ? -1 : it->second);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Object access

ObjRef Database::resolve(int from_file, const Value& pptr) const {
    if (pptr.is_null() || from_file < 0 || from_file >= (int)files.size()) return {};
    int64_t fid = pptr["m_FileID"].i64();
    int64_t pid = pptr["m_PathID"].i64();
    if (pid == 0) return {};
    if (fid == 0) return {from_file, pid};
    auto& f = *files[from_file];
    if (fid < 0 || fid > (int64_t)f.external_files.size()) return {};
    int target = f.external_files[(size_t)fid - 1];
    if (target == -1) return {};
    return {target, pid};
}

const ObjectInfo* Database::info(ObjRef ref) const {
    if (ref.file < 0 || ref.file >= (int)files.size()) return nullptr;
    auto& f = *files[ref.file];
    auto it = f.by_id.find(ref.id);
    return it == f.by_id.end() ? nullptr : &f.objects[it->second];
}

int Database::class_of(ObjRef ref) const {
    auto* o = info(ref);
    return o ? o->class_id : 0;
}

std::vector<ObjRef> Database::objects_of(int class_id) const {
    std::vector<ObjRef> out;
    for (int i = 0; i < (int)files.size(); ++i)
        for (auto& o : files[i]->objects)
            if (o.class_id == class_id) out.push_back({i, o.path_id});
    return out;
}

namespace {
struct StopRead {};

struct ValueReader {
    Reader r;
    const TypeTree& tt;

    void read(int index, Value& out) {
        const TypeNode& n = tt.nodes[index];
        bool align = (n.meta_flag & 0x4000) != 0;
        switch (n.kind) {
        case TypeNode::K::Prim: read_prim(n.prim, out); break;
        case TypeNode::K::String: {
            int32_t len = r.i32();
            if (len < 0) throw std::runtime_error("bad string length");
            const uint8_t* p = r.bytes((size_t)len);
            out.kind = Kind::String;
            out.str.assign((const char*)p, (size_t)len);
            align = true;
            break;
        }
        case TypeNode::K::Typeless: {
            int32_t len = r.i32();
            if (len < 0) throw std::runtime_error("bad data length");
            out.kind = Kind::Bytes;
            out.prim = Prim::U8;
            out.data = r.bytes((size_t)len);
            out.size = (size_t)len;
            break;
        }
        case TypeNode::K::Array: {
            const TypeNode& arr = tt.nodes[n.children[0]];
            if (arr.meta_flag & 0x4000) align = true;
            read_array(arr, out);
            break;
        }
        case TypeNode::K::Registry: throw StopRead{};
        case TypeNode::K::Struct:
            if (n.type_flags & 1 && n.children.size() >= 2) {
                read_array(n, out);
            } else {
                out.kind = Kind::Object;
                out.fields.resize(n.children.size());
                for (size_t c = 0; c < n.children.size(); ++c) {
                    out.fields[c].first = tt.nodes[n.children[c]].name;
                    read(n.children[c], out.fields[c].second);
                }
            }
            break;
        }
        if (align) r.align(4);
    }

    void read_array(const TypeNode& arr, Value& out) {
        if (arr.children.size() < 2) throw std::runtime_error("bad array node");
        int32_t count = r.i32();
        if (count < 0) throw std::runtime_error("bad array length");
        const TypeNode& elem = tt.nodes[arr.children[1]];
        if (elem.kind == TypeNode::K::Prim && !(elem.meta_flag & 0x4000)) {
            size_t es = prim_size(elem.prim);
            out.kind = Kind::Bytes;
            out.prim = elem.prim;
            out.big = r.big;
            out.data = r.bytes(es * (size_t)count);
            out.size = es * (size_t)count;
            return;
        }
        if ((size_t)count > r.left()) throw std::runtime_error("array longer than object");
        out.kind = Kind::Array;
        out.items.resize((size_t)count);
        for (int32_t i = 0; i < count; ++i) read(arr.children[1], out.items[(size_t)i]);
    }

    void read_prim(Prim p, Value& out) {
        switch (p) {
        case Prim::Bool: out.kind = Kind::Bool; out.i = r.u8() != 0; break;
        case Prim::I8: out.kind = Kind::Int; out.i = r.i8(); break;
        case Prim::U8: out.kind = Kind::UInt; out.u = r.u8(); break;
        case Prim::I16: out.kind = Kind::Int; out.i = r.i16(); break;
        case Prim::U16: out.kind = Kind::UInt; out.u = r.u16(); break;
        case Prim::I32: out.kind = Kind::Int; out.i = r.i32(); break;
        case Prim::U32: out.kind = Kind::UInt; out.u = r.u32(); break;
        case Prim::I64: out.kind = Kind::Int; out.i = r.i64(); break;
        case Prim::U64: out.kind = Kind::UInt; out.u = r.u64(); break;
        case Prim::F32: out.kind = Kind::Float; out.f = r.f32(); break;
        case Prim::F64: out.kind = Kind::Float; out.f = r.f64(); break;
        default: break;
        }
    }
};
} // namespace

Value Database::read(ObjRef ref) const {
    Value v;
    const ObjectInfo* o = info(ref);
    if (!o) return v;
    auto& f = *files[ref.file];
    if (!f.type_trees) throw std::runtime_error("'" + f.name + "' was built without type trees; cannot parse it");
    if (o->type_index < 0 || o->type_index >= (int)f.types.size() || !f.types[o->type_index].tree)
        throw std::runtime_error("object has no type tree");
    if (o->offset + o->size > f.size) throw std::runtime_error("object outside its file");
    ValueReader vr{Reader(f.data + o->offset, o->size, f.big), *f.types[o->type_index].tree};
    try {
        vr.read(0, v);
    } catch (const StopRead&) {
    }
    return v;
}

std::pair<const uint8_t*, size_t> Database::resource(const std::string& path, uint64_t offset, uint64_t size) const {
    auto it = resources_.find(lower(basename_of(path)));
    if (it == resources_.end()) return {nullptr, 0};
    if (offset + size > it->second.second) return {nullptr, 0};
    return {it->second.first + offset, (size_t)size};
}

} // namespace xl
