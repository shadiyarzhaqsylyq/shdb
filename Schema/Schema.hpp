#ifndef SCHEMA_HPP
#define SCHEMA_HPP

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

enum class DataType : uint8_t {
    INT = 0,
    VARCHAR = 1
};

struct ColumnDef {
    std::string name;
    DataType type{DataType::INT};
    uint32_t length{0}; // For VARCHAR length; 0 for INT
    uint32_t offset{0}; // Byte offset in row buffer
    uint32_t size{0};   // Size in row buffer
    bool is_primary_key{false};
};

struct Schema {
    bool has_schema{false};
    std::string table_name{"employees"};
    std::vector<ColumnDef> columns;
    uint32_t row_size{0};
    uint32_t primary_key_index{0};

    void add_column(const std::string& name, DataType type, uint32_t length = 0, bool is_pk = false);
    int find_column(const std::string& name) const;
};

Schema create_default_schema();

struct Value {
    DataType type{DataType::INT};
    int32_t int_val{0};
    std::string str_val;
};

struct DynamicRow {
    std::vector<Value> values;
    uint32_t get_pk_value(const Schema& schema) const;
};

#endif // SCHEMA_HPP
