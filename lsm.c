/*
  db.c — a tiny database engine, now backed by an LSM-tree.

   - WHERE clauses on SELECT/DELETE/UPDATE (id/username/email, =, !=, <, >, <=, >=)
   - UPDATE <id> <username> <email>              (point update, full row)
   - UPDATE SET col = val [...] [WHERE ...]      (SET + optional WHERE)
   - DELETE <id>                                 (point delete)
   - DELETE WHERE <col> <op> <val>               (bulk delete)
   - SELECT COUNT [WHERE ...]                    (aggregate)
   - CREATE TABLE ...                            (accepted; schema is fixed)
   - BEGIN / COMMIT / ROLLBACK                   (in-memory snapshot transactions)
   - .help meta command

  STORAGE ENGINE: LSM-tree (memtable + immutable SSTables on disk).

   - Writes (insert / update / delete) land in an in-memory sorted
     "memtable" as upserts; a delete is a tombstone entry, not a real
     removal, so the row's absence can still shadow an older on-disk
     copy of the same key.
   - When the memtable reaches MEMTABLE_FLUSH_THRESHOLD rows (or a
     COMMIT / .exit happens), it is serialized as a new immutable
     SSTable file on disk and the memtable is cleared. SSTables are
     numbered in increasing (= newer) order.
   - Point reads (duplicate-key checks, point UPDATE/DELETE) check the
     memtable first, then SSTables from newest to oldest, and return
     on the first hit — so they stay O(log n) per SSTable/memtable via
     binary search, but O(number of SSTables) in the worst case. WHERE
     scans still do a full merge of every SSTable + the memtable
     (there's no secondary index to plan around), same limitation the
     original B+tree version had for anything but a point lookup.
   - Once there are COMPACTION_THRESHOLD SSTables, they're merged into
     a single SSTable (newest values win, tombstones are finally
     dropped since there's nothing older left below them).
   - The first CLI argument is now a *directory*, not a single file:
     it holds one file per SSTable (sst_<n>.dat) plus a small MANIFEST
     text file recording SSTable order and the next id to hand out.
   - Transactions are still all-in-memory snapshots: BEGIN copies the
     current memtable and remembers how many SSTables existed;
     ROLLBACK restores that memtable copy and deletes any SSTables
     created since BEGIN (in normal operation none are, since
     auto-flush is suppressed inside a transaction); COMMIT discards
     the snapshot and force-flushes the memtable to a new SSTable
     immediately, so it's durable without waiting for .exit.
   - No WAL: like the original engine, writes are only durable once
     they've been flushed to an SSTable (on COMMIT, on hitting the
     flush threshold, or on .exit). A crash between those points loses
     unflushed writes, same as the original losing un-written pages.


*/



#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
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
  STATEMENT_CREATE,    // CREATE TABLE (no-op; schema fixed)
  STATEMENT_BEGIN,
  STATEMENT_COMMIT,
  STATEMENT_ROLLBACK
} StatementType;

// comparison operators for WHERE clauses
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

void print_row(Row* row) {
  printf("(%d, %s, %s)\n", row->id, row->username, row->email);
}

/* =====================  LSM-TREE STORAGE ENGINE  ===================== */

// One logical row-version: either a live row, or a tombstone marking
// that `id` has been deleted as of this entry's "generation".
typedef struct {
  uint32_t id;
  bool tombstone;
  Row row;
} LSMEntry;

// A sorted-by-id, growable array of entries. Used both as the mutable
// memtable and as the (fully materialized in memory once loaded)
// contents of an immutable SSTable — this is a teaching engine, so
// SSTables are small enough to just keep resident rather than reading
// through a separate on-disk index.
typedef struct {
  LSMEntry* entries;
  uint32_t count;
  uint32_t capacity;
} EntryList;

typedef struct {
  uint32_t sst_id;  // monotonically increasing; higher id = newer data
  EntryList data;   // sorted by id, immutable once flushed to disk
} SSTable;

#define MEMTABLE_FLUSH_THRESHOLD 32  // rows before an implicit flush
#define COMPACTION_THRESHOLD 4       // SSTables before we merge them all

