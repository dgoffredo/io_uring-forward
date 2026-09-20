set title 'Output Throughput Versus Send/Recv/Splice Size'

set xlabel 'send/recv/splice size (pages)'
set ylabel 'output throughput (MB/s)'

set xtics 4
set grid

# set terminal svg size 1024,768 fixed enhanced font 'Arial,12' butt dashlength 1.0

plot 'recvsend-tcp.by-pages' with errorbars title 'recvsend-tcp', \
     'splice-tcp.by-pages' using ($1+0.1):2:3 with errorbars title 'splice-tcp'

