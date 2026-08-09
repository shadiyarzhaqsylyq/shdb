#include "Schema.hpp"

void Schema::add_column(const std::string& name, DataType type, uint32_t length, bool is_pk) {
    ColumnDef col;
    col.name = name;
    col.type = type;
    col.is_primary_key = is_pk;
    col.offset = row_size;

    if (type == DataType::INT) {
        col.length = 0;
        col.size = sizeof(int32_t);
    } else { // VARCHAR
        col.length = (length > 0) ? length : 32;
        col.size = col.length + 1; // Null-terminated string buffer
    }

    if (is_pk) {
        primary_key_index = static_cast<uint32_t>(columns.size());
    }

    row_size += col.size;
    columns.push_back(col);
    has_schema = true;
}

int Schema::find_column(const std::string& name) const {
    for (size_t i = 0; i < columns.size(); ++i) {
        std::string c_name = columns[i].name;
        std::string q_name = name;
        std::transform(c_name.begin(), c_name.end(), c_name.begin(), ::tolower);
        std::transform(q_name.begin(), q_name.end(), q_name.begin(), ::tolower);
        if (c_name == q_name) return static_cast<int>(i);
    }
    return -1;
}

Schema create_default_schema() {
    Schema s;
    s.table_name = "employees";
    s.add_column("id", DataType::INT, 0, true);
    s.add_column("name", DataType::VARCHAR, 32, false);
    s.add_column("salary", DataType::INT, 0, false);
    s.add_column("department", DataType::VARCHAR, 32, false);
    s.add_column("city", DataType::VARCHAR, 32, false);
    return s;
}

uint32_t DynamicRow::get_pk_value(const Schema& schema) const {
    if (schema.primary_key_index < values.size()) {
        return static_cast<uint32_t>(values[schema.primary_key_index].int_val);
    }
    return 0;
}
