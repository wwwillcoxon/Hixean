# Changelog

Formato de [Keep a Changelog](https://keepachangelog.com/es-ES/1.1.0/) y
versionado semántico. El número de versión es el del compilador y del
lenguaje a la vez: `hxc version` lo imprime y los manifiestos lo comparan
con `TARGET hixe >= 0.1`.

## [0.1.0] — 2026-10-03

Primera versión pública. Compilador `hxc` en C11 sin dependencias, biblioteca
estándar mínima y gramática documentada. Lo que hay aquí se puede comprobar
con `make test` (22 programas) y `make size` (hola mundo ≤ 12 KiB).

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
  con `--jobs`; `hxc check --json` y `hxc version --json` para el editor, el
  LSP y el CI.
- **Editores**: extensión de VS Code en `editors/vscode/` con resaltado,
  quince plantillas, comandos de compilación y prueba, y el panel de problemas
  alimentado por los diagnósticos de `hxc`.
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
- Un `TYPE` sin nombre (por ejemplo un archivo que empieza con basura y acaba
  en `END TYPE`) llegaba al verificador con el nombre a NULL y lo hacía saltar
  en `strlen`. El fuzzer lo encontró; ahora es `E0202` con un nombre anónimo
  interno. Lo mismo con la variable de un `FOR`, que daba `E0206`.

### Notas

- No hay empaquetado `.hxv`/`.hxa`: `hxc build` produce un ELF, Mach-O o PE
  nativo.
- `audio` y `gpu` **no** existen como capacidades: no hay dispositivo ni
  compilador por objetivo con el que probarlas, y no se ha declarado ninguna
  promesa que no se pueda cumplir.
- El compilador se compila a sí mismo en C, no en Hixean.

[0.1.0]: https://github.com/wwwillcoxon/Hixean/releases/tag/v0.1.0