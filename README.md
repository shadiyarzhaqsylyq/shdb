## Educational databases in C/C++.
can use any column name
1 file 1 table
Flexible Primary Key - user_id INT PRIMARY KEY or product code INT PRIMARY KEY
VARCHAR(N), STRING(N), CHAR(N).
```
db=# CREATE TABLE employees (id INT PRIMARY KEY, name VARCHAR(32), salary INT, department VARCHAR(32), city VARCHAR(32)); do not support different names, can not change order of columns
db=# INSERT INTO employees VALUES (1, 'Alice', 50000, 'HR', 'New York');
db=# INSERT INTO employees VALUES (2, 'John', 60000, 'HR', 'Chicago');
db=# INSERT INTO employees VALUES (3, 'Bob', 70000, 'SE', 'Miami');
db=# INSERT INTO employees VALUES (4, 'Molly', 80000, 'SE', 'Chicago');
db=# INSERT INTO employees VALUES (5, 'Hector', 90000, 'Sales', 'New York');
db=# INSERT INTO employees VALUES (6, 'Sarah', 100000, 'Sales', 'Miami');

db=# INSERT INTO employees VALUES (7, 'Jack', 110000, 'Eng', 'SF');
db=# INSERT INTO employees VALUES (8, 'Sadie', 120000, 'Eng', 'Washington');


db=# SELECT * FROM employees;

db=# SELECT * FROM employees WHERE department = 'SE' AND salary > 50000;
db=# SELECT * FROM employees WHERE city = 'New York' OR city = 'Chicago';




SELECT * FROM employees 
WHERE (city = 'Miami' AND department = 'Sales') OR salary > 100000;

SELECT * FROM employees 
WHERE city = 'Miami' AND (department = 'SE' OR department = 'Sales');


UPDATE employees SET city = 'Miami', salary = 100000 WHERE id = 1;
UPDATE users SET salary = 75000 WHERE id >= 3 AND (city = 'New York' OR city = 'Chicago');

DELETE: Filters and deletes matching rows by any custom column condition.
DELETE FROM users WHERE id < 6;

Add 2 rows again:
db=# INSERT INTO employees VALUES (7, 'Jack', 110000, 'Eng', 'SF');
db=# INSERT INTO employees VALUES (8, 'Sadie', 120000, 'Eng', 'Washington');

DELETE FROM users WHERE id < 6 OR department = 'Sales';

DELETE FROM users WHERE id < 1 AND department = 'SE';


```

TRY
```

Example 1: Custom Columns & Custom Primary Key

CREATE TABLE users (user_id INT PRIMARY KEY, email VARCHAR(64), age INT, country VARCHAR(32));
INSERT INTO users VALUES (101, 'alice@test.com', 28, 'USA');
INSERT INTO users VALUES (102, 'bob@test.com', 35, 'Canada');


Example 2: Adding Many Custom Columns
CREATE TABLE products (product_code INT PRIMARY KEY, title VARCHAR(50), price INT, stock INT, category VARCHAR(32), brand VARCHAR(32));
INSERT INTO products VALUES (501, 'Laptop', 1200, 15, 'Electronics', 'TechCorp');


```
Not Implemented catalog, table_heap, page, buffer pool, disk manager, WAL/recovery, query optimizer, executor.
# C++
db=# \d
Tree:
- leaf (size 1)
  - 1

db=# \q

g++ -std=c++17 -O2 db.cpp -o db


./db mydb.db


# C


gcc -std=c99 -Wall -Wextra db.c -o db
