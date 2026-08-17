package main

import "core:fmt"
import "core:mem"
import "core:os"
import "core:strconv"
import "core:strings"

// ============================================================================
// Data Types & Schema Definition
// ============================================================================

PAGE_SIZE :: 4096
TABLE_MAX_PAGES :: 400
INVALID_PAGE_NUM :: u32(max(u32))
INSERT_PAGE_SAFETY_MARGIN :: 8
SCHEMA_MAGIC :: 0x5343484D

MAX_COLUMNS :: 32
MAX_NAME_LEN :: 64
MAX_STR_LEN :: 256
MAX_TOKENS :: 256
MAX_ASSIGNMENTS :: 32

Data_Type :: enum i32 {
    INT = 0,
    VARCHAR = 1,
}

Column_Def :: struct {
    name: [MAX_NAME_LEN]u8,
    type: Data_Type,
    length: u32, // For VARCHAR length; 0 for INT
    offset: u32, // Byte offset in row buffer
    size: u32,   // Size in row buffer
    is_primary_key: bool,
}

Schema :: struct {
    has_schema: bool,
    table_name: [MAX_NAME_LEN]u8,
    columns: [MAX_COLUMNS]Column_Def,
    num_columns: u32,
    row_size: u32,
    primary_key_index: u32,
}

copy_str_to_fixed :: proc(dst: []u8, src: string) {
    mem.zero_slice(dst)
    n := min(len(dst) - 1, len(src))
    if n > 0 {
        copy(dst[:n], src[:n])
    }
    dst[n] = 0
}

fixed_to_string :: proc(buf: []u8) -> string {
    for b, i in buf {
        if b == 0 {
            return string(buf[:i])
        }
    }
    return string(buf)
}

schema_add_column :: proc(schema: ^Schema, name: string, type: Data_Type, length: u32, is_pk: bool) {
    if schema.num_columns >= MAX_COLUMNS { return }

    col := &schema.columns[schema.num_columns]
    copy_str_to_fixed(col.name[:], name)
    col.type = type
    col.is_primary_key = is_pk
    col.offset = schema.row_size

    if type == .INT {
        col.length = 0
        col.size = size_of(i32)
    } else { // VARCHAR
        col.length = length > 0 ? length : 32
        col.size = col.length + 1 // Null-terminated string buffer
    }

    if is_pk {
        schema.primary_key_index = schema.num_columns
    }

    schema.row_size += col.size
    schema.num_columns += 1
    schema.has_schema = true
}

schema_find_column :: proc(schema: ^Schema, name: string) -> int {
    for i in 0..<schema.num_columns {
        col_name := fixed_to_string(schema.columns[i].name[:])
        if strings.equal_fold(col_name, name) {
            return int(i)
        }
    }
    return -1
}

// ============================================================================
// Dynamic Value & Row Structures
// ============================================================================

Value :: struct {
    type: Data_Type,
    int_val: i32,
    str_val: [MAX_STR_LEN]u8,
}

Dynamic_Row :: struct {
    values: [MAX_COLUMNS]Value,
    num_values: u32,
}

get_pk_value :: proc(row: ^Dynamic_Row, schema: ^Schema) -> u32 {
    if schema.primary_key_index < row.num_values {
        return u32(row.values[schema.primary_key_index].int_val)
    }
    return 0
}

Execute_Result :: enum {
    EXECUTE_SUCCESS,
    EXECUTE_DUPLICATE_KEY,
    EXECUTE_NOT_FOUND,
    EXECUTE_TX_ALREADY_ACTIVE,
    EXECUTE_NO_ACTIVE_TX,
    EXECUTE_TABLE_FULL,
}

Meta_Command_Result :: enum {
    META_COMMAND_SUCCESS,
    META_COMMAND_UNRECOGNIZED_COMMAND,
}

Prepare_Result :: enum {
    PREPARE_SUCCESS,
    PREPARE_NEGATIVE_ID,
    PREPARE_STRING_TOO_LONG,
    PREPARE_SYNTAX_ERROR,
    PREPARE_UNRECOGNIZED_STATEMENT,
}

Statement_Type :: enum {
    STATEMENT_INSERT,
    STATEMENT_SELECT,
    STATEMENT_UPDATE,
    STATEMENT_DELETE,
    STATEMENT_CREATE,
    STATEMENT_BEGIN,
    STATEMENT_COMMIT,
    STATEMENT_ROLLBACK,
}

Where_Op :: enum {
    WHERE_OP_EQ,
    WHERE_OP_NEQ,
    WHERE_OP_GT,
    WHERE_OP_LT,
    WHERE_OP_GE,
    WHERE_OP_LE,
}

Node_Type :: enum u8 {
    NODE_INTERNAL = 0,
    NODE_LEAF = 1,
}

// ============================================================================
// Serialization Utilities
// ============================================================================

serialize_row :: proc(source: ^Dynamic_Row, destination: rawptr, schema: ^Schema) {
    mem.zero(destination, int(schema.row_size))
    for i in 0..<schema.num_columns {
        col := &schema.columns[i]
        dest_ptr := node_ptr(destination, int(col.offset))
        if i < source.num_values {
            val := &source.values[i]
            if col.type == .INT {
                v := val.int_val
                mem.copy(dest_ptr, &v, size_of(i32))
            } else {
                str_val := fixed_to_string(val.str_val[:])
                str_len := min(u32(len(str_val)), col.length)
                mem.copy(dest_ptr, &val.str_val[0], int(str_len))
                dest_slice := mem.slice_ptr(dest_ptr, int(col.size))
                dest_slice[col.length] = 0
            }
        }
    }
}

deserialize_row :: proc(source: rawptr, destination: ^Dynamic_Row, schema: ^Schema) {
    destination.num_values = schema.num_columns
    for i in 0..<schema.num_columns {
        col := &schema.columns[i]
        src_ptr := node_ptr(source, int(col.offset))
        val := &destination.values[i]
        if col.type == .INT {
            v: i32
            mem.copy(&v, src_ptr, size_of(i32))
            val.type = .INT
            val.int_val = v
        } else {
            val.type = .VARCHAR
            mem.copy(&val.str_val[0], src_ptr, int(col.size))
            val.str_val[col.size - 1] = 0
        }
    }
}

print_row :: proc(row: ^Dynamic_Row, schema: ^Schema) {
    fmt.printf("(")
    for i in 0..<schema.num_columns {
        if i > 0 { fmt.printf(", ") }
        if schema.columns[i].type == .INT {
            fmt.printf("%d", row.values[i].int_val)
        } else {
            fmt.printf("'%s'", fixed_to_string(row.values[i].str_val[:]))
        }
    }
    fmt.printf(")\n")
}

// ============================================================================
// WHERE-clause Structures & Evaluation
// ============================================================================

Expr_Type :: enum {
    EXPR_COMPARISON,
    EXPR_LOGICAL,
}

Logical_Op :: enum {
    LOGICAL_AND,
    LOGICAL_OR,
}

Literal :: struct {
    is_string: bool,
    int_value: u32,
    str_value: [MAX_STR_LEN]u8,
}

Expr :: struct {
    type: Expr_Type,

    // EXPR_COMPARISON fields
    column: [MAX_NAME_LEN]u8,
    op: Where_Op,
    value: Literal,

    // EXPR_LOGICAL fields
    log_op: Logical_Op,
    left: ^Expr,
    right: ^Expr,
}

free_expr :: proc(expr: ^Expr) {
    if expr == nil { return }
    if expr.type == .EXPR_LOGICAL {
        free_expr(expr.left)
        free_expr(expr.right)
    }
    free(expr)
}

