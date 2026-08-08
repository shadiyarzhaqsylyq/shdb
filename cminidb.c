/*
  db.c — a tiny database engine.
 
   - WHERE clauses on SELECT/DELETE/UPDATE (id/username/email, =, !=, <, >, <=, >=)
   - UPDATE <id> <username> <email>              (point update, full row)
   - UPDATE SET col = val [...] [WHERE ...]      (SET + optional WHERE)
   - DELETE <id>                                 (point delete)
   - DELETE WHERE <col> <op> <val>               (bulk delete)
   - SELECT COUNT [WHERE ...]                    (aggregate)
   - CREATE TABLE ...                            (accepted; schema is fixed)
   - BEGIN / COMMIT / ROLLBACK                   (in-memory snapshot transactions)
   - .help meta command

   - No node merging on delete underflow (rows are removed, but a leaf
     that empties out isn't rebalanced back into its siblings). Fine for
     a tutorial engine; a "real" engine would coalesce nodes.
     WHERE is evaluated with a full table scan, since there's only a
     primary-key B+tree index — there's no secondary index to plan
     around. The point lookups (single UPDATE/DELETE by id) do still use
     the B+tree via table_find(), so those stay O(log n).
     Transactions are all-in-memory: BEGIN snapshots whichever pages are
     currently cached, ROLLBACK restores from that snapshot (and evicts
     any pages that were first loaded during the transaction, so they
     re-read clean from disk), COMMIT discards the snapshot and flushes
     dirty pages to disk immediately (instead of waiting for .exit).
*/



#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
  char* buffer;
  size_t buffer_length;
  ssize_t input_length;
} InputBuffer;

typedef enum {
  EXECUTE_SUCCESS,
  EXECUTE_DUPLICATE_KEY,
  EXECUTE_NOT_FOUND,          // UPDATE/DELETE by id found nothing
  EXECUTE_TX_ALREADY_ACTIVE,  // nested BEGIN
  EXECUTE_NO_ACTIVE_TX        // COMMIT/ROLLBACK with no BEGIN
} ExecuteResult;

typedef enum {
  META_COMMAND_SUCCESS,
  META_COMMAND_UNRECOGNIZED_COMMAND
} MetaCommandResult;

typedef enum {
  PREPARE_SUCCESS,
  PREPARE_NEGATIVE_ID,
  PREPARE_STRING_TOO_LONG,
  PREPARE_SYNTAX_ERROR,
  PREPARE_UNRECOGNIZED_STATEMENT
} PrepareResult;

typedef enum {
  STATEMENT_INSERT,
  STATEMENT_SELECT,
  STATEMENT_UPDATE,
  STATEMENT_DELETE,
  STATEMENT_CREATE,    // NEW: CREATE TABLE (no-op; schema fixed)
  STATEMENT_BEGIN,
  STATEMENT_COMMIT,
  STATEMENT_ROLLBACK
} StatementType;

// NEW: comparison operators for WHERE clauses
typedef enum {
  WHERE_EQ,
  WHERE_NE,
  WHERE_GT,
  WHERE_LT,
  WHERE_GE,
  WHERE_LE
} WhereOp;

#define COLUMN_USERNAME_SIZE 32
#define COLUMN_EMAIL_SIZE 255

typedef struct {
  uint32_t id;
  char username[COLUMN_USERNAME_SIZE + 1];
  char email[COLUMN_EMAIL_SIZE + 1];
} Row;

typedef struct {
  StatementType type;
  Row row_to_insert;  // used by INSERT and point-UPDATE (new values)

  // WHERE clause, shared by SELECT, bulk DELETE, and SET-UPDATE
  bool has_where;
  char where_column[16];       // "id" | "username" | "email"
  bool where_is_string;
  WhereOp where_op;
  uint32_t where_int_value;
  char where_str_value[COLUMN_EMAIL_SIZE + 1];

  bool is_count;       // SELECT COUNT [...]
  uint32_t target_id;  // id for point UPDATE/DELETE

  // SET-style UPDATE: which columns to change + their new values
  bool is_set_update;
  bool set_username;
  bool set_email;
  char set_username_value[COLUMN_USERNAME_SIZE + 1];
  char set_email_value[COLUMN_EMAIL_SIZE + 1];
} Statement;

#define size_of_attribute(Struct, Attribute) sizeof(((Struct*)0)->Attribute)

const uint32_t ID_SIZE = size_of_attribute(Row, id);
const uint32_t USERNAME_SIZE = size_of_attribute(Row, username);
const uint32_t EMAIL_SIZE = size_of_attribute(Row, email);
const uint32_t ID_OFFSET = 0;
const uint32_t USERNAME_OFFSET = ID_OFFSET + ID_SIZE;
const uint32_t EMAIL_OFFSET = USERNAME_OFFSET + USERNAME_SIZE;
const uint32_t ROW_SIZE = ID_SIZE + USERNAME_SIZE + EMAIL_SIZE;

const uint32_t PAGE_SIZE = 4096;
#define TABLE_MAX_PAGES 400
#define INVALID_PAGE_NUM UINT32_MAX

typedef struct {
  int file_descriptor;
  uint32_t file_length;
  uint32_t num_pages;
  void* pages[TABLE_MAX_PAGES];
} Pager;

typedef struct {
  Pager* pager;
  uint32_t root_page_num;

  // NEW: transaction state
  bool in_transaction;
  uint32_t tx_original_num_pages;
  void* tx_backup[TABLE_MAX_PAGES];
  bool tx_was_cached[TABLE_MAX_PAGES];
} Table;

typedef struct {
  Table* table;
  uint32_t page_num;
  uint32_t cell_num;
  bool end_of_table;
} Cursor;

void print_row(Row* row) {
  printf("(%d, %s, %s)\n", row->id, row->username, row->email);
}

typedef enum { NODE_INTERNAL, NODE_LEAF } NodeType;

/* Common Node Header Layout */
const uint32_t NODE_TYPE_SIZE = sizeof(uint8_t);
const uint32_t NODE_TYPE_OFFSET = 0;
const uint32_t IS_ROOT_SIZE = sizeof(uint8_t);
const uint32_t IS_ROOT_OFFSET = NODE_TYPE_SIZE;
const uint32_t PARENT_POINTER_SIZE = sizeof(uint32_t);
const uint32_t PARENT_POINTER_OFFSET = IS_ROOT_OFFSET + IS_ROOT_SIZE;
const uint8_t COMMON_NODE_HEADER_SIZE =
    NODE_TYPE_SIZE + IS_ROOT_SIZE + PARENT_POINTER_SIZE;

