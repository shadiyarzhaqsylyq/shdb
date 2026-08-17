package main

import "core:fmt"
import "core:mem"
import "core:os"
import "core:strconv"
import "core:strings"

// ============================================================================
// Constants & Data Structures
// ============================================================================

COLUMN_USERNAME_SIZE :: 32
COLUMN_EMAIL_SIZE :: 255

Row :: struct {
    id: u32,
    username: [COLUMN_USERNAME_SIZE + 1]u8,
    email: [COLUMN_EMAIL_SIZE + 1]u8,
}

ID_SIZE :: size_of(u32)
USERNAME_SIZE :: COLUMN_USERNAME_SIZE + 1
EMAIL_SIZE :: COLUMN_EMAIL_SIZE + 1
ID_OFFSET :: 0
USERNAME_OFFSET :: ID_OFFSET + ID_SIZE
EMAIL_OFFSET :: USERNAME_OFFSET + USERNAME_SIZE
ROW_SIZE :: ID_SIZE + USERNAME_SIZE + EMAIL_SIZE

PAGE_SIZE :: 4096
TABLE_MAX_PAGES :: 400
INVALID_PAGE_NUM :: u32(max(u32))

Execute_Result :: enum {
    EXECUTE_SUCCESS,
    EXECUTE_DUPLICATE_KEY,
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
}

Statement :: struct {
    type: Statement_Type,
    row_to_insert: Row,
}

Pager :: struct {
    file_descriptor: ^os.File,
    file_length: u32,
    num_pages: u32,
    pages: [TABLE_MAX_PAGES]rawptr,
}

Table :: struct {
    pager: ^Pager,
    root_page_num: u32,
}

Cursor :: struct {
    table: ^Table,
    page_num: u32,
    cell_num: u32,
    end_of_table: bool,
}

Node_Type :: enum u8 {
    NODE_INTERNAL = 0,
    NODE_LEAF = 1,
}

// ============================================================================
// B+Tree Node Headers & Layout
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
LEAF_NODE_KEY_OFFSET :: 0
LEAF_NODE_VALUE_SIZE :: ROW_SIZE
LEAF_NODE_VALUE_OFFSET :: LEAF_NODE_KEY_OFFSET + LEAF_NODE_KEY_SIZE
LEAF_NODE_CELL_SIZE :: LEAF_NODE_KEY_SIZE + LEAF_NODE_VALUE_SIZE
LEAF_NODE_SPACE_FOR_CELLS :: PAGE_SIZE - LEAF_NODE_HEADER_SIZE
LEAF_NODE_MAX_CELLS :: LEAF_NODE_SPACE_FOR_CELLS / LEAF_NODE_CELL_SIZE
LEAF_NODE_RIGHT_SPLIT_COUNT :: (LEAF_NODE_MAX_CELLS + 1) / 2
LEAF_NODE_LEFT_SPLIT_COUNT :: (LEAF_NODE_MAX_CELLS + 1) - LEAF_NODE_RIGHT_SPLIT_COUNT

// ============================================================================
// Node Pointer Helpers
// ============================================================================

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

leaf_node_num_cells :: proc(node: rawptr) -> ^u32 {
    return cast(^u32)node_ptr(node, LEAF_NODE_NUM_CELLS_OFFSET)
}

leaf_node_next_leaf :: proc(node: rawptr) -> ^u32 {
    return cast(^u32)node_ptr(node, LEAF_NODE_NEXT_LEAF_OFFSET)
}

leaf_node_cell :: proc(node: rawptr, cell_num: u32) -> rawptr {
    return node_ptr(node, LEAF_NODE_HEADER_SIZE + int(cell_num) * LEAF_NODE_CELL_SIZE)
}

leaf_node_key :: proc(node: rawptr, cell_num: u32) -> ^u32 {
    return cast(^u32)leaf_node_cell(node, cell_num)
}