evaluate_expr :: proc(expr: ^Expr, row: ^Dynamic_Row, schema: ^Schema) -> bool {
    if expr == nil { return true }

    if expr.type == .EXPR_LOGICAL {
        if expr.log_op == .LOGICAL_AND {
            return evaluate_expr(expr.left, row, schema) && evaluate_expr(expr.right, row, schema)
        } else if expr.log_op == .LOGICAL_OR {
            return evaluate_expr(expr.left, row, schema) || evaluate_expr(expr.right, row, schema)
        }
        return false
    }

    col_idx := schema_find_column(schema, fixed_to_string(expr.column[:]))
    if col_idx < 0 || u32(col_idx) >= row.num_values { return false }

    col_def := &schema.columns[col_idx]
    cell_val := &row.values[col_idx]

    cmp: int = 0
    if col_def.type == .INT {
        v := i32(expr.value.int_value)
        cmp = (cell_val.int_val > v ? 1 : 0) - (cell_val.int_val < v ? 1 : 0)
    } else {
        s1 := fixed_to_string(cell_val.str_val[:])
        s2 := fixed_to_string(expr.value.str_value[:])
        c := strings.compare(s1, s2)
        cmp = (c > 0 ? 1 : 0) - (c < 0 ? 1 : 0)
    }

    #partial switch expr.op {
    case .WHERE_OP_EQ:  return cmp == 0
    case .WHERE_OP_NEQ: return cmp != 0
    case .WHERE_OP_GT:  return cmp > 0
    case .WHERE_OP_LT:  return cmp < 0
    case .WHERE_OP_GE:  return cmp >= 0
    case .WHERE_OP_LE:  return cmp <= 0
    }
    return false
}

Update_Assignment :: struct {
    column_name: [MAX_NAME_LEN]u8,
    value_text: [MAX_STR_LEN]u8,
}

Statement :: struct {
    type: Statement_Type,
    row_to_insert: Dynamic_Row,
    table_name: [MAX_NAME_LEN]u8,
    created_schema: Schema,

    where_expr: ^Expr,
    is_count: bool,
    target_id: u32,

    // SET-style UPDATE
    is_set_update: bool,
    update_assignments: [MAX_ASSIGNMENTS]Update_Assignment,
    num_update_assignments: u32,
}

// ============================================================================
// B+Tree Layout Helpers
// ============================================================================

NODE_TYPE_SIZE :: size_of(u8)
NODE_TYPE_OFFSET :: 0
IS_ROOT_SIZE :: size_of(u8)
IS_ROOT_OFFSET :: NODE_TYPE_OFFSET + NODE_TYPE_SIZE
PARENT_POINTER_SIZE :: size_of(u32)
PARENT_POINTER_OFFSET :: IS_ROOT_OFFSET + IS_ROOT_SIZE
COMMON_NODE_HEADER_SIZE :: NODE_TYPE_SIZE + IS_ROOT_SIZE + PARENT_POINTER_SIZE

INTERNAL_NODE_NUM_KEYS_SIZE :: size_of(u32)
INTERNAL_NODE_NUM_KEYS_OFFSET :: COMMON_NODE_HEADER_SIZE
INTERNAL_NODE_RIGHT_CHILD_SIZE :: size_of(u32)
INTERNAL_NODE_RIGHT_CHILD_OFFSET :: INTERNAL_NODE_NUM_KEYS_OFFSET + INTERNAL_NODE_NUM_KEYS_SIZE
INTERNAL_NODE_HEADER_SIZE :: COMMON_NODE_HEADER_SIZE + INTERNAL_NODE_NUM_KEYS_SIZE + INTERNAL_NODE_RIGHT_CHILD_SIZE

INTERNAL_NODE_KEY_SIZE :: size_of(u32)
INTERNAL_NODE_CHILD_SIZE :: size_of(u32)
INTERNAL_NODE_CELL_SIZE :: INTERNAL_NODE_CHILD_SIZE + INTERNAL_NODE_KEY_SIZE
INTERNAL_NODE_MAX_KEYS :: 3

LEAF_NODE_NUM_CELLS_SIZE :: size_of(u32)
LEAF_NODE_NUM_CELLS_OFFSET :: COMMON_NODE_HEADER_SIZE
LEAF_NODE_NEXT_LEAF_SIZE :: size_of(u32)
LEAF_NODE_NEXT_LEAF_OFFSET :: LEAF_NODE_NUM_CELLS_OFFSET + LEAF_NODE_NUM_CELLS_SIZE
LEAF_NODE_HEADER_SIZE :: COMMON_NODE_HEADER_SIZE + LEAF_NODE_NUM_CELLS_SIZE + LEAF_NODE_NEXT_LEAF_SIZE
LEAF_NODE_KEY_SIZE :: size_of(u32)

node_ptr :: proc(node: rawptr, offset: int) -> ^u8 {
    return &(cast([^]u8)node)[offset]
}

get_node_type :: proc(node: rawptr) -> Node_Type {
    return cast(Node_Type)node_ptr(node, NODE_TYPE_OFFSET)^
}

set_node_type :: proc(node: rawptr, type: Node_Type) {
    node_ptr(node, NODE_TYPE_OFFSET)^ = u8(type)
}

is_node_root :: proc(node: rawptr) -> bool {
    return node_ptr(node, IS_ROOT_OFFSET)^ != 0
}

set_node_root :: proc(node: rawptr, is_root: bool) {
    node_ptr(node, IS_ROOT_OFFSET)^ = u8(is_root ? 1 : 0)
}

node_parent :: proc(node: rawptr) -> ^u32 {
    return cast(^u32)node_ptr(node, PARENT_POINTER_OFFSET)
}

internal_node_num_keys :: proc(node: rawptr) -> ^u32 {
    return cast(^u32)node_ptr(node, INTERNAL_NODE_NUM_KEYS_OFFSET)
}

internal_node_right_child :: proc(node: rawptr) -> ^u32 {
    return cast(^u32)node_ptr(node, INTERNAL_NODE_RIGHT_CHILD_OFFSET)
}

internal_node_cell :: proc(node: rawptr, cell_num: u32) -> ^u32 {
    return cast(^u32)node_ptr(node, INTERNAL_NODE_HEADER_SIZE + int(cell_num) * INTERNAL_NODE_CELL_SIZE)
}

internal_node_child :: proc(node: rawptr, child_num: u32) -> ^u32 {
    num_keys := internal_node_num_keys(node)^
    if child_num > num_keys {
        fmt.printf("Tried to access child_num %d > num_keys %d\n", child_num, num_keys)
        os.exit(1)
    } else if child_num == num_keys {
        right_child := internal_node_right_child(node)
        if right_child^ == INVALID_PAGE_NUM {
            fmt.printf("Tried to access right child of node, but was invalid page\n")
            os.exit(1)
        }
        return right_child
    } else {
        child := internal_node_cell(node, child_num)
        if child^ == INVALID_PAGE_NUM {
            fmt.printf("Tried to access child %d of node, but was invalid page\n", child_num)
            os.exit(1)
        }
        return child
    }
}

internal_node_key :: proc(node: rawptr, key_num: u32) -> ^u32 {
    cell_ptr := internal_node_cell(node, key_num)
    return cast(^u32)node_ptr(cell_ptr, INTERNAL_NODE_CHILD_SIZE)
}

leaf_node_cell_size :: proc(row_size: u32) -> u32 {
    return LEAF_NODE_KEY_SIZE + row_size
}

leaf_node_max_cells :: proc(row_size: u32) -> u32 {
    if row_size == 0 { return 0 }
    space := u32(PAGE_SIZE - LEAF_NODE_HEADER_SIZE)
    return space / leaf_node_cell_size(row_size)
}

leaf_node_num_cells :: proc(node: rawptr) -> ^u32 {
    return cast(^u32)node_ptr(node, LEAF_NODE_NUM_CELLS_OFFSET)
}

leaf_node_next_leaf :: proc(node: rawptr) -> ^u32 {
    return cast(^u32)node_ptr(node, LEAF_NODE_NEXT_LEAF_OFFSET)
}

leaf_node_cell :: proc(node: rawptr, cell_num: u32, row_size: u32) -> rawptr {
    return node_ptr(node, LEAF_NODE_HEADER_SIZE + int(cell_num) * int(leaf_node_cell_size(row_size)))
}

leaf_node_key :: proc(node: rawptr, cell_num: u32, row_size: u32) -> ^u32 {
    return cast(^u32)leaf_node_cell(node, cell_num, row_size)
}

leaf_node_value :: proc(node: rawptr, cell_num: u32, row_size: u32) -> rawptr {
    cell_ptr := leaf_node_cell(node, cell_num, row_size)
    return node_ptr(cell_ptr, int(LEAF_NODE_KEY_SIZE))
}

// ============================================================================
// Pager Structure & Functions
// ============================================================================

Pager :: struct {
    file_descriptor: ^os.File,
    file_length: u32,
    num_pages: u32,
    pages: [TABLE_MAX_PAGES]rawptr,
}