/* Internal Node Header Layout */
const uint32_t INTERNAL_NODE_NUM_KEYS_SIZE = sizeof(uint32_t);
const uint32_t INTERNAL_NODE_NUM_KEYS_OFFSET = COMMON_NODE_HEADER_SIZE;
const uint32_t INTERNAL_NODE_RIGHT_CHILD_SIZE = sizeof(uint32_t);
const uint32_t INTERNAL_NODE_RIGHT_CHILD_OFFSET =
    INTERNAL_NODE_NUM_KEYS_OFFSET + INTERNAL_NODE_NUM_KEYS_SIZE;
const uint32_t INTERNAL_NODE_HEADER_SIZE = COMMON_NODE_HEADER_SIZE +
                                            INTERNAL_NODE_NUM_KEYS_SIZE +
                                            INTERNAL_NODE_RIGHT_CHILD_SIZE;

/* Internal Node Body Layout */
const uint32_t INTERNAL_NODE_KEY_SIZE = sizeof(uint32_t);
const uint32_t INTERNAL_NODE_CHILD_SIZE = sizeof(uint32_t);
const uint32_t INTERNAL_NODE_CELL_SIZE =
    INTERNAL_NODE_CHILD_SIZE + INTERNAL_NODE_KEY_SIZE;
const uint32_t INTERNAL_NODE_MAX_KEYS = 3;  // kept small for easy testing

/* Leaf Node Header Layout */
const uint32_t LEAF_NODE_NUM_CELLS_SIZE = sizeof(uint32_t);
const uint32_t LEAF_NODE_NUM_CELLS_OFFSET = COMMON_NODE_HEADER_SIZE;
const uint32_t LEAF_NODE_NEXT_LEAF_SIZE = sizeof(uint32_t);
const uint32_t LEAF_NODE_NEXT_LEAF_OFFSET =
    LEAF_NODE_NUM_CELLS_OFFSET + LEAF_NODE_NUM_CELLS_SIZE;
const uint32_t LEAF_NODE_HEADER_SIZE = COMMON_NODE_HEADER_SIZE +
                                        LEAF_NODE_NUM_CELLS_SIZE +
                                        LEAF_NODE_NEXT_LEAF_SIZE;

/* Leaf Node Body Layout */
const uint32_t LEAF_NODE_KEY_SIZE = sizeof(uint32_t);
const uint32_t LEAF_NODE_KEY_OFFSET = 0;
const uint32_t LEAF_NODE_VALUE_SIZE = ROW_SIZE;
const uint32_t LEAF_NODE_VALUE_OFFSET =
    LEAF_NODE_KEY_OFFSET + LEAF_NODE_KEY_SIZE;
const uint32_t LEAF_NODE_CELL_SIZE = LEAF_NODE_KEY_SIZE + LEAF_NODE_VALUE_SIZE;
const uint32_t LEAF_NODE_SPACE_FOR_CELLS = PAGE_SIZE - LEAF_NODE_HEADER_SIZE;
const uint32_t LEAF_NODE_MAX_CELLS =
    LEAF_NODE_SPACE_FOR_CELLS / LEAF_NODE_CELL_SIZE;
const uint32_t LEAF_NODE_RIGHT_SPLIT_COUNT = (LEAF_NODE_MAX_CELLS + 1) / 2;
const uint32_t LEAF_NODE_LEFT_SPLIT_COUNT =
    (LEAF_NODE_MAX_CELLS + 1) - LEAF_NODE_RIGHT_SPLIT_COUNT;

NodeType get_node_type(void* node) {
  uint8_t value = *((uint8_t*)(node + NODE_TYPE_OFFSET));
  return (NodeType)value;
}

void set_node_type(void* node, NodeType type) {
  uint8_t value = type;
  *((uint8_t*)(node + NODE_TYPE_OFFSET)) = value;
}

bool is_node_root(void* node) {
  uint8_t value = *((uint8_t*)(node + IS_ROOT_OFFSET));
  return (bool)value;
}

void set_node_root(void* node, bool is_root) {
  uint8_t value = is_root;
  *((uint8_t*)(node + IS_ROOT_OFFSET)) = value;
}

uint32_t* node_parent(void* node) { return node + PARENT_POINTER_OFFSET; }

uint32_t* internal_node_num_keys(void* node) {
  return node + INTERNAL_NODE_NUM_KEYS_OFFSET;
}

uint32_t* internal_node_right_child(void* node) {
  return node + INTERNAL_NODE_RIGHT_CHILD_OFFSET;
}

uint32_t* internal_node_cell(void* node, uint32_t cell_num) {
  return node + INTERNAL_NODE_HEADER_SIZE + cell_num * INTERNAL_NODE_CELL_SIZE;
}

uint32_t* internal_node_child(void* node, uint32_t child_num) {
  uint32_t num_keys = *internal_node_num_keys(node);
  if (child_num > num_keys) {
    printf("Tried to access child_num %d > num_keys %d\n", child_num, num_keys);
    exit(EXIT_FAILURE);
  } else if (child_num == num_keys) {
    uint32_t* right_child = internal_node_right_child(node);
    if (*right_child == INVALID_PAGE_NUM) {
      printf("Tried to access right child of node, but was invalid page\n");
      exit(EXIT_FAILURE);
    }
    return right_child;
  } else {
    uint32_t* child = internal_node_cell(node, child_num);
    if (*child == INVALID_PAGE_NUM) {
      printf("Tried to access child %d of node, but was invalid page\n", child_num);
      exit(EXIT_FAILURE);
    }
    return child;
  }
}

uint32_t* internal_node_key(void* node, uint32_t key_num) {
  return (void*)internal_node_cell(node, key_num) + INTERNAL_NODE_CHILD_SIZE;
}

uint32_t* leaf_node_num_cells(void* node) {
  return node + LEAF_NODE_NUM_CELLS_OFFSET;
}

uint32_t* leaf_node_next_leaf(void* node) {
  return node + LEAF_NODE_NEXT_LEAF_OFFSET;
}

void* leaf_node_cell(void* node, uint32_t cell_num) {
  return node + LEAF_NODE_HEADER_SIZE + cell_num * LEAF_NODE_CELL_SIZE;
}

uint32_t* leaf_node_key(void* node, uint32_t cell_num) {
  return leaf_node_cell(node, cell_num);
}

void* leaf_node_value(void* node, uint32_t cell_num) {
  return leaf_node_cell(node, cell_num) + LEAF_NODE_KEY_SIZE;
}

void* get_page(Pager* pager, uint32_t page_num) {
  if (page_num > TABLE_MAX_PAGES) {
    printf("Tried to fetch page number out of bounds. %d > %d\n", page_num,
           TABLE_MAX_PAGES);
    exit(EXIT_FAILURE);
  }

  if (pager->pages[page_num] == NULL) {
    void* page = malloc(PAGE_SIZE);
    uint32_t num_pages = pager->file_length / PAGE_SIZE;
    if (pager->file_length % PAGE_SIZE) {
      num_pages += 1;
    }
    if (page_num <= num_pages) {
      lseek(pager->file_descriptor, page_num * PAGE_SIZE, SEEK_SET);
      ssize_t bytes_read = read(pager->file_descriptor, page, PAGE_SIZE);
      if (bytes_read == -1) {
        printf("Error reading file: %d\n", errno);
        exit(EXIT_FAILURE);
      }
    }
    pager->pages[page_num] = page;
    if (page_num >= pager->num_pages) {
      pager->num_pages = page_num + 1;
    }
  }
  return pager->pages[page_num];
}

