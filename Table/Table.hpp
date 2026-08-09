#ifndef TABLE_HPP
#define TABLE_HPP

#include "Pager.hpp"
#include "Schema.hpp"
#include <memory>

class Table;

struct Cursor {
    Table* table{nullptr};
    uint32_t page_num{0};
    uint32_t cell_num{0};
    bool end_of_table{false};
};

class Table {
public:
    std::unique_ptr<Pager> pager;
    uint32_t root_page_num{0};
    Schema schema;

    // Transaction state
    bool in_transaction{false};
    uint32_t tx_original_num_pages{0};
    void* tx_backup[TABLE_MAX_PAGES]{nullptr};
    bool tx_was_cached[TABLE_MAX_PAGES]{false};

    explicit Table(const std::string& filename);
    ~Table();

    void tx_free_backups();
    void initialize_leaf_node(void* node);
    void initialize_internal_node(void* node);
    uint32_t get_node_max_key(void* node);

    Cursor* leaf_node_find(uint32_t page_num, uint32_t key);
    uint32_t internal_node_find_child(void* node, uint32_t key);
    Cursor* internal_node_find(uint32_t page_num, uint32_t key);
    Cursor* table_find(uint32_t key);
    Cursor* table_start();

    void* cursor_value(Cursor* cursor);
    void cursor_advance(Cursor* cursor);
    void leaf_node_delete(Cursor* cursor);

    uint32_t get_unused_page_num();
    void create_new_root(uint32_t right_child_page_num);
    void update_internal_node_key(void* node, uint32_t old_key, uint32_t new_key);
    void internal_node_split_and_insert(uint32_t parent_page_num, uint32_t child_page_num);
    void internal_node_insert(uint32_t parent_page_num, uint32_t child_page_num);
    void leaf_node_split_and_insert(Cursor* cursor, uint32_t key, DynamicRow* value);
    void leaf_node_insert(Cursor* cursor, uint32_t key, DynamicRow* value);

    void print_tree(uint32_t page_num, uint32_t indentation_level);
};

void serialize_row(const DynamicRow* source, void* destination, const Schema& schema);
void deserialize_row(const void* source, DynamicRow* destination, const Schema& schema);
void print_row(const DynamicRow* row, const Schema& schema);

#endif // TABLE_HPP
