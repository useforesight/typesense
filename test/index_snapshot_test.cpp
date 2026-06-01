#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <sys/stat.h>
#include <unistd.h>

#include <collection.h>
#include <collection_manager.h>
#include <field.h>
#include <index.h>
#include <store.h>
#include <string_utils.h>
#include <threadpool.h>
#include <tsconfig.h>

namespace {

std::vector<field> parse_fields(nlohmann::json fields_json, const std::string& collection_name) {
    std::vector<field> parsed_fields;
    std::string fallback_field_type;
    size_t num_auto_detect_fields = 0;

    for(auto& field_json: fields_json) {
        auto op = field::json_field_to_field(false, field_json, parsed_fields, fallback_field_type,
                                             num_auto_detect_fields, collection_name);
        EXPECT_TRUE(op.ok()) << op.error();
    }

    return parsed_fields;
}

nlohmann::json collection_meta_for(const std::string& collection_name,
                                   uint32_t collection_id,
                                   const std::vector<field>& fields,
                                   const std::string& default_sorting_field) {
    nlohmann::json fields_json = nlohmann::json::array();
    auto fields_op = field::fields_to_json_fields(fields, default_sorting_field, fields_json);
    EXPECT_TRUE(fields_op.ok()) << fields_op.error();

    nlohmann::json collection_meta;
    collection_meta[Collection::COLLECTION_NAME_KEY] = collection_name;
    collection_meta[Collection::COLLECTION_ID_KEY] = collection_id;
    collection_meta[Collection::COLLECTION_SEARCH_FIELDS_KEY] = fields_json;
    collection_meta[Collection::COLLECTION_DEFAULT_SORTING_FIELD_KEY] = default_sorting_field;
    collection_meta[Collection::COLLECTION_CREATED] = 123;
    collection_meta[Collection::COLLECTION_NUM_MEMORY_SHARDS] = 1;
    collection_meta[Collection::COLLECTION_FALLBACK_FIELD_TYPE] = "";
    collection_meta[Collection::COLLECTION_SYMBOLS_TO_INDEX] = nlohmann::json::array();
    collection_meta[Collection::COLLECTION_SEPARATORS] = nlohmann::json::array();
    collection_meta[Collection::COLLECTION_ENABLE_NESTED_FIELDS] = false;
    collection_meta[Collection::COLLECTION_SYNONYM_SETS] = nlohmann::json::array();
    collection_meta[Collection::COLLECTION_curation_sets] = nlohmann::json::array();

    return collection_meta;
}

Collection* new_snapshot_collection(Store& store, ThreadPool& thread_pool, const std::vector<field>& fields,
                                    uint32_t next_seq_id) {
    (void) thread_pool;
    spp::sparse_hash_map<std::string, std::string> referenced_in;
    spp::sparse_hash_map<std::string, std::set<reference_pair_t>> async_referenced_ins;
    std::vector<std::string> synonym_sets;
    std::vector<std::string> curation_sets;

    auto op = Collection::new_collection("snapshot_coll", 0, 123, next_seq_id, &store, fields, "points",
                                         1.0f, "", {}, {}, false, nullptr, referenced_in,
                                         nlohmann::json::object(), async_referenced_ins,
                                         synonym_sets, curation_sets, true);
    EXPECT_TRUE(op.ok()) << op.error();
    return op.get();
}

nlohmann::json search_with_vector_query(Collection* collection, const std::string& vector_query) {
    auto search_op = collection->search("*", {}, "", {}, {}, {0}, 10, 1, FREQUENCY, {true},
                                        Index::DROP_TOKENS_THRESHOLD,
                                        spp::sparse_hash_set<std::string>(),
                                        spp::sparse_hash_set<std::string>(), 10, "", 30, 5,
                                        "", 10, {}, {}, {}, 0,
                                        "<mark>", "</mark>", {}, 1000, true, false, true, "",
                                        false, 6000 * 1000, 4, 7, fallback, 4, {off},
                                        32767, 32767, 2, false, true, vector_query);
    EXPECT_TRUE(search_op.ok()) << search_op.error();
    return search_op.get();
}

std::vector<std::string> hit_ids(const nlohmann::json& results) {
    std::vector<std::string> ids;
    for(const auto& hit: results["hits"]) {
        ids.push_back(hit["document"]["id"].get<std::string>());
    }
    return ids;
}

uint32_t index_snapshot_version(Store& store, const std::string& collection_name = "snapshot_coll") {
    std::string version_str;
    StoreStatus status = store.get(Collection::get_index_snapshot_version_key(collection_name), version_str);
    if(status != StoreStatus::FOUND) {
        return 0;
    }
    return StringUtils::deserialize_uint32_t(version_str);
}

}  // namespace

