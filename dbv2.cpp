// db.cpp - Modern C++17 port of the simple B+ tree SQL engine
// Compile: g++ -std=c++17 -O2 -Wall -Wextra -o db db.cpp
// Run:     ./db mydb.db

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// ============================================================================
// Constants
// ============================================================================

constexpr size_t PAGE_SIZE                 = 4096;
constexpr size_t TABLE_MAX_PAGES           = 400;
constexpr uint32_t INVALID_PAGE_NUM        = UINT32_MAX;
constexpr size_t INSERT_PAGE_SAFETY_MARGIN = 8;
constexpr uint32_t SCHEMA_MAGIC            = 0x5343484D;

constexpr size_t MAX_COLUMNS     = 32;
constexpr size_t MAX_NAME_LEN    = 64;
constexpr size_t MAX_STR_LEN     = 256;
constexpr size_t MAX_TOKENS      = 256;
constexpr size_t MAX_ASSIGNMENTS = 32;

// ============================================================================
// Helpers
// ============================================================================

inline bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    return std::equal(a.begin(), a.end(), b.begin(),
                      [](char x, char y) {
                          return std::tolower(static_cast<unsigned char>(x)) ==
                                 std::tolower(static_cast<unsigned char>(y));
                      });
}

inline std::string to_lower(std::string_view s) {
    std::string r;
    r.reserve(s.size());
    for (unsigned char c : s)
        r.push_back(static_cast<char>(std::tolower(c)));
    return r;
}

// Fixed-size buffer helpers used only for the on-disk layout
template <size_t N>
void write_fixed(char (&dst)[N], std::string_view src) {
    std::fill(std::begin(dst), std::end(dst), '\0');
    size_t n = std::min(N - 1, src.size());
    std::copy_n(src.data(), n, dst);
}

template <size_t N>
std::string_view read_fixed(const char (&src)[N]) {
    size_t len = 0;
    while (len < N && src[len] != '\0') ++len;
    return {src, len};
}

// ============================================================================
// Data types & schema
// ============================================================================

enum class DataType : int32_t { INT = 0, VARCHAR = 1 };

struct ColumnDef {
    char name[MAX_NAME_LEN]{};
    DataType type = DataType::INT;
    uint32_t length = 0;
    uint32_t offset = 0;
    uint32_t size = 0;
    bool is_primary_key = false;
};

struct Schema {
    bool has_schema = false;
    char table_name[MAX_NAME_LEN]{};
    std::array<ColumnDef, MAX_COLUMNS> columns{};
    uint32_t num_columns = 0;
    uint32_t row_size = 0;
    uint32_t primary_key_index = 0;
};

void schema_add_column(Schema& schema, std::string_view name, DataType type,
                       uint32_t length, bool is_pk) {
    if (schema.num_columns >= MAX_COLUMNS) return;

    auto& col = schema.columns[schema.num_columns];
    write_fixed(col.name, name);
    col.type = type;
    col.is_primary_key = is_pk;
    col.offset = schema.row_size;

    if (type == DataType::INT) {
        col.length = 0;
        col.size = sizeof(int32_t);
    } else {
        col.length = length > 0 ? length : 32;
        col.size = col.length + 1;
    }

    if (is_pk) schema.primary_key_index = schema.num_columns;

    schema.row_size += col.size;
    ++schema.num_columns;
    schema.has_schema = true;
}

int schema_find_column(const Schema& schema, std::string_view name) {
    for (uint32_t i = 0; i < schema.num_columns; ++i) {
        if (iequals(read_fixed(schema.columns[i].name), name))
            return static_cast<int>(i);
    }
    return -1;
}

// ============================================================================
// Dynamic value / row
// ============================================================================

struct Value {
    DataType type = DataType::INT;
    int32_t int_val = 0;
    char str_val[MAX_STR_LEN]{};
};

struct DynamicRow {
    std::array<Value, MAX_COLUMNS> values{};
    uint32_t num_values = 0;
};

uint32_t get_pk_value(const DynamicRow& row, const Schema& schema) {
    if (schema.primary_key_index < row.num_values)
        return static_cast<uint32_t>(row.values[schema.primary_key_index].int_val);
    return 0;
}

// ============================================================================
// Enums
// ============================================================================

enum class ExecuteResult {
    Success, DuplicateKey, NotFound, TxAlreadyActive, NoActiveTx, TableFull
};

enum class MetaCommandResult { Success, Unrecognized };

enum class PrepareResult {
    Success, NegativeId, StringTooLong, SyntaxError, UnrecognizedStatement
};

enum class StatementType {
    Insert, Select, Update, Delete, Create, Begin, Commit, Rollback
};

enum class WhereOp { Eq, Neq, Gt, Lt, Ge, Le };
enum class NodeType : uint8_t { Internal = 0, Leaf = 1 };
enum class ExprType { Comparison, Logical };
enum class LogicalOp { And, Or };

// ============================================================================
// Serialization
// ============================================================================

void serialize_row(const DynamicRow& source, void* destination, const Schema& schema) {
    auto* dest = static_cast<std::byte*>(destination);
    std::fill_n(dest, schema.row_size, std::byte{0});

    for (uint32_t i = 0; i < schema.num_columns; ++i) {
        const auto& col = schema.columns[i];
        auto* dest_ptr = dest + col.offset;

        if (i >= source.num_values) continue;

        const auto& val = source.values[i];
        if (col.type == DataType::INT) {
            int32_t v = val.int_val;
            std::memcpy(dest_ptr, &v, sizeof(v));
        } else {
            auto sv = read_fixed(val.str_val);
            size_t n = std::min(static_cast<size_t>(col.length), sv.size());
            std::memcpy(dest_ptr, sv.data(), n);
            dest_ptr[col.length] = std::byte{0};
        }
    }
}

void deserialize_row(const void* source, DynamicRow& destination, const Schema& schema) {
    destination.num_values = schema.num_columns;
    const auto* src = static_cast<const std::byte*>(source);

    for (uint32_t i = 0; i < schema.num_columns; ++i) {
        const auto& col = schema.columns[i];
        const auto* src_ptr = src + col.offset;
        auto& val = destination.values[i];

        if (col.type == DataType::INT) {
            int32_t v = 0;
            std::memcpy(&v, src_ptr, sizeof(v));
            val.type = DataType::INT;
            val.int_val = v;
        } else {
            val.type = DataType::VARCHAR;
            std::memcpy(val.str_val, src_ptr, col.size);
            val.str_val[col.size - 1] = '\0';
        }
    }
}

void print_row(const DynamicRow& row, const Schema& schema) {
    std::cout << '(';
    for (uint32_t i = 0; i < schema.num_columns; ++i) {
        if (i) std::cout << ", ";
        if (schema.columns[i].type == DataType::INT)
            std::cout << row.values[i].int_val;
        else
            std::cout << '\'' << read_fixed(row.values[i].str_val) << '\'';
    }
    std::cout << ")\n";
}

// ============================================================================
// Expression tree (owned by unique_ptr)
// ============================================================================

struct Literal {
    bool is_string = false;
    uint32_t int_value = 0;
    char str_value[MAX_STR_LEN]{};
};

struct Expr {
    ExprType type = ExprType::Comparison;

    // comparison
    char column[MAX_NAME_LEN]{};
    WhereOp op = WhereOp::Eq;
    Literal value;

    // logical
    LogicalOp log_op = LogicalOp::And;
    std::unique_ptr<Expr> left;
    std::unique_ptr<Expr> right;
};

