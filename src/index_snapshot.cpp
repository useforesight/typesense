#include "index.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

#include "file_utils.h"
#include "logger.h"
#include "posting.h"

namespace {

constexpr char SNAPSHOT_MAGIC[] = {'T', 'S', 'I', 'S', 'N', 'A', 'P', '1'};
constexpr uint32_t SNAPSHOT_VERSION = 2;
constexpr size_t FILE_COPY_BUFFER_SIZE = 64 * 1024;

template <typename T>
void write_pod(std::ostream& out, const T& value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
    if(!out.good()) {
        throw std::runtime_error("Unable to write index snapshot.");
    }
}

template <typename T>
T read_pod(std::istream& in) {
    T value{};
    in.read(reinterpret_cast<char*>(&value), sizeof(T));
    if(!in.good()) {
        throw std::runtime_error("Unable to read index snapshot.");
    }
    return value;
}

void write_u8(std::ostream& out, uint8_t value) { write_pod(out, value); }
void write_u32(std::ostream& out, uint32_t value) { write_pod(out, value); }
void write_u64(std::ostream& out, uint64_t value) { write_pod(out, value); }
void write_i64(std::ostream& out, int64_t value) { write_pod(out, value); }

uint8_t read_u8(std::istream& in) { return read_pod<uint8_t>(in); }
uint32_t read_u32(std::istream& in) { return read_pod<uint32_t>(in); }
uint64_t read_u64(std::istream& in) { return read_pod<uint64_t>(in); }
int64_t read_i64(std::istream& in) { return read_pod<int64_t>(in); }

void write_string(std::ostream& out, const std::string& value) {
    write_u64(out, value.size());
    out.write(value.data(), value.size());
    if(!out.good()) {
        throw std::runtime_error("Unable to write string to index snapshot.");
    }
}

std::string read_string(std::istream& in) {
    const auto size = read_u64(in);
    std::string value(size, '\0');
    if(size != 0) {
        in.read(&value[0], size);
    }
    if(!in.good()) {
        throw std::runtime_error("Unable to read string from index snapshot.");
    }
    return value;
}

uint64_t get_file_size_bytes(const std::string& path) {
    struct stat info;
    if(stat(path.c_str(), &info) != 0 || info.st_size < 0) {
        return 0;
    }

    return static_cast<uint64_t>(info.st_size);
}

int64_t elapsed_ms_since(const std::chrono::time_point<std::chrono::high_resolution_clock>& begin) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - begin).count();
}

struct temporary_file_t {
    std::string path;

    explicit temporary_file_t(std::string path): path(std::move(path)) {}
    temporary_file_t(const temporary_file_t&) = delete;
    temporary_file_t& operator=(const temporary_file_t&) = delete;

    ~temporary_file_t() {
        if(!path.empty()) {
            delete_path(path, false);
        }
    }
};

temporary_file_t make_temporary_payload_file(const std::string& base_path) {
    std::string path_template = base_path + ".payload.XXXXXX";
    std::vector<char> path_chars(path_template.begin(), path_template.end());
    path_chars.push_back('\0');

    const int fd = mkstemp(path_chars.data());
    if(fd == -1) {
        throw std::runtime_error("Unable to create temporary index snapshot payload file: " +
                                 std::string(std::strerror(errno)));
    }

    if(close(fd) != 0) {
        const auto err = errno;
        delete_path(path_chars.data(), false);
        throw std::runtime_error("Unable to close temporary index snapshot payload file: " +
                                 std::string(std::strerror(err)));
    }

    return temporary_file_t(std::string(path_chars.data()));
}

void copy_exact_bytes(std::istream& in, std::ostream& out, uint64_t size) {
    std::array<char, FILE_COPY_BUFFER_SIZE> buffer{};
    uint64_t remaining = size;

    while(remaining != 0) {
        const auto chunk_size = static_cast<std::streamsize>(
            std::min<uint64_t>(remaining, buffer.size()));
        in.read(buffer.data(), chunk_size);
        if(in.gcount() != chunk_size || in.bad()) {
            throw std::runtime_error("Unable to read payload from index snapshot.");
        }

        out.write(buffer.data(), chunk_size);
        if(!out.good()) {
            throw std::runtime_error("Unable to write payload to index snapshot.");
        }

        remaining -= static_cast<uint64_t>(chunk_size);
    }
}

