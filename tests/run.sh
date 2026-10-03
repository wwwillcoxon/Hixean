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

echo "== capacidad net (sockets reales por loopback) =="
if [ "$(uname -s)" = "Linux" ]; then
  ./build/hxc build tests/hxc_red.hxe -o build/red
  ./build/red > build/red.out
  diff -u tests/hxc_red.out build/red.out && echo "ok     ida y vuelta UDP por loopback"
else
  echo "ok     net omitida: sockets por syscall solo en Linux"
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

echo "== recuperacion del lexer =="
printf 'DIM s AS STRING = "sin cerrar\nPRINT s\n' > build/cadena.hxe
if ./build/hxc check build/cadena.hxe 2>&1 | grep -q "E0103"; then
  echo "ok     cadena sin cerrar hasta el fin de archivo con E0103"
else
  echo "FALLO: no se reporto la cadena sin cerrar"; exit 1
fi

echo "== consultas .hxq =="
for q in base net aritmetica nada; do
  ./build/hxc query tests/queries/$q.hxq --path tests/kits > build/q_$q.out
  diff -u tests/queries/$q.out build/q_$q.out && echo "ok     consulta $q"
done
printf 'QUERY mala\n  HACE base\nEND QUERY\n' > build/mala.hxq
for q in "QUERY sin cerrar\n  PROVIDES base\n" "QUERY vacia\nEND QUERY\n" \
         "QUERY compara\n  PROVIDES base >= 1\nEND QUERY\n"; do
  printf "$q" > build/rota.hxq
  if ./build/hxc query build/rota.hxq --path tests/kits >/dev/null 2>&1; then
    echo "FALLO: una consulta invalida deberia fallar"; exit 1
  fi
done
./build/hxc query build/mala.hxq --path tests/kits 2>&1 | grep -q "E0814" \
  && echo "ok     4 consultas invalidas rechazadas con diagnostico"

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

echo "== puerta de capacidades =="
printf 'KIT sin_net 0.1.0\n  ENTRY red.hxe\nEND KIT\n' > build/sin_net.hxk
mkdir -p build/redsrc && cp tests/hxc_red.hxe build/redsrc/red.hxe
if ./build/hxc build --kit build/sin_net.hxk -o build/red2 >/dev/null 2>&1; then
  echo "FALLO: un manifiesto sin CAPABILITY no deberia compilar una fuente con ENABLE net"
  exit 1
fi
echo "ok     CAPABILITY net exigido por el manifiesto"

echo "== diagnosticos que antes llegaban a cc =="
printf 'DIM a AS INT = 1\nDIM a AS INT = 2\nPRINT a\n' > build/redeclara.hxe
if ./build/hxc check build/redeclara.hxe 2>&1 | grep -q "E0315"; then
  echo "ok     dos DIM del mismo nombre en un ambito con E0315"
else
  echo "FALLO: la redeclaracion deberia dar E0315"; exit 1
fi
printf 'DIM x AS INT = 1\nIF x = 1 THEN\n  DIM x AS INT = 2\n  PRINT x\nEND IF\nPRINT x\n' > build/sombra.hxe
./build/hxc run build/sombra.hxe > build/sombra.out 2>/dev/null
printf '2\n1\n' > build/sombra.expected
diff -u build/sombra.expected build/sombra.out >/dev/null \
  && echo "ok     sombrear en un ambito mas hondo si se permite"
printf 'DIM w AS vec2 = (1.0, 0.0)\nPRINT DOT(w, w)\n' > build/vec2dot.hxe
if ./build/hxc check build/vec2dot.hxe 2>&1 | grep -q "E0402"; then
  echo "ok     DOT sobre un vec2 da E0402 y no C invalido"
else
  echo "FALLO: DOT con vec2 deberia dar E0402"; exit 1
fi
printf 'CONST S AS STRING = "ho" ++ "la"\nCONST T AS STRING = "hola"\nPRINT S, T\n' > build/constcadena.hxe
./build/hxc run build/constcadena.hxe > build/constcadena.out 2>/dev/null
printf 'hola        hola\n' > build/constcadena.expected
diff -u build/constcadena.expected build/constcadena.out >/dev/null \
  && echo "ok     CONST de cadena, literal y concatenado, sin C invalido"

echo "== diagnosticos en json =="
printf 'DIM x AS INT = "hola"\nDIM y AS INT = noexiste\n' > build/json.hxe
./build/hxc check build/json.hxe --json 2> build/json.out || true
if command -v python3 >/dev/null 2>&1; then
  python3 -c "
import json,sys
d=json.load(open('build/json.out'))
xs=d['diagnostics']
assert len(xs)==2, xs
assert [x['code'] for x in xs]==['E0301','E0304'], xs
assert xs[0]['line']==1 and xs[0]['col']==16, xs[0]
assert xs[1].get('help'), xs[1]
assert d['errors']==2, d
" && echo "ok     check --json con posiciones, codigos, nota y ayuda"
else
  grep -q '"code":"E0301"' build/json.out && grep -q '"help":' build/json.out \
    && echo "ok     check --json trae codigo y ayuda (sin python3 no se valida el JSON)"
fi
./build/hxc version --json | grep -q '"version": "0.1.0"' && echo "ok     version --json para las herramientas"

echo "== extension de vscode =="
if command -v node >/dev/null 2>&1; then
  node editors/vscode/test/smoke.js
else
  echo "ok     extension omitida: no hay node"
fi