pager_open :: proc(filename: string) -> ^Pager {
    fd, err := os.open(filename, os.O_RDWR | os.O_CREATE)
    if err != nil {
        fmt.printf("Unable to open file\n")
        os.exit(1)
    }

    fsize, ferr := os.file_size(fd)
    if ferr != nil {
        fmt.printf("Error obtaining file stats\n")
        os.exit(1)
    }

    pager := new(Pager)
    pager.file_descriptor = fd
    pager.file_length = u32(fsize)
    pager.num_pages = pager.file_length / PAGE_SIZE

    if pager.file_length % PAGE_SIZE != 0 {
        fmt.printf("Db file is not a whole number of pages. Corrupt file.\n")
        os.exit(1)
    }

    for i in 0..<TABLE_MAX_PAGES {
        pager.pages[i] = nil
    }

    return pager
}

pager_get_page :: proc(pager: ^Pager, page_num: u32) -> rawptr {
    if page_num >= TABLE_MAX_PAGES {
        fmt.printf("Tried to fetch page number out of bounds. %d >= %d\n", page_num, TABLE_MAX_PAGES)
        os.exit(1)
    }

    if pager.pages[page_num] == nil {
        page, _ := mem.alloc(PAGE_SIZE)
        mem.zero(page, PAGE_SIZE)

        offset := i64(page_num) * PAGE_SIZE
        if u32(offset) < pager.file_length {
            slice := mem.slice_ptr(cast(^u8)page, PAGE_SIZE)
            _, err := os.read_at(pager.file_descriptor, slice, offset)
            if err != nil {
                fmt.printf("Error reading file\n")
                os.exit(1)
            }
        }
        pager.pages[page_num] = page
        if page_num >= pager.num_pages {
            pager.num_pages = page_num + 1
        }
    }
    return pager.pages[page_num]
}

pager_flush :: proc(pager: ^Pager, page_num: u32) {
    if pager.pages[page_num] == nil {
        fmt.printf("Tried to flush null page\n")
        os.exit(1)
    }

    offset := i64(page_num) * PAGE_SIZE
    slice := mem.slice_ptr(cast(^u8)pager.pages[page_num], PAGE_SIZE)
    _, err := os.write_at(pager.file_descriptor, slice, offset)
    if err != nil {
        fmt.printf("Error writing to file\n")
        os.exit(1)
    }
}

pager_close :: proc(pager: ^Pager) {
    for i in 0..<TABLE_MAX_PAGES {
        if pager.pages[i] != nil {
            free(pager.pages[i])
            pager.pages[i] = nil
        }
    }
    os.close(pager.file_descriptor)
    free(pager)
}

// ============================================================================
// Table & Cursor Structures & B+Tree Operations
// ============================================================================

Table :: struct {
    pager: ^Pager,
    root_page_num: u32,
    schema: Schema,

    // Transaction state
    in_transaction: bool,
    tx_original_num_pages: u32,
    tx_backup: [TABLE_MAX_PAGES]rawptr,
    tx_was_cached: [TABLE_MAX_PAGES]bool,
}

Cursor :: struct {
    table: ^Table,
    page_num: u32,
    cell_num: u32,
    end_of_table: bool,
}

initialize_leaf_node :: proc(node: rawptr) {
    set_node_type(node, .NODE_LEAF)
    set_node_root(node, false)
    leaf_node_num_cells(node)^ = 0
    leaf_node_next_leaf(node)^ = 0
}

initialize_internal_node :: proc(node: rawptr) {
    set_node_type(node, .NODE_INTERNAL)
    set_node_root(node, false)
    internal_node_num_keys(node)^ = 0
    internal_node_right_child(node)^ = INVALID_PAGE_NUM
}

tx_free_backups :: proc(table: ^Table) {
    for i in 0..<TABLE_MAX_PAGES {
        if table.tx_backup[i] != nil {
            free(table.tx_backup[i])
            table.tx_backup[i] = nil
        }
    }
}

Meta_Page :: struct {
    magic: u32,
    root_page_num: u32,
    schema: Schema,
}

table_open :: proc(filename: string) -> ^Table {
    table := new(Table)
    table.pager = pager_open(filename)
    table.in_transaction = false
    table.tx_original_num_pages = 0

    for i in 0..<TABLE_MAX_PAGES {
        table.tx_backup[i] = nil
        table.tx_was_cached[i] = false
    }

    if table.pager.num_pages == 0 {
        table.root_page_num = 1
        table.schema.has_schema = false

        meta := cast(^Meta_Page)pager_get_page(table.pager, 0)
        meta.magic = SCHEMA_MAGIC
        meta.root_page_num = table.root_page_num
        meta.schema = table.schema

        root_node := pager_get_page(table.pager, table.root_page_num)
        initialize_leaf_node(root_node)
        set_node_root(root_node, true)
    } else {
        meta := cast(^Meta_Page)pager_get_page(table.pager, 0)
        if meta.magic == SCHEMA_MAGIC {
            table.root_page_num = meta.root_page_num
            table.schema = meta.schema
        } else {
            table.root_page_num = 1
            table.schema.has_schema = false
        }
    }
    return table
}

table_close :: proc(table: ^Table) {
    tx_free_backups(table)
    for i in 0..<table.pager.num_pages {
        if table.pager.pages[i] == nil { continue }
        pager_flush(table.pager, i)
        free(table.pager.pages[i])
        table.pager.pages[i] = nil
    }
    pager_close(table.pager)
    free(table)
}

get_node_max_key :: proc(table: ^Table, node: rawptr) -> u32 {
    if get_node_type(node) == .NODE_LEAF {
        return leaf_node_key(node, leaf_node_num_cells(node)^ - 1, table.schema.row_size)^
    }
    right_child := pager_get_page(table.pager, internal_node_right_child(node)^)
    return get_node_max_key(table, right_child)
}

leaf_node_find :: proc(table: ^Table, page_num: u32, key: u32) -> ^Cursor {
    node := pager_get_page(table.pager, page_num)
    num_cells := leaf_node_num_cells(node)^

    cursor := new(Cursor)
    cursor.table = table
    cursor.page_num = page_num
    cursor.end_of_table = false

    min_index: u32 = 0
    one_past_max_index: u32 = num_cells
    for one_past_max_index != min_index {
        index := (min_index + one_past_max_index) / 2
        key_at_index := leaf_node_key(node, index, table.schema.row_size)^
        if key == key_at_index {
            cursor.cell_num = index
            return cursor
        }
        if key < key_at_index {
            one_past_max_index = index
        } else {
            min_index = index + 1
        }
    }
    cursor.cell_num = min_index
    return cursor
}

internal_node_find_child :: proc(node: rawptr, key: u32) -> u32 {
    num_keys := internal_node_num_keys(node)^
    min_index: u32 = 0
    max_index: u32 = num_keys
    for min_index != max_index {
        index := (min_index + max_index) / 2
        key_to_right := internal_node_key(node, index)^
        if key_to_right >= key {
            max_index = index
        } else {
            min_index = index + 1
        }
    }
    return min_index
}

internal_node_find :: proc(table: ^Table, page_num: u32, key: u32) -> ^Cursor {
    node := pager_get_page(table.pager, page_num)
    child_index := internal_node_find_child(node, key)
    child_num := internal_node_child(node, child_index)^
    child := pager_get_page(table.pager, child_num)
    #partial switch get_node_type(child) {
    case .NODE_LEAF:
        return leaf_node_find(table, child_num, key)
    case .NODE_INTERNAL:
        return internal_node_find(table, child_num, key)
    }
    return nil
}

table_find :: proc(table: ^Table, key: u32) -> ^Cursor {
    root_node := pager_get_page(table.pager, table.root_page_num)
    if get_node_type(root_node) == .NODE_LEAF {
        return leaf_node_find(table, table.root_page_num, key)
    } else {
        return internal_node_find(table, table.root_page_num, key)
    }
}

table_start :: proc(table: ^Table) -> ^Cursor {
    cursor := table_find(table, 0)
    node := pager_get_page(table.pager, cursor.page_num)
    num_cells := leaf_node_num_cells(node)^
    cursor.end_of_table = (num_cells == 0)
    return cursor
}

cursor_value :: proc(cursor: ^Cursor) -> rawptr {
    page := pager_get_page(cursor.table.pager, cursor.page_num)
    return leaf_node_value(page, cursor.cell_num, cursor.table.schema.row_size)
}

