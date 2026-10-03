# ADR 0007: monomorfización, traits estáticos e iteradores perezosos

## Estado

aceptado (M5)

## Contexto

M4 tenía `Result<T,E>` como una estructura única sin genéricos: el tipo de
éxito de un `Err` lo decidía el contexto, y comparar `Result` con `Result` sólo
miraba el nombre. Faltaban tres cosas:

1. **Genéricos.** Se anunciaban en el diseño pero `TYPE`/`FUNCTION` los
   descartaban al parsear (`hx_skip_generic`). Sin ellos no hay forma de
   escribir `Caja<Int>` ni `Max(a, b)` sobre dos tipos.
2. **Traits.** Una biblioteca no puede ofrecer una función que sirva para
   cualquier tipo sin recurrir a `PTR` y conversiones.
3. **Iteradores.** Recorrer secuencias en un lenguaje sin heap obliga a decidir
   dónde vive el estado del recorrido.

El objetivo sigue siendo AOT estático, sin VM, sin tabla de personalidades y con
el perfil freestanding por debajo de 12 KiB.

## Decisión

**Monomorfización en el punto de llamada** (`src/mono.c`). Los parámetros de
tipo se declaran `<T>` y se deducen de los argumentos; cada combinación que
aparece en el programa crea una instancia. La instancia es una **copia
profunda** del cuerpo con los tipos sustituidos: compartir los nodos del AST
haría que la comprobación de una instancia escribiera sus tipos sobre los de la
siguiente. Por eso `mono.c` clona statements, expresiones, patrones y tramos de
interpolación. El tipo de un `TYPE` genérico no se renombra en el AST: el
nombre escrito se conserva y la instancia vive en `t->decl`, lo que permite
recomponer firmas clonadas sin volver a instanciar dos veces.

**Traits con despacho estático.** `TRAIT` declara firmas, `IMPLEMENTAR <tipo>
PARA <trait>` las implementa. `Trait.Metodo(a, b)` se resuelve mirando el tipo
del primer argumento y llamando directamente a esa implementación: no hay tabla
virtual ni punteros a función genéricos en el binario. `SELF` se sustituye por el tipo
que implementa.

**Iteradores perezosos con estado en arena.** El protocolo son dos campos
(`void *estado`, `int32_t (*paso)(void *, void *)`). `Rango` genera un estado
de dos enteros; `MAP`, `FILTER` y `TAKE` reservan el suyo con el asignador de
la arena que crea la sentencia `FOR .. IN` y encadenan un `paso` al anterior.
La secuencia nunca se materializa: `Rango(1, 1000000).TAKE(3)` reserva una
fracción de una arena y produce tres elementos. Como el estado vive en la arena
del bucle, `BREAK` y `CONTINUE` la liberan por el mismo mecanismo que `ARENA`.

Los ayudantes se generan en `_runtime.h` como `static inline`, indexados por el
mangled del tipo (`hx_iter_map_i_to_s`), así que un programa que no usa
iteradores no paga nada: el bloque se emite sólo si el análisis previo del
programa encuentra un `FOR .. IN`.

## Consecuencias

- `Max(3, 9)` y `Max("alfa", "beta")` son dos funciones C distintas; el
  programa paga por lo que usa, no por lo que podría usar.
- El tipo del `Ok` de un `Err(...)` lo decide la firma de retorno, y comparar
  tipos parametrizados ahora compara también los argumentos (esto destapó que
  comparar `STRING` con `>` se aceptaba en silencio: ahora da `E0307`).
- No hay polimorfismo dynamico: un trait no puede implementarse para un tipo
  que todavía no se conoce y no hay boxing.
- `MAP`/`FILTER` reciben el *nombre* de una función de primer orden, no una
  lambda: no hay valores de función en el lenguaje todavía (`E0714`).
- Los adaptadores se generan por tipo de elemento, y `MAP` por par
  (origen, resultado) porque puede cambiar el tipo.