bool evaluate_expr(const Expr* expr, const DynamicRow& row, const Schema& schema) {
    if (!expr) return true;

    if (expr->type == ExprType::Logical) {
        bool l = evaluate_expr(expr->left.get(), row, schema);
        bool r = evaluate_expr(expr->right.get(), row, schema);
        return expr->log_op == LogicalOp::And ? (l && r) : (l || r);
    }

    int col_idx = schema_find_column(schema, read_fixed(expr->column));
    if (col_idx < 0 || static_cast<uint32_t>(col_idx) >= row.num_values) return false;

    const auto& col_def = schema.columns[col_idx];
    const auto& cell = row.values[col_idx];

    int cmp = 0;
    if (col_def.type == DataType::INT) {
        int32_t v = static_cast<int32_t>(expr->value.int_value);
        cmp = (cell.int_val > v) - (cell.int_val < v);
    } else {
        auto s1 = read_fixed(cell.str_val);
        auto s2 = read_fixed(expr->value.str_value);
        cmp = s1.compare(s2);
    }

    switch (expr->op) {
        case WhereOp::Eq:  return cmp == 0;
        case WhereOp::Neq: return cmp != 0;
        case WhereOp::Gt:  return cmp > 0;
        case WhereOp::Lt:  return cmp < 0;
        case WhereOp::Ge:  return cmp >= 0;
        case WhereOp::Le:  return cmp <= 0;
    }
    return false;
}

struct UpdateAssignment {
    char column_name[MAX_NAME_LEN]{};
    char value_text[MAX_STR_LEN]{};
};

struct Statement {
    StatementType type = StatementType::Insert;
    DynamicRow row_to_insert;
    char table_name[MAX_NAME_LEN]{};
    Schema created_schema;

    std::unique_ptr<Expr> where_expr;
    bool is_count = false;

    bool is_set_update = false;
    std::array<UpdateAssignment, MAX_ASSIGNMENTS> update_assignments{};
    uint32_t num_update_assignments = 0;
};

// ============================================================================
// B+ tree layout (byte offsets stay identical to original)
// ============================================================================

constexpr size_t NODE_TYPE_SIZE        = 1;
constexpr size_t NODE_TYPE_OFFSET      = 0;
constexpr size_t IS_ROOT_SIZE          = 1;
constexpr size_t IS_ROOT_OFFSET        = NODE_TYPE_OFFSET + NODE_TYPE_SIZE;
constexpr size_t PARENT_POINTER_SIZE   = 4;
constexpr size_t PARENT_POINTER_OFFSET = IS_ROOT_OFFSET + IS_ROOT_SIZE;
constexpr size_t COMMON_NODE_HEADER_SIZE = NODE_TYPE_SIZE + IS_ROOT_SIZE + PARENT_POINTER_SIZE;

constexpr size_t INTERNAL_NODE_NUM_KEYS_SIZE     = 4;
constexpr size_t INTERNAL_NODE_NUM_KEYS_OFFSET   = COMMON_NODE_HEADER_SIZE;
constexpr size_t INTERNAL_NODE_RIGHT_CHILD_SIZE  = 4;
constexpr size_t INTERNAL_NODE_RIGHT_CHILD_OFFSET = INTERNAL_NODE_NUM_KEYS_OFFSET + INTERNAL_NODE_NUM_KEYS_SIZE;
constexpr size_t INTERNAL_NODE_HEADER_SIZE       = COMMON_NODE_HEADER_SIZE + INTERNAL_NODE_NUM_KEYS_SIZE + INTERNAL_NODE_RIGHT_CHILD_SIZE;

constexpr size_t INTERNAL_NODE_KEY_SIZE   = 4;
constexpr size_t INTERNAL_NODE_CHILD_SIZE = 4;
constexpr size_t INTERNAL_NODE_CELL_SIZE  = INTERNAL_NODE_CHILD_SIZE + INTERNAL_NODE_KEY_SIZE;
constexpr size_t INTERNAL_NODE_MAX_KEYS   = 3;

constexpr size_t LEAF_NODE_NUM_CELLS_SIZE  = 4;
constexpr size_t LEAF_NODE_NUM_CELLS_OFFSET = COMMON_NODE_HEADER_SIZE;
constexpr size_t LEAF_NODE_NEXT_LEAF_SIZE  = 4;
constexpr size_t LEAF_NODE_NEXT_LEAF_OFFSET = LEAF_NODE_NUM_CELLS_OFFSET + LEAF_NODE_NUM_CELLS_SIZE;
constexpr size_t LEAF_NODE_HEADER_SIZE     = COMMON_NODE_HEADER_SIZE + LEAF_NODE_NUM_CELLS_SIZE + LEAF_NODE_NEXT_LEAF_SIZE;
constexpr size_t LEAF_NODE_KEY_SIZE        = 4;

inline std::byte* node_bytes(void* node) { return static_cast<std::byte*>(node); }
inline const std::byte* node_bytes(const void* node) { return static_cast<const std::byte*>(node); }

NodeType get_node_type(const void* node) {
    return static_cast<NodeType>(*(node_bytes(node) + NODE_TYPE_OFFSET));
}
void set_node_type(void* node, NodeType t) {
    *(node_bytes(node) + NODE_TYPE_OFFSET) = static_cast<std::byte>(t);
}

bool is_node_root(const void* node) {
    return *(node_bytes(node) + IS_ROOT_OFFSET) != std::byte{0};
}
void set_node_root(void* node, bool root) {
    *(node_bytes(node) + IS_ROOT_OFFSET) = root ? std::byte{1} : std::byte{0};
}

uint32_t* node_parent(void* node) {
    return reinterpret_cast<uint32_t*>(node_bytes(node) + PARENT_POINTER_OFFSET);
}

uint32_t* internal_node_num_keys(void* node) {
    return reinterpret_cast<uint32_t*>(node_bytes(node) + INTERNAL_NODE_NUM_KEYS_OFFSET);
}
uint32_t* internal_node_right_child(void* node) {
    return reinterpret_cast<uint32_t*>(node_bytes(node) + INTERNAL_NODE_RIGHT_CHILD_OFFSET);
}
uint32_t* internal_node_cell(void* node, uint32_t cell) {
    return reinterpret_cast<uint32_t*>(
        node_bytes(node) + INTERNAL_NODE_HEADER_SIZE + cell * INTERNAL_NODE_CELL_SIZE);
}

uint32_t* internal_node_child(void* node, uint32_t child_num) {
    uint32_t nkeys = *internal_node_num_keys(node);
    if (child_num > nkeys) {
        std::cerr << "Tried to access child_num " << child_num << " > num_keys " << nkeys << '\n';
        std::exit(1);
    }
    if (child_num == nkeys) {
        auto* right = internal_node_right_child(node);
        if (*right == INVALID_PAGE_NUM) {
            std::cerr << "Tried to access right child of node, but was invalid page\n";
            std::exit(1);
        }
        return right;
    }
    auto* child = internal_node_cell(node, child_num);
    if (*child == INVALID_PAGE_NUM) {
        std::cerr << "Tried to access child " << child_num << " of node, but was invalid page\n";
        std::exit(1);
    }
    return child;
}

uint32_t* internal_node_key(void* node, uint32_t key_num) {
    auto* cell = internal_node_cell(node, key_num);
    return reinterpret_cast<uint32_t*>(reinterpret_cast<std::byte*>(cell) + INTERNAL_NODE_CHILD_SIZE);
}

uint32_t leaf_node_cell_size(uint32_t row_size) {
    return static_cast<uint32_t>(LEAF_NODE_KEY_SIZE + row_size);
}
uint32_t leaf_node_max_cells(uint32_t row_size) {
    if (row_size == 0) return 0;
    return static_cast<uint32_t>((PAGE_SIZE - LEAF_NODE_HEADER_SIZE) / leaf_node_cell_size(row_size));
}

