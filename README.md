# Hixean

Lenguaje de programación compilado, estático, con inferencia local.
Evolución moderna de QBasic: `PRINT "Hola mundo"` sigue siendo un programa válido.

```
$ make
$ ./build/hxc run examples/hola.hxe
Hola mundo
```

## Página

`site/` es la página pública: `index.html`, `style.css` y `script.js`, sin
dependencias ni fuentes remotas. Los ocho ejemplos de Hixean que hay en ella se
ejecutan con `hxc` y se comparan con su salida real en cada `make test`, igual
que los del manual (`tools/verificar-ejemplos.py`), porque una página con
ejemplos que mienten es peor que no tener página.

## Manual

`docs/manual.html` es el manual interactivo: los mismos ejemplos que el corpus,
con búsqueda, tema oscuro, copia de código y la tabla de diagnósticos filtrable
por familia. No necesita nada externo (un solo archivo, sin JavaScript de
terceros) y trae estilos de impresión, así que «Guardar PDF» produce un
documento paginado con el diálogo del navegador.

```
make manual
```

## Distribución

El compilador y los paquetes van por caminos distintos, y conviene no
confundirlos: el primero se instala, el segundo se consulta.

### El compilador

La fuente de verdad es la **release de GitHub**: `git tag v0.1.0` dispara
`.github/workflows/release.yml`, que compila y prueba las seis combinaciones
(linux x64/arm64, macos x64/arm64, windows x64/arm64), adjunta un tarball por
plataforma y calcula `SHA256SUMS` sobre lo que subió cada runner.

```
curl -fsSL https://raw.githubusercontent.com/wwwillcoxon/Hixean/main/tools/install.sh | sh
hxc version
```

El script verifica el checksum y **aborta sin instalar nada** si no coincide.
En Windows, `tools\install.ps1` en PowerShell. También hay fórmula de Homebrew
en `packaging/homebrew/` y manifiesto de winget en `packaging/winget/`; los dos
dejan el `sha256` marcado para copiarlo de la release en vez de escribirlo a
mano.

### Los paquetes

```
$ hxc pack aritmetica.hxk --out registro
aritmetica 1.0.0  ->  registro/aritmetica/aritmetica.hxk (1 modulos)

$ hxc install aritmetica --registry registro --into ~/.hixean/paquetes
$ hxc build --kit ~/.hixean/paquetes/aritmetica/aritmetica.hxk \
      --path ~/.hixean/paquetes -o aritmetica
```

Un registro es **un directorio**, y por tanto también un repositorio git
clonado o cualquier servidor estático: no hay servidor central, ni cuentas, ni
TLS dentro del compilador. La misma consulta `.hxq` con `--path` sirve para
todos los casos, incluido el de buscar en el directorio del proyecto.

Lo que todavía **no** hay, y conviene decir: firma de paquetes, semver completo
en las dependencias, resolución automática de conflictos y un índice central.
El capítulo 22 del manual ([`docs/manual.html`](docs/manual.html)) lo explica con
los comandos y los códigos de diagnóstico.

## Publicar la página

`site/` y `docs/manual.html` se publican en GitHub Pages con
`.github/workflows/pages.yml`, que sube el artefacto en cada push a `main` que
toque `site/` o `docs/`:

```
https://wwwillcoxon.github.io/Hixean/                  <- la página
https://wwwillcoxon.github.io/Hixean/docs/manual.html  <- el manual
```

El workflow pone `site/` en la raíz del artefacto y `docs/` al lado, que es lo
que hace que el enlace `../docs/manual.html` de la página resuelva dentro del
subruta `/Hixean/`. Se usa un workflow y no "deploy from a branch" porque el
despliegado por rama solo ofrece la raíz del repositorio o una carpeta llamada
`docs`, y en la raíz no hay un `index.html`.

Para verlo en local sin servidor:

```
python3 -m http.server -d site 8000
```

Un dominio propio (`hixean.dev`) se añade con un `CNAME` en el artefacto y el
registro `CNAME` del dominio apuntando a `<usuario>.github.io`.

## Editores

`editors/vscode/` es una extensión de VS Code con resaltado, plantillas y los
diagnósticos reales de `hxc` en el panel de problemas: lee `hxc check --json`,
así que el mensaje, el código y la posición son los del compilador. Se prueba
sin abrir el editor (`node editors/vscode/test/smoke.js`, y también desde
`make test`). El detalle está en `editors/vscode/README.md`.

## Estado

