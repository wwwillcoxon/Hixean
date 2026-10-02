#!/bin/sh
set -e
cd "$(dirname "$0")/.."
echo "== hxc test =="
./build/hxc test tests/*.hxt tests/*.hxe
echo
echo "== ejemplos =="
./build/hxc run examples/hola.hxe
./build/hxc build examples/tco.hxe -o build/tco
echo -n "tco (10M iteraciones de cola): "
(ulimit -s 128; ./build/tco | tr '\n' ' ')
echo
echo "== puertas de tamano =="
./build/hxc build examples/hola.hxe -o build/hola
./build/hxc size build/hola