uint32_t get_node_max_key(Pager* pager, void* node) {
  if (get_node_type(node) == NODE_LEAF) {
    return *leaf_node_key(node, *leaf_node_num_cells(node) - 1);
  }
  void* right_child = get_page(pager, *internal_node_right_child(node));
  return get_node_max_key(pager, right_child);
}

void print_constants() {
  printf("ROW_SIZE: %d\n", ROW_SIZE);
  printf("COMMON_NODE_HEADER_SIZE: %d\n", COMMON_NODE_HEADER_SIZE);
  printf("LEAF_NODE_HEADER_SIZE: %d\n", LEAF_NODE_HEADER_SIZE);
  printf("LEAF_NODE_CELL_SIZE: %d\n", LEAF_NODE_CELL_SIZE);
  printf("LEAF_NODE_SPACE_FOR_CELLS: %d\n", LEAF_NODE_SPACE_FOR_CELLS);
  printf("LEAF_NODE_MAX_CELLS: %d\n", LEAF_NODE_MAX_CELLS);
}

void indent(uint32_t level) {
  for (uint32_t i = 0; i < level; i++) printf("  ");
}

void print_tree(Pager* pager, uint32_t page_num, uint32_t indentation_level) {
  void* node = get_page(pager, page_num);
  uint32_t num_keys, child;

  switch (get_node_type(node)) {
    case (NODE_LEAF):
      num_keys = *leaf_node_num_cells(node);
      indent(indentation_level);
      printf("- leaf (size %d)\n", num_keys);
      for (uint32_t i = 0; i < num_keys; i++) {
        indent(indentation_level + 1);
        printf("- %d\n", *leaf_node_key(node, i));
      }
      break;
    case (NODE_INTERNAL):
      num_keys = *internal_node_num_keys(node);
      indent(indentation_level);
      printf("- internal (size %d)\n", num_keys);
      if (num_keys > 0) {
        for (uint32_t i = 0; i < num_keys; i++) {
          child = *internal_node_child(node, i);
          print_tree(pager, child, indentation_level + 1);
          indent(indentation_level + 1);
          printf("- key %d\n", *internal_node_key(node, i));
        }
        child = *internal_node_right_child(node);
        print_tree(pager, child, indentation_level + 1);
      }
      break;
  }
}

void serialize_row(Row* source, void* destination) {
  memcpy(destination + ID_OFFSET, &(source->id), ID_SIZE);
  memcpy(destination + USERNAME_OFFSET, &(source->username), USERNAME_SIZE);
  memcpy(destination + EMAIL_OFFSET, &(source->email), EMAIL_SIZE);
}

void deserialize_row(void* source, Row* destination) {
  memcpy(&(destination->id), source + ID_OFFSET, ID_SIZE);
  memcpy(&(destination->username), source + USERNAME_OFFSET, USERNAME_SIZE);
  memcpy(&(destination->email), source + EMAIL_OFFSET, EMAIL_SIZE);
}

void initialize_leaf_node(void* node) {
  set_node_type(node, NODE_LEAF);
  set_node_root(node, false);
  *leaf_node_num_cells(node) = 0;
  *leaf_node_next_leaf(node) = 0;
}

void initialize_internal_node(void* node) {
  set_node_type(node, NODE_INTERNAL);
  set_node_root(node, false);
  *internal_node_num_keys(node) = 0;
  *internal_node_right_child(node) = INVALID_PAGE_NUM;
}

Cursor* leaf_node_find(Table* table, uint32_t page_num, uint32_t key) {
  void* node = get_page(table->pager, page_num);
  uint32_t num_cells = *leaf_node_num_cells(node);

  Cursor* cursor = malloc(sizeof(Cursor));
  cursor->table = table;
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

Cursor* internal_node_find(Table* table, uint32_t page_num, uint32_t key) {
  void* node = get_page(table->pager, page_num);
  uint32_t child_index = internal_node_find_child(node, key);
  uint32_t child_num = *internal_node_child(node, child_index);
  void* child = get_page(table->pager, child_num);
  switch (get_node_type(child)) {
    case NODE_LEAF:
      return leaf_node_find(table, child_num, key);
    case NODE_INTERNAL:
      return internal_node_find(table, child_num, key);
  }
}

Cursor* table_find(Table* table, uint32_t key) {
  uint32_t root_page_num = table->root_page_num;
  void* root_node = get_page(table->pager, root_page_num);
  if (get_node_type(root_node) == NODE_LEAF) {
    return leaf_node_find(table, root_page_num, key);
  } else {
    return internal_node_find(table, root_page_num, key);
  }
}

Cursor* table_start(Table* table) {
  Cursor* cursor = table_find(table, 0);
  void* node = get_page(table->pager, cursor->page_num);
  uint32_t num_cells = *leaf_node_num_cells(node);
  cursor->end_of_table = (num_cells == 0);
  return cursor;
}

void* cursor_value(Cursor* cursor) {
  uint32_t page_num = cursor->page_num;
  void* page = get_page(cursor->table->pager, page_num);
  return leaf_node_value(page, cursor->cell_num);
}

void cursor_advance(Cursor* cursor) {
  uint32_t page_num = cursor->page_num;
  void* node = get_page(cursor->table->pager, page_num);
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

// NEW: remove a cell from a leaf node, shifting everything after it left.
// (No underflow handling / node merging — see file header notes.)
void leaf_node_delete(Cursor* cursor) {
  void* node = get_page(cursor->table->pager, cursor->page_num);
  uint32_t num_cells = *leaf_node_num_cells(node);
  for (uint32_t i = cursor->cell_num; i < num_cells - 1; i++) {
    memcpy(leaf_node_cell(node, i), leaf_node_cell(node, i + 1),
           LEAF_NODE_CELL_SIZE);
  }
  *(leaf_node_num_cells(node)) -= 1;
}

Pager* pager_open(const char* filename) {
  int fd = open(filename, O_RDWR | O_CREAT, S_IWUSR | S_IRUSR);
  if (fd == -1) {
    printf("Unable to open file\n");
    exit(EXIT_FAILURE);
  }

  off_t file_length = lseek(fd, 0, SEEK_END);

  Pager* pager = malloc(sizeof(Pager));
  pager->file_descriptor = fd;
  pager->file_length = file_length;
  pager->num_pages = (file_length / PAGE_SIZE);

  if (file_length % PAGE_SIZE != 0) {
    printf("Db file is not a whole number of pages. Corrupt file.\n");
    exit(EXIT_FAILURE);
  }

  for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
    pager->pages[i] = NULL;
  }
  return pager;
}

Table* db_open(const char* filename) {
  Pager* pager = pager_open(filename);

  Table* table = malloc(sizeof(Table));
  table->pager = pager;
  table->root_page_num = 0;
  table->in_transaction = false;              // NEW
  table->tx_original_num_pages = 0;            // NEW
  for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
    table->tx_backup[i] = NULL;                // NEW
    table->tx_was_cached[i] = false;           // NEW
  }

  if (pager->num_pages == 0) {
    void* root_node = get_page(pager, 0);
    initialize_leaf_node(root_node);
    set_node_root(root_node, true);
  }

  return table;
}

