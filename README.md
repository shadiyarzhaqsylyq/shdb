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

db=# SELECT * FROM employees;
(1, Alice, 50000, HR, New York)
(2, John, 60000, HR, Chicago)
(3, Bob, 70000, SE, Miami)
db=# SELECT * FROM employees WHERE department = 'SE' AND salary > 50000;
(3, Bob, 70000, SE, Miami)
db=# SELECT * FROM employees WHERE city = 'New York' OR city = 'Chicago';
(1, Alice, 50000, HR, New York)
(2, John, 60000, HR, Chicago)



SELECT * FROM employees 
WHERE (city = 'Miami' AND department = 'HR') OR salary > 70000;

SELECT * FROM employees 
WHERE city = 'Miami' AND (department = 'HR' OR department = 'Sales');

SELECT * FROM employees 
WHERE (city = 'Miami' AND name = 'Sarah') OR (id <= 1);


UPDATE employees SET city = 'Miami', salary = 100000 WHERE id = 1;

DELETE: Filters and deletes matching rows by any custom column condition.

```

Another Example
```

Example 1: Custom Columns & Custom Primary Key

CREATE TABLE users (user_id INT PRIMARY KEY, email VARCHAR(64), age INT, country VARCHAR(32));
INSERT INTO users VALUES (101, 'alice@test.com', 28, 'USA');
INSERT INTO users VALUES (102, 'bob@test.com', 35, 'Canada');

SELECT * FROM users WHERE age >= 30;
-- Output: (102, 'bob@test.com', 35, 'Canada')

UPDATE users SET email = 'alice_new@test.com' WHERE user_id = 101;
SELECT * FROM users WHERE user_id = 101;
-- Output: (101, 'alice_new@test.com', 28, 'USA')


Example 2: Adding Many Custom Columns
CREATE TABLE products (product_code INT PRIMARY KEY, title VARCHAR(50), price INT, stock INT, category VARCHAR(32), brand VARCHAR(32));
INSERT INTO products VALUES (501, 'Laptop', 1200, 15, 'Electronics', 'TechCorp');

SELECT * FROM products WHERE price > 1000;
-- Output: (501, 'Laptop', 1200, 15, 'Electronics', 'TechCorp')


-- Simple AND condition:
SELECT * FROM users WHERE age >= 21 AND country = 'USA';

-- Simple OR condition:
SELECT * FROM users WHERE country = 'USA' OR country = 'Canada';

-- Complex combined logic with parentheses ():
SELECT * FROM users WHERE salary > 50000 AND (department = 'Engineering' OR department = 'HR');

-- Updating rows with AND/OR conditions:
UPDATE users SET salary = 75000 WHERE age >= 30 AND (city = 'New York' OR city = 'Chicago');

-- Deleting rows with AND/OR conditions:
DELETE FROM users WHERE age < 18 OR country = 'Unknown';

```
Not Implemented catalog, table_heap, page, buffer pool, disk manager, WAL/recovery, query optimizer, executor.

db=# \d
Tree:
- leaf (size 1)
  - 1

db=# \q

g++ -std=c++17 -O2 db.cpp -o db


./db mydb.db

