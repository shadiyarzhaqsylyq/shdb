## Educational database in C.


can use any column name, types


1 file 1 table


Flexible Primary Key - user_id INT PRIMARY KEY or product code INT PRIMARY KEY


supported types

VARCHAR(N), STRING(N), CHAR(N), INT, INTEGER

VARCHAR - default 32
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

CREATE TABLE products (product_id INT PRIMARY KEY, product_name VARCHAR(50), category VARCHAR(32), price DECIMAL(8, 2), stock_quantity INT, warehouse_city VARCHAR(32)); or


CREATE TABLE products (product_id INT PRIMARY KEY, product_name VARCHAR, category VARCHAR, price FLOAT, stock_quantity INT, warehouse_city VARCHAR);

INSERT INTO products VALUES (101, 'Laptop', 'Electronics', 999.99, 45, 'Seattle');
INSERT INTO products VALUES (102, 'Desk Chair', 'Furniture', 150.00, 120, 'Austin');
INSERT INTO products VALUES (103, 'Monitor', 'Electronics', 299.50, 85, 'Seattle');
INSERT INTO products VALUES (104, 'Coffee Maker', 'Appliances', 79.99, 200, 'Denver');
INSERT INTO products VALUES (105, 'Standing Desk', 'Furniture', 450.00, 30, 'Austin');
INSERT INTO products VALUES (106, 'Wireless Mouse', 'Electronics', 25.00, 350, 'Seattle');
INSERT INTO products VALUES (107, 'Blender', 'Appliances', 60.00, 90, 'Denver');
INSERT INTO products VALUES (108, 'Mechanical Keyboard', 'Electronics', 110.00, 150, 'Boston');

```







Not Implemented catalog (tables, schemas, indexes), Buffer Pool Manager, LRU-K Replacer, Disk Scheduler, disk manager, WAL/recovery, query optimizer, executor.

db=# \d - prints Tree

db=# \q - exit




# C


gcc -Wall -Wextra db.c -o db

./db sql.db

./db sql
