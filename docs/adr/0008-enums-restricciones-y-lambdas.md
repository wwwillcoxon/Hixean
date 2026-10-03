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

**`FUNC(...) ... END` se eleva a función del módulo.** No hay valores de
función en el lenguaje ni memoria dinámica para capturas, así que una lambda se
compila como una función normal con nombre generado (`hx_anon_<modulo>_<n>`) y
se referencia con `&hx_call_...`, igual que el nombre de una función de primer
orden. Si alguna vez hace falta capturar el entorno, la decisión a revisar es
ésta, no el resto de la cadena.

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
- Las lambdas no capturan. `MAP(FUNC(x AS INT) AS INT ... END)` funciona, y un
  `FUNC` que intente capturar da un error de verificación, no un fallo raro.
- `DONDE` sólo admite traits, no tipos de clase ni restricciones múltiples
  (`T: A + B`); se comprueba una por una.