InputBuffer* new_input_buffer() {
  InputBuffer* input_buffer = malloc(sizeof(InputBuffer));
  input_buffer->buffer = NULL;
  input_buffer->buffer_length = 0;
  input_buffer->input_length = 0;
  return input_buffer;
}

void print_prompt() { printf("db > "); }

void read_input(InputBuffer* input_buffer) {
  ssize_t bytes_read =
      getline(&(input_buffer->buffer), &(input_buffer->buffer_length), stdin);
  if (bytes_read <= 0) {
    printf("Error reading input\n");
    exit(EXIT_FAILURE);
  }
  input_buffer->input_length = bytes_read - 1;
  input_buffer->buffer[bytes_read - 1] = 0;
}

void close_input_buffer(InputBuffer* input_buffer) {
  free(input_buffer->buffer);
  free(input_buffer);
}

void pager_flush(Pager* pager, uint32_t page_num) {
  if (pager->pages[page_num] == NULL) {
    printf("Tried to flush null page\n");
    exit(EXIT_FAILURE);
  }
  off_t offset = lseek(pager->file_descriptor, page_num * PAGE_SIZE, SEEK_SET);
  if (offset == -1) {
    printf("Error seeking: %d\n", errno);
    exit(EXIT_FAILURE);
  }
  ssize_t bytes_written =
      write(pager->file_descriptor, pager->pages[page_num], PAGE_SIZE);
  if (bytes_written == -1) {
    printf("Error writing: %d\n", errno);
    exit(EXIT_FAILURE);
  }
}

// NEW: free any transaction backup buffers still held by the table.
void tx_free_backups(Table* table) {
  for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
    if (table->tx_backup[i] != NULL) {
      free(table->tx_backup[i]);
      table->tx_backup[i] = NULL;
    }
  }
}

void db_close(Table* table) {
  Pager* pager = table->pager;

  // If the process exits mid-transaction, don't silently commit partial
  // work — flush what's on disk as-is (already-committed state) and drop
  // the in-memory snapshot.
  tx_free_backups(table);  // NEW

  for (uint32_t i = 0; i < pager->num_pages; i++) {
    if (pager->pages[i] == NULL) continue;
    pager_flush(pager, i);
    free(pager->pages[i]);
    pager->pages[i] = NULL;
  }

  int result = close(pager->file_descriptor);
  if (result == -1) {
    printf("Error closing db file.\n");
    exit(EXIT_FAILURE);
  }
  for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
    void* page = pager->pages[i];
    if (page) {
      free(page);
      pager->pages[i] = NULL;
    }
  }
  free(pager);
  free(table);
}

void print_help() {
  printf("Meta commands:\n");
  printf("  .exit               close the database and quit\n");
  printf("  .btree              print the B+tree structure\n");
  printf("  .constants          print internal size constants\n");
  printf("  .help               show this message\n");
  printf("Statements:\n");
  printf("  create table ...                 (accepted; schema is fixed)\n");
  printf("  insert <id> <username> <email>\n");
  printf("  select [count] [where <col> <op> <val>]\n");
  printf("    col: id | username | email   op: = != > < >= <=\n");
  printf("  update <id> <username> <email>   (point update, full row)\n");
  printf("  update set <col> = <val> [...] [where <col> <op> <val>]\n");
  printf("    col for SET: username | email\n");
  printf("  delete <id>\n");
  printf("  delete where <col> <op> <val>\n");
  printf("  begin | commit | rollback\n");
}

MetaCommandResult do_meta_command(InputBuffer* input_buffer, Table* table) {
  if (strcmp(input_buffer->buffer, ".exit") == 0) {
    close_input_buffer(input_buffer);
    db_close(table);
    exit(EXIT_SUCCESS);
  } else if (strcmp(input_buffer->buffer, ".btree") == 0) {
    printf("Tree:\n");
    print_tree(table->pager, 0, 0);
    return META_COMMAND_SUCCESS;
  } else if (strcmp(input_buffer->buffer, ".constants") == 0) {
    printf("Constants:\n");
    print_constants();
    return META_COMMAND_SUCCESS;
  } else if (strcmp(input_buffer->buffer, ".help") == 0) {  // NEW
    print_help();
    return META_COMMAND_SUCCESS;
  } else {
    return META_COMMAND_UNRECOGNIZED_COMMAND;
  }
}

// NEW: shared WHERE-clause parser. Expects "where" to already be consumed;
// pulls <column> <op> <value> off the same strtok stream as the caller.
PrepareResult parse_where_clause(Statement* statement) {
  char* column = strtok(NULL, " ");
  char* op_str = strtok(NULL, " ");
  char* value = strtok(NULL, " ");
  if (column == NULL || op_str == NULL || value == NULL) {
    return PREPARE_SYNTAX_ERROR;
  }
  if (strcmp(column, "id") != 0 && strcmp(column, "username") != 0 &&
      strcmp(column, "email") != 0) {
    return PREPARE_SYNTAX_ERROR;
  }
  strncpy(statement->where_column, column, sizeof(statement->where_column) - 1);
  statement->where_column[sizeof(statement->where_column) - 1] = '\0';

  if (strcmp(op_str, "=") == 0) statement->where_op = WHERE_EQ;
  else if (strcmp(op_str, "!=") == 0) statement->where_op = WHERE_NE;
  else if (strcmp(op_str, ">=") == 0) statement->where_op = WHERE_GE;
  else if (strcmp(op_str, "<=") == 0) statement->where_op = WHERE_LE;
  else if (strcmp(op_str, ">") == 0) statement->where_op = WHERE_GT;
  else if (strcmp(op_str, "<") == 0) statement->where_op = WHERE_LT;
  else return PREPARE_SYNTAX_ERROR;

  if (strcmp(column, "id") == 0) {
    statement->where_is_string = false;
    statement->where_int_value = (uint32_t)atoi(value);
  } else {
    statement->where_is_string = true;
    strncpy(statement->where_str_value, value,
            sizeof(statement->where_str_value) - 1);
    statement->where_str_value[sizeof(statement->where_str_value) - 1] = '\0';
  }
  return PREPARE_SUCCESS;
}