cursor_advance :: proc(cursor: ^Cursor) {
    node := pager_get_page(cursor.table.pager, cursor.page_num)
    cursor.cell_num += 1
    if cursor.cell_num >= leaf_node_num_cells(node)^ {
        next_page_num := leaf_node_next_leaf(node)^
        if next_page_num == 0 {
            cursor.end_of_table = true
        } else {
            cursor.page_num = next_page_num
            cursor.cell_num = 0
        }
    }
}

leaf_node_delete :: proc(cursor: ^Cursor) {
    node := pager_get_page(cursor.table.pager, cursor.page_num)
    num_cells := leaf_node_num_cells(node)^
    cell_sz := leaf_node_cell_size(cursor.table.schema.row_size)
    for i in cursor.cell_num..<num_cells - 1 {
        dst := leaf_node_cell(node, i, cursor.table.schema.row_size)
        src := leaf_node_cell(node, i + 1, cursor.table.schema.row_size)
        mem.copy(dst, src, int(cell_sz))
    }
    leaf_node_num_cells(node)^ -= 1
}

get_unused_page_num :: proc(table: ^Table) -> u32 {
    return table.pager.num_pages
}

create_new_root :: proc(table: ^Table, right_child_page_num: u32) {
    root := pager_get_page(table.pager, table.root_page_num)
    right_child := pager_get_page(table.pager, right_child_page_num)
    left_child_page_num := get_unused_page_num(table)
    left_child := pager_get_page(table.pager, left_child_page_num)

    if get_node_type(root) == .NODE_INTERNAL {
        initialize_internal_node(right_child)
        initialize_internal_node(left_child)
    }

    mem.copy(left_child, root, PAGE_SIZE)
    set_node_root(left_child, false)

    if get_node_type(left_child) == .NODE_INTERNAL {
        child: rawptr
        for i in 0..<internal_node_num_keys(left_child)^ {
            child = pager_get_page(table.pager, internal_node_child(left_child, i)^)
            node_parent(child)^ = left_child_page_num
        }
        child = pager_get_page(table.pager, internal_node_right_child(left_child)^)
        node_parent(child)^ = left_child_page_num
    }

    initialize_internal_node(root)
    set_node_root(root, true)
    internal_node_num_keys(root)^ = 1
    internal_node_child(root, 0)^ = left_child_page_num
    left_child_max_key := get_node_max_key(table, left_child)
    internal_node_key(root, 0)^ = left_child_max_key
    internal_node_right_child(root)^ = right_child_page_num
    node_parent(left_child)^ = table.root_page_num
    node_parent(right_child)^ = table.root_page_num
}

update_internal_node_key :: proc(node: rawptr, old_key: u32, new_key: u32) {
    old_child_index := internal_node_find_child(node, old_key)
    internal_node_key(node, old_child_index)^ = new_key
}

internal_node_insert :: proc(table: ^Table, parent_page_num: u32, child_page_num: u32) {
    parent := pager_get_page(table.pager, parent_page_num)
    child := pager_get_page(table.pager, child_page_num)
    child_max_key := get_node_max_key(table, child)
    index := internal_node_find_child(parent, child_max_key)

    original_num_keys := internal_node_num_keys(parent)^
    if original_num_keys >= INTERNAL_NODE_MAX_KEYS {
        internal_node_split_and_insert(table, parent_page_num, child_page_num)
        return
    }

    right_child_page_num := internal_node_right_child(parent)^
    if right_child_page_num == INVALID_PAGE_NUM {
        internal_node_right_child(parent)^ = child_page_num
        return
    }

    right_child := pager_get_page(table.pager, right_child_page_num)
    internal_node_num_keys(parent)^ = original_num_keys + 1

    if child_max_key > get_node_max_key(table, right_child) {
        internal_node_child(parent, original_num_keys)^ = right_child_page_num
        internal_node_key(parent, original_num_keys)^ = get_node_max_key(table, right_child)
        internal_node_right_child(parent)^ = child_page_num
    } else {
        for i := original_num_keys; i > index; i -= 1 {
            destination := internal_node_cell(parent, i)
            source := internal_node_cell(parent, i - 1)
            mem.copy(destination, source, INTERNAL_NODE_CELL_SIZE)
        }
        internal_node_child(parent, index)^ = child_page_num
        internal_node_key(parent, index)^ = child_max_key
    }
}

internal_node_split_and_insert :: proc(table: ^Table, parent_page_num: u32, child_page_num: u32) {
    old_page_num := parent_page_num
    old_node := pager_get_page(table.pager, parent_page_num)
    old_max := get_node_max_key(table, old_node)

    child := pager_get_page(table.pager, child_page_num)
    child_max := get_node_max_key(table, child)

    new_page_num := get_unused_page_num(table)

    splitting_root := is_node_root(old_node)
    parent: rawptr
    new_node: rawptr = nil

    if splitting_root {
        create_new_root(table, new_page_num)
        parent = pager_get_page(table.pager, table.root_page_num)
        old_page_num = internal_node_child(parent, 0)^
        old_node = pager_get_page(table.pager, old_page_num)
    } else {
        parent = pager_get_page(table.pager, node_parent(old_node)^)
        new_node = pager_get_page(table.pager, new_page_num)
        initialize_internal_node(new_node)
    }

    old_num_keys := internal_node_num_keys(old_node)
    cur_page_num := internal_node_right_child(old_node)^
    cur := pager_get_page(table.pager, cur_page_num)

    internal_node_insert(table, new_page_num, cur_page_num)
    node_parent(cur)^ = new_page_num
    internal_node_right_child(old_node)^ = INVALID_PAGE_NUM

    for i := INTERNAL_NODE_MAX_KEYS - 1; i > (INTERNAL_NODE_MAX_KEYS / 2); i -= 1 {
        cur_page_num = internal_node_child(old_node, u32(i))^
        cur = pager_get_page(table.pager, cur_page_num)
        internal_node_insert(table, new_page_num, cur_page_num)
        node_parent(cur)^ = new_page_num
        old_num_keys^ -= 1
    }

    internal_node_right_child(old_node)^ = internal_node_child(old_node, old_num_keys^ - 1)^
    old_num_keys^ -= 1

    max_after_split := get_node_max_key(table, old_node)
    destination_page_num := child_max < max_after_split ? old_page_num : new_page_num

    internal_node_insert(table, destination_page_num, child_page_num)
    node_parent(child)^ = destination_page_num

    update_internal_node_key(parent, old_max, get_node_max_key(table, old_node))

    if !splitting_root {
        internal_node_insert(table, node_parent(old_node)^, new_page_num)
        node_parent(new_node)^ = node_parent(old_node)^
    }
}

leaf_node_split_and_insert :: proc(cursor: ^Cursor, key: u32, value: ^Dynamic_Row) {
    table := cursor.table
    old_node := pager_get_page(table.pager, cursor.page_num)
    old_max := get_node_max_key(table, old_node)
    new_page_num := get_unused_page_num(table)
    new_node := pager_get_page(table.pager, new_page_num)

    initialize_leaf_node(new_node)
    node_parent(new_node)^ = node_parent(old_node)^
    leaf_node_next_leaf(new_node)^ = leaf_node_next_leaf(old_node)^
    leaf_node_next_leaf(old_node)^ = new_page_num

    max_cells := leaf_node_max_cells(table.schema.row_size)
    right_split_count := (max_cells + 1) / 2
    left_split_count := (max_cells + 1) - right_split_count
    cell_sz := leaf_node_cell_size(table.schema.row_size)

    for i := i32(max_cells); i >= 0; i -= 1 {
        destination_node: rawptr
        if u32(i) >= left_split_count {
            destination_node = new_node
        } else {
            destination_node = old_node
        }
        index_within_node := u32(i) % left_split_count
        destination := leaf_node_cell(destination_node, index_within_node, table.schema.row_size)

        if u32(i) == cursor.cell_num {
            serialize_row(value, leaf_node_value(destination_node, index_within_node, table.schema.row_size), &table.schema)
            leaf_node_key(destination_node, index_within_node, table.schema.row_size)^ = key
        } else if u32(i) > cursor.cell_num {
            mem.copy(destination, leaf_node_cell(old_node, u32(i - 1), table.schema.row_size), int(cell_sz))
        } else {
            mem.copy(destination, leaf_node_cell(old_node, u32(i), table.schema.row_size), int(cell_sz))
        }
    }

    leaf_node_num_cells(old_node)^ = left_split_count
    leaf_node_num_cells(new_node)^ = right_split_count

    if is_node_root(old_node) {
        create_new_root(table, new_page_num)
    } else {
        parent_page_num := node_parent(old_node)^
        new_max := get_node_max_key(table, old_node)
        parent := pager_get_page(table.pager, parent_page_num)
        update_internal_node_key(parent, old_max, new_max)
        internal_node_insert(table, parent_page_num, new_page_num)
    }
}