uint32_t* leaf_node_num_cells(void* node) {
    return reinterpret_cast<uint32_t*>(node_bytes(node) + LEAF_NODE_NUM_CELLS_OFFSET);
}
uint32_t* leaf_node_next_leaf(void* node) {
    return reinterpret_cast<uint32_t*>(node_bytes(node) + LEAF_NODE_NEXT_LEAF_OFFSET);
}

void* leaf_node_cell(void* node, uint32_t cell_num, uint32_t row_size) {
    return node_bytes(node) + LEAF_NODE_HEADER_SIZE + cell_num * leaf_node_cell_size(row_size);
}
uint32_t* leaf_node_key(void* node, uint32_t cell_num, uint32_t row_size) {
    return static_cast<uint32_t*>(leaf_node_cell(node, cell_num, row_size));
}
void* leaf_node_value(void* node, uint32_t cell_num, uint32_t row_size) {
    return static_cast<std::byte*>(leaf_node_cell(node, cell_num, row_size)) + LEAF_NODE_KEY_SIZE;
}

// ============================================================================
// Pager (RAII)
// ============================================================================

struct Pager {
    std::FILE* file = nullptr;
    uint32_t file_length = 0;
    uint32_t num_pages = 0;
    std::array<std::unique_ptr<std::byte[]>, TABLE_MAX_PAGES> pages{};

    Pager() = default;
    Pager(const Pager&) = delete;
    Pager& operator=(const Pager&) = delete;

    ~Pager() {
        if (file) std::fclose(file);
    }
};

std::unique_ptr<Pager> pager_open(std::string_view filename) {
    auto pager = std::make_unique<Pager>();

    pager->file = std::fopen(std::string(filename).c_str(), "r+b");
    if (!pager->file) {
        pager->file = std::fopen(std::string(filename).c_str(), "w+b");
        if (!pager->file) {
            std::cerr << "Unable to open file\n";
            std::exit(1);
        }
    }

    std::fseek(pager->file, 0, SEEK_END);
    long fsize = std::ftell(pager->file);
    std::fseek(pager->file, 0, SEEK_SET);

    pager->file_length = static_cast<uint32_t>(fsize);
    pager->num_pages = pager->file_length / PAGE_SIZE;

    if (pager->file_length % PAGE_SIZE != 0) {
        std::cerr << "Db file is not a whole number of pages. Corrupt file.\n";
        std::exit(1);
    }
    return pager;
}

void* pager_get_page(Pager& pager, uint32_t page_num) {
    if (page_num >= TABLE_MAX_PAGES) {
        std::cerr << "Tried to fetch page number out of bounds\n";
        std::exit(1);
    }

    if (!pager.pages[page_num]) {
        auto page = std::make_unique<std::byte[]>(PAGE_SIZE);
        std::fill_n(page.get(), PAGE_SIZE, std::byte{0});

        long offset = static_cast<long>(page_num) * PAGE_SIZE;
        if (static_cast<uint32_t>(offset) < pager.file_length) {
            std::fseek(pager.file, offset, SEEK_SET);
            std::fread(page.get(), 1, PAGE_SIZE, pager.file);
        }
        pager.pages[page_num] = std::move(page);
        if (page_num >= pager.num_pages)
            pager.num_pages = page_num + 1;
    }
    return pager.pages[page_num].get();
}

void pager_flush(Pager& pager, uint32_t page_num) {
    if (!pager.pages[page_num]) {
        std::cerr << "Tried to flush null page\n";
        std::exit(1);
    }
    long offset = static_cast<long>(page_num) * PAGE_SIZE;
    std::fseek(pager.file, offset, SEEK_SET);
    if (std::fwrite(pager.pages[page_num].get(), 1, PAGE_SIZE, pager.file) != PAGE_SIZE) {
        std::cerr << "Error writing to file\n";
        std::exit(1);
    }
}

// ============================================================================
// Table & Cursor
// ============================================================================

struct Table {
    std::unique_ptr<Pager> pager;
    uint32_t root_page_num = 0;
    Schema schema;

    bool in_transaction = false;
    uint32_t tx_original_num_pages = 0;
    std::array<std::unique_ptr<std::byte[]>, TABLE_MAX_PAGES> tx_backup{};
    std::array<bool, TABLE_MAX_PAGES> tx_was_cached{};
};

struct Cursor {
    Table* table = nullptr;
    uint32_t page_num = 0;
    uint32_t cell_num = 0;
    bool end_of_table = false;
};

void initialize_leaf_node(void* node) {
    set_node_type(node, NodeType::Leaf);
    set_node_root(node, false);
    *leaf_node_num_cells(node) = 0;
    *leaf_node_next_leaf(node) = 0;
}

void initialize_internal_node(void* node) {
    set_node_type(node, NodeType::Internal);
    set_node_root(node, false);
    *internal_node_num_keys(node) = 0;
    *internal_node_right_child(node) = INVALID_PAGE_NUM;
}

void tx_free_backups(Table& table) {
    for (auto& p : table.tx_backup) p.reset();
}

struct MetaPage {
    uint32_t magic = 0;
    uint32_t root_page_num = 0;
    Schema schema;
};

std::unique_ptr<Table> table_open(std::string_view filename) {
    auto table = std::make_unique<Table>();
    table->pager = pager_open(filename);

    if (table->pager->num_pages == 0) {
        table->root_page_num = 1;
        table->schema.has_schema = false;

        auto* meta = static_cast<MetaPage*>(pager_get_page(*table->pager, 0));
        meta->magic = SCHEMA_MAGIC;
        meta->root_page_num = table->root_page_num;
        meta->schema = table->schema;

        void* root = pager_get_page(*table->pager, table->root_page_num);
        initialize_leaf_node(root);
        set_node_root(root, true);
    } else {
        auto* meta = static_cast<MetaPage*>(pager_get_page(*table->pager, 0));
        if (meta->magic == SCHEMA_MAGIC) {
            table->root_page_num = meta->root_page_num;
            table->schema = meta->schema;
        } else {
            table->root_page_num = 1;
            table->schema.has_schema = false;
        }
    }
    return table;
}

void table_close(Table& table) {
    tx_free_backups(table);
    for (uint32_t i = 0; i < table.pager->num_pages; ++i) {
        if (table.pager->pages[i]) {
            pager_flush(*table.pager, i);
            table.pager->pages[i].reset();
        }
    }
}

uint32_t get_node_max_key(Table& table, void* node) {
    if (get_node_type(node) == NodeType::Leaf) {
        uint32_t n = *leaf_node_num_cells(node);
        return *leaf_node_key(node, n - 1, table.schema.row_size);
    }
    void* right = pager_get_page(*table.pager, *internal_node_right_child(node));
    return get_node_max_key(table, right);
}

std::unique_ptr<Cursor> leaf_node_find(Table& table, uint32_t page_num, uint32_t key) {
    void* node = pager_get_page(*table.pager, page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);

    auto cursor = std::make_unique<Cursor>();
    cursor->table = &table;
    cursor->page_num = page_num;
    cursor->end_of_table = false;

    uint32_t lo = 0, hi = num_cells;
    while (lo != hi) {
        uint32_t mid = (lo + hi) / 2;
        uint32_t key_at = *leaf_node_key(node, mid, table.schema.row_size);
        if (key == key_at) {
            cursor->cell_num = mid;
            return cursor;
        }
        if (key < key_at) hi = mid;
        else lo = mid + 1;
    }
    cursor->cell_num = lo;
    return cursor;
}

uint32_t internal_node_find_child(void* node, uint32_t key) {
    uint32_t nkeys = *internal_node_num_keys(node);
    uint32_t lo = 0, hi = nkeys;
    while (lo != hi) {
        uint32_t mid = (lo + hi) / 2;
        if (*internal_node_key(node, mid) >= key) hi = mid;
        else lo = mid + 1;
    }
    return lo;
}