TEST(IndexSnapshotTest, SnapshotRoundTripsMemoryIndexWithoutDocumentReplay) {
    const std::string db_path = "/tmp/typesense_test/index_snapshot_test_db";
    const std::string snapshot_path = "/tmp/typesense_test/index_snapshot_test.idxsnap";
    system(("rm -rf " + db_path + " " + snapshot_path + " " + snapshot_path + ".tmp && mkdir -p " + db_path).c_str());

    Store store(db_path);
    ThreadPool thread_pool(4);
    std::atomic<bool> quit = false;
    CollectionManager::get_instance().init(&store, &thread_pool, 1.0f, "auth_key", quit);

    auto fields_json = R"([
        {"name": "title", "type": "string"},
        {"name": "starring", "type": "string", "infix": true},
        {"name": "category", "type": "string[]", "facet": true, "optional": true},
        {"name": "year", "type": "int32", "facet": true},
        {"name": "price", "type": "int32", "range_index": true},
        {"name": "location", "type": "geopoint", "optional": true, "track_missing_values": true},
        {"name": "points", "type": "int32"}
    ])"_json;

    auto fields = parse_fields(fields_json, "snapshot_coll");
    auto collection_meta = collection_meta_for("snapshot_coll", 0, fields, "points");
    auto* collection = new_snapshot_collection(store, thread_pool, fields, 0);

    ASSERT_TRUE(collection->add(R"({"id":"1","title":"fast red jeep","starring":"Kieran-Graham","category":["cars","tools"],"year":2024,"price":19,"location":[48.90615,2.34358],"points":30})").ok());
    ASSERT_TRUE(collection->add(R"({"id":"2","title":"slow blue truck","starring":"Ada-Lovelace","category":["cars"],"year":2021,"price":29,"location":[48.8462,2.34515],"points":20})").ok());
    ASSERT_TRUE(collection->add(R"({"id":"3","title":"quiet green train","starring":"Grace-Hopper","category":["rail"],"year":2019,"price":9,"points":10})").ok());

    std::vector<sort_by> sort_fields = {sort_by("points", "DESC")};

    auto text_before = collection->search("jeep", {"title"}, "", {}, sort_fields, {0}, 10, 1,
                                          FREQUENCY, {false}).get();
    auto filter_before = collection->search("*", {}, "price:<25 && year:>=2019", {}, sort_fields, {0}, 10, 1,
                                            FREQUENCY, {false}).get();
    auto facet_before = collection->search("*", {}, "", {"category"}, sort_fields, {0}, 10, 1,
                                           FREQUENCY, {false}).get();
    auto infix_before = collection->search("gra", {"starring"}, "", {}, sort_fields, {0}, 10, 1,
                                           FREQUENCY, {false}, Index::DROP_TOKENS_THRESHOLD,
                                           spp::sparse_hash_set<std::string>(),
                                           spp::sparse_hash_set<std::string>(), 10, "", 30, 4, "",
                                           Index::TYPO_TOKENS_THRESHOLD, "", "", {}, 3, "<mark>", "</mark>",
                                           {}, 1000000, true, false, true, "", false, 6000 * 1000, 4, 7,
                                           fallback, 4, {always}).get();
    auto geo_before = collection->search("*", {}, "location: ([48.90615, 2.34358], radius: 3 km)",
                                         {}, sort_fields, {0}, 10, 1, FREQUENCY, {false}).get();

    const auto snapshot_version = index_snapshot_version(store);
    auto save_op = collection->save_index_snapshot(snapshot_path, collection_meta, snapshot_version);
    ASSERT_TRUE(save_op.ok()) << save_op.error();

    auto expected_manifest = collection->build_index_snapshot_manifest(collection_meta, snapshot_version);

    std::string next_seq_id_str;
    ASSERT_EQ(StoreStatus::FOUND, store.get(Collection::get_next_seq_id_key("snapshot_coll"), next_seq_id_str));
    const auto next_seq_id = StringUtils::deserialize_uint32_t(next_seq_id_str);

    delete collection;

    auto* restored = new_snapshot_collection(store, thread_pool, fields, next_seq_id);
    auto load_op = restored->load_index_snapshot(snapshot_path, expected_manifest);
    ASSERT_TRUE(load_op.ok()) << load_op.error();
    ASSERT_EQ(3, restored->get_num_documents());
    ASSERT_EQ(3, restored->_get_index()->num_seq_ids());

    auto text_after = restored->search("jeep", {"title"}, "", {}, sort_fields, {0}, 10, 1,
                                       FREQUENCY, {false}).get();
    auto filter_after = restored->search("*", {}, "price:<25 && year:>=2019", {}, sort_fields, {0}, 10, 1,
                                         FREQUENCY, {false}).get();
    auto facet_after = restored->search("*", {}, "", {"category"}, sort_fields, {0}, 10, 1,
                                        FREQUENCY, {false}).get();
    auto infix_after = restored->search("gra", {"starring"}, "", {}, sort_fields, {0}, 10, 1,
                                        FREQUENCY, {false}, Index::DROP_TOKENS_THRESHOLD,
                                        spp::sparse_hash_set<std::string>(),
                                        spp::sparse_hash_set<std::string>(), 10, "", 30, 4, "",
                                        Index::TYPO_TOKENS_THRESHOLD, "", "", {}, 3, "<mark>", "</mark>",
                                        {}, 1000000, true, false, true, "", false, 6000 * 1000, 4, 7,
                                        fallback, 4, {always}).get();
    auto geo_after = restored->search("*", {}, "location: ([48.90615, 2.34358], radius: 3 km)",
                                      {}, sort_fields, {0}, 10, 1, FREQUENCY, {false}).get();

    ASSERT_EQ(text_before["hits"][0]["document"]["id"], text_after["hits"][0]["document"]["id"]);
    ASSERT_EQ(filter_before["found"], filter_after["found"]);
    ASSERT_EQ(facet_before["facet_counts"], facet_after["facet_counts"]);
    ASSERT_EQ(infix_before["hits"][0]["document"]["id"], infix_after["hits"][0]["document"]["id"]);
    ASSERT_EQ(geo_before["found"], geo_after["found"]);

    delete restored;
    CollectionManager::get_instance().dispose();
    thread_pool.shutdown();
}

TEST(IndexSnapshotTest, SnapshotManifestMismatchRejectsStaleIndex) {
    const std::string db_path = "/tmp/typesense_test/index_snapshot_stale_db";
    const std::string snapshot_path = "/tmp/typesense_test/index_snapshot_stale.idxsnap";
    system(("rm -rf " + db_path + " " + snapshot_path + " " + snapshot_path + ".tmp && mkdir -p " + db_path).c_str());

    Store store(db_path);
    ThreadPool thread_pool(4);
    std::atomic<bool> quit = false;
    CollectionManager::get_instance().init(&store, &thread_pool, 1.0f, "auth_key", quit);

    auto fields_json = R"([
        {"name": "title", "type": "string"},
        {"name": "points", "type": "int32"}
    ])"_json;

    auto fields = parse_fields(fields_json, "snapshot_coll");
    auto collection_meta = collection_meta_for("snapshot_coll", 0, fields, "points");
    auto* collection = new_snapshot_collection(store, thread_pool, fields, 0);

    ASSERT_TRUE(collection->add(R"({"id":"1","title":"fast red jeep","points":30})").ok());

    const auto snapshot_version = index_snapshot_version(store);
    auto save_op = collection->save_index_snapshot(snapshot_path, collection_meta, snapshot_version);
    ASSERT_TRUE(save_op.ok()) << save_op.error();

    auto expected_manifest = collection->build_index_snapshot_manifest(collection_meta, snapshot_version);
    auto stale_manifest = expected_manifest;
    stale_manifest["index_snapshot_version"] = snapshot_version + 1;
    ASSERT_TRUE(store.insert("$unrelated_index_snapshot_noise", "value"));

    std::string next_seq_id_str;
    ASSERT_EQ(StoreStatus::FOUND, store.get(Collection::get_next_seq_id_key("snapshot_coll"), next_seq_id_str));
    const auto next_seq_id = StringUtils::deserialize_uint32_t(next_seq_id_str);

    delete collection;

    auto* restored = new_snapshot_collection(store, thread_pool, fields, next_seq_id);
    auto stale_load_op = restored->load_index_snapshot(snapshot_path, stale_manifest);
    ASSERT_FALSE(stale_load_op.ok());
    ASSERT_EQ(409, stale_load_op.code());
    ASSERT_EQ("Index snapshot manifest does not match current collection state. Mismatched fields: "
              "index_snapshot_version snapshot=" + std::to_string(snapshot_version) +
              " expected=" + std::to_string(snapshot_version + 1),
              stale_load_op.error());
    ASSERT_EQ(0, restored->get_num_documents());
    ASSERT_EQ(0, restored->_get_index()->num_seq_ids());

    auto fresh_load_op = restored->load_index_snapshot(snapshot_path, expected_manifest);
    ASSERT_TRUE(fresh_load_op.ok()) << fresh_load_op.error();
    ASSERT_EQ(1, restored->get_num_documents());
    ASSERT_EQ(1, restored->_get_index()->num_seq_ids());

    delete restored;
    CollectionManager::get_instance().dispose();
    thread_pool.shutdown();
}

