## Educational database in C.


cdb.c - There is something wrong when I Create Table, Insert Data, Quit? When I open file after quitting, and use SELECT command it prints garbage. it is corrected in cdbv2.c



can use any column name, types


1 file 1 table


Flexible Primary Key - user_id INT PRIMARY KEY or product_code INT PRIMARY KEY


supported types

VARCHAR(N), STRING(N), CHAR(N), INT, INTEGER

VARCHAR, CHAR - default 32
Operators - >=, <=, =, <, >
```

*CREATE*
CREATE TABLE table (id INT PRIMARY KEY, name VARCHAR, did VARCHAR, dep VARCHAR, salary INT, city VARCHAR);

*INSERT*
INSERT INTO table VALUES (1, 'Alice', '1010A-1001a', 'Finance', 12000, 'NY');
INSERT INTO table VALUES (2, 'Bob', '1010A-1001b', 'IT', 18500, 'LA');
INSERT INTO table VALUES (3, 'Charlie', '2020B-2001a', 'HR', 14000, 'SF');
INSERT INTO table VALUES (4, 'Diana', '2020B-2002b', 'Engineering', 21000, 'Chicago');
INSERT INTO table VALUES (5, 'Evan', '3030C-3001a', 'HR', 13500, 'SF');
INSERT INTO table VALUES (6, 'Fiona', '3030C-3002b', 'Engineering', 16800, 'Chicago');
INSERT INTO table VALUES (7, 'George', '4040D-4001a', 'Finance', 19500, 'NY');
INSERT INTO table VALUES (8, 'Hannah', '4040D-4002b', 'IT', 17200, 'Miami');
INSERT INTO table VALUES (9, 'John', '5050F-5001a', 'Sales', 10000, 'LA');
INSERT INTO table VALUES (10, 'Artur', '5050F-5002b', 'Sales', 11000, 'Miami');




*SELECT*
Table for testing SELECT
INSERT INTO table VALUES (1, 'Alice', '1010A-1001a', 'Finance', 12000, 'NY');
INSERT INTO table VALUES (2, 'Bob', '1010A-1001b', 'IT', 1300, 'LA');
INSERT INTO table VALUES (3, 'Charlie', '2020B-2001a', 'HR', 7000, 'SF');
INSERT INTO table VALUES (4, 'Diana', '2020B-2002b', 'Eng', 13000, 'NY');
INSERT INTO table VALUES (5, 'Evan', '3030C-3001a', 'HR', 8000, 'SF');

SELECT * FROM table;
SELECT * FROM table WHERE city = 'SF';
SELECT * FROM table WHERE city = 'NY' AND salary > 10000;
SELECT * FROM table WHERE city = 'NY' OR city = 'SF';
SELECT * FROM table WHERE (dep = 'HR' AND city = 'SF') OR id < 3;
SELECT * FROM table WHERE city = 'NY' AND (dep = 'Eng' OR dep = 'Finance');



*Update*
Table for testing UPDATE
INSERT INTO table VALUES (1, 'Alice', '1010A-1001a', 'Finance', 12000, 'NY');
INSERT INTO table VALUES (2, 'Bob', '1010A-1001b', 'IT', 1300, 'SF');
INSERT INTO table VALUES (3, 'Charlie', '2020B-2001a', 'HR', 7000, 'SF');
INSERT INTO table VALUES (4, 'Diana', '2020B-2002b', 'Eng', 13000, 'NY');
INSERT INTO table VALUES (5, 'Artur', '5050F-5002b', 'Finance', 11000, 'Miami');

UPDATE table SET name = 'Gordon', did = 'xxxx-xxxx' WHERE id >= '4';
UPDATE table SET id = 101 WHERE id <= 5 AND (dep = 'Eng' OR dep = 'Finance');




*DELETE*
Table for testing DELETE
INSERT INTO table VALUES (1, 'Alice', '1010A-1001a', 'Finance', 12000, 'NY');
INSERT INTO table VALUES (2, 'Bob', '1010A-1001b', 'HR', 1300, 'SF');
INSERT INTO table VALUES (3, 'Charlie', '2020B-2001a', 'HR', 7000, 'SF');
INSERT INTO table VALUES (4, 'Diana', '2020B-2002b', 'Eng', 13000, 'LA');
INSERT INTO table VALUES (5, 'Artur', '5050F-5002b', 'Finance', 11000, 'Miami');


DELETE from table WHERE id > 3;
DELETE FROM table WHERE id < 4;
DELETE FROM table WHERE city = 'SF' OR city = 'NY';
DELETE FROM table WHERE city = 'SF' AND dep = 'HR';
DELETE FROM table WHERE salary > 10000 AND salary < 13000;
SELECT * FROM table WHERE (a >= '' AND b = '') OR c = '';
SELECT * FROM table WHERE a >= '' AND (b = '' OR c = '');


```





Implemented - Buffer Pool Manager, WA/Recovery


Not Implemented catalog (tables, schemas, indexes),LRU-K Replacer, Disk Scheduler, disk manager, query optimizer, executor.



db=# \d - prints Tree


db=# \q - exit




# C


gcc -Wall -Wextra db.c -o db


./db sql.db


./db sql