typedef struct {
  char dirpath[480];
  char manifest_path[512];
  uint32_t next_sst_id;

  EntryList memtable;  // newest data, in-memory, mutable

  SSTable** sstables;  // oldest -> newest
  uint32_t num_sstables;
  uint32_t sstables_capacity;

  // transaction state
  bool in_transaction;
  EntryList tx_memtable_backup;
  uint32_t tx_sstables_snapshot_count;
} Table;

void entrylist_init(EntryList* list) {
  list->entries = NULL;
  list->count = 0;
  list->capacity = 0;
}

void entrylist_free(EntryList* list) {
  free(list->entries);
  list->entries = NULL;
  list->count = 0;
  list->capacity = 0;
}

void entrylist_reserve(EntryList* list, uint32_t min_capacity) {
  if (list->capacity >= min_capacity) return;
  uint32_t new_cap = list->capacity == 0 ? 8 : list->capacity;
  while (new_cap < min_capacity) new_cap *= 2;
  list->entries = realloc(list->entries, new_cap * sizeof(LSMEntry));
  list->capacity = new_cap;
}

// Deep copy (used for transaction snapshots).
void entrylist_copy(EntryList* dst, EntryList* src) {
  entrylist_init(dst);
  if (src->count == 0) return;
  entrylist_reserve(dst, src->count);
  memcpy(dst->entries, src->entries, src->count * sizeof(LSMEntry));
  dst->count = src->count;
}

// Binary search for `id`. Returns true and sets *index_out to the
// matching slot if found; otherwise returns false and sets *index_out
// to the sorted insertion point.
bool entrylist_bsearch(EntryList* list, uint32_t id, uint32_t* index_out) {
  uint32_t lo = 0, hi = list->count;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    if (list->entries[mid].id == id) {
      *index_out = mid;
      return true;
    }
    if (list->entries[mid].id < id) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  *index_out = lo;
  return false;
}

// Insert-or-overwrite `id` with a new version (row, or a tombstone if
// row is NULL), keeping the list sorted by id.
void entrylist_upsert(EntryList* list, uint32_t id, bool tombstone, Row* row) {
  uint32_t idx;
  bool found = entrylist_bsearch(list, id, &idx);
  if (found) {
    list->entries[idx].tombstone = tombstone;
    if (row) {
      list->entries[idx].row = *row;
    } else {
      memset(&list->entries[idx].row, 0, sizeof(Row));
    }
    return;
  }
  entrylist_reserve(list, list->count + 1);
  memmove(&list->entries[idx + 1], &list->entries[idx],
          (list->count - idx) * sizeof(LSMEntry));
  list->entries[idx].id = id;
  list->entries[idx].tombstone = tombstone;
  if (row) {
    list->entries[idx].row = *row;
  } else {
    memset(&list->entries[idx].row, 0, sizeof(Row));
  }
  list->count++;
}

char* sstable_path(Table* table, uint32_t sst_id, char* buf, size_t buf_size) {
  snprintf(buf, buf_size, "%s/sst_%u.dat", table->dirpath, sst_id);
  return buf;
}

bool sstable_write_file(EntryList* data, const char* path) {
  FILE* f = fopen(path, "wb");
  if (!f) return false;
  uint32_t count = data->count;
  fwrite(&count, sizeof(uint32_t), 1, f);
  for (uint32_t i = 0; i < count; i++) {
    LSMEntry* e = &data->entries[i];
    uint8_t tomb = e->tombstone ? 1 : 0;
    char uname[COLUMN_USERNAME_SIZE + 1];
    char email[COLUMN_EMAIL_SIZE + 1];
    memset(uname, 0, sizeof(uname));
    memset(email, 0, sizeof(email));
    strncpy(uname, e->row.username, COLUMN_USERNAME_SIZE);
    strncpy(email, e->row.email, COLUMN_EMAIL_SIZE);

    fwrite(&tomb, 1, 1, f);
    fwrite(&e->id, sizeof(uint32_t), 1, f);
    fwrite(uname, 1, sizeof(uname), f);
    fwrite(email, 1, sizeof(email), f);
  }
  fclose(f);
  return true;
}

