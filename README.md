## Educational database in C.


cdb.c - There is something wrong when I Create Table, Insert Data, Quit? When I open file after quitting, and use SELECT command it prints garbage. it is corrected in cdbv2.c



can use any column name, types


1 file 1 table


Flexible Primary Key - user_id INT PRIMARY KEY or product_code INT PRIMARY KEY


supported types

VARCHAR(N), STRING(N), CHAR(N), INT, INTEGER

VARCHAR, CHAR - default 32
```

CREATE TABLE products (product_id INT PRIMARY KEY, product_name VARCHAR, category VARCHAR, price INT, stock_quant INT, warehouse_city VARCHAR); or

CREATE TABLE products (product_id INT PRIMARY KEY, product_name CHAR, category CHAR, price INT, stock_quant INT, warehouse_city CHAR);


INSERT INTO products VALUES (101, 'Laptop', 'Electronics', 110, 10, 'New York');
INSERT INTO products VALUES (102, 'Smartphone', 'Electronics', 120, 20, 'Chicago');
INSERT INTO products VALUES (103, 'Headphones', 'Electronics', 130, 30, 'Miami');
INSERT INTO products VALUES (104, 'Desk Chair', 'Furniture', 140, 40, 'Chicago');
INSERT INTO products VALUES (105, 'Monitor', 'Electronics', 150, 50, 'New York');
INSERT INTO products VALUES (106, 'Keyboard', 'Electronics', 160, 60, 'Miami');
INSERT INTO products VALUES (107, 'Coffee Maker', 'Appliances', 170, 70, 'LA');
INSERT INTO products VALUES (108, 'Blender', 'Appliances', 180, 80, 'SF');
INSERT INTO products VALUES (109, 'Microwave', 'Appliances', 190, 90, 'LA');
INSERT INTO products VALUES (110, 'Standing Desk', 'Furniture', 200, 100, 'SF');


SELECT * FROM products;

SELECT * FROM products WHERE category = 'Appliances' AND product_id > 107;
SELECT * FROM products WHERE city = 'LA' OR city = 'SF';




SELECT * FROM products WHERE (city = 'LA' AND category = 'Appliances') OR stock_quant > 50;

SELECT * FROM products WHERE city = 'Chicago' AND (category = 'Electronics' OR category = 'Furniture');


UPDATE products SET city = 'SF', price = 1000 WHERE product_id = 101;
UPDATE products SET stock_quant = 1 WHERE product_id >= 106 AND (city = 'LA' OR city = 'SF');

DELETE: Filters and deletes matching rows by any custom column condition.
DELETE FROM products WHERE product_id < 106;

Add 2 rows again:
INSERT INTO products VALUES (107, 'Coffee Maker', 'Appliances', 170, 70, 'LA');
INSERT INTO products VALUES (108, 'Blender', 'Appliances', 180, 80, 'SF');


DELETE FROM products WHERE product_id > 6 OR city = '';

DELETE FROM products WHERE product_id > 1 AND city = '';

```







Not Implemented catalog (tables, schemas, indexes), Buffer Pool Manager, LRU-K Replacer, Disk Scheduler, disk manager, WAL/recovery, query optimizer, executor.



db=# \d - prints Tree

db=# \q - exit




# C


gcc -Wall -Wextra db.c -o db

./db sql.db

./db sql