| hito | qué funciona | puerta |
|---|---|---|
| M0 | lexer, parser RD con recuperación, diagnósticos con spans/códigos, arena+interning | CI en Linux/Windows/macOS |
| M1 | `PRINT`, literales, `DIM`, asignación, backend C, perfiles `freestanding`/`libc`, `hxc size` | hola mundo ≤ 12 KiB |
| M2 | `IF`/`WHILE`/`FOR`, funciones, `TYPE`, módulos, `IMPORT`, aritmética verificada, TCO real | fib y TCO con pila de 128 KiB |
| M3 | `STRING` inmutable, interpolación, `++`, intrínsecos, `CONST` | corpus de pruebas con salida esperada |
| M4 | `Result`/`?` sin *unwinding*, `MATCH` con patrones y rangos | propagación verificada en el corpus |
| M4b | `DEFER` de bloque y de función, epílogos encadenados | orden LIFO verificado en el corpus |
| M5a | `ARENA` con reservas reales (mmap en freestanding, malloc en libc) | escape de arena rechazado por el verificador |
| M5c | `REF` con unicidad por sentencia | prestamo anidado del mismo origen rechazado |
| M6 | vectores: literales, componentes, swizzle, `DOT`/`CROSS`/`NORMALIZED` | `v.zyx` y `f(a,b).y` en el corpus |
| M5 | genéricos monomorfizados, `TRAIT`/`IMPLEMENTAR PARA` con despacho estático, iteradores lazy (`ITER<T>`, `FOR x IN`, `MAP`/`FILTER`/`TAKE`) | `TAKE` sobre 1 000 sin materializar |
| M7 | compilación por módulos con caché de objetos y unidad `.hxc` (interfaz + biblioteca) | recompilar 1 de 20 módulos: 240 ms |
| M8 | `ENUM` con `MATCH` exhaustivo, `DONDE T: Trait`, funciones anónimas `FUNC`, división verificada | `1 / 0` da `E0305`; `n / 0` aborta con 70 |
| M9 | `PTR` con `&`/`^`, compilación paralela, paquetes `.hxk` con resolución de dependencias | 20 módulos: 4 636 ms → 1 767 ms |
| M10 | subtipado estructural (LSP) con materialización de la conversión | `Perro` sirve donde se pide `Animal` |
| M11 | capacidades: `ENABLE net` con sockets por syscall directa y puerta `CAPABILITY` del manifiesto | ida y vuelta UDP por loopback |
| M12 | consultas `.hxq` (`hxc query`) para elegir paquetes por lo que ofrecen; manual HTML interactivo | consulta por `PROVIDES`+`VERSION` acierta y filtra |
| M13 | 0.1.0: licencia, changelog, política de versiones, `hxc check --json`, extensión de VS Code, `-Werror`, sanitizers y fuzzer | el corpus pasa instrumentado y 3 000 mutaciones no matan al front-end |
| M14 | distribución: releases con SHA256, `install.sh`/`install.ps1`, Homebrew, winget, y registro de paquetes con `hxc pack`/`hxc install` | el paquete instalado se construye y ejecuta desde el registro |

M4 cubre `Result<T,E>` con `Ok`/`Err`, el operador `?` y `MATCH` con
patrones de constructor, literales, rangos y bindings. El error se propaga
como valor; el runtime no tiene tabla de personalidades (ver `docs/adr/0001`).

```hixean
ARENA temporal
  DIM buf AS INT[64]      ' reserva visible; se libera al salir del bloque
  buf[0] = 1
END ARENA
```

`ARENA` es un bump allocator sobre `mmap` en el perfil freestanding (con
respaldo estático fuera de Linux x86_64) y sobre `malloc` en el perfil libc.
Los epílogos de bloque se encadenan con `goto` y una bandera de modo, así que
`DEFER` y `ARENA` comparten el mismo mecanismo sin pila en runtime.

M7 compila cada módulo a su propia unidad de traducción (`build/gen/m*.c` más
un `_entry.c` y un `_rtmem.c`) y guarda el objeto en `build/obj/<hash>.o`, con
el hash FNV-1a de la fuente C, la versión del compilador, el perfil y las
cabeceras de las que depende. Tocar un módulo recompila una sola unidad.

Las bibliotecas se publican como una unidad `.hxc` (interfaz: tipos, constantes
y firmas exportadas, con el hash del fuente) más un `lib<modulo>.a` con la
implementación. Quien use la biblioteca no necesita sus fuentes:

```
hxc build mate.hxs --emit-hxc unidades/     ' publica unidades/mate.hxc + libmate.a
hxc build usa.hxe --use-hxc unidades/       ' compila usando sólo la interfaz
```

M5 monomorfiza en el punto de llamada: `FUNCTION Max<T>(a AS T, b AS T) AS T`
comprobado con `Max(3, 9)` y con `Max("alfa", "beta")` genera dos funciones C
distintas, y lo mismo ocurre con `TYPE Caja<T>` usada como `Caja<Int>` y
`Caja<STRING>`. Los `TRAIT` declaran métodos y `IMPLEMENTAR <tipo> PARA <trait>`
los implementa; la llamada `Compara.Mayor(a, b)` se resuelve estáticamente por
el tipo del primer argumento, sin tabla virtual.