TEST(IndexSnapshotTest, SaveIndexSnapshotsWritesCurrentCollectionState) {
    const std::string db_path = "/tmp/typesense_test/index_snapshot_shutdown_db";
    const std::string snapshot_dir = "/tmp/typesense_test/index_snapshot_shutdown";
    const std::string collection_name = "shutdown_snapshot_coll";
    system(("rm -rf " + db_path + " " + snapshot_dir + " && mkdir -p " + db_path + " " + snapshot_dir).c_str());

    Store store(db_path);
    ThreadPool thread_pool(4);
    std::atomic<bool> quit = false;
    auto& collection_manager = CollectionManager::get_instance();
    collection_manager.init(&store, &thread_pool, 1.0f, "auth_key", quit);

    Config::get_instance().set_enable_index_snapshot(true);
    Config::get_instance().set_index_snapshot_dir(snapshot_dir);

    std::vector<field> fields = {
        field("title", field_types::STRING, false),
        field("points", field_types::INT32, false)
    };
    auto create_op = collection_manager.create_collection(collection_name, 1, fields, "points");
    ASSERT_TRUE(create_op.ok()) << create_op.error();
    auto* collection = create_op.get();
    const auto snapshot_path = collection->get_index_snapshot_path(snapshot_dir);

    ASSERT_TRUE(collection->add(R"({"id":"1","title":"fast red jeep","points":30})").ok());
    collection_manager.save_index_snapshots();
    ASSERT_TRUE(file_exists(snapshot_path));

    auto first_manifest_op = Index::read_snapshot_manifest(snapshot_path);
    ASSERT_TRUE(first_manifest_op.ok()) << first_manifest_op.error();
    auto first_manifest = first_manifest_op.get();
    ASSERT_EQ(index_snapshot_version(store, collection_name),
              first_manifest["index_snapshot_version"].get<uint32_t>());

    ASSERT_TRUE(collection->add(R"({"id":"2","title":"slow blue truck","points":20})").ok());
    ASSERT_GT(index_snapshot_version(store, collection_name),
              first_manifest["index_snapshot_version"].get<uint32_t>());
    collection_manager.save_index_snapshots();

    auto second_manifest_op = Index::read_snapshot_manifest(snapshot_path);
    ASSERT_TRUE(second_manifest_op.ok()) << second_manifest_op.error();
    auto second_manifest = second_manifest_op.get();
    ASSERT_EQ(index_snapshot_version(store, collection_name),
              second_manifest["index_snapshot_version"].get<uint32_t>());

    struct stat snapshot_stat_before{};
    ASSERT_EQ(0, stat(snapshot_path.c_str(), &snapshot_stat_before));
    sleep(1);
    collection_manager.save_index_snapshots();
    struct stat snapshot_stat_after{};
    ASSERT_EQ(0, stat(snapshot_path.c_str(), &snapshot_stat_after));
    ASSERT_EQ(snapshot_stat_before.st_mtime, snapshot_stat_after.st_mtime);

    Config::get_instance().set_enable_index_snapshot(false);
    Config::get_instance().set_index_snapshot_dir("");
    collection_manager.dispose();
    thread_pool.shutdown();
}

TEST(IndexSnapshotTest, SaveIndexSnapshotDoesNotReplaceExistingSnapshotWhenCommitValidationFails) {
    const std::string db_path = "/tmp/typesense_test/index_snapshot_commit_validation_db";
    const std::string snapshot_path = "/tmp/typesense_test/index_snapshot_commit_validation.idxsnap";
    system(("rm -rf " + db_path + " " + snapshot_path + " " + snapshot_path + ".tmp && mkdir -p " + db_path).c_str());

    Store store(db_path);
    ThreadPool thread_pool(4);
    std::atomic<bool> quit = false;
    CollectionManager::get_instance().init(&store, &thread_pool, 1.0f, "auth_key", quit);

    auto fields_json = R"([
        {"name": "title", "type": "string"},
        {"name": "points", "type": "int32"}
    ])"_json;
    auto fields = parse_fields(fields_json, "snapshot_coll");
    auto collection_meta = collection_meta_for("snapshot_coll", 0, fields, "points");
    auto* collection = new_snapshot_collection(store, thread_pool, fields, 0);

    ASSERT_TRUE(collection->add(R"({"id":"1","title":"fast red jeep","points":30})").ok());
    const auto first_snapshot_version = index_snapshot_version(store);
    auto first_save_op = collection->save_index_snapshot(snapshot_path, collection_meta, first_snapshot_version);
    ASSERT_TRUE(first_save_op.ok()) << first_save_op.error();

    auto first_manifest_op = Index::read_snapshot_manifest(snapshot_path);
    ASSERT_TRUE(first_manifest_op.ok()) << first_manifest_op.error();
    auto first_manifest = first_manifest_op.get();
    ASSERT_EQ(first_snapshot_version, first_manifest["index_snapshot_version"].get<uint32_t>());
    ASSERT_EQ(1, first_manifest["num_documents"].get<uint32_t>());

    ASSERT_TRUE(collection->add(R"({"id":"2","title":"slow blue truck","points":20})").ok());
    auto rejected_save_op = collection->save_index_snapshot(snapshot_path, collection_meta,
                                                           index_snapshot_version(store), []() {
        return Option<bool>(409, "reject snapshot commit");
    });
    ASSERT_FALSE(rejected_save_op.ok());
    ASSERT_FALSE(file_exists(snapshot_path + ".tmp"));

    auto current_manifest_op = Index::read_snapshot_manifest(snapshot_path);
    ASSERT_TRUE(current_manifest_op.ok()) << current_manifest_op.error();
    EXPECT_EQ(first_manifest, current_manifest_op.get());

    delete collection;
    CollectionManager::get_instance().dispose();
    thread_pool.shutdown();
}

TEST(IndexSnapshotTest, DropCollectionRemovesIndexSnapshotFiles) {
    const std::string db_path = "/tmp/typesense_test/index_snapshot_drop_db";
    const std::string snapshot_dir = "/tmp/typesense_test/index_snapshot_drop";
    const std::string collection_name = "drop_snapshot_coll";
    system(("rm -rf " + db_path + " " + snapshot_dir + " && mkdir -p " + db_path + " " + snapshot_dir).c_str());

    Store store(db_path);
    ThreadPool thread_pool(4);
    std::atomic<bool> quit = false;
    auto& collection_manager = CollectionManager::get_instance();
    collection_manager.init(&store, &thread_pool, 1.0f, "auth_key", quit);

    Config::get_instance().set_enable_index_snapshot(true);
    Config::get_instance().set_index_snapshot_dir(snapshot_dir);

    std::vector<field> fields = {
        field("title", field_types::STRING, false),
        field("points", field_types::INT32, false)
    };
    auto create_op = collection_manager.create_collection(collection_name, 1, fields, "points");
    ASSERT_TRUE(create_op.ok()) << create_op.error();
    auto* collection = create_op.get();
    const auto snapshot_path = collection->get_index_snapshot_path(snapshot_dir);

    ASSERT_TRUE(collection->add(R"({"id":"1","title":"fast red jeep","points":30})").ok());
    collection_manager.save_index_snapshots();
    ASSERT_TRUE(file_exists(snapshot_path));

    std::ofstream tmp_snapshot(snapshot_path + ".tmp");
    tmp_snapshot << "partial";
    tmp_snapshot.close();
    ASSERT_TRUE(file_exists(snapshot_path + ".tmp"));

    auto drop_op = collection_manager.drop_collection(collection_name, true);
    ASSERT_TRUE(drop_op.ok()) << drop_op.error();
    ASSERT_FALSE(file_exists(snapshot_path));
    ASSERT_FALSE(file_exists(snapshot_path + ".tmp"));

    Config::get_instance().set_enable_index_snapshot(false);
    Config::get_instance().set_index_snapshot_dir("");
    collection_manager.dispose();
    thread_pool.shutdown();
}

