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
// Data Types & Schema Definition
// ============================================================================

constexpr uint32_t PAGE_SIZE = 4096;
constexpr uint32_t TABLE_MAX_PAGES = 400;
constexpr uint32_t INVALID_PAGE_NUM = UINT32_MAX;
constexpr uint32_t INSERT_PAGE_SAFETY_MARGIN = 8;
constexpr uint32_t SCHEMA_MAGIC = 0x5343484D; // 'SCHM'

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

    void add_column(const std::string& name, DataType type, uint32_t length = 0, bool is_pk = false) {
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

    int find_column(const std::string& name) const {
        for (size_t i = 0; i < columns.size(); ++i) {
            std::string c_name = columns[i].name;
            std::string q_name = name;
            std::transform(c_name.begin(), c_name.end(), c_name.begin(), ::tolower);
            std::transform(q_name.begin(), q_name.end(), q_name.begin(), ::tolower);
            if (c_name == q_name) return static_cast<int>(i);
        }
        return -1;
    }
};

// Default Fallback Schema if database is initialized without CREATE TABLE
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

// ============================================================================
// Dynamic Value & Row Structures
// ============================================================================

struct Value {
    DataType type{DataType::INT};
    int32_t int_val{0};
    std::string str_val;
};

struct DynamicRow {
    std::vector<Value> values;

    uint32_t get_pk_value(const Schema& schema) const {
        if (schema.primary_key_index < values.size()) {
            return static_cast<uint32_t>(values[schema.primary_key_index].int_val);
        }
        return 0;
    }
};

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

// ============================================================================
// Serialization Utilities
// ============================================================================

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

// ============================================================================
// WHERE-clause AST
// ============================================================================

struct Expr {
    virtual ~Expr() = default;
    virtual bool evaluate(const DynamicRow& row, const Schema& schema) const = 0;
};

struct Literal {
    bool is_string{false};
    uint32_t int_value{0};
    std::string str_value;
};

struct ComparisonExpr : Expr {
    std::string column;
    WhereOp op{WhereOp::EQ};
    Literal value;

    bool evaluate(const DynamicRow& row, const Schema& schema) const override {
        int col_idx = schema.find_column(column);
        if (col_idx < 0 || static_cast<size_t>(col_idx) >= row.values.size()) return false;

        const auto& col_def = schema.columns[col_idx];
        const auto& cell_val = row.values[col_idx];

        int cmp = 0;
        if (col_def.type == DataType::INT) {
            int32_t v = static_cast<int32_t>(value.int_value);
            cmp = (cell_val.int_val > v) - (cell_val.int_val < v);
        } else {
            int c = std::strcmp(cell_val.str_val.c_str(), value.str_value.c_str());
            cmp = (c > 0) - (c < 0);
        }

        switch (op) {
            case WhereOp::EQ: return cmp == 0;
            case WhereOp::NE: return cmp != 0;
            case WhereOp::GT: return cmp > 0;
            case WhereOp::LT: return cmp < 0;
            case WhereOp::GE: return cmp >= 0;
            case WhereOp::LE: return cmp <= 0;
        }
        return false;
    }
};

struct LogicalExpr : Expr {
    enum class LogicOp { AND, OR } op;
    std::unique_ptr<Expr> left;
    std::unique_ptr<Expr> right;

    LogicalExpr(LogicOp op_, std::unique_ptr<Expr> left_, std::unique_ptr<Expr> right_)
        : op(op_), left(std::move(left_)), right(std::move(right_)) {}

    bool evaluate(const DynamicRow& row, const Schema& schema) const override {
        if (op == LogicOp::AND) return left->evaluate(row, schema) && right->evaluate(row, schema);
        return left->evaluate(row, schema) || right->evaluate(row, schema);
    }
};

struct UpdateAssignment {
    std::string column_name;
    std::string value_text;
};

struct Statement {
    StatementType type{StatementType::SELECT};
    DynamicRow row_to_insert;
    std::string table_name;
    Schema created_schema;

    std::unique_ptr<Expr> where;
    bool is_count{false};
    uint32_t target_id{0};

    // SET-style UPDATE
    bool is_set_update{false};
    std::vector<UpdateAssignment> update_assignments;
};

