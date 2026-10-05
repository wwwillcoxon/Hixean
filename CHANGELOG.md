# Changelog

Formato de [Keep a Changelog](https://keepachangelog.com/es-ES/1.1.0/) y
versionado semántico. El número de versión es el del compilador y del
lenguaje a la vez: `hxc version` lo imprime y los manifiestos lo comparan
con `TARGET hixe >= 0.2`.

## [0.3.0] — sin publicar

La sección de una versión publicada no se toca: es exactamente lo que hay en su
`tag`, y lo que va después de publicar va aquí. No es una regla de estilo, la
comprueba `tools/verificar-changelog.py`, que compara las dos cosas.

### Añadido

- **Publicada la release `v0.2.0`**, con un artefacto (`linux-x64`) y su
  `SHA256SUMS`. Probada de punta a punta contra la release real, no en local: el
  instalador descarga, verifica el hash, extrae y el binario instalado compila un
  programa que importa `std.texto`. El `sha256` de la fórmula de Homebrew está
  copiado del fichero publicado y comprobado descargando el tarball aparte.
- **`ARRAY[T]` acepta `Map`, `Filter` y `Fold`.** Un ARRAY no era un ITER, así que
  había que escribir el bucle a mano con `Len` y `At`, y encima no se podía encadenar
  nada encima. Ahora se convierte en iterador con `hx_iter_darr` y a partir de ahí
  encadena igual que un `Rango`.
  ```hixean
  DIM a AS ARRAY[INT]
  FOR x IN a.Map(FUNC(n AS INT) AS INT
    RETURN n * 2
  END FUNC)
    PRINT x
  NEXT
  PRINT a.Fold(0, Suma)
  ```
  - `Fold` no produce una secuencia sino un valor, así que se emite con un ayudante
    por par de tipos: un bucle no cabe en una expresión de C, y el bucle vive en la
    función que devuelve el valor.
- **ADR 0016: el perfil `freestanding` es de Linux x86-64 y solo de Linux
  x86-64.** El perfil sin libc es el que sostiene la puerta de 12 KiB, y arrastra
  una consecuencia que no se ve en la puerta: las syscalls están escritas para una
  arquitectura y un kernel concretos. Un `asm` en línea con el número equivocado no
  avisa, el programa arranca y muere con `SIGSEGV`, y el mensaje no sirve para nada.
  Se decide por qué no se extiende a macOS ni a Windows, por qué la puerta solo se
  mide en Linux, y por qué el error de compilación no se maquilla: el
  generador de código es correcto para su destino, lo que no se puede es hacer que
  un ejecutable de Linux arranque en Windows.
- **`docs/complejos.md`.** El documento que faltaba para cuando ya no basta con
  `hxc run hola.hxe`: qué sostiene hoy un proyecto —módulos, paquetes, compilación
  incremental, interfaz sin fuentes— y qué habría que escribir para uno grande:
  `std.io`, reloj y azar, sistema de proyectos, `HTTP` sobre TCP, y por qué los
  bindings de C son una capacidad que ya existe y no una palabra clave nueva.
  Está escrito desde el código: los ejemplos del documento compilan, y cada
  afirmación sobre lo que hay se comprobó contra el fuente.
- **`docs/metadatos-repo.md`.** La descripción, la homepage y los topics del
  repositorio en GitHub son lo único del proyecto que vive fuera de los ficheros y
  que ninguna puerta puede leer, así que aquí está escrito qué poner y por qué.
  Queda pendiente porque el token de este entorno es de instalación de GitHub App y
  no tiene permiso de administración sobre el repositorio: el 403 es al escribir, no
  al leer.
- **Capacidad `time`: reloj, espera y azar.** `ENABLE time` da cinco funciones:
  `TIME_MS`, `TIME_NS`, `TIME_SLEEP`, `TIME_RANDOM` y `TIME_RANDOM_BETWEEN`. Es lo
  más pequeño que puede hacer falta un programa que mide algo, y sin esto no hay
  manera de comprobar nada que dependa del tiempo: un bucle «mientras no pase un
  segundo» necesita saber cuánto ha pasado.
  - El reloj es `CLOCK_MONOTONIC` y **su origen no se dice**: es monótono para que
    nadie lo pueda atrasar y romper una medida, pero desconocido, así que lo que
    sirve son diferencias y no el número.
  - Los rangos del azar son **medio abiertos**, como `Rango(0, n)`. Cerrarlos por
    arriba obligaría a decidir qué pasa cuando sale el máximo, y un bucle que sortea
    hasta alcanzarlo se colgaría.
  - La entropía **viene del kernel**, con `getrandom` (syscall 318) y cayendo a
    `/dev/urandom` si el kernel no lo tiene. Un LCG sembrado con la hora regala su
    clave, porque el estado inicial se prueba. A partir de ahí el reparto es
    SplitMix64 con descarte de resto, para que `TIME_RANDOM(6)` no dé el 0 más
    veces que el 5.
  - El perfil `libc` necesita `_POSIX_C_SOURCE`, que no ponía: con `-std=c11` glibc
    declara las funciones de POSIX pero no las macros, y `CLOCK_MONOTONIC` es una
    macro. Va en `_runtime.h` antes de la primera cabecera del sistema, que es el
    único sitio donde puede ir.
  - **Una puerta que compila el runtime de la otra plataforma.** Hay trozos del
    runtime que solo se emiten en un sistema —el reloj de Windows, el net de POSIX— y
    no se compilan nunca aquí, porque el `#if defined(_WIN32)` que los elige se
    evalúa al compilar `hxc`. Un error en ellos sale en el CI de Windows, y con un
    mensaje que habla de una constante que no existe. `tools/verificar-win-runtime.py`
    extrae el bloque tal cual se emite y lo compila aparte con los mismos avisos que
    el resto del proyecto, más `-Wcast-function-type`, que es el que prohibe convertir
    `FARPROC` en la firma de `BCryptGenRandom`. Eso es lo que pasó con `rand_s`, que
    además no existe en MinGW.
  - **Windows tiene su propio bloque**, con `QueryPerformanceCounter` y `BCryptGenRandom`. El
    `clock()` del CRT mide tiempo de CPU, así que un programa con una espera dentro
    mide casi cero y la parecería rota sin estarlo. Y `Sleep` del CRT solo acepta
    milisegundos enteros, así que una espera de menos de uno se redondea a cero.
  - `tests/tiempo.hxe` no comprueba ningún número exacto —un reloj da uno distinto
    cada vez— sino cosas que tienen que ser verdad siempre: que dormir 11 tarda al
    menos 11, que el dado reparte, que dos sorteos seguidos no coinciden.
- **Las funciones anónimas capturan lo que usan de fuera, por valor.** Era la
  carencia que mas limitaba a quien escribe: sin captura no hay forma de escribir
  un filtro que dependa del dato, y habia que escribir una `FUNCTION` de nivel
  superior con un parámetro de mas por cada dato del que dependía.
  ```hixean
  DIM suelo AS INT = 10
  FOR z IN Rango(1, 4).Map(FUNC(n AS INT) AS INT
    RETURN n * suelo
  END FUNC)
    PRINT z
  NEXT
  ' 10, 20, 30
  ```
  - El verificador recorre el cuerpo de la lambda buscando los nombres libres: los
    que resuelven en el ámbito de otra `FUNCTION`. Los del módulo no cuentan, que
    se ven sin cerrar nada. Y lo de dentro tapa lo de fuera: un `DIM` propio, un
    parámetro de `FOR` o un enlace de `MATCH` no se capturan.
  - El cierre es una estructura por lambda y un parámetro oculto detrás de los
    declarados. Los ayudantes de `MAP` y `FILTER` con cierre se emiten aparte, y
    solo para las combinaciones de tipos que los usan: una lambda que no captura
    sigue siendo una función normal con su puntero y no paga una llamada
    indirecta de más. En un lenguaje con una puerta de 12 KiB, pagar eso en todos
    los `MAP` para algo que no se usa no sería honesto.
  - Se guarda el valor, no una referencia, en el momento en que aparece el `MAP`.
    Con `MAP` y `FILTER` todavía no se puede distinguir, porque el iterador se
    consume en la misma sentencia; la estructura guarda el valor y es lo correcto
    para cuando los cierres sean valores de primer orden.
  - 35 programas en el corpus, 8896 <= 12288 bytes, ASan y fuzzer limpios.
- **El camino de vuelta: `s.ToInt()` y `s.ToFloat()`.** Antes solo había una
  dirección, y un programa que lee un argumento o un fichero de configuración
  tenía que escribir el parser a mano. Se escribe en el runtime, sin `strtol`
  ni `strtod`, porque el perfil `freestanding` no tiene libc.
  ```hixean
  DIM puerto AS I64 = "8080".ToInt()
  DIM precio AS FLOAT = "19.99".ToFloat()
  ```
  - Si el texto no es un número, el programa **aborta con 70** y un mensaje, en vez
    de devolver 0. Un 0 silencioso convierte un dato malo en un dato bueno.
  - `ToInt` devuelve `I64` y no `INT` a propósito: `INT` son 32 bits, y con `INT`
    `"9223372036854775807"` salía como `-1` sin decir nada. Pedir un `INT` da
    `E0301` en vez de truncar en silencio.
- **Lo que hace falta para que Hixean se encuentre y se vea al compartir el
  enlace.** Cuatro paginas con `canonical`, Open Graph completo y `twitter:card`;
  un JSON-LD `SoftwareSourceCode` en el indice; `sitemap.xml` y `robots.txt`; un
  favicon SVG y sus PNG de 16, 32, 180 y 512; una imagen de 1200x630 para la
  tarjeta de los enlaces; y un manifest del sitio que es JSON de verdad.
  - El canonical importa mas de lo que parece: GitHub Pages sirve `/Hixean/` y
    `/Hixean/index.html` como dos direcciones del mismo documento, y sin esto hay
    dos paginas con el mismo contenido de las que el buscador tiene que elegir.
  - No hay conversor de SVG ni Pillow en el repositorio, y meter una dependencia
    para cuatro rectangulos no compensa. `tools/generar-iconos.py` escribe los PNG
    a mano: `zlib` de la biblioteca estandar, una fuente de mapa de bits de 5x7
    dibujada a proposito (el sitio ya es monoespaciada, asi que pixelado encaja),
    y `--ver` para mirar el resultado en ASCII sin abrir un visor.
  - Todo esto lo vigila `site/test/humo.js`: que cada pagina tenga canonical
    absoluto y distinto, las cinco etiquetas de Open Graph, twitter:card, favicon,
    que el JSON-LD sea JSON que parsea, que los PNG tengan las medidas que
    prometen, y que el sitemap liste lo que existe.
- **El manual entra en el humo del sitio**, y eso destapo que tenia diez
  `border-radius`, contra la regla del proyecto de que las cajas van rectas. No
  se comprobaba porque el manual no estaba en la lista de paginas del test.

### Arreglado

- **Una puerta que comprueba que la versión que se anuncia es la que es.** La versión
  aparece en trece sitios —los tres pies del sitio, el subtítulo de instalación, dos
  ejemplos de instalación, el enlace a la release, el ejemplo de `git tag` del README,
  la versión de la fórmula de Homebrew y dos de winget— y nada comparaba unos con
  otros. Subir `HX_VERSION` sin subir el resto deja la página anunciando una versión que
  no existe, que es la misma clase de mentira que este proyecto lleva cuatro commits
  quitándose de encima.

  La versión no se lee de `src/common.c`: se le pregunta a `hxc version`, que es la
  única respuesta que importa y que además falla si el compilador no está construido.
  Y **no** busca cualquier `0.x.y`, porque hay versiones que deben quedarse donde
  están: «`E0212` retirado en 0.2.0» es historia, y el `0.0.1` del `.hxc` y el `0.0.0`
  del `.hxk` son formato de fichero, no versión del lenguaje. Una puerta demasiado
  ancha daría quejidos falsos y acabarían apagándola.

  Probada subi��do `HX_VERSION` a `0.3.0` sin tocar nada más: falla los once patrones y
  dice cuál es cuál, porque «no anuncia la versión» sin decir dónde no lleva a ninguna
  parte.
- **En Windows el reloj daba cero.** `QueryPerformanceCounter` cuenta marcas desde el
  arranque del sistema, y multiplicarlas por mil millones para pasarlas a
  nanosegundos se sale del `int64` en cuanto la máquina lleva un rato encendida: a 10
  MHz, un año de uptime son 3.15e20 marcas y el `int64` llega a 9.2e18. El signo se iba
  al otro lado y `TIME_NS()` devolvía un número negativo, así que las tres
  comprobaciones de tiempo del corpus fallaban y la capacidad parecía rota. Ahora se
  divide primero y solo se multiplica el resto, que siempre es menor que la
  frecuencia.
  La puerta que compila el runtime de Windows lo debería haber visto y no lo veía,
  porque su `QueryPerformanceCounter` de mentira devolvía 12345: multiplicar eso por
  mil millones no desborda nunca. Ahora el stub arranca con un año de uptime y
  comprueba que el reloj dé del orden de 3.15e16, y se ha probado que con el código
  viejo la puerta falla diciendo qué número salió.
- **La puerta del runtime de Windows no enseñaba por qué fallaba.** Decía «el runtime
  de Windows no funciona» y nada más, que es el mismo problema que sufre el primer
  día que el del primer día, con `HX_TIME_WIN undeclared`: un mensaje sin número no lleva a
  ninguna parte. Ahora imprime lo que diga el programa.
- **La página decía que Windows no pasa el corpus.** Escribía «macOS y Windows: el
  corpus pasa 33 de 34», y Windows lleva tiempo en verde entero; el corpus era de 38
  pruebas, no 34. La página además no lleva cifras ahora, y no por
  modestia: cualquier número escrito a mano ahí se queda viejo al mes siguiente y
  una página que dice «33 de 34» cuando pasan 38 es una página que miente sin que nadie
  la toque. Se queda lo que no caduca, que es que Linux pasa todas las puertas, que
  Windows pasa el corpus entero con la prueba de red omitida porque el `net` son stubs
  fuera de Linux, y que macOS tiene una en rojo y a la vista.
- **`hxc fmt` no existe y estaba escrito en pasado** dentro de la lista de «estas
  cinco cosas que convierten proyecto en lenguaje» del ADR 0013, con sus cuatro
  Sisters todas verdaderas. Sale de la lista y pasa a lo que falta, con la nota de por
  qué: una promesa en pasado que no está en ninguna parte que la compruebe es la forma
  más fácil de mentir sin querer. La extensión de VS Code ya lo tenía bien, en «qué
  le falta».
- **Tres ADR decían cosas que ya no eran ciertas.** El 0004 ponía «aceptada para M4
  (no implementado aún)» de lo que M4, M5c y M18 ya tienen detrás; el 0006 decía que
  la compilación en paralelo era «el siguiente paso natural» cuando es lo que cerró
  M9, de 4 636 ms a 1 767 ms; y el 0007 decía que `MAP` y `FILTER` reciben «el nombre
  de una función, no una lambda». Enmendados los tres con una nota de revisión fechada,
  que es como este proyecto ya hizo con el 0008.
- **`docs/complejos.md` se contradecía con el código en tres sitios**, y dos los
  escribió el mismo commit que hizo la capacidad `time`: decía que el reloj y el azar
  estaban pendientes cuando ya estaban hechos, y que existe `std.mat` cuando el único
  módulo de la biblioteca es `std.texto`. Un documento de planificación que se queda
  viejo no vale como plan, así que la sección 4 pasa a decir que está hecha y el orden
  de la lista pierde el punto que ya no queda.
- **La guía de programación decía que una `FUNC` «no captura el entorno»**, que es lo
  contrario de lo que hace desde 0.2.0, y es el PDF que se descarga. Ahora explica la
  captura por valor y por qué dos `MAP` con la misma lambda no se pisan entre sí.
- **El título de la gramática decía `v0.1-dev`** con la versión en 0.2.0.
- **La tabla de hitos se había parado en M20** y no sabía nada de este mes: la
  captura de lambdas, el camino de vuelta a número, los verbos de iterador del
  `ARRAY`, la capacidad `time` y la publicación no eran ningún hito. El libro mayor del
  proyecto era lo único que no decía qué tenía dentro.
  Hay cinco filas nuevas, M21 a M25, con su tercera columna como todos los demás:
  qué puerta demuestra que la cosa funciona. La última es de documentación y
  distribución porque el proyecto ya cuenta eso como hito —M12 mete el manual HTML y
  M13 mete la extensión, los sanitizers y el fuzzer— y no como otra cosa.
- **`verificar-cifras.py` se quedaba callado a partir de veintidós hitos.** La tabla
  de números en letra acababa ahí, así que un hito nuevo hacía fallar la puerta sin
  motivo aparente. Además el patrón del ADR no incluía la `í` acentuada, de modo que
  en cuanto una palabra la hubiera tenido, la comprobación habría dejado de encontrar
  nada y habría pasado sin comprobar. Los dos arreglados, y probados: se rompen a
  propósito las cifras y se ve quejarse.
- **El changelog se estaba reescribiendo por dentro de una versión ya publicada.**
  Veintiséis commits de trabajo posterior al tag `v0.2.0` estaban escritos dentro de su
  bloque, así que el changelog anunciaba cosas que nadie podía descargar —la captura
  de lambdas, `ToInt`, `ARRAY.Map`, la capacidad `time`— y anunciaba como ausentes
  cosas que sí estaban publicadas. La sección de `0.2.0` vuelve a ser exactamente la
  que hay en su tag, y lo de después vive en `[0.3.0] — sin publicar`.
  No ha sido un cambio de criterio sino la aplicación de una regla mecánica, y por eso
  no deja sitio a la duda: lo publicado es el tag, y lo que va después va en su
  sección. `tools/verificar-changelog.py` lo comprueba comparando el fichero con
  `git show v0.2.0:CHANGELOG.md` y dice qué línea se movió.

  La regla arregla sola una contradicción que no se había visto: dentro de la sección
  de `0.2.0` decía «el camino de vuelta (`s.ToInt()`) sigue sin existir». Al quedar
  `s.ToInt()` en 0.3.0, la frase vuelve a ser cierta de 0.2.0. Antes no era una
  contradicción sino una mezcla de dos versiones en el mismo sitio.
- **Un `TYPE` que se menciona a sí mismo reventaba el compilador.** `TYPE Nodo<T>
  … siguiente AS Nodo<T> …` se instanciaba, el clon se volvía a instanciar, y así
  hasta que la pila se desbordaba. Con un valor directo no hay forma de cortarlo, así
  que ahora el tipo queda desconocido y se dice por qué.
- **`2.5.ToString()` daba `2.0`.** El camino de texto a número —el de `ToString()` y el
  de `++` dentro de un texto— casteaba el valor a `int64_t` antes de formatearlo, y
  `(int64_t)2.5` es 2: la fracción se pierde en el cast, no en el formateo. El cast
  estaba ahí a propósito para los `INT` de 32 bits, que sin convertir no compilan en
  una llamada sin prototipo, pero se aplicaba a todos los tipos. Ahora solo castea el
  ayudante que toma un entero.
  `PRINT 2.5` siempre dio bien, porque ese camino no lleva cast, y por eso el bug se
  escondió: **el `.out` esperado se había regenerado con el bug dentro**, así que el
  corpus daba luz verde sobre un `2.0` que nunca debió imprimirse.
- **Una puerta que compara `PRINT` y `ToString` en vez de fiarse del `.out`.**
  Con el esperado arreglado el corpus ya sirve, pero la lección es que un `.out`
  regenerado sin mirar no prueba nada: cuadra con lo que el programa haga, para bien o
  para mal. Esta puerta imprime el mismo número por los dos caminos y compara las
  columnas, así que un cast que se cuele en uno de los dos sale aunque el esperado
  vuelva a mentir.
- **Encadenar verbos sobre un `ARRAY[T]` no compilaba.** El `ARRAY` se convierte en
  iterador, y su `Map` y su `Filter` llevan nombres propios —`MapD` y `FilterD`— que
  la cadena de iteradores no conocía. La cadena los tomaba por una llamada más y le
  pedía un ayudante para un elemento que no sabía: `hx_iter_filter_u`, con la `u` de
  desconocido. `a.Map(...).Filter(...)` no compilaba y `a.Map(...).Take(2)` tampoco,
  aunque el comentario de la cabecera del test decía que sí.
- **El bloque de captura de una lambda se construía con una declaración, y eso no
  vale cuando la lambda aparece dentro de una expresión.** Era la forma que se usó
  para el iterador de un `FOR`, y funciona porque ahí hay sitio. Ahora se construye
  con un literal compuesto, `&(struct hx_cap_x){ hx_v_extra }`, que es una
  expresión y vale en cualquier sitio. Con eso una lambda que captura puede ir en un
  `MAP`, un `FILTER` o un `FOLD`, encadenados o no, y dentro de un `ARENA` con la
  arena de ese bloque.
- **`Fold` con una lambda que captura reventaba el programa.** El emisor calculaba si
  la lambda capturaba y luego tiraba ese dato: pasaba `(void *)&hx_call_f` sin el
  bloque, y el ayudante llamaba a la lambda con dos argumentos cuando esperaba tres.
  Como el tercer argumento se leía de la nada, el programa moría con `SIGSEGV`. Ahora
  hay una variante del ayudante, `hx_fold_<a>_<e>cap`, con el bloque delante, y la
  llamada lo construye con un literal compuesto: un `Fold` es una expresión y en C no
  se declara nada dentro de una expresión.
  El bloque se construía con una declaración antes de la sentencia, como se hacía
  para el iterador de un `FOR`, pero un `Fold` puede salir en un `PRINT`, en un `DIM`,
  en un `RETURN` o en la condición de un `IF`, y ahí no había dónde ponerla.
- **`FUNC` capturando y `FOLD` sin capturar con el mismo par de tipos generaban un
  solo ayudante**, con la firma de la que se registró primero. El que no capturaba
  tenía que pasar un argumento de más y el enlazado lo echaba. Ahora la clave que
  registra el par incluye la captura, y salen los dos ayudantes.
- **Un `ToString()` dentro de una lambda no emitía su bloque de runtime** y el
  enlazado decía que `hx_i64_str` no existía. El motivo: las lambdas no se
  escaneaban, así que nada de lo que hubiera dentro pedía su runtime. Una `FUNC` sí
  se escanea ahora.
- **`s.Upper()` salía sin convertir** cuando se añadían métodos sin argumentos a la
  misma marca que `ToString`: la marca 2 es «método sin argumentos» y la comparten
  `Upper`, `Lower`, `Len` e `IsEmpty`. `ToString` se reconoce ahora por su nombre.
- **Un nombre suelto que es una `FUNCTION` se emitía como `&hx_call_f` incluso siendo
  el destino de una asignación**, lo que daba un destino no asignable.
- **Un identificador que era a la vez una variable y el nombre de una función se
  resolvía siempre como la función.** Como los identificadores no distinguen
  mayúsculas, `DIM suma AS INT` al lado de `FUNCTION Suma` son el mismo nombre, y en
  `Total(desde)` la línea `suma = suma + n` se emitía como una llamada a `Suma`. En
  Linux era un warning y el resultado era basura —`4198720` donde tocaba `6`—; en
  Windows, con `-Werror`, era un error de compilación. Ahora manda la variable, y
  quien marca si un nombre es una función es el checker, que es el único que lleva
  los ámbitos.
- **Las comparaciones de `tests/run.sh` no paraban el script.** `diff -u a b &&
  echo "ok"`, con `set -e`, no falla nunca: en una lista con `&&` solo el último
  comando decide el código de salida, y el último era el `echo`. Ocho
  comparaciones del corpus podían fallar en silencio y seguir. Ahora hay una
  función `comprobar` que sale con error, y las ocho pasan por ella.
- **El corpus entero pasa en Windows**, que hasta ahora no se comprobaba entero:
  el checkout convertia los `.out` a CRLF y el programa tambien escribia CRLF, que
  es cosa que compara dos cosas iguales con dos cosas iguales. Con `.gitattributes`
  fijando LF y el runtime poniendo la salida en modo binario sobre el descriptor,
  los dos lados escriben lo mismo byte a byte.
  - El modo va con `_setmode(1, _O_BINARY)` y no con `_fileno(stdout)`: en un hijo
    creado con `STARTF_USESTDHANDLES` el stream puede no estar resuelto todavia, y
    si `_fileno` devuelve -1 el `_setmode` falla sin avisar y no cambia nada.
  - `hxc size` y el test de la extension buscaban `build/hxc` cuando en Windows es
    `build/hxc.exe`. CreateProcess lo resuelve solo, asi que el corpus no se
    enteraba, pero cualquier cosa que abra el fichero por su cuenta si. Ahora hay
    un sitio unico que sabe como se llama el binario de verdad: `tools/hxc_bin.py`.
  - Tambien lo dice la salida de hxc, que es el mismo problema un nivel mas
    arriba: el CRT convertia sus `\n` en CRLF y las consultas `.hxq` devolvian
    lineas que se veian iguales y no lo eran.
  - Las herramientas de Python dicen UTF-8 al abrir ficheros y al leer la salida
    de `hxc`. En Windows, abrir sin `encoding` decodifica con la codificacion del
    sistema, y los diagnosticos del manual llegaban con las tildes rotas: el
    documento decia «se encontro Animal» y llegaba «se encontr� Animal». Lo
    peligroso es que mientras los dos lados esten mal decodificados se parecen y
    el test pasa; en cuanto uno deja de estarlo, falla sin decir por que.
  - Las consultas `.hxq` no encontraban nada en Windows: el listado de directorios
    filtraba siempre los directorios, y la consulta necesita el nombre del
    directorio para entrar en `<ruta>/<nombre>/<nombre>.hxk`.
  - Un fallo que solo sea de fin de linea se dice: `hxc test` compara normalizando
    el CRLF y, si es eso, dice cuantos CRLF sobran y avisa de la causa probable. Sin
    eso decia que esperado y obtenido eran el mismo texto, que no es informacion.
- **Un `FLOAT` se imprimía mal de dos maneras.** `PRINT 19.99` salía
  `19.989999999`, porque el formateador multiplicaba por diez nueve veces y cada
  paso redondeaba un poco más; ahora multiplica una vez y redondea. Y `PRINT 1e20`
  salía `18446744073709551615.000000000`, que no es el número que se le dio: a
  partir de 2^64 la parte entera no cabe en un `uint64` y la conversión en C no
  avisa. Ahora ese rango, y solo ese, se escribe en notación científica, perdiendo
  precisión pero no la forma.
  - Los ceros de la izquierda de la fracción **se quedan** porque son el número:
    la fracción de `1.005` son cinco dígitos con tres ceros delante, y quitarlos
    decía `1.5`. Los de la derecha sobran, y `0.0` ahora se imprime `0.0` en vez
    de `0`.
- **`verificar-ejemplos.py` decia «ok» con un FALLO cinco líneas antes.** Devolvía
  1, asi que la puerta se paraba igual, pero quien leyera el final de la salida se
  quedaba con la última línea. Ahora el «ok» solo se imprime si no falló nada, y
  en caso contrario dice cuántos de cuántos ejemplos fallaron.

## [0.2.0] — 2026-10-04

Veintiún hitos después de 0.1.0, más todo lo que hay debajo. Sigue siendo `0.x`:
el número mayor se mueve cuando algo incompatible lo obliga, y esta vez lo hay.

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

- **La release publica un artefacto, el de `linux-x64`.** Es el único camino de
  extremo a extremo que está verde: el perfil `freestanding` es de Linux, y la
  puerta de 12 KiB solo se mide ahí. En `release.yml` está escrito qué falta para
  cada una de las otras cinco. La fórmula de Homebrew dice en voz alta que no hay
  artefacto para macOS y arm64 en vez de apuntar a un 404, y el manifiesto de
  winget se marca como no publicable porque sus dos zip no existen.
  - Windows: el corpus pasa entero (33 pruebas; la de sockets se omite porque el
    net del runtime son stubs). Lo que impedía comparar era el fin de línea: el
    CRT de Windows pasa `
` a `
`, y un lenguaje que compara la salida byte a
    byte no puede tolerarlo. Ahora el punto de entrada del perfil `libc` pone la
    salida en modo binario antes de escribir nada.

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
- **El PDF tenía 3792 fragmentos de texto invisibles.** El fondo de los bloques
  de código y de los avisos fija el color de relleno, y en PDF ese color es el que
  usan también las letras: sin devolverlo a negro, todo el texto posterior salía en
  gris claro sobre blanco. Se dibujaba y no se veía, que es la peor forma de
  fallar. Ahora el color se restablece y `tools/verificar-pdf.py` recorre los
  flujos como los ve el visor y falla si algún texto sale con un relleno claro
  encima, o si se sale de la caja. También se quitan las comillas del código en
  línea que el índice imprimía y el cuerpo no.
- **La CI ha encontrado doce fallos de portabilidad** que llevaban ahí desde el
  primer día, porque el repositorio tenía un solo commit en el remoto y nunca se
  había ejecutado sobre este código. Ninguno es de la lógica del lenguaje:
  - `kit.c` usaba `dirent` y `mkdir(ruta, modo)` sin condicionar a la plataforma.
  - La decisión de `mkdir`/`_mkdir` estaba duplicada en dos ficheros, y solo una
    estaba bien condicionada.
  - `hx_wait` usaba `waitpid` y `WIFEXITED`, que no existen en Windows.
  - `main.c` declaraba `hx_arg` y el scratch de los valores por defecto dentro del
    `#else` de POSIX.
  - Los envoltorios de `printf` no declarados como tales: clang se quejaba de
    `-Wformat-nonliteral`, y al anotarlos aparecieron 40 llamadas que pasaban un
    mensaje ya formateado como formato, más dos bugs de verdad (una bandera `NULL`
    en el `CASE ELSE` de un `MATCH` y un `%.*s` con `size_t` donde toca `int`).
  - `%z` en el `printf` de mingw, `_SC_NPROCESSORS_ONLN` en macOS, `end_col` puesto
    y sin usar, y `--gc-sections`, que es de GNU ld.
  - En el código generado: `static inline` en una variable, `memcpy`/`memset`
    redeclarados (en macOS `memcpy` es una macro) y los stubs de `net`, que
    dejaban un `#endif` sin su `#if`.
  - **Queda uno abierto:** en macOS, `tests/genericos.hxt` imprime ``
    donde debe imprimir `777`. Es la interpolación de un entero en una función
    genérica, y solo falla ahí; el C generado es idéntico en los dos sistemas, así
    que la causa está en el runtime o en el enlazado de macOS y hace falta esa
    máquina para mirarlo. La CI lo deja en rojo a la vista en vez de marcarlo como
    permitido.
- **El perfil `freestanding` solo genera programas de Linux x86-64**, y hasta
  ahora no lo decía nadie en ninguna parte. Emite su propio `_start` y sus
  syscalls con `asm` en línea, así que el binario no arranca en macOS ni en
  Windows aunque el compilador se compile allí. Ahora:
  - El perfil por defecto es el que produce programas ejecutables en la máquina
    donde se compila: `freestanding` en Linux, `libc` en los otros dos, y el
    corpus dice con cuál ha pasado en vez de decir solo que pasó.
  - La puerta de 12 KiB, que mide el perfil `freestanding`, se mide solo en Linux.
  - Está escrito en la gramática, en el manual, en la guía y en la página del
    sitio. Es la afirmación más fuerte que hacía el proyecto («una release», «seis
    plataformas») y era a medias: el compilador sí, los programas no.
- **Los parámetros con valor por defecto llegan al C.** Se declaraban, el
  verificador contaba cuáles eran obligatorios y dejaba pasar la llamada con menos
  argumentos... y el emisor escribía una llamada con N argumentos para una función
  de C con N+1. `Saluda("ana")` con `saludo AS STRING = "Hola"` no compilaba.
- **`Err("...")` como argumento.** `RETURN Err(...)` dentro de una función con
  `Result<INT, STRING>` funciona desde hace tiempo, y pasar un `Err(...)` a una
  función que espera ese tipo fallaba con el inútil «se esperaba Result, se
  encontró Result». La regla que lo resolvía estaba escrita solo para el `RETURN`.
- **`STRING.ToString()`** devuelve el mismo texto. No hace falta para nada, pero si
  el método solo estuviera en los números, `algo.ToString()` cambiaría según el
  tipo.
- **Un `Result` sin los dos parámetros escritos da `E0219`** cuando `?` o `CASE
  Ok(v)` necesitan saber la carga. Antes el `?` se quedaba sin tipo, la
  comprobación se hacía con un tipo `NULL` (que no comprueba nada) y el C salía con
  `hx_result r = hx_t0.i`.
- **El sitio es una guía de instalación**, con un directorio de documentos y una
  página de términos. Ninguna caja redondeada, y `site/test/humo.js` lo vigila.
- **Hay una guía de programación en PDF** (18 páginas) generada con
  `tools/generar-pdf.py`, que escribe el PDF sin dependencias: ni pandoc, ni LaTeX,
  ni reportlab. Los 24 ejemplos del documento se compilan y se ejecutan en la
  puerta, y 20 de ellos comprueban también su salida.
- **`ToString()` en los números.** `PRINT 42` funciona desde el principio, pero no
  había forma de *convertir* un número en texto, así que no se podía escribir
  `"n = " ++ n.ToString()`: no había ningún camino de número a cadena. Ahora `INT`,
  `I64`, `FLOAT`, `BOOL` y `DURATION` tienen `ToString()`, con el texto en memoria
  propia (el runtime ya tenía el formateo para `PRINT`, pero devolvía un puntero a
  la pila, que solo servía mientras duraba la llamada).
  - El texto no se libera. Hixean no tiene recolector y no va a fingir uno; quien
    llame mucho en un bucle debe encerrarlo en un `ARENA`.
  - El camino de vuelta (`s.ToInt()`) sigue sin existir: es lo siguiente.
- **Operadores de bits.** No había ninguno: `AND` `OR` `XOR` eran `BOOL` a secas
  (`E0307`), `&` es dirección de, `|` es alternancia de patrón y `^` es
  desreferencia. Sin una forma de calcular un hash, una bandera o un protocolo con
  bits, en un lenguaje con perfil `freestanding` y syscalls directas.
  - `AND` `OR` `XOR` leen de dos maneras según el tipo de los operandos: con
    `BOOL` son booleanos y con `INT`/`I64` son la operación de bits. Mezclarlos es
    `E0307` con la nota de que a un lado le falta el otro, porque el lenguaje no
    convierte un `BOOL` a entero ni al revés por su cuenta.
  - `~` es el complemento a bits. `~` ya era un token del lexer y no significaba
    nada. `NOT` hace lo mismo sobre un entero.
  - `<<` y `>>` mueven bits, con la precedencia de C (`==` se ata más que `AND`,
    así que `x AND 1 == 1` es `x AND (1 == 1)`).
  - **El desplazamiento está comprobado.** El C no dice nada de mover 32
    posiciones un `INT` ni de mover a la izquierda un negativo. Con cuenta
    constante lo dice el verificador (`E0316`) y con cuenta variable lo dice el
    runtime, que aborta con el mismo código 70 que el desbordamiento de la suma.
- **Se pueden sobrecargar los operadores que faltaban.** Se anunciaban
  `+ - * / MOD ++ == <> < <= > >=`, y de ellos solo funcionaban `+ - * / == <`:
  - `<>` y `!=` no se sobrecargaban. El parser acepta las dos grafías pero el
    emisor escribe siempre `<>`, así que la clave de búsqueda y el nombre de la
    función no coincidían.
  - `<= > >=` no se sobrecargaban porque `hx_binop_spelling` devolvía `"%"` para
    `MOD` y no tenía grafía para ellos.
  - **`MOD` no se sobrecargaba, en dos sitios a la vez**: el verificador
    preguntaba si el nombre de la función era un operador buscando caracteres
    raros, y `OPERATOR MOD` se llamaba `mod`, que es un nombre corriente. Ahora
    se usa `is_operator`, que es lo que el parser ya sabe. También el parser
    ignoraba el signo cuando venía como palabra clave, y se quedaba con el
    nombre literal `operator`.
  - `+%` `-%` `+|` `-|` `*|` ahora se pueden sobrecargar: estaban en la tabla del parser
    y no en el documento.
  - Sobrecargar `!=` y `<>` a la vez da `E0213` diciendo que son el mismo
    operador, en vez de dos definiciones con el mismo nombre en el C.
- **Los vectores se pueden escribir.** `VEC2`, `VEC3` y `VEC4` estaban en el
  AST, el verificador, el emisor, el runtime generado y el `.hxc`, y no había
  forma de declarar una variable de ese tipo: no estaban en la lista de tipos
  incorporados. Ahora cada verbo tiene además la forma de método (`a.DOT(b`
  además de `DOT(a, b)`), `ADD`, `SUB` y `SCALE` que solo estaban en el runtime,
  y un `DIM` sin valor inicial es el vector cero (antes emitía
  `hx_vec3 v = hx_zero_int32()`, que el C no acepta).
  - **`LEN` de un vector devolvía la longitud al cuadrado**: `LEN((1,2,3))` decía
    14 en vez de 3.74. `NORMALIZED` se compensaba con una raíz propia, así que
    el fallo no se notaba; ahora la longitud es la longitud.
  - Cuatro `FLOAT` en un literal ya no se adivinan como `QUAT`: adivinar el tipo
    de un literal es peor que no adivinar, y `DIM q AS VEC4 = (1.0, 2.0, 3.0,
    4.0)` fallaba con un `E0301` que no cuadraba con nada.
  - `MAT4` y `QUAT` se quedan **sin nombre** a propósito: no hay constructor.
- **El guard de un `CASE` se emitía antes que las ligaduras del patrón**:
  `CASE x WHEN x > 10` generaba C que no compilaba, con `hx_v_x` sin declarar.
  Ahora cada `CASE` declara sus ligaduras antes de evaluar su condición. El
  `else if` encadenado no servía (al terminar un `CASE` sus ligaduras ya están
  muertas y el siguiente las necesita vivas), así que los brazos van planos con
  una bandera: mismo orden, mismo resultado.
- **`AS Result` sin escribir `<T,E>` daba el diagnóstico más inútil del
  lenguaje**: «se esperaba Result, se encontró Result». `Ok(n)` sabe su carga y
  la anotación vacía no dice cuál esperaba, así que ahora un `Result` sin
  parámetros acepta cualquier `Result`.
- **`CASE Ok(v) | Err(v)` generaba dos declaraciones del mismo nombre en C** y
  el compilador moría con un conflicto de tipos. Es `E0217` ahora, con el
  nombre que choca y la recomendación de escribir un `CASE` por alternativa.
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