TEST(IndexSnapshotTest, StartupRemovesStaleIndexSnapshotFiles) {
    const std::string db_path = "/tmp/typesense_test/index_snapshot_cleanup_db";
    const std::string snapshot_dir = "/tmp/typesense_test/index_snapshot_cleanup";
    system(("rm -rf " + db_path + " " + snapshot_dir + " && mkdir -p " + db_path + " " + snapshot_dir).c_str());

    auto fields_json = R"([
        {"name": "title", "type": "string"},
        {"name": "points", "type": "int32"}
    ])"_json;
    auto fields = parse_fields(fields_json, "snapshot_coll");
    auto collection_meta = collection_meta_for("snapshot_coll", 0, fields, "points");

    Store store(db_path);
    ASSERT_TRUE(store.insert(Collection::get_meta_key("snapshot_coll"), collection_meta.dump()));
    ASSERT_TRUE(store.insert(Collection::get_next_seq_id_key("snapshot_coll"),
                             StringUtils::serialize_uint32_t(0)));

    const std::string stale_snapshot_path = snapshot_dir + "/42.idxsnap";
    const std::string stale_tmp_snapshot_path = snapshot_dir + "/42.idxsnap.tmp";
    const std::string interrupted_save_payload_path = snapshot_dir + "/42.idxsnap.tmp.payload.ABCDEF";
    const std::string interrupted_load_payload_path = snapshot_dir + "/0.idxsnap.payload.ABCDEF";
    const std::string unrelated_snapshot_path = snapshot_dir + "/not-a-collection.idxsnap";
    {
        std::ofstream stale_snapshot(stale_snapshot_path);
        stale_snapshot << "stale";
        std::ofstream stale_tmp_snapshot(stale_tmp_snapshot_path);
        stale_tmp_snapshot << "stale tmp";
        std::ofstream interrupted_save_payload(interrupted_save_payload_path);
        interrupted_save_payload << "stale save payload";
        std::ofstream interrupted_load_payload(interrupted_load_payload_path);
        interrupted_load_payload << "stale load payload";
        std::ofstream unrelated_snapshot(unrelated_snapshot_path);
        unrelated_snapshot << "leave me alone";
    }

    ASSERT_TRUE(file_exists(stale_snapshot_path));
    ASSERT_TRUE(file_exists(stale_tmp_snapshot_path));
    ASSERT_TRUE(file_exists(interrupted_save_payload_path));
    ASSERT_TRUE(file_exists(interrupted_load_payload_path));
    ASSERT_TRUE(file_exists(unrelated_snapshot_path));

    ThreadPool thread_pool(4);
    std::atomic<bool> quit = false;
    auto& collection_manager = CollectionManager::get_instance();
    collection_manager.init(&store, &thread_pool, 1.0f, "auth_key", quit);

    Config::get_instance().set_enable_index_snapshot(true);
    Config::get_instance().set_index_snapshot_dir(snapshot_dir);

    auto load_op = collection_manager.load(1, 100);
    ASSERT_TRUE(load_op.ok()) << load_op.error();

    auto collection = collection_manager.get_collection("snapshot_coll");
    ASSERT_NE(nullptr, collection);
    ASSERT_TRUE(file_exists(collection->get_index_snapshot_path(snapshot_dir)));
    ASSERT_FALSE(file_exists(stale_snapshot_path));
    ASSERT_FALSE(file_exists(stale_tmp_snapshot_path));
    ASSERT_FALSE(file_exists(interrupted_save_payload_path));
    ASSERT_FALSE(file_exists(interrupted_load_payload_path));
    ASSERT_TRUE(file_exists(unrelated_snapshot_path));
    collection.reset();

    Config::get_instance().set_enable_index_snapshot(false);
    Config::get_instance().set_index_snapshot_dir("");
    collection_manager.dispose();
    thread_pool.shutdown();
}

TEST(IndexSnapshotTest, InterruptedStartupDoesNotPublishOrSnapshotPartialIndex) {
    const std::string db_path = "/tmp/typesense_test/index_snapshot_interrupted_startup_db";
    const std::string snapshot_dir = "/tmp/typesense_test/index_snapshot_interrupted_startup";
    system(("rm -rf " + db_path + " " + snapshot_dir + " && mkdir -p " + db_path + " " + snapshot_dir).c_str());

    Store store(db_path);
    ThreadPool thread_pool(4);
    std::atomic<bool> quit = false;
    auto& collection_manager = CollectionManager::get_instance();
    collection_manager.init(&store, &thread_pool, 1.0f, "auth_key", quit);

    auto fields_json = R"([
        {"name": "title", "type": "string"},
        {"name": "points", "type": "int32"}
    ])"_json;
    auto fields = parse_fields(fields_json, "snapshot_coll");
    auto collection_meta = collection_meta_for("snapshot_coll", 0, fields, "points");
    auto* source_collection = new_snapshot_collection(store, thread_pool, fields, 0);
    ASSERT_TRUE(source_collection->add(R"({"id":"1","title":"fast red jeep","points":30})").ok());
    ASSERT_TRUE(source_collection->add(R"({"id":"2","title":"slow blue truck","points":20})").ok());
    ASSERT_TRUE(store.insert(Collection::get_meta_key("snapshot_coll"), collection_meta.dump()));
    delete source_collection;

    Config::get_instance().set_enable_index_snapshot(true);
    Config::get_instance().set_index_snapshot_dir(snapshot_dir);
    quit = true;

    auto load_op = collection_manager.load(1, 1);
    ASSERT_TRUE(load_op.ok()) << load_op.error();
    ASSERT_EQ(nullptr, collection_manager.get_collection("snapshot_coll"));
    ASSERT_FALSE(file_exists(snapshot_dir + "/0.idxsnap"));

    Config::get_instance().set_enable_index_snapshot(false);
    Config::get_instance().set_index_snapshot_dir("");
    collection_manager.dispose();
    thread_pool.shutdown();
}

TEST(IndexSnapshotTest, SnapshotManifestDocumentCountIsInformational) {
    const std::string db_path = "/tmp/typesense_test/index_snapshot_doc_count_db";
    const std::string snapshot_path = "/tmp/typesense_test/index_snapshot_doc_count.idxsnap";
    system(("rm -rf " + db_path + " " + snapshot_path + " " + snapshot_path + ".tmp && mkdir -p " + db_path).c_str());

    Store store(db_path);
    ThreadPool thread_pool(4);
    std::atomic<bool> quit = false;
    CollectionManager::get_instance().init(&store, &thread_pool, 1.0f, "auth_key", quit);

    auto fields_json = R"([
        {"name": "title", "type": "string"},
        {"name": "points", "type": "int32"}
    ])"_json;

    auto fields = parse_fields(fields_json, "snapshot_coll");
    auto collection_meta = collection_meta_for("snapshot_coll", 0, fields, "points");
    auto* collection = new_snapshot_collection(store, thread_pool, fields, 0);

    ASSERT_TRUE(collection->add(R"({"id":"1","title":"fast red jeep","points":30})").ok());

    const auto snapshot_version = index_snapshot_version(store);
    auto save_op = collection->save_index_snapshot(snapshot_path, collection_meta, snapshot_version);
    ASSERT_TRUE(save_op.ok()) << save_op.error();

    auto expected_manifest = collection->build_index_snapshot_manifest(collection_meta, snapshot_version);
    expected_manifest["num_documents"] = 42;

    {
        std::fstream snapshot_file(snapshot_path, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(snapshot_file.good());
        std::string snapshot_bytes((std::istreambuf_iterator<char>(snapshot_file)),
                                   std::istreambuf_iterator<char>());
        const std::string original_count = "\"num_documents\":1";
        const std::string tampered_count = "\"num_documents\":9";
        const auto count_pos = snapshot_bytes.find(original_count);
        ASSERT_NE(std::string::npos, count_pos);
        snapshot_file.clear();
        snapshot_file.seekp(static_cast<std::streamoff>(count_pos));
        snapshot_file.write(tampered_count.data(), static_cast<std::streamsize>(tampered_count.size()));
        ASSERT_TRUE(snapshot_file.good());
    }

    std::string next_seq_id_str;
    ASSERT_EQ(StoreStatus::FOUND, store.get(Collection::get_next_seq_id_key("snapshot_coll"), next_seq_id_str));
    const auto next_seq_id = StringUtils::deserialize_uint32_t(next_seq_id_str);

    delete collection;

    auto* restored = new_snapshot_collection(store, thread_pool, fields, next_seq_id);
    auto load_op = restored->load_index_snapshot(snapshot_path, expected_manifest);
    ASSERT_TRUE(load_op.ok()) << load_op.error();
    ASSERT_EQ(1, restored->get_num_documents());
    ASSERT_EQ(1, restored->_get_index()->num_seq_ids());

    delete restored;
    CollectionManager::get_instance().dispose();
    thread_pool.shutdown();
}