// NEW: does a deserialized row satisfy the statement's WHERE clause?
bool row_matches_where(Row* row, Statement* statement) {
  if (!statement->has_where) return true;

  int cmp;
  if (strcmp(statement->where_column, "id") == 0) {
    uint32_t v = statement->where_int_value;
    cmp = (row->id > v) - (row->id < v);
  } else if (strcmp(statement->where_column, "username") == 0) {
    int c = strcmp(row->username, statement->where_str_value);
    cmp = (c > 0) - (c < 0);
  } else {
    int c = strcmp(row->email, statement->where_str_value);
    cmp = (c > 0) - (c < 0);
  }

  switch (statement->where_op) {
    case WHERE_EQ: return cmp == 0;
    case WHERE_NE: return cmp != 0;
    case WHERE_GT: return cmp > 0;
    case WHERE_LT: return cmp < 0;
    case WHERE_GE: return cmp >= 0;
    case WHERE_LE: return cmp <= 0;
  }
  return false;
}

PrepareResult prepare_insert(InputBuffer* input_buffer, Statement* statement) {
  statement->type = STATEMENT_INSERT;

  char* keyword = strtok(input_buffer->buffer, " ");
  char* id_string = strtok(NULL, " ");
  char* username = strtok(NULL, " ");
  char* email = strtok(NULL, " ");

  if (id_string == NULL || username == NULL || email == NULL) {
    return PREPARE_SYNTAX_ERROR;
  }
  int id = atoi(id_string);
  if (id < 0) return PREPARE_NEGATIVE_ID;
  if (strlen(username) > COLUMN_USERNAME_SIZE) return PREPARE_STRING_TOO_LONG;
  if (strlen(email) > COLUMN_EMAIL_SIZE) return PREPARE_STRING_TOO_LONG;

  statement->row_to_insert.id = id;
  strcpy(statement->row_to_insert.username, username);
  strcpy(statement->row_to_insert.email, email);
  return PREPARE_SUCCESS;
}

// Point form:  update <id> <username> <email>
// SET form:    update set <col> = <val> [<col> = <val> ...] [where <col> <op> <val>]
PrepareResult prepare_update(InputBuffer* input_buffer, Statement* statement) {
  statement->type = STATEMENT_UPDATE;
  statement->has_where = false;
  statement->is_set_update = false;
  statement->set_username = false;
  statement->set_email = false;

  char* keyword = strtok(input_buffer->buffer, " ");
  char* next = strtok(NULL, " ");
  if (next == NULL) return PREPARE_SYNTAX_ERROR;

  // --- UPDATE SET ---
  if (strcmp(next, "set") == 0) {
    statement->is_set_update = true;
    while (true) {
      char* col = strtok(NULL, " ");
      if (col == NULL) break;
      if (strcmp(col, "where") == 0) {
        PrepareResult result = parse_where_clause(statement);
        if (result != PREPARE_SUCCESS) return result;
        statement->has_where = true;
        break;
      }
      char* eq = strtok(NULL, " ");
      char* val = strtok(NULL, " ");
      if (eq == NULL || val == NULL || strcmp(eq, "=") != 0) {
        return PREPARE_SYNTAX_ERROR;
      }
      if (strcmp(col, "username") == 0) {
        if (strlen(val) > COLUMN_USERNAME_SIZE) return PREPARE_STRING_TOO_LONG;
        statement->set_username = true;
        strncpy(statement->set_username_value, val,
                sizeof(statement->set_username_value) - 1);
        statement->set_username_value[sizeof(statement->set_username_value) - 1] =
            '\0';
      } else if (strcmp(col, "email") == 0) {
        if (strlen(val) > COLUMN_EMAIL_SIZE) return PREPARE_STRING_TOO_LONG;
        statement->set_email = true;
        strncpy(statement->set_email_value, val,
                sizeof(statement->set_email_value) - 1);
        statement->set_email_value[sizeof(statement->set_email_value) - 1] = '\0';
      } else {
        // cannot SET the primary key
        return PREPARE_SYNTAX_ERROR;
      }
    }
    if (!statement->set_username && !statement->set_email) {
      return PREPARE_SYNTAX_ERROR;  // must set at least one column
    }
    return PREPARE_SUCCESS;
  }

  // --- classic point form: update <id> <username> <email> ---
  char* id_string = next;
  char* username = strtok(NULL, " ");
  char* email = strtok(NULL, " ");

  if (id_string == NULL || username == NULL || email == NULL) {
    return PREPARE_SYNTAX_ERROR;
  }
  int id = atoi(id_string);
  if (id < 0) return PREPARE_NEGATIVE_ID;
  if (strlen(username) > COLUMN_USERNAME_SIZE) return PREPARE_STRING_TOO_LONG;
  if (strlen(email) > COLUMN_EMAIL_SIZE) return PREPARE_STRING_TOO_LONG;

  statement->row_to_insert.id = id;
  strcpy(statement->row_to_insert.username, username);
  strcpy(statement->row_to_insert.email, email);
  return PREPARE_SUCCESS;
}

// NEW
PrepareResult prepare_delete(InputBuffer* input_buffer, Statement* statement) {
  statement->type = STATEMENT_DELETE;
  statement->has_where = false;

  char* keyword = strtok(input_buffer->buffer, " ");
  char* next = strtok(NULL, " ");
  if (next == NULL) return PREPARE_SYNTAX_ERROR;

  if (strcmp(next, "where") == 0) {
    PrepareResult result = parse_where_clause(statement);
    if (result != PREPARE_SUCCESS) return result;
    statement->has_where = true;
    return PREPARE_SUCCESS;
  }

  int id = atoi(next);
  if (id < 0) return PREPARE_NEGATIVE_ID;
  statement->target_id = (uint32_t)id;
  return PREPARE_SUCCESS;
}

// NEW
PrepareResult prepare_select(InputBuffer* input_buffer, Statement* statement) {
  statement->type = STATEMENT_SELECT;
  statement->has_where = false;
  statement->is_count = false;

  char* keyword = strtok(input_buffer->buffer, " ");
  char* next = strtok(NULL, " ");
  if (next == NULL) return PREPARE_SUCCESS;

  if (strcmp(next, "count") == 0) {
    statement->is_count = true;
    next = strtok(NULL, " ");
    if (next == NULL) return PREPARE_SUCCESS;
  }

  if (strcmp(next, "where") != 0) return PREPARE_SYNTAX_ERROR;
  PrepareResult result = parse_where_clause(statement);
  if (result != PREPARE_SUCCESS) return result;
  statement->has_where = true;
  return PREPARE_SUCCESS;
}

