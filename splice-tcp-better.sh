# david@carbon12:~/src/io_uring-forward $ head splice-tcp.by-pages
# 10 1812.43 159.981
# 11 1888.82 74.103
# 12 2243.52 57.8657

sqlite3 <<'END_SQL'
create table splice  (pages integer not null, mean real not null, stddev real not null);
create table recvsend(pages integer not null, mean real not null, stddev real not null);

.separator " "
.import splice-tcp.by-pages   splice
.import recvsend-tcp.by-pages recvsend

.mode tabs

select
  splice.pages as pages,
  (splice.mean - recvsend.mean) / recvsend.mean * 100.0 as percent_better_splice
from splice inner join recvsend
  on splice.pages = recvsend.pages
order by splice.pages;

END_SQL

