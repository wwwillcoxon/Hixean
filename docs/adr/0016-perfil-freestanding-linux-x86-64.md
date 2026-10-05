# ADR 0016: el perfil freestanding es de Linux x86-64

## Estado

aceptado (0.1.0)

## Contexto

Hixean tiene dos perfiles de generación de código, y la diferencia entre ellos no
es estética:

- **`libc`**: el enlazado normal del sistema. `printf`, `malloc`, `memcpy` vienen de
  la biblioteca de C. Es lo que se quiere al depurar, porque el depurador conoce los
  símbolos y los nombres de función salen en la traza.
- **`freestanding`**: sin biblioteca. El programa lleva su propio punto de entrada
  (`_start`), su propia `memcpy`/`memset` (`_rtmem.c`) y habla con el kernel
  mediante syscalls escritas con `asm` en línea. Lo único que enlaza es la unidad
  de traducción que produce `hxc`, y el binario no contiene libc.

El perfil `freestanding` es el que sostiene la puerta de 12 KiB. En el perfil `libc`
un programa mínimo ronda los 800 KiB de binario y unas 300 KiB de RSS, casi todo la
biblioteca; con `freestanding` el mismo programa son **8896 bytes** de binario y
`_rtmem.c` ocupa unos cientos. La puerta mide el binario, porque es lo que se puede
medir igual en todas partes y porque es lo que importa cuando el ejecutable va
dentro de otra cosa.

El perfil `freestanding` arrastra una consecuencia que no se ve en la puerta: las
syscalls están escritas para una arquitectura y un kernel concretos. `socket` es la
41 en Linux x86-64, y en otra cosa es otro número; `write` es la 1 y en macOS ni
existe como syscall. Un `asm` en línea con el número equivocado no avisa: el
programa arranca y muere con `SIGSEGV` o con `SIGSYS`, y el mensaje no dice nada
útil.

La tentación aquí es mirar la lista de sistemas que alguien ejecutó Hixean y
elegir el más común, o el que el proyecto ya soporta. La lista es Windows y macOS,
los dos sistemas donde el proyecto tiene CI verde.

Y hay un segundo problema, más sutil, en la misma decisión.

## Decisión

**El perfil `freestanding` es de Linux x86-64 y solo de Linux x86-64.**

En cualquier otro sistema el perfil por defecto es `libc`, y `--freestanding` sigue
aceptado pero produce un binario que no arranca allí. No se avisa de ello al
escribir el binario, porque el generador de código es correcto para su destino: lo
que no se puede es hacer que un ejecutable para Linux se ejecute en Windows.

Dos cosas quedan así:

1. **El compilador sí es portátil, el perfil no.** `hxc` se compila y se prueba en
   Linux, macOS y Windows. Lo que no es portable son los binarios que produce
   `freestanding`. Confundir las dos cosas llevaría a romper el CI para arreglar algo
   que no está roto.
2. **La puerta de 12 KiB solo se mide en Linux.** En macOS y Windows el objetivo es
   que el corpus pase, no que quepa. Medir la puerta donde el perfil por defecto es
   `libc` daría un número de cientos de KiB que no dice nada del perfil que la
   puerta protege.

Por qué Linux x86-64 y no otra cosa: es la única plataforma donde el proyecto tiene
un ejecutable que se puede construir, medir y ejecutar en cada commit. Añadir un
sistema al perfil `freestanding` significa mantener sus números de syscall, su
convención de `asm` en línea y su `_start` en un proyecto cuyo propósito no es
tener un kernel. Es un trabajo que se puede hacer cuando haya un usuario de ese
sistema, no antes.

## Alternativas descartadas

- **Freestanding en macOS.** Su `syscall` existe pero lleva convención de BSD y un
  número de syscall distinto para todo, y `write` no está: hay que reescribir la
  capa de entrada. Además el mercado es el que menos se pierde sin esto.
- **Freestanding en Windows.** Peor: NT usa un modelo de syscall de dos etapas
  (`Nt*` en `ntdll`) donde el número va en un registro, no en la instrucción, así
  que no cabe en `asm` en línea de la forma que se está haciendo.
- **Varios perfiles en la misma ejecución, con un flag `--freestanding` que se
  ignora en los sistemas que no lo soportan.** El programa compila y después no
  arranca, con un fallo que no dice nada. Un error de compilación o un aviso claro
  valen más que un binario muerto.
- **Medir la puerta en todos los sistemas.** Un número que significa algo distinto
  en cada sistema es peor que no medir: hace creer que hay una regresión donde la
  diferencia es el perfil.
- **Sacar la lista de sistemas soportados del propio código**, como hizo Linux en
  `/etc/os-release`. Una lista escrita a mano se queda vieja en silencio, que es
  justo el fallo que cuesta una tarde de depuración.

## Consecuencias

- El CI corre los tres sistemas para el perfil por defecto, y la puerta de tamaño
  solo en Linux. Es lo que ya hace `.github/workflows/ci.yml`.
- Un programa que necesite los dos perfiles —compilar aquí, ejecutar en un
  Raspberry Pi— no tiene todavía un flujo que lo cubra. La puerta de 12 KiB y el
  perfil `libc` son el mismo programa con dos formas de salir; si eso hace falta,
  hace falta una forma de compilar dos veces y copiar el binario, que no existe.
- `freestanding` no es portable por diseño y no se va a hacer portable: quien quiera
  otra arquitectura escribe otro perfil detrás de la misma interfaz. El coste de
  añadirlo es el de escribir su capa de syscalls, no el de tocar el emisor.
- La lista de lo que `freestanding` **no** tiene está en la página del sitio y en la
  gramática, y es larga: no hay heap del sistema, ni archivos, ni procesos, ni
  señales, ni reloj. Un ARRAY crece con memoria de la arena, que se devuelve al
  salir el bloque. Esto no es una limitación de implementación pendiente de
  levantar; es lo que significa no tener libc.