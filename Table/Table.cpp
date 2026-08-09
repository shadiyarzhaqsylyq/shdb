#include "Table.hpp"
#include <cstring>
#include <iostream>

void serialize_row(const DynamicRow* source, void* destination, const Schema& schema) {
    std::memset(destination, 0, schema.row_size);
    for (size_t i = 0; i < schema.columns.size(); ++i) {
        const auto& col = schema.columns[i];
        uint8_t* dest_ptr = static_cast<uint8_t*>(destination) + col.offset;
        if (i < source->values.size()) {
            const auto& val = source->values[i];
            if (col.type == DataType::INT) {
                int32_t v = val.int_val;
                std::memcpy(dest_ptr, &v, sizeof(int32_t));
            } else {
                std::strncpy(reinterpret_cast<char*>(dest_ptr), val.str_val.c_str(), col.length);
                dest_ptr[col.length] = '\0';
            }
        }
    }
}

void deserialize_row(const void* source, DynamicRow* destination, const Schema& schema) {
    destination->values.clear();
    destination->values.resize(schema.columns.size());
    for (size_t i = 0; i < schema.columns.size(); ++i) {
        const auto& col = schema.columns[i];
        const uint8_t* src_ptr = static_cast<const uint8_t*>(source) + col.offset;
        if (col.type == DataType::INT) {
            int32_t v = 0;
            std::memcpy(&v, src_ptr, sizeof(int32_t));
            destination->values[i].type = DataType::INT;
            destination->values[i].int_val = v;
        } else {
            std::vector<char> buf(col.size, 0);
            std::memcpy(buf.data(), src_ptr, col.size);
            buf[col.size - 1] = '\0';
            destination->values[i].type = DataType::VARCHAR;
            destination->values[i].str_val = std::string(buf.data());
        }
    }
}

void print_row(const DynamicRow* row, const Schema& schema) {
    std::cout << "(";
    for (size_t i = 0; i < schema.columns.size(); ++i) {
        if (i > 0) std::cout << ", ";
        if (schema.columns[i].type == DataType::INT) {
            std::cout << row->values[i].int_val;
        } else {
            std::cout << "'" << row->values[i].str_val << "'";
        }
    }
    std::cout << ")\n";
}

Table::Table(const std::string& filename) {
    pager = std::make_unique<Pager>(filename);
    root_page_num = 0;
    in_transaction = false;
    tx_original_num_pages = 0;

    for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
        tx_backup[i] = nullptr;
        tx_was_cached[i] = false;
    }

    if (pager->get_num_pages() == 0) {
        schema = create_default_schema();
        void* root_node = pager->get_page(0);
        initialize_leaf_node(root_node);
        set_node_root(root_node, true);
    } else {
        // Load persistent schema or fallback
        schema = create_default_schema();
    }
}

Table::~Table() {
    tx_free_backups();
    for (uint32_t i = 0; i < pager->get_num_pages(); i++) {
        if (pager->get_page_direct(i) == nullptr) continue;
        pager->flush(i);
        std::free(pager->get_page_direct(i));
        pager->set_page_direct(i, nullptr);
    }
}

void Table::tx_free_backups() {
    for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
        if (tx_backup[i] != nullptr) {
            std::free(tx_backup[i]);
            tx_backup[i] = nullptr;
        }
    }
}

void Table::initialize_leaf_node(void* node) {
    set_node_type(node, NodeType::LEAF);
    set_node_root(node, false);
    *leaf_node_num_cells(node) = 0;
    *leaf_node_next_leaf(node) = 0;
}

void Table::initialize_internal_node(void* node) {
    set_node_type(node, NodeType::INTERNAL);
    set_node_root(node, false);
    *internal_node_num_keys(node) = 0;
    *internal_node_right_child(node) = INVALID_PAGE_NUM;
}

uint32_t Table::get_node_max_key(void* node) {
    if (get_node_type(node) == NodeType::LEAF) {
        return *leaf_node_key(node, *leaf_node_num_cells(node) - 1, schema.row_size);
    }
    void* right_child = pager->get_page(*internal_node_right_child(node));
    return get_node_max_key(right_child);
}