leaf_node_insert :: proc(cursor: ^Cursor, key: u32, value: ^Dynamic_Row) {
    table := cursor.table
    node := pager_get_page(table.pager, cursor.page_num)
    num_cells := leaf_node_num_cells(node)^
    max_cells := leaf_node_max_cells(table.schema.row_size)

    if num_cells >= max_cells {
        leaf_node_split_and_insert(cursor, key, value)
        return
    }
    cell_sz := leaf_node_cell_size(table.schema.row_size)
    if cursor.cell_num < num_cells {
        for i := num_cells; i > cursor.cell_num; i -= 1 {
            dst := leaf_node_cell(node, i, table.schema.row_size)
            src := leaf_node_cell(node, i - 1, table.schema.row_size)
            mem.copy(dst, src, int(cell_sz))
        }
    }
    leaf_node_num_cells(node)^ += 1
    leaf_node_key(node, cursor.cell_num, table.schema.row_size)^ = key
    serialize_row(value, leaf_node_value(node, cursor.cell_num, table.schema.row_size), &table.schema)
}

print_tree :: proc(table: ^Table, page_num: u32, indentation_level: u32) {
    node := pager_get_page(table.pager, page_num)
    num_keys: u32
    child: u32

    for _ in 0..<indentation_level { fmt.printf("  ") }

    switch get_node_type(node) {
    case .NODE_LEAF:
        num_keys = leaf_node_num_cells(node)^
        fmt.printf("- leaf (size %d)\n", num_keys)
        for i in 0..<num_keys {
            for _ in 0..<indentation_level + 1 { fmt.printf("  ") }
            fmt.printf("- %d\n", leaf_node_key(node, i, table.schema.row_size)^)
        }
    case .NODE_INTERNAL:
        num_keys = internal_node_num_keys(node)^
        fmt.printf("- internal (size %d)\n", num_keys)
        if num_keys > 0 {
            for i in 0..<num_keys {
                child = internal_node_child(node, i)^
                print_tree(table, child, indentation_level + 1)
                for _ in 0..<indentation_level + 1 { fmt.printf("  ") }
                fmt.printf("- key %d\n", internal_node_key(node, i)^)
            }
            child = internal_node_right_child(node)^
            print_tree(table, child, indentation_level + 1)
        }
    }
}

// ============================================================================
// SQL Lexer & Parser
// ============================================================================

Token_Kind :: enum {
    TOKEN_IDENTIFIER,
    TOKEN_STRING_LITERAL,
    TOKEN_NUMBER,
    TOKEN_SYMBOL,
    TOKEN_END,
}

Token :: struct {
    kind: Token_Kind,
    text: [MAX_STR_LEN]u8,
}

Token_List :: struct {
    tokens: [MAX_TOKENS]Token,
    count: u32,
    cursor: u32,
}

tokenize_input :: proc(input: string, list: ^Token_List) {
    list.count = 0
    list.cursor = 0
    pos := 0
    length := len(input)

    is_space :: proc(c: u8) -> bool {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'
    }

    is_digit :: proc(c: u8) -> bool {
        return c >= '0' && c <= '9'
    }

    is_alpha :: proc(c: u8) -> bool {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
    }

    is_alnum :: proc(c: u8) -> bool {
        return is_alpha(c) || is_digit(c)
    }

    for pos < length && list.count < MAX_TOKENS - 1 {
        if is_space(input[pos]) {
            pos += 1
            continue
        }

        if input[pos] == ';' {
            list.tokens[list.count].kind = .TOKEN_SYMBOL
            copy_str_to_fixed(list.tokens[list.count].text[:], ";")
            list.count += 1
            pos += 1
            continue
        }

        if input[pos] == '\'' {
            pos += 1
            start := pos
            for pos < length && input[pos] != '\'' {
                pos += 1
            }
            str_sub := input[start:pos]
            if pos < length && input[pos] == '\'' { pos += 1 }

            list.tokens[list.count].kind = .TOKEN_STRING_LITERAL
            copy_str_to_fixed(list.tokens[list.count].text[:], str_sub)
            list.count += 1
            continue
        }

        // Two-character operators
        if pos + 1 < length {
            op2 := input[pos:pos+2]
            if op2 == "<=" || op2 == ">=" || op2 == "!=" || op2 == "<>" {
                list.tokens[list.count].kind = .TOKEN_SYMBOL
                copy_str_to_fixed(list.tokens[list.count].text[:], op2)
                list.count += 1
                pos += 2
                continue
            }
        }

        if strings.contains_rune("=<>(),*", rune(input[pos])) {
            list.tokens[list.count].kind = .TOKEN_SYMBOL
            copy_str_to_fixed(list.tokens[list.count].text[:], input[pos:pos+1])
            list.count += 1
            pos += 1
            continue
        }

        if is_digit(input[pos]) {
            start := pos
            for pos < length && is_digit(input[pos]) {
                pos += 1
            }
            list.tokens[list.count].kind = .TOKEN_NUMBER
            copy_str_to_fixed(list.tokens[list.count].text[:], input[start:pos])
            list.count += 1
            continue
        }

        if is_alpha(input[pos]) || input[pos] == '_' || input[pos] == '.' || input[pos] == '\\' {
            start := pos
            for pos < length && (is_alnum(input[pos]) || input[pos] == '_' || input[pos] == '.' || input[pos] == '\\') {
                pos += 1
            }
            list.tokens[list.count].kind = .TOKEN_IDENTIFIER
            copy_str_to_fixed(list.tokens[list.count].text[:], input[start:pos])
            list.count += 1
            continue
        }

        pos += 1
    }

    list.tokens[list.count].kind = .TOKEN_END
    list.tokens[list.count].text[0] = 0
}

peek_token :: proc(list: ^Token_List) -> ^Token {
    return &list.tokens[list.cursor]
}

peek_token_str :: proc(list: ^Token_List) -> string {
    return fixed_to_string(list.tokens[list.cursor].text[:])
}

advance_token :: proc(list: ^Token_List) -> ^Token {
    tok := &list.tokens[list.cursor]
    list.cursor += 1
    return tok
}

match_token :: proc(list: ^Token_List, text: string) -> bool {
    if strings.equal_fold(peek_token_str(list), text) {
        list.cursor += 1
        return true
    }
    return false
}

parse_comparison :: proc(list: ^Token_List, schema: ^Schema, out: ^^Expr) -> Prepare_Result {
    col := advance_token(list)
    op := advance_token(list)
    val := advance_token(list)

    if col.kind != .TOKEN_IDENTIFIER { return .PREPARE_SYNTAX_ERROR }

    col_name := fixed_to_string(col.text[:])
    col_idx := schema_find_column(schema, col_name)
    if col_idx < 0 { return .PREPARE_SYNTAX_ERROR }

    cmp := new(Expr)
    cmp.type = .EXPR_COMPARISON
    copy_str_to_fixed(cmp.column[:], fixed_to_string(schema.columns[col_idx].name[:]))

    op_text := fixed_to_string(op.text[:])
    if op_text == "="      { cmp.op = .WHERE_OP_EQ }
    else if op_text == "!=" || op_text == "<>" { cmp.op = .WHERE_OP_NEQ }
    else if op_text == ">=" { cmp.op = .WHERE_OP_GE }
    else if op_text == "<=" { cmp.op = .WHERE_OP_LE }
    else if op_text == ">"  { cmp.op = .WHERE_OP_GT }
    else if op_text == "<"  { cmp.op = .WHERE_OP_LT }
    else {
        free(cmp)
        return .PREPARE_SYNTAX_ERROR
    }

    val_text := fixed_to_string(val.text[:])
    if schema.columns[col_idx].type == .INT {
        cmp.value.is_string = false
        int_v, ok := strconv.parse_int(val_text, 10)
        if !ok {
            free(cmp)
            return .PREPARE_SYNTAX_ERROR
        }
        if schema.columns[col_idx].is_primary_key && int_v < 0 {
            free(cmp)
            return .PREPARE_NEGATIVE_ID
        }
        cmp.value.int_value = u32(int_v)
    } else {
        cmp.value.is_string = true
        copy_str_to_fixed(cmp.value.str_value[:], val_text)
    }

    out^ = cmp
    return .PREPARE_SUCCESS
}

