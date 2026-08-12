## Educational database in C/C++.
## C++ db does not have transactions, !=, <>, AND and OR commands.




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
INSERT INTO table VALUES (6, 'Fiona', '3030C-3002f', 'Engineering', 16800, 'Chicago');
INSERT INTO table VALUES (7, 'George', '4040D-4001g', 'Finance', 19500, 'NY');
INSERT INTO table VALUES (8, 'Hannah', '4040D-4002h', 'IT', 17200, 'Miami');




*SELECT*
SELECT * FROM table;
SELECT * FROM table WHERE city = 'SF';
SELECT * FROM table WHERE city = 'NY' AND salary > 10000;
SELECT * FROM table WHERE city = 'Miami' OR city = 'SF';
SELECT * FROM table WHERE (dep = 'HR' AND city = 'SF') OR id < 3;
SELECT * FROM table WHERE city = 'NY' AND (dep = 'Engineering' OR dep = 'Finance');



*Update*

Example1
INSERT INTO table VALUES (1, 'Alice', '1010A-1001a', 'Finance', 9000, 'NY');
INSERT INTO table VALUES (2, 'Bob', '1010A-1001b', 'IT', 8500, 'LA');
INSERT INTO table VALUES (3, 'Charlie', '2020B-2001c', 'HR', 4000, 'SF');
INSERT INTO table VALUES (4, 'Diana', '2020B-2002d', 'Engineering', 18000, 'Chicago');
INSERT INTO table VALUES (5, 'Evan', '3030C-3001e', 'HR', 13500, 'SF');

UPDATE table SET name = 'Gordon', did = 'xxxx-xxxx' WHERE id >= 4;
UPDATE table SET salary = 99999 WHERE salary <= 10000 AND (dep = 'Engineering' OR dep = 'Finance');




*DELETE*

Example1
INSERT INTO table VALUES (1, 'Alice', '1010A-1001a', 'Finance', 9000, 'NY');
INSERT INTO table VALUES (2, 'Bob', '1010A-1001b', 'IT', 8500, 'LA');
INSERT INTO table VALUES (3, 'Charlie', '2020B-2001c', 'HR', 14000, 'SF');
INSERT INTO table VALUES (4, 'Diana', '2020B-2002d', 'Engineering', 21000, 'Chicago');
INSERT INTO table VALUES (5, 'Evan', '3030C-3001e', 'HR', 13500, 'SF');


DELETE FROM table WHERE id > 1;
DELETE FROM table WHERE id < 5;
DELETE FROM table WHERE city = 'SF' OR city = 'NY';
DELETE FROM table WHERE city = 'SF' AND dep = 'HR';
DELETE FROM table WHERE salary >= 10000 AND dep = 'HR';


Example2
INSERT INTO table VALUES (1, 'Alice', '1010A-1001a', 'Finance', 10000, 'NY');
INSERT INTO table VALUES (2, 'Bob', '1010A-1001b', 'IT', 10, 'LA');
INSERT INTO table VALUES (3, 'Charlie', '2020B-2001c', 'HR', 1000, 'SF');
INSERT INTO table VALUES (4, 'Diana', '2020B-2002d', 'Engineering', 10, 'Chicago');
INSERT INTO table VALUES (5, 'Evan', '3030C-3001e', 'HR', 1000, 'SF');

DELETE FROM table WHERE (dep = 'HR' AND city = 'SF') OR salary <= 100;


Example3
INSERT INTO table VALUES (1, 'Alice', '1010A-1001a', 'Finance', 100, 'NY');
INSERT INTO table VALUES (2, 'Bob', '1010A-1001b', 'IT', 100, 'LA');
INSERT INTO table VALUES (3, 'Charlie', '2020B-2001c', 'HR', 2000, 'SF');
INSERT INTO table VALUES (4, 'Diana', '2020B-2002d', 'Engineering', 21000, 'LA');
INSERT INTO table VALUES (5, 'Evan', '3030C-3001e', 'HR', 3000, 'Miami');


DELETE FROM table WHERE salary > 1000 AND (city = 'LA' OR city = 'SF');


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
