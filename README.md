## Educational database in C.


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

CREATE TABLE users (user_id INT PRIMARY KEY, email VARCHAR(64), age INT, country VARCHAR(32));
INSERT INTO users VALUES (101, 'alice@test.com', 28, 'USA');
INSERT INTO users VALUES (102, 'bob@test.com', 35, 'Canada');

Example3


CREATE TABLE products (product_id INT PRIMARY KEY, product_name VARCHAR, category VARCHAR, price INT, stock_quant INT, warehouse_city VARCHAR);
INSERT INTO products VALUES (101, 'Laptop', 'Electronics', 1200, 45, 'New York');
INSERT INTO products VALUES (102, 'Smartphone', 'Electronics', 800, 120, 'Chicago');
INSERT INTO products VALUES (103, 'Headphones', 'Electronics', 150, 200, 'Miami');
INSERT INTO products VALUES (104, 'Desk Chair', 'Furniture', 250, 35, 'Chicago');
INSERT INTO products VALUES (105, 'Monitor', 'Electronics', 350, 80, 'New York');
INSERT INTO products VALUES (106, 'Keyboard', 'Electronics', 75, 150, 'Miami');
INSERT INTO products VALUES (107, 'Coffee Maker', 'Appliances', 90, 60, 'LA');
INSERT INTO products VALUES (108, 'Blender', 'Appliances', 60, 90, 'Washington');

SELECT * FROM products;



Example4

CREATE TABLE books (id INT PRIMARY KEY, title CHAR, price INT, genre CHAR, author CHAR);

INSERT INTO books VALUES (1, 'The Great Gatsby', 15, 'Fiction', 'F. Scott Fitzgerald');
INSERT INTO books VALUES (2, '1984', 20, 'Sci-Fi', 'George Orwell');
INSERT INTO books VALUES (3, 'To Kill a Mockingbird', 18, 'Fiction', 'Harper Lee');
INSERT INTO books VALUES (4, 'Dune', 25, 'Sci-Fi', 'Frank Herbert');
INSERT INTO books VALUES (5, 'Sapiens', 30, 'Non-Fiction', 'Yuval Noah Harari');
INSERT INTO books VALUES (6, 'Atomic Habits', 22, 'Non-Fiction', 'James Clear');
INSERT INTO books VALUES (7, 'The Hobbit', 16, 'Fantasy', 'J.R.R. Tolkien');
INSERT INTO books VALUES (8, 'Harry Potter', 28, 'Fantasy', 'J.K. Rowling');
```







Not Implemented catalog (tables, schemas, indexes), Buffer Pool Manager, LRU-K Replacer, Disk Scheduler, disk manager, WAL/recovery, query optimizer, executor.

db=# \d - prints Tree

db=# \q - exit




# C


gcc -Wall -Wextra db.c -o db

./db sql.db

./db sql
