#ifndef BTREE_HPP
#define BTREE_HPP

#include <cstdint>
#include <cstdlib>
#include <iostream>

constexpr uint32_t PAGE_SIZE = 4096;
constexpr uint32_t TABLE_MAX_PAGES = 400;
constexpr uint32_t INVALID_PAGE_NUM = UINT32_MAX;
constexpr uint32_t INSERT_PAGE_SAFETY_MARGIN = 8;
constexpr uint32_t SCHEMA_MAGIC = 0x5343484D; // 'SCHM'

enum class NodeType : uint8_t {
    INTERNAL = 0,
    LEAF = 1
};

constexpr uint32_t NODE_TYPE_SIZE = sizeof(uint8_t);
constexpr uint32_t NODE_TYPE_OFFSET = 0;
constexpr uint32_t IS_ROOT_SIZE = sizeof(uint8_t);
constexpr uint32_t IS_ROOT_OFFSET = NODE_TYPE_SIZE;
constexpr uint32_t PARENT_POINTER_SIZE = sizeof(uint32_t);
constexpr uint32_t PARENT_POINTER_OFFSET = IS_ROOT_OFFSET + IS_ROOT_SIZE;
constexpr uint32_t COMMON_NODE_HEADER_SIZE = NODE_TYPE_SIZE + IS_ROOT_SIZE + PARENT_POINTER_SIZE;

constexpr uint32_t INTERNAL_NODE_NUM_KEYS_SIZE = sizeof(uint32_t);
constexpr uint32_t INTERNAL_NODE_NUM_KEYS_OFFSET = COMMON_NODE_HEADER_SIZE;
constexpr uint32_t INTERNAL_NODE_RIGHT_CHILD_SIZE = sizeof(uint32_t);
constexpr uint32_t INTERNAL_NODE_RIGHT_CHILD_OFFSET =
    INTERNAL_NODE_NUM_KEYS_OFFSET + INTERNAL_NODE_NUM_KEYS_SIZE;

constexpr uint32_t INTERNAL_NODE_HEADER_SIZE = COMMON_NODE_HEADER_SIZE +
                                                INTERNAL_NODE_NUM_KEYS_SIZE +
                                                INTERNAL_NODE_RIGHT_CHILD_SIZE;

constexpr uint32_t INTERNAL_NODE_KEY_SIZE = sizeof(uint32_t);
constexpr uint32_t INTERNAL_NODE_CHILD_SIZE = sizeof(uint32_t);
constexpr uint32_t INTERNAL_NODE_CELL_SIZE = INTERNAL_NODE_CHILD_SIZE + INTERNAL_NODE_KEY_SIZE;
constexpr uint32_t INTERNAL_NODE_MAX_KEYS = 3;

constexpr uint32_t LEAF_NODE_NUM_CELLS_SIZE = sizeof(uint32_t);
constexpr uint32_t LEAF_NODE_NUM_CELLS_OFFSET = COMMON_NODE_HEADER_SIZE;
constexpr uint32_t LEAF_NODE_NEXT_LEAF_SIZE = sizeof(uint32_t);
constexpr uint32_t LEAF_NODE_NEXT_LEAF_OFFSET =
    LEAF_NODE_NUM_CELLS_OFFSET + LEAF_NODE_NUM_CELLS_SIZE;
constexpr uint32_t LEAF_NODE_HEADER_SIZE = COMMON_NODE_HEADER_SIZE +
                                            LEAF_NODE_NUM_CELLS_SIZE +
                                            LEAF_NODE_NEXT_LEAF_SIZE;

constexpr uint32_t LEAF_NODE_KEY_SIZE = sizeof(uint32_t);

inline uint8_t* node_ptr(void* node, size_t offset) {
    return static_cast<uint8_t*>(node) + offset;
}

inline NodeType get_node_type(void* node) {
    return static_cast<NodeType>(*node_ptr(node, NODE_TYPE_OFFSET));
}