void write_file_payload(std::ostream& out, const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if(!in.good()) {
        throw std::runtime_error("Unable to open index snapshot payload for reading.");
    }

    const auto file_size = in.tellg();
    if(file_size < 0) {
        throw std::runtime_error("Unable to determine index snapshot payload size.");
    }

    write_u64(out, static_cast<uint64_t>(file_size));
    in.seekg(0, std::ios::beg);
    copy_exact_bytes(in, out, static_cast<uint64_t>(file_size));
}

void read_file_payload(std::istream& in, const std::string& path) {
    const auto size = read_u64(in);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if(!out.good()) {
        throw std::runtime_error("Unable to open index snapshot payload for writing.");
    }

    copy_exact_bytes(in, out, size);
    out.close();
    if(!out.good()) {
        throw std::runtime_error("Unable to flush index snapshot payload.");
    }
}

void write_vector_indexes(std::ostream& out,
                          const spp::sparse_hash_map<std::string, hnsw_index_t*>& vector_index,
                          const std::string& tmp_path) {
    write_u64(out, vector_index.size());
    for(const auto& vec_index_kv: vector_index) {
        const auto& field_name = vec_index_kv.first;
        auto* hnsw_index = vec_index_kv.second;

        write_string(out, field_name);
        write_u64(out, hnsw_index->num_dim);
        write_u32(out, static_cast<uint32_t>(hnsw_index->distance_type));
        write_u64(out, hnsw_index->vecdex->getMaxElements());
        write_u64(out, hnsw_index->vecdex->getCurrentElementCount());
        write_u64(out, hnsw_index->vecdex->getDeletedCount());

        auto payload_file = make_temporary_payload_file(tmp_path);
        {
            std::lock_guard lk(hnsw_index->repair_m);
            hnsw_index->vecdex->saveIndex(payload_file.path);
        }
        write_file_payload(out, payload_file.path);
    }
}

void read_vector_indexes(std::istream& in,
                         const spp::sparse_hash_map<std::string, hnsw_index_t*>& vector_index,
                         const std::string& snapshot_path) {
    const auto vector_count = read_u64(in);
    if(vector_count != vector_index.size()) {
        throw std::runtime_error("Snapshot vector index count does not match schema.");
    }

    for(uint64_t i = 0; i < vector_count; i++) {
        auto field_name = read_string(in);
        if(vector_index.count(field_name) == 0) {
            throw std::runtime_error("Snapshot contains an unknown vector field `" + field_name + "`.");
        }

        auto* hnsw_index = vector_index.at(field_name);
        const auto num_dim = read_u64(in);
        const auto distance_type = static_cast<vector_distance_type_t>(read_u32(in));
        const auto max_elements = read_u64(in);
        const auto current_count = read_u64(in);
        const auto deleted_count = read_u64(in);

        if(num_dim != hnsw_index->num_dim || distance_type != hnsw_index->distance_type) {
            throw std::runtime_error("Snapshot vector field `" + field_name + "` does not match schema.");
        }

        auto payload_file = make_temporary_payload_file(snapshot_path);
        read_file_payload(in, payload_file.path);

        std::unique_ptr<hnswlib::HierarchicalNSW<float>> loaded_index(
            new hnswlib::HierarchicalNSW<float>(hnsw_index->space,
                                                payload_file.path,
                                                false,
                                                max_elements,
                                                true));
        if(loaded_index->getMaxElements() != max_elements ||
           loaded_index->getCurrentElementCount() != current_count ||
           loaded_index->getDeletedCount() != deleted_count) {
            throw std::runtime_error("Snapshot vector field `" + field_name + "` failed validation.");
        }

        std::lock_guard lk(hnsw_index->repair_m);
        delete hnsw_index->vecdex;
        hnsw_index->vecdex = loaded_index.release();
    }
}

void write_u32_vector(std::ostream& out, const std::vector<uint32_t>& values) {
    write_u64(out, values.size());
    for(const auto value: values) {
        write_u32(out, value);
    }
}

std::vector<uint32_t> read_u32_vector(std::istream& in) {
    const auto size = read_u64(in);
    std::vector<uint32_t> values;
    values.reserve(size);
    for(uint64_t i = 0; i < size; i++) {
        values.push_back(read_u32(in));
    }
    return values;
}