parse_primary :: proc(list: ^Token_List, schema: ^Schema, out: ^^Expr) -> Prepare_Result {
    tok := peek_token(list)
    tok_text := fixed_to_string(tok.text[:])
    if tok.kind == .TOKEN_SYMBOL && tok_text == "(" {
        advance_token(list) // consume '('
        res := parse_expr(list, schema, out)
        if res != .PREPARE_SUCCESS { return res }
        next_tok := peek_token(list)
        next_text := fixed_to_string(next_tok.text[:])
        if next_tok.kind != .TOKEN_SYMBOL || next_text != ")" {
            free_expr(out^)
            out^ = nil
            return .PREPARE_SYNTAX_ERROR
        }
        advance_token(list) // consume ')'
        return .PREPARE_SUCCESS
    }
    return parse_comparison(list, schema, out)
}

parse_and :: proc(list: ^Token_List, schema: ^Schema, out: ^^Expr) -> Prepare_Result {
    left: ^Expr = nil
    res := parse_primary(list, schema, &left)
    if res != .PREPARE_SUCCESS { return res }

    for match_token(list, "AND") {
        right: ^Expr = nil
        res = parse_primary(list, schema, &right)
        if res != .PREPARE_SUCCESS {
            free_expr(left)
            return res
        }
        parent := new(Expr)
        parent.type = .EXPR_LOGICAL
        parent.log_op = .LOGICAL_AND
        parent.left = left
        parent.right = right
        left = parent
    }
    out^ = left
    return .PREPARE_SUCCESS
}

parse_or :: proc(list: ^Token_List, schema: ^Schema, out: ^^Expr) -> Prepare_Result {
    left: ^Expr = nil
    res := parse_and(list, schema, &left)
    if res != .PREPARE_SUCCESS { return res }

    for match_token(list, "OR") {
        right: ^Expr = nil
        res = parse_and(list, schema, &right)
        if res != .PREPARE_SUCCESS {
            free_expr(left)
            return res
        }
        parent := new(Expr)
        parent.type = .EXPR_LOGICAL
        parent.log_op = .LOGICAL_OR
        parent.left = left
        parent.right = right
        left = parent
    }
    out^ = left
    return .PREPARE_SUCCESS
}

parse_expr :: proc(list: ^Token_List, schema: ^Schema, out: ^^Expr) -> Prepare_Result {
    return parse_or(list, schema, out)
}

parse_where_clause :: proc(list: ^Token_List, schema: ^Schema, statement: ^Statement) -> Prepare_Result {
    if !match_token(list, "WHERE") { return .PREPARE_SUCCESS }
    return parse_expr(list, schema, &statement.where_expr)
}

prepare_statement :: proc(list: ^Token_List, schema: ^Schema, statement: ^Statement) -> Prepare_Result {
    mem.zero(statement, size_of(Statement))
    if list.count == 0 || peek_token(list).kind == .TOKEN_END {
        return .PREPARE_SYNTAX_ERROR
    }

    first_text := peek_token_str(list)

    // CREATE TABLE
    if strings.equal_fold(first_text, "create") {
        advance_token(list)
        if !match_token(list, "table") { return .PREPARE_SYNTAX_ERROR }
        tbl := advance_token(list)
        statement.type = .STATEMENT_CREATE
        copy_str_to_fixed(statement.table_name[:], fixed_to_string(tbl.text[:]))

        if peek_token_str(list) != "(" { return .PREPARE_SYNTAX_ERROR }
        advance_token(list) // consume '('

        statement.created_schema.has_schema = false
        copy_str_to_fixed(statement.created_schema.table_name[:], fixed_to_string(tbl.text[:]))

        for list.cursor < list.count && peek_token_str(list) != ")" {
            col_name := advance_token(list)
            if col_name.kind != .TOKEN_IDENTIFIER { return .PREPARE_SYNTAX_ERROR }

            col_type := advance_token(list)
            col_type_str := fixed_to_string(col_type.text[:])

            dt: Data_Type = .INT
            len: u32 = 0

            if strings.equal_fold(col_type_str, "int") || strings.equal_fold(col_type_str, "integer") {
                dt = .INT
            } else if strings.equal_fold(col_type_str, "varchar") || strings.equal_fold(col_type_str, "string") || strings.equal_fold(col_type_str, "char") {
                dt = .VARCHAR
                len = 32
                if peek_token_str(list) == "(" {
                    advance_token(list)
                    len_tok := advance_token(list)
                    parsed_len, ok := strconv.parse_int(fixed_to_string(len_tok.text[:]), 10)
                    if ok { len = u32(parsed_len) }
                    if peek_token_str(list) == ")" { advance_token(list) }
                }
            } else {
                return .PREPARE_SYNTAX_ERROR
            }

            is_pk := false
            if strings.equal_fold(peek_token_str(list), "primary") {
                advance_token(list)
                if strings.equal_fold(peek_token_str(list), "key") { advance_token(list) }
                is_pk = true
            }

            schema_add_column(&statement.created_schema, fixed_to_string(col_name.text[:]), dt, len, is_pk)

            if peek_token_str(list) == "," {
                advance_token(list)
            }
        }

        if peek_token_str(list) == ")" { advance_token(list) }
        return .PREPARE_SUCCESS
    }

    if strings.equal_fold(first_text, "begin") || strings.equal_fold(first_text, "start") {
        advance_token(list)
        if strings.equal_fold(first_text, "start") { match_token(list, "transaction") }
        statement.type = .STATEMENT_BEGIN
        return .PREPARE_SUCCESS
    }

    if strings.equal_fold(first_text, "commit") {
        advance_token(list)
        statement.type = .STATEMENT_COMMIT
        return .PREPARE_SUCCESS
    }

    if strings.equal_fold(first_text, "rollback") {
        advance_token(list)
        statement.type = .STATEMENT_ROLLBACK
        return .PREPARE_SUCCESS
    }

    // INSERT INTO
    if strings.equal_fold(first_text, "insert") {
        advance_token(list)
        match_token(list, "into")
        advance_token(list) // consume table name

        if peek_token_str(list) == "(" {
            for list.cursor < list.count && peek_token_str(list) != ")" {
                advance_token(list)
            }
            if peek_token_str(list) == ")" { advance_token(list) }
        }

        if !match_token(list, "values") { return .PREPARE_SYNTAX_ERROR }
        if peek_token_str(list) == "(" { advance_token(list) }

        statement.type = .STATEMENT_INSERT
        statement.row_to_insert.num_values = schema.num_columns

        for i in 0..<schema.num_columns {
            val_tok := advance_token(list)
            if peek_token_str(list) == "," { advance_token(list) }

            v := &statement.row_to_insert.values[i]
            v.type = schema.columns[i].type

            val_str := fixed_to_string(val_tok.text[:])
            if schema.columns[i].type == .INT {
                int_v, ok := strconv.parse_int(val_str, 10)
                if !ok { return .PREPARE_SYNTAX_ERROR }
                if schema.columns[i].is_primary_key && int_v < 0 { return .PREPARE_NEGATIVE_ID }
                v.int_val = i32(int_v)
            } else {
                if u32(len(val_str)) > schema.columns[i].length { return .PREPARE_STRING_TOO_LONG }
                copy_str_to_fixed(v.str_val[:], val_str)
            }
        }

        if peek_token_str(list) == ")" { advance_token(list) }
        return .PREPARE_SUCCESS
    }

    // SELECT
    if strings.equal_fold(first_text, "select") {
        advance_token(list)
        statement.type = .STATEMENT_SELECT

        if strings.equal_fold(peek_token_str(list), "count") {
            advance_token(list)
            if peek_token_str(list) == "(" { advance_token(list) }
            if peek_token_str(list) == "*" { advance_token(list) }
            if peek_token_str(list) == ")" { advance_token(list) }
            statement.is_count = true
        } else if peek_token_str(list) == "*" {
            advance_token(list)
        }

        if match_token(list, "from") {
            advance_token(list)
        }

        return parse_where_clause(list, schema, statement)
    }

    // UPDATE
    if strings.equal_fold(first_text, "update") {
        advance_token(list)
        statement.type = .STATEMENT_UPDATE
        statement.is_set_update = true

        advance_token(list) // table name
        match_token(list, "set")

        for list.cursor < list.count &&
            !strings.equal_fold(peek_token_str(list), "where") &&
            peek_token_str(list) != ";" {
            if peek_token_str(list) == "," {
                advance_token(list)
                continue
            }

            col := advance_token(list)
            if !match_token(list, "=") { return .PREPARE_SYNTAX_ERROR }
            val := advance_token(list)

            if statement.num_update_assignments < MAX_ASSIGNMENTS {
                assign := &statement.update_assignments[statement.num_update_assignments]
                statement.num_update_assignments += 1
                copy_str_to_fixed(assign.column_name[:], fixed_to_string(col.text[:]))
                copy_str_to_fixed(assign.value_text[:], fixed_to_string(val.text[:]))
            }
        }

        return parse_where_clause(list, schema, statement)
    }

    // DELETE
    if strings.equal_fold(first_text, "delete") {
        advance_token(list)
        statement.type = .STATEMENT_DELETE

        if match_token(list, "from") {
            advance_token(list)
        }

        return parse_where_clause(list, schema, statement)
    }

    return .PREPARE_UNRECOGNIZED_STATEMENT
}

