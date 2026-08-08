/*
  db_lsm.c — Tiny LSM-Tree database engine


  gcc -O2 -o db_lsm db_lsm.c
  ./db_lsm mydb.db

  Simplified single-/two-level LSM

    create table ...
    insert <id> <username> <email>
    select [count] [where <col> <op> <val>]
    update <id> <username> <email>
    update set <col> = <val> [...] [where ...]
    delete <id>
    delete where <col> <op> <val>
    begin / commit / rollback
    .help  .exit  .stats  .sstables

  Architecture
  ------------
  - Active memtable: dynamically-sized sorted array of Entries (by id).
  - On size threshold → freeze memtable and flush it as a new SSTable
    (sorted run). SSTables live in a list (newest first).
  - Deletes are tombstones.
  - Point lookups and scans merge newest → oldest, keeping only the
    latest non-tombstone version of each key.
  - Transactions: simple in-memory snapshot of the memtable + list of
    SSTables that existed at BEGIN. ROLLBACK restores that view;
    COMMIT just clears the snapshot (flushes still happen normally).
  - Persistence: on .exit / explicit flush the current set of SSTables
    is written to <dbname>.sst.* files and a small manifest. On open
    the manifest is read and SSTables are loaded back.

  This is intentionally minimal and educational — no bloom filters,
  no leveled compaction beyond a simple size-triggered full merge,
  no WAL. Good base to grow.
*/

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

/* ------------------------------------------------------------------ */
/*  Constants & basic types                                           */
/* ------------------------------------------------------------------ */

#define COLUMN_USERNAME_SIZE 32
#define COLUMN_EMAIL_SIZE    255
#define MEMTABLE_FLUSH_THRESHOLD  64   /* rows; small for easy testing */
#define MAX_SSTABLES              64
#define MAX_FILENAME              512

typedef struct {
  uint32_t id;
  char username[COLUMN_USERNAME_SIZE + 1];
  char email[COLUMN_EMAIL_SIZE + 1];
  bool tombstone;          /* true = deletion marker */
} Entry;

typedef struct {
  Entry* entries;
  uint32_t count;
  uint32_t capacity;
} SortedRun;               /* both memtable and SSTable use this */

typedef struct {
  SortedRun* runs[MAX_SSTABLES];  /* newest at index 0 */
  uint32_t num_runs;
  char base_name[MAX_FILENAME];   /* used for sst file names */
} SSTableList;

/* Transaction snapshot */
typedef struct {
  SortedRun* memtable_copy;
  SortedRun* runs_copy[MAX_SSTABLES];
  uint32_t num_runs_copy;
  bool active;
} TxSnapshot;

typedef struct {
  SortedRun* memtable;            /* active mutable sorted run */
  SSTableList sstables;
  TxSnapshot tx;
  uint32_t next_sst_id;           /* for naming sst files */
} Table;

/* ------------------------------------------------------------------ */
/*  Statement / parser types (kept compatible with original)          */
/* ------------------------------------------------------------------ */

typedef struct {
  char* buffer;
  size_t buffer_length;
  ssize_t input_length;
} InputBuffer;

typedef enum {
  EXECUTE_SUCCESS,
  EXECUTE_DUPLICATE_KEY,
  EXECUTE_NOT_FOUND,
  EXECUTE_TX_ALREADY_ACTIVE,
  EXECUTE_NO_ACTIVE_TX
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
  STATEMENT_CREATE,
  STATEMENT_BEGIN,
  STATEMENT_COMMIT,
  STATEMENT_ROLLBACK
} StatementType;

typedef enum {
  WHERE_EQ, WHERE_NE, WHERE_GT, WHERE_LT, WHERE_GE, WHERE_LE
} WhereOp;

typedef struct {
  uint32_t id;
  char username[COLUMN_USERNAME_SIZE + 1];
  char email[COLUMN_EMAIL_SIZE + 1];
} Row;

typedef struct {
  StatementType type;
  Row row_to_insert;

  bool has_where;
  char where_column[16];
  bool where_is_string;
  WhereOp where_op;
  uint32_t where_int_value;
  char where_str_value[COLUMN_EMAIL_SIZE + 1];

  bool is_count;
  uint32_t target_id;

  bool is_set_update;
  bool set_username;
  bool set_email;
  char set_username_value[COLUMN_USERNAME_SIZE + 1];
  char set_email_value[COLUMN_EMAIL_SIZE + 1];
} Statement;

/* ------------------------------------------------------------------ */
/*  SortedRun helpers                                                 */
/* ------------------------------------------------------------------ */