SSTable* sstable_load_file(const char* path, uint32_t sst_id) {
  FILE* f = fopen(path, "rb");
  if (!f) return NULL;

  SSTable* sst = malloc(sizeof(SSTable));
  sst->sst_id = sst_id;
  entrylist_init(&sst->data);

  uint32_t count = 0;
  if (fread(&count, sizeof(uint32_t), 1, f) != 1) {
    fclose(f);
    entrylist_free(&sst->data);
    free(sst);
    return NULL;
  }
  entrylist_reserve(&sst->data, count);

  for (uint32_t i = 0; i < count; i++) {
    uint8_t tomb;
    uint32_t id;
    char uname[COLUMN_USERNAME_SIZE + 1];
    char email[COLUMN_EMAIL_SIZE + 1];
    fread(&tomb, 1, 1, f);
    fread(&id, sizeof(uint32_t), 1, f);
    fread(uname, 1, sizeof(uname), f);
    fread(email, 1, sizeof(email), f);

    LSMEntry* e = &sst->data.entries[i];
    e->id = id;
    e->tombstone = (tomb != 0);
    e->row.id = id;
    memcpy(e->row.username, uname, sizeof(uname));
    memcpy(e->row.email, email, sizeof(email));
  }
  sst->data.count = count;
  fclose(f);
  return sst;
}

void table_write_manifest(Table* table) {
  FILE* f = fopen(table->manifest_path, "wb");
  if (!f) return;
  fprintf(f, "next_sst_id %u\n", table->next_sst_id);
  for (uint32_t i = 0; i < table->num_sstables; i++) {
    fprintf(f, "sst %u\n", table->sstables[i]->sst_id);
  }
  fclose(f);
}

void table_append_sstable(Table* table, SSTable* sst) {
  if (table->num_sstables == table->sstables_capacity) {
    uint32_t new_cap = table->sstables_capacity == 0 ? 4 : table->sstables_capacity * 2;
    table->sstables = realloc(table->sstables, new_cap * sizeof(SSTable*));
    table->sstables_capacity = new_cap;
  }
  table->sstables[table->num_sstables++] = sst;
}

void table_maybe_compact(Table* table) {
  if (table->in_transaction) return;
  if (table->num_sstables < COMPACTION_THRESHOLD) return;

  // Merge every SSTable, oldest to newest, so newer versions win.
  EntryList merged;
  entrylist_init(&merged);
  for (uint32_t i = 0; i < table->num_sstables; i++) {
    SSTable* sst = table->sstables[i];
    for (uint32_t j = 0; j < sst->data.count; j++) {
      LSMEntry* e = &sst->data.entries[j];
      entrylist_upsert(&merged, e->id, e->tombstone, &e->row);
    }
  }

  // This is about to become the only (oldest) SSTable, with nothing
  // older below it, so tombstones have finally done their job and can
  // be dropped for good.
  uint32_t w = 0;
  for (uint32_t r = 0; r < merged.count; r++) {
    if (!merged.entries[r].tombstone) merged.entries[w++] = merged.entries[r];
  }
  merged.count = w;

  uint32_t new_id = table->next_sst_id++;
  char path[512];
  sstable_path(table, new_id, path, sizeof(path));
  sstable_write_file(&merged, path);

  for (uint32_t i = 0; i < table->num_sstables; i++) {
    char old_path[512];
    sstable_path(table, table->sstables[i]->sst_id, old_path, sizeof(old_path));
    remove(old_path);
    entrylist_free(&table->sstables[i]->data);
    free(table->sstables[i]);
  }
  table->num_sstables = 0;

  SSTable* new_sst = malloc(sizeof(SSTable));
  new_sst->sst_id = new_id;
  new_sst->data = merged;  // transfer ownership of the merged buffer
  table_append_sstable(table, new_sst);

  table_write_manifest(table);
}

// Flush the memtable to a brand-new, immutable SSTable on disk. No-op
// if the memtable is currently empty.
void table_flush_memtable(Table* table) {
  if (table->memtable.count == 0) return;

  uint32_t id = table->next_sst_id++;
  char path[512];
  sstable_path(table, id, path, sizeof(path));
  sstable_write_file(&table->memtable, path);

  SSTable* sst = malloc(sizeof(SSTable));
  sst->sst_id = id;
  entrylist_copy(&sst->data, &table->memtable);
  table_append_sstable(table, sst);

  entrylist_free(&table->memtable);
  entrylist_init(&table->memtable);

  table_write_manifest(table);
  table_maybe_compact(table);
}

