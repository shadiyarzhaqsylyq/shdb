## Educational database in C.


cdb.c - There is something wrong when I Create Table, Insert Data, Quit? When I open file after quitting, and use SELECT command it prints garbage. it is corrected in cdbv2.c



can use any column name, types


1 file 1 table


Flexible Primary Key - user_id INT PRIMARY KEY or product_code INT PRIMARY KEY


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




CREATE TABLE products (product_id INT PRIMARY KEY, product_name VARCHAR, category VARCHAR, price INT, stock_quant INT, warehouse_city VARCHAR); or

CREATE TABLE products (product_id INT PRIMARY KEY, product_name CHAR, category CHAR, price INT, stock_quant INT, warehouse_city CHAR);


INSERT INTO products VALUES (101, 'Laptop', 'Electronics', 1200, 45, 'New York');
INSERT INTO products VALUES (102, 'Smartphone', 'Electronics', 800, 120, 'Chicago');
INSERT INTO products VALUES (103, 'Headphones', 'Electronics', 150, 200, 'Miami');
INSERT INTO products VALUES (104, 'Desk Chair', 'Furniture', 250, 35, 'Chicago');
INSERT INTO products VALUES (105, 'Monitor', 'Electronics', 350, 80, 'New York');
INSERT INTO products VALUES (106, 'Keyboard', 'Electronics', 75, 150, 'Miami');
INSERT INTO products VALUES (107, 'Coffee Maker', 'Appliances', 90, 60, 'LA');
INSERT INTO products VALUES (108, 'Blender', 'Appliances', 60, 90, 'LA');

SELECT * FROM products;

SELECT * FROM products WHERE category = 'Electronics' AND product_id > 104;
SELECT * FROM products WHERE city = 'New York' OR city = 'Chicago';




SELECT * FROM products WHERE (city = 'LA' AND category = 'Appliences') OR price > 100;

SELECT * FROM employees WHERE city = '' AND (category = 'SE' OR category = 'Sales');


UPDATE employees SET city = 'Miami', salary = 100000 WHERE id = 1;
UPDATE users SET salary = 75000 WHERE id >= 3 AND (city = 'New York' OR city = 'Chicago');

DELETE: Filters and deletes matching rows by any custom column condition.
DELETE FROM employees WHERE id < 6;

Add 2 rows again:
INSERT INTO employees VALUES (7, 'Jack', 110000, 'Eng', 'SF');
INSERT INTO employees VALUES (8, 'Sadie', 120000, 'Eng', 'Washington');

DELETE FROM employees WHERE id > 6 OR department = 'Sales';

DELETE FROM employees WHERE id > 1 AND department = 'SE';

```







Not Implemented catalog (tables, schemas, indexes), Buffer Pool Manager, LRU-K Replacer, Disk Scheduler, disk manager, WAL/recovery, query optimizer, executor.



db=# \d - prints Tree

db=# \q - exit




# C


gcc -Wall -Wextra db.c -o db

./db sql.db

./db sql
