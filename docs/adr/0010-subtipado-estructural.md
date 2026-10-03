# ADR 0010: subtipado estructural de registros

## Estado

aceptado (M10)

## Contexto

Dos `TYPE` con la misma forma son tipos distintos: `F(b)` donde `F` espera un
`C` y `b` es un `B` con los mismos campos fallaba con `E0301`. En un lenguaje
sin herencia, esto obliga a declarar un `TYPE` por cada interfaz que se vaya a
reutilizar, o a convertir a mano campo por campo.

Hixean no tiene herencia (ver ADR 0004) ni polimorfismo dinámico (ADR 0007):
el sistema de tipos es estructural por construcción, así que la incompatibilidad
entre registros equivalentes era una contradicción con el resto del diseño.

## Decisión

**Subtipado de anchura con campos invariantes.** Un registro `B` es subtipo de
`A` si todos los campos de `A` existen en `B` con el mismo tipo. La regla es
asimétrica a propósito: `Perro` (con un campo más) sirve donde se pide
`Animal`, pero no al revés.

Los campos son invariantes, no covariantes, porque `REF` permite escribir a
través de un `PTR` y un `B` con un campo `x AS I64` no podría escribir un
`INT` donde se espera `I64`.

**La conversión se materializa.** Aceptar la asignación no basta: el C
generado tiene structs distintas. Cuando el verificador acepta una conversión
por subtipado, marca la expresión (`HxExpr.conv_ty`) y el emisor escribe un
literal compuesto que copia los campos comunes:

```c
hx_T_Animal hx_v_a = (hx_T_Animal){ .nombre = hx_v_p.nombre, .patas = hx_v_p.patas };
```

La marca se quita mientras se emiten los campos para no recursar, y se restaura
por si la misma expresión se emite dos veces.

**Los `ENUM` quedan fuera.** Dos enums con las mismas variantes son
nominalmente distintos: sus valores son enteros y el subtipado no aportaría nada
que no haga un `CONVERT`.

## Consecuencias

- La comprobación es recursiva sobre los campos y se ejecuta en cada coerción.
  Con diez niveles de anidamiento el coste es acotado por el tamaño del tipo, no
  por el número de tipos del programa.
- Una conversión copia los campos: no hay coste en tiempo de ejecución cuando el
  destino se puede escribir directamente, pero sí cuando el destino es una
  expresión de la derecha de una asignación.
- El error sigue siendo `E0301`, sin código nuevo: el mensaje ya Nombraba los dos
  tipos, así que el usuario ve la incompatibilidad concreta.
- Un `TYPE` con un campo `PTR` sigue siendo invariante: dos registros con
  punteros a la misma forma no son compatibles, porque no hay forma de comprobar
  que apuntan al mismo sitio.
