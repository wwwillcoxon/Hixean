# ADR 0008: ENUM, restricciones `DONDE` y funciones anónimas

## Estado

aceptado (M8)

## Contexto

M5 dejó tres huecos que se notaban al escribir código real:

1. **ENUM no existía.** La palabra clave estaba reservada desde el principio y
   el parser la descartaba. Para representar un estado había que usar
   `CONST AS INT`, que no lleva el nombre del tipo: `F(op: INT)` no dice si
   recibe un color o un código de error.
2. **Los traits no restrict.** Una función genérica no puede mencionar un
   método de trait sobre su propio parámetro de tipo, así que `Max<T>` tenía
   que repetir la comparación o viver solo con `INT`.
3. **`MAP`/`FILTER` exigían el nombre de una función.** Unalambda evitaría
   exponer una función de nivel superior que sólo existe para un `MAP`.

## Decisión

**ENUM es un INT con nombre.** `ENUM Color / ROJO / VERDE / AZUL / END ENUM`
genera una constante `Color_ROJO = 0` por variante y un tipo que se representa
como `int32_t` en C pero conserva su declaración. Se puede comparar, pasar por
referencia a `REF`, guardar en un `TYPE` y usar en un `MATCH`; no admite
aritmética. El `MATCH` sobre un `ENUM` es exhaustivo cuando aparecen todas las
variantes, con la misma regla que `Result`. Dos métodos: `.ordinal` (el entero)
y `.Nombre` (el texto, con un `switch` generado por el compilador).

**`DONDE T: Trait` se comprueba al instanciar.** El parser acepta restricciones
detrás del tipo de retorno; al crear la instancia se busca la implementación
del trait para el tipo concreto y, si no existe, `E0716` en el punto de
llamada, no dentro del cuerpo. Una vez sustituido `T`, el cuerpo puede llamar a
`Trait.Metodo(x, ...)` con el despacho estático de siempre: no hay tabla virtual
ni coste extra.

**`FUNC(...) ... END` se eleva a función del módulo.** No hay valores de función
en el lenguaje, así que una lambda se compila como una función normal con nombre
generado (`hx_anon_<modulo>_<n>`) y se referencia con `&hx_call_...`, igual que
el nombre de una función de primer orden.

**Revisada en 0.2.0: las lambdas capturan, por valor.** Esta ADR decía que no, y
que si alguna vez hiciese falta capturar la decisión a revisar era ésta. Hizo
falta: sin captura no hay forma de escribir un filtro que dependa del dato, que
es el caso que la gente quiere escribir, y obligaba a una `FUNCTION` de nivel
superior con un parámetro de más por cada dato del que dependía.

El mecanismo: el verificador recorre el cuerpo de la lambda buscando los nombres
libres —los que resuelven en el ámbito de otra `FUNCTION`— y el generador emite
una estructura por lambda, un parámetro oculto detrás de los declarados, y
ayudantes de `MAP` y `FILTER` aparte que reciben el cierre. Los ayudantes con
cierre se emiten solo para las combinaciones de tipos que los usan, para que una
lambda sin capturas no pague una llamada indirecta de más.

Se guardan los valores, no las referencias, y no por comodidad: un bucle que
crea la lambda necesita seguir su camino sin que el valor cambie por debajo.
Todavía no se puede comprobar con `MAP` y `FILTER`, que consumen el iterador en la
misma sentencia; se verá cuando los cierres sean valores de primer orden, que es
el siguiente paso natural y el que queda abierto aquí.

**División verificada.** `/` y `MOD` sobre enteros pasan por ayudantes que
comprueban el divisor cero y el desbordamiento de `INT_MIN / -1`. Con un
divisor constante cero el error es de compilación (`E0305`): es mejor no generar
código que aborta.

## Consecuencias

- El tamaño del runtime sólo crece si el programa divide: los ayudantes están en
  el bloque que ya se emite para `+`/`-` verificados.
- `ENUM` no es una unión discriminada: no hay emparejamiento con `CASE`, no hay
  `AS` con carga de valor y no hay `PRINT` que muestre el nombre sin pasar por
  `.Nombre`.
- Las lambdas capturan **por valor** desde 0.2.0. `MAP(FUNC(x AS INT) AS INT ...
  END)` sigue funcionando igual que antes cuando no usa nada de fuera; cuando lo
  usa, se genera la estructura de captura y su ayudante.
- `DONDE` sólo admite traits, no tipos de clase ni restricciones múltiples
  (`T: A + B`); se comprueba una por una.