// ============================================================================
// Execution Engine
// ============================================================================

row_matches_where :: proc(row: ^Dynamic_Row, statement: ^Statement, schema: ^Schema) -> bool {
    if statement.where_expr == nil { return true }
    return evaluate_expr(statement.where_expr, row, schema)
}

execute_insert :: proc(statement: ^Statement, table: ^Table) -> Execute_Result {
    if table.pager.num_pages + INSERT_PAGE_SAFETY_MARGIN > TABLE_MAX_PAGES {
        return .EXECUTE_TABLE_FULL
    }

    key_to_insert := get_pk_value(&statement.row_to_insert, &table.schema)
    cursor := table_find(table, key_to_insert)

    node := pager_get_page(table.pager, cursor.page_num)
    num_cells := leaf_node_num_cells(node)^
    if cursor.cell_num < num_cells {
        key_at_index := leaf_node_key(node, cursor.cell_num, table.schema.row_size)^
        if key_at_index == key_to_insert {
            free(cursor)
            return .EXECUTE_DUPLICATE_KEY
        }
    }

    leaf_node_insert(cursor, key_to_insert, &statement.row_to_insert)
    free(cursor)
    return .EXECUTE_SUCCESS
}

Pending_Update :: struct {
    old_key: u32,
    new_key: u32,
    row: Dynamic_Row,
}

execute_update :: proc(statement: ^Statement, table: ^Table) -> Execute_Result {
    pending := make([dynamic]Pending_Update)
    defer delete(pending)

    cursor := table_start(table)
    row: Dynamic_Row

    for !cursor.end_of_table {
        deserialize_row(cursor_value(cursor), &row, &table.schema)
        if row_matches_where(&row, statement, &table.schema) {
            old_key := get_pk_value(&row, &table.schema)

            for i in 0..<statement.num_update_assignments {
                assign := &statement.update_assignments[i]
                col_name := fixed_to_string(assign.column_name[:])
                col_idx := schema_find_column(&table.schema, col_name)
                if col_idx >= 0 && u32(col_idx) < row.num_values {
                    val_str := fixed_to_string(assign.value_text[:])
                    if table.schema.columns[col_idx].type == .INT {
                        int_v, _ := strconv.parse_int(val_str, 10)
                        row.values[col_idx].int_val = i32(int_v)
                    } else {
                        copy_str_to_fixed(row.values[col_idx].str_val[:], val_str)
                    }
                }
            }

            append(&pending, Pending_Update{
                old_key = old_key,
                new_key = get_pk_value(&row, &table.schema),
                row = row,
            })
        }
        cursor_advance(cursor)
    }
    free(cursor)

    applied: u32 = 0
    skipped_duplicates: u32 = 0

    for &u in pending {
        if u.new_key == u.old_key {
            c := table_find(table, u.old_key)
            node := pager_get_page(table.pager, c.page_num)
            num_cells := leaf_node_num_cells(node)^
            if c.cell_num < num_cells &&
               leaf_node_key(node, c.cell_num, table.schema.row_size)^ == u.old_key {
                serialize_row(&u.row, cursor_value(c), &table.schema)
                applied += 1
            }
            free(c)
            continue
        }

        dest := table_find(table, u.new_key)
        dest_node := pager_get_page(table.pager, dest.page_num)
        dest_num_cells := leaf_node_num_cells(dest_node)^
        duplicate := (dest.cell_num < dest_num_cells &&
                      leaf_node_key(dest_node, dest.cell_num, table.schema.row_size)^ == u.new_key)
        free(dest)

        if duplicate {
            skipped_duplicates += 1
            continue
        }

        old_cursor := table_find(table, u.old_key)
        old_node := pager_get_page(table.pager, old_cursor.page_num)
        old_num_cells := leaf_node_num_cells(old_node)^
        if old_cursor.cell_num < old_num_cells &&
           leaf_node_key(old_node, old_cursor.cell_num, table.schema.row_size)^ == u.old_key {
            leaf_node_delete(old_cursor)
            free(old_cursor)

            insert_cursor := table_find(table, u.new_key)
            leaf_node_insert(insert_cursor, u.new_key, &u.row)
            free(insert_cursor)
            applied += 1
        } else {
            free(old_cursor)
        }
    }

    if skipped_duplicates > 0 {
        fmt.printf("UPDATE %d (skipped %d due to duplicate key)\n", applied, skipped_duplicates)
    } else {
        fmt.printf("UPDATE %d\n", applied)
    }
    return .EXECUTE_SUCCESS
}

execute_delete :: proc(statement: ^Statement, table: ^Table) -> Execute_Result {
    keys_to_delete := make([dynamic]u32)
    defer delete(keys_to_delete)

    cursor := table_start(table)
    row: Dynamic_Row

    for !cursor.end_of_table {
        deserialize_row(cursor_value(cursor), &row, &table.schema)
        if row_matches_where(&row, statement, &table.schema) {
            append(&keys_to_delete, get_pk_value(&row, &table.schema))
        }
        cursor_advance(cursor)
    }
    free(cursor)

    for key in keys_to_delete {
        c := table_find(table, key)
        node := pager_get_page(table.pager, c.page_num)
        num_cells := leaf_node_num_cells(node)^
        if c.cell_num < num_cells && leaf_node_key(node, c.cell_num, table.schema.row_size)^ == key {
            leaf_node_delete(c)
        }
        free(c)
    }
    fmt.printf("DELETE %d\n", len(keys_to_delete))
    return .EXECUTE_SUCCESS
}

execute_select :: proc(statement: ^Statement, table: ^Table) -> Execute_Result {
    cursor := table_start(table)
    row: Dynamic_Row
    match_count: u32 = 0

    for !cursor.end_of_table {
        deserialize_row(cursor_value(cursor), &row, &table.schema)
        if row_matches_where(&row, statement, &table.schema) {
            match_count += 1
            if !statement.is_count { print_row(&row, &table.schema) }
        }
        cursor_advance(cursor)
    }
    free(cursor)

    if statement.is_count {
        fmt.printf("%d row(s).\n", match_count)
    }
    return .EXECUTE_SUCCESS
}

execute_begin :: proc(table: ^Table) -> Execute_Result {
    if table.in_transaction { return .EXECUTE_TX_ALREADY_ACTIVE }

    table.in_transaction = true
    table.tx_original_num_pages = table.pager.num_pages

    for i in 0..<TABLE_MAX_PAGES {
        page_ptr := table.pager.pages[i]
        if page_ptr != nil {
            table.tx_backup[i], _ = mem.alloc(PAGE_SIZE)
            mem.copy(table.tx_backup[i], page_ptr, PAGE_SIZE)
            table.tx_was_cached[i] = true
        } else {
            table.tx_backup[i] = nil
            table.tx_was_cached[i] = false
        }
    }
    return .EXECUTE_SUCCESS
}

execute_commit :: proc(table: ^Table) -> Execute_Result {
    if !table.in_transaction { return .EXECUTE_NO_ACTIVE_TX }

    tx_free_backups(table)
    for i in 0..<table.pager.num_pages {
        if table.pager.pages[i] != nil {
            pager_flush(table.pager, i)
        }
    }
    table.in_transaction = false
    return .EXECUTE_SUCCESS
}

