## Educational databases in C. B+TREE and LSMTREE IMPLEMENTATION.
```

Example:
create table users
insert 1 alice alice@example.com
insert 2 bob bob@example.com
insert 3 carol carol@example.com

select
select where id = 2
select where username = alice
select where email != bob@example.com
select where id > 1
select where id >= 2
select where id < 3
select where id <= 2

select count
select count where id > 1
select count where username = alice

Update whole row
update 2 bobby bobby@example.com

update set username = bobby where id = 2
update set email = new@example.com where username = alice
update set username = newname email = new@example.com where id = 1
update set username = everyone

Point delete
delete 3

Delete with where
delete where id = 2
delete where username = alice
delete where id > 1
delete where email != carol@example.com

Transactions
begin
insert 10 dave dave@example.com
update set username = davey where id = 10
commit

begin
delete where id = 10
rollback

```
Implement lexer, parser, ast, catalog, table_heap, page, buffer pool, disk manager, WAL/recovery, query optimizer, executor.