std::vector<uint32_t> raw_ids_to_vector(void* raw_ids) {
    std::vector<uint32_t> ids;
    if(raw_ids != nullptr) {
        ids_t::uncompress(raw_ids, ids);
    }
    return ids;
}

void write_raw_ids(std::ostream& out, void* raw_ids) {
    write_u32_vector(out, raw_ids_to_vector(raw_ids));
}

void* read_raw_ids(std::istream& in) {
    auto ids = read_u32_vector(in);
    if(ids.empty()) {
        return nullptr;
    }
    return ids_t::create(ids);
}

void write_id_list(std::ostream& out, const id_list_t* list) {
    std::vector<uint32_t> ids;
    if(list != nullptr) {
        const_cast<id_list_t*>(list)->uncompress(ids);
    }
    write_u32_vector(out, ids);
}

void read_id_list(std::istream& in, id_list_t* list) {
    auto ids = read_u32_vector(in);
    for(const auto id: ids) {
        list->upsert(id);
    }
}

void write_posting_list(std::ostream& out, const posting_list_t* list) {
    if(list == nullptr) {
        write_u64(out, 0);
        return;
    }

    write_u64(out, list->num_ids());
    auto iter = const_cast<posting_list_t*>(list)->new_iterator();
    while(iter.valid()) {
        write_u32(out, iter.id());
        std::vector<uint32_t> offsets;
        posting_list_t::get_offsets(iter, offsets);
        write_u32_vector(out, offsets);
        iter.next();
    }
}

void read_posting_list(std::istream& in, posting_list_t* list) {
    const auto count = read_u64(in);
    for(uint64_t i = 0; i < count; i++) {
        const auto id = read_u32(in);
        auto offsets = read_u32_vector(in);
        list->upsert(id, offsets);
    }
}

std::vector<art_document> read_art_documents(std::istream& in, int64_t score) {
    const auto count = read_u64(in);
    std::vector<art_document> documents;
    documents.reserve(count);

    for(uint64_t i = 0; i < count; i++) {
        const auto id = read_u32(in);
        auto offsets = read_u32_vector(in);
        documents.emplace_back(id, score, offsets);
    }

    return documents;
}

void write_art_documents(std::ostream& out, const void* raw_posting) {
    std::unique_ptr<posting_list_t> owned(posting_t::to_owned_posting_list(raw_posting));
    if(owned == nullptr) {
        write_u64(out, 0);
        return;
    }

    write_u64(out, owned->num_ids());
    auto iter = owned->new_iterator();
    while(iter.valid()) {
        write_u32(out, iter.id());
        std::vector<uint32_t> offsets;
        posting_list_t::get_offsets(iter, offsets);
        write_u32_vector(out, offsets);
        iter.next();
    }
}

struct art_snapshot_write_ctx {
    std::ostream* out;
    art_tree* tree;
};

int write_art_leaf(void* data, const unsigned char* key, uint32_t key_len, void* value) {
    auto* ctx = static_cast<art_snapshot_write_ctx*>(data);
    std::string token(reinterpret_cast<const char*>(key), key_len);
    write_string(*ctx->out, token);

    auto* leaf = static_cast<art_leaf*>(art_search(ctx->tree, key, key_len));
    write_i64(*ctx->out, leaf == nullptr ? std::numeric_limits<int64_t>::min() : leaf->max_score);
    write_art_documents(*ctx->out, value);
    return 0;
}

void write_art_tree(std::ostream& out, art_tree* tree) {
    write_u64(out, tree == nullptr ? 0 : art_size(tree));
    if(tree == nullptr) {
        return;
    }

    art_snapshot_write_ctx ctx{&out, tree};
    art_iter(tree, write_art_leaf, &ctx);
}

void read_art_tree(std::istream& in, art_tree* tree) {
    const auto leaf_count = read_u64(in);
    for(uint64_t i = 0; i < leaf_count; i++) {
        auto token = read_string(in);
        const auto max_score = read_i64(in);
        auto documents = read_art_documents(in, max_score);
        if(documents.empty()) {
            continue;
        }
        art_inserts(tree,
                    reinterpret_cast<const unsigned char*>(token.data()),
                    static_cast<int>(token.size()),
                    max_score,
                    documents);
    }
}

