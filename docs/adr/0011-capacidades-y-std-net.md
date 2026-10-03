# ADR 0011: capacidades y `std.net`

## Estado

aceptado (M11)

## Contexto

El perfil `freestanding` habla con el kernel mediante syscalls directas y con
nada más: no hay libc, niutildades, ni sistemas de archivos, ni dispositivos. Eso es una
decisión de tamaño y de portability (ADR 0005), pero deja una pregunta: si un
programa necesita un socket, ¿qué lo impide?

Sin respuesta, `std.net` habría sido un módulo de la biblioteca estándar como
cualquier otro, importable desde cualquier programa. Eso no es un problema
mientras el lenguaje sea pequeño; lo es cuando el lenguaje pretenda ejecutar
en(targets) donde abrir un socket es una decisión de seguridad.

## Decisión

**Una capacidad se declara en el fuente y se autoriza en el manifiesto.**
`ENABLE net` en el código dice qué usa el programa. `CAPABILITY net` en el
`.hxk` dice qué se autoriza. `hxc build --kit` compara ambas listas después de
comprobar y se niega a construir si el fuente usa algo que el manifiesto no
declara. Un paquete sin manifiesto puede usar `ENABLE` (el caso de un programa
suelto), pero un paquete tiene que declararlo.

**`net` es la primera capacidad, y habla con el kernel.** Las primitivas viven en
el runtime y sólo se emiten si el programa usa `NET_*`:

- `freestanding`: syscalls directas de Linux x86_64 (`socket` 41, `connect` 42,
  `accept` 43, `sendto` 44, `recvfrom` 45, `bind` 49, `listen` 50, `close` 3) con
  `struct sockaddr_in` a mano y conversión de orden de bytes explícita. El
  binario resultante no contiene libc.
- `libc`: las llamadas de la biblioteca, que es lo que se quiere al depurar o
  al portar a otro sistema.
- otras plataformas con `freestanding`: las funciones existen y devuelven `-1`.
  Preferimos un programa que no hace nada a uno que no compila.

La API es deliberadamente plana: `NET_SEND(fd, puerto, ip, datos)` con la
dirección como un `I64` de cuatro octetos, en lugar de un `TYPE Direccion` que
habría que definir en el lenguaje y mantener sincronizado con el runtime. El
texto recibido se copia a una arena, así que vive hasta el final del bloque.

**El mecanismo está pensado para capacidades que no se pueden probar.** `audio`
y `gpu` necesitan un dispositivo de sonido o un compilador por objetivo. No se
declaran hasta que haya algo que probar; el mecanismo de `ENABLE`/`CAPABILITY`
ya las admite cuando llegue el momento.

## Consecuencias

- Un programa con `ENABLE net` compila a un binario de unos 9 KB que habla con
  sockets de verdad. La prueba de la suite manda un datagrama por loopback y lo
  recibe.
- No hay TLS, no hay resolución de nombres y no hay IPv6: `NET_SEND` habla con
  una dirección numérica. Para lo que necesita un servidor, lo que hay es un
  `NET_BIND` local.
- La comparación de capacidades es por nombre de texto, no por grafo: si un
  paquete declara `CAPABILITY net` pero su dependencia no lo hace, la puerta no
  lo detecta. Habría que propagar el uso desde las dependencias.
- El flag `--capability` que se mencionaba en la documentación anterior no
  existe: `ENABLE` en el fuente es el único interruptor.