SortedRun* sorted_run_create(uint32_t initial_cap) {
  SortedRun* r = malloc(sizeof(SortedRun));
  r->capacity = initial_cap ? initial_cap : 16;
  r->count = 0;
  r->entries = malloc(r->capacity * sizeof(Entry));
  return r;
}

void sorted_run_free(SortedRun* r) {
  if (!r) return;
  free(r->entries);
  free(r);
}

SortedRun* sorted_run_clone(const SortedRun* src) {
  if (!src) return NULL;
  SortedRun* r = sorted_run_create(src->count);
  r->count = src->count;
  if (src->count)
    memcpy(r->entries, src->entries, src->count * sizeof(Entry));
  return r;
}

/* Binary search for the insertion point / exact match.
   Returns the index where id should be, and sets *found. */
uint32_t sorted_run_find(const SortedRun* r, uint32_t id, bool* found) {
  uint32_t lo = 0, hi = r->count;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    if (r->entries[mid].id < id)
      lo = mid + 1;
    else
      hi = mid;
  }
  *found = (lo < r->count && r->entries[lo].id == id);
  return lo;
}

void sorted_run_ensure(SortedRun* r, uint32_t need) {
  if (need <= r->capacity) return;
  while (r->capacity < need) r->capacity *= 2;
  r->entries = realloc(r->entries, r->capacity * sizeof(Entry));
}

/* Insert or replace (latest version wins). Tombstones are just entries. */
void sorted_run_upsert(SortedRun* r, const Entry* e) {
  bool found;
  uint32_t idx = sorted_run_find(r, e->id, &found);
  if (found) {
    r->entries[idx] = *e;          /* replace in place */
    return;
  }
  sorted_run_ensure(r, r->count + 1);
  memmove(&r->entries[idx + 1], &r->entries[idx],
          (r->count - idx) * sizeof(Entry));
  r->entries[idx] = *e;
  r->count++;
}

/* ------------------------------------------------------------------ */
/*  Multi-run merge (newest → oldest) for a given key or full scan    */
/* ------------------------------------------------------------------ */

/* Result of resolving a key across memtable + all SSTables */
typedef struct {
  bool present;     /* false = does not exist (or only tombstones) */
  Entry entry;
} Resolved;

/* Walk runs from newest to oldest; first non-tombstone wins.
   runs[0] is newest. memtable is examined first. */
Resolved resolve_key(const SortedRun* memtable,
                     const SSTableList* sst,
                     uint32_t id) {
  Resolved res = { .present = false };
  bool found;

  if (memtable) {
    uint32_t idx = sorted_run_find(memtable, id, &found);
    if (found) {
      if (memtable->entries[idx].tombstone)
        return res;                 /* newest is a delete */
      res.present = true;
      res.entry = memtable->entries[idx];
      return res;
    }
  }

  for (uint32_t i = 0; i < sst->num_runs; i++) {
    const SortedRun* run = sst->runs[i];
    uint32_t idx = sorted_run_find(run, id, &found);
    if (found) {
      if (run->entries[idx].tombstone)
        return res;
      res.present = true;
      res.entry = run->entries[idx];
      return res;
    }
  }
  return res;
}

/* Full scan: produce a temporary sorted array of the latest live rows.
   Caller must free the returned SortedRun. */
SortedRun* merge_all_live(const SortedRun* memtable, const SSTableList* sst) {
  /* Upper bound on total entries */
  uint32_t total = memtable ? memtable->count : 0;
  for (uint32_t i = 0; i < sst->num_runs; i++)
    total += sst->runs[i]->count;

  SortedRun* out = sorted_run_create(total + 1);

  /* Collect every unique id that appears, newest version first */
  /* Simple O(N log N) approach: dump everything, sort, then unique */
  Entry* tmp = malloc(total * sizeof(Entry));
  uint32_t n = 0;

  if (memtable) {
    for (uint32_t i = 0; i < memtable->count; i++)
      tmp[n++] = memtable->entries[i];
  }
  for (uint32_t r = 0; r < sst->num_runs; r++) {
    for (uint32_t i = 0; i < sst->runs[r]->count; i++)
      tmp[n++] = sst->runs[r]->entries[i];
  }

  if (n == 0) {
    free(tmp);
    return out;
  }

  /* Sort by id ascending, stable enough for our purposes */
  int cmp_entry(const void* a, const void* b) {
    const Entry* ea = a;
    const Entry* eb = b;
    if (ea->id < eb->id) return -1;
    if (ea->id > eb->id) return 1;
    return 0;
  }
  qsort(tmp, n, sizeof(Entry), cmp_entry);

  /* Keep only the first occurrence of each id (we will reverse the
     run order later so that newest is examined first).  Because we
     dumped newest runs first, after a stable sort the first of each
     id group is from the newest run that contained it. */
  /* Actually qsort is not stable.  Re-dump in newest-first order and
     use a seen set, or just walk and take the first of each group
     after sorting by (id, then reverse-run-order).  For simplicity
     we re-resolve every unique id. */

  uint32_t i = 0;
  while (i < n) {
    uint32_t id = tmp[i].id;
    /* skip to end of this id group */
    uint32_t j = i;
    while (j < n && tmp[j].id == id) j++;

    Resolved r = resolve_key(memtable, sst, id);
    if (r.present) {
      sorted_run_ensure(out, out->count + 1);
      out->entries[out->count++] = r.entry;
    }
    i = j;
  }

  free(tmp);
  return out;
}