void write_manifest_header(std::ostream& out, const nlohmann::json& manifest) {
    out.write(SNAPSHOT_MAGIC, sizeof(SNAPSHOT_MAGIC));
    write_u32(out, SNAPSHOT_VERSION);

    const auto manifest_dump = manifest.dump(-1, ' ', false, nlohmann::detail::error_handler_t::ignore);
    write_u64(out, manifest_dump.size());
    out.write(manifest_dump.data(), manifest_dump.size());
    if(!out.good()) {
        throw std::runtime_error("Unable to write index snapshot manifest.");
    }
}

nlohmann::json read_manifest_header(std::istream& in) {
    char magic[sizeof(SNAPSHOT_MAGIC)];
    in.read(magic, sizeof(magic));
    if(!in.good() || std::memcmp(magic, SNAPSHOT_MAGIC, sizeof(SNAPSHOT_MAGIC)) != 0) {
        throw std::runtime_error("Invalid index snapshot magic.");
    }

    const auto version = read_u32(in);
    if(version != SNAPSHOT_VERSION) {
        throw std::runtime_error("Unsupported index snapshot version.");
    }

    const auto manifest_size = read_u64(in);
    std::string manifest_dump(manifest_size, '\0');
    if(manifest_size != 0) {
        in.read(&manifest_dump[0], manifest_size);
    }
    if(!in.good()) {
        throw std::runtime_error("Unable to read index snapshot manifest.");
    }

    auto manifest = nlohmann::json::parse(manifest_dump, nullptr, false);
    if(manifest.is_discarded()) {
        throw std::runtime_error("Unable to parse index snapshot manifest.");
    }

    return manifest;
}

}  // namespace

void adi_tree_t::snapshot_write(std::ostream& out) const {
    write_u64(out, id_keys.size());
    for(const auto& kv: id_keys) {
        write_u32(out, kv.first);
        write_string(out, kv.second);
    }
}

void adi_tree_t::snapshot_read(std::istream& in) {
    std::vector<uint32_t> existing_ids;
    existing_ids.reserve(id_keys.size());
    for(const auto& kv: id_keys) {
        existing_ids.push_back(kv.first);
    }
    for(const auto id: existing_ids) {
        remove(id);
    }

    const auto count = read_u64(in);
    for(uint64_t i = 0; i < count; i++) {
        const auto id = read_u32(in);
        auto key = read_string(in);
        index(id, key);
    }
}

void num_tree_t::snapshot_write(std::ostream& out) const {
    write_u64(out, int64map.size());
    for(const auto& kv: int64map) {
        write_i64(out, kv.first);
        void* ids = kv.second;
        write_raw_ids(out, ids);
    }
}

void num_tree_t::snapshot_read(std::istream& in) {
    for(auto& kv: int64map) {
        ids_t::destroy_list(kv.second);
    }
    int64map.clear();

    const auto count = read_u64(in);
    for(uint64_t i = 0; i < count; i++) {
        const auto value = read_i64(in);
        auto ids = read_u32_vector(in);
        for(const auto id: ids) {
            insert(value, id);
        }
    }
}

void NumericTrie::Node::snapshot_collect(std::vector<std::pair<uint64_t, std::vector<uint32_t>>>& entries,
                                         const char& max_level, uint64_t prefix, char level) const {
    if(level == max_level) {
        void* ids = seq_ids;
        auto ids_vec = raw_ids_to_vector(ids);
        if(!ids_vec.empty()) {
            entries.emplace_back(prefix, std::move(ids_vec));
        }
        return;
    }

    if(children == nullptr) {
        return;
    }

    for(uint16_t i = 0; i < EXPANSE; i++) {
        if(children[i] != nullptr) {
            children[i]->snapshot_collect(entries, max_level, (prefix << 8) | i, level + 1);
        }
    }
}

void NumericTrie::snapshot_write(std::ostream& out, bool geopoint) const {
    std::vector<std::pair<uint64_t, std::vector<uint32_t>>> positive_entries;
    std::vector<std::pair<uint64_t, std::vector<uint32_t>>> negative_entries;

    if(positive_trie != nullptr) {
        positive_trie->snapshot_collect(positive_entries, max_level);
    }
    if(!geopoint && negative_trie != nullptr) {
        negative_trie->snapshot_collect(negative_entries, max_level);
    }

    write_u8(out, static_cast<uint8_t>(max_level));
    write_u8(out, static_cast<uint8_t>(geopoint));
    write_u64(out, positive_entries.size() + negative_entries.size());

    for(const auto& entry: positive_entries) {
        if(geopoint) {
            write_u8(out, 2);
            write_u64(out, entry.first);
        } else {
            write_u8(out, 0);
            write_i64(out, static_cast<int64_t>(entry.first));
        }
        write_u32_vector(out, entry.second);
    }

    for(const auto& entry: negative_entries) {
        write_u8(out, 1);
        write_i64(out, -static_cast<int64_t>(entry.first));
        write_u32_vector(out, entry.second);
    }
}

