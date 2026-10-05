# Proyectos complejos

Este documento es para cuando ya no basta con un `hxc run hola.hxe`. Está escrito
desde lo que hay hoy, marcando con claridad qué existe y qué habría que escribir,
porque un plan que finge que todo está a medio hacer no sirve para decidir nada.

La pregunta de fondo no es «qué librerías añadir», sino **qué es un programa
grande en Hixean**. Un lenguaje sin sistema de proyectos obliga a que cada programa
que pase de mil líneas tenga su propio Makefile, y entonces Hixean deja de ser un
lenguaje y pasa a ser un compilador que necesita andamiaje.

## Lo que ya sostiene un proyecto

No es poco, y conviene saberlo antes de hacer la lista de lo que falta:

- **Módulos y paquetes.** Un `.hxs` es un módulo, un `.hxk` un manifiesto de paquete
  con `PROVIDES` y `REQUIRE`, y `hxc query` los busca. `hxc pack` publica en un
  registro y `hxc install` instala. Hay compilación incremental por unidad de
  traducción, con el hash de la fuente C, la versión de `hxc`, el perfil y las
  cabeceras de las que depende.
- **Interfaz sin fuentes.** `hxc build --emit-hxc DIR` escribe un `.hxc` por módulo y
  el programa se compila contra esa interfaz sin ver el código. Es lo que permite
  publicar una biblioteca y que el que la usa no dependa de su implementación.
- **Perfiles.** `freestanding` sin libc, con la puerta de 12 KiB; `libc` con la
  biblioteca del sistema.
- **Puertas de verdad.** `tests/run.sh` sobre 37 programas, ASan y UBSan, fuzzer con
  los crashes conocidos guardados, `tools/verificar-utf8.py`, el manual y el sitio se
  comprueban en cada commit.

Lo que **no** hay es el paso de la propia biblioteca estándar con contenido. `std`
tiene módulos pequeños: `std.texto`, `std.mat`, `std.net`. No tiene ni un archivo.

## 1. std.io: archivos

Lo primero, porque sin esto no hay programa que guarde nada, y todo lo demás se
construye encima.

El perfil `freestanding` habla con el kernel mediante syscalls de Linux x86-64, así
que abrir un archivo ahí son tres números de syscall (`openat`, `read`, `close`) más
`lseek` para moverse. Es poco código. El problema no es la llamada: es la semántica.

Hace falta decidir cosas que el lenguaje todavía no tiene:

- **¿Un `FILE` es un valor?** Si `DIM f AS FILE` y después `f.Close()` y además
  `f.Close()` otra vez, ¿qué pasa? Hoy `STRING` es un valor y copiarlo copia el
  puntero, así que un `FILE` copiado dos veces cerraría dos veces el mismo
  descriptor. O `FILE` es un valor con un único propietario y copiar está prohibido,
  o lleva un contador, o `Close` devuelve MAYBE y no hay error que propagar porque no
  hay excepciones.
- **¿Lee todo a memoria o por trozos?** Leer un archivo de 2 GiB en un ARRAY es
  una insensatez aunque se pueda. `FOR linea IN f.Lineas()` devuelve strings y cada una
  reserva en la arena; un archivo de un millón de líneas se queda sin memoria antes
  de empezar a imprimir.
- **¿Y las rutas?** Windows usa `\`, Unix usa `/`. `std.texto` ya normaliza a `/`
  para que el manual y los `.hxq` se puedan escribir igual, y lo mismo tendría que
  hacer `std.io` o la mitad de los programas dejarán de funcionar al cambiar de
  sistema.

Y el otro perfil, `libc`, tiene `fopen`/`fread`/`fclose` y un búfer del sistema que
hice todo el trabajo. **El mismo `FILE` tiene que funcionar en los dos perfiles**, y
sus resultados no son iguales: en `libc` el búfer hace que un archivo de texto con
`\n` dé lo mismo, pero el Manejo de fin de línea cambia entre Windows y Unix. Esa
diferencia hay que decidirla, no descubrirla.

## 2. Enlazar C: una capacidad, no una palabra clave

Esta es la más importante de la lista, y la respuesta es que **ya está hecha**: no
hace falta tocar la gramática.

Cuando un programa usa un módulo publicado con `--emit-hxc`, `hx_link_objects` mete
`lib<modulo>.a` en la línea de enlace. Ese `.a` lo puede haber hecho un `hxc build
--kit` o lo puede haber hecho un `ar` a mano. No se mira lo que hay dentro. O sea:
un bindings de SDL o de zlib es un módulo Hixean con funciones `EXPORT` que
llaman a la biblioteca de C, y un `.hxk` que lo declara.

```
' modulos/c.hxs — un módulo de esos puede ser escrito en C, o en Hixean con
' IMPORTS, y para el resto del proyecto es lo mismo
EXPORT FUNCTION Load(ruta AS STRING) AS PTR AS INT
END FUNCTION
```

```
' modulos/sdl.hxs
IMPORT c