std::unique_ptr<Cursor> internal_node_find(Table& table, uint32_t page_num, uint32_t key) {
    void* node = pager_get_page(*table.pager, page_num);
    uint32_t child_idx = internal_node_find_child(node, key);
    uint32_t child_num = *internal_node_child(node, child_idx);
    void* child = pager_get_page(*table.pager, child_num);

    if (get_node_type(child) == NodeType::Leaf)
        return leaf_node_find(table, child_num, key);
    return internal_node_find(table, child_num, key);
}

std::unique_ptr<Cursor> table_find(Table& table, uint32_t key) {
    void* root = pager_get_page(*table.pager, table.root_page_num);
    if (get_node_type(root) == NodeType::Leaf)
        return leaf_node_find(table, table.root_page_num, key);
    return internal_node_find(table, table.root_page_num, key);
}

std::unique_ptr<Cursor> table_start(Table& table) {
    auto cursor = table_find(table, 0);
    void* node = pager_get_page(*table.pager, cursor->page_num);
    cursor->end_of_table = (*leaf_node_num_cells(node) == 0);
    return cursor;
}

void* cursor_value(Cursor& cursor) {
    void* page = pager_get_page(*cursor.table->pager, cursor.page_num);
    return leaf_node_value(page, cursor.cell_num, cursor.table->schema.row_size);
}

void cursor_advance(Cursor& cursor) {
    void* node = pager_get_page(*cursor.table->pager, cursor.page_num);
    ++cursor.cell_num;
    if (cursor.cell_num >= *leaf_node_num_cells(node)) {
        uint32_t next = *leaf_node_next_leaf(node);
        if (next == 0) cursor.end_of_table = true;
        else {
            cursor.page_num = next;
            cursor.cell_num = 0;
        }
    }
}

void leaf_node_delete(Cursor& cursor) {
    void* node = pager_get_page(*cursor.table->pager, cursor.page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);
    uint32_t cell_sz = leaf_node_cell_size(cursor.table->schema.row_size);

    for (uint32_t i = cursor.cell_num; i + 1 < num_cells; ++i) {
        void* dst = leaf_node_cell(node, i, cursor.table->schema.row_size);
        void* src = leaf_node_cell(node, i + 1, cursor.table->schema.row_size);
        std::memcpy(dst, src, cell_sz);
    }
    --*leaf_node_num_cells(node);
}

uint32_t get_unused_page_num(Table& table) {
    return table.pager->num_pages;
}

void create_new_root(Table& table, uint32_t right_child_page_num) {
    void* root = pager_get_page(*table.pager, table.root_page_num);
    void* right_child = pager_get_page(*table.pager, right_child_page_num);
    uint32_t left_page = get_unused_page_num(table);
    void* left_child = pager_get_page(*table.pager, left_page);

    if (get_node_type(root) == NodeType::Internal) {
        initialize_internal_node(right_child);
        initialize_internal_node(left_child);
    }

    std::memcpy(left_child, root, PAGE_SIZE);
    set_node_root(left_child, false);

    if (get_node_type(left_child) == NodeType::Internal) {
        for (uint32_t i = 0; i < *internal_node_num_keys(left_child); ++i) {
            void* ch = pager_get_page(*table.pager, *internal_node_child(left_child, i));
            *node_parent(ch) = left_page;
        }
        void* ch = pager_get_page(*table.pager, *internal_node_right_child(left_child));
        *node_parent(ch) = left_page;
    }

    initialize_internal_node(root);
    set_node_root(root, true);
    *internal_node_num_keys(root) = 1;
    *internal_node_child(root, 0) = left_page;
    *internal_node_key(root, 0) = get_node_max_key(table, left_child);
    *internal_node_right_child(root) = right_child_page_num;
    *node_parent(left_child) = table.root_page_num;
    *node_parent(right_child) = table.root_page_num;
}

void update_internal_node_key(void* node, uint32_t old_key, uint32_t new_key) {
    uint32_t idx = internal_node_find_child(node, old_key);
    *internal_node_key(node, idx) = new_key;
}

void internal_node_split_and_insert(Table& table, uint32_t parent_page_num, uint32_t child_page_num);

void internal_node_insert(Table& table, uint32_t parent_page_num, uint32_t child_page_num) {
    void* parent = pager_get_page(*table.pager, parent_page_num);
    void* child = pager_get_page(*table.pager, child_page_num);
    uint32_t child_max = get_node_max_key(table, child);
    uint32_t index = internal_node_find_child(parent, child_max);

    uint32_t orig_keys = *internal_node_num_keys(parent);
    if (orig_keys >= INTERNAL_NODE_MAX_KEYS) {
        internal_node_split_and_insert(table, parent_page_num, child_page_num);
        return;
    }

    uint32_t right_page = *internal_node_right_child(parent);
    if (right_page == INVALID_PAGE_NUM) {
        *internal_node_right_child(parent) = child_page_num;
        return;
    }

    void* right_child = pager_get_page(*table.pager, right_page);
    *internal_node_num_keys(parent) = orig_keys + 1;

    if (child_max > get_node_max_key(table, right_child)) {
        *internal_node_child(parent, orig_keys) = right_page;
        *internal_node_key(parent, orig_keys) = get_node_max_key(table, right_child);
        *internal_node_right_child(parent) = child_page_num;
    } else {
        for (uint32_t i = orig_keys; i > index; --i) {
            std::memcpy(internal_node_cell(parent, i),
                        internal_node_cell(parent, i - 1),
                        INTERNAL_NODE_CELL_SIZE);
        }
        *internal_node_child(parent, index) = child_page_num;
        *internal_node_key(parent, index) = child_max;
    }
}

void internal_node_split_and_insert(Table& table, uint32_t parent_page_num, uint32_t child_page_num) {
    uint32_t old_page = parent_page_num;
    void* old_node = pager_get_page(*table.pager, parent_page_num);
    uint32_t old_max = get_node_max_key(table, old_node);

    void* child = pager_get_page(*table.pager, child_page_num);
    uint32_t child_max = get_node_max_key(table, child);

    uint32_t new_page = get_unused_page_num(table);
    bool splitting_root = is_node_root(old_node);

    void* parent = nullptr;
    void* new_node = nullptr;

    if (splitting_root) {
        create_new_root(table, new_page);
        parent = pager_get_page(*table.pager, table.root_page_num);
        old_page = *internal_node_child(parent, 0);
        old_node = pager_get_page(*table.pager, old_page);
    } else {
        parent = pager_get_page(*table.pager, *node_parent(old_node));
        new_node = pager_get_page(*table.pager, new_page);
        initialize_internal_node(new_node);
    }

    auto* old_nkeys = internal_node_num_keys(old_node);
    uint32_t cur_page = *internal_node_right_child(old_node);
    void* cur = pager_get_page(*table.pager, cur_page);

    internal_node_insert(table, new_page, cur_page);
    *node_parent(cur) = new_page;
    *internal_node_right_child(old_node) = INVALID_PAGE_NUM;

    for (int i = static_cast<int>(INTERNAL_NODE_MAX_KEYS) - 1;
         i > static_cast<int>(INTERNAL_NODE_MAX_KEYS / 2); --i) {
        cur_page = *internal_node_child(old_node, static_cast<uint32_t>(i));
        cur = pager_get_page(*table.pager, cur_page);
        internal_node_insert(table, new_page, cur_page);
        *node_parent(cur) = new_page;
        --*old_nkeys;
    }

    *internal_node_right_child(old_node) = *internal_node_child(old_node, *old_nkeys - 1);
    --*old_nkeys;

    uint32_t max_after = get_node_max_key(table, old_node);
    uint32_t dest = (child_max < max_after) ? old_page : new_page;

    internal_node_insert(table, dest, child_page_num);
    *node_parent(child) = dest;

    update_internal_node_key(parent, old_max, get_node_max_key(table, old_node));

    if (!splitting_root) {
        internal_node_insert(table, *node_parent(old_node), new_page);
        *node_parent(new_node) = *node_parent(old_node);
    }
}