/* ------------------------------------------------------------------ */
/*  SSTable list & flush                                              */
/* ------------------------------------------------------------------ */

void sstable_list_init(SSTableList* l, const char* base) {
  l->num_runs = 0;
  memset(l->runs, 0, sizeof(l->runs));
  strncpy(l->base_name, base, MAX_FILENAME - 1);
  l->base_name[MAX_FILENAME - 1] = '\0';
}

void sstable_list_push_front(SSTableList* l, SortedRun* run) {
  if (l->num_runs >= MAX_SSTABLES) {
    fprintf(stderr, "Too many SSTables\n");
    exit(1);
  }
  memmove(&l->runs[1], &l->runs[0], l->num_runs * sizeof(SortedRun*));
  l->runs[0] = run;
  l->num_runs++;
}

/* Very simple size-triggered compaction: when we have many runs,
   merge everything into one new run and discard the old ones. */
void maybe_compact(Table* table) {
  if (table->sstables.num_runs < 4) return;

  SortedRun* merged = merge_all_live(NULL, &table->sstables);
  /* free old runs */
  for (uint32_t i = 0; i < table->sstables.num_runs; i++)
    sorted_run_free(table->sstables.runs[i]);
  table->sstables.num_runs = 0;
  if (merged->count > 0)
    sstable_list_push_front(&table->sstables, merged);
  else
    sorted_run_free(merged);
}

void flush_memtable(Table* table) {
  if (table->memtable->count == 0) return;

  SortedRun* frozen = table->memtable;
  table->memtable = sorted_run_create(16);
  sstable_list_push_front(&table->sstables, frozen);
  maybe_compact(table);
}

/* ------------------------------------------------------------------ */
/*  Persistence (simple binary SSTables + manifest)                   */
/* ------------------------------------------------------------------ */

void write_run_to_file(const SortedRun* run, const char* path) {
  FILE* f = fopen(path, "wb");
  if (!f) {
    perror("fopen sst");
    return;
  }
  fwrite(&run->count, sizeof(uint32_t), 1, f);
  if (run->count)
    fwrite(run->entries, sizeof(Entry), run->count, f);
  fclose(f);
}

SortedRun* read_run_from_file(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) return NULL;
  uint32_t count = 0;
  if (fread(&count, sizeof(uint32_t), 1, f) != 1) {
    fclose(f);
    return NULL;
  }
  SortedRun* r = sorted_run_create(count + 1);
  r->count = count;
  if (count && fread(r->entries, sizeof(Entry), count, f) != count) {
    sorted_run_free(r);
    fclose(f);
    return NULL;
  }
  fclose(f);
  return r;
}

void save_manifest(Table* table) {
  char path[MAX_FILENAME];
  snprintf(path, sizeof(path), "%s.manifest", table->sstables.base_name);
  FILE* f = fopen(path, "w");
  if (!f) return;
  fprintf(f, "%u\n", table->next_sst_id);
  for (uint32_t i = 0; i < table->sstables.num_runs; i++) {
    char sstpath[MAX_FILENAME];
    snprintf(sstpath, sizeof(sstpath), "%s.sst.%u",
             table->sstables.base_name, table->next_sst_id - 1 - i);
    /* We re-write every run with a fresh id on save for simplicity */
  }
  /* Simpler: just dump current runs with sequential names */
  fprintf(f, "%u\n", table->sstables.num_runs);
  for (uint32_t i = 0; i < table->sstables.num_runs; i++) {
    char sstpath[MAX_FILENAME];
    snprintf(sstpath, sizeof(sstpath), "%s.sst.%u",
             table->sstables.base_name, i);
    write_run_to_file(table->sstables.runs[i], sstpath);
    fprintf(f, "%s\n", sstpath);
  }
  fclose(f);
}