inline void set_node_type(void* node, NodeType type) {
    *node_ptr(node, NODE_TYPE_OFFSET) = static_cast<uint8_t>(type);
}

inline bool is_node_root(void* node) {
    return static_cast<bool>(*node_ptr(node, IS_ROOT_OFFSET));
}

inline void set_node_root(void* node, bool is_root) {
    *node_ptr(node, IS_ROOT_OFFSET) = static_cast<uint8_t>(is_root);
}

inline uint32_t* node_parent(void* node) {
    return reinterpret_cast<uint32_t*>(node_ptr(node, PARENT_POINTER_OFFSET));
}

inline uint32_t* internal_node_num_keys(void* node) {
    return reinterpret_cast<uint32_t*>(node_ptr(node, INTERNAL_NODE_NUM_KEYS_OFFSET));
}

inline uint32_t* internal_node_right_child(void* node) {
    return reinterpret_cast<uint32_t*>(node_ptr(node, INTERNAL_NODE_RIGHT_CHILD_OFFSET));
}

inline uint32_t* internal_node_cell(void* node, uint32_t cell_num) {
    return reinterpret_cast<uint32_t*>(node_ptr(node, INTERNAL_NODE_HEADER_SIZE + cell_num * INTERNAL_NODE_CELL_SIZE));
}

inline uint32_t* internal_node_child(void* node, uint32_t child_num) {
    uint32_t num_keys = *internal_node_num_keys(node);
    if (child_num > num_keys) {
        std::cout << "Tried to access child_num " << child_num << " > num_keys " << num_keys << "\n";
        std::exit(EXIT_FAILURE);
    } else if (child_num == num_keys) {
        uint32_t* right_child = internal_node_right_child(node);
        if (*right_child == INVALID_PAGE_NUM) {
            std::cout << "Tried to access right child of node, but was invalid page\n";
            std::exit(EXIT_FAILURE);
        }
        return right_child;
    } else {
        uint32_t* child = internal_node_cell(node, child_num);
        if (*child == INVALID_PAGE_NUM) {
            std::cout << "Tried to access child " << child_num << " of node, but was invalid page\n";
            std::exit(EXIT_FAILURE);
        }
        return child;
    }
}

inline uint32_t* internal_node_key(void* node, uint32_t key_num) {
    return reinterpret_cast<uint32_t*>(node_ptr(internal_node_cell(node, key_num), INTERNAL_NODE_CHILD_SIZE));
}

inline uint32_t leaf_node_cell_size(uint32_t row_size) {
    return LEAF_NODE_KEY_SIZE + row_size;
}

inline uint32_t leaf_node_max_cells(uint32_t row_size) {
    uint32_t space = PAGE_SIZE - LEAF_NODE_HEADER_SIZE;
    return space / leaf_node_cell_size(row_size);
}

inline uint32_t* leaf_node_num_cells(void* node) {
    return reinterpret_cast<uint32_t*>(node_ptr(node, LEAF_NODE_NUM_CELLS_OFFSET));
}

inline uint32_t* leaf_node_next_leaf(void* node) {
    return reinterpret_cast<uint32_t*>(node_ptr(node, LEAF_NODE_NEXT_LEAF_OFFSET));
}

inline void* leaf_node_cell(void* node, uint32_t cell_num, uint32_t row_size) {
    return node_ptr(node, LEAF_NODE_HEADER_SIZE + cell_num * leaf_node_cell_size(row_size));
}

inline uint32_t* leaf_node_key(void* node, uint32_t cell_num, uint32_t row_size) {
    return reinterpret_cast<uint32_t*>(leaf_node_cell(node, cell_num, row_size));
}

inline void* leaf_node_value(void* node, uint32_t cell_num, uint32_t row_size) {
    return node_ptr(leaf_node_cell(node, cell_num, row_size), LEAF_NODE_KEY_SIZE);
}

#endif // BTREE_HPP