void leaf_node_split_and_insert(Cursor& cursor, uint32_t key, const DynamicRow& value) {
    Table& table = *cursor.table;
    void* old_node = pager_get_page(*table.pager, cursor.page_num);
    uint32_t old_max = get_node_max_key(table, old_node);
    uint32_t new_page = get_unused_page_num(table);
    void* new_node = pager_get_page(*table.pager, new_page);

    initialize_leaf_node(new_node);
    *node_parent(new_node) = *node_parent(old_node);
    *leaf_node_next_leaf(new_node) = *leaf_node_next_leaf(old_node);
    *leaf_node_next_leaf(old_node) = new_page;

    uint32_t max_cells = leaf_node_max_cells(table.schema.row_size);
    uint32_t right_count = (max_cells + 1) / 2;
    uint32_t left_count = (max_cells + 1) - right_count;
    uint32_t cell_sz = leaf_node_cell_size(table.schema.row_size);

    for (int32_t i = static_cast<int32_t>(max_cells); i >= 0; --i) {
        void* dest_node = (static_cast<uint32_t>(i) >= left_count) ? new_node : old_node;
        uint32_t idx = static_cast<uint32_t>(i) % left_count;
        void* dest = leaf_node_cell(dest_node, idx, table.schema.row_size);

        if (static_cast<uint32_t>(i) == cursor.cell_num) {
            serialize_row(value, leaf_node_value(dest_node, idx, table.schema.row_size), table.schema);
            *leaf_node_key(dest_node, idx, table.schema.row_size) = key;
        } else if (static_cast<uint32_t>(i) > cursor.cell_num) {
            std::memcpy(dest, leaf_node_cell(old_node, static_cast<uint32_t>(i - 1), table.schema.row_size), cell_sz);
        } else {
            std::memcpy(dest, leaf_node_cell(old_node, static_cast<uint32_t>(i), table.schema.row_size), cell_sz);
        }
    }

    *leaf_node_num_cells(old_node) = left_count;
    *leaf_node_num_cells(new_node) = right_count;

    if (is_node_root(old_node)) {
        create_new_root(table, new_page);
    } else {
        uint32_t parent_page = *node_parent(old_node);
        uint32_t new_max = get_node_max_key(table, old_node);
        void* parent = pager_get_page(*table.pager, parent_page);
        update_internal_node_key(parent, old_max, new_max);
        internal_node_insert(table, parent_page, new_page);
    }
}

void leaf_node_insert(Cursor& cursor, uint32_t key, const DynamicRow& value) {
    Table& table = *cursor.table;
    void* node = pager_get_page(*table.pager, cursor.page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);
    uint32_t max_cells = leaf_node_max_cells(table.schema.row_size);

    if (num_cells >= max_cells) {
        leaf_node_split_and_insert(cursor, key, value);
        return;
    }

    uint32_t cell_sz = leaf_node_cell_size(table.schema.row_size);
    if (cursor.cell_num < num_cells) {
        for (uint32_t i = num_cells; i > cursor.cell_num; --i) {
            std::memcpy(leaf_node_cell(node, i, table.schema.row_size),
                        leaf_node_cell(node, i - 1, table.schema.row_size),
                        cell_sz);
        }
    }

    ++*leaf_node_num_cells(node);
    *leaf_node_key(node, cursor.cell_num, table.schema.row_size) = key;
    serialize_row(value, leaf_node_value(node, cursor.cell_num, table.schema.row_size), table.schema);
}

void print_tree(Table& table, uint32_t page_num, uint32_t indent) {
    void* node = pager_get_page(*table.pager, page_num);

    auto print_indent = [&](uint32_t n) {
        for (uint32_t i = 0; i < n; ++i) std::cout << "  ";
    };

    print_indent(indent);
    if (get_node_type(node) == NodeType::Leaf) {
        uint32_t n = *leaf_node_num_cells(node);
        std::cout << "- leaf (size " << n << ")\n";
        for (uint32_t i = 0; i < n; ++i) {
            print_indent(indent + 1);
            std::cout << "- " << *leaf_node_key(node, i, table.schema.row_size) << '\n';
        }
    } else {
        uint32_t n = *internal_node_num_keys(node);
        std::cout << "- internal (size " << n << ")\n";
        if (n > 0) {
            for (uint32_t i = 0; i < n; ++i) {
                print_tree(table, *internal_node_child(node, i), indent + 1);
                print_indent(indent + 1);
                std::cout << "- key " << *internal_node_key(node, i) << '\n';
            }
            print_tree(table, *internal_node_right_child(node), indent + 1);
        }
    }
}

// ============================================================================
// Lexer / Parser
// ============================================================================

enum class TokenKind { Identifier, StringLiteral, Number, Symbol, End };

struct Token {
    TokenKind kind = TokenKind::End;
    std::string text;
};

struct TokenList {
    std::vector<Token> tokens;
    size_t cursor = 0;
};

void tokenize_input(std::string_view input, TokenList& list) {
    list.tokens.clear();
    list.cursor = 0;
    size_t pos = 0;
    const size_t len = input.size();

    auto is_space = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    };
    auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
    auto is_alpha = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    };
    auto is_alnum = [&](char c) { return is_alpha(c) || is_digit(c); };

    while (pos < len && list.tokens.size() < MAX_TOKENS - 1) {
        if (is_space(input[pos])) { ++pos; continue; }

        if (input[pos] == ';') {
            list.tokens.push_back({TokenKind::Symbol, ";"});
            ++pos;
            continue;
        }

        if (input[pos] == '\'') {
            ++pos;
            size_t start = pos;
            while (pos < len && input[pos] != '\'') ++pos;
            list.tokens.push_back({TokenKind::StringLiteral,
                                   std::string(input.substr(start, pos - start))});
            if (pos < len) ++pos;
            continue;
        }

        if (pos + 1 < len) {
            auto two = input.substr(pos, 2);
            if (two == "<=" || two == ">=" || two == "!=" || two == "<>") {
                list.tokens.push_back({TokenKind::Symbol, std::string(two)});
                pos += 2;
                continue;
            }
        }

        if (std::string_view("=<>(),*").find(input[pos]) != std::string_view::npos) {
            list.tokens.push_back({TokenKind::Symbol, std::string(1, input[pos])});
            ++pos;
            continue;
        }

        if (is_digit(input[pos])) {
            size_t start = pos;
            while (pos < len && is_digit(input[pos])) ++pos;
            list.tokens.push_back({TokenKind::Number, std::string(input.substr(start, pos - start))});
            continue;
        }

        if (is_alpha(input[pos]) || input[pos] == '_' || input[pos] == '.' || input[pos] == '\\') {
            size_t start = pos;
            while (pos < len && (is_alnum(input[pos]) || input[pos] == '_' ||
                                 input[pos] == '.' || input[pos] == '\\'))
                ++pos;
            list.tokens.push_back({TokenKind::Identifier, std::string(input.substr(start, pos - start))});
            continue;
        }
        ++pos;
    }
    list.tokens.push_back({TokenKind::End, {}});
}

Token& peek_token(TokenList& list) { return list.tokens[list.cursor]; }
std::string_view peek_token_str(TokenList& list) { return list.tokens[list.cursor].text; }

Token& advance_token(TokenList& list) {
    return list.tokens[list.cursor++];
}

bool match_token(TokenList& list, std::string_view text) {
    if (iequals(peek_token_str(list), text)) {
        ++list.cursor;
        return true;
    }
    return false;
}