Los iteradores son perezosos de verdad: `FOR x IN Rango(1, 1000).Take(3)` sólo
construye el estado en una arena y el elemento se produce al pedirlo. `MAP`
puede cambiar el tipo del elemento (`Rango(1,3).MAP(F)` con `F: INT -> STRING`)
y `FILTER` sigue pidiendo elementos hasta que uno pasa.

```
FUNCTION Doble(n AS INT) AS INT
  RETURN n * 2
END FUNCTION

FOR x IN Rango(1, 1000000).Map(Doble).Take(3)
  PRINT x            ' 2, 4, 6: la cadena no se materializa
NEXT x
```

M8 cierra el lenguaje del núcleo:

```
ENUM Color                 ' un ENUM es un INT con nombre y se compara
  ROJO                     ' por valor, no por aritmética
  VERDE
  AZUL
END ENUM

DIM c AS Color = Color.AZUL
PRINT c.ordinal            ' 2
PRINT c.Nombre             ' "AZUL"

MATCH c                    ' el MATCH sobre un ENUM es exhaustivo
  CASE ROJO THEN PRINT "rojo"
  CASE VERDE THEN PRINT "verde"
  CASE AZUL THEN PRINT "azul"
END MATCH
```

```
FUNCTION Max<T>(a AS T, b AS T) AS T DONDE T: Compara
  RETURN Compara.Mayor(a, b)     ' Compara.Mayor está disponible por el DONDE
END FUNCTION
```

`MAP` y `FILTER` aceptan una `FUNC(...) ... END` sin capturas, que el
compilador eleva a una función del módulo:

```
FOR x IN Rango(1, 1000).Map(FUNC(n AS INT) AS INT
  RETURN n * n
END).Take(3)
  PRINT x
NEXT x
```

La división y el módulo se comprueban: `1 / 0` no compila (`E0305`) y un
divisor que sólo se conoce en ejecución aborta con 70 en vez de provocar una
`SIGFPE`.

M9 cierra las herramientas que faltaban:

`PTR` es un tipo declarable con `&x` para tomar la dirección de una variable y
`p^` para leer o escribir a través de ella. Es el puntero crudo del lenguaje:
no hay conversiones implícitas, `&` no acepta un literal (`E0720`), `&` sobre
una variable de `ARENA` se rechaza porque el puntero quedaría colgando
(`E0721`), `^` fuera de un `PTR` da `E0722` y no se puede escribir en un
miembro de un temporal (`E0723`).

```
DIM x AS INT = 5
DIM p AS PTR AS INT = &x
p^ = p^ + 1
PRINT p^              ' 6
IF p == 0 THEN PRINT "nulo"
```

La compilación de las unidades de traducción es paralela: hasta ocho procesos
`cc` simultáneos, uno por núcleo por defecto y `--jobs N` para fijarlo. En 20
módulos el tiempo de `cc` baja de 4 636 ms a 1 767 ms en esta máquina de 4
núcleos, y el resultado del programa no cambia.

Los paquetes se describen en un manifiesto `.hxk`:

```
KIT aritmetica 1.0.0
  TARGET hixe >= 0.1
  PROFILE freestanding
  ENTRY aritmetica.hxe
  DEP base >= 0.2          ' se busca en las rutas de --path
  REQUIRE vectores         ' una FEATURE propia o heredada
  PROVIDES aritmetica
  DEFINE HX_MATEMATICAS 1
END KIT
```

`hxc kit <manifiesto> --path DIR` resuelve y muestra el grafo; `hxc build --kit
<manifiesto>` construye el paquete: toma `ENTRY`, aplica `PROFILE` y pasa los
`DEFINE` a `cc`, y los directorios de las dependencias se añaden a la búsqueda de
módulos. Un paquete que no está, una versión que no encaja (`E0806`), una
`REQUIRE` sin `FEATURE` (`E0808`) o una instrucción desconocida (`E0804`) se
rechazan con el archivo y la línea.

M10 añade subtipado estructural para los registros: un `TYPE` con más campos
sirve donde se pide uno con menos, siempre que los campos comúns sean del mismo
tipo. Al aceptar la asignación, el compilador materializa la conversión
copiando campo a campo, de modo que en C no queda ninguna struct incompatible:

```
TYPE Animal            TYPE Perro             ' Perro tiene un campo más
  nombre AS STRING       nombre AS STRING
  patas AS INT           patas AS INT
END TYPE                 ladridos AS INT
                        END TYPE

DIM p AS Perro
DIM a AS Animal = p     ' copia nombre y patas
Describe(p)            ' un Perro donde se pide un Animal
```