execute_rollback :: proc(table: ^Table) -> Execute_Result {
    if !table.in_transaction { return .EXECUTE_NO_ACTIVE_TX }

    for i in 0..<TABLE_MAX_PAGES {
        if u32(i) < table.tx_original_num_pages {
            if table.tx_was_cached[i] {
                mem.copy(table.pager.pages[i], table.tx_backup[i], PAGE_SIZE)
                free(table.tx_backup[i])
                table.tx_backup[i] = nil
            } else if table.pager.pages[i] != nil {
                free(table.pager.pages[i])
                table.pager.pages[i] = nil
            }
        } else {
            if table.pager.pages[i] != nil {
                free(table.pager.pages[i])
                table.pager.pages[i] = nil
            }
            if table.tx_backup[i] != nil {
                free(table.tx_backup[i])
                table.tx_backup[i] = nil
            }
        }
    }
    table.pager.num_pages = table.tx_original_num_pages
    table.in_transaction = false
    return .EXECUTE_SUCCESS
}

execute_statement :: proc(statement: ^Statement, table: ^Table) -> Execute_Result {
    #partial switch statement.type {
    case .STATEMENT_INSERT:
        return execute_insert(statement, table)
    case .STATEMENT_SELECT:
        return execute_select(statement, table)
    case .STATEMENT_UPDATE:
        return execute_update(statement, table)
    case .STATEMENT_DELETE:
        return execute_delete(statement, table)
    case .STATEMENT_CREATE:
        table.schema = statement.created_schema

        meta := cast(^Meta_Page)pager_get_page(table.pager, 0)
        meta.magic = SCHEMA_MAGIC
        meta.root_page_num = table.root_page_num
        meta.schema = table.schema

        root_node := pager_get_page(table.pager, table.root_page_num)
        initialize_leaf_node(root_node)
        set_node_root(root_node, true)
        fmt.printf("CREATE TABLE (%d columns configured)\n", table.schema.num_columns)
        return .EXECUTE_SUCCESS
    case .STATEMENT_BEGIN:
        return execute_begin(table)
    case .STATEMENT_COMMIT:
        return execute_commit(table)
    case .STATEMENT_ROLLBACK:
        return execute_rollback(table)
    }
    return .EXECUTE_SUCCESS
}

// ============================================================================
// Shell & Meta Commands
// ============================================================================

print_help :: proc() {
    fmt.printf(
        "SQL Commands:\n" +
        "  CREATE TABLE <name> (<pk_col> INT PRIMARY KEY, <col2> VARCHAR(32), <col3> INT, ...);\n" +
        "  INSERT INTO <name> VALUES (<val1>, '<val2>', ...);\n" +
        "  SELECT * FROM <name> [WHERE <expr>];\n" +
        "  SELECT COUNT(*) FROM <name> [WHERE <expr>];\n" +
        "  UPDATE <name> SET <col> = <val> WHERE <expr>;\n" +
        "  DELETE FROM <name> WHERE <expr>;\n" +
        "  (WHERE supports =, !=, <>, <, >, <=, >=, AND, OR, and parentheses ())\n" +
        "  BEGIN; | COMMIT; | ROLLBACK;\n" +
        "Meta commands:\n" +
        "  \\q or .exit      quit the shell\n" +
        "  \\d or .btree     print the B+tree structure\n" +
        "  \\c or .constants print page size constants\n" +
        "  \\? or .help      show this message\n",
    )
}

print_constants :: proc(table: ^Table) {
    fmt.printf(
        "ROW_SIZE: %d\n" +
        "COMMON_NODE_HEADER_SIZE: %d\n" +
        "LEAF_NODE_HEADER_SIZE: %d\n" +
        "LEAF_NODE_CELL_SIZE: %d\n" +
        "LEAF_NODE_MAX_CELLS: %d\n",
        table.schema.row_size,
        COMMON_NODE_HEADER_SIZE,
        LEAF_NODE_HEADER_SIZE,
        leaf_node_cell_size(table.schema.row_size),
        leaf_node_max_cells(table.schema.row_size),
    )
}

do_meta_command :: proc(input: string, table: ^Table) -> Meta_Command_Result {
    if input == ".exit" || input == "\\q" {
        table_close(table)
        os.exit(0)
    } else if input == ".btree" || input == "\\d" {
        fmt.printf("Tree:\n")
        print_tree(table, table.root_page_num, 0)
        return .META_COMMAND_SUCCESS
    } else if input == ".constants" || input == "\\c" {
        fmt.printf("Constants:\n")
        print_constants(table)
        return .META_COMMAND_SUCCESS
    } else if input == ".help" || input == "\\?" {
        print_help()
        return .META_COMMAND_SUCCESS
    }
    return .META_COMMAND_UNRECOGNIZED_COMMAND
}

read_line :: proc(buf: []u8) -> (string, bool) {
    n, err := os.read(os.stdin, buf)
    if err != nil || n <= 0 { return "", false }
    line := strings.trim_right(string(buf[:n]), "\r\n")
    return line, true
}

main :: proc() {
    if len(os.args) < 2 {
        fmt.printf("Must supply a database filename.\n")
        os.exit(1)
    }

    filename := os.args[1]
    table := table_open(filename)

    input_buf: [1024]u8

    for {
        fmt.printf("db=# ")

        input_buffer, ok := read_line(input_buf[:])
        if !ok { break }

        input_buffer = strings.trim_space(input_buffer)
        if len(input_buffer) == 0 { continue }

        if input_buffer[0] == '.' || input_buffer[0] == '\\' {
            switch do_meta_command(input_buffer, table) {
            case .META_COMMAND_SUCCESS:
                continue
            case .META_COMMAND_UNRECOGNIZED_COMMAND:
                fmt.printf("Unrecognized command '%s'\n", input_buffer)
                continue
            }
        }

        tokens: Token_List
        tokenize_input(input_buffer, &tokens)

        statement: Statement
        prep_res := prepare_statement(&tokens, &table.schema, &statement)

        switch prep_res {
        case .PREPARE_SUCCESS:
            // Handled below
        case .PREPARE_NEGATIVE_ID:
            fmt.printf("ID must be positive.\n")
            free_expr(statement.where_expr)
            continue
        case .PREPARE_STRING_TOO_LONG:
            fmt.printf("String is too long for column budget.\n")
            free_expr(statement.where_expr)
            continue
        case .PREPARE_SYNTAX_ERROR:
            fmt.printf("Syntax error. Could not parse statement.\n")
            free_expr(statement.where_expr)
            continue
        case .PREPARE_UNRECOGNIZED_STATEMENT:
            fmt.printf("Unrecognized keyword at start of '%s'.\n", input_buffer)
            free_expr(statement.where_expr)
            continue
        }

        if !table.schema.has_schema &&
           (statement.type == .STATEMENT_INSERT || statement.type == .STATEMENT_SELECT ||
            statement.type == .STATEMENT_UPDATE || statement.type == .STATEMENT_DELETE) {
            fmt.printf("Error: No table schema found. Please run CREATE TABLE first.\n")
            free_expr(statement.where_expr)
            continue
        }

        exec_res := execute_statement(&statement, table)
        free_expr(statement.where_expr)

        switch exec_res {
        case .EXECUTE_SUCCESS:
            if statement.type == .STATEMENT_INSERT {
                fmt.printf("INSERT 0 1\n")
            } else if statement.type == .STATEMENT_BEGIN {
                fmt.printf("BEGIN\n")
            } else if statement.type == .STATEMENT_COMMIT {
                fmt.printf("COMMIT\n")
            } else if statement.type == .STATEMENT_ROLLBACK {
                fmt.printf("ROLLBACK\n")
            }
        case .EXECUTE_DUPLICATE_KEY:
            fmt.printf("Error: Duplicate key.\n")
        case .EXECUTE_NOT_FOUND:
            fmt.printf("Error: row not found.\n")
        case .EXECUTE_TX_ALREADY_ACTIVE:
            fmt.printf("Error: a transaction is already active.\n")
        case .EXECUTE_NO_ACTIVE_TX:
            fmt.printf("Error: no active transaction.\n")
        case .EXECUTE_TABLE_FULL:
            fmt.printf("Error: table is full (max %d pages).\n", TABLE_MAX_PAGES)
        }
    }

    table_close(table)
}