void load_manifest(Table* table) {
  char path[MAX_FILENAME];
  snprintf(path, sizeof(path), "%s.manifest", table->sstables.base_name);
  FILE* f = fopen(path, "r");
  if (!f) return;

  unsigned next_id = 0, num = 0;
  if (fscanf(f, "%u\n%u\n", &next_id, &num) != 2) {
    fclose(f);
    return;
  }
  table->next_sst_id = next_id;

  for (uint32_t i = 0; i < num && i < MAX_SSTABLES; i++) {
    char sstpath[MAX_FILENAME];
    if (!fgets(sstpath, sizeof(sstpath), f)) break;
    /* strip newline */
    size_t len = strlen(sstpath);
    if (len && sstpath[len-1] == '\n') sstpath[len-1] = 0;
    SortedRun* r = read_run_from_file(sstpath);
    if (r)
      sstable_list_push_front(&table->sstables, r);
  }
  fclose(f);
}

/* ------------------------------------------------------------------ */
/*  Table open / close                                                */
/* ------------------------------------------------------------------ */

Table* db_open(const char* filename) {
  Table* t = calloc(1, sizeof(Table));
  t->memtable = sorted_run_create(16);
  sstable_list_init(&t->sstables, filename);
  t->next_sst_id = 0;
  t->tx.active = false;
  load_manifest(t);
  return t;
}

void db_close(Table* table) {
  /* Flush any remaining memtable so nothing is lost */
  flush_memtable(table);
  save_manifest(table);

  sorted_run_free(table->memtable);
  for (uint32_t i = 0; i < table->sstables.num_runs; i++)
    sorted_run_free(table->sstables.runs[i]);

  if (table->tx.active) {
    sorted_run_free(table->tx.memtable_copy);
    for (uint32_t i = 0; i < table->tx.num_runs_copy; i++)
      sorted_run_free(table->tx.runs_copy[i]);
  }
  free(table);
}

/* ------------------------------------------------------------------ */
/*  WHERE matching (identical logic to original)                      */
/* ------------------------------------------------------------------ */