PrepareResult parse_comparison(TokenList& list, const Schema& schema, std::unique_ptr<Expr>& out) {
    auto& col = advance_token(list);
    auto& op  = advance_token(list);
    auto& val = advance_token(list);

    if (col.kind != TokenKind::Identifier) return PrepareResult::SyntaxError;

    int col_idx = schema_find_column(schema, col.text);
    if (col_idx < 0) return PrepareResult::SyntaxError;

    auto cmp = std::make_unique<Expr>();
    cmp->type = ExprType::Comparison;
    write_fixed(cmp->column, read_fixed(schema.columns[col_idx].name));

    if (op.text == "=")           cmp->op = WhereOp::Eq;
    else if (op.text == "!=" || op.text == "<>") cmp->op = WhereOp::Neq;
    else if (op.text == ">=")     cmp->op = WhereOp::Ge;
    else if (op.text == "<=")     cmp->op = WhereOp::Le;
    else if (op.text == ">")      cmp->op = WhereOp::Gt;
    else if (op.text == "<")      cmp->op = WhereOp::Lt;
    else return PrepareResult::SyntaxError;

    if (schema.columns[col_idx].type == DataType::INT) {
        cmp->value.is_string = false;
        try {
            long v = std::stol(val.text);
            if (schema.columns[col_idx].is_primary_key && v < 0)
                return PrepareResult::NegativeId;
            cmp->value.int_value = static_cast<uint32_t>(v);
        } catch (...) {
            return PrepareResult::SyntaxError;
        }
    } else {
        cmp->value.is_string = true;
        write_fixed(cmp->value.str_value, val.text);
    }

    out = std::move(cmp);
    return PrepareResult::Success;
}

PrepareResult parse_expr(TokenList& list, const Schema& schema, std::unique_ptr<Expr>& out);

PrepareResult parse_primary(TokenList& list, const Schema& schema, std::unique_ptr<Expr>& out) {
    if (peek_token(list).kind == TokenKind::Symbol && peek_token_str(list) == "(") {
        advance_token(list);
        auto res = parse_expr(list, schema, out);
        if (res != PrepareResult::Success) return res;
        if (peek_token(list).kind != TokenKind::Symbol || peek_token_str(list) != ")") {
            out.reset();
            return PrepareResult::SyntaxError;
        }
        advance_token(list);
        return PrepareResult::Success;
    }
    return parse_comparison(list, schema, out);
}

PrepareResult parse_and(TokenList& list, const Schema& schema, std::unique_ptr<Expr>& out) {
    std::unique_ptr<Expr> left;
    auto res = parse_primary(list, schema, left);
    if (res != PrepareResult::Success) return res;

    while (match_token(list, "AND")) {
        std::unique_ptr<Expr> right;
        res = parse_primary(list, schema, right);
        if (res != PrepareResult::Success) return res;

        auto parent = std::make_unique<Expr>();
        parent->type = ExprType::Logical;
        parent->log_op = LogicalOp::And;
        parent->left = std::move(left);
        parent->right = std::move(right);
        left = std::move(parent);
    }
    out = std::move(left);
    return PrepareResult::Success;
}

PrepareResult parse_or(TokenList& list, const Schema& schema, std::unique_ptr<Expr>& out) {
    std::unique_ptr<Expr> left;
    auto res = parse_and(list, schema, left);
    if (res != PrepareResult::Success) return res;

    while (match_token(list, "OR")) {
        std::unique_ptr<Expr> right;
        res = parse_and(list, schema, right);
        if (res != PrepareResult::Success) return res;

        auto parent = std::make_unique<Expr>();
        parent->type = ExprType::Logical;
        parent->log_op = LogicalOp::Or;
        parent->left = std::move(left);
        parent->right = std::move(right);
        left = std::move(parent);
    }
    out = std::move(left);
    return PrepareResult::Success;
}

PrepareResult parse_expr(TokenList& list, const Schema& schema, std::unique_ptr<Expr>& out) {
    return parse_or(list, schema, out);
}

PrepareResult parse_where_clause(TokenList& list, const Schema& schema, Statement& stmt) {
    if (!match_token(list, "WHERE")) return PrepareResult::Success;
    return parse_expr(list, schema, stmt.where_expr);
}

PrepareResult prepare_statement(TokenList& list, const Schema& schema, Statement& stmt) {
    stmt = Statement{};                     // reset
    if (list.tokens.empty() || peek_token(list).kind == TokenKind::End)
        return PrepareResult::SyntaxError;

    auto first = peek_token_str(list);

    // CREATE TABLE
    if (iequals(first, "create")) {
        advance_token(list);
        if (!match_token(list, "table")) return PrepareResult::SyntaxError;
        auto& tbl = advance_token(list);
        stmt.type = StatementType::Create;
        write_fixed(stmt.table_name, tbl.text);

        if (peek_token_str(list) != "(") return PrepareResult::SyntaxError;
        advance_token(list);

        write_fixed(stmt.created_schema.table_name, tbl.text);

        while (list.cursor < list.tokens.size() && peek_token_str(list) != ")") {
            auto& col_name = advance_token(list);
            if (col_name.kind != TokenKind::Identifier) return PrepareResult::SyntaxError;

            auto& col_type = advance_token(list);
            DataType dt = DataType::INT;
            uint32_t len = 0;

            if (iequals(col_type.text, "int") || iequals(col_type.text, "integer")) {
                dt = DataType::INT;
            } else if (iequals(col_type.text, "varchar") || iequals(col_type.text, "string") ||
                       iequals(col_type.text, "char")) {
                dt = DataType::VARCHAR;
                len = 32;
                if (peek_token_str(list) == "(") {
                    advance_token(list);
                    auto& len_tok = advance_token(list);
                    try { len = static_cast<uint32_t>(std::stoul(len_tok.text)); }
                    catch (...) {}
                    if (peek_token_str(list) == ")") advance_token(list);
                }
            } else {
                return PrepareResult::SyntaxError;
            }

            bool is_pk = false;
            if (iequals(peek_token_str(list), "primary")) {
                advance_token(list);
                if (iequals(peek_token_str(list), "key")) advance_token(list);
                is_pk = true;
            }

            schema_add_column(stmt.created_schema, col_name.text, dt, len, is_pk);
            if (peek_token_str(list) == ",") advance_token(list);
        }
        if (peek_token_str(list) == ")") advance_token(list);
        return PrepareResult::Success;
    }

    if (iequals(first, "begin") || iequals(first, "start")) {
        advance_token(list);
        if (iequals(first, "start")) match_token(list, "transaction");
        stmt.type = StatementType::Begin;
        return PrepareResult::Success;
    }
    if (iequals(first, "commit")) {
        advance_token(list);
        stmt.type = StatementType::Commit;
        return PrepareResult::Success;
    }
    if (iequals(first, "rollback")) {
        advance_token(list);
        stmt.type = StatementType::Rollback;
        return PrepareResult::Success;
    }

    // INSERT
    if (iequals(first, "insert")) {
        advance_token(list);
        match_token(list, "into");
        advance_token(list); // table name

        if (peek_token_str(list) == "(") {
            while (list.cursor < list.tokens.size() && peek_token_str(list) != ")")
                advance_token(list);
            if (peek_token_str(list) == ")") advance_token(list);
        }
        if (!match_token(list, "values")) return PrepareResult::SyntaxError;
        if (peek_token_str(list) == "(") advance_token(list);

        stmt.type = StatementType::Insert;
        stmt.row_to_insert.num_values = schema.num_columns;

        for (uint32_t i = 0; i < schema.num_columns; ++i) {
            auto& val_tok = advance_token(list);
            if (peek_token_str(list) == ",") advance_token(list);

            auto& v = stmt.row_to_insert.values[i];
            v.type = schema.columns[i].type;

            if (schema.columns[i].type == DataType::INT) {
                try {
                    long vlong = std::stol(val_tok.text);
                    if (schema.columns[i].is_primary_key && vlong < 0)
                        return PrepareResult::NegativeId;
                    v.int_val = static_cast<int32_t>(vlong);
                } catch (...) {
                    return PrepareResult::SyntaxError;
                }
            } else {
                if (val_tok.text.size() > schema.columns[i].length)
                    return PrepareResult::StringTooLong;
                write_fixed(v.str_val, val_tok.text);
            }
        }
        if (peek_token_str(list) == ")") advance_token(list);
        return PrepareResult::Success;
    }

    // SELECT
    if (iequals(first, "select")) {
        advance_token(list);
        stmt.type = StatementType::Select;

        if (iequals(peek_token_str(list), "count")) {
            advance_token(list);
            if (peek_token_str(list) == "(") advance_token(list);
            if (peek_token_str(list) == "*") advance_token(list);
            if (peek_token_str(list) == ")") advance_token(list);
            stmt.is_count = true;
        } else if (peek_token_str(list) == "*") {
            advance_token(list);
        }

        if (match_token(list, "from")) advance_token(list);
        return parse_where_clause(list, schema, stmt);
    }

    // UPDATE
    if (iequals(first, "update")) {
        advance_token(list);
        stmt.type = StatementType::Update;
        stmt.is_set_update = true;
        advance_token(list); // table
        match_token(list, "set");

        while (list.cursor < list.tokens.size() &&
               !iequals(peek_token_str(list), "where") &&
               peek_token_str(list) != ";") {
            if (peek_token_str(list) == ",") {
                advance_token(list);
                continue;
            }
            auto& col = advance_token(list);
            if (!match_token(list, "=")) return PrepareResult::SyntaxError;
            auto& val = advance_token(list);

            if (stmt.num_update_assignments < MAX_ASSIGNMENTS) {
                auto& a = stmt.update_assignments[stmt.num_update_assignments++];
                write_fixed(a.column_name, col.text);
                write_fixed(a.value_text, val.text);
            }
        }
        return parse_where_clause(list, schema, stmt);
    }

    // DELETE
    if (iequals(first, "delete")) {
        advance_token(list);
        stmt.type = StatementType::Delete;
        if (match_token(list, "from")) advance_token(list);
        return parse_where_clause(list, schema, stmt);
    }

    return PrepareResult::UnrecognizedStatement;
}