PrepareResult prepare_statement(InputBuffer* input_buffer,
                                 Statement* statement) {
  if (strncmp(input_buffer->buffer, "insert", 6) == 0) {
    return prepare_insert(input_buffer, statement);
  }
  if (strncmp(input_buffer->buffer, "select", 6) == 0) {
    return prepare_select(input_buffer, statement);
  }
  if (strncmp(input_buffer->buffer, "update", 6) == 0) {
    return prepare_update(input_buffer, statement);
  }
  if (strncmp(input_buffer->buffer, "delete", 6) == 0) {
    return prepare_delete(input_buffer, statement);
  }
  if (strncmp(input_buffer->buffer, "create", 6) == 0) {
    // CREATE TABLE ... — schema is fixed (id, username, email).
    statement->type = STATEMENT_CREATE;
    return PREPARE_SUCCESS;
  }
  if (strcmp(input_buffer->buffer, "begin") == 0) {
    statement->type = STATEMENT_BEGIN;
    return PREPARE_SUCCESS;
  }
  if (strcmp(input_buffer->buffer, "commit") == 0) {
    statement->type = STATEMENT_COMMIT;
    return PREPARE_SUCCESS;
  }
  if (strcmp(input_buffer->buffer, "rollback") == 0) {
    statement->type = STATEMENT_ROLLBACK;
    return PREPARE_SUCCESS;
  }
  return PREPARE_UNRECOGNIZED_STATEMENT;
}

uint32_t get_unused_page_num(Pager* pager) { return pager->num_pages; }

void create_new_root(Table* table, uint32_t right_child_page_num) {
  void* root = get_page(table->pager, table->root_page_num);
  void* right_child = get_page(table->pager, right_child_page_num);
  uint32_t left_child_page_num = get_unused_page_num(table->pager);
  void* left_child = get_page(table->pager, left_child_page_num);

  if (get_node_type(root) == NODE_INTERNAL) {
    initialize_internal_node(right_child);
    initialize_internal_node(left_child);
  }

  memcpy(left_child, root, PAGE_SIZE);
  set_node_root(left_child, false);

  if (get_node_type(left_child) == NODE_INTERNAL) {
    void* child;
    for (int i = 0; i < *internal_node_num_keys(left_child); i++) {
      child = get_page(table->pager, *internal_node_child(left_child, i));
      *node_parent(child) = left_child_page_num;
    }
    child = get_page(table->pager, *internal_node_right_child(left_child));
    *node_parent(child) = left_child_page_num;
  }

  initialize_internal_node(root);
  set_node_root(root, true);
  *internal_node_num_keys(root) = 1;
  *internal_node_child(root, 0) = left_child_page_num;
  uint32_t left_child_max_key = get_node_max_key(table->pager, left_child);
  *internal_node_key(root, 0) = left_child_max_key;
  *internal_node_right_child(root) = right_child_page_num;
  *node_parent(left_child) = table->root_page_num;
  *node_parent(right_child) = table->root_page_num;
}

void internal_node_split_and_insert(Table* table, uint32_t parent_page_num,
                                     uint32_t child_page_num);

void internal_node_insert(Table* table, uint32_t parent_page_num,
                           uint32_t child_page_num) {
  void* parent = get_page(table->pager, parent_page_num);
  void* child = get_page(table->pager, child_page_num);
  uint32_t child_max_key = get_node_max_key(table->pager, child);
  uint32_t index = internal_node_find_child(parent, child_max_key);

  uint32_t original_num_keys = *internal_node_num_keys(parent);
  if (original_num_keys >= INTERNAL_NODE_MAX_KEYS) {
    internal_node_split_and_insert(table, parent_page_num, child_page_num);
    return;
  }

  uint32_t right_child_page_num = *internal_node_right_child(parent);
  if (right_child_page_num == INVALID_PAGE_NUM) {
    *internal_node_right_child(parent) = child_page_num;
    return;
  }

  void* right_child = get_page(table->pager, right_child_page_num);
  *internal_node_num_keys(parent) = original_num_keys + 1;

  if (child_max_key > get_node_max_key(table->pager, right_child)) {
    *internal_node_child(parent, original_num_keys) = right_child_page_num;
    *internal_node_key(parent, original_num_keys) =
        get_node_max_key(table->pager, right_child);
    *internal_node_right_child(parent) = child_page_num;
  } else {
    for (uint32_t i = original_num_keys; i > index; i--) {
      void* destination = internal_node_cell(parent, i);
      void* source = internal_node_cell(parent, i - 1);
      memcpy(destination, source, INTERNAL_NODE_CELL_SIZE);
    }
    *internal_node_child(parent, index) = child_page_num;
    *internal_node_key(parent, index) = child_max_key;
  }
}

void update_internal_node_key(void* node, uint32_t old_key, uint32_t new_key) {
  uint32_t old_child_index = internal_node_find_child(node, old_key);
  *internal_node_key(node, old_child_index) = new_key;
}

void internal_node_split_and_insert(Table* table, uint32_t parent_page_num,
                                     uint32_t child_page_num) {
  uint32_t old_page_num = parent_page_num;
  void* old_node = get_page(table->pager, parent_page_num);
  uint32_t old_max = get_node_max_key(table->pager, old_node);

  void* child = get_page(table->pager, child_page_num);
  uint32_t child_max = get_node_max_key(table->pager, child);

  uint32_t new_page_num = get_unused_page_num(table->pager);

  uint32_t splitting_root = is_node_root(old_node);
  void* parent;
  void* new_node;

  if (splitting_root) {
    create_new_root(table, new_page_num);
    parent = get_page(table->pager, table->root_page_num);
    old_page_num = *internal_node_child(parent, 0);
    old_node = get_page(table->pager, old_page_num);
  } else {
    parent = get_page(table->pager, *node_parent(old_node));
    new_node = get_page(table->pager, new_page_num);
    initialize_internal_node(new_node);
  }

  uint32_t* old_num_keys = internal_node_num_keys(old_node);
  uint32_t cur_page_num = *internal_node_right_child(old_node);
  void* cur = get_page(table->pager, cur_page_num);

  internal_node_insert(table, new_page_num, cur_page_num);
  *node_parent(cur) = new_page_num;
  *internal_node_right_child(old_node) = INVALID_PAGE_NUM;

  for (int i = INTERNAL_NODE_MAX_KEYS - 1; i > INTERNAL_NODE_MAX_KEYS / 2; i--) {
    cur_page_num = *internal_node_child(old_node, i);
    cur = get_page(table->pager, cur_page_num);
    internal_node_insert(table, new_page_num, cur_page_num);
    *node_parent(cur) = new_page_num;
    (*old_num_keys)--;
  }

  *internal_node_right_child(old_node) =
      *internal_node_child(old_node, *old_num_keys - 1);
  (*old_num_keys)--;

  uint32_t max_after_split = get_node_max_key(table->pager, old_node);
  uint32_t destination_page_num =
      child_max < max_after_split ? old_page_num : new_page_num;

  internal_node_insert(table, destination_page_num, child_page_num);
  *node_parent(child) = destination_page_num;

  update_internal_node_key(parent, old_max,
                            get_node_max_key(table->pager, old_node));

  if (!splitting_root) {
    internal_node_insert(table, *node_parent(old_node), new_page_num);
    *node_parent(new_node) = *node_parent(old_node);
  }
}

