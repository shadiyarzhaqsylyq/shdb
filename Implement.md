# X - is not implemented, + - is implemented

## AND, OR

```
db=# CREATE TABLE employees (id INT PRIMARY KEY, name VARCHAR(32), salary INT, department VARCHAR(32), city VARCHAR(32));
db=# INSERT INTO employees VALUES (1, 'Alice', 50000, 'HR', 'New York');
db=# INSERT INTO employees VALUES (2, 'John', 60000, 'HR', 'Chicago');
db=# INSERT INTO employees VALUES (3, 'Bob', 70000, 'SE', 'Miami');
db=# INSERT INTO employees VALUES (4, 'Molly', 80000, 'SE', 'Chicago');
db=# INSERT INTO employees VALUES (5, 'Hector', 90000, 'Sales', 'New York');
db=# INSERT INTO employees VALUES (6, 'Sarah', 100000, 'Sales', 'Miami');



SELECT * FROM employees 
WHERE department = 'SE' AND salary > 50000;

SELECT * FROM employees 
WHERE city = 'New York' OR city = 'Chicago';


SELECT * FROM employees 
WHERE (city = 'Miami' AND department = 'HR') OR salary > 70000;

SELECT * FROM employees 
WHERE city = 'Miami' AND (department = 'HR' OR department = 'Sales');



SELECT id, name, city FROM employees
WHERE (city = 'Miami' AND name = 'Sarah') OR (id <= 1);


SELECT id, name, department 
FROM employees 
WHERE department = 'SE' AND (price < 2 OR price > 4);


```
