#pragma once

#include <s2/s2polygon.h>
#include <s2/s2region_coverer.h>
#include <s2/s2latlng.h>
#include <s2/s2cell_id.h>
#include <s2/s2loop.h>
#include <s2/s2point.h>
#include <s2/s2region_term_indexer.h>
#include <s2/s2cap.h>
#include <s2/s2builder.h>
#include <iosfwd>
#include <memory>
#include <unordered_map>
#include <vector>
#include "option.h"
#include "numeric_range_trie.h"

class GeoPolygonIndex {
private:
    S2RegionTermIndexer* indexer = nullptr;
    NumericTrie* numericTrie;
    std::unordered_map<uint32_t, std::vector<std::unique_ptr<S2Polygon>>> seqidToPolygons;
public:
    GeoPolygonIndex() {
        numericTrie = new NumericTrie(32);

        //initialize with all default options
        indexer = new S2RegionTermIndexer();
    }

    ~GeoPolygonIndex() {
        delete numericTrie;
        delete indexer;
    }

    // Add a polygon to the index
    Option<bool> addPolygon(const std::vector<double>& coordinates, uint32_t seq_id);

    // Find all polygons that might contain the given point
    std::vector<uint32_t> findContainingPolygonsRecords(double lat, double lng);

    //remove polygon from index
    void removePolygon(uint32_t seq_id);

    void snapshot_write(std::ostream& out) const;

    void snapshot_read(std::istream& in);

    size_t size() const {
        return seqidToPolygons.size();
    }
};
