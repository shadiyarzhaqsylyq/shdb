## Educational database in C.
can use any column name
1 file 1 table
Flexible Primary Key - user_id INT PRIMARY KEY or product code INT PRIMARY KEY
VARCHAR(N), STRING(N), CHAR(N).
```

Example1
CREATE TABLE employees (id INT PRIMARY KEY, name VARCHAR(32), salary INT, department VARCHAR(32), city VARCHAR(32));
INSERT INTO employees VALUES (1, 'Alice', 50000, 'HR', 'New York');
INSERT INTO employees VALUES (2, 'John', 60000, 'HR', 'Chicago');
INSERT INTO employees VALUES (3, 'Bob', 70000, 'SE', 'Miami');
INSERT INTO employees VALUES (4, 'Molly', 80000, 'SE', 'Chicago');
INSERT INTO employees VALUES (5, 'Hector', 90000, 'Sales', 'New York');
INSERT INTO employees VALUES (6, 'Sarah', 100000, 'Sales', 'Miami');
INSERT INTO employees VALUES (7, 'Jack', 110000, 'Eng', 'SF');
INSERT INTO employees VALUES (8, 'Sadie', 120000, 'Eng', 'Washington');


SELECT * FROM employees;

SELECT * FROM employees WHERE department = 'SE' AND salary > 50000;
SELECT * FROM employees WHERE city = 'New York' OR city = 'Chicago';




SELECT * FROM employees 
WHERE (city = 'Miami' AND department = 'Sales') OR salary > 100000;

SELECT * FROM employees 
WHERE city = 'Miami' AND (department = 'SE' OR department = 'Sales');


UPDATE employees SET city = 'Miami', salary = 100000 WHERE id = 1;
UPDATE users SET salary = 75000 WHERE id >= 3 AND (city = 'New York' OR city = 'Chicago');

DELETE: Filters and deletes matching rows by any custom column condition.
DELETE FROM employees WHERE id < 6;

Add 2 rows again:
INSERT INTO employees VALUES (7, 'Jack', 110000, 'Eng', 'SF');
INSERT INTO employees VALUES (8, 'Sadie', 120000, 'Eng', 'Washington');

DELETE FROM employees WHERE id > 6 OR department = 'Sales';

DELETE FROM employees WHERE id > 1 AND department = 'SE';




Example2

CREATE TABLE users (user_id INT PRIMARY KEY, email VARCHAR(64), age INT, country VARCHAR(32));
INSERT INTO users VALUES (101, 'alice@test.com', 28, 'USA');
INSERT INTO users VALUES (102, 'bob@test.com', 35, 'Canada');

Example3

CREATE TABLE products (product_code INT PRIMARY KEY, title VARCHAR(50), price INT, stock INT, category VARCHAR(32), brand VARCHAR(32));
INSERT INTO products VALUES (501, 'Laptop', 1200, 15, 'Electronics', 'TechCorp');
INSERT INTO products VALUES (502, 'Wireless Headphones', 250, 45, 'Electronics', 'SoundWave');
INSERT INTO products VALUES (503, 'Ergonomic Office Chair', 350, 20, 'Furniture', 'ErgoComfort');
INSERT INTO products VALUES (504, 'Stainless Steel Water Bottle', 25, 150, 'Kitchenware', 'HydroGear');
INSERT INTO products VALUES (505, 'Mechanical Gaming Keyboard', 110, 60, 'Electronics', 'KeyMaster');
INSERT INTO products VALUES (506, 'Running Shoes', 85, 80, 'Apparel', 'Stride');
INSERT INTO products VALUES (507, 'Smart Fitness Watch', 199, 35, 'Electronics', 'FitPulse');
INSERT INTO products VALUES (508, 'Organic Dark Roast Coffee', 18, 200, 'Groceries', 'RoastCraft');


```







Not Implemented catalog (tables, schemas, indexes), Buffer Pool Manager, LRU-K Replacer, Disk Scheduler, disk manager, WAL/recovery, query optimizer, executor.
# C++
db=# \d
Tree:
- leaf (size 1)
  - 1

db=# \q




# C


gcc -Wall -Wextra db.c -o db

./db sql.db
or
./db sql


