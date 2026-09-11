# Using db.odin — examples

## Building and starting it

```bash
odin build . -out:dbshell
./dbshell mydata.db
```

`mydata.db` is created if it doesn't exist yet, and reopened (schema, tables,
and data intact) if it does. Every command below is typed at the `db=#`
prompt and ends with a semicolon.

---

## 1. Creating a table

```sql
CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(32), age INT);
```

- Every table needs exactly one `PRIMARY KEY` column, and it must be `INT`.
- `VARCHAR(n)` sets the max string length; if you omit `(n)` it defaults to 32.
- `INTEGER`, `STRING`, and `CHAR` are also accepted as synonyms for `INT`/`VARCHAR`.

```sql
CREATE TABLE orders (id INT PRIMARY KEY, user_id INT, amount INT);
```

A single `.db` file can hold multiple tables — see §5.

---

## 2. Insert, select, update, delete

```sql
INSERT INTO users VALUES (1, 'Alice', 30);
INSERT INTO users VALUES (2, 'Bob', 25);

SELECT * FROM users;
-- (1, 'Alice', 30)
-- (2, 'Bob', 25)

SELECT COUNT(*) FROM users;
-- 2 row(s).

UPDATE users SET age = 31 WHERE id = 1;

DELETE FROM users WHERE id = 2;
```

### WHERE clauses

Supports `=`, `!=` / `<>`, `<`, `>`, `<=`, `>=`, plus `AND`, `OR`, and
parentheses for grouping:

```sql
SELECT * FROM users WHERE age > 26;
SELECT * FROM users WHERE name = 'Alice' OR name = 'Bob';
SELECT * FROM users WHERE age >= 18 AND (name = 'Alice' OR age < 20);
SELECT COUNT(*) FROM users WHERE age != 30;
```

`UPDATE` and `DELETE` accept the same `WHERE` syntax and apply to every
matching row, not just one:

```sql
UPDATE users SET age = 0 WHERE age < 18;
DELETE FROM users WHERE age > 60;
```

### ORDER BY, LIMIT, OFFSET

`SELECT` (but not `SELECT COUNT(*)`, which always reports the total match
count) can sort and page its results:

```sql
SELECT * FROM users ORDER BY age;             -- ascending by default
SELECT * FROM users ORDER BY age DESC;
SELECT * FROM users ORDER BY name;             -- works on VARCHAR columns too

SELECT * FROM users LIMIT 10;
SELECT * FROM users ORDER BY age DESC LIMIT 5 OFFSET 5;   -- page 2, 5 per page

SELECT * FROM users WHERE age > 20 ORDER BY age DESC LIMIT 3;
```

---

## 3. Transactions

```sql
BEGIN;
INSERT INTO users VALUES (3, 'Carol', 22);
UPDATE users SET age = 40 WHERE id = 1;
ROLLBACK;          -- undoes both changes above
```

```sql
BEGIN;
INSERT INTO users VALUES (3, 'Carol', 22);
COMMIT;             -- makes it permanent
```

A transaction covers the *whole file* — every table, `CREATE TABLE`,
`DROP TABLE`, and `ALTER TABLE` included, not just one table's rows. Only
one transaction can be open at a time (`BEGIN` again before `COMMIT`/
`ROLLBACK` is an error).

---

## 4. Dropping a table

```sql
DROP TABLE orders;
```

Removes the table and its data. The name is immediately free to reuse in a
new `CREATE TABLE`, and the space it used gets recycled by later inserts
elsewhere in the file rather than left as dead weight. Like everything
else, this rolls back cleanly if done inside a `BEGIN` ... `ROLLBACK`.

---

## 5. Multiple tables in one file

```sql
CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(32));
CREATE TABLE orders (id INT PRIMARY KEY, user_id INT, amount INT);

INSERT INTO users  VALUES (1, 'Alice');
INSERT INTO orders VALUES (100, 1, 50);
INSERT INTO orders VALUES (101, 1, 75);

SELECT * FROM orders WHERE user_id = 1;
-- (100, 1, 50)
-- (101, 1, 75)
```

Each table has its own independent schema and B+tree; they just share the
one `.db` file. There's no `JOIN` — if you need to combine rows from two
tables, run separate `SELECT`s and match them up yourself (or ask me to add
`JOIN` support).

---

## 6. Changing a table's columns

```sql
ALTER TABLE users ADD COLUMN email VARCHAR(32) DEFAULT 'n/a';
SELECT * FROM users;
-- (1, 'Alice', 'n/a')

ALTER TABLE users ADD COLUMN age INT;     -- no DEFAULT given -> starts at 0
ALTER TABLE users DROP COLUMN email;
```

Notes:
- `COLUMN` is optional: `ALTER TABLE users ADD age INT;` works too.
- If you skip `DEFAULT`, new columns start as `0` (INT) or `''` (VARCHAR)
  for every existing row.
- You can't drop the primary key column, and you can't drop a table's last
  remaining column.
- This rewrites every row under the hood (column layout changes), so it
  touches the whole table — fine for a few thousand rows, not something to
  do in a hot loop.

---



The `[name]` is optional only when the file has exactly one table:

```
db=# .tables
  users (3 columns)
  orders (3 columns)
db=# .btree users
Tree (users):
- leaf (size 2)
  - 1
  - 2
db=# .constants orders
Constants (orders):
ROW_SIZE: 12
...
```

---

## Quick reference

```sql
CREATE TABLE <name> (<col> <type> [PRIMARY KEY], ...);
DROP TABLE <name>;
ALTER TABLE <name> ADD [COLUMN] <col> <type> [DEFAULT <value>];
ALTER TABLE <name> DROP [COLUMN] <col>;

INSERT INTO <name> VALUES (<v1>, <v2>, ...);
SELECT * FROM <name> [WHERE <expr>] [ORDER BY <col> [ASC|DESC]] [LIMIT <n> [OFFSET <n>]];
SELECT COUNT(*) FROM <name> [WHERE <expr>];
UPDATE <name> SET <col> = <val> [, <col2> = <val2> ...] [WHERE <expr>];
DELETE FROM <name> [WHERE <expr>];

BEGIN; | COMMIT; | ROLLBACK;
```
