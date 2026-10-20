#include "framekeyboard/placement_store.hpp"
#include <cerrno>
#include <cmath>
#include <json-c/json.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace framekeyboard {
namespace {
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
void require(bool valid) {
    if (!valid) {
        throw std::runtime_error("Invalid saved keyboard placement");
    }
}
json_object* field(json_object* object, const char* name, json_type type) {
    json_object* value{};
    require(json_object_object_get_ex(object, name, &value) && json_object_is_type(value, type));
    return value;
}
double number(json_object* value) {
    require(value &&
            (json_object_is_type(value, json_type_double) || json_object_is_type(value, json_type_int)));
    const double result = json_object_get_double(value);
    require(std::isfinite(result));
    return result;
}
void validate_transform(const Transform& m) {
    for (const auto& row : m) {
        for (double value : row) {
            require(std::isfinite(value));
        }
    }
    for (std::size_t a = 0; a < 3; ++a) {
        require(std::abs(m[a][3]) < 1000);
        for (std::size_t b = 0; b < 3; ++b) {
            double dot = 0;
            for (std::size_t r = 0; r < 3; ++r) {
                dot += m[r][a] * m[r][b];
            }
            require(std::abs(dot - (a == b ? 1 : 0)) < .001);
        }
    }
    const double determinant = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                               m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                               m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    require(std::abs(determinant - 1) < .001);
}
void validate(const SavedPlacement& saved) {
    require(std::isfinite(saved.width) && saved.width >= .45 && saved.width <= 2);
    validate_transform(saved.transform);
    if (saved.dashboard) {
        validate_transform(*saved.dashboard);
    }
    if (saved.dashboard_bar) {
        require(saved.dashboard.has_value());
        validate_transform(*saved.dashboard_bar);
    }
}
Transform read_transform(json_object* rows) {
    require(json_object_is_type(rows, json_type_array) && json_object_array_length(rows) == 3);
    Transform result{};
    for (std::size_t r = 0; r < 3; ++r) {
        auto* row = json_object_array_get_idx(rows, r);
        require(json_object_is_type(row, json_type_array) && json_object_array_length(row) == 4);
        for (std::size_t c = 0; c < 4; ++c) {
            result[r][c] = number(json_object_array_get_idx(row, c));
        }
    }
    return result;
}
json_object* write_transform(const Transform& transform) {
    auto* rows = json_object_new_array();
    for (const auto& values : transform) {
        auto* row = json_object_new_array();
        for (double value : values) {
            json_object_array_add(row, json_object_new_double(value));
        }
        json_object_array_add(rows, row);
    }
    return rows;
}
} // namespace
std::optional<SavedPlacement> load_placement(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        return {};
    }
    require(std::filesystem::file_size(path) <= 16384);
    Json document(json_object_from_file(path.c_str()), json_object_put);
    require(document != nullptr);
    require(json_object_get_int(field(document.get(), "schema_version", json_type_int)) == 1);
    require(std::string(json_object_get_string(field(document.get(), "space", json_type_string))) ==
            "standing");
    const std::string universe =
        json_object_get_string(field(document.get(), "universe", json_type_string));
    require(!universe.empty() && universe.find_first_not_of("0123456789") == std::string::npos);
    SavedPlacement saved{};
    saved.universe = std::stoull(universe);
    json_object* width{};
    require(json_object_object_get_ex(document.get(), "width_m", &width));
    saved.width = number(width);
    saved.transform = read_transform(field(document.get(), "transform", json_type_array));
    json_object* dashboard{};
    if (json_object_object_get_ex(document.get(), "dashboard_anchor", &dashboard)) {
        saved.dashboard = read_transform(dashboard);
    }
    json_object* bar{};
    if (json_object_object_get_ex(document.get(), "dashboard_bar", &bar)) {
        saved.dashboard_bar = read_transform(bar);
    }
    json_object* full_rotation{};
    if (json_object_object_get_ex(document.get(), "dashboard_full_rotation", &full_rotation)) {
        require(json_object_is_type(full_rotation, json_type_boolean));
        saved.dashboard_full_rotation = json_object_get_boolean(full_rotation);
    }
    validate(saved);
    return saved;
}
void save_placement(const std::filesystem::path& path, const SavedPlacement& saved) {
    validate(saved);
    Json document(json_object_new_object(), json_object_put);
    json_object_object_add(document.get(), "schema_version", json_object_new_int(1));
    json_object_object_add(document.get(), "space", json_object_new_string("standing"));
    json_object_object_add(document.get(), "universe",
                           json_object_new_string(std::to_string(saved.universe).c_str()));
    json_object_object_add(document.get(), "width_m", json_object_new_double(saved.width));
    json_object_object_add(document.get(), "transform", write_transform(saved.transform));
    if (saved.dashboard) {
        json_object_object_add(document.get(), "dashboard_anchor", write_transform(*saved.dashboard));
    }
    if (saved.dashboard_bar) {
        json_object_object_add(document.get(), "dashboard_bar", write_transform(*saved.dashboard_bar));
    }
    json_object_object_add(document.get(), "dashboard_full_rotation",
                           json_object_new_boolean(saved.dashboard_full_rotation));
    const std::string data =
        std::string(json_object_to_json_string_ext(document.get(), JSON_C_TO_STRING_PRETTY)) + '\n';
    std::filesystem::create_directories(path.parent_path());
    // A partial write must never replace the last usable position.
    std::string temporary = (path.parent_path() / ".placement-XXXXXX").string();
    const int fd = mkstemp(temporary.data());
    if (fd < 0) {
        throw std::runtime_error("Cannot create placement temporary file");
    }
    try {
        std::size_t offset = 0;
        while (offset < data.size()) {
            const auto written = write(fd, data.data() + offset, data.size() - offset);
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written <= 0) {
                throw std::runtime_error("Cannot write keyboard placement");
            }
            offset += static_cast<std::size_t>(written);
        }
        if (fsync(fd) != 0) {
            throw std::runtime_error("Cannot flush keyboard placement");
        }
        std::filesystem::rename(temporary, path);
        close(fd);
    } catch (...) {
        close(fd);
        unlink(temporary.c_str());
        throw;
    }
}
} // namespace framekeyboard