Cursor* Table::leaf_node_find(uint32_t page_num, uint32_t key) {
    void* node = pager->get_page(page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);

    auto cursor = new Cursor();
    cursor->table = this;
    cursor->page_num = page_num;
    cursor->end_of_table = false;

    uint32_t min_index = 0;
    uint32_t one_past_max_index = num_cells;
    while (one_past_max_index != min_index) {
        uint32_t index = (min_index + one_past_max_index) / 2;
        uint32_t key_at_index = *leaf_node_key(node, index, schema.row_size);
        if (key == key_at_index) {
            cursor->cell_num = index;
            return cursor;
        }
        if (key < key_at_index) {
            one_past_max_index = index;
        } else {
            min_index = index + 1;
        }
    }
    cursor->cell_num = min_index;
    return cursor;
}

uint32_t Table::internal_node_find_child(void* node, uint32_t key) {
    uint32_t num_keys = *internal_node_num_keys(node);
    uint32_t min_index = 0;
    uint32_t max_index = num_keys;
    while (min_index != max_index) {
        uint32_t index = (min_index + max_index) / 2;
        uint32_t key_to_right = *internal_node_key(node, index);
        if (key_to_right >= key) {
            max_index = index;
        } else {
            min_index = index + 1;
        }
    }
    return min_index;
}

Cursor* Table::internal_node_find(uint32_t page_num, uint32_t key) {
    void* node = pager->get_page(page_num);
    uint32_t child_index = internal_node_find_child(node, key);
    uint32_t child_num = *internal_node_child(node, child_index);
    void* child = pager->get_page(child_num);
    switch (get_node_type(child)) {
        case NodeType::LEAF:
            return leaf_node_find(child_num, key);
        case NodeType::INTERNAL:
            return internal_node_find(child_num, key);
    }
    return nullptr;
}

Cursor* Table::table_find(uint32_t key) {
    void* root_node = pager->get_page(root_page_num);
    if (get_node_type(root_node) == NodeType::LEAF) {
        return leaf_node_find(root_page_num, key);
    } else {
        return internal_node_find(root_page_num, key);
    }
}

Cursor* Table::table_start() {
    Cursor* cursor = table_find(0);
    void* node = pager->get_page(cursor->page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);
    cursor->end_of_table = (num_cells == 0);
    return cursor;
}

void* Table::cursor_value(Cursor* cursor) {
    void* page = pager->get_page(cursor->page_num);
    return leaf_node_value(page, cursor->cell_num, schema.row_size);
}

void Table::cursor_advance(Cursor* cursor) {
    void* node = pager->get_page(cursor->page_num);
    cursor->cell_num += 1;
    if (cursor->cell_num >= (*leaf_node_num_cells(node))) {
        uint32_t next_page_num = *leaf_node_next_leaf(node);
        if (next_page_num == 0) {
            cursor->end_of_table = true;
        } else {
            cursor->page_num = next_page_num;
            cursor->cell_num = 0;
        }
    }
}

void Table::leaf_node_delete(Cursor* cursor) {
    void* node = pager->get_page(cursor->page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);
    uint32_t cell_sz = leaf_node_cell_size(schema.row_size);
    for (uint32_t i = cursor->cell_num; i < num_cells - 1; i++) {
        std::memcpy(leaf_node_cell(node, i, schema.row_size), leaf_node_cell(node, i + 1, schema.row_size), cell_sz);
    }
    *(leaf_node_num_cells(node)) -= 1;
}

uint32_t Table::get_unused_page_num() { return pager->get_num_pages(); }

void Table::create_new_root(uint32_t right_child_page_num) {
    void* root = pager->get_page(root_page_num);
    void* right_child = pager->get_page(right_child_page_num);
    uint32_t left_child_page_num = get_unused_page_num();
    void* left_child = pager->get_page(left_child_page_num);

    if (get_node_type(root) == NodeType::INTERNAL) {
        initialize_internal_node(right_child);
        initialize_internal_node(left_child);
    }

    std::memcpy(left_child, root, PAGE_SIZE);
    set_node_root(left_child, false);

    if (get_node_type(left_child) == NodeType::INTERNAL) {
        void* child;
        for (uint32_t i = 0; i < *internal_node_num_keys(left_child); i++) {
            child = pager->get_page(*internal_node_child(left_child, i));
            *node_parent(child) = left_child_page_num;
        }
        child = pager->get_page(*internal_node_right_child(left_child));
        *node_parent(child) = left_child_page_num;
    }

    initialize_internal_node(root);
    set_node_root(root, true);
    *internal_node_num_keys(root) = 1;
    *internal_node_child(root, 0) = left_child_page_num;
    uint32_t left_child_max_key = get_node_max_key(left_child);
    *internal_node_key(root, 0) = left_child_max_key;
    *internal_node_right_child(root) = right_child_page_num;
    *node_parent(left_child) = root_page_num;
    *node_parent(right_child) = root_page_num;
}