EXPORT FUNCTION Init() AS INT
  RETURN c.InitSDL()
END FUNCTION

EXPORT FUNCTION Imagen(nombre AS STRING) AS PTR AS INT
  RETURN c.ImgLoad(nombre)
END FUNCTION
```

```
KIT sdl 0.1.0
  TARGET hixe >= 0.2
  PROFILE freestanding
  ENTRY sdl.hxe
  PROVIDES sdl
  REQUIRE base
END KIT
```

El `PTR AS INT` es el tipo de puntero que Hixean ya tiene; para un puntero que C
devuelve y Hixean no debe mirar, `PTR AS INT` no es la respuesta correcta y hace
falta un `VOID*` o `ANY`, que no existen. Esa es una de las cosas que un bindings
revela: el lenguaje no tiene un tipo para «un puntero opaco».

Por qué así y no con un `EXTERN FUNCTION` en la gramática:

- **`EXTERN` metería C dentro del lenguaje**, y entonces habría que decidir qué es
  una estructura, una unión, cómo se llama a una función variádica, y qué pasa con
  los `PTR` que C devuelve y nadie pidió. Cada una de esas preguntas tiene una
  respuesta mala.
- **El módulo ya da todo lo que hace falta**: nombres calificados, tipos en la
  interfaz publicada, verificación de que el `.hxc` y el `.a` coinciden por hash, y
  `--use-hxc` para compilar sin las fuentes.
- **El enlazador es el que sabe.** Si hace falta `-lSDL2` o `-I/usr/include/SDL2`,
  eso lo decide la receta de enlace, no el lenguaje.

Lo que falta es de comfort, no de capacidad: que la receta de enlace venga en el
`.hxk` (`LINK -lSDL2`, `LINK -L/usr/lib`), que `c.hxs` deje de ser un fichero que
hay que escribir a mano, y una forma de decir «el `.a` es de aquí» para quien no
quiera instalar nada. Nada de eso necesita gramática.

## 3. Red: TCP y HTTP

`std.net` ya habla con el kernel: sockets, `connect`, `send`, `recv` por syscall en
`freestanding` y por biblioteca en `libc`. Lo que falta es lo que se escribe encima,
y es bastante:

- **`HTTP` sobre TCP.** `GET` y `POST` con cabeceras y estado, y la lectura de la
  respuesta por `Content-Length` y por `Transfer-Encoding: chunked`. TLS no: eso son
  meses de trabajo y hay que decirlo, no dejarlo para luego.
- **El bloqueo es el problema de verdad.** Todo el `std.net` actual es síncrono: un
  `recv` se queda esperando. Un servidor con un solo hilo es un servidor que se
  queda colgado con la primera conexión lenta. Hace falta o un `SELECT` con tiempo
  de espera, o hilos, o no bloqueante con un bucle de eventos. Los tres son grandes.
- **DNS.** Hoy se conecta a una IP. Resolver un nombre es un protocolo sobre UDP,
  con registros de longitud variable y reintentos, y necesita leer texto de un
  paquete que no es de confianza.
- **Y el `freestanding` obliga a duplicar.** `libc` tiene `getaddrinfo`. En
  `freestanding` hay que hablar con el resolver del sistema, que tampoco es trivial
  porque `/etc/resolv.conf` hay que leerlo.

Un orden razonable: TCP ya está, así que lo primero es un `HTTP` GET sin más, para
poder descargar una release de Hixean desde un programa escrito en Hixean. Después
el servidor, que es donde está el bloqueo. El DNS, al final.

## 4. Reloj y azar

Pequeños, y se hdicen pronto porque sin ellos no hay ninguna prueba de tiempo.

- **`Reloj()`** devuelve milisegundos desde un origen fijo. En `libc`, `clock_gettime`.
  En `freestanding`, `clock_gettime` es la syscall 228. También `HiRes()` con
  nanoseconds, porque `Sleep(1)` para medir no sirve y los juegos lo notan.
- **`Aleatorio(min, max)`** sobre `/dev/urandom`, que en `freestanding` es la
  syscall 318, con el buffer y el hash de lo leído. Sin eso, un juego que sortea
  enemigos es igual de repetible cada partida, y un programa que sortea una clave es
  peor.
- **`Sleep(ms)`** para esperar sin ocupar el procesador: `nanosleep` es la syscall
  35, y en `libc` la misma llamada.

Ninguno de los tres necesita decisión semántica difícil. Se pueden hacer juntos en
una tarde, y son la base de cualquier cosa que se pueda medir.

## 5. Sistema de proyectos: `hxc new`

El más grande y el que más cosas toca, así que va el último en la lista aunque sea
el que más falta hace.

Hoy un proyecto es un directorio con ficheros sueltos y un Makefile escrito a mano
por quien lo escribió. Con tres archivos ya hay un problema; con treinta, hay un
problema de compilación incremental, de dónde se sacan las dependencias, y de qué
pasa cuando dos personas tocan el mismo sitio.

`hxc new mi_juego` debería crear una estructura —`src/`, un `.hxk`, un `hx.json`
con lo que el proyecto es— y `hxc build` en el directorio debería saber qué
compilar, en qué orden y qué rehacer. Con eso salen gratis las cuatro cosas que hoy
no existen:

1. **Pruebas junto al código**, y `hxc test` que las encuentra y las corre. El
   corpus de 37 programas es una lista escrita a mano; un proyecto que crece se
   queda sin probar por forgotten.
2. **Compilación incremental de verdad**, no solo el `build/obj/<hash>.o` por unidad
   de traducción. Hoy existe el hash, pero quién depende de quién lo sabe el
   Makefile, no `hxc`.
3. **Un `hxq` que consulta el proyecto**, para no reescribir a mano lo que
   `hxc query` ya sabe buscar por `PROVIDES`.
4. **Un binario reproducible sin pensar en ello**, porque la receta es del proyecto
   y no de quien lo compiló.

Lo que hay que decidir antes de escribir una línea: si el manifiesto es un `.hxk`
que ya existe o un formato nuevo. La respuesta easy es que `.hxk`: ya declara
`PROVIDES`, `REQUIRE` y `TARGET`, ya se resuelve con `hxc query`, y un formato
nuevo para lo mismo es un formato más que mantener.

## Lo que no va aquí

- **Juegos.** No es que no valgan: es que la lista de arriba es la que falta, y un
  juego necesita archivos, reloj, azar, bindings de SDL y un `hxk` que lo
  empaquete. Cuando las seis cosas existan, un juego es un proyecto que usa las
  seis. Escribirlos ahora sería escribir un `import` de cinco cosas que no existen.
- **Un gestor de paquetes en red.** El registro es un directorio con `PROVIDES`
  dentro, y `hxc pack` publica en local. Que el mismo formato funcione sobre `HTTP`
  es un cambio pequeño, pero hasta que haya `HTTP` es un fichero en el aire.
- **Concurrencia.** Hilos, `LOCK`, canales, `SELECT`. No hay ni una decisión tomada
  sobre cómo se ven los datos compartidos en el lenguaje, y acertarla sin
  programs que la usen es inventarla. Con `std.net` síncrono ya hay un problema que
  resolver; el resto espera a tener programas que la sufran de verdad.
- **Un gestor de errores decente.** No hay excepciones. Hay `RESULT`, que está bien
  para lo que se ha querido usar, y `ERROR` en `E`, que es poco. Un lenguaje con tipos
  de error por valor no necesita excepciones, pero necesita una manera de subir un
  error por cuatro niveles sin escribir `IF` en los cuatro, y no está.

## Orden

1. `std.io` — sin archivos no hay nada de lo de abajo.
2. Reloj, azar y `Sleep` — una tarde, y desbloquean las pruebas de tiempo.
3. Sistema de proyectos — `hx.json`, estructura de `hxc new`, `hxc test`.
4. Enlazar C como receta de enlace — la capacidad ya existe; esto es el comfort.
5. `HTTP` sobre TCP, luego servidor, luego DNS.
6. Concurrencia, cuando haya programas que la pidan.