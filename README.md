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

INSERT INTO table VALUES ();


SELECT * FROM table;
output:



SELECT * FROM table WHERE a = '' AND b > 1001;
output:




SELECT * FROM table WHERE a = '' OR b = '';
output:




SELECT * FROM table WHERE (a = '' AND b = '') OR c < 10000;
output:




SELECT * FROM table WHERE a = '' AND (b = '' OR c = '');
output:



UDPATE

UPDATE table SET a = '', b = '' WHERE c >= '';
SELECT output:




UPDATE table SET a = 1 WHERE b <= 1002 AND (c = '' OR d = '');
SELECT output:



DELETE: Filters and deletes matching rows by any custom column condition.
DELETE from table WHERE a > '';
SELECT output:


UPDATE table SET a = 45 WHERE b = 1001;
SELECT output:






DELETE FROM table WHERE a < 30;
SELECT output:



DELETE FROM table WHERE a > b OR c;
SELECT output:


DELETE FROM table WHERE a > 10 AND c > 30;
SELECT output:


```





Implemented - Buffer Pool Manager, WA/Recovery


Not Implemented catalog (tables, schemas, indexes),LRU-K Replacer, Disk Scheduler, disk manager, query optimizer, executor.



db=# \d - prints Tree


db=# \q - exit




# C


gcc -Wall -Wextra db.c -o db


./db sql.db


./db sql
