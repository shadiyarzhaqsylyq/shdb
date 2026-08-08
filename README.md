## Educational databases in C/C++.
can use any column name

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


UPDATE users SET email = 'bob@test.com', age = 30 WHERE user_id = 1;).

DELETE: Filters and deletes matching rows by any custom column condition.

```
Not Implemented catalog, table_heap, page, buffer pool, disk manager, WAL/recovery, query optimizer, executor.

db=# \d
Tree:
- leaf (size 1)
  - 1

db=# \q

g++ -std=c++17 -O2 db.cpp -o db


./db mydb.db