TEST(IndexSnapshotTest, SnapshotRoundTripsVectorIndexWithDeletedLabels) {
    const std::string db_path = "/tmp/typesense_test/index_snapshot_vector_db";
    const std::string snapshot_path = "/tmp/typesense_test/index_snapshot_vector.idxsnap";
    system(("rm -rf " + db_path + " " + snapshot_path + " " + snapshot_path + ".tmp && mkdir -p " + db_path).c_str());

    Store store(db_path);
    ThreadPool thread_pool(4);
    std::atomic<bool> quit = false;
    CollectionManager::get_instance().init(&store, &thread_pool, 1.0f, "auth_key", quit);

    auto fields_json = R"([
        {"name": "title", "type": "string"},
        {"name": "points", "type": "int32"},
        {"name": "vec", "type": "float[]", "num_dim": 4}
    ])"_json;

    auto fields = parse_fields(fields_json, "snapshot_coll");
    auto collection_meta = collection_meta_for("snapshot_coll", 0, fields, "points");
    auto* collection = new_snapshot_collection(store, thread_pool, fields, 0);

    ASSERT_TRUE(collection->add(R"({"id":"0","title":"alpha","points":10,"vec":[0.851758,0.909671,0.823431,0.372063]})").ok());
    ASSERT_TRUE(collection->add(R"({"id":"1","title":"beta","points":20,"vec":[0.97826,0.933157,0.39557,0.306488]})").ok());
    ASSERT_TRUE(collection->add(R"({"id":"2","title":"gamma","points":30,"vec":[0.230606,0.634397,0.514009,0.399594]})").ok());
    ASSERT_TRUE(collection->remove("1").ok());

    auto* vector_index = collection->_get_index()->_get_vector_index().at("vec")->vecdex;
    ASSERT_EQ(16, vector_index->getMaxElements());
    ASSERT_EQ(3, vector_index->getCurrentElementCount());
    ASSERT_EQ(1, vector_index->getDeletedCount());

    auto vector_before = search_with_vector_query(collection, "vec:([0.851758,0.909671,0.823431,0.372063], k: 10)");
    ASSERT_EQ(2, vector_before["found"].get<size_t>());
    ASSERT_EQ((std::vector<std::string>{"0", "2"}), hit_ids(vector_before));

    const auto snapshot_version = index_snapshot_version(store);
    auto save_op = collection->save_index_snapshot(snapshot_path, collection_meta, snapshot_version);
    ASSERT_TRUE(save_op.ok()) << save_op.error();

    auto expected_manifest = collection->build_index_snapshot_manifest(collection_meta, snapshot_version);

    std::string next_seq_id_str;
    ASSERT_EQ(StoreStatus::FOUND, store.get(Collection::get_next_seq_id_key("snapshot_coll"), next_seq_id_str));
    const auto next_seq_id = StringUtils::deserialize_uint32_t(next_seq_id_str);

    delete collection;

    auto* restored = new_snapshot_collection(store, thread_pool, fields, next_seq_id);
    auto load_op = restored->load_index_snapshot(snapshot_path, expected_manifest);
    ASSERT_TRUE(load_op.ok()) << load_op.error();
    ASSERT_EQ(2, restored->get_num_documents());
    ASSERT_EQ(2, restored->_get_index()->num_seq_ids());

    auto* restored_vector_index = restored->_get_index()->_get_vector_index().at("vec")->vecdex;
    ASSERT_EQ(16, restored_vector_index->getMaxElements());
    ASSERT_EQ(3, restored_vector_index->getCurrentElementCount());
    ASSERT_EQ(1, restored_vector_index->getDeletedCount());

    auto vector_after = search_with_vector_query(restored, "vec:([0.851758,0.909671,0.823431,0.372063], k: 10)");
    ASSERT_EQ(hit_ids(vector_before), hit_ids(vector_after));

    delete restored;
    CollectionManager::get_instance().dispose();
    thread_pool.shutdown();
}