void table_flush_if_threshold(Table* table) {
  if (!table->in_transaction && table->memtable.count >= MEMTABLE_FLUSH_THRESHOLD) {
    table_flush_memtable(table);
  }
}

// Point lookup: does a *live* row currently exist for `id`? Checks the
// memtable first (always the newest data), then SSTables from newest
// to oldest, and stops at the first hit either way.
bool table_get_live(Table* table, uint32_t id, Row* out_row) {
  uint32_t idx;
  if (entrylist_bsearch(&table->memtable, id, &idx)) {
    LSMEntry* e = &table->memtable.entries[idx];
    if (e->tombstone) return false;
    if (out_row) *out_row = e->row;
    return true;
  }
  for (int i = (int)table->num_sstables - 1; i >= 0; i--) {
    SSTable* sst = table->sstables[i];
    if (entrylist_bsearch(&sst->data, id, &idx)) {
      LSMEntry* e = &sst->data.entries[idx];
      if (e->tombstone) return false;
      if (out_row) *out_row = e->row;
      return true;
    }
  }
  return false;
}

// Build the fully merged, sorted-by-id view of the table (every
// SSTable oldest-to-newest, then the memtable on top), used for WHERE
// scans since there's no secondary index to plan around. If
// `include_tombstones` is false, deleted rows are dropped from the
// result before returning.
void table_build_view(Table* table, EntryList* out, bool include_tombstones) {
  entrylist_init(out);
  for (uint32_t i = 0; i < table->num_sstables; i++) {
    SSTable* sst = table->sstables[i];
    for (uint32_t j = 0; j < sst->data.count; j++) {
      LSMEntry* e = &sst->data.entries[j];
      entrylist_upsert(out, e->id, e->tombstone, &e->row);
    }
  }
  for (uint32_t j = 0; j < table->memtable.count; j++) {
    LSMEntry* e = &table->memtable.entries[j];
    entrylist_upsert(out, e->id, e->tombstone, &e->row);
  }
  if (!include_tombstones) {
    uint32_t w = 0;
    for (uint32_t r = 0; r < out->count; r++) {
      if (!out->entries[r].tombstone) out->entries[w++] = out->entries[r];
    }
    out->count = w;
  }
}

void print_lsm_structure(Table* table) {
  printf("Memtable: %u row(s) (flushes at %u)\n", table->memtable.count,
         (unsigned)MEMTABLE_FLUSH_THRESHOLD);
  printf("SSTables (oldest to newest, %u total, compacts at %u):\n",
         table->num_sstables, (unsigned)COMPACTION_THRESHOLD);
  for (uint32_t i = 0; i < table->num_sstables; i++) {
    printf("  - sst_%u.dat: %u row(s)\n", table->sstables[i]->sst_id,
           table->sstables[i]->data.count);
  }
}

void print_constants() {
  printf("ROW_SIZE: %zu\n", sizeof(Row));
  printf("USERNAME_SIZE: %d\n", COLUMN_USERNAME_SIZE);
  printf("EMAIL_SIZE: %d\n", COLUMN_EMAIL_SIZE);
  printf("MEMTABLE_FLUSH_THRESHOLD: %u\n", (unsigned)MEMTABLE_FLUSH_THRESHOLD);
  printf("COMPACTION_THRESHOLD: %u\n", (unsigned)COMPACTION_THRESHOLD);
}