void Table::update_internal_node_key(void* node, uint32_t old_key, uint32_t new_key) {
    uint32_t old_child_index = internal_node_find_child(node, old_key);
    *internal_node_key(node, old_child_index) = new_key;
}

void Table::internal_node_insert(uint32_t parent_page_num, uint32_t child_page_num) {
    void* parent = pager->get_page(parent_page_num);
    void* child = pager->get_page(child_page_num);
    uint32_t child_max_key = get_node_max_key(child);
    uint32_t index = internal_node_find_child(parent, child_max_key);

    uint32_t original_num_keys = *internal_node_num_keys(parent);
    if (original_num_keys >= INTERNAL_NODE_MAX_KEYS) {
        internal_node_split_and_insert(parent_page_num, child_page_num);
        return;
    }

    uint32_t right_child_page_num = *internal_node_right_child(parent);
    if (right_child_page_num == INVALID_PAGE_NUM) {
        *internal_node_right_child(parent) = child_page_num;
        return;
    }

    void* right_child = pager->get_page(right_child_page_num);
    *internal_node_num_keys(parent) = original_num_keys + 1;

    if (child_max_key > get_node_max_key(right_child)) {
        *internal_node_child(parent, original_num_keys) = right_child_page_num;
        *internal_node_key(parent, original_num_keys) = get_node_max_key(right_child);
        *internal_node_right_child(parent) = child_page_num;
    } else {
        for (uint32_t i = original_num_keys; i > index; i--) {
            void* destination = internal_node_cell(parent, i);
            void* source = internal_node_cell(parent, i - 1);
            std::memcpy(destination, source, INTERNAL_NODE_CELL_SIZE);
        }
        *internal_node_child(parent, index) = child_page_num;
        *internal_node_key(parent, index) = child_max_key;
    }
}

void Table::leaf_node_split_and_insert(Cursor* cursor, uint32_t key, DynamicRow* value) {
    void* old_node = pager->get_page(cursor->page_num);
    uint32_t old_max = get_node_max_key(old_node);
    uint32_t new_page_num = get_unused_page_num();
    void* new_node = pager->get_page(new_page_num);
    initialize_leaf_node(new_node);
    *node_parent(new_node) = *node_parent(old_node);
    *leaf_node_next_leaf(new_node) = *leaf_node_next_leaf(old_node);
    *leaf_node_next_leaf(old_node) = new_page_num;

    uint32_t max_cells = leaf_node_max_cells(schema.row_size);
    uint32_t right_split_count = (max_cells + 1) / 2;
    uint32_t left_split_count = (max_cells + 1) - right_split_count;
    uint32_t cell_sz = leaf_node_cell_size(schema.row_size);

    for (int32_t i = max_cells; i >= 0; i--) {
        void* destination_node;
        if (static_cast<uint32_t>(i) >= left_split_count) {
            destination_node = new_node;
        } else {
            destination_node = old_node;
        }
        uint32_t index_within_node = i % left_split_count;
        void* destination = leaf_node_cell(destination_node, index_within_node, schema.row_size);

        if (static_cast<uint32_t>(i) == cursor->cell_num) {
            serialize_row(value, leaf_node_value(destination_node, index_within_node, schema.row_size), schema);
            *leaf_node_key(destination_node, index_within_node, schema.row_size) = key;
        } else if (static_cast<uint32_t>(i) > cursor->cell_num) {
            std::memcpy(destination, leaf_node_cell(old_node, i - 1, schema.row_size), cell_sz);
        } else {
            std::memcpy(destination, leaf_node_cell(old_node, i, schema.row_size), cell_sz);
        }
    }

    *(leaf_node_num_cells(old_node)) = left_split_count;
    *(leaf_node_num_cells(new_node)) = right_split_count;

    if (is_node_root(old_node)) {
        return create_new_root(new_page_num);
    } else {
        uint32_t parent_page_num = *node_parent(old_node);
        uint32_t new_max = get_node_max_key(old_node);
        void* parent = pager->get_page(parent_page_num);
        update_internal_node_key(parent, old_max, new_max);
        internal_node_insert(parent_page_num, new_page_num);
        return;
    }
}

