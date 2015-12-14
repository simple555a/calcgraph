#!/bin/bash

set -e

./example &

DATA="10Y 1.23
5Y 2.34
1Y 2.10
3Y 2.55"

while read line; do
	echo -n $line | nc -u -w0 localhost 8080
done <<< "$DATA"


sleep 0.1

kill -INT %1