Los campos son invariantes (no hay subtipado de covarianza) porque se puede
escribir a través de `REF`, y dos registros con un campo del mismo nombre pero
de distinto tipo siguen siendo incompatibles.

M11 añade el mecanismo de capacidades. Un programa declara lo que usa y el
manifiesto tiene que declararlo también, de modo que una revisión puede exigir
que `net` esté autorizado:

```
ENABLE net

DIM envio AS INT = NET_UDP()
PRINT NET_BIND(envio, 45000)
PRINT NET_SEND(envio, 45001, 2130706433, "hola red")
PRINT NET_RECV(recibo)
```

En el perfil `freestanding` los sockets son syscalls directas (`socket`, `bind`,
`sendto`, `recvfrom`, `listen`, `accept`, `connect`), sin libc; en el perfil
`libc` son las llamadas de la biblioteca. El programa se ejecuta en un binario
de 9 KB que habla con el kernel de verdad: la prueba de la suite manda un
datagrama por loopback y lo recibe en el otro extremo.

```
$ ./build/hxc build --kit paquete.hxk -o salida
hx: paquete.hxk usa la capacidad 'net' pero el manifiesto no la declara con
    CAPABILITY net
```

M12 cierra las consultas: un archivo `.hxq` pregunta qué paquetes hay en las
rutas sin compilar nada, con la misma forma de predicados que usa el manifiesto.

```
QUERY matematicas por encima de la base 0.2
  DEP base >= 0.2
  PROVIDES aritmetica
END QUERY
```

```
$ hxc query consultas/aritmetica.hxq --path tests/kits
aritmetica 1.0.0  tests/kits/aritmetica.hxk
```

Los predicados (`PROVIDES`, `FEATURE`, `CAPABILITY`, `DEP`, `VERSION`) se
combinan con Y y la salida va en orden alfabético, así que sirve tanto para
leerla como para compararla en un script. La descripción tras `QUERY` es texto
libre para quien abra el archivo.

`audio` y `gpu` siguen sin existir: necesitan un dispositivo o un compilador por
objetivo, y no hay forma honesta de probarlos aquí. M12 se cierra sin ellos antes
que inventar una capacidad que no abre nada.

`hxc test` ejecuta el corpus `.hxt` y compara con la salida esperada (`.hxt.out`).
Si el `.out` no existe, se escribe y la prueba se cuenta como nueva.

## Comandos

```
hxc run   <archivo.hxe> [--freestanding|--libc] [--timing] [--keep-c]
hxc build <archivo.hxe> [-o salida] [--emit-only] [--keep-c]
hxc build <archivo.hxe> --emit-hxc DIR     publica la interfaz .hxc y lib<mod>.a
hxc build <archivo.hxe> --use-hxc DIR      compila contra interfaces .hxc
hxc test  <archivo.hxt>...
hxc check <archivo.hxe>
hxc size  <binario>
hxc kit    <archivo.hxk> [--path DIR]   resuelve dependencias y muestra el plan
hxc query  <archivo.hxq> [--path DIR]   busca paquetes por lo que ofrecen
```

## Perfiles de binario

- `freestanding` (por defecto): `-nostdlib -nostartfiles`, `_start` propio,
  syscalls directas, `memcpy`/`memset` propios. **Hola mundo: 8 896 bytes.**
- `libc`: `main()` + `libc`, útil cuando se quiere `printf`/`malloc` del sistema.

## Medidas en esta máquina

GCC 13.3, x86_64, Ubuntu 24.04. Reproducible con `make test`.

| métrica | valor | nota |
|---|---|---|
| front-end + emisión a C, 10 012 líneas | **60 ms** | puerta de diseño: 200 ms |
| `cc -O2` + link del mismo caso | 928 ms | fuera de la puerta, cacheado aparte |
| el mismo caso con la caché de objetos | **1 ms** | 0 unidades recompiladas |
| 20 módulos: front-end + emisión | **19 ms** | 20 módulos cargados |
| 20 módulos en frío (22 unidades) | 4 755 ms | dominated por 22 invocaciones de `cc` |
| 20 módulos sin cambios | **9 ms** de `cc` | caché completa |
| tocar 1 de 20 módulos | **240 ms** | 1 unidad recompilada |
| hola mundo, perfil freestanding | **8 896 B** | puerta: 12 288 B |
| TCO: 10 M iteraciones, pila 128 KiB | sin desbordamiento | `for(;;)` generado por `hxc` |

## Principio de ejecución

El compilador es C11 sin dependencias más allá del C estándar: todo el AST,
los diagnósticos, la arena y la tabla de símbolos viven en un único
`build/hxc` que compila con `cc`. No hay paso de bootstrapping todavía; es
un objetivo posterior y la razón de esta elección (ver `docs/adr/0001`).

Documentación: `docs/grammar.md` (gramática) y `docs/adr/` (decisiones).