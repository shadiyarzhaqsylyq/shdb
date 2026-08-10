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

CREATE TABLE table (id INT PRIMARY KEY, name VARCHAR, did VARCHAR, department VARCHAR, salary INT, city VARCHAR);



INSERT INTO table VALUES (1, 'Alice', '1010A-1001a', 'Finance', 12000, 'NY');
INSERT INTO table VALUES (2, 'Bob', '0101A-1001a', 'IT', 18500, 'LA');
INSERT INTO table VALUES (3, 'Charlie', '2020B-2001b', 'HR', 14000, 'SF');
INSERT INTO table VALUES (4, 'Diana', '0202B-2002b', 'Engineering', 21000, 'Chicago');
INSERT INTO table VALUES (5, 'Evan', '3030C-3001c', 'HR', 13500, 'SF');
INSERT INTO table VALUES (6, 'Fiona', '0303C-3002c', 'Engineering', 16800, 'Chicago');
INSERT INTO table VALUES (7, 'George', '4040D-4001d', 'Finance', 19500, 'NY');
INSERT INTO table VALUES (8, 'Hannah', '0404D-4002d', 'IT', 17200, 'Miami');
INSERT INTO table VALUES (9, 'John', '5050F-5001f', 'Sales', 10000, 'LA');
INSERT INTO table VALUES (10, 'Artur', '0505F_5002f', 'Sales, 11000, 'Miami');



SELECT * FROM table;
SELECT * FROM table WHERE a = '';
SELECT * FROM table WHERE a = '' AND b > '';
SELECT * FROM table WHERE a = '' OR b = '';
SELECT * FROM table WHERE (a = '' AND b = '') OR c < '';
SELECT * FROM table WHERE a = '' AND (b = '' OR c = '');



UPDATE table SET a = '', b = '' WHERE c >= '';
UPDATE table SET a = 1 WHERE b <= '' AND (c = '' OR d = '');





DELETE from table WHERE a > '';
DELETE FROM table WHERE a < '';
DELETE FROM table WHERE a > '' OR c = '';
DELETE FROM table WHERE a > '' AND c > '';





```





Implemented - Buffer Pool Manager, WA/Recovery


Not Implemented catalog (tables, schemas, indexes),LRU-K Replacer, Disk Scheduler, disk manager, query optimizer, executor.



db=# \d - prints Tree


db=# \q - exit




# C


gcc -Wall -Wextra db.c -o db


./db sql.db


./db sql
