## Educational database in C/C++.





1 file 1 table


Flexible Primary Key - user_id INT PRIMARY KEY or product_code INT PRIMARY KEY


supported types

VARCHAR(N), STRING(N), CHAR(N), INT, INTEGER

VARCHAR, CHAR - default 32


Operators - >=, <=, =, <, >, !=, <>

!= and <> have the same meaning "not equal to".
```

*CREATE*
CREATE TABLE table (id INT PRIMARY KEY, name VARCHAR, did VARCHAR, dep VARCHAR, salary INT, city VARCHAR);

*INSERT*
INSERT INTO table VALUES (1, 'Alice', '1010A-1001a', 'Engineering', 12000, 'NY');
INSERT INTO table VALUES (2, 'Bob', '1010A-1001b', 'IT', 18500, 'LA');
INSERT INTO table VALUES (3, 'Charlie', '2020B-2001c', 'HR', 14000, 'SF');
INSERT INTO table VALUES (4, 'Diana', '2020B-2002d', 'Finance', 21000, 'Chicago');
INSERT INTO table VALUES (5, 'Evan', '3030C-3001e', 'HR', 13500, 'SF');



*CREATE*
CREATE TABLE table (id INT PRIMARY KEY, name VARCHAR, did VARCHAR, dep VARCHAR, salary INT, city VARCHAR);

*INSERT*
INSERT INTO table VALUES (1, 'Alice', '1010A-1001a', 'Engineering', 12000, 'NY');
INSERT INTO table VALUES (2, 'Bob', '1010A-1001b', 'IT', 18500, 'LA');
INSERT INTO table VALUES (3, 'Charlie', '2020B-2001c', 'HR', 14000, 'SF');
INSERT INTO table VALUES (4, 'Diana', '2020B-2002d', 'Finance', 21000, 'Chicago');
INSERT INTO table VALUES (5, 'Evan', '3030C-3001e', 'HR', 13500, 'SF');
output:
(1, 'Alice', '1010A-1001a', 'Engineering', 12000, 'NY')
(2, 'Bob', '1010A-1001b', 'IT', 18500, 'LA')
(3, 'Charlie', '2020B-2001c', 'HR', 14000, 'SF')
(4, 'Diana', '2020B-2002d', 'Finance', 21000, 'Chicago')
(5, 'Evan', '3030C-3001e', 'HR', 13500, 'SF')




*SELECT*
SELECT * FROM table;
SELECT * FROM table WHERE city = '';

Not supported
SELECT * FROM table WHERE a = '' AND b > '';
SELECT * FROM table WHERE a = '' OR city = '';
SELECT * FROM table WHERE (a = '' AND b = '') OR c < '';
SELECT * FROM table WHERE a = '' AND (b = '' OR d = '');



*Update*
UPDATE table SET a = '', b = '' WHERE c >= '';

Not supported
UPDATE table SET a = '' WHERE b <= '' AND (c = '' OR d = '');




*DELETE*
DELETE FROM table WHERE id > '';
DELETE FROM table WHERE id < '';

Not supported
DELETE FROM table WHERE a = '' OR city = 'b';
DELETE FROM table WHERE a = '' AND b = 'HR';
DELETE FROM table WHERE a >= '' AND b = '';
DELETE FROM table WHERE (a = '' AND b = '') OR c <= '';
DELETE FROM table WHERE a > '' AND (b = '' OR c = '');




```





Not Implemented - Buffer Pool Manager, WAL/Recovery, Catalog, LRU-K replacer, Disk Scheduler, Disk Manager, query optimizer, executor.



db=# \d - prints Tree


db=# \q - exit




# C(db.c)


gcc -Wall -Wextra db.c -o db


./db sql.db


./db sql


# C++(dbv2.cpp)

g++ -Wall -Wextra .cpp -o db

./db sql.db

./db sql