// ============================================================================
// Execution
// ============================================================================

bool row_matches_where(const DynamicRow& row, const Statement& stmt, const Schema& schema) {
    return evaluate_expr(stmt.where_expr.get(), row, schema);
}

ExecuteResult execute_insert(Statement& stmt, Table& table) {
    if (table.pager->num_pages + INSERT_PAGE_SAFETY_MARGIN > TABLE_MAX_PAGES)
        return ExecuteResult::TableFull;

    uint32_t key = get_pk_value(stmt.row_to_insert, table.schema);
    auto cursor = table_find(table, key);

    void* node = pager_get_page(*table.pager, cursor->page_num);
    uint32_t ncells = *leaf_node_num_cells(node);
    if (cursor->cell_num < ncells &&
        *leaf_node_key(node, cursor->cell_num, table.schema.row_size) == key)
        return ExecuteResult::DuplicateKey;

    leaf_node_insert(*cursor, key, stmt.row_to_insert);
    return ExecuteResult::Success;
}

struct PendingUpdate {
    uint32_t old_key = 0;
    uint32_t new_key = 0;
    DynamicRow row;
};

ExecuteResult execute_update(Statement& stmt, Table& table) {
    std::vector<PendingUpdate> pending;
    auto cursor = table_start(table);
    DynamicRow row;

    while (!cursor->end_of_table) {
        deserialize_row(cursor_value(*cursor), row, table.schema);
        if (row_matches_where(row, stmt, table.schema)) {
            uint32_t old_key = get_pk_value(row, table.schema);

            for (uint32_t i = 0; i < stmt.num_update_assignments; ++i) {
                const auto& a = stmt.update_assignments[i];
                int idx = schema_find_column(table.schema, read_fixed(a.column_name));
                if (idx >= 0 && static_cast<uint32_t>(idx) < row.num_values) {
                    auto val_sv = read_fixed(a.value_text);
                    if (table.schema.columns[idx].type == DataType::INT) {
                        try { row.values[idx].int_val = static_cast<int32_t>(std::stol(std::string(val_sv))); }
                        catch (...) {}
                    } else {
                        write_fixed(row.values[idx].str_val, val_sv);
                    }
                }
            }
            pending.push_back({old_key, get_pk_value(row, table.schema), row});
        }
        cursor_advance(*cursor);
    }

    uint32_t applied = 0, skipped = 0;
    for (auto& u : pending) {
        if (u.new_key == u.old_key) {
            auto c = table_find(table, u.old_key);
            void* node = pager_get_page(*table.pager, c->page_num);
            if (c->cell_num < *leaf_node_num_cells(node) &&
                *leaf_node_key(node, c->cell_num, table.schema.row_size) == u.old_key) {
                serialize_row(u.row, cursor_value(*c), table.schema);
                ++applied;
            }
            continue;
        }

        auto dest = table_find(table, u.new_key);
        void* dnode = pager_get_page(*table.pager, dest->page_num);
        bool dup = dest->cell_num < *leaf_node_num_cells(dnode) &&
                   *leaf_node_key(dnode, dest->cell_num, table.schema.row_size) == u.new_key;
        if (dup) { ++skipped; continue; }

        auto oldc = table_find(table, u.old_key);
        void* onode = pager_get_page(*table.pager, oldc->page_num);
        if (oldc->cell_num < *leaf_node_num_cells(onode) &&
            *leaf_node_key(onode, oldc->cell_num, table.schema.row_size) == u.old_key) {
            leaf_node_delete(*oldc);
            auto ins = table_find(table, u.new_key);
            leaf_node_insert(*ins, u.new_key, u.row);
            ++applied;
        }
    }

    if (skipped)
        std::cout << "UPDATE " << applied << " (skipped " << skipped << " due to duplicate key)\n";
    else
        std::cout << "UPDATE " << applied << '\n';
    return ExecuteResult::Success;
}

ExecuteResult execute_delete(Statement& stmt, Table& table) {
    std::vector<uint32_t> keys;
    auto cursor = table_start(table);
    DynamicRow row;

    while (!cursor->end_of_table) {
        deserialize_row(cursor_value(*cursor), row, table.schema);
        if (row_matches_where(row, stmt, table.schema))
            keys.push_back(get_pk_value(row, table.schema));
        cursor_advance(*cursor);
    }

    for (uint32_t key : keys) {
        auto c = table_find(table, key);
        void* node = pager_get_page(*table.pager, c->page_num);
        if (c->cell_num < *leaf_node_num_cells(node) &&
            *leaf_node_key(node, c->cell_num, table.schema.row_size) == key)
            leaf_node_delete(*c);
    }
    std::cout << "DELETE " << keys.size() << '\n';
    return ExecuteResult::Success;
}

ExecuteResult execute_select(Statement& stmt, Table& table) {
    auto cursor = table_start(table);
    DynamicRow row;
    uint32_t count = 0;

    while (!cursor->end_of_table) {
        deserialize_row(cursor_value(*cursor), row, table.schema);
        if (row_matches_where(row, stmt, table.schema)) {
            ++count;
            if (!stmt.is_count) print_row(row, table.schema);
        }
        cursor_advance(*cursor);
    }
    if (stmt.is_count) std::cout << count << " row(s).\n";
    return ExecuteResult::Success;
}