void NumericTrie::snapshot_read(std::istream& in, bool geopoint) {
    delete negative_trie;
    delete positive_trie;
    negative_trie = nullptr;
    positive_trie = nullptr;

    const auto snapshot_max_level = read_u8(in);
    const auto snapshot_geopoint = read_u8(in) != 0;
    if(snapshot_max_level != static_cast<uint8_t>(max_level) || snapshot_geopoint != geopoint) {
        throw std::runtime_error("NumericTrie snapshot shape does not match index schema.");
    }

    const auto count = read_u64(in);
    for(uint64_t i = 0; i < count; i++) {
        const auto tag = read_u8(in);
        if(tag == 2) {
            const auto prefix = read_u64(in);
            const auto cell_id = prefix << (8 * (8 - max_level));
            auto ids = read_u32_vector(in);
            for(const auto id: ids) {
                insert_geopoint(cell_id, id);
            }
        } else {
            const auto value = read_i64(in);
            auto ids = read_u32_vector(in);
            for(const auto id: ids) {
                insert(value, id);
            }
        }
    }
}

void facet_index_t::snapshot_write(std::ostream& out) const {
    write_u32(out, next_facet_id.load());
    write_u64(out, facet_field_map.size());

    for(const auto& field_kv: facet_field_map) {
        write_string(out, field_kv.first);
        const auto& facet_index = field_kv.second;

        write_u8(out, facet_index.has_value_index ? 1 : 0);
        write_u8(out, facet_index.has_hash_index ? 1 : 0);

        write_u64(out, facet_index.fvalue_seq_ids.size());
        for(const auto& fvalue_kv: facet_index.fvalue_seq_ids) {
            write_string(out, fvalue_kv.first);
            write_u32(out, fvalue_kv.second.facet_id);
            write_u8(out, fvalue_kv.second.seq_ids != nullptr ? 1 : 0);
            if(fvalue_kv.second.seq_ids != nullptr) {
                void* ids = fvalue_kv.second.seq_ids;
                write_raw_ids(out, ids);
            }
        }

        write_u64(out, facet_index.fid_fvalues.size());
        for(const auto& fid_kv: facet_index.fid_fvalues) {
            write_u32(out, fid_kv.first);
            write_string(out, fid_kv.second);
        }

        write_u8(out, facet_index.seq_id_hashes != nullptr ? 1 : 0);
        if(facet_index.seq_id_hashes != nullptr) {
            write_posting_list(out, facet_index.seq_id_hashes);
        }

        write_u64(out, facet_index.fhash_to_int64_map.size());
        for(const auto& fhash_kv: facet_index.fhash_to_int64_map) {
            write_u32(out, fhash_kv.first);
            write_i64(out, fhash_kv.second);
        }
    }
}

void facet_index_t::snapshot_read(std::istream& in) {
    facet_field_map.clear();
    next_facet_id = read_u32(in);

    const auto field_count = read_u64(in);
    for(uint64_t i = 0; i < field_count; i++) {
        auto field_name = read_string(in);
        facet_field_map.try_emplace(field_name);
        auto& facet_index = facet_field_map.at(field_name);

        facet_index.has_value_index = read_u8(in) != 0;
        facet_index.has_hash_index = read_u8(in) != 0;

        const auto fvalue_count = read_u64(in);
        for(uint64_t j = 0; j < fvalue_count; j++) {
            auto fvalue = read_string(in);
            facet_id_seq_ids_t fis;
            fis.facet_id = read_u32(in);
            const bool has_seq_ids = read_u8(in) != 0;
            if(has_seq_ids) {
                fis.seq_ids = read_raw_ids(in);
                const auto count = ids_t::num_ids(fis.seq_ids);
                fis.facet_count_it = facet_index.counts.emplace(fvalue, count, fis.facet_id);
            }
            facet_index.fvalue_seq_ids.emplace(fvalue, fis);
        }

        const auto fid_count = read_u64(in);
        for(uint64_t j = 0; j < fid_count; j++) {
            const auto fid = read_u32(in);
            auto fvalue = read_string(in);
            facet_index.fid_fvalues.emplace(fid, fvalue);
        }

        delete facet_index.seq_id_hashes;
        facet_index.seq_id_hashes = nullptr;
        const bool has_hashes = read_u8(in) != 0;
        if(has_hashes) {
            facet_index.seq_id_hashes = new posting_list_t(256);
            read_posting_list(in, facet_index.seq_id_hashes);
        }

        const auto fhash_count = read_u64(in);
        for(uint64_t j = 0; j < fhash_count; j++) {
            const auto fhash = read_u32(in);
            const auto value = read_i64(in);
            facet_index.fhash_to_int64_map.emplace(fhash, value);
        }
    }
}