// ============================================================================
// B+Tree Layout Helpers (Dynamic Row Support)
// ============================================================================

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
    Schema schema;

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
            schema = create_default_schema();
            void* root_node = pager->get_page(0);
            initialize_leaf_node(root_node);
            set_node_root(root_node, true);
        } else {
            // Load persistent schema or fallback
            schema = create_default_schema();
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
            return *leaf_node_key(node, *leaf_node_num_cells(node) - 1, schema.row_size);
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
        return leaf_node_value(page, cursor->cell_num, schema.row_size);
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
        uint32_t cell_sz = leaf_node_cell_size(schema.row_size);
        for (uint32_t i = cursor->cell_num; i < num_cells - 1; i++) {
            std::memcpy(leaf_node_cell(node, i, schema.row_size), leaf_node_cell(node, i + 1, schema.row_size), cell_sz);
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

    void leaf_node_split_and_insert(Cursor* cursor, uint32_t key, DynamicRow* value) {
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

    void leaf_node_insert(Cursor* cursor, uint32_t key, DynamicRow* value) {
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
// SQL Lexer & Parser
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

            if (input[pos] == '\'') {
                std::string str;
                pos++;
                while (pos < input.size() && input[pos] != '\'') {
                    str += input[pos];
                    pos++;
                }
                if (pos < input.size()) pos++;
                tokens.push_back({Token::Kind::StringLiteral, str});
                continue;
            }

            if (pos + 1 < input.size()) {
                std::string op2 = input.substr(pos, 2);
                if (op2 == "!=" || op2 == "<=" || op2 == ">=" || op2 == "<>") {
                    tokens.push_back({Token::Kind::Symbol, op2});
                    pos += 2;
                    continue;
                }
            }

            if (std::string("=<>(),*").find(input[pos]) != std::string::npos) {
                tokens.push_back({Token::Kind::Symbol, std::string(1, input[pos])});
                pos++;
                continue;
            }

            if (std::isdigit(static_cast<unsigned char>(input[pos]))) {
                std::string num;
                while (pos < input.size() && std::isdigit(static_cast<unsigned char>(input[pos]))) {
                    num += input[pos];
                    pos++;
                }
                tokens.push_back({Token::Kind::Number, num});
                continue;
            }

            if (std::isalpha(static_cast<unsigned char>(input[pos])) || input[pos] == '_' || input[pos] == '.' || input[pos] == '\\') {
                std::string ident;
                while (pos < input.size() && (std::isalnum(static_cast<unsigned char>(input[pos])) || input[pos] == '_' || input[pos] == '.' || input[pos] == '\\')) {
                    ident += input[pos];
                    pos++;
                }
                tokens.push_back({Token::Kind::Identifier, ident});
                continue;
            }

            pos++;
        }
        tokens.push_back({Token::Kind::End, ""});
        return tokens;
    }
};

class Parser {
private:
    std::vector<Token> tokens;
    size_t cursor{0};
    const Schema& schema;

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

    PrepareResult parse_comparison(std::unique_ptr<Expr>& out) {
        Token col = advance();
        Token op = advance();
        Token val = advance();

        if (col.kind != Token::Kind::Identifier) return PrepareResult::SYNTAX_ERROR;

        int col_idx = schema.find_column(col.text);
        if (col_idx < 0) return PrepareResult::SYNTAX_ERROR;

        auto cmp = std::make_unique<ComparisonExpr>();
        cmp->column = schema.columns[col_idx].name;

        if (op.text == "=") cmp->op = WhereOp::EQ;
        else if (op.text == "!=" || op.text == "<>") cmp->op = WhereOp::NE;
        else if (op.text == ">=") cmp->op = WhereOp::GE;
        else if (op.text == "<=") cmp->op = WhereOp::LE;
        else if (op.text == ">") cmp->op = WhereOp::GT;
        else if (op.text == "<") cmp->op = WhereOp::LT;
        else return PrepareResult::SYNTAX_ERROR;

        if (schema.columns[col_idx].type == DataType::INT) {
            cmp->value.is_string = false;
            try {
                size_t consumed = 0;
                long int_v = std::stol(val.text, &consumed);
                if (consumed != val.text.size()) return PrepareResult::SYNTAX_ERROR;
                if (schema.columns[col_idx].is_primary_key && int_v < 0) return PrepareResult::NEGATIVE_ID;
                cmp->value.int_value = static_cast<uint32_t>(int_v);
            } catch (...) {
                return PrepareResult::SYNTAX_ERROR;
            }
        } else {
            cmp->value.is_string = true;
            cmp->value.str_value = val.text;
        }

        out = std::move(cmp);
        return PrepareResult::SUCCESS;
    }

    PrepareResult parse_primary(std::unique_ptr<Expr>& out) {
        if (peek().text == "(") {
            advance();
            PrepareResult result = parse_or_expr(out);
            if (result != PrepareResult::SUCCESS) return result;
            if (peek().text != ")") return PrepareResult::SYNTAX_ERROR;
            advance();
            return PrepareResult::SUCCESS;
        }
        return parse_comparison(out);
    }

    PrepareResult parse_and_expr(std::unique_ptr<Expr>& out) {
        std::unique_ptr<Expr> left;
        PrepareResult result = parse_primary(left);
        if (result != PrepareResult::SUCCESS) return result;

        while (to_lower(peek().text) == "and") {
            advance();
            std::unique_ptr<Expr> right;
            result = parse_primary(right);
            if (result != PrepareResult::SUCCESS) return result;
            left = std::make_unique<LogicalExpr>(LogicalExpr::LogicOp::AND,
                                                  std::move(left), std::move(right));
        }

        out = std::move(left);
        return PrepareResult::SUCCESS;
    }

    PrepareResult parse_or_expr(std::unique_ptr<Expr>& out) {
        std::unique_ptr<Expr> left;
        PrepareResult result = parse_and_expr(left);
        if (result != PrepareResult::SUCCESS) return result;

        while (to_lower(peek().text) == "or") {
            advance();
            std::unique_ptr<Expr> right;
            result = parse_and_expr(right);
            if (result != PrepareResult::SUCCESS) return result;
            left = std::make_unique<LogicalExpr>(LogicalExpr::LogicOp::OR,
                                                  std::move(left), std::move(right));
        }

        out = std::move(left);
        return PrepareResult::SUCCESS;
    }

    PrepareResult parse_where_clause(Statement& statement) {
        if (!match("WHERE")) return PrepareResult::SUCCESS;
        return parse_or_expr(statement.where);
    }

public:
    Parser(std::vector<Token> tok, const Schema& sch)
        : tokens(std::move(tok)), schema(sch) {}

    PrepareResult prepare_statement(Statement& statement) {
        if (tokens.empty() || peek().kind == Token::Kind::End) {
            return PrepareResult::SYNTAX_ERROR;
        }

        std::string first = to_lower(peek().text);

        // CREATE TABLE <name> (col1 INT PRIMARY KEY, col2 VARCHAR(32), ...)
        if (first == "create") {
            advance();
            if (!match("table")) return PrepareResult::SYNTAX_ERROR;
            Token tbl = advance();
            statement.type = StatementType::CREATE;
            statement.table_name = tbl.text;

            if (peek().text != "(") return PrepareResult::SYNTAX_ERROR;
            advance(); // consume '('

            Schema new_schema;
            new_schema.table_name = tbl.text;

            while (cursor < tokens.size() && peek().text != ")") {
                Token col_name = advance();
                if (col_name.kind != Token::Kind::Identifier) return PrepareResult::SYNTAX_ERROR;

                Token col_type = advance();
                std::string type_str = to_lower(col_type.text);

                DataType dt = DataType::INT;
                uint32_t len = 0;

                if (type_str == "int" || type_str == "integer") {
                    dt = DataType::INT;
                } else if (type_str == "varchar" || type_str == "string" || type_str == "char") {
                    dt = DataType::VARCHAR;
                    len = 32;
                    if (peek().text == "(") {
                        advance();
                        Token len_tok = advance();
                        try {
                            len = static_cast<uint32_t>(std::stoul(len_tok.text));
                        } catch (...) {
                            return PrepareResult::SYNTAX_ERROR;
                        }
                        if (peek().text == ")") advance();
                    }
                } else {
                    return PrepareResult::SYNTAX_ERROR;
                }

                bool is_pk = false;
                if (to_lower(peek().text) == "primary") {
                    advance();
                    if (to_lower(peek().text) == "key") advance();
                    is_pk = true;
                }

                new_schema.add_column(col_name.text, dt, len, is_pk);

                if (peek().text == ",") {
                    advance();
                }
            }

            if (peek().text == ")") advance();
            statement.created_schema = new_schema;
            return PrepareResult::SUCCESS;
        }

        if (first == "begin" || first == "start") {
            advance();
            if (first == "start") match("transaction");
            statement.type = StatementType::BEGIN;
            return PrepareResult::SUCCESS;
        }

        if (first == "commit") {
            advance();
            statement.type = StatementType::COMMIT;
            return PrepareResult::SUCCESS;
        }

        if (first == "rollback") {
            advance();
            statement.type = StatementType::ROLLBACK;
            return PrepareResult::SUCCESS;
        }

        // INSERT INTO <table> VALUES (val1, val2, ...)
        if (first == "insert") {
            advance();
            match("into");
            advance(); // consume table name

            if (peek().text == "(") {
                while (cursor < tokens.size() && peek().text != ")") advance();
                if (peek().text == ")") advance();
            }

            if (!match("values")) return PrepareResult::SYNTAX_ERROR;
            if (peek().text == "(") advance();

            statement.type = StatementType::INSERT;
            statement.row_to_insert.values.clear();

            for (size_t i = 0; i < schema.columns.size(); ++i) {
                Token val_tok = advance();
                if (peek().text == ",") advance();

                Value v;
                v.type = schema.columns[i].type;

                if (schema.columns[i].type == DataType::INT) {
                    try {
                        v.int_val = static_cast<int32_t>(std::stol(val_tok.text));
                        if (schema.columns[i].is_primary_key && v.int_val < 0) return PrepareResult::NEGATIVE_ID;
                    } catch (...) {
                        return PrepareResult::SYNTAX_ERROR;
                    }
                } else {
                    if (val_tok.text.length() > schema.columns[i].length) return PrepareResult::STRING_TOO_LONG;
                    v.str_val = val_tok.text;
                }
                statement.row_to_insert.values.push_back(v);
            }

            if (peek().text == ")") advance();
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
                advance();
            }

            return parse_where_clause(statement);
        }

        // UPDATE <table> SET col1 = val1 [, col2 = val2] [WHERE ...]
        if (first == "update") {
            advance();
            statement.type = StatementType::UPDATE;
            statement.is_set_update = true;

            advance(); // table name
            match("set");

            while (cursor < tokens.size() && to_lower(peek().text) != "where" && peek().text != ";") {
                if (peek().text == ",") {
                    advance();
                    continue;
                }

                Token col = advance();
                if (!match("=")) return PrepareResult::SYNTAX_ERROR;
                Token val = advance();

                statement.update_assignments.push_back({col.text, val.text});
            }

            return parse_where_clause(statement);
        }

        // DELETE FROM <table> [WHERE ...]
        if (first == "delete") {
            advance();
            statement.type = StatementType::DELETE;

            if (match("from")) {
                advance();
            }

            return parse_where_clause(statement);
        }

        return PrepareResult::UNRECOGNIZED_STATEMENT;
    }
};