TEST(IndexSnapshotTest, SnapshotRoundTripsMultipleVectorAndGeoPolygonFields) {
    const std::string db_path = "/tmp/typesense_test/index_snapshot_multi_vector_geo_db";
    const std::string snapshot_path = "/tmp/typesense_test/index_snapshot_multi_vector_geo.idxsnap";
    system(("rm -rf " + db_path + " " + snapshot_path + " " + snapshot_path + ".tmp && mkdir -p " + db_path).c_str());

    Store store(db_path);
    ThreadPool thread_pool(4);
    std::atomic<bool> quit = false;
    CollectionManager::get_instance().init(&store, &thread_pool, 1.0f, "auth_key", quit);

    auto fields_json = R"([
        {"name": "title", "type": "string"},
        {"name": "points", "type": "int32"},
        {"name": "vec", "type": "float[]", "num_dim": 4},
        {"name": "embedding", "type": "float[]", "num_dim": 3},
        {"name": "service_area", "type": "geopolygon"},
        {"name": "restricted_area", "type": "geopolygon"}
    ])"_json;

    auto fields = parse_fields(fields_json, "snapshot_coll");
    auto collection_meta = collection_meta_for("snapshot_coll", 0, fields, "points");
    auto* collection = new_snapshot_collection(store, thread_pool, fields, 0);

    ASSERT_TRUE(collection->add(R"({"id":"0","title":"alpha","points":10,"vec":[1.0,0.0,0.0,0.0],"embedding":[0.0,1.0,0.0],"service_area":[0.0,0.0,1.0,0.0,1.0,1.0,0.0,1.0],"restricted_area":[4.0,4.0,5.0,4.0,5.0,5.0,4.0,5.0]})").ok());
    ASSERT_TRUE(collection->add(R"({"id":"1","title":"beta","points":20,"vec":[0.0,1.0,0.0,0.0],"embedding":[1.0,0.0,0.0],"service_area":[10.0,10.0,11.0,10.0,11.0,11.0,10.0,11.0],"restricted_area":[2.0,2.0,3.0,2.0,3.0,3.0,2.0,3.0]})").ok());
    ASSERT_TRUE(collection->add(R"({"id":"2","title":"gamma","points":30,"vec":[0.0,0.0,1.0,0.0],"embedding":[0.0,0.0,1.0],"service_area":[20.0,20.0,21.0,20.0,21.0,21.0,20.0,21.0],"restricted_area":[6.0,6.0,7.0,6.0,7.0,7.0,6.0,7.0]})").ok());

    auto vec_before = search_with_vector_query(collection, "vec:([1.0,0.0,0.0,0.0], k: 1)");
    auto embedding_before = search_with_vector_query(collection, "embedding:([0.0,0.0,1.0], k: 1)");
    auto service_area_before = collection->search("*", {}, "service_area:(0.5, 0.5)", {}, {}, {0}, 10, 1, FREQUENCY).get();
    auto restricted_area_before = collection->search("*", {}, "restricted_area:(2.5, 2.5)", {}, {}, {0}, 10, 1, FREQUENCY).get();

    ASSERT_EQ((std::vector<std::string>{"0"}), hit_ids(vec_before));
    ASSERT_EQ((std::vector<std::string>{"2"}), hit_ids(embedding_before));
    ASSERT_EQ((std::vector<std::string>{"0"}), hit_ids(service_area_before));
    ASSERT_EQ((std::vector<std::string>{"1"}), hit_ids(restricted_area_before));
    ASSERT_EQ(3, collection->_get_index()->_get_vector_index().at("vec")->vecdex->getCurrentElementCount());
    ASSERT_EQ(3, collection->_get_index()->_get_vector_index().at("embedding")->vecdex->getCurrentElementCount());
    ASSERT_EQ(3, collection->_get_index()->get_geopolygon_index("service_area")->size());
    ASSERT_EQ(3, collection->_get_index()->get_geopolygon_index("restricted_area")->size());

    const auto snapshot_version = index_snapshot_version(store);
    auto save_op = collection->save_index_snapshot(snapshot_path, collection_meta, snapshot_version);
    ASSERT_TRUE(save_op.ok()) << save_op.error();

    auto expected_manifest = collection->build_index_snapshot_manifest(collection_meta, snapshot_version);

    std::string next_seq_id_str;
    ASSERT_EQ(StoreStatus::FOUND, store.get(Collection::get_next_seq_id_key("snapshot_coll"), next_seq_id_str));
    const auto next_seq_id = StringUtils::deserialize_uint32_t(next_seq_id_str);

    delete collection;

    auto* restored = new_snapshot_collection(store, thread_pool, fields, next_seq_id);
    auto load_op = restored->load_index_snapshot(snapshot_path, expected_manifest);
    ASSERT_TRUE(load_op.ok()) << load_op.error();
    ASSERT_EQ(3, restored->get_num_documents());
    ASSERT_EQ(3, restored->_get_index()->num_seq_ids());
    ASSERT_EQ(3, restored->_get_index()->_get_vector_index().at("vec")->vecdex->getCurrentElementCount());
    ASSERT_EQ(3, restored->_get_index()->_get_vector_index().at("embedding")->vecdex->getCurrentElementCount());
    ASSERT_EQ(3, restored->_get_index()->get_geopolygon_index("service_area")->size());
    ASSERT_EQ(3, restored->_get_index()->get_geopolygon_index("restricted_area")->size());

    auto vec_after = search_with_vector_query(restored, "vec:([1.0,0.0,0.0,0.0], k: 1)");
    auto embedding_after = search_with_vector_query(restored, "embedding:([0.0,0.0,1.0], k: 1)");
    auto service_area_after = restored->search("*", {}, "service_area:(0.5, 0.5)", {}, {}, {0}, 10, 1, FREQUENCY).get();
    auto restricted_area_after = restored->search("*", {}, "restricted_area:(2.5, 2.5)", {}, {}, {0}, 10, 1, FREQUENCY).get();

    ASSERT_EQ(hit_ids(vec_before), hit_ids(vec_after));
    ASSERT_EQ(hit_ids(embedding_before), hit_ids(embedding_after));
    ASSERT_EQ(hit_ids(service_area_before), hit_ids(service_area_after));
    ASSERT_EQ(hit_ids(restricted_area_before), hit_ids(restricted_area_after));

    delete restored;
    CollectionManager::get_instance().dispose();
    thread_pool.shutdown();
}

TEST(IndexSnapshotTest, SnapshotRoundTripsVectorUpdateDeleteAndReinsert) {
    const std::string db_path = "/tmp/typesense_test/index_snapshot_vector_lifecycle_db";
    const std::string snapshot_path = "/tmp/typesense_test/index_snapshot_vector_lifecycle.idxsnap";
    system(("rm -rf " + db_path + " " + snapshot_path + " " + snapshot_path + ".tmp && mkdir -p " + db_path).c_str());

    Store store(db_path);
    ThreadPool thread_pool(4);
    std::atomic<bool> quit = false;
    CollectionManager::get_instance().init(&store, &thread_pool, 1.0f, "auth_key", quit);

    auto fields_json = R"([
        {"name": "title", "type": "string"},
        {"name": "points", "type": "int32"},
        {"name": "vec", "type": "float[]", "num_dim": 2}
    ])"_json;

    auto fields = parse_fields(fields_json, "snapshot_coll");
    auto collection_meta = collection_meta_for("snapshot_coll", 0, fields, "points");
    auto* collection = new_snapshot_collection(store, thread_pool, fields, 0);

    ASSERT_TRUE(collection->add(R"({"id":"0","title":"alpha","points":10,"vec":[0.1,0.1]})").ok());
    ASSERT_TRUE(collection->add(R"({"id":"1","title":"beta","points":20,"vec":[0.9,0.9]})").ok());
    ASSERT_TRUE(collection->add(R"({"id":"0","title":"alpha updated","points":11,"vec":[0.85,0.85]})", UPDATE).ok());
    ASSERT_TRUE(collection->remove("1").ok());
    ASSERT_TRUE(collection->add(R"({"id":"1","title":"beta reinserted","points":21,"vec":[0.2,0.95]})").ok());

    auto* vector_index = collection->_get_index()->_get_vector_index().at("vec")->vecdex;
    const auto max_elements_before = vector_index->getMaxElements();
    const auto current_count_before = vector_index->getCurrentElementCount();
    const auto deleted_count_before = vector_index->getDeletedCount();

    auto updated_before = search_with_vector_query(collection, "vec:([0.85,0.85], k: 1)");
    auto reinserted_before = search_with_vector_query(collection, "vec:([0.2,0.95], k: 1)");
    ASSERT_EQ((std::vector<std::string>{"0"}), hit_ids(updated_before));
    ASSERT_EQ((std::vector<std::string>{"1"}), hit_ids(reinserted_before));

    const auto snapshot_version = index_snapshot_version(store);
    auto save_op = collection->save_index_snapshot(snapshot_path, collection_meta, snapshot_version);
    ASSERT_TRUE(save_op.ok()) << save_op.error();

    auto expected_manifest = collection->build_index_snapshot_manifest(collection_meta, snapshot_version);

    std::string next_seq_id_str;
    ASSERT_EQ(StoreStatus::FOUND, store.get(Collection::get_next_seq_id_key("snapshot_coll"), next_seq_id_str));
    const auto next_seq_id = StringUtils::deserialize_uint32_t(next_seq_id_str);

    delete collection;

    auto* restored = new_snapshot_collection(store, thread_pool, fields, next_seq_id);
    auto load_op = restored->load_index_snapshot(snapshot_path, expected_manifest);
    ASSERT_TRUE(load_op.ok()) << load_op.error();
    ASSERT_EQ(2, restored->get_num_documents());
    ASSERT_EQ(2, restored->_get_index()->num_seq_ids());

    auto* restored_vector_index = restored->_get_index()->_get_vector_index().at("vec")->vecdex;
    ASSERT_EQ(max_elements_before, restored_vector_index->getMaxElements());
    ASSERT_EQ(current_count_before, restored_vector_index->getCurrentElementCount());
    ASSERT_EQ(deleted_count_before, restored_vector_index->getDeletedCount());

    auto updated_after = search_with_vector_query(restored, "vec:([0.85,0.85], k: 1)");
    auto reinserted_after = search_with_vector_query(restored, "vec:([0.2,0.95], k: 1)");
    ASSERT_EQ(hit_ids(updated_before), hit_ids(updated_after));
    ASSERT_EQ(hit_ids(reinserted_before), hit_ids(reinserted_after));

    delete restored;
    CollectionManager::get_instance().dispose();
    thread_pool.shutdown();
}