leaf_node_value :: proc(node: rawptr, cell_num: u32) -> rawptr {
    return node_ptr(leaf_node_cell(node, cell_num), LEAF_NODE_KEY_SIZE)
}

// ============================================================================
// String & Row Serialization Helpers
// ============================================================================

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

print_row :: proc(row: ^Row) {
    fmt.printf("(%d, %s, %s)\n", row.id, fixed_to_string(row.username[:]), fixed_to_string(row.email[:]))
}

serialize_row :: proc(source: ^Row, destination: rawptr) {
    mem.copy(node_ptr(destination, ID_OFFSET), &source.id, ID_SIZE)
    mem.copy(node_ptr(destination, USERNAME_OFFSET), &source.username[0], USERNAME_SIZE)
    mem.copy(node_ptr(destination, EMAIL_OFFSET), &source.email[0], EMAIL_SIZE)
}

deserialize_row :: proc(source: rawptr, destination: ^Row) {
    mem.copy(&destination.id, node_ptr(source, ID_OFFSET), ID_SIZE)
    mem.copy(&destination.username[0], node_ptr(source, USERNAME_OFFSET), USERNAME_SIZE)
    mem.copy(&destination.email[0], node_ptr(source, EMAIL_OFFSET), EMAIL_SIZE)
}

// ============================================================================
// Pager & Memory Operations
// ============================================================================

