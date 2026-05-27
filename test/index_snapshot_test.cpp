#include <gtest/gtest.h>

#include <sys/stat.h>
#include <unistd.h>

#include <collection.h>
#include <collection_manager.h>
#include <field.h>
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

    const auto store_seq_number = store.get_latest_seq_number();
    auto save_op = collection->save_index_snapshot(snapshot_path, collection_meta, store_seq_number);
    ASSERT_TRUE(save_op.ok()) << save_op.error();

    auto expected_manifest = collection->build_index_snapshot_manifest(collection_meta, store_seq_number);

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

    const auto store_seq_number = store.get_latest_seq_number();
    auto save_op = collection->save_index_snapshot(snapshot_path, collection_meta, store_seq_number);
    ASSERT_TRUE(save_op.ok()) << save_op.error();

    auto expected_manifest = collection->build_index_snapshot_manifest(collection_meta, store_seq_number);
    auto stale_manifest = expected_manifest;
    stale_manifest["store_seq_number"] = store_seq_number + 1;

    std::string next_seq_id_str;
    ASSERT_EQ(StoreStatus::FOUND, store.get(Collection::get_next_seq_id_key("snapshot_coll"), next_seq_id_str));
    const auto next_seq_id = StringUtils::deserialize_uint32_t(next_seq_id_str);

    delete collection;

    auto* restored = new_snapshot_collection(store, thread_pool, fields, next_seq_id);
    auto stale_load_op = restored->load_index_snapshot(snapshot_path, stale_manifest);
    ASSERT_FALSE(stale_load_op.ok());
    ASSERT_EQ(409, stale_load_op.code());
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

    Store target_store(target_db_path);
    ASSERT_TRUE(target_store.insert(Collection::get_next_seq_id_key("snapshot_coll"), next_seq_id_str));
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
                                                         target_store.get_latest_seq_number());
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
                                                       store.get_latest_seq_number());
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
