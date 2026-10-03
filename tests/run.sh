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
echo "== biblioteca .hxc (interfaz + libmate.a, sin fuentes de mate) =="
rm -rf build/units
./build/hxc build tests/hxc/mate.hxs --emit-hxc build/units -o build/mate
./build/hxc build tests/hxc/usa.hxe --use-hxc build/units -o build/usa
./build/usa > build/usa.out
diff -u tests/hxc/usa.out build/usa.out && echo "ok     tests/hxc/usa.out"
echo
echo "== unidad .hxc dañada =="
mkdir -p build/units_bad build/sinsrc
head -c 40 build/units/mate.hxc > build/units_bad/mate.hxc
cp tests/hxc/usa.hxe build/sinsrc/usa.hxe
if ./build/hxc build build/sinsrc/usa.hxe --use-hxc build/units_bad -o build/usa 2>&1 | grep -q E0602; then
  echo "ok     unidad truncada rechazada con E0602"
else
  echo "FALLO: no se rechazo la unidad truncada"; exit 1
fi
printf 'XXXX' > build/units_bad/mate.hxc
tail -c +5 build/units/mate.hxc >> build/units_bad/mate.hxc
if ./build/hxc build build/sinsrc/usa.hxe --use-hxc build/units_bad -o build/usa 2>&1 | grep -q E0601; then
  echo "ok     unidad con magia incorrecta rechazada con E0601"
else
  echo "FALLO: no se rechazo la unidad con magia incorrecta"; exit 1
fi

echo "== paquetes .hxk =="
./build/hxc build --kit tests/kits/aritmetica.hxk --path tests/kits -o build/kit_aritmetica
./build/kit_aritmetica > build/kit.out
echo "32" > build/kit.expected
diff -u build/kit.expected build/kit.out && echo "ok     el paquete aritmetica se construye y ejecuta"
./build/hxc kit tests/kits/aritmetica.hxk --path tests/kits | grep -q "resolution base 0.2.0" \
  && echo "ok     resolucion de dependencias"
for k in roto sin_permiso nuevo_dep churro; do
  if ./build/hxc kit tests/kits/$k.hxk --path tests/kits >/dev/null 2>&1; then
    echo "FALLO: el manifiesto $k deberia fallar"; exit 1
  fi
done
echo "ok     4 manifiestos rotos rechazados con diagnostico"

echo "== cache de objetos =="
rm -rf build/inc build/obj
cp -r bench/multi build/inc
./build/hxc build build/inc/main.hxe -o build/multi --timing 2>&1 | grep -o "([0-9]* TU recompiladas)" \
  | grep -q "(22 TU recompiladas)" && echo "ok     22 unidades en frio"
./build/hxc build build/inc/main.hxe -o build/multi --timing 2>&1 | grep -q "0 TU recompiladas" \
  && echo "ok     0 unidades sin cambios"
# la clave es el contenido, no la marca de tiempo: tocar sin cambiar no recompila
touch build/inc/m7.hxs
./build/hxc build build/inc/main.hxe -o build/multi --timing 2>&1 | grep -q "0 TU recompiladas" \
  && echo "ok     tocar sin cambiar no recompila"
sed -i "0,/RETURN acc + 7/s//RETURN acc + 4242/" build/inc/m7.hxs
./build/hxc build build/inc/main.hxe -o build/multi --timing 2>&1 | grep -q "1 TU recompiladas" \
  && echo "ok     1 unidad tras cambiar un modulo"
echo
echo "== puertas de tamano =="
./build/hxc build examples/hola.hxe -o build/hola
./build/hxc size build/hola
