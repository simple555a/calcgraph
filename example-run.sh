#!/bin/bash

set -e

./example &

# test data based on https://www.treasury.gov/resource-center/data-chart-center/interest-rates/Pages/TextView.aspx?data=yield
DATA="1Y 0.51
2Y 0.91
3Y 1.19
5Y 1.59
7Y 1.93
10Y 2.15
20Y 2.55
30Y 2.91"

while read line; do
	echo -n $line | nc -u -w0 localhost 8080
done <<< "$DATA"


sleep 0.1

kill -INT %1
