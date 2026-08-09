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

CREATE TABLE aircraft_fleet (aircraft_code INT PRIMARY KEY, model_name CHAR, category CHAR, hoc INT, active_fleet INT, primary_hub CHAR);

INSERT INTO aircraft_fleet VALUES (1001, 'Boeing 737 MAX 8', 'Narrowbody', 4200, 45, 'Dallas');
INSERT INTO aircraft_fleet VALUES (1002, 'Airbus A350-900', 'Widebody', 9800, 18, 'London');
INSERT INTO aircraft_fleet VALUES (1003, 'Embraer E190', 'Regional', 2900, 32, 'Chicago');
INSERT INTO aircraft_fleet VALUES (1004, 'Gulfstream G650', 'Private Jet', 5600, 8, 'London');
INSERT INTO aircraft_fleet VALUES (1005, 'Airbus A320neo', 'Narrowbody', 3900, 60, 'Atlanta');
INSERT INTO aircraft_fleet VALUES (1006, 'Boeing 777-300ER', 'Widebody', 11500, 14, 'New York');
INSERT INTO aircraft_fleet VALUES (1007, 'Bombardier CRJ900', 'Regional', 3100, 25, 'Chicago');
INSERT INTO aircraft_fleet VALUES (1008, 'Boeing 747-8F', 'Cargo Widebody', 14200, 10, 'Atlanta');
INSERT INTO aircraft_fleet VALUES (1009, 'Cessna Citation X', 'Private Jet', 4100, 12, 'New York');
INSERT INTO aircraft_fleet VALUES (1010, 'Airbus A330-900neo', 'Widebody', 8900, 22, 'Dallas');


SELECT * FROM aircraft_fleet;
output:
(1001, 'Boeing 737 MAX 8', 'Narrowbody', 4200, 45, 'Dallas')
(1002, 'Airbus A350-900', 'Widebody', 9800, 18, 'London')
(1003, 'Embraer E190', 'Regional', 2900, 32, 'Chicago')
(1004, 'Gulfstream G650', 'Private Jet', 5600, 8, 'London')
(1005, 'Airbus A320neo', 'Narrowbody', 3900, 60, 'Atlanta')
(1006, 'Boeing 777-300ER', 'Widebody', 11500, 14, 'New York')
(1007, 'Bombardier CRJ900', 'Regional', 3100, 25, 'Chicago')
(1008, 'Boeing 747-8F', 'Cargo Widebody', 14200, 10, 'Atlanta')
(1009, 'Cessna Citation X', 'Private Jet', 4100, 12, 'New York')
(1010, 'Airbus A330-900neo', 'Widebody', 8900, 22, 'Dallas')


SELECT * FROM aircraft_fleet WHERE category = 'Widebody' AND aircraft_code > 1001;
output:
(1002, 'Airbus A350-900', 'Widebody', 9800, 18, 'London')
(1006, 'Boeing 777-300ER', 'Widebody', 11500, 14, 'New York')
(1010, 'Airbus A330-900neo', 'Widebody', 8900, 22, 'Dallas')

SELECT * FROM products WHERE city = 'Dallas' OR city = 'New York;
output:
(1001, 'Boeing 737 MAX 8', 'Narrowbody', 4200, 45, 'Dallas')
(1006, 'Boeing 777-300ER', 'Widebody', 11500, 14, 'New York')
(1009, 'Cessna Citation X', 'Private Jet', 4100, 12, 'New York')
(1010, 'Airbus A330-900neo', 'Widebody', 8900, 22, 'Dallas')


SELECT * FROM products WHERE (city = '' AND category = 'Norrowbody') OR hoc < 60;
output:




SELECT * FROM products WHERE city = 'Chicago' AND (category = 'Electronics' OR category = 'Furniture');
output:



UPDATE products SET city = 'Boston', price = 1000 WHERE product_id >= 106;
output:




UPDATE products SET stock_quant = 1 WHERE stock_quant <= 50 AND (city = 'Chicago' OR city = 'New York');
output:




DELETE: Filters and deletes matching rows by any custom column condition.
DELETE FROM products WHERE product_id < 106;
output:



DELETE FROM products WHERE price > 50 OR city = 'Chicago';
output:


DELETE FROM products WHERE product_id > 103 AND stock_quant > 1;
output:


```







Not Implemented catalog (tables, schemas, indexes), Buffer Pool Manager, LRU-K Replacer, Disk Scheduler, disk manager, WAL/recovery, query optimizer, executor.



db=# \d - prints Tree

db=# \q - exit




# C


gcc -Wall -Wextra db.c -o db

./db sql.db

./db sql
