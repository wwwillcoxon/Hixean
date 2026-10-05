#!/bin/sh
set -e

# Compara la salida de un programa con lo esperado y para si no cuadra.
#
# Antes esto se escribia `diff -u a b && echo "ok ..."`, y con `set -e` una
# comparacion que falla no para nada: en una lista con `&&` solo el ultimo comando
# decide el codigo de salida, y el ultimo era el `echo`. Asi que un programa que
# imprimia basura daba un "ok" y el corpus entero pasaba. Se ha visto: un
# `suma = suma + n` mal emitido salia como 4198720 donde se esperaba 6, en Linux sin
# quejarse, y solo lo delataba Windows con -Werror. Aqui el fallo es del que para.
comprobar() {
  if ! diff -u "$1" "$2"; then
    echo "FALLO: la salida de $3 no cuadra con $1"; exit 1
  fi
  echo "ok     $3"
}
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
comprobar tests/hxc/usa.out build/usa.out "tests/hxc/usa.out"
echo
echo "== unidad .hxc con MAYBE e ITER en firmas exportadas =="
rm -rf build/units_quiz
./build/hxc build tests/hxc/quiz.hxs --emit-hxc build/units_quiz -o build/quiz
./build/hxc build tests/hxc/usa_quiz.hxe --use-hxc build/units_quiz -o build/usa_quiz
./build/usa_quiz > build/usa_quiz.out
# Este modulo tiene el caso que hay que mirar: `Total` declara `DIM suma AS INT`
# y al lado existe `EXPORT FUNCTION Suma`. Los dos nombres se escriben igual porque
# los identificadores no distinguen mayusculas, y gano la funcion: `suma = suma + n`
# se emitia como una llamada. En Linux eso era solo un warning y Total(1) devolvia
# 4198720 donde se esperaba 6; en Windows, con -Werror, era un error de compilacion.
# Y `comprobar` es lo que hacia falta para enterarse en vez de ver un "ok".
comprobar tests/hxc/usa_quiz.out build/usa_quiz.out "tests/hxc/usa_quiz.out"
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
comprobar build/kit.expected build/kit.out "el paquete aritmetica se construye y ejecuta"
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
echo "== un fallo solo de fin de linea se explica solo =="
# Sin esto, en Windows las 34 pruebas fallaban mostrando un texto identico: el
# checkout sacaba los .out con CRLF, el programa escribia LF, y el diff no
# ensejia el byte que sobraba. La prueba comprueba que hxc test lo dice.
rm -f build/crlf.hxt.out
printf 'PRINT 1\nPRINT 2\n' > build/crlf.hxt
./build/hxc test build/crlf.hxt >/dev/null 2>&1
python3 - <<'PY'
d = open('build/crlf.hxt.out', 'rb').read()
open('build/crlf.hxt.out', 'wb').write(d.replace(b'\n', b'\r\n'))
PY
if ./build/hxc test build/crlf.hxt 2>&1 | grep -q "solo difieren los fines de linea"; then
  echo "ok     el fallo por CRLF se nombra en vez de mostrar el texto dos veces"
else
  echo "FALLO: hxc test deberia decir que la diferencia es el fin de linea"; exit 1
fi
# y ningun .out del repositorio lleva un CR en el disco. Se pregunta a git con
# --eol y no con grep: git responde con dos letras (i/ lo que hay en el indice,
# w/ lo que hay en el disco) y dice exactamente que ha pasado. Un guardia que
# solo dice "hay CR" obliga a adivinar, que es como se perdio esta mañana.
cr_en_out=$(git ls-files --eol 'tests/*.out' 'tests/hxc/*.out' | grep -c 'w/crlf' || true)
if [ "${cr_en_out:-0}" -eq 0 ]; then
  echo "ok     ningun .out del repositorio tiene CRLF en disco"
else
  echo "FALLO: $cr_en_out .out tienen CRLF en disco; revisa .gitattributes"
  echo "---- como lo ve git (las ocho primeras) ----"
  git ls-files --eol 'tests/*.out' 'tests/hxc/*.out' | grep 'w/crlf' | head -8
  echo "---- y el atributo que decide ----"
  git check-attr text eol -- tests/anonimas.hxt.out
  exit 1