Option<nlohmann::json> Index::read_snapshot_manifest(const std::string& snapshot_path) {
    std::ifstream in(snapshot_path, std::ios::binary);
    if(!in.good()) {
        return Option<nlohmann::json>(404, "Index snapshot not found.");
    }

    try {
        return Option<nlohmann::json>(read_manifest_header(in));
    } catch(const std::exception& e) {
        return Option<nlohmann::json>(400, e.what());
    }
}

Option<bool> Index::save_snapshot(const std::string& snapshot_path, const nlohmann::json& manifest,
                                  const std::function<Option<bool>()>& before_commit) const {
    auto begin = std::chrono::high_resolution_clock::now();
    const auto tmp_path = snapshot_path + ".tmp";
    std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
    if(!out.good()) {
        return Option<bool>(500, "Unable to open index snapshot for writing.");
    }

    try {
        std::shared_lock lock(mutex);
        write_manifest_header(out, manifest);

        write_u64(out, search_index.size());
        for(const auto& kv: search_index) {
            write_string(out, kv.first);
            write_art_tree(out, kv.second);
        }

        write_u64(out, numerical_index.size());
        for(const auto& kv: numerical_index) {
            write_string(out, kv.first);
            kv.second->snapshot_write(out);
        }

        write_u64(out, reference_index.size());
        for(const auto& kv: reference_index) {
            write_string(out, kv.first);
            kv.second->snapshot_write(out);
        }

        write_u64(out, object_array_reference_index.size());
        for(const auto& kv: object_array_reference_index) {
            write_string(out, kv.first);
            write_u64(out, kv.second->size());
            for(const auto& ref_kv: *kv.second) {
                write_u32(out, ref_kv.first.first);
                write_u32(out, ref_kv.first.second);
                write_u32(out, ref_kv.second);
            }
        }

        write_u64(out, range_index.size());
        for(const auto& kv: range_index) {
            write_string(out, kv.first);
            kv.second->snapshot_write(out, false);
        }

        write_u64(out, geo_range_index.size());
        for(const auto& kv: geo_range_index) {
            write_string(out, kv.first);
            kv.second->snapshot_write(out, true);
        }

        write_u64(out, geo_array_index.size());
        for(const auto& kv: geo_array_index) {
            write_string(out, kv.first);
            write_u64(out, kv.second->size());
            for(const auto& geo_kv: *kv.second) {
                write_u32(out, geo_kv.first);
                const auto count = static_cast<uint64_t>(geo_kv.second[0]);
                write_u64(out, count);
                for(uint64_t i = 0; i < count; i++) {
                    write_i64(out, geo_kv.second[i + 1]);
                }
            }
        }

        write_u64(out, field_geopolygon_index.size());
        for(const auto& kv: field_geopolygon_index) {
            write_string(out, kv.first);
            kv.second->snapshot_write(out);
        }

        write_vector_indexes(out, vector_index, tmp_path);

        facet_index_v4->snapshot_write(out);

        write_u64(out, sort_index.size());
        for(const auto& kv: sort_index) {
            write_string(out, kv.first);
            write_u64(out, kv.second->size());
            for(const auto& sort_kv: *kv.second) {
                write_u32(out, sort_kv.first);
                write_i64(out, sort_kv.second);
            }
        }

        write_u64(out, str_sort_index.size());
        for(const auto& kv: str_sort_index) {
            write_string(out, kv.first);
            kv.second->snapshot_write(out);
        }

        write_u64(out, infix_index.size());
        for(const auto& kv: infix_index) {
            write_string(out, kv.first);
            write_u64(out, kv.second.size());
            for(const auto* infix_set: kv.second) {
                write_u64(out, infix_set->size());
                for(auto it = infix_set->begin(); it != infix_set->end(); ++it) {
                    write_string(out, it.key());
                }
            }
        }

        write_id_list(out, seq_ids);

        write_u64(out, field_missing_index.size());
        for(const auto& kv: field_missing_index) {
            write_string(out, kv.first);
            write_id_list(out, kv.second);
        }

        out.close();
        if(!out.good()) {
            throw std::runtime_error("Unable to flush index snapshot.");
        }

        if(before_commit) {
            auto commit_op = before_commit();
            if(!commit_op.ok()) {
                delete_path(tmp_path, false);
                return commit_op;
            }
        }

        if(!rename_path(tmp_path, snapshot_path)) {
            delete_path(tmp_path, false);
            return Option<bool>(500, "Unable to move index snapshot into place.");
        }

        LOG(INFO) << "Saved index snapshot " << snapshot_path
                  << " in " << elapsed_ms_since(begin) << " ms"
                  << " (" << get_file_size_bytes(snapshot_path) << " bytes).";
    } catch(const std::exception& e) {
        out.close();
        delete_path(tmp_path, false);
        return Option<bool>(500, e.what());
    }

    return Option<bool>(true);
}