TEST(IndexSnapshotTest, SnapshotRoundTripsGeoPolygonIndex) {
    const std::string db_path = "/tmp/typesense_test/index_snapshot_geopolygon_db";
    const std::string snapshot_path = "/tmp/typesense_test/index_snapshot_geopolygon.idxsnap";
    system(("rm -rf " + db_path + " " + snapshot_path + " " + snapshot_path + ".tmp && mkdir -p " + db_path).c_str());

    Store store(db_path);
    ThreadPool thread_pool(4);
    std::atomic<bool> quit = false;
    CollectionManager::get_instance().init(&store, &thread_pool, 1.0f, "auth_key", quit);

    auto fields_json = R"([
        {"name": "name", "type": "string"},
        {"name": "area", "type": "geopolygon"},
        {"name": "points", "type": "int32"}
    ])"_json;

    auto fields = parse_fields(fields_json, "snapshot_coll");
    auto collection_meta = collection_meta_for("snapshot_coll", 0, fields, "points");
    auto* collection = new_snapshot_collection(store, thread_pool, fields, 0);

    ASSERT_TRUE(collection->add(R"({"id":"0","name":"square","area":[0.0,0.0,1.0,0.0,1.0,1.0,0.0,1.0],"points":10})").ok());
    ASSERT_TRUE(collection->add(R"({"id":"1","name":"rectangle","area":[2.0,2.0,5.0,2.0,5.0,4.0,2.0,4.0],"points":20})").ok());
    ASSERT_TRUE(collection->add(R"({"id":"2","name":"larger square","area":[0.0,0.0,2.0,0.0,2.0,2.0,0.0,2.0],"points":30})").ok());
    ASSERT_TRUE(collection->remove("0").ok());

    auto square_before = collection->search("*", {}, "area:(0.5, 0.5)", {}, {}, {0}, 10, 1, FREQUENCY).get();
    auto rectangle_before = collection->search("*", {}, "area:(2.5, 3.5)", {}, {}, {0}, 10, 1, FREQUENCY).get();
    ASSERT_EQ((std::vector<std::string>{"2"}), hit_ids(square_before));
    ASSERT_EQ((std::vector<std::string>{"1"}), hit_ids(rectangle_before));
    ASSERT_EQ(2, collection->_get_index()->get_geopolygon_index("area")->size());

    const auto snapshot_version = index_snapshot_version(store);
    auto save_op = collection->save_index_snapshot(snapshot_path, collection_meta, snapshot_version);
    ASSERT_TRUE(save_op.ok()) << save_op.error();

    auto expected_manifest = collection->build_index_snapshot_manifest(collection_meta, snapshot_version);

    std::string next_seq_id_str;
    ASSERT_EQ(StoreStatus::FOUND, store.get(Collection::get_next_seq_id_key("snapshot_coll"), next_seq_id_str));
    const auto next_seq_id = StringUtils::deserialize_uint32_t(next_seq_id_str);

    delete collection;

    auto* restored = new_snapshot_collection(store, thread_pool, fields, next_seq_id);
    auto load_op = restored->load_index_snapshot(snapshot_path, expected_manifest);
    ASSERT_TRUE(load_op.ok()) << load_op.error();
    ASSERT_EQ(2, restored->get_num_documents());
    ASSERT_EQ(2, restored->_get_index()->num_seq_ids());
    ASSERT_EQ(2, restored->_get_index()->get_geopolygon_index("area")->size());

    auto square_after = restored->search("*", {}, "area:(0.5, 0.5)", {}, {}, {0}, 10, 1, FREQUENCY).get();
    auto rectangle_after = restored->search("*", {}, "area:(2.5, 3.5)", {}, {}, {0}, 10, 1, FREQUENCY).get();
    ASSERT_EQ(hit_ids(square_before), hit_ids(square_after));
    ASSERT_EQ(hit_ids(rectangle_before), hit_ids(rectangle_after));

    delete restored;
    CollectionManager::get_instance().dispose();
    thread_pool.shutdown();
}

TEST(IndexSnapshotTest, CollectionManagerLoadsSnapshotBeforeDocumentReplay) {
    const std::string source_db_path = "/tmp/typesense_test/index_snapshot_source_db";
    const std::string target_db_path = "/tmp/typesense_test/index_snapshot_target_db";
    const std::string snapshot_dir = "/tmp/typesense_test/index_snapshot_startup";
    system(("rm -rf " + source_db_path + " " + target_db_path + " " + snapshot_dir +
            " && mkdir -p " + source_db_path + " " + target_db_path + " " + snapshot_dir).c_str());

    auto fields_json = R"([
        {"name": "title", "type": "string"},
        {"name": "points", "type": "int32"}
    ])"_json;
    auto fields = parse_fields(fields_json, "snapshot_coll");
    auto collection_meta = collection_meta_for("snapshot_coll", 0, fields, "points");

    Store source_store(source_db_path);
    ThreadPool source_thread_pool(4);
    std::atomic<bool> source_quit = false;
    CollectionManager::get_instance().init(&source_store, &source_thread_pool, 1.0f, "auth_key", source_quit);

    auto* source_collection = new_snapshot_collection(source_store, source_thread_pool, fields, 0);
    ASSERT_TRUE(source_collection->add(R"({"id":"1","title":"fast red jeep","points":30})").ok());

    std::string next_seq_id_str;
    ASSERT_EQ(StoreStatus::FOUND, source_store.get(Collection::get_next_seq_id_key("snapshot_coll"), next_seq_id_str));
    const auto snapshot_version = index_snapshot_version(source_store);

    Store target_store(target_db_path);
    ASSERT_TRUE(target_store.insert(Collection::get_next_seq_id_key("snapshot_coll"), next_seq_id_str));
    ASSERT_TRUE(target_store.insert(Collection::get_index_snapshot_version_key("snapshot_coll"),
                                    StringUtils::serialize_uint32_t(snapshot_version)));
    ASSERT_TRUE(target_store.insert(Collection::get_meta_key("snapshot_coll"), collection_meta.dump()));

    CollectionManager::get_instance().dispose();
    source_thread_pool.shutdown();

    Config::get_instance().set_enable_index_snapshot(true);
    Config::get_instance().set_index_snapshot_dir(snapshot_dir);

    ThreadPool target_thread_pool(4);
    std::atomic<bool> target_quit = false;
    CollectionManager::get_instance().init(&target_store, &target_thread_pool, 1.0f, "auth_key", target_quit);

    const auto snapshot_path = source_collection->get_index_snapshot_path(snapshot_dir);
    auto save_op = source_collection->save_index_snapshot(snapshot_path, collection_meta,
                                                         snapshot_version);
    EXPECT_TRUE(save_op.ok()) << save_op.error();
    delete source_collection;

    auto load_op = CollectionManager::load_collection(collection_meta, 100, StoreStatus::NOT_FOUND, target_quit, {});
    EXPECT_TRUE(load_op.ok()) << load_op.error();

    auto restored = CollectionManager::get_instance().get_collection("snapshot_coll");
    EXPECT_NE(nullptr, restored);
    if(restored != nullptr) {
        EXPECT_EQ(1, restored->get_num_documents());
        EXPECT_EQ(1, restored->_get_index()->num_seq_ids());
    }
    restored.reset();

    Config::get_instance().set_enable_index_snapshot(false);
    Config::get_instance().set_index_snapshot_dir("");
    CollectionManager::get_instance().dispose();
    target_thread_pool.shutdown();
}

