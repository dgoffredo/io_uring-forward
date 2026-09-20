with diffs as (
  select
    s.io_pages as io_pages,
    (s.output_megabytes_per_second - rs.output_megabytes_per_second) / rs.output_megabytes_per_second * 100.0 as splice_percent_better
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
  means.splice_percent_better_average as splice_percent_better_mean,
  sqrt(avg(power(diffs.splice_percent_better - means.splice_percent_better_average, 2))) as standard_deviation
from diffs inner join means
  on diffs.io_pages = means.io_pages
group by
  means.io_pages,
  means.splice_percent_better_average
order by means.io_pages;

