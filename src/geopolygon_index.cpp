#include "geopolygon_index.h"

#include <s2/util/coding/coder.h>

#include <istream>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <string>

namespace {

template <typename T>
void write_geopolygon_pod(std::ostream& out, const T& value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
    if(!out.good()) {
        throw std::runtime_error("Unable to write geopolygon index snapshot.");
    }
}

template <typename T>
T read_geopolygon_pod(std::istream& in) {
    T value{};
    in.read(reinterpret_cast<char*>(&value), sizeof(T));
    if(!in.good()) {
        throw std::runtime_error("Unable to read geopolygon index snapshot.");
    }
    return value;
}

void write_geopolygon_bytes(std::ostream& out, const char* data, size_t size) {
    write_geopolygon_pod(out, static_cast<uint64_t>(size));
    if(size != 0) {
        out.write(data, size);
    }
    if(!out.good()) {
        throw std::runtime_error("Unable to write geopolygon payload to index snapshot.");
    }
}

std::string read_geopolygon_bytes(std::istream& in) {
    const auto size = read_geopolygon_pod<uint64_t>(in);
    std::string bytes(size, '\0');
    if(size != 0) {
        in.read(&bytes[0], size);
    }
    if(!in.good()) {
        throw std::runtime_error("Unable to read geopolygon payload from index snapshot.");
    }
    return bytes;
}

}  // namespace

Option<bool> GeoPolygonIndex::addPolygon(const std::vector<double>& coordinates, uint32_t seq_id) {
    // Convert each ring of coordinates to an S2Loop
    std::vector <S2Point> points;

    const int coordinates_size = coordinates.size();
    for (size_t point_index = 0; point_index < coordinates_size; point_index += 2) {
        double lat = coordinates[point_index];
        double lon = coordinates[point_index + 1];

        S2LatLng latLng(S1Angle::Degrees(coordinates[point_index]),
                        S1Angle::Degrees(coordinates[point_index + 1]));
        points.push_back(latLng.ToPoint());
    }

    auto loop = std::make_unique<S2Loop>(points);
    loop->Normalize();

    //passing vector of loops to check for empty loops
    std::vector<std::unique_ptr<S2Loop>> loops;
    loops.emplace_back(std::move(loop));

    // Create polygon from loops
    std::unique_ptr<S2Polygon> polygon = std::make_unique<S2Polygon>(std::move(loops));

    std::vector <uint64_t> cell_ids;

    S2Error error;
    if (polygon->FindValidationError(&error)) {
        return Option<bool>(400, "Geopolygon for seq_id " +
                                 std::to_string(seq_id) +
                                 " is invalid: " + error.text());
    }

    for (const auto& term: indexer->GetIndexTerms(*polygon, "")) {
        auto cell = S2CellId::FromToken(term);
        cell_ids.push_back(cell.id());
    }

    for (const auto& cell_id: cell_ids) {
        numericTrie->insert_geopoint(cell_id, seq_id);
    }
    seqidToPolygons[seq_id].emplace_back(std::move(polygon));

    return Option<bool>(true);
}


std::vector<uint32_t> GeoPolygonIndex::findContainingPolygonsRecords(double lat, double lng) {
    S2LatLng latLng(S1Angle::Degrees(lat), S1Angle::Degrees(lng));
    S2Point point = latLng.ToPoint();

    std::vector <uint32_t> candidate_seq_ids, result_seq_ids;

    std::vector <uint64_t> cell_ids;
    for (const auto& term: indexer->GetQueryTerms(point, "")) {
        auto cell = S2CellId::FromToken(term);
        cell_ids.push_back(cell.id());
    }

    numericTrie->search_geopoints(cell_ids, candidate_seq_ids);

    //second pass validation check
    for (const auto& id: candidate_seq_ids) {
        if (seqidToPolygons.find(id) != seqidToPolygons.end()) {
            for (const auto& polygon: seqidToPolygons.at(id)) {
                if (polygon->Contains(point)) {
                    result_seq_ids.push_back(id);
                }
            }
        }
    }

    return result_seq_ids;
}

void GeoPolygonIndex::removePolygon(uint32_t seq_id) {
    if (seqidToPolygons.find(seq_id) != seqidToPolygons.end()) {
        std::vector <uint32_t> cell_ids;

        for (const auto& polygon: seqidToPolygons.at(seq_id)) {
            for (const auto& term: indexer->GetIndexTerms(*polygon, "")) {
                auto cell = S2CellId::FromToken(term);
                cell_ids.push_back(cell.id());
            }

            for (const auto& cell_id: cell_ids) {
                numericTrie->delete_geopoint(cell_id, seq_id);
            }
        }

        seqidToPolygons.erase(seq_id);
    }
}

void GeoPolygonIndex::snapshot_write(std::ostream& out) const {
    write_geopolygon_pod(out, static_cast<uint64_t>(seqidToPolygons.size()));

    for(const auto& seq_polygons: seqidToPolygons) {
        write_geopolygon_pod(out, seq_polygons.first);
        write_geopolygon_pod(out, static_cast<uint64_t>(seq_polygons.second.size()));

        for(const auto& polygon: seq_polygons.second) {
            Encoder encoder;
            polygon->Encode(&encoder);
            write_geopolygon_bytes(out, encoder.base(), encoder.length());
        }
    }
}

void GeoPolygonIndex::snapshot_read(std::istream& in) {
    delete numericTrie;
    numericTrie = new NumericTrie(32);
    seqidToPolygons.clear();

    const auto seq_count = read_geopolygon_pod<uint64_t>(in);
    for(uint64_t i = 0; i < seq_count; i++) {
        const auto seq_id = read_geopolygon_pod<uint32_t>(in);
        const auto polygon_count = read_geopolygon_pod<uint64_t>(in);

        for(uint64_t j = 0; j < polygon_count; j++) {
            auto encoded = read_geopolygon_bytes(in);
            Decoder decoder(encoded.data(), encoded.size());
            auto polygon = std::make_unique<S2Polygon>();
            if(!polygon->Decode(&decoder) || decoder.avail() != 0) {
                throw std::runtime_error("Unable to decode geopolygon index snapshot.");
            }

            for(const auto& term: indexer->GetIndexTerms(*polygon, "")) {
                auto cell = S2CellId::FromToken(term);
                numericTrie->insert_geopoint(cell.id(), seq_id);
            }
            seqidToPolygons[seq_id].emplace_back(std::move(polygon));
        }
    }
}