Table* db_open(const char* dirpath) {
  struct stat st;
  if (stat(dirpath, &st) != 0) {
    if (mkdir(dirpath, 0755) != 0) {
      printf("Unable to create database directory '%s'\n", dirpath);
      exit(EXIT_FAILURE);
    }
  }

  Table* table = malloc(sizeof(Table));
  strncpy(table->dirpath, dirpath, sizeof(table->dirpath) - 1);
  table->dirpath[sizeof(table->dirpath) - 1] = '\0';
  snprintf(table->manifest_path, sizeof(table->manifest_path), "%s/MANIFEST",
           table->dirpath);

  table->next_sst_id = 1;
  entrylist_init(&table->memtable);
  table->sstables = NULL;
  table->num_sstables = 0;
  table->sstables_capacity = 0;
  table->in_transaction = false;
  entrylist_init(&table->tx_memtable_backup);
  table->tx_sstables_snapshot_count = 0;

  FILE* mf = fopen(table->manifest_path, "r");
  if (mf) {
    char line[256];
    uint32_t ids[4096];
    uint32_t num_ids = 0;
    while (fgets(line, sizeof(line), mf)) {
      uint32_t v;
      if (sscanf(line, "next_sst_id %u", &v) == 1) {
        table->next_sst_id = v;
        continue;
      }
      if (sscanf(line, "sst %u", &v) == 1) {
        if (num_ids < 4096) ids[num_ids++] = v;
        continue;
      }
    }
    fclose(mf);

    for (uint32_t i = 0; i < num_ids; i++) {
      char path[512];
      sstable_path(table, ids[i], path, sizeof(path));
      SSTable* sst = sstable_load_file(path, ids[i]);
      if (sst) table_append_sstable(table, sst);
    }
  }

  return table;
}

void db_close(Table* table) {
  // If the process exits mid-transaction, whatever's currently in the
  // memtable (including any not-yet-committed writes from that
  // transaction) still gets flushed below — COMMIT is the normal
  // durability point, but .exit persists the current in-memory state
  // rather than silently discarding it. The pre-transaction snapshot
  // itself is no longer needed either way.
  entrylist_free(&table->tx_memtable_backup);

  table_flush_memtable(table);

  for (uint32_t i = 0; i < table->num_sstables; i++) {
    entrylist_free(&table->sstables[i]->data);
    free(table->sstables[i]);
  }
  free(table->sstables);
  entrylist_free(&table->memtable);
  free(table);
}

/* =====================  CLI / statement layer  ===================== */

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

void print_help() {
  printf("Meta commands:\n");
  printf("  .exit               flush and close the database, then quit\n");
  printf("  .lsm                print memtable + SSTable structure\n");
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
  } else if (strcmp(input_buffer->buffer, ".lsm") == 0 ||
             strcmp(input_buffer->buffer, ".btree") == 0) {
    print_lsm_structure(table);
    return META_COMMAND_SUCCESS;
  } else if (strcmp(input_buffer->buffer, ".constants") == 0) {
    printf("Constants:\n");
    print_constants();
    return META_COMMAND_SUCCESS;
  } else if (strcmp(input_buffer->buffer, ".help") == 0) {
    print_help();
    return META_COMMAND_SUCCESS;
  } else {
    return META_COMMAND_UNRECOGNIZED_COMMAND;
  }
}

// shared WHERE-clause parser. Expects "where" to already be consumed;
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

// does a deserialized row satisfy the statement's WHERE clause?
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

  strtok(input_buffer->buffer, " ");  // keyword
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

  strtok(input_buffer->buffer, " ");  // keyword
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