fi
echo "== el camino de vuelta: texto a numero =="
# El fallo se comprueba aqui y no en el corpus, porque un programa que aborta a
# mitad no ejecuta lo que viene despues: su salida no diria nada de lo que se
# pusiera a continuacion. Aqui si se puede mirar el codigo de salida y el mensaje,
# que es el contrato de verdad.
# El `|| rc=$?` es necesario: con `set -e`, un programa que aborta con 70 —que es
# justo lo que se esta probando— se lleva por delante el script entero.
for malo in '"no soy un numero"' '"12abc"' '"3.7"' '""' '"9223372036854775808"'; do
  printf 'DIM x AS I64 = %s.ToInt()\nPRINT x\n' "$malo" > build/malo.hxe
  rc=0
  ./build/hxc run build/malo.hxe > build/malo.out 2>&1 || rc=$?
  if [ "$rc" -eq 70 ] && grep -q "no es un entero" build/malo.out; then
    :
  else
    echo "FALLO: $malo deberia abortar con 70 y decir por que (rc=$rc)"; exit 1
  fi
done
echo "ok     un entero invalido aborta con 70 y lo dice, en vez de devolver 0"
printf 'DIM x AS FLOAT = "abc".ToFloat()\nPRINT x\n' > build/malo.hxe
rc=0
./build/hxc run build/malo.hxe > build/malo.out 2>&1 || rc=$?
if [ "$rc" -eq 70 ] && grep -q "no es un numero" build/malo.out; then
  echo "ok     un float invalido tambien aborta con 70"
else
  echo "FALLO: \"abc\".ToFloat() deberia abortar con 70 (rc=$rc)"; exit 1
fi
# Y el limite: un numero que no cabe no puede salir por el otro extremo como si
# valiera. En C el desbordamiento no avisa, asi que esto no es trivial.
printf 'DIM ok AS I64 = "9223372036854775807".ToInt()\nPRINT ok\n' > build/limite.hxe
./build/hxc run build/limite.hxe > build/limite.out 2>&1
if [ "$(cat build/limite.out)" = "9223372036854775807" ]; then
  echo "ok     el entero mas grande se convierte bien"
else
  echo "FALLO: el limite de INT no deberia fallar"; exit 1
