## Educational database in Odin.





1 file 1 table


Flexible Primary Key - user_id INT PRIMARY KEY or product_code INT PRIMARY KEY

Types


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
INSERT INTO table VALUES (3, 'Charlie', '2020B-2001a', 'HR', 4000, 'SF');
INSERT INTO table VALUES (4, 'Diana', '2020B-2001b', 'Finance', 21000, 'LA');
INSERT INTO table VALUES (5, 'Evan', '3030C-3001a', 'HR', 3500, 'SF');
INSERT INTO table VALUES (6, 'Lex', '3030C-3001b', 'HR', 11000, 'NY');


*SELECT*
SELECT * FROM table;
SELECT * FROM table WHERE a = '';
SELECT * FROM table WHERE a = '' AND b > '';
SELECT * FROM table WHERE a = '' OR b = '';


SELECT * FROM table WHERE (a = '' AND b = '') OR c < '';
SELECT * FROM table WHERE a = '' AND (b = '' OR d = '');



*Update*
UPDATE table SET a = '', b = '' WHERE c >= '';


UPDATE table SET a = '' WHERE b <= '' AND (c = '' OR d = '');




*DELETE*
DELETE FROM table WHERE a > '';
DELETE FROM table WHERE a < '';


DELETE FROM table WHERE a = '' OR b = '';
DELETE FROM table WHERE a = '' AND b = '';
DELETE FROM table WHERE (a = '' AND b = '') OR c <= '';
DELETE FROM table WHERE a > '' AND (b = '' OR c = '');


```





Not Implemented - Buffer Pool Manager, WAL/Recovery, Catalog, LRU-K replacer, Disk Scheduler, Disk Manager, query optimizer, executor, No free-page list, database wide transactions, Joins, foreign keys, USE statement


gcc -Wall -Wextra db.c -o prog



db=# \d - prints Tree


db=# \q - exit






./db db.sql


./db sql


