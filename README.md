# Hixean

Lenguaje de programación compilado, estático, con inferencia local.
Evolución moderna de QBasic: `PRINT "Hola mundo"` sigue siendo un programa válido.

```
$ make
$ ./build/hxc run examples/hola.hxe
Hola mundo
```

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
| M8–M12 | capacidades (`std.net`/`audio`/`gpu`), LSP, `.hxk`/`.hxq` | *pendiente* |

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