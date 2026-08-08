#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

// ============================================================================
// Data Types & Constants
// ============================================================================

constexpr size_t COLUMN_USERNAME_SIZE = 32;
constexpr size_t COLUMN_EMAIL_SIZE = 255;

struct Row {
    uint32_t id{0};
    char username[COLUMN_USERNAME_SIZE + 1]{0};
    char email[COLUMN_EMAIL_SIZE + 1]{0};
};

constexpr uint32_t ID_SIZE = sizeof(uint32_t);
constexpr uint32_t USERNAME_SIZE = COLUMN_USERNAME_SIZE + 1;
constexpr uint32_t EMAIL_SIZE = COLUMN_EMAIL_SIZE + 1;
constexpr uint32_t ID_OFFSET = 0;
constexpr uint32_t USERNAME_OFFSET = ID_OFFSET + ID_SIZE;
constexpr uint32_t EMAIL_OFFSET = USERNAME_OFFSET + USERNAME_SIZE;
constexpr uint32_t ROW_SIZE = ID_SIZE + USERNAME_SIZE + EMAIL_SIZE;

constexpr uint32_t PAGE_SIZE = 4096;
constexpr uint32_t TABLE_MAX_PAGES = 400;
constexpr uint32_t INVALID_PAGE_NUM = UINT32_MAX;
constexpr uint32_t INSERT_PAGE_SAFETY_MARGIN = 8;

enum class ExecuteResult {
    SUCCESS,
    DUPLICATE_KEY,
    NOT_FOUND,
    TX_ALREADY_ACTIVE,
    NO_ACTIVE_TX,
    TABLE_FULL
};

enum class MetaCommandResult {
    SUCCESS,
    UNRECOGNIZED_COMMAND
};

enum class PrepareResult {
    SUCCESS,
    NEGATIVE_ID,
    STRING_TOO_LONG,
    SYNTAX_ERROR,
    UNRECOGNIZED_STATEMENT
};

enum class StatementType {
    INSERT,
    SELECT,
    UPDATE,
    DELETE,
    CREATE,
    BEGIN,
    COMMIT,
    ROLLBACK
};

enum class WhereOp {
    EQ,
    NE,
    GT,
    LT,
    GE,
    LE
};

enum class NodeType : uint8_t {
    INTERNAL = 0,
    LEAF = 1
};

struct Statement {
    StatementType type{StatementType::SELECT};
    Row row_to_insert{};

    // WHERE clause
    bool has_where{false};
    std::string where_column;
    bool where_is_string{false};
    WhereOp where_op{WhereOp::EQ};
    uint32_t where_int_value{0};
    std::string where_str_value;

    bool is_count{false};
    uint32_t target_id{0};

    // SET-style UPDATE
    bool is_set_update{false};
    bool set_username{false};
    bool set_email{false};
    std::string set_username_value;
    std::string set_email_value;
};

// ============================================================================
// B+Tree Layout Helpers
// ============================================================================

constexpr uint32_t NODE_TYPE_SIZE = sizeof(uint8_t);
constexpr uint32_t NODE_TYPE_OFFSET = 0;
constexpr uint32_t IS_ROOT_SIZE = sizeof(uint8_t);
constexpr uint32_t IS_ROOT_OFFSET = NODE_TYPE_SIZE;
constexpr uint32_t PARENT_POINTER_SIZE = sizeof(uint32_t);
constexpr uint32_t PARENT_POINTER_OFFSET = IS_ROOT_OFFSET + IS_ROOT_SIZE;
constexpr uint32_t COMMON_NODE_HEADER_SIZE = NODE_TYPE_SIZE + IS_ROOT_SIZE + PARENT_POINTER_SIZE;

/* Internal Node Header Layout */
constexpr uint32_t INTERNAL_NODE_NUM_KEYS_SIZE = sizeof(uint32_t);
constexpr uint32_t INTERNAL_NODE_NUM_KEYS_OFFSET = COMMON_NODE_HEADER_SIZE;
constexpr uint32_t INTERNAL_NODE_RIGHT_CHILD_SIZE = sizeof(uint32_t);
constexpr uint32_t INTERNAL_NODE_RIGHT_CHILD_OFFSET =
    INTERNAL_NODE_NUM_KEYS_OFFSET + INTERNAL_NODE_NUM_KEYS_SIZE;
constexpr uint32_t INTERNAL_NODE_HEADER_SIZE = COMMON_NODE_HEADER_SIZE +
                                                INTERNAL_NODE_NUM_KEYS_SIZE +
                                                INTERNAL_NODE_RIGHT_CHILD_SIZE;

/* Internal Node Body Layout */
constexpr uint32_t INTERNAL_NODE_KEY_SIZE = sizeof(uint32_t);
constexpr uint32_t INTERNAL_NODE_CHILD_SIZE = sizeof(uint32_t);
constexpr uint32_t INTERNAL_NODE_CELL_SIZE = INTERNAL_NODE_CHILD_SIZE + INTERNAL_NODE_KEY_SIZE;
constexpr uint32_t INTERNAL_NODE_MAX_KEYS = 3;

/* Leaf Node Header Layout */
constexpr uint32_t LEAF_NODE_NUM_CELLS_SIZE = sizeof(uint32_t);
constexpr uint32_t LEAF_NODE_NUM_CELLS_OFFSET = COMMON_NODE_HEADER_SIZE;
constexpr uint32_t LEAF_NODE_NEXT_LEAF_SIZE = sizeof(uint32_t);
constexpr uint32_t LEAF_NODE_NEXT_LEAF_OFFSET =
    LEAF_NODE_NUM_CELLS_OFFSET + LEAF_NODE_NUM_CELLS_SIZE;
