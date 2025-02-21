#!/bin/sh

set -x

seconds=1200
sed_script="/^$seconds s/q"

for pages in $(seq 16 -1 1); do
  for family in tcp unix; do
    for io in splicetee recvsend; do
      ./echo-server "$io" "$family" "$pages" | sed "$sed_script"
      rm -rf /tmp/echo-server-*
      mv log "$io-$family-$pages.log"
    done
  done
done