// ============================================================================
// Execution Engine
// ============================================================================

bool row_matches_where(const DynamicRow* row, const Statement* statement, const Schema& schema) {
    if (!statement->where) return true;
    return statement->where->evaluate(*row, schema);
}

ExecuteResult execute_insert(Statement* statement, Table* table) {
    if (table->pager->get_num_pages() + INSERT_PAGE_SAFETY_MARGIN > TABLE_MAX_PAGES) {
        return ExecuteResult::TABLE_FULL;
    }

    uint32_t key_to_insert = statement->row_to_insert.get_pk_value(table->schema);
    Cursor* cursor = table->table_find(key_to_insert);

    void* node = table->pager->get_page(cursor->page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);
    if (cursor->cell_num < num_cells) {
        uint32_t key_at_index = *leaf_node_key(node, cursor->cell_num, table->schema.row_size);
        if (key_at_index == key_to_insert) {
            delete cursor;
            return ExecuteResult::DUPLICATE_KEY;
        }
    }

    table->leaf_node_insert(cursor, key_to_insert, &statement->row_to_insert);
    delete cursor;
    return ExecuteResult::SUCCESS;
}

ExecuteResult execute_update(Statement* statement, Table* table) {
    Cursor* cursor = table->table_start();
    DynamicRow row;
    uint32_t count = 0;

    while (!cursor->end_of_table) {
        deserialize_row(table->cursor_value(cursor), &row, table->schema);
        if (row_matches_where(&row, statement, table->schema)) {
            for (const auto& assign : statement->update_assignments) {
                int col_idx = table->schema.find_column(assign.column_name);
                if (col_idx >= 0 && static_cast<size_t>(col_idx) < row.values.size()) {
                    if (table->schema.columns[col_idx].type == DataType::INT) {
                        row.values[col_idx].int_val = static_cast<int32_t>(std::stol(assign.value_text));
                    } else {
                        row.values[col_idx].str_val = assign.value_text;
                    }
                }
            }
            serialize_row(&row, table->cursor_value(cursor), table->schema);
            count++;
        }
        table->cursor_advance(cursor);
    }
    delete cursor;
    std::cout << "UPDATE " << count << "\n";
    return ExecuteResult::SUCCESS;
}