void leaf_node_split_and_insert(Cursor* cursor, uint32_t key, Row* value) {
  void* old_node = get_page(cursor->table->pager, cursor->page_num);
  uint32_t old_max = get_node_max_key(cursor->table->pager, old_node);
  uint32_t new_page_num = get_unused_page_num(cursor->table->pager);
  void* new_node = get_page(cursor->table->pager, new_page_num);
  initialize_leaf_node(new_node);
  *node_parent(new_node) = *node_parent(old_node);
  *leaf_node_next_leaf(new_node) = *leaf_node_next_leaf(old_node);
  *leaf_node_next_leaf(old_node) = new_page_num;

  for (int32_t i = LEAF_NODE_MAX_CELLS; i >= 0; i--) {
    void* destination_node;
    if (i >= LEAF_NODE_LEFT_SPLIT_COUNT) {
      destination_node = new_node;
    } else {
      destination_node = old_node;
    }
    uint32_t index_within_node = i % LEAF_NODE_LEFT_SPLIT_COUNT;
    void* destination = leaf_node_cell(destination_node, index_within_node);

    if (i == cursor->cell_num) {
      serialize_row(value, leaf_node_value(destination_node, index_within_node));
      *leaf_node_key(destination_node, index_within_node) = key;
    } else if (i > cursor->cell_num) {
      memcpy(destination, leaf_node_cell(old_node, i - 1), LEAF_NODE_CELL_SIZE);
    } else {
      memcpy(destination, leaf_node_cell(old_node, i), LEAF_NODE_CELL_SIZE);
    }
  }

  *(leaf_node_num_cells(old_node)) = LEAF_NODE_LEFT_SPLIT_COUNT;
  *(leaf_node_num_cells(new_node)) = LEAF_NODE_RIGHT_SPLIT_COUNT;

  if (is_node_root(old_node)) {
    return create_new_root(cursor->table, new_page_num);
  } else {
    uint32_t parent_page_num = *node_parent(old_node);
    uint32_t new_max = get_node_max_key(cursor->table->pager, old_node);
    void* parent = get_page(cursor->table->pager, parent_page_num);
    update_internal_node_key(parent, old_max, new_max);
    internal_node_insert(cursor->table, parent_page_num, new_page_num);
    return;
  }
}

void leaf_node_insert(Cursor* cursor, uint32_t key, Row* value) {
  void* node = get_page(cursor->table->pager, cursor->page_num);
  uint32_t num_cells = *leaf_node_num_cells(node);
  if (num_cells >= LEAF_NODE_MAX_CELLS) {
    leaf_node_split_and_insert(cursor, key, value);
    return;
  }
  if (cursor->cell_num < num_cells) {
    for (uint32_t i = num_cells; i > cursor->cell_num; i--) {
      memcpy(leaf_node_cell(node, i), leaf_node_cell(node, i - 1),
             LEAF_NODE_CELL_SIZE);
    }
  }
  *(leaf_node_num_cells(node)) += 1;
  *(leaf_node_key(node, cursor->cell_num)) = key;
  serialize_row(value, leaf_node_value(node, cursor->cell_num));
}

ExecuteResult execute_insert(Statement* statement, Table* table) {
  Row* row_to_insert = &(statement->row_to_insert);
  uint32_t key_to_insert = row_to_insert->id;
  Cursor* cursor = table_find(table, key_to_insert);

  void* node = get_page(table->pager, cursor->page_num);
  uint32_t num_cells = *leaf_node_num_cells(node);
  if (cursor->cell_num < num_cells) {
    uint32_t key_at_index = *leaf_node_key(node, cursor->cell_num);
    if (key_at_index == key_to_insert) {
      free(cursor);
      return EXECUTE_DUPLICATE_KEY;
    }
  }

  leaf_node_insert(cursor, row_to_insert->id, row_to_insert);
  free(cursor);
  return EXECUTE_SUCCESS;
}

// Point update (by id) or SET-style update (optional WHERE, full scan).
ExecuteResult execute_update(Statement* statement, Table* table) {
  // --- classic point form ---
  if (!statement->is_set_update) {
    uint32_t id = statement->row_to_insert.id;
    Cursor* cursor = table_find(table, id);
    void* node = get_page(table->pager, cursor->page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);

    if (cursor->cell_num >= num_cells ||
        *leaf_node_key(node, cursor->cell_num) != id) {
      free(cursor);
      return EXECUTE_NOT_FOUND;
    }

    serialize_row(&statement->row_to_insert,
                  leaf_node_value(node, cursor->cell_num));
    free(cursor);
    printf("1 row updated.\n");
    return EXECUTE_SUCCESS;
  }

  // --- SET form: full scan + optional WHERE, update matching rows in place ---
  Cursor* cursor = table_start(table);
  Row row;
  uint32_t count = 0;

  while (!(cursor->end_of_table)) {
    deserialize_row(cursor_value(cursor), &row);
    if (row_matches_where(&row, statement)) {
      if (statement->set_username) {
        strncpy(row.username, statement->set_username_value,
                COLUMN_USERNAME_SIZE);
        row.username[COLUMN_USERNAME_SIZE] = '\0';
      }
      if (statement->set_email) {
        strncpy(row.email, statement->set_email_value, COLUMN_EMAIL_SIZE);
        row.email[COLUMN_EMAIL_SIZE] = '\0';
      }
      serialize_row(&row, cursor_value(cursor));
      count++;
    }
    cursor_advance(cursor);
  }
  free(cursor);
  printf("%d row(s) updated.\n", count);
  return EXECUTE_SUCCESS;
}

// NEW: point delete and bulk (WHERE) delete.
ExecuteResult execute_delete(Statement* statement, Table* table) {
  if (!statement->has_where) {
    uint32_t id = statement->target_id;
    Cursor* cursor = table_find(table, id);
    void* node = get_page(table->pager, cursor->page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);

    if (cursor->cell_num >= num_cells ||
        *leaf_node_key(node, cursor->cell_num) != id) {
      free(cursor);
      return EXECUTE_NOT_FOUND;
    }
    leaf_node_delete(cursor);
    free(cursor);
    printf("1 row deleted.\n");
    return EXECUTE_SUCCESS;
  }

  // Bulk delete: collect matching ids from a full scan first, then delete
  // each by key. (Deleting while scanning would invalidate the cursor.)
  uint32_t capacity = 16;
  uint32_t count = 0;
  uint32_t* ids = malloc(capacity * sizeof(uint32_t));

  Cursor* cursor = table_start(table);
  Row row;
  while (!(cursor->end_of_table)) {
    deserialize_row(cursor_value(cursor), &row);
    if (row_matches_where(&row, statement)) {
      if (count == capacity) {
        capacity *= 2;
        ids = realloc(ids, capacity * sizeof(uint32_t));
      }
      ids[count++] = row.id;
    }
    cursor_advance(cursor);
  }
  free(cursor);

  for (uint32_t i = 0; i < count; i++) {
    Cursor* c = table_find(table, ids[i]);
    void* node = get_page(table->pager, c->page_num);
    uint32_t num_cells = *leaf_node_num_cells(node);
    if (c->cell_num < num_cells && *leaf_node_key(node, c->cell_num) == ids[i]) {
      leaf_node_delete(c);
    }
    free(c);
  }
  free(ids);
  printf("%d row(s) deleted.\n", count);
  return EXECUTE_SUCCESS;
}