bool row_matches_where(const Entry* e, Statement* statement) {
  if (!statement->has_where) return true;
  if (e->tombstone) return false;

  int cmp;
  if (strcmp(statement->where_column, "id") == 0) {
    uint32_t v = statement->where_int_value;
    cmp = (e->id > v) - (e->id < v);
  } else if (strcmp(statement->where_column, "username") == 0) {
    int c = strcmp(e->username, statement->where_str_value);
    cmp = (c > 0) - (c < 0);
  } else {
    int c = strcmp(e->email, statement->where_str_value);
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

void print_entry(const Entry* e) {
  if (e->tombstone) return;
  printf("(%u, %s, %s)\n", e->id, e->username, e->email);
}

/* ------------------------------------------------------------------ */
/*  Execute helpers                                                   */
/* ------------------------------------------------------------------ */

ExecuteResult execute_insert(Statement* statement, Table* table) {
  uint32_t id = statement->row_to_insert.id;

  /* Check for existing live key */
  Resolved r = resolve_key(table->memtable, &table->sstables, id);
  if (r.present)
    return EXECUTE_DUPLICATE_KEY;

  Entry e = {
    .id = id,
    .tombstone = false
  };
  strncpy(e.username, statement->row_to_insert.username, COLUMN_USERNAME_SIZE);
  e.username[COLUMN_USERNAME_SIZE] = 0;
  strncpy(e.email, statement->row_to_insert.email, COLUMN_EMAIL_SIZE);
  e.email[COLUMN_EMAIL_SIZE] = 0;

  sorted_run_upsert(table->memtable, &e);

  if (table->memtable->count >= MEMTABLE_FLUSH_THRESHOLD)
    flush_memtable(table);

  return EXECUTE_SUCCESS;
}

ExecuteResult execute_update(Statement* statement, Table* table) {
  if (!statement->is_set_update) {
    /* classic point form */
    uint32_t id = statement->row_to_insert.id;
    Resolved r = resolve_key(table->memtable, &table->sstables, id);
    if (!r.present)
      return EXECUTE_NOT_FOUND;

    Entry e = r.entry;
    e.tombstone = false;
    strncpy(e.username, statement->row_to_insert.username, COLUMN_USERNAME_SIZE);
    e.username[COLUMN_USERNAME_SIZE] = 0;
    strncpy(e.email, statement->row_to_insert.email, COLUMN_EMAIL_SIZE);
    e.email[COLUMN_EMAIL_SIZE] = 0;
    sorted_run_upsert(table->memtable, &e);

    if (table->memtable->count >= MEMTABLE_FLUSH_THRESHOLD)
      flush_memtable(table);

    printf("1 row updated.\n");
    return EXECUTE_SUCCESS;
  }

  /* SET form — full scan */
  SortedRun* live = merge_all_live(table->memtable, &table->sstables);
  uint32_t count = 0;
  for (uint32_t i = 0; i < live->count; i++) {
    Entry* e = &live->entries[i];
    if (!row_matches_where(e, statement)) continue;

    if (statement->set_username) {
      strncpy(e->username, statement->set_username_value, COLUMN_USERNAME_SIZE);
      e->username[COLUMN_USERNAME_SIZE] = 0;
    }
    if (statement->set_email) {
      strncpy(e->email, statement->set_email_value, COLUMN_EMAIL_SIZE);
      e->email[COLUMN_EMAIL_SIZE] = 0;
    }
    e->tombstone = false;
    sorted_run_upsert(table->memtable, e);
    count++;
  }
  sorted_run_free(live);

  if (table->memtable->count >= MEMTABLE_FLUSH_THRESHOLD)
    flush_memtable(table);

  printf("%u row(s) updated.\n", count);
  return EXECUTE_SUCCESS;
}

ExecuteResult execute_delete(Statement* statement, Table* table) {
  if (!statement->has_where) {
    uint32_t id = statement->target_id;
    Resolved r = resolve_key(table->memtable, &table->sstables, id);
    if (!r.present)
      return EXECUTE_NOT_FOUND;

    Entry tomb = { .id = id, .tombstone = true };
    sorted_run_upsert(table->memtable, &tomb);

    if (table->memtable->count >= MEMTABLE_FLUSH_THRESHOLD)
      flush_memtable(table);

    printf("1 row deleted.\n");
    return EXECUTE_SUCCESS;
  }

  /* bulk delete via tombstones */
  SortedRun* live = merge_all_live(table->memtable, &table->sstables);
  uint32_t count = 0;
  for (uint32_t i = 0; i < live->count; i++) {
    Entry* e = &live->entries[i];
    if (!row_matches_where(e, statement)) continue;
    Entry tomb = { .id = e->id, .tombstone = true };
    sorted_run_upsert(table->memtable, &tomb);
    count++;
  }
  sorted_run_free(live);

  if (table->memtable->count >= MEMTABLE_FLUSH_THRESHOLD)
    flush_memtable(table);

  printf("%u row(s) deleted.\n", count);
  return EXECUTE_SUCCESS;
}

ExecuteResult execute_select(Statement* statement, Table* table) {
  SortedRun* live = merge_all_live(table->memtable, &table->sstables);
  uint32_t match = 0;
  for (uint32_t i = 0; i < live->count; i++) {
    if (row_matches_where(&live->entries[i], statement)) {
      match++;
      if (!statement->is_count)
        print_entry(&live->entries[i]);
    }
  }
  sorted_run_free(live);

  if (statement->is_count)
    printf("%u row(s).\n", match);
  return EXECUTE_SUCCESS;
}

ExecuteResult execute_begin(Table* table) {
  if (table->tx.active)
    return EXECUTE_TX_ALREADY_ACTIVE;

  table->tx.memtable_copy = sorted_run_clone(table->memtable);
  table->tx.num_runs_copy = table->sstables.num_runs;
  for (uint32_t i = 0; i < table->sstables.num_runs; i++)
    table->tx.runs_copy[i] = sorted_run_clone(table->sstables.runs[i]);
  table->tx.active = true;
  return EXECUTE_SUCCESS;
}

ExecuteResult execute_commit(Table* table) {
  if (!table->tx.active)
    return EXECUTE_NO_ACTIVE_TX;

  /* discard snapshot */
  sorted_run_free(table->tx.memtable_copy);
  for (uint32_t i = 0; i < table->tx.num_runs_copy; i++)
    sorted_run_free(table->tx.runs_copy[i]);
  table->tx.active = false;
  table->tx.memtable_copy = NULL;
  table->tx.num_runs_copy = 0;
  return EXECUTE_SUCCESS;
}

ExecuteResult execute_rollback(Table* table) {
  if (!table->tx.active)
    return EXECUTE_NO_ACTIVE_TX;

  /* restore memtable */
  sorted_run_free(table->memtable);
  table->memtable = table->tx.memtable_copy;
  table->tx.memtable_copy = NULL;

  /* restore sstable list */
  for (uint32_t i = 0; i < table->sstables.num_runs; i++)
    sorted_run_free(table->sstables.runs[i]);
  table->sstables.num_runs = table->tx.num_runs_copy;
  for (uint32_t i = 0; i < table->tx.num_runs_copy; i++)
    table->sstables.runs[i] = table->tx.runs_copy[i];

  table->tx.active = false;
  table->tx.num_runs_copy = 0;
  return EXECUTE_SUCCESS;
}

ExecuteResult execute_create(Table* table) {
  (void)table;
  printf("Table created (schema is fixed: id, username, email).\n");
  return EXECUTE_SUCCESS;
}

ExecuteResult execute_statement(Statement* statement, Table* table) {
  switch (statement->type) {
    case STATEMENT_INSERT:   return execute_insert(statement, table);
    case STATEMENT_SELECT:   return execute_select(statement, table);
    case STATEMENT_UPDATE:   return execute_update(statement, table);
    case STATEMENT_DELETE:   return execute_delete(statement, table);
    case STATEMENT_CREATE:   return execute_create(table);
    case STATEMENT_BEGIN:    return execute_begin(table);
    case STATEMENT_COMMIT:   return execute_commit(table);
    case STATEMENT_ROLLBACK: return execute_rollback(table);
  }
  return EXECUTE_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  Parser (largely identical to original)                            */
/* ------------------------------------------------------------------ */

PrepareResult parse_where_clause(Statement* statement) {
  char* column = strtok(NULL, " ");
  char* op_str = strtok(NULL, " ");
  char* value  = strtok(NULL, " ");
  if (!column || !op_str || !value) return PREPARE_SYNTAX_ERROR;

  if (strcmp(column, "id") && strcmp(column, "username") && strcmp(column, "email"))
    return PREPARE_SYNTAX_ERROR;

  strncpy(statement->where_column, column, sizeof(statement->where_column) - 1);
  statement->where_column[sizeof(statement->where_column) - 1] = 0;

  if      (strcmp(op_str, "=")  == 0) statement->where_op = WHERE_EQ;
  else if (strcmp(op_str, "!=") == 0) statement->where_op = WHERE_NE;
  else if (strcmp(op_str, ">=") == 0) statement->where_op = WHERE_GE;
  else if (strcmp(op_str, "<=") == 0) statement->where_op = WHERE_LE;
  else if (strcmp(op_str, ">")  == 0) statement->where_op = WHERE_GT;
  else if (strcmp(op_str, "<")  == 0) statement->where_op = WHERE_LT;
  else return PREPARE_SYNTAX_ERROR;

  if (strcmp(column, "id") == 0) {
    statement->where_is_string = false;
    statement->where_int_value = (uint32_t)atoi(value);
  } else {
    statement->where_is_string = true;
    strncpy(statement->where_str_value, value, sizeof(statement->where_str_value) - 1);
    statement->where_str_value[sizeof(statement->where_str_value) - 1] = 0;
  }
  return PREPARE_SUCCESS;
}

PrepareResult prepare_insert(InputBuffer* ib, Statement* s) {
  s->type = STATEMENT_INSERT;
  strtok(ib->buffer, " ");
  char* id_s = strtok(NULL, " ");
  char* user = strtok(NULL, " ");
  char* mail = strtok(NULL, " ");
  if (!id_s || !user || !mail) return PREPARE_SYNTAX_ERROR;

  int id = atoi(id_s);
  if (id < 0) return PREPARE_NEGATIVE_ID;
  if (strlen(user) > COLUMN_USERNAME_SIZE) return PREPARE_STRING_TOO_LONG;
  if (strlen(mail) > COLUMN_EMAIL_SIZE) return PREPARE_STRING_TOO_LONG;

  s->row_to_insert.id = (uint32_t)id;
  strcpy(s->row_to_insert.username, user);
  strcpy(s->row_to_insert.email, mail);
  return PREPARE_SUCCESS;
}

PrepareResult prepare_update(InputBuffer* ib, Statement* s) {
  s->type = STATEMENT_UPDATE;
  s->has_where = false;
  s->is_set_update = false;
  s->set_username = false;
  s->set_email = false;

  strtok(ib->buffer, " ");
  char* next = strtok(NULL, " ");
  if (!next) return PREPARE_SYNTAX_ERROR;

  if (strcmp(next, "set") == 0) {
    s->is_set_update = true;
    while (1) {
      char* col = strtok(NULL, " ");
      if (!col) break;
      if (strcmp(col, "where") == 0) {
        PrepareResult r = parse_where_clause(s);
        if (r != PREPARE_SUCCESS) return r;
        s->has_where = true;
        break;
      }
      char* eq  = strtok(NULL, " ");
      char* val = strtok(NULL, " ");
      if (!eq || !val || strcmp(eq, "=") != 0) return PREPARE_SYNTAX_ERROR;

      if (strcmp(col, "username") == 0) {
        if (strlen(val) > COLUMN_USERNAME_SIZE) return PREPARE_STRING_TOO_LONG;
        s->set_username = true;
        strncpy(s->set_username_value, val, COLUMN_USERNAME_SIZE);
        s->set_username_value[COLUMN_USERNAME_SIZE] = 0;
      } else if (strcmp(col, "email") == 0) {
        if (strlen(val) > COLUMN_EMAIL_SIZE) return PREPARE_STRING_TOO_LONG;
        s->set_email = true;
        strncpy(s->set_email_value, val, COLUMN_EMAIL_SIZE);
        s->set_email_value[COLUMN_EMAIL_SIZE] = 0;
      } else {
        return PREPARE_SYNTAX_ERROR; /* cannot SET primary key */
      }
    }
    if (!s->set_username && !s->set_email) return PREPARE_SYNTAX_ERROR;
    return PREPARE_SUCCESS;
  }

  /* point form */
  char* id_s = next;
  char* user = strtok(NULL, " ");
  char* mail = strtok(NULL, " ");
  if (!id_s || !user || !mail) return PREPARE_SYNTAX_ERROR;
  int id = atoi(id_s);
  if (id < 0) return PREPARE_NEGATIVE_ID;
  if (strlen(user) > COLUMN_USERNAME_SIZE) return PREPARE_STRING_TOO_LONG;
  if (strlen(mail) > COLUMN_EMAIL_SIZE) return PREPARE_STRING_TOO_LONG;

  s->row_to_insert.id = (uint32_t)id;
  strcpy(s->row_to_insert.username, user);
  strcpy(s->row_to_insert.email, mail);
  return PREPARE_SUCCESS;
}

PrepareResult prepare_delete(InputBuffer* ib, Statement* s) {
  s->type = STATEMENT_DELETE;
  s->has_where = false;
  strtok(ib->buffer, " ");
  char* next = strtok(NULL, " ");
  if (!next) return PREPARE_SYNTAX_ERROR;

  if (strcmp(next, "where") == 0) {
    PrepareResult r = parse_where_clause(s);
    if (r != PREPARE_SUCCESS) return r;
    s->has_where = true;
    return PREPARE_SUCCESS;
  }
  int id = atoi(next);
  if (id < 0) return PREPARE_NEGATIVE_ID;
  s->target_id = (uint32_t)id;
  return PREPARE_SUCCESS;
}

PrepareResult prepare_select(InputBuffer* ib, Statement* s) {
  s->type = STATEMENT_SELECT;
  s->has_where = false;
  s->is_count = false;

  strtok(ib->buffer, " ");
  char* next = strtok(NULL, " ");
  if (!next) return PREPARE_SUCCESS;

  if (strcmp(next, "count") == 0) {
    s->is_count = true;
    next = strtok(NULL, " ");
    if (!next) return PREPARE_SUCCESS;
  }
  if (strcmp(next, "where") != 0) return PREPARE_SYNTAX_ERROR;
  PrepareResult r = parse_where_clause(s);
  if (r != PREPARE_SUCCESS) return r;
  s->has_where = true;
  return PREPARE_SUCCESS;
}

PrepareResult prepare_statement(InputBuffer* ib, Statement* s) {
  if (strncmp(ib->buffer, "insert", 6) == 0) return prepare_insert(ib, s);
  if (strncmp(ib->buffer, "select", 6) == 0) return prepare_select(ib, s);
  if (strncmp(ib->buffer, "update", 6) == 0) return prepare_update(ib, s);
  if (strncmp(ib->buffer, "delete", 6) == 0) return prepare_delete(ib, s);
  if (strncmp(ib->buffer, "create", 6) == 0) {
    s->type = STATEMENT_CREATE;
    return PREPARE_SUCCESS;
  }
  if (strcmp(ib->buffer, "begin") == 0)    { s->type = STATEMENT_BEGIN;    return PREPARE_SUCCESS; }
  if (strcmp(ib->buffer, "commit") == 0)   { s->type = STATEMENT_COMMIT;   return PREPARE_SUCCESS; }
  if (strcmp(ib->buffer, "rollback") == 0) { s->type = STATEMENT_ROLLBACK; return PREPARE_SUCCESS; }
  return PREPARE_UNRECOGNIZED_STATEMENT;
}

/* ------------------------------------------------------------------ */
/*  Meta commands & main loop                                         */
/* ------------------------------------------------------------------ */

void print_help() {
  printf("Meta commands:\n");
  printf("  .exit       close the database and quit\n");
  printf("  .help       show this message\n");
  printf("  .stats      memtable size + number of SSTables\n");
  printf("  .sstables   list SSTable sizes (newest first)\n");
  printf("  .flush      force memtable flush\n");
  printf("Statements:\n");
  printf("  create table ...\n");
  printf("  insert <id> <username> <email>\n");
  printf("  select [count] [where <col> <op> <val>]\n");
  printf("  update <id> <username> <email>\n");
  printf("  update set <col> = <val> [...] [where ...]\n");
  printf("  delete <id>\n");
  printf("  delete where <col> <op> <val>\n");
  printf("  begin | commit | rollback\n");
  printf("\nLSM notes:\n");
  printf("  - Writes go to an in-memory sorted memtable.\n");
  printf("  - When memtable reaches %d rows it is flushed as an SSTable.\n",
         MEMTABLE_FLUSH_THRESHOLD);
  printf("  - Deletes are tombstones; compaction removes them later.\n");
  printf("  - Reads merge newest → oldest runs.\n");
}

MetaCommandResult do_meta_command(InputBuffer* ib, Table* table) {
  if (strcmp(ib->buffer, ".exit") == 0) {
    db_close(table);
    free(ib->buffer);
    free(ib);
    exit(EXIT_SUCCESS);
  }
  if (strcmp(ib->buffer, ".help") == 0) {
    print_help();
    return META_COMMAND_SUCCESS;
  }
  if (strcmp(ib->buffer, ".stats") == 0) {
    printf("memtable entries : %u\n", table->memtable->count);
    printf("SSTables         : %u\n", table->sstables.num_runs);
    printf("in transaction   : %s\n", table->tx.active ? "yes" : "no");
    return META_COMMAND_SUCCESS;
  }
  if (strcmp(ib->buffer, ".sstables") == 0) {
    printf("SSTables (newest first):\n");
    for (uint32_t i = 0; i < table->sstables.num_runs; i++)
      printf("  [%u] %u entries\n", i, table->sstables.runs[i]->count);
    if (table->sstables.num_runs == 0)
      printf("  (none)\n");
    return META_COMMAND_SUCCESS;
  }
  if (strcmp(ib->buffer, ".flush") == 0) {
    flush_memtable(table);
    printf("Memtable flushed.\n");
    return META_COMMAND_SUCCESS;
  }
  return META_COMMAND_UNRECOGNIZED_COMMAND;
}

InputBuffer* new_input_buffer() {
  InputBuffer* ib = malloc(sizeof(InputBuffer));
  ib->buffer = NULL;
  ib->buffer_length = 0;
  ib->input_length = 0;
  return ib;
}

void print_prompt() { printf("db > "); }

void read_input(InputBuffer* ib) {
  ssize_t bytes = getline(&ib->buffer, &ib->buffer_length, stdin);
  if (bytes <= 0) {
    printf("Error reading input\n");
    exit(EXIT_FAILURE);
  }
  ib->input_length = bytes - 1;
  ib->buffer[bytes - 1] = 0;
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    printf("Must supply a database filename.\n");
    exit(EXIT_FAILURE);
  }

  Table* table = db_open(argv[1]);
  InputBuffer* ib = new_input_buffer();

  printf("LSM-Tree engine ready. Type .help for commands.\n");

  while (1) {
    print_prompt();
    read_input(ib);

    if (ib->buffer[0] == '.') {
      switch (do_meta_command(ib, table)) {
        case META_COMMAND_SUCCESS: continue;
        case META_COMMAND_UNRECOGNIZED_COMMAND:
          printf("Unrecognized command '%s'\n", ib->buffer);
          continue;
      }
    }

    Statement statement;
    switch (prepare_statement(ib, &statement)) {
      case PREPARE_SUCCESS: break;
      case PREPARE_NEGATIVE_ID:
        printf("ID must be positive.\n"); continue;
      case PREPARE_STRING_TOO_LONG:
        printf("String is too long.\n"); continue;
      case PREPARE_SYNTAX_ERROR:
        printf("Syntax error. Could not parse statement.\n"); continue;
      case PREPARE_UNRECOGNIZED_STATEMENT:
        printf("Unrecognized keyword at start of '%s'.\n", ib->buffer);
        continue;
    }

    switch (execute_statement(&statement, table)) {
      case EXECUTE_SUCCESS:
        printf("Executed.\n");
        break;
      case EXECUTE_DUPLICATE_KEY:
        printf("Error: Duplicate key.\n");
        break;
      case EXECUTE_NOT_FOUND:
        printf("Error: row not found.\n");
        break;
      case EXECUTE_TX_ALREADY_ACTIVE:
        printf("Error: a transaction is already active.\n");
        break;
      case EXECUTE_NO_ACTIVE_TX:
        printf("Error: no active transaction.\n");
        break;
    }
  }
  return 0;
}