get_page :: proc(pager: ^Pager, page_num: u32) -> rawptr {
    if page_num >= TABLE_MAX_PAGES {
        fmt.printf("Tried to fetch page number out of bounds. %d >= %d\n", page_num, TABLE_MAX_PAGES)
        os.exit(1)
    }

    if pager.pages[page_num] == nil {
        page, _ := mem.alloc(PAGE_SIZE)
        mem.zero(page, PAGE_SIZE) // Zero-initialize new page

        offset := i64(page_num) * PAGE_SIZE

        // Only read from disk if the page already exists in the file
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

get_node_max_key :: proc(pager: ^Pager, node: rawptr) -> u32 {
    if get_node_type(node) == .NODE_LEAF {
        return leaf_node_key(node, leaf_node_num_cells(node)^ - 1)^
    }
    right_child := get_page(pager, internal_node_right_child(node)^)
    return get_node_max_key(pager, right_child)
}

print_constants :: proc() {
    fmt.printf("ROW_SIZE: %d\n", ROW_SIZE)
    fmt.printf("COMMON_NODE_HEADER_SIZE: %d\n", COMMON_NODE_HEADER_SIZE)
    fmt.printf("LEAF_NODE_HEADER_SIZE: %d\n", LEAF_NODE_HEADER_SIZE)
    fmt.printf("LEAF_NODE_CELL_SIZE: %d\n", LEAF_NODE_CELL_SIZE)
    fmt.printf("LEAF_NODE_SPACE_FOR_CELLS: %d\n", LEAF_NODE_SPACE_FOR_CELLS)
    fmt.printf("LEAF_NODE_MAX_CELLS: %d\n", LEAF_NODE_MAX_CELLS)
}

indent :: proc(level: u32) {
    for _ in 0..<level {
        fmt.printf("  ")
    }
}

print_tree :: proc(pager: ^Pager, page_num: u32, indentation_level: u32) {
    node := get_page(pager, page_num)
    num_keys: u32
    child: u32

    switch get_node_type(node) {
    case .NODE_LEAF:
        num_keys = leaf_node_num_cells(node)^
        indent(indentation_level)
        fmt.printf("- leaf (size %d)\n", num_keys)
        for i in 0..<num_keys {
            indent(indentation_level + 1)
            fmt.printf("- %d\n", leaf_node_key(node, i)^)
        }
    case .NODE_INTERNAL:
        num_keys = internal_node_num_keys(node)^
        indent(indentation_level)
        fmt.printf("- internal (size %d)\n", num_keys)
        if num_keys > 0 {
            for i in 0..<num_keys {
                child = internal_node_child(node, i)^
                print_tree(pager, child, indentation_level + 1)

                indent(indentation_level + 1)
                fmt.printf("- key %d\n", internal_node_key(node, i)^)
            }
            child = internal_node_right_child(node)^
            print_tree(pager, child, indentation_level + 1)
        }
    }
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

// ============================================================================
// Cursor & Navigation Operations
// ============================================================================

leaf_node_find :: proc(table: ^Table, page_num: u32, key: u32) -> ^Cursor {
    node := get_page(table.pager, page_num)
    num_cells := leaf_node_num_cells(node)^

    cursor := new(Cursor)
    cursor.table = table
    cursor.page_num = page_num
    cursor.end_of_table = false

    min_index: u32 = 0
    one_past_max_index: u32 = num_cells
    for one_past_max_index != min_index {
        index := (min_index + one_past_max_index) / 2
        key_at_index := leaf_node_key(node, index)^
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
    node := get_page(table.pager, page_num)

    child_index := internal_node_find_child(node, key)
    child_num := internal_node_child(node, child_index)^
    child := get_page(table.pager, child_num)
    #partial switch get_node_type(child) {
    case .NODE_LEAF:
        return leaf_node_find(table, child_num, key)
    case .NODE_INTERNAL:
        return internal_node_find(table, child_num, key)
    }
    return nil
}

table_find :: proc(table: ^Table, key: u32) -> ^Cursor {
    root_page_num := table.root_page_num
    root_node := get_page(table.pager, root_page_num)

    if get_node_type(root_node) == .NODE_LEAF {
        return leaf_node_find(table, root_page_num, key)
    } else {
        return internal_node_find(table, root_page_num, key)
    }
}

table_start :: proc(table: ^Table) -> ^Cursor {
    cursor := table_find(table, 0)

    node := get_page(table.pager, cursor.page_num)
    num_cells := leaf_node_num_cells(node)^
    cursor.end_of_table = (num_cells == 0)

    return cursor
}

cursor_value :: proc(cursor: ^Cursor) -> rawptr {
    page_num := cursor.page_num
    page := get_page(cursor.table.pager, page_num)
    return leaf_node_value(page, cursor.cell_num)
}

cursor_advance :: proc(cursor: ^Cursor) {
    page_num := cursor.page_num
    node := get_page(cursor.table.pager, page_num)

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

// ============================================================================
// Database Management & Disk Operations
// ============================================================================

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

db_open :: proc(filename: string) -> ^Table {
    pager := pager_open(filename)

    table := new(Table)
    table.pager = pager
    table.root_page_num = 0

    if pager.num_pages == 0 {
        root_node := get_page(pager, 0)
        initialize_leaf_node(root_node)
        set_node_root(root_node, true)
    }

    return table
}

print_prompt :: proc() { fmt.printf("db > ") }

read_line :: proc(buf: []u8) -> (string, bool) {
    n, err := os.read(os.stdin, buf)
    if err != nil || n <= 0 { return "", false }
    line := strings.trim_right(string(buf[:n]), "\r\n")
    return line, true
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

db_close :: proc(table: ^Table) {
    pager := table.pager

    for i in 0..<pager.num_pages {
        if pager.pages[i] == nil { continue }
        pager_flush(pager, i)
        free(pager.pages[i])
        pager.pages[i] = nil
    }

    os.close(pager.file_descriptor)

    for i in 0..<TABLE_MAX_PAGES {
        if pager.pages[i] != nil {
            free(pager.pages[i])
            pager.pages[i] = nil
        }
    }
    free(pager)
    free(table)
}

do_meta_command :: proc(input: string, table: ^Table) -> Meta_Command_Result {
    if input == ".exit" {
        db_close(table)
        os.exit(0)
    } else if input == ".btree" {
        fmt.printf("Tree:\n")
        print_tree(table.pager, 0, 0)
        return .META_COMMAND_SUCCESS
    } else if input == ".constants" {
        fmt.printf("Constants:\n")
        print_constants()
        return .META_COMMAND_SUCCESS
    } else {
        return .META_COMMAND_UNRECOGNIZED_COMMAND
    }
}

prepare_insert :: proc(input: string, statement: ^Statement) -> Prepare_Result {
    statement.type = .STATEMENT_INSERT

    parts := strings.split(input, " ")
    defer delete(parts)

    if len(parts) < 4 {
        return .PREPARE_SYNTAX_ERROR
    }

    id_val, ok := strconv.parse_int(parts[1], 10)
    if !ok || id_val < 0 {
        return .PREPARE_NEGATIVE_ID
    }

    username := parts[2]
    email := parts[3]

    if len(username) > COLUMN_USERNAME_SIZE {
        return .PREPARE_STRING_TOO_LONG
    }
    if len(email) > COLUMN_EMAIL_SIZE {
        return .PREPARE_STRING_TOO_LONG
    }

    statement.row_to_insert.id = u32(id_val)
    copy_str_to_fixed(statement.row_to_insert.username[:], username)
    copy_str_to_fixed(statement.row_to_insert.email[:], email)

    return .PREPARE_SUCCESS
}

prepare_statement :: proc(input: string, statement: ^Statement) -> Prepare_Result {
    if strings.has_prefix(input, "insert") {
        return prepare_insert(input, statement)
    }
    if input == "select" {
        statement.type = .STATEMENT_SELECT
        return .PREPARE_SUCCESS
    }

    return .PREPARE_UNRECOGNIZED_STATEMENT
}

// ============================================================================
// Split & Insert Logic
// ============================================================================

get_unused_page_num :: proc(pager: ^Pager) -> u32 { return pager.num_pages }

create_new_root :: proc(table: ^Table, right_child_page_num: u32) {
    root := get_page(table.pager, table.root_page_num)
    right_child := get_page(table.pager, right_child_page_num)
    left_child_page_num := get_unused_page_num(table.pager)
    left_child := get_page(table.pager, left_child_page_num)

    if get_node_type(root) == .NODE_INTERNAL {
        initialize_internal_node(right_child)
        initialize_internal_node(left_child)
    }

    mem.copy(left_child, root, PAGE_SIZE)
    set_node_root(left_child, false)

    if get_node_type(left_child) == .NODE_INTERNAL {
        child: rawptr
        for i in 0..<internal_node_num_keys(left_child)^ {
            child = get_page(table.pager, internal_node_child(left_child, i)^)
            node_parent(child)^ = left_child_page_num
        }
        child = get_page(table.pager, internal_node_right_child(left_child)^)
        node_parent(child)^ = left_child_page_num
    }

    initialize_internal_node(root)
    set_node_root(root, true)
    internal_node_num_keys(root)^ = 1
    internal_node_child(root, 0)^ = left_child_page_num
    left_child_max_key := get_node_max_key(table.pager, left_child)
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
    parent := get_page(table.pager, parent_page_num)
    child := get_page(table.pager, child_page_num)
    child_max_key := get_node_max_key(table.pager, child)
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

    right_child := get_page(table.pager, right_child_page_num)
    internal_node_num_keys(parent)^ = original_num_keys + 1

    if child_max_key > get_node_max_key(table.pager, right_child) {
        internal_node_child(parent, original_num_keys)^ = right_child_page_num
        internal_node_key(parent, original_num_keys)^ = get_node_max_key(table.pager, right_child)
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
    old_node := get_page(table.pager, parent_page_num)
    old_max := get_node_max_key(table.pager, old_node)

    child := get_page(table.pager, child_page_num)
    child_max := get_node_max_key(table.pager, child)

    new_page_num := get_unused_page_num(table.pager)

    splitting_root := is_node_root(old_node)

    parent: rawptr
    new_node: rawptr
    if splitting_root {
        create_new_root(table, new_page_num)
        parent = get_page(table.pager, table.root_page_num)
        old_page_num = internal_node_child(parent, 0)^
        old_node = get_page(table.pager, old_page_num)
    } else {
        parent = get_page(table.pager, node_parent(old_node)^)
        new_node = get_page(table.pager, new_page_num)
        initialize_internal_node(new_node)
    }

    old_num_keys := internal_node_num_keys(old_node)

    cur_page_num := internal_node_right_child(old_node)^
    cur := get_page(table.pager, cur_page_num)

    internal_node_insert(table, new_page_num, cur_page_num)
    node_parent(cur)^ = new_page_num
    internal_node_right_child(old_node)^ = INVALID_PAGE_NUM

    for i := INTERNAL_NODE_MAX_KEYS - 1; i > INTERNAL_NODE_MAX_KEYS / 2; i -= 1 {
        cur_page_num = internal_node_child(old_node, u32(i))^
        cur = get_page(table.pager, cur_page_num)

        internal_node_insert(table, new_page_num, cur_page_num)
        node_parent(cur)^ = new_page_num

        old_num_keys^ -= 1
    }

    internal_node_right_child(old_node)^ = internal_node_child(old_node, old_num_keys^ - 1)^
    old_num_keys^ -= 1

    max_after_split := get_node_max_key(table.pager, old_node)
    destination_page_num := child_max < max_after_split ? old_page_num : new_page_num

    internal_node_insert(table, destination_page_num, child_page_num)
    node_parent(child)^ = destination_page_num

    update_internal_node_key(parent, old_max, get_node_max_key(table.pager, old_node))

    if !splitting_root {
        internal_node_insert(table, node_parent(old_node)^, new_page_num)
        node_parent(new_node)^ = node_parent(old_node)^
    }
}

leaf_node_split_and_insert :: proc(cursor: ^Cursor, key: u32, value: ^Row) {
    old_node := get_page(cursor.table.pager, cursor.page_num)
    old_max := get_node_max_key(cursor.table.pager, old_node)
    new_page_num := get_unused_page_num(cursor.table.pager)
    new_node := get_page(cursor.table.pager, new_page_num)
    initialize_leaf_node(new_node)
    node_parent(new_node)^ = node_parent(old_node)^
    leaf_node_next_leaf(new_node)^ = leaf_node_next_leaf(old_node)^
    leaf_node_next_leaf(old_node)^ = new_page_num

    for i := i32(LEAF_NODE_MAX_CELLS); i >= 0; i -= 1 {
        destination_node: rawptr
        if u32(i) >= LEAF_NODE_LEFT_SPLIT_COUNT {
            destination_node = new_node
        } else {
            destination_node = old_node
        }
        index_within_node := u32(i) % LEAF_NODE_LEFT_SPLIT_COUNT
        destination := leaf_node_cell(destination_node, index_within_node)

        if u32(i) == cursor.cell_num {
            serialize_row(value, leaf_node_value(destination_node, index_within_node))
            leaf_node_key(destination_node, index_within_node)^ = key
        } else if u32(i) > cursor.cell_num {
            mem.copy(destination, leaf_node_cell(old_node, u32(i - 1)), LEAF_NODE_CELL_SIZE)
        } else {
            mem.copy(destination, leaf_node_cell(old_node, u32(i)), LEAF_NODE_CELL_SIZE)
        }
    }

    leaf_node_num_cells(old_node)^ = LEAF_NODE_LEFT_SPLIT_COUNT
    leaf_node_num_cells(new_node)^ = LEAF_NODE_RIGHT_SPLIT_COUNT

    if is_node_root(old_node) {
        create_new_root(cursor.table, new_page_num)
        return
    } else {
        parent_page_num := node_parent(old_node)^
        new_max := get_node_max_key(cursor.table.pager, old_node)
        parent := get_page(cursor.table.pager, parent_page_num)

        update_internal_node_key(parent, old_max, new_max)
        internal_node_insert(cursor.table, parent_page_num, new_page_num)
        return
    }
}

leaf_node_insert :: proc(cursor: ^Cursor, key: u32, value: ^Row) {
    node := get_page(cursor.table.pager, cursor.page_num)

    num_cells := leaf_node_num_cells(node)^
    if num_cells >= LEAF_NODE_MAX_CELLS {
        leaf_node_split_and_insert(cursor, key, value)
        return
    }

    if cursor.cell_num < num_cells {
        for i := num_cells; i > cursor.cell_num; i -= 1 {
            mem.copy(leaf_node_cell(node, i), leaf_node_cell(node, i - 1), LEAF_NODE_CELL_SIZE)
        }
    }

    leaf_node_num_cells(node)^ += 1
    leaf_node_key(node, cursor.cell_num)^ = key
    serialize_row(value, leaf_node_value(node, cursor.cell_num))
}

// ============================================================================
// Execution Engine
// ============================================================================

execute_insert :: proc(statement: ^Statement, table: ^Table) -> Execute_Result {
    row_to_insert := &statement.row_to_insert
    key_to_insert := row_to_insert.id
    cursor := table_find(table, key_to_insert)

    node := get_page(table.pager, cursor.page_num)
    num_cells := leaf_node_num_cells(node)^

    if cursor.cell_num < num_cells {
        key_at_index := leaf_node_key(node, cursor.cell_num)^
        if key_at_index == key_to_insert {
            free(cursor)
            return .EXECUTE_DUPLICATE_KEY
        }
    }

    leaf_node_insert(cursor, row_to_insert.id, row_to_insert)

    free(cursor)
    return .EXECUTE_SUCCESS
}

execute_select :: proc(statement: ^Statement, table: ^Table) -> Execute_Result {
    cursor := table_start(table)

    row: Row
    for !cursor.end_of_table {
        deserialize_row(cursor_value(cursor), &row)
        print_row(&row)
        cursor_advance(cursor)
    }

    free(cursor)
    return .EXECUTE_SUCCESS
}

execute_statement :: proc(statement: ^Statement, table: ^Table) -> Execute_Result {
    switch statement.type {
    case .STATEMENT_INSERT:
        return execute_insert(statement, table)
    case .STATEMENT_SELECT:
        return execute_select(statement, table)
    }
    return .EXECUTE_SUCCESS
}

// ============================================================================
// Main Shell Loop
// ============================================================================

main :: proc() {
    if len(os.args) < 2 {
        fmt.printf("Must supply a database filename.\n")
        os.exit(1)
    }

    filename := os.args[1]
    table := db_open(filename)

    input_buf: [1024]u8

    for {
        print_prompt()

        input_buffer, ok := read_line(input_buf[:])
        if !ok { break }

        input_buffer = strings.trim_space(input_buffer)
        if len(input_buffer) == 0 { continue }

        if input_buffer[0] == '.' {
            switch do_meta_command(input_buffer, table) {
            case .META_COMMAND_SUCCESS:
                continue
            case .META_COMMAND_UNRECOGNIZED_COMMAND:
                fmt.printf("Unrecognized command '%s'\n", input_buffer)
                continue
            }
        }

        statement: Statement
        switch prepare_statement(input_buffer, &statement) {
        case .PREPARE_SUCCESS:
            // ok
        case .PREPARE_NEGATIVE_ID:
            fmt.printf("ID must be positive.\n")
            continue
        case .PREPARE_STRING_TOO_LONG:
            fmt.printf("String is too long.\n")
            continue
        case .PREPARE_SYNTAX_ERROR:
            fmt.printf("Syntax error. Could not parse statement.\n")
            continue
        case .PREPARE_UNRECOGNIZED_STATEMENT:
            fmt.printf("Unrecognized keyword at start of '%s'.\n", input_buffer)
            continue
        }

        switch execute_statement(&statement, table) {
        case .EXECUTE_SUCCESS:
            fmt.printf("Executed.\n")
        case .EXECUTE_DUPLICATE_KEY:
            fmt.printf("Error: Duplicate key.\n")
        }
    }

    db_close(table)
}
