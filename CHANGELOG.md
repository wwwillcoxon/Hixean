# Changelog

Formato de [Keep a Changelog](https://keepachangelog.com/es-ES/1.1.0/) y
versionado semántico. El número de versión es el del compilador y del
lenguaje a la vez: `hxc version` lo imprime y los manifiestos lo comparan
con `TARGET hixe >= 0.2`.

## [0.2.0] — 2026-10-04

Veintiún hitos después de 0.1.0. Sigue siendo `0.x`: el número mayor se mueve
cuando algo incompatible lo obliga, y esta vez lo hay.

### Cambios incompatibles

- **El `.hxc` es ahora formato 2.** Una unidad publicada con 0.1.0 no la lee este
  `hxc`: `E0603` dice qué formato trae la unidad, cuál entiende este compilador y
  qué abi espera. Publicar una biblioteca es una cosa que hay que rehacer con
  cada `hxc` nuevo.
- **Los paquetes de este repositorio piden `TARGET hixe >= 0.2`.** El
  comparador acepta versiones numéricas, así que un paquete con `>= 0.1` seguiría
  compilando con 0.2.0; lo que ya no compila es al revés. Un paquete construido
  aquí lleva un `.hxc` de formato 2 y un `hxc` 0.1.0 no lo entendería.
- **Quince palabras clave que existían sin uso ya no están reservadas**:
  `IMPL`, `COMPTIME`, `DYN`, `PUBLIC`, `SHADER`, `VERTEX`, `FRAGMENT`,
  `COMPUTE`, `INPUT`, `UNIFORM`, `OUTPUT`, `SHADOW`, `ASSERT`, `PURE` y `ENTRY`.
  No hacían nada y reservaban nombres que un programa debería poder usar. La
  entrada `ENTRY` de los manifiestos `.hxk` no es esta palabra clave: la lee
  `kit.c` como texto. Un programa que usara uno de esos quince nombres como
  variable ahora compila; uno que esperara que estuvieran reservados, no.
- **`E0212` queda retirado** («UNIQUE está reservado pero no implementado»). Los
  errores de `UNIQUE` son `E0216` y `E0218`, y los dos llevan una nota que
  menciona `E0212`, como pide la política de códigos de diagnóstico. El código
  vuelve a estar libre para reutilizar.
- **`^` ahora atraviesa un `REF`.** Antes `p^.campo` sobre un `REF` era `E0722`;
  lo que no se puede hacer es llamar métodos a través de un `REF`, y eso sigue
  siendo un error.
- **Reasignar un `REF` que es una variable es `E0408`.** Antes escribía a través
  del puntero, que al principio es `NULL`, y el programa moría en silencio. El
  préstamo se hace al declarar, no al reasignar.
- **Un método sobre el resultado de una llamada se resuelve.** `f(x).Metodo` era
  `E0305` porque el emisor solo miraba el nombre; ahora compila.
- **Un arreglo dentro de un registro reserva su memoria al construirlo.** Antes
  las celdas eran un `hx_span` con el puntero a `NULL`, y escribir en
  `t.celdas[0]` escribía en el vacío.

### Añadido

- **`lib/` viaja en el paquete y `install.sh` lo instala** en
  `<prefijo>/lib/hixean`, que es donde el binario lo mira. `install.sh` además
  acepta un `.tar.gz` local como argumento, para instalar sin red y para poder
  probar el paquete antes de publicar la release.
- **`std.texto`, la primera biblioteca estándar**: `StartsWith`, `EndsWith`,
  `Contains`, `Replace`, `Split`, `Join`, `PadLeft` y `PadRight`, escritas en
  Hixean en `lib/hixean/std.texto.hxs`. El compilador la busca sola junto a su
  binario, así que `IMPORT std.texto` no necesita `-I`. Los métodos que ya
  tenía `STRING` se quedan donde estaban.
- **Rutas de Import**: `-I DIR` repetible, `HX_LIB` con varios directorios
  separados por `:` o `;`, y la biblioteca que vino con el compilador al final
  de la lista. Los módulos con punto (`std.texto` → `std.texto.hxs`) se
  resuelven probando el nombre corto y el largo. El nombre del módulo importado
  se comprueba contra la ruta del `IMPORT`.
- **`ARRAY[T]`: el arreglo dinámico**, con `Len`, `At(i)`, `Set(i, v)` y
  `Push(v)`. Crece por duplicación y sin `realloc`: se reserva el bloque nuevo
  desde la arena y se copia, así que el viejo se queda hasta que la arena se
  libere. `T[]` es lo mismo escrito de otra forma. En un arreglo de tamaño fijo
  `Set` y `Push` dan `E0306`.
- **`Set(i, v)`**: la escritura comprobada, que no existía (solo se leía con
  `At`).
- **Un método ya no necesita que el receptor sea un camino**: antes
  `nombres.At(1).Upper()` no encontraba el `Upper`, porque el receptor era una
  llamada. Ahora `f(x).Metodo()` funciona con cualquier método del tipo.
- **`UNIQUE` funciona**: un campo `UNIQUE REF T` es el dueño del préstamo, y
  asignarle una variable la mueve. Una variable no puede estar en dos campos a
  la vez (`E0218`), y `UNIQUE` en un campo que no es `REF` da `E0216`.
- **`^` también funciona sobre un `REF`**, no solo sobre un `PTR`: los dos son
  punteros en C, y sin esto un campo `REF` no se podía ni leer. Y
  `DIM d AS REF C = k` ahora presta `k` en vez de dar `E0301`.
- **`MAYBE T` y `NIL`**: un valor o nada, con `.IsNil`, `.Or(x)`, `.Map(f)` y
  `CASE NIL` en un `MATCH`. Envolver es implícito; desempaquetar no, y un
  `MAYBE U` tampoco vale donde se espera `MAYBE T`.
- **Sobrecarga de operadores**: `FUNCTION OPERATOR + (a AS MiTipo, b AS MiTipo)
  AS MiTipo` para `+` `-` `*` `/` `MOD` `++` `==` `<>` `<` `<=` `>` `>=`. El tipo
  del primer parámetro decide cuál se usa, y la aritmética entera sigue igual.
- **Arreglos legibles**: `a.Len()` devuelve el tamaño (una constante del tipo,
  sin coste en tiempo de ejecución) y `a.At(i)` comprueba el índice y aborta con
  el número en pantalla. `a[i]` sigue sin comprobar, documentado como tal.
- **`FOR ... IN` sobre un `ITER` que ya existe** y no solo sobre `Rango(...)`:
  una función puede recibir un iterador y recorrerlo.
- **`E0717`**: `Rango(...)` fuera del iterable de un `FOR` se rechaza con un
  diagnóstico que explica por qué, en vez de reventar el compilador.
- **Salida para herramientas**: `hxc check --json` y `hxc version --json`.
- **Editores**: extensión de VS Code en `editors/vscode/` con resaltado,
  quince plantillas, comandos de compilación y prueba, y el panel de problemas
  alimentado por los diagnósticos de `hxc`.
- **Distribución del compilador**: `make dist` produce
  `hixean-<versión>-<plataforma>.tar.gz` con su SHA256; la CI publica una
  release por etiqueta con las seis combinaciones de sistema y arquitectura;
  `tools/install.sh` y `tools/install.ps1` instalan verificando el checksum;
  hay fórmula de Homebrew y manifiesto de winget.
- **Distribución de paquetes**: `hxc pack` publica un paquete en un registro
  copiando su manifiesto y sus módulos, y `hxc install` lo trae desde el
  registro. El registro es un directorio, así que puede ser un repositorio git
  clonado; las consultas `.hxq` lo recorren sin cambiar de formato.
- **Página pública**: `site/` con `index.html`, `style.css` y `script.js`, sin
  dependencias ni fuentes remotas: tema claro/oscuro, pestañas de ejemplos,
  copiado de código, contadores, barra de progreso y enlace de salto. Se publica
  en `github.io/Hixean/` junto con el manual mediante `.github/workflows/pages.yml`.
- **Documentación**: `docs/grammar.md` (gramática completa con códigos de
  diagnóstico), `docs/manual.html` (manual interactivo con búsqueda, tema
  oscuro y tabla de errores filtrable) y quince ADR en `docs/adr/`.

### Corregido

- **`install.sh` no copiaba la biblioteca**: en POSIX no hay expansión de nombres
  de fichero en una asignación, así que `LIB="$TMP"/hixean-*/lib/hixean` se
  quedaba literal, la prueba `[ -d ]` fallaba y el script salía con éxito sin
  haber copiado nada. Ahora el directorio se recorre con un `for`, y si no
  aparece `bin/hxc` se dice con un error.
- **Un bloque `TEST` tumbaba el compilador**: el `HxStmtVec` del cuerpo se
  declaraba sin inicializar, así que el primer `push` escribía por un puntero
  basura. Y `END TEST` tampoco era un final de bloque reconocido, así que el
  cuerpo se comía el resto del archivo. Ahora un bloque `TEST` se lee y se
  salta, que es lo que el parser siempre pretendió.
- **Los `switch` por tipo, expresión, sentencia y patrón ya no admiten
  `default`**, así que añadir un `TY_*`, `EX_*` o `ST_*` rompe la compilación en
  vez de colarse por un retorno por defecto. El que moría en silencio era
  `hx_ty_equal`: sin caso `TY_MAYBE` daba `1` para cualquier par, o sea que
  todos los `MAYBE` eran el mismo tipo (y lo mismo pasaba con `ITER`).
- **El `.hxc` perdía `MAYBE` e `ITER`**: al publicar una biblioteca, esas dos
  firmas se serializaban como `VOID`, y al consumirla sin fuentes llegaban como
  `VOID`. Ahora tienen etiqueta propia y el formato es 2.
- **La pila del perfil `freestanding` arrancaba desalineada 8 bytes**, porque el
  kernel empuja `argc` sobre una pila alineada a 16. Con escalares no se nota;
  al copiar un struct con `movaps` reventaba. `_start` realinea ahora.
- **`FOR ... IN` sobre un `ITER` existente reventaba el compilador**, porque el
  emisor lo trataba como si fuera un constructor. `Rango(...)` fuera de un `FOR`
  reventaba también.
- **Monomorfizar una función genérica dejaba punteros al cuerpo original**:
  `hx_clone_expr` no clonaba la expresión de `EX_MEMBER`, `EX_MEMB` ni `EX_DEREF`,
  y `hx_clone_stmt` ignoraba `ST_FORIN`, así que una instancia podía rigir sobre
  la declaración.
- **`hx_count_defers` no contaba un `DEFER` dentro de otro `DEFER`**, con lo que
  el bloque no abría su epílogo y el defer anidado se ejecutaba fuera de orden.
- **Envolver en un `MAYBE` aceptaba de más**: `DIM texto AS MAYBE STRING = a`
  con `a AS MAYBE INT` pasaba el verificador y reventaba al abrir el valor.
- **Los typedef de `MAYBE` se declaraban en cada cabecera**, y con dos módulos
  el C de abajo no compilaba por tenerlos dos veces.
- **El perfil `freestanding` no emitía el typedef de `hx_iter`** cuando el
  programa no iteraba nunca, pero una cabecera podía declararlo.
- `a[1..3]` se parseaba y se comprobaba, pero el emisor se comía el final del
  rango y leía un solo elemento: `E0210` lo dice ahora, y los rangos de verdad
  llegan con los arreglos dinámicos.
- Un arreglo dentro de un registro era un `hx_span` con el puntero a NULL:
  escribir en `t.celdas[0]` escribía en el vacío. Ahora el constructor del
  registro reserva la memoria desde la arena estática.
- La caché de objetos no se invalidaba al cambiar el compilador (solo miraba la
  versión, que durante el desarrollo no cambia): un `.o` viejo se reutilizaba y
  producía un binario roto sin avisar. Ahora la clave incluye un hash del propio
  ejecutable.
- `NIL` imprimía un cero: ahora es parte de `MAYBE T`.
- **Reasignar un `REF` que es una variable escribía a través del puntero**, que
  al principio es `NULL`, y el programa moría en silencio. Ahora es `E0408`:
  el préstamo se hace al declarar.
- **`ARRAY[T]` no se diferenciaba de uno fijo al imprimir el tipo**: el nombre
  era `ARRAY` en los dos casos, así que un `E0301` decía «se esperaba ARRAY, se
  encontró ARRAY».
- El runtime escribía 12 bytes donde el literal tenía 11: cada pánico de
  desbordamiento salía precedido de un byte nulo. Ahora hay un verificador
  (`tools/verificar-runtime.py`) que comprueba todas las longitudes fijas del
  runtime contra sus literales.
- Un `TYPE` sin nombre (por ejemplo un archivo que empieza con basura y acaba en
  `END TYPE`) llegaba al verificador con el nombre a NULL y lo hacía saltar en
  `strlen`. El fuzzer lo encontró; ahora es `E0202` con un nombre anónimo
  interno. Lo mismo con la variable de un `FOR`, que daba `E0206`.

## [0.1.0] — 2026-10-03

**No se publicó nunca.** El número de versión estaba en el binario y en este
changelog, pero no había forma de instalar nada: `tools/dist.sh`,
`tools/install.sh` y el workflow que adjunta los tarballs llegaron tres commits
después, así que 0.1.0 solo era un árbol de código. No hay etiqueta `v0.1.0`, y
el enlace de abajo apunta al commit, no a una release. La primera release
descargable es 0.2.0.

Compilador `hxc` en C11 sin dependencias, biblioteca estándar mínima y gramática
documentada. Lo que hay aquí se puede comprobar con `make test` (22 programas) y
`make size` (hola mundo ≤ 12 KiB).

### Añadido

- **Léxico y sintaxis**: literales `INT` `I64` `FLOAT` `STRING` `BOOL`
  `DURATION` con base 2/8/16 y separadores, interpolación siempre activa,
  comentarios de cuatro estilos, recuperación de errores con spans y códigos.
- **Sentencias**: `IF`/`ELSEIF`/`ELSE`, `WHILE`/`WEND`, `FOR … TO … STEP`,
  `BREAK`, `CONTINUE`, `EXIT`, `MATCH` con patrones, rangos y `CASE ELSE`
  obligatorio.
- **Funciones**: `FUNCTION`, parámetros con valor por defecto, `EXPORT`,
  recursión de cola convertida a bucle en el HIR.
- **Datos**: `TYPE` con subtipado estructural y materialización al emitir,
  `ENUM` con `MATCH` exhaustivo, vectores (`vec2` `vec3` `vec4`) con swizzle,
  `DOT`, `CROSS`, `NORMALIZED`.
- **Errores sin *unwinding***: `Result<T,E>`, `Ok`/`Err`, propagación con `?`,
  `MATCH` sobre `Result`, `DEFER` con epílogos encadenados por `goto`.
- **Memoria**: `ARENA` con reservas reales (mmap o malloc), `REF` con unicidad
  por ámbito, `PTR` con `&` y `^`.
- **Genéricos y traits**: `FUNCTION F<T>`, `TYPE Caja<T>`, monomorfización,
  `TRAIT`/`IMPLEMENTAR PARA`/`METODO`, restricciones `DONDE T: Trait`,
  funciones anónimas `FUNC`.
- **Iteradores perezosos**: `ITER<T>`, `FOR x IN`, `MAP`, `FILTER`, `TAKE`
  sobre `Rango`.
- **Módulos y bibliotecas**: `MODULE`, `IMPORT`, `EXPORT`, compilación por
  módulo con caché de objetos, unidad `.hxc` (interfaz + biblioteca) que
  permite usar una biblioteca sin sus fuentes.
- **Paquetes**: manifiestos `.hxk` con `DEP`, `REQUIRE`, `FEATURE`,
  `PROVIDES`, `PROFILE`, `DEFINE`, `CAPABILITY`; `hxc kit` y
  `hxc build --kit`.
- **Consultas `.hxq`**: búsqueda de paquetes por predicados (`PROVIDES`,
  `FEATURE`, `CAPABILITY`, `DEP`, `VERSION`) sin compilar nada.
- **Capacidades**: `ENABLE` en el fuente, `CAPABILITY` en el manifiesto,
  `std.net` con sockets por syscall directa (`NET_UDP`, `NET_TCP`, `NET_BIND`,
  `NET_SEND`, `NET_RECV`, `NET_LISTEN`, `NET_ACCEPT`, `NET_CONNECT`,
  `NET_CLOSE`).
- **Herramientas**: `hxc run`, `build`, `test`, `check`, `size`, `kit`,
  `query`, `version`; perfiles `freestanding` y `libc`; compilación paralela
  con `--jobs`.
- **Documentación**: `docs/grammar.md` (gramática completa con códigos de
  diagnóstico), `docs/manual.html` (manual interactivo con búsqueda, tema
  oscuro y tabla de errores filtrable) y trece ADR en `docs/adr/`.
- **Aritmética explícita**: `+%` `-%` (envuelve), `+|` `-|` `*|` (satura),
  mientras que `+` y `-` verifican el desbordamiento y abortan con el
  código 70.

### Corregido

- `*|` emitía el token `*|` al C: el programa fallaba al compilar con `cc` en
  lugar de dar un diagnóstico de Hixean. Ahora hay `hx_mul_sat_i32/i64`.
- `+%` y `-%` emitían `+`/`-` con desbordamiento indefinido; ahora usan
  aritmética sin signo, que es el *wrap* que promete la gramática.
- Comparar dos `STRING` con `=` o `<>` generaba C inválido
  (`hx_str` es un `struct`); ahora pasa por `hx_str_eq`.
- `DOT`, `CROSS`, `NORMALIZED` y `NORMALIZE` solo existen para `vec3` en el
  runtime y el verificador no miraba la anchura: `E0402` lo dice antes.
- Dos `DIM` del mismo nombre en el mismo ámbito generaban dos variables en C;
  ahora es `E0315`. Sombrear en un ámbito más hondo sigue permitido.
- Un `CONST` de cadena emitía `hx_lit(...)` como inicializador estático, que
  no es una constante en C; ahora la expresión se dobla en el emisor.
- Una cadena sin cerrar se lexaba hasta el final del archivo y arrastraba una
  cascada de errores falsos; ahora es `E0103`.
- El listado de directorios de `hxc query` usaba `dirent.h`, que no compila
  en Windows; ahora hay dos implementaciones.

### Notas

- No hay empaquetado `.hxv`/`.hxa`: `hxc build` produce un ELF, Mach-O o PE
  nativo.
- `audio` y `gpu` **no** existen como capacidades: no hay dispositivo ni
  compilador por objetivo con el que probarlas, y no se ha declarado ninguna
  promesa que no se pueda cumplir.
- El compilador se compila a sí mismo en C, no en Hixean.
[0.2.0]: https://github.com/wwwillcoxon/Hixean/releases/tag/v0.2.0
[0.1.0]: https://github.com/wwwillcoxon/Hixean/commit/402127b