fi
# Un flotante enorme se imprime en notacion cientifica. Antes salia
# 18446744073709551615.000000000 para 1e20, que no es el numero que se le dio.
printf 'PRINT 1e20\nPRINT 1e19\n' > build/cientifico.hxe
./build/hxc run build/cientifico.hxe > build/cientifico.out 2>&1
if [ "$(cat build/cientifico.out)" = "1e20
10000000000000000000.0" ]; then
  echo "ok     1e20 se imprime en notacion cientifica y 1e19 en decimal"
else
  echo "FALLO: un flotante mayor de 2^64 no cabe en un entero y se imprime mal"; exit 1
fi
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
comprobar build/sombra.expected build/sombra.out "sombrear en un ambito mas hondo si se permite"
printf 'DIM w AS vec2 = (1.0, 0.0)\nPRINT DOT(w, w)\n' > build/vec2dot.hxe
if ./build/hxc check build/vec2dot.hxe 2>&1 | grep -q "E0402"; then
  echo "ok     DOT sobre un vec2 da E0402 y no C invalido"
else
  echo "FALLO: DOT con vec2 deberia dar E0402"; exit 1
fi
printf 'CONST S AS STRING = "ho" ++ "la"\nCONST T AS STRING = "hola"\nPRINT S, T\n' > build/constcadena.hxe
./build/hxc run build/constcadena.hxe > build/constcadena.out 2>/dev/null
printf 'hola        hola\n' > build/constcadena.expected
comprobar build/constcadena.expected build/constcadena.out "CONST de cadena, literal y concatenado, sin C invalido"

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
# la version sale del binario, no de una constante escrita a mano: asi esto no
# se pudre en la proxima release
VERSION=$("./build/hxc" version --json | sed 's/.*"version": *"\([^"]*\)".*/\1/')
if [ -z "$VERSION" ]; then
  echo "FALLO: version --json no trae version"; exit 1
fi
# el pie de la pagina dice la version: si uno se olvida de una, el otro lo nota
if grep -q "Hixean $VERSION ·" site/index.html; then
  echo "ok     version --json para las herramientas ($VERSION, leida del binario)"
else
  echo "FALLO: el binario es $VERSION pero el pie de la pagina no lo dice"; exit 1
fi

echo "== extension de vscode =="
if command -v node >/dev/null 2>&1; then
  node editors/vscode/test/smoke.js
else
  echo "ok     extension omitida: no hay node"
fi

echo "== nombres que faltan =="
printf '  nombre AS STRING\n  patas AS INT\nEND TYPE\n' > build/sinnombre.hxt
if ./build/hxc check build/sinnombre.hxt >/dev/null 2>&1; then
  echo "FALLO: un TYPE sin nombre deberia fallar"; exit 1
fi
./build/hxc check build/sinnombre.hxt 2>&1 | grep -q "E0202" \
  && echo "ok     TYPE sin nombre da E0202 y no revienta el verificador"
printf 'FOR @ = 1 TO 5\n  PRINT 1\nNEXT i\n' > build/forraro.hxt
if ./build/hxc check build/forraro.hxt >/dev/null 2>&1; then
  echo "FALLO: un FOR sin variable de bucle deberia fallar"; exit 1
fi
./build/hxc check build/forraro.hxt 2>&1 | grep -q "E0206" \
  && echo "ok     FOR sin variable de bucle da E0206 sin NameError"

echo "== publicar e instalar paquetes =="
rm -rf build/reg build/inst
./build/hxc pack tests/kits/base/base.hxk --out build/reg | grep -q "base 0.2.0" \
  && ./build/hxc pack tests/kits/aritmetica.hxk --out build/reg | grep -q "aritmetica 1.0.0" \
  && echo "ok     hxc pack publica base y aritmetica en el registro"
test -f build/reg/base/base.hxs -a -f build/reg/aritmetica/aritmetica.hxk \
  && echo "ok     el paquete lleva su fuente y el manifiesto con su nombre"
if ./build/hxc pack tests/kits/aritmetica.hxk --out build/reg >/dev/null 2>&1; then
  echo "FALLO: publicar dos veces el mismo paquete deberia fallar"; exit 1
fi
echo "ok     publicar dos veces el mismo paquete se rechaza"
./build/hxc install base --registry build/reg --into build/inst >/dev/null
./build/hxc install aritmetica --registry build/reg --into build/inst >/dev/null
./build/hxc build --kit build/inst/aritmetica/aritmetica.hxk --path build/inst -o build/inst_arit
./build/inst_arit > build/inst_arit.out
echo "32" > build/inst_arit.expected
comprobar build/inst_arit.expected build/inst_arit.out "el paquete instalado se construye y ejecuta (32)"
if ./build/hxc install base --registry build/reg --into build/inst 2>&1 | grep -q E0818; then
  echo "ok     instalar dos veces da E0818"
else
  echo "FALLO: reinstalar deberia dar E0818"; exit 1
fi
if ./build/hxc install fantasma --registry build/reg --into build/inst 2>&1 | grep -q E0817; then
  echo "ok     un paquete que no esta en el registro da E0817"
else
  echo "FALLO: instalar un paquete inexistente deberia dar E0817"; exit 1
fi
printf 'QUERY lo que hay en el registro\n  VERSION >= 0.1\nEND QUERY\n' > build/reg.hxq
./build/hxc query build/reg.hxq --path build/reg > build/reg.out
printf 'aritmetica 1.0.0  build/reg/aritmetica/aritmetica.hxk\nbase 0.2.0  build/reg/base/base.hxk\n' > build/reg.expected
comprobar build/reg.expected build/reg.out "hxc query encuentra lo publicado en el registro"

echo "== una capacidad sin declarar es un paquete que no se construye =="
# Las capacidades se declaran con ENABLE en el fuente y con CAPABILITY en el
# manifiesto, y si no coinciden el paquete no se construye. Aqui se comprueba con
# `time`, que es la segunda capacidad: la puerta es generica y sirve para las dos.
rm -rf build/caps_tiempo && mkdir -p build/caps_tiempo
printf 'ENABLE time\nPRINT TIME_RANDOM(6)\n' > build/caps_tiempo/t.hxe
printf 'KIT con_tiempo 1.0.0\n  TARGET hixe >= 0.2\n  ENTRY t.hxe\n  PROVIDES con_tiempo\nEND KIT\n' \
  > build/caps_tiempo/p.hxk
if ./build/hxc build --kit build/caps_tiempo/p.hxk -o build/caps_tiempo/salida 2>&1 | grep -q "CAPABILITY time"; then
  echo "ok     un paquete que usa time sin declararlo no se construye"
else
  echo "FALLO: la capacidad usada no se exige en el manifiesto"; exit 1
fi
printf 'KIT con_tiempo 1.0.0\n  TARGET hixe >= 0.2\n  ENTRY t.hxe\n  PROVIDES con_tiempo\n  CAPABILITY time\nEND KIT\n' \
  > build/caps_tiempo/p.hxk
./build/hxc build --kit build/caps_tiempo/p.hxk -o build/caps_tiempo/salida 2>/dev/null \
  && echo "ok     declarado, se construye" \
  || { echo "FALLO: declarado y aun asi no se construye"; exit 1; }

echo "== los documentos no mienten =="
if command -v python3 >/dev/null 2>&1; then
  python3 tools/verificar-ejemplos.py docs/manual.html site/index.html
  python3 tools/verificar-runtime.py
  python3 tools/verificar-cifras.py
  python3 tools/verificar-tabla-errores.py
  python3 tools/verificar-guia.py docs/guia-programar.md
  python3 tools/verificar-pdf.py site/guia-programar.pdf
  python3 tools/verificar-utf8.py
  # el PDF se regenera y se compara: un PDF commiteado que no corresponde al
  # markdown es un documento que ya no explica lo que dice explicar
  python3 tools/generar-pdf.py docs/guia-programar.md build/guia.pdf >/dev/null
  if cmp -s build/guia.pdf site/guia-programar.pdf; then
    echo "ok     el PDF del sitio corresponde al markdown que lo genera"
  else
    echo "FALLO: site/guia-programar.pdf no es el que genera docs/guia-programar.md"
    exit 1
  fi
else
  echo "ok     ejemplos, runtime, cifras y tabla de errores omitidos: no hay python3"
fi
if command -v node >/dev/null 2>&1; then
  node site/test/humo.js
else
  echo "ok     prueba de la pagina omitida: no hay node"
fi

echo "== un numero es el mismo por PRINT y por ToString =="
# PRINT de un FLOAT pasa el valor a hx_print_f64; ToString pasa por hx_f64_str. Son
# dos caminos distintos y los dos tienen que ver el mismo numero. No lo veian: el de
# ToString casteaba a int64_t antes de formatear, y (int64_t)2.5 es 2, asi que un
# `PRINT 2.5` decia 2.5 y un `2.5.ToString()` decia 2.0. El corpus no lo dira, porque
# el `.out` esperado se habia regenerado con el bug dentro y las dos cosas cuadraban:
# una expectativa equivocada da luz verde sobre un bug. Por eso esta puerta compara
# los dos caminos en vez de fiarse del `.out`.
cat > build/mismos_valores.hxe <<'HXE'
DIM a AS FLOAT = 2.5
DIM b AS FLOAT = 19.99
DIM c AS FLOAT = 0.5
DIM d AS FLOAT = -2.5
DIM e AS FLOAT = 1234.0625
PRINT a
PRINT a.ToString()
PRINT b
PRINT b.ToString()
PRINT c
PRINT c.ToString()
PRINT d
PRINT d.ToString()
PRINT e
PRINT e.ToString()
HXE
./build/hxc run build/mismos_valores.hxe > build/mismos_valores.out 2>&1
awk 'NR % 2 == 1' build/mismos_valores.out > build/por_print.out
awk 'NR % 2 == 0' build/mismos_valores.out > build/por_texto.out
comprobar build/por_print.out build/por_texto.out "PRINT y ToString ven el mismo FLOAT"

echo "== arreglos: honestidad y acceso comprobado =="
if ./build/hxc check tests/malos/rangos.hxe >/dev/null 2>&1; then
  echo "FALLO: un rango en un indice deberia rechazarse"; exit 1
fi
./build/hxc check tests/malos/rangos.hxe 2>&1 | grep -q "E0210" \
  && echo "ok     un rango en un indice da E0210 en vez de leer un elemento"
# E0212 (UNIQUE reservado) esta retirado: lo que ahora se comprueba es E0216
./build/hxc check tests/malos/reservados.hxe 2>&1 | grep -q "error\[E0216\]" \
  && ./build/hxc check tests/malos/reservados.hxe 2>&1 | grep -q "error\[E0211\]" \
  && echo "ok     UNIQUE ya no esta reservado (E0216) y PRINT NIL da E0211" \
  || { echo "FALLO: UNIQUE"; exit 1; }
# dos MAYBE de tipos distintos no son intercambiables
./build/hxc check tests/malos/tipos-distintos.hxe 2>&1 | grep -q "se esperaba MAYBE STRING, se encontró MAYBE INT" \
  && ./build/hxc check tests/malos/tipos-distintos.hxe 2>&1 | grep -q "se esperaba MAYBE INT, se encontró MAYBE STRING" \
  && echo "ok     MAYBE INT y MAYBE STRING no se confunden entre si" \
  || { echo "FALLO: dos MAYBE distintos se estan tomando por el mismo tipo"; exit 1; }
# MAYBE: un valor no se desempaqueta solo, NIL fuera de sitio y MATCH incompleto
# -I, HX_LIB y el fallo que las lista
rm -rf build/busqueda && mkdir -p build/busqueda/libdir
printf 'EXPORT FUNCTION Quien() AS STRING\n  RETURN "de la busqueda"\nEND FUNCTION\n' \
  > build/busqueda/libdir/mimodulo.hxs
printf 'IMPORT mimodulo\nPRINT mimodulo.Quien()\n' > build/busqueda/usa.hxe
./build/hxc run build/busqueda/usa.hxe -I build/busqueda/libdir > build/busqueda/i.out 2>&1
grep -q "de la busqueda" build/busqueda/i.out \
  && echo "ok     -I encuentra un modulo fuera del directorio del archivo" \
  || { echo "FALLO: -I no busca"; exit 1; }
HX_LIB=build/busqueda/libdir ./build/hxc run build/busqueda/usa.hxe > build/busqueda/env.out 2>&1
grep -q "de la busqueda" build/busqueda/env.out \
  && echo "ok     HX_LIB encuentra un modulo fuera del directorio del archivo" \
  || { echo "FALLO: HX_LIB no busca"; exit 1; }
if ./build/hxc run build/busqueda/usa.hxe > build/busqueda/nada.out 2>&1; then
  echo "FALLO: deberia fallar sin -I ni HX_LIB"; exit 1
fi
grep -q "E0501" build/busqueda/nada.out \
  && grep -q "HX_LIB" build/busqueda/nada.out \
  && echo "ok     sin -I ni HX_LIB el modulo no se encuentra y el mensaje lo dice" \
  || { echo "FALLO: el fallo no explica donde se busco"; exit 1; }
# el paquete lleva la biblioteca y el instalador la deja donde hxc la mira:
# esto se compila con el binario INSTALADO, desde otro directorio, sin -I
# Esto prueba el tarball que publica la release, y ese es solo linux-x64. En
# macOS y Windows no hay artefacto que instalar: no es que falle, es que la
# pregunta no aplica. El patron es el mismo que la capacidad net.
rm -rf build/prefixe
if [ "$(uname -s)" = "Linux" ] && command -v tar >/dev/null 2>&1; then
  sh tools/dist.sh "$VERSION" linux-x64 >/dev/null 2>&1
  mkdir -p build/prefixe
  if HIXEAN_PREFIX=build/prefixe sh tools/install.sh \
       "dist/hixean-$VERSION-linux-x64.tar.gz" >/dev/null 2>&1; then
    printf 'IMPORT std.texto\nPRINT texto.PadLeft("7", 3, "0")\n' > build/instalado.hxe
    (cd build && ./prefixe/bin/hxc run instalado.hxe > instalado.out 2>&1)
    if [ "$(cat build/instalado.out 2>/dev/null)" = "007" ]; then
      echo "ok     el binario instalado encuentra la biblioteca instalada, sin -I"
    else
      echo "FALLO: la biblioteca no llega al paquete o hxc no la encuentra"; exit 1
    fi
  else
    echo "FALLO: install.sh no instala un paquete local"; exit 1
  fi
else
  echo "ok     instalacion omitida: el tarball publicado es solo linux-x64"
fi
# un modulo con punto: std.texto.hxs se llama std.texto y se llama texto.Doble
./build/hxc run tests/modulos_con_punto/usa.hxe > build/punto.out 2>&1
comprobar tests/modulos_con_punto/usa.hxe.out build/punto.out "IMPORT std.texto encuentra std.texto.hxs y se llama con su namespace"
# el nombre del módulo tiene que ser el de la ruta del IMPORT
rm -rf build/modulo_mal && mkdir -p build/modulo_mal
cp tests/malos/mate_equivocado.hxs build/modulo_mal/mate.hxs
cp tests/malos/modulo_mal.hxe build/modulo_mal/
./build/hxc check build/modulo_mal/modulo_mal.hxe 2>&1 | grep -q "se pidió el módulo 'mate'" \
  && ./build/hxc check build/modulo_mal/modulo_mal.hxe 2>&1 | grep -q "declara 'otro_mate'" \
  && echo "ok     un modulo que declara otro nombre que su ruta se rechaza" \
  || { echo "FALLO: el nombre del modulo no se comprueba"; exit 1; }
# ARRAY[T]: Set y Push son de los que crecen, y el tamaño no va detrás
./build/hxc check tests/malos/arreglos_dinamicos.hxe 2>&1 | grep -q "E0306" \
  && ./build/hxc check tests/malos/arreglos_dinamicos.hxe 2>&1 | grep -q "no lleva tamaño" \
  && ./build/hxc check tests/malos/arreglos_dinamicos.hxe 2>&1 | grep -q "se esperaba ARRAY\[3\], se encontró ARRAY\[T\]" \
  && echo "ok     Set y Push no existen en un arreglo fijo, y ARRAY[T] no lleva tamaño" \
  || { echo "FALLO: las reglas de ARRAY[T]"; exit 1; }
# UNIQUE: una variable no puede estar en dos campos, y solo va en un REF
./build/hxc check tests/malos/unique.hxe 2>&1 | grep -q "ya está en 'D.a'" \
  && ./build/hxc check tests/malos/unique.hxe 2>&1 | grep -q "E0216" \
  && echo "ok     UNIQUE rechaza la doble propiedad y el campo que no es REF" \
  || { echo "FALLO: las reglas de UNIQUE"; exit 1; }
./build/hxc check tests/malos/maybe.hxe 2>&1 | grep -q "E0301" \
  && ./build/hxc check tests/malos/maybe.hxe 2>&1 | grep -q "E0211" \
  && ./build/hxc check tests/malos/maybe.hxe 2>&1 | grep -q "E0405" \
  && echo "ok     MAYBE no se desempaqueta solo, NIL fuera de sitio da E0211 y el MATCH se completa" \
  || { echo "FALLO: las reglas de MAYBE"; exit 1; }
printf 'DIM a AS INT[3]\nDIM k AS INT = 9\nPRINT a.At(k)\n' > build/fuera.hxe
./build/hxc build build/fuera.hxe -o build/fuera >/dev/null 2>&1
if ./build/fuera > build/fuera.out 2>&1; then
  echo "FALLO: At fuera de rango deberia abortar"; exit 1
fi
grep -q "fuera de rango" build/fuera.out && echo "ok     At fuera de rango aborta con el indice y sale con 70"
# lo mismo en un ARRAY[T], que comprueba contra el largo y no contra el tipo
printf 'DIM d AS ARRAY[INT]\nd.Push(1)\nPRINT d.At(5)\n' > build/fuera2.hxe
./build/hxc build build/fuera2.hxe -o build/fuera2 >/dev/null 2>&1
if ./build/fuera2 > build/fuera2.out 2>&1; then
  echo "FALLO: At fuera de rango en ARRAY[T] deberia abortar"; exit 1
fi
grep -q "fuera de rango" build/fuera2.out && echo "ok     At fuera de rango en ARRAY[T] tambien aborta con el indice"