TEST(IndexSnapshotTest, CollectionManagerLoadsVectorAndGeoPolygonSnapshotBeforeDocumentReplay) {
    const std::string source_db_path = "/tmp/typesense_test/index_snapshot_vector_geo_source_db";
    const std::string target_db_path = "/tmp/typesense_test/index_snapshot_vector_geo_target_db";
    const std::string snapshot_dir = "/tmp/typesense_test/index_snapshot_vector_geo_startup";
    system(("rm -rf " + source_db_path + " " + target_db_path + " " + snapshot_dir +
            " && mkdir -p " + source_db_path + " " + target_db_path + " " + snapshot_dir).c_str());

    auto fields_json = R"([
        {"name": "title", "type": "string"},
        {"name": "points", "type": "int32"},
        {"name": "vec", "type": "float[]", "num_dim": 4},
        {"name": "area", "type": "geopolygon"}
    ])"_json;
    auto fields = parse_fields(fields_json, "snapshot_coll");
    auto collection_meta = collection_meta_for("snapshot_coll", 0, fields, "points");

    Store source_store(source_db_path);
    ThreadPool source_thread_pool(4);
    std::atomic<bool> source_quit = false;
    CollectionManager::get_instance().init(&source_store, &source_thread_pool, 1.0f, "auth_key", source_quit);

    auto* source_collection = new_snapshot_collection(source_store, source_thread_pool, fields, 0);
    ASSERT_TRUE(source_collection->add(R"({"id":"0","title":"alpha","points":10,"vec":[0.851758,0.909671,0.823431,0.372063],"area":[0.0,0.0,1.0,0.0,1.0,1.0,0.0,1.0]})").ok());
    ASSERT_TRUE(source_collection->add(R"({"id":"1","title":"beta","points":20,"vec":[0.97826,0.933157,0.39557,0.306488],"area":[2.0,2.0,5.0,2.0,5.0,4.0,2.0,4.0]})").ok());

    std::string next_seq_id_str;
    ASSERT_EQ(StoreStatus::FOUND, source_store.get(Collection::get_next_seq_id_key("snapshot_coll"), next_seq_id_str));
    const auto snapshot_version = index_snapshot_version(source_store);

    Store target_store(target_db_path);
    ASSERT_TRUE(target_store.insert(Collection::get_next_seq_id_key("snapshot_coll"), next_seq_id_str));
    ASSERT_TRUE(target_store.insert(Collection::get_index_snapshot_version_key("snapshot_coll"),
                                    StringUtils::serialize_uint32_t(snapshot_version)));
    ASSERT_TRUE(target_store.insert(Collection::get_meta_key("snapshot_coll"), collection_meta.dump()));

    CollectionManager::get_instance().dispose();
    source_thread_pool.shutdown();

    Config::get_instance().set_enable_index_snapshot(true);
    Config::get_instance().set_index_snapshot_dir(snapshot_dir);

    ThreadPool target_thread_pool(4);
    std::atomic<bool> target_quit = false;
    CollectionManager::get_instance().init(&target_store, &target_thread_pool, 1.0f, "auth_key", target_quit);

    const auto snapshot_path = source_collection->get_index_snapshot_path(snapshot_dir);
    auto save_op = source_collection->save_index_snapshot(snapshot_path, collection_meta,
                                                         snapshot_version);
    EXPECT_TRUE(save_op.ok()) << save_op.error();
    delete source_collection;

    auto load_op = CollectionManager::load_collection(collection_meta, 100, StoreStatus::NOT_FOUND, target_quit, {});
    EXPECT_TRUE(load_op.ok()) << load_op.error();

    auto restored = CollectionManager::get_instance().get_collection("snapshot_coll");
    EXPECT_NE(nullptr, restored);
    if(restored != nullptr) {
        EXPECT_EQ(2, restored->get_num_documents());
        EXPECT_EQ(2, restored->_get_index()->num_seq_ids());
        auto* restored_vector_index = restored->_get_index()->_get_vector_index().at("vec")->vecdex;
        EXPECT_EQ(2, restored_vector_index->getCurrentElementCount());
        EXPECT_EQ(0, restored_vector_index->getDeletedCount());
        EXPECT_EQ(2, restored->_get_index()->get_geopolygon_index("area")->size());
    }
    restored.reset();

    Config::get_instance().set_enable_index_snapshot(false);
    Config::get_instance().set_index_snapshot_dir("");
    CollectionManager::get_instance().dispose();
    target_thread_pool.shutdown();
}

TEST(IndexSnapshotTest, CollectionManagerFallsBackToReplayWhenSnapshotBodyIsCorrupt) {
    const std::string db_path = "/tmp/typesense_test/index_snapshot_corrupt_db";
    const std::string snapshot_dir = "/tmp/typesense_test/index_snapshot_corrupt";
    system(("rm -rf " + db_path + " " + snapshot_dir + " && mkdir -p " + db_path + " " + snapshot_dir).c_str());

    auto fields_json = R"([
        {"name": "title", "type": "string"},
        {"name": "points", "type": "int32"}
    ])"_json;
    auto fields = parse_fields(fields_json, "snapshot_coll");
    auto collection_meta = collection_meta_for("snapshot_coll", 0, fields, "points");

    {
        Store store(db_path);
        ThreadPool thread_pool(4);
        std::atomic<bool> quit = false;
        CollectionManager::get_instance().init(&store, &thread_pool, 1.0f, "auth_key", quit);

        auto* collection = new_snapshot_collection(store, thread_pool, fields, 0);
        ASSERT_TRUE(collection->add(R"({"id":"1","title":"fast red jeep","points":30})").ok());
        ASSERT_TRUE(store.insert(Collection::get_meta_key("snapshot_coll"), collection_meta.dump()));

        const auto snapshot_path = collection->get_index_snapshot_path(snapshot_dir);
        auto save_op = collection->save_index_snapshot(snapshot_path, collection_meta,
                                                       index_snapshot_version(store));
        ASSERT_TRUE(save_op.ok()) << save_op.error();

        struct stat snapshot_stat {};
        ASSERT_EQ(0, stat(snapshot_path.c_str(), &snapshot_stat));
        ASSERT_GT(snapshot_stat.st_size, 8);
        ASSERT_EQ(0, truncate(snapshot_path.c_str(), snapshot_stat.st_size - 8));

        delete collection;
        CollectionManager::get_instance().dispose();
        thread_pool.shutdown();
    }

    Config::get_instance().set_enable_index_snapshot(true);
    Config::get_instance().set_index_snapshot_dir(snapshot_dir);

    Store load_store(db_path);
    ThreadPool load_thread_pool(4);
    std::atomic<bool> load_quit = false;
    CollectionManager::get_instance().init(&load_store, &load_thread_pool, 1.0f, "auth_key", load_quit);

    auto load_op = CollectionManager::load_collection(collection_meta, 100, StoreStatus::NOT_FOUND, load_quit, {});
    EXPECT_TRUE(load_op.ok()) << load_op.error();

    auto restored = CollectionManager::get_instance().get_collection("snapshot_coll");
    EXPECT_NE(nullptr, restored);
    if(restored != nullptr) {
        EXPECT_EQ(1, restored->get_num_documents());
        EXPECT_EQ(1, restored->_get_index()->num_seq_ids());
    }
    restored.reset();

    Config::get_instance().set_enable_index_snapshot(false);
    Config::get_instance().set_index_snapshot_dir("");
    CollectionManager::get_instance().dispose();
    load_thread_pool.shutdown();
}
