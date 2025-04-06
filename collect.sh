#!/bin/sh

set -x

lines=240
sed_script="${lines}q"

for pages in $(seq 64); do
  for family in tcp unix; do
    for io in splice recvsend; do
      ./echo-server-simpler "$io" "$family" "$pages" | sed "$sed_script"
      rm -rf /tmp/echo-server-*
      mv log "$io-$family-$pages.log"
    done
  done
done


