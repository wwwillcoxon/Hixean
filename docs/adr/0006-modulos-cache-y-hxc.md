# ADR 0006: compilación por módulos, caché de objetos y unidad `.hxc`

## Estado

aceptado (M7)

## Contexto

Hasta M6 el compilador emitía **un** archivo C por programa y lo compilaba con
una sola invocación de `cc`. Eso era cómodo, pero:

1. Un programa de 20 módulos tardaba lo mismo en recompilarse completo aunque
   cambiara un solo archivo, porque no había forma de saber qué parte había
   cambiado.
2. No había manera de entregar una biblioteca sin sus fuentes, que es la forma
   habitual de compartir código entre equipos o de publicar un paquete.
3. Un solo archivo de 200 KB concentra los costes fijos de `cc` (preprocesado,
   análisis de tipos, generación de código) en una unidad que no se puede
   reutilizar.

## Decisión

**Cada módulo emite su propia unidad de traducción.** El punto de entrada emite
`_entry.c` y el perfil `freestanding` añade `_rtmem.c` (donde viven `memcpy` y
`memset`, antes declarados `extern` a mano). Un módulo `.c` incluye `_runtime.h`,
las cabeceras de los módulos que importa y la suya propia.

**Caché de objetos por contenido.** Antes de invocar `cc` se calcula un hash
FNV-1a de la fuente C generada, la versión del compilador, el nivel de
optimización, el perfil y el contenido de las cabeceras de las que depende esa
unidad. Si `build/obj/<hash>.o` existe, no se invoca `cc`. La clave es el
contenido, no la marca de tiempo: el resultado no depende del orden de
compilación y un objeto obsoleto nunca se reutiliza.

**La biblioteca se publica como interfaz + implementación.** `--emit-hxc DIR`
escribe `DIR/<modulo>.hxc` y `DIR/lib<modulo>.a`:

- El `.hxc` lleva número de formato y de ABI, el hash del fuente y sólo los
  elementos exportados (tipos con sus campos, constantes con su valor,
  funciones con sus parámetros y su retorno). Los tipos se serializan como un
  árbol etiquetado, con longitud en bytes por elemento, de forma que la
  lectura pueda validar cada desplazamiento y la profundidad antes de
  materializarlo.
- La biblioteca contiene el objeto del módulo, con `_start`, `hx_main`,
  `hx_static_init` y `hx_static_arena` renombrados a `__hxlib_*` con `objcopy`.
  Sin ese renombrado, enlazar una biblioteca y el propio punto de entrada
  produciría definiciones duplicadas de `_start`.

`--use-hxc DIR` hace que, cuando un `IMPORT` no encuentre fuente, se lea la
unidad y se enlace `lib<modulo>.a` en lugar de compilar el módulo.

## Consecuencias

- Tocar un módulo recompila una unidad: 240 ms sobre 20 módulos, frente a
  4 711 ms en frío. Sin cambios, 9 ms de `cc`.
- El enlace de una biblioteca depende de que el `.hxc` y el `.a` se construyan
  del mismo commit; el hash del fuente está en el `.hxc` precisamente para que
  una herramienta pueda comprobarlo.
- El frente `.hxc` se ejecuta en orden inverso al de enlace (objetos primero,
  bibliotecas después), que es lo que espera `cc`.
- En frío, 20 módulos cuestan más que uno solo (4 711 ms frente a 890 ms) por el
  coste de lanzar `cc` 22 veces. Es el precio de la recompilación incremental y
  no se ha optimizado (compilación en paralelo es el siguiente paso natural).
- El formato es deliberadamente simple: no serializa cuerpos de función, ni
  literales compuestos, ni la tabla de tipos builtins del módulo. Un `.hxc` no
  sustituye a las fuentes para recompilar la biblioteca, sólo para usarla.