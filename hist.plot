set style data histograms
set boxwidth 0.9 relative
set style fill solid 1.0 border -1

set title 'Histogram of Output Throughput'
set ylabel 'count'
set xlabel 'send/splice throughput (MB/s)'

plot 'recvsend-tcp-1.log.hist' u 2:xticlabels(sprintf("%.0f", $1)) title 'recvsend-tcp-1', 'recvsend-unix-1.log.hist' u 2:xticlabels(sprintf("%.0f", $1)) title 'recvsend-unix-1', 'splice-tcp-1.log.hist' u 2:xticlabels(sprintf("%.0f", $1)) title 'splice-tcp-1', 'splice-unix-1.log.hist' u 2:xticlabels(sprintf("%.0f", $1)) title 'splice-unix-1',