ExecuteResult execute_begin(Table& table) {
    if (table.in_transaction) return ExecuteResult::TxAlreadyActive;
    table.in_transaction = true;
    table.tx_original_num_pages = table.pager->num_pages;

    for (size_t i = 0; i < TABLE_MAX_PAGES; ++i) {
        if (table.pager->pages[i]) {
            table.tx_backup[i] = std::make_unique<std::byte[]>(PAGE_SIZE);
            std::memcpy(table.tx_backup[i].get(), table.pager->pages[i].get(), PAGE_SIZE);
            table.tx_was_cached[i] = true;
        } else {
            table.tx_backup[i].reset();
            table.tx_was_cached[i] = false;
        }
    }
    return ExecuteResult::Success;
}

ExecuteResult execute_commit(Table& table) {
    if (!table.in_transaction) return ExecuteResult::NoActiveTx;
    tx_free_backups(table);
    for (uint32_t i = 0; i < table.pager->num_pages; ++i)
        if (table.pager->pages[i]) pager_flush(*table.pager, i);
    table.in_transaction = false;
    return ExecuteResult::Success;
}

ExecuteResult execute_rollback(Table& table) {
    if (!table.in_transaction) return ExecuteResult::NoActiveTx;

    for (size_t i = 0; i < TABLE_MAX_PAGES; ++i) {
        if (static_cast<uint32_t>(i) < table.tx_original_num_pages) {
            if (table.tx_was_cached[i]) {
                std::memcpy(table.pager->pages[i].get(), table.tx_backup[i].get(), PAGE_SIZE);
                table.tx_backup[i].reset();
            } else {
                table.pager->pages[i].reset();
            }
        } else {
            table.pager->pages[i].reset();
            table.tx_backup[i].reset();
        }
    }
    table.pager->num_pages = table.tx_original_num_pages;
    table.in_transaction = false;
    return ExecuteResult::Success;
}

ExecuteResult execute_statement(Statement& stmt, Table& table) {
    switch (stmt.type) {
        case StatementType::Insert:   return execute_insert(stmt, table);
        case StatementType::Select:   return execute_select(stmt, table);
        case StatementType::Update:   return execute_update(stmt, table);
        case StatementType::Delete:   return execute_delete(stmt, table);
        case StatementType::Create: {
            table.schema = stmt.created_schema;
            auto* meta = static_cast<MetaPage*>(pager_get_page(*table.pager, 0));
            meta->magic = SCHEMA_MAGIC;
            meta->root_page_num = table.root_page_num;
            meta->schema = table.schema;
            void* root = pager_get_page(*table.pager, table.root_page_num);
            initialize_leaf_node(root);
            set_node_root(root, true);
            std::cout << "CREATE TABLE (" << table.schema.num_columns << " columns configured)\n";
            return ExecuteResult::Success;
        }
        case StatementType::Begin:    return execute_begin(table);
        case StatementType::Commit:   return execute_commit(table);
        case StatementType::Rollback: return execute_rollback(table);
    }
    return ExecuteResult::Success;
}

// ============================================================================
// Shell
// ============================================================================

void print_help() {
    std::cout <<
        "SQL Commands:\n"
        "  CREATE TABLE <name> (<pk_col> INT PRIMARY KEY, <col2> VARCHAR(32), ...);\n"
        "  INSERT INTO <name> VALUES (...);\n"
        "  SELECT * | COUNT(*) FROM <name> [WHERE <expr>];\n"
        "  UPDATE <name> SET col = val WHERE <expr>;\n"
        "  DELETE FROM <name> WHERE <expr>;\n"
        "  BEGIN; COMMIT; ROLLBACK;\n"
        "Meta:\n"
        "  \\q / .exit      quit\n"
        "  \\d / .btree     print B+tree\n"
        "  \\c / .constants constants\n"
        "  \\? / .help      this help\n";
}

void print_constants(const Table& table) {
    std::cout << "ROW_SIZE: " << table.schema.row_size << '\n'
              << "COMMON_NODE_HEADER_SIZE: " << COMMON_NODE_HEADER_SIZE << '\n'
              << "LEAF_NODE_HEADER_SIZE: " << LEAF_NODE_HEADER_SIZE << '\n'
              << "LEAF_NODE_CELL_SIZE: " << leaf_node_cell_size(table.schema.row_size) << '\n'
              << "LEAF_NODE_MAX_CELLS: " << leaf_node_max_cells(table.schema.row_size) << '\n';
}

MetaCommandResult do_meta_command(std::string_view input, Table& table) {
    if (input == ".exit" || input == "\\q") {
        table_close(table);
        std::exit(0);
    }
    if (input == ".btree" || input == "\\d") {
        std::cout << "Tree:\n";
        print_tree(table, table.root_page_num, 0);
        return MetaCommandResult::Success;
    }
    if (input == ".constants" || input == "\\c") {
        std::cout << "Constants:\n";
        print_constants(table);
        return MetaCommandResult::Success;
    }
    if (input == ".help" || input == "\\?") {
        print_help();
        return MetaCommandResult::Success;
    }
    return MetaCommandResult::Unrecognized;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Must supply a database filename.\n";
        return 1;
    }

    auto table = table_open(argv[1]);

    std::string line;
    while (true) {
        std::cout << "db=# " << std::flush;
        if (!std::getline(std::cin, line)) break;

        // trim
        auto start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        auto end = line.find_last_not_of(" \t\r\n");
        line = line.substr(start, end - start + 1);
        if (line.empty()) continue;

        if (line[0] == '.' || line[0] == '\\') {
            if (do_meta_command(line, *table) == MetaCommandResult::Unrecognized)
                std::cout << "Unrecognized command '" << line << "'\n";
            continue;
        }

        TokenList tokens;
        tokenize_input(line, tokens);

        Statement stmt;
        auto prep = prepare_statement(tokens, table->schema, stmt);

        if (prep != PrepareResult::Success) {
            switch (prep) {
                case PrepareResult::NegativeId:       std::cout << "ID must be positive.\n"; break;
                case PrepareResult::StringTooLong:    std::cout << "String is too long for column budget.\n"; break;
                case PrepareResult::SyntaxError:      std::cout << "Syntax error. Could not parse statement.\n"; break;
                case PrepareResult::UnrecognizedStatement:
                    std::cout << "Unrecognized keyword at start of '" << line << "'.\n"; break;
                default: break;
            }
            continue;
        }

        if (!table->schema.has_schema &&
            (stmt.type == StatementType::Insert || stmt.type == StatementType::Select ||
             stmt.type == StatementType::Update || stmt.type == StatementType::Delete)) {
            std::cout << "Error: No table schema found. Please run CREATE TABLE first.\n";
            continue;
        }

        auto res = execute_statement(stmt, *table);

        switch (res) {
            case ExecuteResult::Success:
                if (stmt.type == StatementType::Insert)   std::cout << "INSERT 0 1\n";
                else if (stmt.type == StatementType::Begin)    std::cout << "BEGIN\n";
                else if (stmt.type == StatementType::Commit)   std::cout << "COMMIT\n";
                else if (stmt.type == StatementType::Rollback) std::cout << "ROLLBACK\n";
                break;
            case ExecuteResult::DuplicateKey:    std::cout << "Error: Duplicate key.\n"; break;
            case ExecuteResult::NotFound:        std::cout << "Error: row not found.\n"; break;
            case ExecuteResult::TxAlreadyActive: std::cout << "Error: a transaction is already active.\n"; break;
            case ExecuteResult::NoActiveTx:      std::cout << "Error: no active transaction.\n"; break;
            case ExecuteResult::TableFull:       std::cout << "Error: table is full (max " << TABLE_MAX_PAGES << " pages).\n"; break;
        }
    }

    table_close(*table);
    return 0;
}