void Table::leaf_node_insert(Cursor* cursor, uint32_t key, DynamicRow* value) {
    void* node = pager->get_page(cursor->page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);
    uint32_t max_cells = leaf_node_max_cells(schema.row_size);
    if (num_cells >= max_cells) {
        leaf_node_split_and_insert(cursor, key, value);
        return;
    }
    uint32_t cell_sz = leaf_node_cell_size(schema.row_size);
    if (cursor->cell_num < num_cells) {
        for (uint32_t i = num_cells; i > cursor->cell_num; i--) {
            std::memcpy(leaf_node_cell(node, i, schema.row_size), leaf_node_cell(node, i - 1, schema.row_size), cell_sz);
        }
    }
    *(leaf_node_num_cells(node)) += 1;
    *(leaf_node_key(node, cursor->cell_num, schema.row_size)) = key;
    serialize_row(value, leaf_node_value(node, cursor->cell_num, schema.row_size), schema);
}

void Table::internal_node_split_and_insert(uint32_t parent_page_num, uint32_t child_page_num) {
    uint32_t old_page_num = parent_page_num;
    void* old_node = pager->get_page(parent_page_num);
    uint32_t old_max = get_node_max_key(old_node);

    void* child = pager->get_page(child_page_num);
    uint32_t child_max = get_node_max_key(child);

    uint32_t new_page_num = get_unused_page_num();

    uint32_t splitting_root = is_node_root(old_node);
    void* parent;
    void* new_node;

    if (splitting_root) {
        create_new_root(new_page_num);
        parent = pager->get_page(root_page_num);
        old_page_num = *internal_node_child(parent, 0);
        old_node = pager->get_page(old_page_num);
    } else {
        parent = pager->get_page(*node_parent(old_node));
        new_node = pager->get_page(new_page_num);
        initialize_internal_node(new_node);
    }

    uint32_t* old_num_keys = internal_node_num_keys(old_node);
    uint32_t cur_page_num = *internal_node_right_child(old_node);
    void* cur = pager->get_page(cur_page_num);

    internal_node_insert(new_page_num, cur_page_num);
    *node_parent(cur) = new_page_num;
    *internal_node_right_child(old_node) = INVALID_PAGE_NUM;

    for (int i = INTERNAL_NODE_MAX_KEYS - 1; i > static_cast<int>(INTERNAL_NODE_MAX_KEYS / 2); i--) {
        cur_page_num = *internal_node_child(old_node, i);
        cur = pager->get_page(cur_page_num);
        internal_node_insert(new_page_num, cur_page_num);
        *node_parent(cur) = new_page_num;
        (*old_num_keys)--;
    }

    *internal_node_right_child(old_node) = *internal_node_child(old_node, *old_num_keys - 1);
    (*old_num_keys)--;

    uint32_t max_after_split = get_node_max_key(old_node);
    uint32_t destination_page_num = child_max < max_after_split ? old_page_num : new_page_num;

    internal_node_insert(destination_page_num, child_page_num);
    *node_parent(child) = destination_page_num;

    update_internal_node_key(parent, old_max, get_node_max_key(old_node));

    if (!splitting_root) {
        internal_node_insert(*node_parent(old_node), new_page_num);
        *node_parent(new_node) = *node_parent(old_node);
    }
}

void Table::print_tree(uint32_t page_num, uint32_t indentation_level) {
    void* node = pager->get_page(page_num);
    uint32_t num_keys, child;

    auto indent = [](uint32_t level) {
        for (uint32_t i = 0; i < level; i++) std::cout << "  ";
    };

    switch (get_node_type(node)) {
        case NodeType::LEAF:
            num_keys = *leaf_node_num_cells(node);
            indent(indentation_level);
            std::cout << "- leaf (size " << num_keys << ")\n";
            for (uint32_t i = 0; i < num_keys; i++) {
                indent(indentation_level + 1);
                std::cout << "- " << *leaf_node_key(node, i, schema.row_size) << "\n";
            }
            break;
        case NodeType::INTERNAL:
            num_keys = *internal_node_num_keys(node);
            indent(indentation_level);
            std::cout << "- internal (size " << num_keys << ")\n";
            if (num_keys > 0) {
                for (uint32_t i = 0; i < num_keys; i++) {
                    child = *internal_node_child(node, i);
                    print_tree(child, indentation_level + 1);
                    indent(indentation_level + 1);
                    std::cout << "- key " << *internal_node_key(node, i) << "\n";
                }
                child = *internal_node_right_child(node);
                print_tree(child, indentation_level + 1);
            }
            break;
    }
}
