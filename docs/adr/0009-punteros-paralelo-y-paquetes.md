# ADR 0009: punteros, compilación paralela y paquetes `.hxk`

## Estado

aceptado (M9)

## Contexto

Tres huecos que impedían usar Hixean en un proyecto real:

1. **No había punteros.** `PTR` estaba en la gramática pero `DIM p AS PTR`
   fallaba, y no había forma de tomar una dirección ni de leer a través de
   ella. La palabra estaba reservada desde el principio.
2. **La compilación era secuencial.** Veintidós invocaciones de `cc` en
   fila: 4,6 s para un programa de 20 módulos, de los que el front-end sólo
   consumía 19 ms.
3. **No había paquetes.** Veintidós palabras clave reservadas (`KIT`, `DEP`,
   `REQUIRE`, `PROVIDES`, `FEATURE`, `TARGET`, `DEFINE`…) sin uso. Para
   compartir código sólo existían las unidades `.hxc`, que no expresan versión
   ni capacidades.

## Decisión

**`PTR` con `&` y `^`.** `&x` toma la dirección de una variable y `p^` lee o
escribe a través del puntero. Es el puntero crudo y explícito: no hay
conversiones implícitas, ni aritmética de punteros, ni `NULL` más allá del
literal `0`. El verificador rechaza lo que puede colgarse o no tener sentido:
`&` sobre un literal (`E0720`), `&` sobre una variable de `ARENA` (`E0721`),
`^` sobre un tipo que no es `PTR` (`E0722`) y la escritura en un miembro de un
temporal (`E0723`). La decisión de no añadir aritmética de punteros es
deliberada: con el perfil `freestanding` y sin heap, un `PTR` aritmético sólo
serviría para interoperar con C, y para eso `REF` ya cubre el caso checked.

**Compilación paralela.** Se lanzan hasta ocho procesos `cc` a la vez, uno por
núcleo por defecto y `--jobs N` para fijarlo. Cada unidad recibe su propio
vector de argumentos porque `hx_cc_argv` escribe punteros y no se puede
compartir; el orden de espera no importa porque cada objeto es independiente.
En Windows, donde no hay `waitpid`, se compila en serie.

**Manifiesto `.hxk`.** `KIT nombre version` seguido de instrucciones
(`ENTRY`, `TARGET`, `PROFILE`, `DEP`, `REQUIRE`, `FEATURE`, `PROVIDES`,
`ASSET`, `DEFINE`, `OPT`) y `END KIT`. `hxc kit` resuelve y muestra el grafo;
`hxc build --kit` construye: toma `ENTRY`, aplica `PROFILE`, pasa los `DEFINE`
a `cc` y añade el directorio de cada dependencia a la búsqueda de módulos. Un
`DEP` busca `<ruta>/<nombre>/<nombre>.hxk` y luego `<ruta>/<nombre>.hxk`, exige
la versión pedida y hereda sus `FEATURE` y `PROVIDES`, que es lo que permite
que un `REQUIRE` se satisfaga desde abajo.

El formato es un subconjunto del lenguaje real, no un Lenguaje aparte: usa las
mismas palabras clave que ya estaban reservadas y el mismo estilo de mensajes.

## Consecuencias

- `&`/`^` no capturan ni se aliasean: un puntero a una `ARENA` es un error de
  compilación, no un fallo en tiempo de ejecución.
- El paralelismo multiplica el consumo de memoria: ocho `cc` a la vez sobre
  archivos grandes. El tope de ocho evita que un portátil se quede sin
  memoria; `--jobs` permite bajarlo.
- El manifiesto no tiene bloqueo ni resolución de conflictos: si dos
  dependencias piden versiones incompatibles de una tercera, la primera que
  se encuentra gana y la segunda falla con `E0806`. Para un gestor de
  paquetes con selección de versiones hace falta más.
- `DEFINE` llega a `cc` como `-D`, así que sólo afecta al C generado, no a la
  comprobación del lenguaje Hixean.