constexpr uint32_t LEAF_NODE_HEADER_SIZE = COMMON_NODE_HEADER_SIZE +
                                            LEAF_NODE_NUM_CELLS_SIZE +
                                            LEAF_NODE_NEXT_LEAF_SIZE;

/* Leaf Node Body Layout */
constexpr uint32_t LEAF_NODE_KEY_SIZE = sizeof(uint32_t);
constexpr uint32_t LEAF_NODE_KEY_OFFSET = 0;
constexpr uint32_t LEAF_NODE_VALUE_SIZE = ROW_SIZE;
constexpr uint32_t LEAF_NODE_VALUE_OFFSET = LEAF_NODE_KEY_OFFSET + LEAF_NODE_KEY_SIZE;
constexpr uint32_t LEAF_NODE_CELL_SIZE = LEAF_NODE_KEY_SIZE + LEAF_NODE_VALUE_SIZE;
constexpr uint32_t LEAF_NODE_SPACE_FOR_CELLS = PAGE_SIZE - LEAF_NODE_HEADER_SIZE;
constexpr uint32_t LEAF_NODE_MAX_CELLS = LEAF_NODE_SPACE_FOR_CELLS / LEAF_NODE_CELL_SIZE;
constexpr uint32_t LEAF_NODE_RIGHT_SPLIT_COUNT = (LEAF_NODE_MAX_CELLS + 1) / 2;
constexpr uint32_t LEAF_NODE_LEFT_SPLIT_COUNT = (LEAF_NODE_MAX_CELLS + 1) - LEAF_NODE_RIGHT_SPLIT_COUNT;

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

inline uint32_t* leaf_node_num_cells(void* node) {
    return reinterpret_cast<uint32_t*>(node_ptr(node, LEAF_NODE_NUM_CELLS_OFFSET));
}

inline uint32_t* leaf_node_next_leaf(void* node) {
    return reinterpret_cast<uint32_t*>(node_ptr(node, LEAF_NODE_NEXT_LEAF_OFFSET));
}

inline void* leaf_node_cell(void* node, uint32_t cell_num) {
    return node_ptr(node, LEAF_NODE_HEADER_SIZE + cell_num * LEAF_NODE_CELL_SIZE);
}

inline uint32_t* leaf_node_key(void* node, uint32_t cell_num) {
    return reinterpret_cast<uint32_t*>(leaf_node_cell(node, cell_num));
}

inline void* leaf_node_value(void* node, uint32_t cell_num) {
    return node_ptr(leaf_node_cell(node, cell_num), LEAF_NODE_KEY_SIZE);
}

// ============================================================================
// Serialization & Serialization Utilities
// ============================================================================

void print_row(const Row* row) {
    std::cout << "(" << row->id << ", " << row->username << ", " << row->email << ")\n";
}

void serialize_row(const Row* source, void* destination) {
    std::memcpy(node_ptr(destination, ID_OFFSET), &(source->id), ID_SIZE);
    std::memcpy(node_ptr(destination, USERNAME_OFFSET), &(source->username), USERNAME_SIZE);
    std::memcpy(node_ptr(destination, EMAIL_OFFSET), &(source->email), EMAIL_SIZE);
}

void deserialize_row(const void* source, Row* destination) {
    std::memcpy(&(destination->id), node_ptr(const_cast<void*>(source), ID_OFFSET), ID_SIZE);
    std::memcpy(&(destination->username), node_ptr(const_cast<void*>(source), USERNAME_OFFSET), USERNAME_SIZE);
    std::memcpy(&(destination->email), node_ptr(const_cast<void*>(source), EMAIL_OFFSET), EMAIL_SIZE);
}

// ============================================================================
// Pager Class
// ============================================================================

class Pager {
private:
    int file_descriptor{-1};
    uint32_t file_length{0};
    uint32_t num_pages{0};
    void* pages[TABLE_MAX_PAGES]{nullptr};

public:
    explicit Pager(const std::string& filename) {
        file_descriptor = open(filename.c_str(), O_RDWR | O_CREAT, S_IWUSR | S_IRUSR);
        if (file_descriptor == -1) {
            std::cout << "Unable to open file\n";
            std::exit(EXIT_FAILURE);
        }

        off_t length = lseek(file_descriptor, 0, SEEK_END);
        file_length = static_cast<uint32_t>(length);
        num_pages = file_length / PAGE_SIZE;

        if (file_length % PAGE_SIZE != 0) {
            std::cout << "Db file is not a whole number of pages. Corrupt file.\n";
            std::exit(EXIT_FAILURE);
        }

        for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
            pages[i] = nullptr;
        }
    }

    ~Pager() {
        for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
            if (pages[i] != nullptr) {
                std::free(pages[i]);
                pages[i] = nullptr;
            }
        }
        if (file_descriptor != -1) {
            close(file_descriptor);
        }
    }

    Pager(const Pager&) = delete;
    Pager& operator=(const Pager&) = delete;

    void* get_page(uint32_t page_num) {
        if (page_num >= TABLE_MAX_PAGES) {
            std::cout << "Tried to fetch page number out of bounds. " << page_num << " >= " << TABLE_MAX_PAGES << "\n";
            std::cout << "Table has reached its maximum size (" << TABLE_MAX_PAGES << " pages).\n";
            std::exit(EXIT_FAILURE);
        }

        if (pages[page_num] == nullptr) {
            void* page = std::malloc(PAGE_SIZE);
            uint32_t npages = file_length / PAGE_SIZE;
            if (file_length % PAGE_SIZE) {
                npages += 1;
            }
            if (page_num <= npages) {
                lseek(file_descriptor, page_num * PAGE_SIZE, SEEK_SET);
                ssize_t bytes_read = read(file_descriptor, page, PAGE_SIZE);
                if (bytes_read == -1) {
                    std::cout << "Error reading file: " << errno << "\n";
                    std::exit(EXIT_FAILURE);
                }
            }
            pages[page_num] = page;
            if (page_num >= num_pages) {
                num_pages = page_num + 1;
            }
        }
        return pages[page_num];
    }

    void flush(uint32_t page_num) {
        if (pages[page_num] == nullptr) {
            std::cout << "Tried to flush null page\n";
            std::exit(EXIT_FAILURE);
        }
        off_t offset = lseek(file_descriptor, page_num * PAGE_SIZE, SEEK_SET);
        if (offset == -1) {
            std::cout << "Error seeking: " << errno << "\n";
            std::exit(EXIT_FAILURE);
        }
        ssize_t bytes_written = write(file_descriptor, pages[page_num], PAGE_SIZE);
        if (bytes_written == -1) {
            std::cout << "Error writing: " << errno << "\n";
            std::exit(EXIT_FAILURE);
        }
    }

    uint32_t get_num_pages() const { return num_pages; }
    void set_num_pages(uint32_t n) { num_pages = n; }
    void* get_page_direct(uint32_t i) { return pages[i]; }
    void set_page_direct(uint32_t i, void* ptr) { pages[i] = ptr; }
};