Option<nlohmann::json> Index::load_snapshot(const std::string& snapshot_path) {
    auto begin = std::chrono::high_resolution_clock::now();
    std::ifstream in(snapshot_path, std::ios::binary);
    if(!in.good()) {
        return Option<nlohmann::json>(404, "Index snapshot not found.");
    }

    try {
        std::unique_lock lock(mutex);
        if(seq_ids->num_ids() != 0) {
            return Option<nlohmann::json>(409, "Index snapshot can only be loaded into an empty index.");
        }

        auto manifest = read_manifest_header(in);

        auto search_count = read_u64(in);
        for(uint64_t i = 0; i < search_count; i++) {
            auto field_name = read_string(in);
            if(search_index.count(field_name) == 0) {
                throw std::runtime_error("Snapshot contains an unknown search field `" + field_name + "`.");
            }
            read_art_tree(in, search_index.at(field_name));
        }

        auto numerical_count = read_u64(in);
        for(uint64_t i = 0; i < numerical_count; i++) {
            auto field_name = read_string(in);
            if(numerical_index.count(field_name) == 0) {
                throw std::runtime_error("Snapshot contains an unknown numerical field `" + field_name + "`.");
            }
            numerical_index.at(field_name)->snapshot_read(in);
        }

        auto reference_count = read_u64(in);
        for(uint64_t i = 0; i < reference_count; i++) {
            auto field_name = read_string(in);
            if(reference_index.count(field_name) == 0) {
                throw std::runtime_error("Snapshot contains an unknown reference field `" + field_name + "`.");
            }
            reference_index.at(field_name)->snapshot_read(in);
        }

        auto object_ref_count = read_u64(in);
        for(uint64_t i = 0; i < object_ref_count; i++) {
            auto field_name = read_string(in);
            if(object_array_reference_index.count(field_name) == 0) {
                throw std::runtime_error("Snapshot contains an unknown object-array reference field `" + field_name + "`.");
            }
            auto* ref_map = object_array_reference_index.at(field_name);
            ref_map->clear();
            const auto entry_count = read_u64(in);
            for(uint64_t j = 0; j < entry_count; j++) {
                const auto seq_id = read_u32(in);
                const auto object_index = read_u32(in);
                const auto ref_seq_id = read_u32(in);
                ref_map->emplace(std::make_pair(seq_id, object_index), ref_seq_id);
            }
        }

        auto range_count = read_u64(in);
        for(uint64_t i = 0; i < range_count; i++) {
            auto field_name = read_string(in);
            if(range_index.count(field_name) == 0) {
                throw std::runtime_error("Snapshot contains an unknown range field `" + field_name + "`.");
            }
            range_index.at(field_name)->snapshot_read(in, false);
        }

        auto geo_range_count = read_u64(in);
        for(uint64_t i = 0; i < geo_range_count; i++) {
            auto field_name = read_string(in);
            if(geo_range_index.count(field_name) == 0) {
                throw std::runtime_error("Snapshot contains an unknown geo field `" + field_name + "`.");
            }
            geo_range_index.at(field_name)->snapshot_read(in, true);
        }

        auto geo_array_count = read_u64(in);
        for(uint64_t i = 0; i < geo_array_count; i++) {
            auto field_name = read_string(in);
            if(geo_array_index.count(field_name) == 0) {
                throw std::runtime_error("Snapshot contains an unknown geo array field `" + field_name + "`.");
            }
            auto* geo_map = geo_array_index.at(field_name);
            for(auto& kv: *geo_map) {
                delete [] kv.second;
            }
            geo_map->clear();

            const auto entry_count = read_u64(in);
            for(uint64_t j = 0; j < entry_count; j++) {
                const auto seq_id = read_u32(in);
                const auto count = read_u64(in);
                auto* packed = new int64_t[count + 1];
                packed[0] = static_cast<int64_t>(count);
                for(uint64_t k = 0; k < count; k++) {
                    packed[k + 1] = read_i64(in);
                }
                geo_map->emplace(seq_id, packed);
            }
        }

        auto geopolygon_count = read_u64(in);
        for(uint64_t i = 0; i < geopolygon_count; i++) {
            auto field_name = read_string(in);
            if(field_geopolygon_index.count(field_name) == 0) {
                throw std::runtime_error("Snapshot contains an unknown geopolygon field `" + field_name + "`.");
            }
            field_geopolygon_index.at(field_name)->snapshot_read(in);
        }

        read_vector_indexes(in, vector_index, snapshot_path);

        facet_index_v4->snapshot_read(in);

        auto sort_count = read_u64(in);
        for(uint64_t i = 0; i < sort_count; i++) {
            auto field_name = read_string(in);
            if(sort_index.count(field_name) == 0) {
                throw std::runtime_error("Snapshot contains an unknown sort field `" + field_name + "`.");
            }
            auto* sort_map = sort_index.at(field_name);
            sort_map->clear();
            const auto entry_count = read_u64(in);
            for(uint64_t j = 0; j < entry_count; j++) {
                const auto seq_id = read_u32(in);
                const auto value = read_i64(in);
                sort_map->emplace(seq_id, value);
            }
        }

        auto str_sort_count = read_u64(in);
        for(uint64_t i = 0; i < str_sort_count; i++) {
            auto field_name = read_string(in);
            if(str_sort_index.count(field_name) == 0) {
                throw std::runtime_error("Snapshot contains an unknown string sort field `" + field_name + "`.");
            }
            str_sort_index.at(field_name)->snapshot_read(in);
        }

        auto infix_count = read_u64(in);
        for(uint64_t i = 0; i < infix_count; i++) {
            auto field_name = read_string(in);
            if(infix_index.count(field_name) == 0) {
                throw std::runtime_error("Snapshot contains an unknown infix field `" + field_name + "`.");
            }
            auto& infix_sets = infix_index.at(field_name);
            const auto set_count = read_u64(in);
            if(set_count != infix_sets.size()) {
                throw std::runtime_error("Snapshot infix shard count does not match schema.");
            }
            for(uint64_t j = 0; j < set_count; j++) {
                infix_sets[j]->clear();
                const auto value_count = read_u64(in);
                for(uint64_t k = 0; k < value_count; k++) {
                    infix_sets[j]->insert(read_string(in));
                }
            }
        }

        read_id_list(in, seq_ids);

        auto missing_count = read_u64(in);
        for(uint64_t i = 0; i < missing_count; i++) {
            auto field_name = read_string(in);
            if(field_missing_index.count(field_name) == 0) {
                throw std::runtime_error("Snapshot contains an unknown missing-value field `" + field_name + "`.");
            }
            read_id_list(in, field_missing_index.at(field_name));
        }

        num_documents = seq_ids->num_ids();
        LOG(INFO) << "Loaded index snapshot " << snapshot_path
                  << " in " << elapsed_ms_since(begin) << " ms"
                  << " (" << get_file_size_bytes(snapshot_path) << " bytes).";
        return Option<nlohmann::json>(manifest);
    } catch(const std::exception& e) {
        return Option<nlohmann::json>(500, e.what());
    }
}
