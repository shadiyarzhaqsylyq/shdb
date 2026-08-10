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


CREATE TABLE table (id INT PRIMARY KEY, name CHAR, department CHAR, salary INT, number CHAR, city CHAR);

INSERT INTO table VALUES (1, 'Alice B.', 'Finance', 12500, '8705-355-1234', 'NY');
INSERT INTO table VALUES (2, 'Bob J.', 'IT', 18500, '8701-555-1234', 'LA');
INSERT INTO table VALUES (3, 'Charlie Jr.', 'HR', 14000, '8771-444-5566', 'SF');
INSERT INTO table VALUES (4, 'Diana A.', 'Engineering', 21000, '8702-333-8899', 'Chicago');
INSERT INTO table VALUES (5, 'Evan X.', 'HR', 13500, '8775-666-7788', 'SF');
INSERT INTO table VALUES (6, 'Fiona Z.', 'Sales', 16800, '8705-222-1100', 'Chicago');
INSERT INTO table VALUES (7, 'George W.', 'Finance', 19500, '8777-999-0011', 'NY');
INSERT INTO table VALUES (8, 'Hannah O.', 'IT', 17200, '8708-444-3322', 'Miami');


SELECT * FROM table;
SELECT * FROM table WHERE a = '' AND b > '';
SELECT * FROM table WHERE a = '' OR b = '';
SELECT * FROM table WHERE (a = '' AND b = '') OR c < 10000;
SELECT * FROM table WHERE a = '' AND (b = '' OR c = '');



UPDATE table SET a = '', b = '' WHERE c >= '';
UPDATE table SET a = 1 WHERE b <= 1002 AND (c = '' OR d = '');





DELETE from table WHERE a > '';
UPDATE table SET a = 45 WHERE b = 1001;
DELETE FROM table WHERE a < 30;
DELETE FROM table WHERE a > b OR c = '';
DELETE FROM table WHERE a > 10 AND c > 30;




```





Implemented - Buffer Pool Manager, WA/Recovery


Not Implemented catalog (tables, schemas, indexes),LRU-K Replacer, Disk Scheduler, disk manager, query optimizer, executor.



db=# \d - prints Tree


db=# \q - exit




# C


gcc -Wall -Wextra db.c -o db


./db sql.db


./db sql