ExecuteResult execute_delete(Statement* statement, Table* table) {
    std::vector<uint32_t> keys_to_delete;
    Cursor* cursor = table->table_start();
    DynamicRow row;

    while (!cursor->end_of_table) {
        deserialize_row(table->cursor_value(cursor), &row, table->schema);
        if (row_matches_where(&row, statement, table->schema)) {
            keys_to_delete.push_back(row.get_pk_value(table->schema));
        }
        table->cursor_advance(cursor);
    }
    delete cursor;

    for (uint32_t key : keys_to_delete) {
        Cursor* c = table->table_find(key);
        void* node = table->pager->get_page(c->page_num);
        uint32_t num_cells = *leaf_node_num_cells(node);
        if (c->cell_num < num_cells && *leaf_node_key(node, c->cell_num, table->schema.row_size) == key) {
            table->leaf_node_delete(c);
        }
        delete c;
    }
    std::cout << "DELETE " << keys_to_delete.size() << "\n";
    return ExecuteResult::SUCCESS;
}

ExecuteResult execute_select(Statement* statement, Table* table) {
    Cursor* cursor = table->table_start();
    DynamicRow row;
    uint32_t match_count = 0;

    while (!cursor->end_of_table) {
        deserialize_row(table->cursor_value(cursor), &row, table->schema);
        if (row_matches_where(&row, statement, table->schema)) {
            match_count++;
            if (!statement->is_count) print_row(&row, table->schema);
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
            table->schema = statement->created_schema;
            std::cout << "CREATE TABLE (" << table->schema.columns.size() << " columns configured)\n";
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
// Meta Commands & Shell
// ============================================================================

void print_help() {
    std::cout << "SQL Commands:\n"
              << "  CREATE TABLE <name> (<pk_col> INT PRIMARY KEY, <col2> VARCHAR(32), <col3> INT, ...);\n"
              << "  INSERT INTO <name> VALUES (<val1>, '<val2>', ...);\n"
              << "  SELECT * FROM <name> [WHERE <col> = <val>];\n"
              << "  SELECT COUNT(*) FROM <name> [WHERE <col> = <val>];\n"
              << "  UPDATE <name> SET <col> = <val> WHERE <col> = <val>;\n"
              << "  DELETE FROM <name> WHERE <col> = <val>;\n"
              << "  BEGIN; | COMMIT; | ROLLBACK;\n"
              << "Meta commands:\n"
              << "  \\q or .exit      quit the shell\n"
              << "  \\d or .btree     print the B+tree structure\n"
              << "  \\c or .constants print page size constants\n"
              << "  \\? or .help      show this message\n";
}

void print_constants(const Table* table) {
    std::cout << "ROW_SIZE: " << table->schema.row_size << "\n"
              << "COMMON_NODE_HEADER_SIZE: " << COMMON_NODE_HEADER_SIZE << "\n"
              << "LEAF_NODE_HEADER_SIZE: " << LEAF_NODE_HEADER_SIZE << "\n"
              << "LEAF_NODE_CELL_SIZE: " << leaf_node_cell_size(table->schema.row_size) << "\n"
              << "LEAF_NODE_MAX_CELLS: " << leaf_node_max_cells(table->schema.row_size) << "\n";
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
        print_constants(table);
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

        Parser parser(tokens, table->schema);
        Statement statement;

        switch (parser.prepare_statement(statement)) {
            case PrepareResult::SUCCESS:
                break;
            case PrepareResult::NEGATIVE_ID:
                std::cout << "ID must be positive.\n";
                continue;
            case PrepareResult::STRING_TOO_LONG:
                std::cout << "String is too long for column budget.\n";
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