ExecuteResult execute_select(Statement* statement, Table* table) {
  Cursor* cursor = table_start(table);
  Row row;
  uint32_t match_count = 0;

  while (!(cursor->end_of_table)) {
    deserialize_row(cursor_value(cursor), &row);
    if (row_matches_where(&row, statement)) {
      match_count++;
      if (!statement->is_count) print_row(&row);
    }
    cursor_advance(cursor);
  }
  free(cursor);

  if (statement->is_count) {
    printf("%d row(s).\n", match_count);
  }
  return EXECUTE_SUCCESS;
}

// NEW: BEGIN — snapshot every currently cached page so ROLLBACK can
// restore it, and remember how many pages existed before the transaction
// so any pages appended during it can be discarded on rollback.
ExecuteResult execute_begin(Table* table) {
  if (table->in_transaction) return EXECUTE_TX_ALREADY_ACTIVE;

  table->in_transaction = true;
  table->tx_original_num_pages = table->pager->num_pages;

  for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
    if (table->pager->pages[i] != NULL) {
      table->tx_backup[i] = malloc(PAGE_SIZE);
      memcpy(table->tx_backup[i], table->pager->pages[i], PAGE_SIZE);
      table->tx_was_cached[i] = true;
    } else {
      table->tx_backup[i] = NULL;
      table->tx_was_cached[i] = false;
    }
  }
  return EXECUTE_SUCCESS;
}

// NEW: COMMIT — drop the snapshot and flush everything to disk right away
// (rather than waiting for .exit), so the transaction is actually durable.
ExecuteResult execute_commit(Table* table) {
  if (!table->in_transaction) return EXECUTE_NO_ACTIVE_TX;

  tx_free_backups(table);
  for (uint32_t i = 0; i < table->pager->num_pages; i++) {
    if (table->pager->pages[i] != NULL) {
      pager_flush(table->pager, i);
    }
  }
  table->in_transaction = false;
  return EXECUTE_SUCCESS;
}

// NEW: ROLLBACK — restore pages that existed before BEGIN from the
// snapshot, evict pages that were first loaded during the transaction
// (so the next read pulls the clean copy from disk), and truncate away
// any pages that were newly allocated during the transaction.
ExecuteResult execute_rollback(Table* table) {
  if (!table->in_transaction) return EXECUTE_NO_ACTIVE_TX;

  for (uint32_t i = 0; i < TABLE_MAX_PAGES; i++) {
    if (i < table->tx_original_num_pages) {
      if (table->tx_was_cached[i]) {
        memcpy(table->pager->pages[i], table->tx_backup[i], PAGE_SIZE);
        free(table->tx_backup[i]);
        table->tx_backup[i] = NULL;
      } else if (table->pager->pages[i] != NULL) {
        free(table->pager->pages[i]);
        table->pager->pages[i] = NULL;
      }
    } else {
      if (table->pager->pages[i] != NULL) {
        free(table->pager->pages[i]);
        table->pager->pages[i] = NULL;
      }
      if (table->tx_backup[i] != NULL) {
        free(table->tx_backup[i]);
        table->tx_backup[i] = NULL;
      }
    }
  }
  table->pager->num_pages = table->tx_original_num_pages;
  table->in_transaction = false;
  return EXECUTE_SUCCESS;
}

ExecuteResult execute_create(Table* table) {
  (void)table;  // schema is fixed; nothing to allocate
  printf("Table created (schema is fixed: id, username, email).\n");
  return EXECUTE_SUCCESS;
}

ExecuteResult execute_statement(Statement* statement, Table* table) {
  switch (statement->type) {
    case (STATEMENT_INSERT):
      return execute_insert(statement, table);
    case (STATEMENT_SELECT):
      return execute_select(statement, table);
    case (STATEMENT_UPDATE):
      return execute_update(statement, table);
    case (STATEMENT_DELETE):
      return execute_delete(statement, table);
    case (STATEMENT_CREATE):
      return execute_create(table);
    case (STATEMENT_BEGIN):
      return execute_begin(table);
    case (STATEMENT_COMMIT):
      return execute_commit(table);
    case (STATEMENT_ROLLBACK):
      return execute_rollback(table);
  }
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    printf("Must supply a database filename.\n");
    exit(EXIT_FAILURE);
  }

  char* filename = argv[1];
  Table* table = db_open(filename);

  InputBuffer* input_buffer = new_input_buffer();
  while (true) {
    print_prompt();
    read_input(input_buffer);

    if (input_buffer->buffer[0] == '.') {
      switch (do_meta_command(input_buffer, table)) {
        case (META_COMMAND_SUCCESS):
          continue;
        case (META_COMMAND_UNRECOGNIZED_COMMAND):
          printf("Unrecognized command '%s'\n", input_buffer->buffer);
          continue;
      }
    }

    Statement statement;
    switch (prepare_statement(input_buffer, &statement)) {
      case (PREPARE_SUCCESS):
        break;
      case (PREPARE_NEGATIVE_ID):
        printf("ID must be positive.\n");
        continue;
      case (PREPARE_STRING_TOO_LONG):
        printf("String is too long.\n");
        continue;
      case (PREPARE_SYNTAX_ERROR):
        printf("Syntax error. Could not parse statement.\n");
        continue;
      case (PREPARE_UNRECOGNIZED_STATEMENT):
        printf("Unrecognized keyword at start of '%s'.\n", input_buffer->buffer);
        continue;
    }

    switch (execute_statement(&statement, table)) {
      case (EXECUTE_SUCCESS):
        printf("Executed.\n");
        break;
      case (EXECUTE_DUPLICATE_KEY):
        printf("Error: Duplicate key.\n");
        break;
      case (EXECUTE_NOT_FOUND):
        printf("Error: row not found.\n");
        break;
      case (EXECUTE_TX_ALREADY_ACTIVE):
        printf("Error: a transaction is already active.\n");
        break;
      case (EXECUTE_NO_ACTIVE_TX):
        printf("Error: no active transaction.\n");
        break;
    }
  }
}
