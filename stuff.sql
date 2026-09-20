create table server_stats(
    io_method text not null, -- 'splice' or 'recvsend'
    address_family text not null, -- 'tcp' or 'unix'
    io_pages integer not null, -- pages of memory per send/recv/splice
    offset_milliseconds real not null, -- timestamp of the run; each run starts at zero
    output_megabytes_per_second real not null, -- "megabyte" = "1,000,000 bytes"
    short_reads_per_second real not null,
    short_writes_echo_per_second real not null,
    cpu_user_milliseconds real not null,
    cpu_system_milliseconds real not null,
    minor_page_faults_per_second real not null,
    major_page_faults_per_second real not null,
    yields_per_second real not null,
    preempts_per_second real not null
);

select
  s.io_pages,
  avg((s.output_megabytes_per_second - rs.output_megabytes_per_second) / rs.output_megabytes_per_second * 100.00)
from server_stats s inner join server_stats rs
  on s.io_pages = rs.io_pages
where
  s.address_family = 'tcp' and
  s.io_method = 'splice' and 
  rs.address_family = 'tcp' and 
  rs.io_method = 'recvsend'
group by s.io_pages
order by s.io_pages;

with diffs as (
  select
    s.io_pages as io_pages,
    (s.output_megabytes_per_second - rs.output_megabytes_per_second) / rs.output_megabytes_per_second * 100.00) as splice_percent_better
  from server_stats s inner join server_stats rs
    on s.io_pages = rs.io_pages
  where
    s.address_family = 'tcp' and
    s.io_method = 'splice' and 
    rs.address_family = 'tcp' and 
    rs.io_method = 'recvsend'
), means as (
  select
    io_pages,
    avg(splice_percent_better) as splice_percent_better_average
  from diffs
  group by io_pages
)
select
  means.io_pages,
  means.splice_percent_better_average,
  sqrt(avg(power(diffs.splice_percent_better - means.splice_percent_better_average, 2)))
from diffs inner join means
  on diffs.io_pages = means.io_pages
group by
  means.io_pages,
  means.splice_percent_better_average
order by means.io_pages;