PrepareResult prepare_delete(InputBuffer* input_buffer, Statement* statement) {
  statement->type = STATEMENT_DELETE;
  statement->has_where = false;

  strtok(input_buffer->buffer, " ");  // keyword
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

PrepareResult prepare_select(InputBuffer* input_buffer, Statement* statement) {
  statement->type = STATEMENT_SELECT;
  statement->has_where = false;
  statement->is_count = false;

  strtok(input_buffer->buffer, " ");  // keyword
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

/* =====================  statement execution  ===================== */

ExecuteResult execute_insert(Statement* statement, Table* table) {
  Row* row = &statement->row_to_insert;

  if (table_get_live(table, row->id, NULL)) {
    return EXECUTE_DUPLICATE_KEY;
  }
  entrylist_upsert(&table->memtable, row->id, false, row);
  table_flush_if_threshold(table);
  return EXECUTE_SUCCESS;
}

ExecuteResult execute_select(Statement* statement, Table* table) {
  EntryList view;
  table_build_view(table, &view, false);

  uint32_t match_count = 0;
  for (uint32_t i = 0; i < view.count; i++) {
    Row row = view.entries[i].row;
    if (row_matches_where(&row, statement)) {
      match_count++;
      if (!statement->is_count) print_row(&row);
    }
  }
  entrylist_free(&view);

  if (statement->is_count) {
    printf("%d row(s).\n", match_count);
  }
  return EXECUTE_SUCCESS;
}

ExecuteResult execute_update(Statement* statement, Table* table) {
  if (!statement->is_set_update) {
    uint32_t id = statement->row_to_insert.id;
    if (!table_get_live(table, id, NULL)) {
      return EXECUTE_NOT_FOUND;
    }
    entrylist_upsert(&table->memtable, id, false, &statement->row_to_insert);
    table_flush_if_threshold(table);
    return EXECUTE_SUCCESS;
  }

  EntryList view;
  table_build_view(table, &view, false);

  uint32_t count = 0;
  for (uint32_t i = 0; i < view.count; i++) {
    Row row = view.entries[i].row;
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
      entrylist_upsert(&table->memtable, row.id, false, &row);
      count++;
    }
  }
  entrylist_free(&view);
  table_flush_if_threshold(table);

  printf("%d row(s) updated.\n", count);
  return EXECUTE_SUCCESS;
}

ExecuteResult execute_delete(Statement* statement, Table* table) {
  if (!statement->has_where) {
    uint32_t id = statement->target_id;
    if (!table_get_live(table, id, NULL)) {
      return EXECUTE_NOT_FOUND;
    }
    entrylist_upsert(&table->memtable, id, true, NULL);
    table_flush_if_threshold(table);
    printf("1 row deleted.\n");
    return EXECUTE_SUCCESS;
  }

  EntryList view;
  table_build_view(table, &view, false);

  uint32_t count = 0;
  for (uint32_t i = 0; i < view.count; i++) {
    Row row = view.entries[i].row;
    if (row_matches_where(&row, statement)) {
      entrylist_upsert(&table->memtable, row.id, true, NULL);
      count++;
    }
  }
  entrylist_free(&view);
  table_flush_if_threshold(table);

  printf("%d row(s) deleted.\n", count);
  return EXECUTE_SUCCESS;
}

ExecuteResult execute_begin(Table* table) {
  if (table->in_transaction) return EXECUTE_TX_ALREADY_ACTIVE;

  table->in_transaction = true;
  entrylist_free(&table->tx_memtable_backup);
  entrylist_copy(&table->tx_memtable_backup, &table->memtable);
  table->tx_sstables_snapshot_count = table->num_sstables;
  return EXECUTE_SUCCESS;
}

// COMMIT — drop the snapshot and force the memtable to disk right away
// (rather than waiting for .exit or the flush threshold), so the
// transaction is actually durable as soon as it commits.
ExecuteResult execute_commit(Table* table) {
  if (!table->in_transaction) return EXECUTE_NO_ACTIVE_TX;

  entrylist_free(&table->tx_memtable_backup);
  table->in_transaction = false;  // so compaction is allowed to run again
  table_flush_memtable(table);
  return EXECUTE_SUCCESS;
}

// ROLLBACK — restore the memtable from the pre-BEGIN snapshot, and
// delete any SSTables that were created since BEGIN (in normal
// operation there won't be any, since auto-flush and compaction are
// both suppressed while a transaction is active).
ExecuteResult execute_rollback(Table* table) {
  if (!table->in_transaction) return EXECUTE_NO_ACTIVE_TX;

  entrylist_free(&table->memtable);
  table->memtable = table->tx_memtable_backup;  // take ownership
  entrylist_init(&table->tx_memtable_backup);

  while (table->num_sstables > table->tx_sstables_snapshot_count) {
    SSTable* sst = table->sstables[table->num_sstables - 1];
    char path[512];
    sstable_path(table, sst->sst_id, path, sizeof(path));
    remove(path);
    entrylist_free(&sst->data);
    free(sst);
    table->num_sstables--;
  }
  table_write_manifest(table);

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
  return EXECUTE_SUCCESS;
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    printf("Must supply a database directory name.\n");
    exit(EXIT_FAILURE);
  }

  char* dirname_arg = argv[1];
  Table* table = db_open(dirname_arg);

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