// ============================================================================
// Table & Cursor Classes
// ============================================================================

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

    // Transaction state
    bool in_transaction{false};
    uint32_t tx_original_num_pages{0};
    void* tx_backup[TABLE_MAX_PAGES]{nullptr};
    bool tx_was_cached[TABLE_MAX_PAGES]{false};

    explicit Table(const std::string& filename) {
        pager = std::make_unique<Pager>(filename);
        root_page_num = 0;
        in_transaction = false;
        tx_original_num_pages = 0;

        for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
            tx_backup[i] = nullptr;
            tx_was_cached[i] = false;
        }

        if (pager->get_num_pages() == 0) {
            void* root_node = pager->get_page(0);
            initialize_leaf_node(root_node);
            set_node_root(root_node, true);
        }
    }

    ~Table() {
        tx_free_backups();
        for (uint32_t i = 0; i < pager->get_num_pages(); i++) {
            if (pager->get_page_direct(i) == nullptr) continue;
            pager->flush(i);
            std::free(pager->get_page_direct(i));
            pager->set_page_direct(i, nullptr);
        }
    }

    void tx_free_backups() {
        for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
            if (tx_backup[i] != nullptr) {
                std::free(tx_backup[i]);
                tx_backup[i] = nullptr;
            }
        }
    }

    void initialize_leaf_node(void* node) {
        set_node_type(node, NodeType::LEAF);
        set_node_root(node, false);
        *leaf_node_num_cells(node) = 0;
        *leaf_node_next_leaf(node) = 0;
    }

    void initialize_internal_node(void* node) {
        set_node_type(node, NodeType::INTERNAL);
        set_node_root(node, false);
        *internal_node_num_keys(node) = 0;
        *internal_node_right_child(node) = INVALID_PAGE_NUM;
    }

    uint32_t get_node_max_key(void* node) {
        if (get_node_type(node) == NodeType::LEAF) {
            return *leaf_node_key(node, *leaf_node_num_cells(node) - 1);
        }
        void* right_child = pager->get_page(*internal_node_right_child(node));
        return get_node_max_key(right_child);
    }

    Cursor* leaf_node_find(uint32_t page_num, uint32_t key) {
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
            uint32_t key_at_index = *leaf_node_key(node, index);
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

    uint32_t internal_node_find_child(void* node, uint32_t key) {
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

    Cursor* internal_node_find(uint32_t page_num, uint32_t key) {
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

    Cursor* table_find(uint32_t key) {
        void* root_node = pager->get_page(root_page_num);
        if (get_node_type(root_node) == NodeType::LEAF) {
            return leaf_node_find(root_page_num, key);
        } else {
            return internal_node_find(root_page_num, key);
        }
    }

    Cursor* table_start() {
        Cursor* cursor = table_find(0);
        void* node = pager->get_page(cursor->page_num);
        uint32_t num_cells = *leaf_node_num_cells(node);
        cursor->end_of_table = (num_cells == 0);
        return cursor;
    }

    void* cursor_value(Cursor* cursor) {
        void* page = pager->get_page(cursor->page_num);
        return leaf_node_value(page, cursor->cell_num);
    }

    void cursor_advance(Cursor* cursor) {
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

    void leaf_node_delete(Cursor* cursor) {
        void* node = pager->get_page(cursor->page_num);
        uint32_t num_cells = *leaf_node_num_cells(node);
        for (uint32_t i = cursor->cell_num; i < num_cells - 1; i++) {
            std::memcpy(leaf_node_cell(node, i), leaf_node_cell(node, i + 1), LEAF_NODE_CELL_SIZE);
        }
        *(leaf_node_num_cells(node)) -= 1;
    }

    uint32_t get_unused_page_num() { return pager->get_num_pages(); }

    void create_new_root(uint32_t right_child_page_num) {
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

    void update_internal_node_key(void* node, uint32_t old_key, uint32_t new_key) {
        uint32_t old_child_index = internal_node_find_child(node, old_key);
        *internal_node_key(node, old_child_index) = new_key;
    }

    void internal_node_split_and_insert(uint32_t parent_page_num, uint32_t child_page_num);

    void internal_node_insert(uint32_t parent_page_num, uint32_t child_page_num) {
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

    void leaf_node_split_and_insert(Cursor* cursor, uint32_t key, Row* value) {
        void* old_node = pager->get_page(cursor->page_num);
        uint32_t old_max = get_node_max_key(old_node);
        uint32_t new_page_num = get_unused_page_num();
        void* new_node = pager->get_page(new_page_num);
        initialize_leaf_node(new_node);
        *node_parent(new_node) = *node_parent(old_node);
        *leaf_node_next_leaf(new_node) = *leaf_node_next_leaf(old_node);
        *leaf_node_next_leaf(old_node) = new_page_num;

        for (int32_t i = LEAF_NODE_MAX_CELLS; i >= 0; i--) {
            void* destination_node;
            if (static_cast<uint32_t>(i) >= LEAF_NODE_LEFT_SPLIT_COUNT) {
                destination_node = new_node;
            } else {
                destination_node = old_node;
            }
            uint32_t index_within_node = i % LEAF_NODE_LEFT_SPLIT_COUNT;
            void* destination = leaf_node_cell(destination_node, index_within_node);

            if (static_cast<uint32_t>(i) == cursor->cell_num) {
                serialize_row(value, leaf_node_value(destination_node, index_within_node));
                *leaf_node_key(destination_node, index_within_node) = key;
            } else if (static_cast<uint32_t>(i) > cursor->cell_num) {
                std::memcpy(destination, leaf_node_cell(old_node, i - 1), LEAF_NODE_CELL_SIZE);
            } else {
                std::memcpy(destination, leaf_node_cell(old_node, i), LEAF_NODE_CELL_SIZE);
            }
        }

        *(leaf_node_num_cells(old_node)) = LEAF_NODE_LEFT_SPLIT_COUNT;
        *(leaf_node_num_cells(new_node)) = LEAF_NODE_RIGHT_SPLIT_COUNT;

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

    void leaf_node_insert(Cursor* cursor, uint32_t key, Row* value) {
        void* node = pager->get_page(cursor->page_num);
        uint32_t num_cells = *leaf_node_num_cells(node);
        if (num_cells >= LEAF_NODE_MAX_CELLS) {
            leaf_node_split_and_insert(cursor, key, value);
            return;
        }
        if (cursor->cell_num < num_cells) {
            for (uint32_t i = num_cells; i > cursor->cell_num; i--) {
                std::memcpy(leaf_node_cell(node, i), leaf_node_cell(node, i - 1), LEAF_NODE_CELL_SIZE);
            }
        }
        *(leaf_node_num_cells(node)) += 1;
        *(leaf_node_key(node, cursor->cell_num)) = key;
        serialize_row(value, leaf_node_value(node, cursor->cell_num));
    }

    void print_tree(uint32_t page_num, uint32_t indentation_level) {
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
                    std::cout << "- " << *leaf_node_key(node, i) << "\n";
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
};

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

// ============================================================================
// SQL Lexer & Parser (PostgreSQL Syntax)
// ============================================================================

struct Token {
    enum class Kind {
        Identifier,
        StringLiteral,
        Number,
        Symbol,
        End
    } kind{Kind::End};
    std::string text;
};

class Lexer {
private:
    std::string input;
    size_t pos{0};

public:
    explicit Lexer(std::string str) : input(std::move(str)) {}

    std::vector<Token> tokenize() {
        std::vector<Token> tokens;
        while (pos < input.size()) {
            if (std::isspace(static_cast<unsigned char>(input[pos]))) {
                pos++;
                continue;
            }

            if (input[pos] == ';') {
                tokens.push_back({Token::Kind::Symbol, ";"});
                pos++;
                continue;
            }

            // Single quote string literals: 'alice'
            if (input[pos] == '\'') {
                std::string str;
                pos++;
                while (pos < input.size() && input[pos] != '\'') {
                    str += input[pos];
                    pos++;
                }
                if (pos < input.size()) pos++; // consume closing quote
                tokens.push_back({Token::Kind::StringLiteral, str});
                continue;
            }

            // Multi-char operators: !=, <=, >=, <>
            if (pos + 1 < input.size()) {
                std::string op2 = input.substr(pos, 2);
                if (op2 == "!=" || op2 == "<=" || op2 == ">=" || op2 == "<>") {
                    tokens.push_back({Token::Kind::Symbol, op2});
                    pos += 2;
                    continue;
                }
            }

            // Single-char operators/symbols: =, <, >, (, ), ,
            if (std::string("=<>(),*").find(input[pos]) != std::string::npos) {
                tokens.push_back({Token::Kind::Symbol, std::string(1, input[pos])});
                pos++;
                continue;
            }

            // Numbers
            if (std::isdigit(static_cast<unsigned char>(input[pos]))) {
                std::string num;
                while (pos < input.size() && std::isdigit(static_cast<unsigned char>(input[pos]))) {
                    num += input[pos];
                    pos++;
                }
                tokens.push_back({Token::Kind::Number, num});
                continue;
            }

            // Identifiers / Keywords / Meta commands
            if (std::isalpha(static_cast<unsigned char>(input[pos])) || input[pos] == '_' || input[pos] == '.' || input[pos] == '\\') {
                std::string ident;
                while (pos < input.size() && (std::isalnum(static_cast<unsigned char>(input[pos])) || input[pos] == '_' || input[pos] == '.' || input[pos] == '\\')) {
                    ident += input[pos];
                    pos++;
                }
                tokens.push_back({Token::Kind::Identifier, ident});
                continue;
            }

            pos++; // Skip unknown chars
        }
        tokens.push_back({Token::Kind::End, ""});
        return tokens;
    }
};

class Parser {
private:
    std::vector<Token> tokens;
    size_t cursor{0};

    Token peek() const { return tokens[cursor]; }
    Token advance() { return tokens[cursor++]; }
    bool match(const std::string& text) {
        if (to_lower(peek().text) == to_lower(text)) {
            cursor++;
            return true;
        }
        return false;
    }

    static std::string to_lower(std::string str) {
        std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) { return std::tolower(c); });
        return str;
    }

    PrepareResult parse_where_clause(Statement& statement) {
        if (!match("WHERE")) return PrepareResult::SUCCESS;

        Token col = advance();
        Token op = advance();
        Token val = advance();

        if (col.kind != Token::Kind::Identifier) return PrepareResult::SYNTAX_ERROR;

        std::string col_name = to_lower(col.text);
        if (col_name != "id" && col_name != "username" && col_name != "email") {
            return PrepareResult::SYNTAX_ERROR;
        }

        statement.has_where = true;
        statement.where_column = col_name;

        if (op.text == "=") statement.where_op = WhereOp::EQ;
        else if (op.text == "!=" || op.text == "<>") statement.where_op = WhereOp::NE;
        else if (op.text == ">=") statement.where_op = WhereOp::GE;
        else if (op.text == "<=") statement.where_op = WhereOp::LE;
        else if (op.text == ">") statement.where_op = WhereOp::GT;
        else if (op.text == "<") statement.where_op = WhereOp::LT;
        else return PrepareResult::SYNTAX_ERROR;

        if (col_name == "id") {
            statement.where_is_string = false;
            try {
                long id_val = std::stol(val.text);
                if (id_val < 0) return PrepareResult::NEGATIVE_ID;
                statement.where_int_value = static_cast<uint32_t>(id_val);
            } catch (...) {
                return PrepareResult::SYNTAX_ERROR;
            }
        } else {
            statement.where_is_string = true;
            statement.where_str_value = val.text;
        }

        return PrepareResult::SUCCESS;
    }

public:
    explicit Parser(std::vector<Token> tok) : tokens(std::move(tok)) {}

    PrepareResult prepare_statement(Statement& statement) {
        if (tokens.empty() || peek().kind == Token::Kind::End) {
            return PrepareResult::SYNTAX_ERROR;
        }

        std::string first = to_lower(peek().text);

        // CREATE TABLE ...
        if (first == "create") {
            advance();
            if (!match("table")) return PrepareResult::SYNTAX_ERROR;
            statement.type = StatementType::CREATE;
            return PrepareResult::SUCCESS;
        }

        // BEGIN / START TRANSACTION
        if (first == "begin" || first == "start") {
            advance();
            if (first == "start") match("transaction");
            statement.type = StatementType::BEGIN;
            return PrepareResult::SUCCESS;
        }

        // COMMIT
        if (first == "commit") {
            advance();
            statement.type = StatementType::COMMIT;
            return PrepareResult::SUCCESS;
        }

        // ROLLBACK
        if (first == "rollback") {
            advance();
            statement.type = StatementType::ROLLBACK;
            return PrepareResult::SUCCESS;
        }

        // INSERT INTO <table> [(cols)] VALUES (id, 'user', 'email')
        if (first == "insert") {
            advance();
            match("into");
            advance(); // consume table name

            // Optional column specifier (id, username, email)
            if (peek().text == "(") {
                while (cursor < tokens.size() && peek().text != ")") advance();
                if (peek().text == ")") advance();
            }

            if (!match("values")) return PrepareResult::SYNTAX_ERROR;

            if (peek().text == "(") advance();

            Token id_tok = advance();
            if (peek().text == ",") advance();
            Token user_tok = advance();
            if (peek().text == ",") advance();
            Token email_tok = advance();

            if (peek().text == ")") advance();

            long id = 0;
            try {
                id = std::stol(id_tok.text);
            } catch (...) {
                return PrepareResult::SYNTAX_ERROR;
            }

            if (id < 0) return PrepareResult::NEGATIVE_ID;
            if (user_tok.text.length() > COLUMN_USERNAME_SIZE) return PrepareResult::STRING_TOO_LONG;
            if (email_tok.text.length() > COLUMN_EMAIL_SIZE) return PrepareResult::STRING_TOO_LONG;

            statement.type = StatementType::INSERT;
            statement.row_to_insert.id = static_cast<uint32_t>(id);
            std::strncpy(statement.row_to_insert.username, user_tok.text.c_str(), COLUMN_USERNAME_SIZE);
            statement.row_to_insert.username[COLUMN_USERNAME_SIZE] = '\0';
            std::strncpy(statement.row_to_insert.email, email_tok.text.c_str(), COLUMN_EMAIL_SIZE);
            statement.row_to_insert.email[COLUMN_EMAIL_SIZE] = '\0';

            return PrepareResult::SUCCESS;
        }

        // SELECT [* | COUNT(*)] FROM <table> [WHERE ...]
        if (first == "select") {
            advance();
            statement.type = StatementType::SELECT;

            if (to_lower(peek().text) == "count") {
                advance();
                if (peek().text == "(") advance();
                if (peek().text == "*") advance();
                if (peek().text == ")") advance();
                statement.is_count = true;
            } else if (peek().text == "*") {
                advance();
            }

            if (match("from")) {
                advance(); // consume table name
            }

            return parse_where_clause(statement);
        }

        // UPDATE <table> SET col1 = val1 [, col2 = val2] [WHERE ...]
        // OR point update syntax: UPDATE <id> <user> <email>
        if (first == "update") {
            advance();
            statement.type = StatementType::UPDATE;

            Token second = advance();
            if (to_lower(second.text) == "set" || (to_lower(peek().text) == "set")) {
                statement.is_set_update = true;
                if (to_lower(second.text) != "set") {
                    // second token was table name
                }

                while (cursor < tokens.size() && to_lower(peek().text) != "where" && peek().text != ";") {
                    if (match("set") || peek().text == ",") {
                        if (peek().text == ",") advance();
                        continue;
                    }

                    Token col = advance();
                    if (!match("=")) return PrepareResult::SYNTAX_ERROR;
                    Token val = advance();

                    std::string col_name = to_lower(col.text);
                    if (col_name == "username") {
                        if (val.text.length() > COLUMN_USERNAME_SIZE) return PrepareResult::STRING_TOO_LONG;
                        statement.set_username = true;
                        statement.set_username_value = val.text;
                    } else if (col_name == "email") {
                        if (val.text.length() > COLUMN_EMAIL_SIZE) return PrepareResult::STRING_TOO_LONG;
                        statement.set_email = true;
                        statement.set_email_value = val.text;
                    } else {
                        return PrepareResult::SYNTAX_ERROR;
                    }
                }

                if (!statement.set_username && !statement.set_email) {
                    return PrepareResult::SYNTAX_ERROR;
                }

                return parse_where_clause(statement);
            } else {
                // Classic point form: UPDATE <id> <username> <email>
                Token user_tok = advance();
                Token email_tok = advance();

                long id = 0;
                try {
                    id = std::stol(second.text);
                } catch (...) {
                    return PrepareResult::SYNTAX_ERROR;
                }

                if (id < 0) return PrepareResult::NEGATIVE_ID;
                if (user_tok.text.length() > COLUMN_USERNAME_SIZE) return PrepareResult::STRING_TOO_LONG;
                if (email_tok.text.length() > COLUMN_EMAIL_SIZE) return PrepareResult::STRING_TOO_LONG;

                statement.row_to_insert.id = static_cast<uint32_t>(id);
                std::strncpy(statement.row_to_insert.username, user_tok.text.c_str(), COLUMN_USERNAME_SIZE);
                statement.row_to_insert.username[COLUMN_USERNAME_SIZE] = '\0';
                std::strncpy(statement.row_to_insert.email, email_tok.text.c_str(), COLUMN_EMAIL_SIZE);
                statement.row_to_insert.email[COLUMN_EMAIL_SIZE] = '\0';

                return PrepareResult::SUCCESS;
            }
        }

        // DELETE FROM <table> [WHERE ...]
        if (first == "delete") {
            advance();
            statement.type = StatementType::DELETE;

            if (match("from")) {
                advance(); // consume table name
            } else {
                // Point delete: DELETE <id>
                if (std::isdigit(static_cast<unsigned char>(peek().text[0]))) {
                    Token id_tok = advance();
                    try {
                        long id = std::stol(id_tok.text);
                        if (id < 0) return PrepareResult::NEGATIVE_ID;
                        statement.target_id = static_cast<uint32_t>(id);
                        return PrepareResult::SUCCESS;
                    } catch (...) {
                        return PrepareResult::SYNTAX_ERROR;
                    }
                }
            }

            return parse_where_clause(statement);
        }

        return PrepareResult::UNRECOGNIZED_STATEMENT;
    }
};

// ============================================================================
// Execution Engine
// ============================================================================

bool row_matches_where(const Row* row, const Statement* statement) {
    if (!statement->has_where) return true;

    int cmp = 0;
    if (statement->where_column == "id") {
        uint32_t v = statement->where_int_value;
        cmp = (row->id > v) - (row->id < v);
    } else if (statement->where_column == "username") {
        int c = std::strcmp(row->username, statement->where_str_value.c_str());
        cmp = (c > 0) - (c < 0);
    } else {
        int c = std::strcmp(row->email, statement->where_str_value.c_str());
        cmp = (c > 0) - (c < 0);
    }

    switch (statement->where_op) {
        case WhereOp::EQ: return cmp == 0;
        case WhereOp::NE: return cmp != 0;
        case WhereOp::GT: return cmp > 0;
        case WhereOp::LT: return cmp < 0;
        case WhereOp::GE: return cmp >= 0;
        case WhereOp::LE: return cmp <= 0;
    }
    return false;
}

ExecuteResult execute_insert(Statement* statement, Table* table) {
    if (table->pager->get_num_pages() + INSERT_PAGE_SAFETY_MARGIN > TABLE_MAX_PAGES) {
        return ExecuteResult::TABLE_FULL;
    }

    Row* row_to_insert = &(statement->row_to_insert);
    uint32_t key_to_insert = row_to_insert->id;
    Cursor* cursor = table->table_find(key_to_insert);

    void* node = table->pager->get_page(cursor->page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);
    if (cursor->cell_num < num_cells) {
        uint32_t key_at_index = *leaf_node_key(node, cursor->cell_num);
        if (key_at_index == key_to_insert) {
            delete cursor;
            return ExecuteResult::DUPLICATE_KEY;
        }
    }

    table->leaf_node_insert(cursor, row_to_insert->id, row_to_insert);
    delete cursor;
    return ExecuteResult::SUCCESS;
}

ExecuteResult execute_update(Statement* statement, Table* table) {
    if (!statement->is_set_update) {
        uint32_t id = statement->row_to_insert.id;
        Cursor* cursor = table->table_find(id);
        void* node = table->pager->get_page(cursor->page_num);
        uint32_t num_cells = *leaf_node_num_cells(node);

        if (cursor->cell_num >= num_cells || *leaf_node_key(node, cursor->cell_num) != id) {
            delete cursor;
            return ExecuteResult::NOT_FOUND;
        }

        serialize_row(&statement->row_to_insert, leaf_node_value(node, cursor->cell_num));
        delete cursor;
        std::cout << "UPDATE 1\n";
        return ExecuteResult::SUCCESS;
    }

    Cursor* cursor = table->table_start();
    Row row;
    uint32_t count = 0;

    while (!cursor->end_of_table) {
        deserialize_row(table->cursor_value(cursor), &row);
        if (row_matches_where(&row, statement)) {
            if (statement->set_username) {
                std::strncpy(row.username, statement->set_username_value.c_str(), COLUMN_USERNAME_SIZE);
                row.username[COLUMN_USERNAME_SIZE] = '\0';
            }
            if (statement->set_email) {
                std::strncpy(row.email, statement->set_email_value.c_str(), COLUMN_EMAIL_SIZE);
                row.email[COLUMN_EMAIL_SIZE] = '\0';
            }
            serialize_row(&row, table->cursor_value(cursor));
            count++;
        }
        table->cursor_advance(cursor);
    }
    delete cursor;
    std::cout << "UPDATE " << count << "\n";
    return ExecuteResult::SUCCESS;
}

ExecuteResult execute_delete(Statement* statement, Table* table) {
    if (!statement->has_where && statement->target_id != 0) {
        uint32_t id = statement->target_id;
        Cursor* cursor = table->table_find(id);
        void* node = table->pager->get_page(cursor->page_num);
        uint32_t num_cells = *leaf_node_num_cells(node);

        if (cursor->cell_num >= num_cells || *leaf_node_key(node, cursor->cell_num) != id) {
            delete cursor;
            return ExecuteResult::NOT_FOUND;
        }
        table->leaf_node_delete(cursor);
        delete cursor;
        std::cout << "DELETE 1\n";
        return ExecuteResult::SUCCESS;
    }

    std::vector<uint32_t> ids_to_delete;
    Cursor* cursor = table->table_start();
    Row row;

    while (!cursor->end_of_table) {
        deserialize_row(table->cursor_value(cursor), &row);
        if (row_matches_where(&row, statement)) {
            ids_to_delete.push_back(row.id);
        }
        table->cursor_advance(cursor);
    }
    delete cursor;

    for (uint32_t id : ids_to_delete) {
        Cursor* c = table->table_find(id);
        void* node = table->pager->get_page(c->page_num);
        uint32_t num_cells = *leaf_node_num_cells(node);
        if (c->cell_num < num_cells && *leaf_node_key(node, c->cell_num) == id) {
            table->leaf_node_delete(c);
        }
        delete c;
    }
    std::cout << "DELETE " << ids_to_delete.size() << "\n";
    return ExecuteResult::SUCCESS;
}

ExecuteResult execute_select(Statement* statement, Table* table) {
    Cursor* cursor = table->table_start();
    Row row;
    uint32_t match_count = 0;

    while (!cursor->end_of_table) {
        deserialize_row(table->cursor_value(cursor), &row);
        if (row_matches_where(&row, statement)) {
            match_count++;
            if (!statement->is_count) print_row(&row);
        }
        table->cursor_advance(cursor);
    }
    delete cursor;

    if (statement->is_count) {
        std::cout << match_count << " row(s).\n";
    }
    return ExecuteResult::SUCCESS;
}

ExecuteResult execute_begin(Table* table) {
    if (table->in_transaction) return ExecuteResult::TX_ALREADY_ACTIVE;

    table->in_transaction = true;
    table->tx_original_num_pages = table->pager->get_num_pages();

    for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
        void* page_ptr = table->pager->get_page_direct(i);
        if (page_ptr != nullptr) {
            table->tx_backup[i] = std::malloc(PAGE_SIZE);
            std::memcpy(table->tx_backup[i], page_ptr, PAGE_SIZE);
            table->tx_was_cached[i] = true;
        } else {
            table->tx_backup[i] = nullptr;
            table->tx_was_cached[i] = false;
        }
    }
    return ExecuteResult::SUCCESS;
}

ExecuteResult execute_commit(Table* table) {
    if (!table->in_transaction) return ExecuteResult::NO_ACTIVE_TX;

    table->tx_free_backups();
    for (uint32_t i = 0; i < table->pager->get_num_pages(); i++) {
        if (table->pager->get_page_direct(i) != nullptr) {
            table->pager->flush(i);
        }
    }
    table->in_transaction = false;
    return ExecuteResult::SUCCESS;
}

ExecuteResult execute_rollback(Table* table) {
    if (!table->in_transaction) return ExecuteResult::NO_ACTIVE_TX;

    for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
        if (i < table->tx_original_num_pages) {
            if (table->tx_was_cached[i]) {
                std::memcpy(table->pager->get_page_direct(i), table->tx_backup[i], PAGE_SIZE);
                std::free(table->tx_backup[i]);
                table->tx_backup[i] = nullptr;
            } else if (table->pager->get_page_direct(i) != nullptr) {
                std::free(table->pager->get_page_direct(i));
                table->pager->set_page_direct(i, nullptr);
            }
        } else {
            if (table->pager->get_page_direct(i) != nullptr) {
                std::free(table->pager->get_page_direct(i));
                table->pager->set_page_direct(i, nullptr);
            }
            if (table->tx_backup[i] != nullptr) {
                std::free(table->tx_backup[i]);
                table->tx_backup[i] = nullptr;
            }
        }
    }
    table->pager->set_num_pages(table->tx_original_num_pages);
    table->in_transaction = false;
    return ExecuteResult::SUCCESS;
}

ExecuteResult execute_statement(Statement* statement, Table* table) {
    switch (statement->type) {
        case StatementType::INSERT:
            return execute_insert(statement, table);
        case StatementType::SELECT:
            return execute_select(statement, table);
        case StatementType::UPDATE:
            return execute_update(statement, table);
        case StatementType::DELETE:
            return execute_delete(statement, table);
        case StatementType::CREATE:
            std::cout << "CREATE TABLE\n";
            return ExecuteResult::SUCCESS;
        case StatementType::BEGIN:
            return execute_begin(table);
        case StatementType::COMMIT:
            return execute_commit(table);
        case StatementType::ROLLBACK:
            return execute_rollback(table);
    }
    return ExecuteResult::SUCCESS;
}

// ============================================================================
// Meta Commands & REPL CLI
// ============================================================================

void print_help() {
    std::cout << "PostgreSQL SQL Commands:\n"
              << "  CREATE TABLE users (id INT PRIMARY KEY, username VARCHAR(32), email VARCHAR(255));\n"
              << "  INSERT INTO users VALUES (1, 'alice', 'alice@example.com');\n"
              << "  SELECT * FROM users [WHERE id = 1];\n"
              << "  SELECT COUNT(*) FROM users [WHERE email = 'alice@example.com'];\n"
              << "  UPDATE users SET username = 'bob' WHERE id = 1;\n"
              << "  DELETE FROM users WHERE id = 1;\n"
              << "  BEGIN; | COMMIT; | ROLLBACK;\n"
              << "Meta commands:\n"
              << "  \\q or .exit      quit the database shell\n"
              << "  \\d or .btree     print the B+tree structure\n"
              << "  \\c or .constants print page size constants\n"
              << "  \\? or .help      show this message\n";
}

void print_constants() {
    std::cout << "ROW_SIZE: " << ROW_SIZE << "\n"
              << "COMMON_NODE_HEADER_SIZE: " << COMMON_NODE_HEADER_SIZE << "\n"
              << "LEAF_NODE_HEADER_SIZE: " << LEAF_NODE_HEADER_SIZE << "\n"
              << "LEAF_NODE_CELL_SIZE: " << LEAF_NODE_CELL_SIZE << "\n"
              << "LEAF_NODE_SPACE_FOR_CELLS: " << LEAF_NODE_SPACE_FOR_CELLS << "\n"
              << "LEAF_NODE_MAX_CELLS: " << LEAF_NODE_MAX_CELLS << "\n";
}

MetaCommandResult do_meta_command(const std::string& input, Table* table) {
    if (input == ".exit" || input == "\\q") {
        std::exit(EXIT_SUCCESS);
    } else if (input == ".btree" || input == "\\d") {
        std::cout << "Tree:\n";
        table->print_tree(0, 0);
        return MetaCommandResult::SUCCESS;
    } else if (input == ".constants" || input == "\\c") {
        std::cout << "Constants:\n";
        print_constants();
        return MetaCommandResult::SUCCESS;
    } else if (input == ".help" || input == "\\?") {
        print_help();
        return MetaCommandResult::SUCCESS;
    }
    return MetaCommandResult::UNRECOGNIZED_COMMAND;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Must supply a database filename.\n";
        std::exit(EXIT_FAILURE);
    }

    std::string filename = argv[1];
    auto table = std::make_unique<Table>(filename);

    std::string input_buffer;
    while (true) {
        std::cout << "db=# ";
        if (!std::getline(std::cin, input_buffer)) {
            break;
        }

        if (input_buffer.empty()) continue;

        if (input_buffer[0] == '.' || input_buffer[0] == '\\') {
            switch (do_meta_command(input_buffer, table.get())) {
                case MetaCommandResult::SUCCESS:
                    continue;
                case MetaCommandResult::UNRECOGNIZED_COMMAND:
                    std::cout << "Unrecognized command '" << input_buffer << "'\n";
                    continue;
            }
        }

        Lexer lexer(input_buffer);
        auto tokens = lexer.tokenize();

        Parser parser(tokens);
        Statement statement;

        switch (parser.prepare_statement(statement)) {
            case PrepareResult::SUCCESS:
                break;
            case PrepareResult::NEGATIVE_ID:
                std::cout << "ID must be positive.\n";
                continue;
            case PrepareResult::STRING_TOO_LONG:
                std::cout << "String is too long.\n";
                continue;
            case PrepareResult::SYNTAX_ERROR:
                std::cout << "Syntax error. Could not parse statement.\n";
                continue;
            case PrepareResult::UNRECOGNIZED_STATEMENT:
                std::cout << "Unrecognized keyword at start of '" << input_buffer << "'.\n";
                continue;
        }

        switch (execute_statement(&statement, table.get())) {
            case ExecuteResult::SUCCESS:
                if (statement.type == StatementType::INSERT) {
                    std::cout << "INSERT 0 1\n";
                } else if (statement.type == StatementType::BEGIN) {
                    std::cout << "BEGIN\n";
                } else if (statement.type == StatementType::COMMIT) {
                    std::cout << "COMMIT\n";
                } else if (statement.type == StatementType::ROLLBACK) {
                    std::cout << "ROLLBACK\n";
                }
                break;
            case ExecuteResult::DUPLICATE_KEY:
                std::cout << "Error: Duplicate key.\n";
                break;
            case ExecuteResult::NOT_FOUND:
                std::cout << "Error: row not found.\n";
                break;
            case ExecuteResult::TX_ALREADY_ACTIVE:
                std::cout << "Error: a transaction is already active.\n";
                break;
            case ExecuteResult::NO_ACTIVE_TX:
                std::cout << "Error: no active transaction.\n";
                break;
            case ExecuteResult::TABLE_FULL:
                std::cout << "Error: table is full (max " << TABLE_MAX_PAGES << " pages).\n";
                break;
        }
    }
    return 0;
}
