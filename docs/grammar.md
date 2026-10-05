# Gramática de Hixean (subconjunto implementado, v0.2)

Notación: `=` definición, `|` alternativa, `{x}` cero o más, `[x]` opcional.
Las palabras clave son ASCII-insensitive a mayúsculas; los identificadores
no ASCII se permiten sin *folding* (QBasic no los tenía).

## 0. Léxico

```
comentario   = "'" , { car , != "'" }        (* hasta fin de línea *)
             | "#" , { car , != \n }
             | "//" , { car , != \n }
             | "/*" , { … } , "*/" ;
```

**No** hay comentarios entre comillas: `"…"` es siempre una cadena. Una cadena
puede abarcar varias líneas, y por eso una línea que empieza por `"` se usa en el
corpus como bloque de comentario; si nunca se cierra, `E0103` la señala al llegar
al final del archivo.

```
entero      = [ "0" , ( "x" hex+ | "o" oct+ | "b" bin+ ) ]
            | digito , { digito | "_" } ;
flotante    = digito , { digito | "_" } , "." , digito , { digito | "_" } , [ exp ]
            | digito , { digito | "_" } , exp ;
duracion    = numero , unidad , { numero , unidad } ;   (* 1h30m, 200ms, 0.25s *)
unidad      = "ns" | "us" | "ms" | "s" | "m" | "h" | "d" ;
cadena      = '"' , { char | interp } , '"' ;
interp      = '{' , expr , '}' ;        (* el escape es "{{" *)
car         = '\\' , ( '"' | '\\' | 'n' | 'r' | 't' | '0' | '{' | '}'
                    | 'x' hex hex | 'u{' hex+ '}' )
            | ?cualquier carácter que no sea '"' ni '\' ni \{ de interpolación? ;
```

La interpolación está **siempre activa**. Una interpolación debe ser un escalar
imprimible (`BOOL`, `INT`, `I64`, `FLOAT`, `DURATION`, `STRING`); el error se
reporta con el span de la cadena completa y código `E0305`.

### Tokens de operador

Se maximizan en este orden: `<-> ** +% -% *% +| -| *| <> != == <= >= && || += -= *= /= ++ .. ->`
y luego los de un carácter `+ - * / < > = ( ) [ ] { } , . : ; ? & | ^ ~ @ $ # !`.

`%` es módulo en infijo; no existe el sufijo de tipo `%` de QBasic
(se escribe `DIM x AS INT`). `^` es XOR; la potencia será `**` (M8).

## 1. Programa

Un archivo es un módulo. `MODULE nombre … END` es opcional; sin él, el nombre
es el nombre de archivo.

```
programa    = { top_item } , EOF ;
top_item    = module | import | funcion | type | const_declaracion | sentencia ;
module      = [ "MODULE" ident ] , { top_item } , [ "END" [ "MODULE" ] ] ;
import      = "IMPORT" , ruta , [ "AS" ident ] ;
ruta        = ident , { "." , ident } ;      (* std.texto *)
```

Una ruta con puntos puede ser un archivo con puntos (`std.texto` →
`std.texto.hxs`, cuyo nombre sale del archivo) o el módulo corto que lo
contiene (`std.hxs`). Se prueban los dos, en ese orden, y el namespace con el
que se llama es el último segmento: `IMPORT std.texto` se usa como
`texto.Funcion(...)`.

Las rutas donde se buscan, en orden:

1. el directorio del archivo de entrada,
2. cada `-I DIR`, en el orden en el que se pasan,
3. `HX_LIB`, con varios directorios separados por `:` o `;`,
4. los directorios que aportan las dependencias de `--kit`,
5. `<hxc>/lib`, `<hxc>/../lib/hixean` y `<hxc>/../lib`, que es donde queda la
   biblioteca cuando el instalador pone el binario en `<prefijo>/bin`.

Si no aparece, `E0501` dice cuáles eran.

En un `.hxe` las sentencias de nivel superior forman el `Main` implícito.
Sólo `EXPORT` es visible desde otro módulo.

## 2. Declaraciones

```
funcion     = [ "EXPORT" ] "FUNCTION" ( ident | "OPERATOR" op )
              "(" [ params ] ")" [ "AS" tipo ] , { sentencia } , "END" "FUNCTION" ;
params      = param , { "," , param } ;
param       = [ "REF" ] ident , [ ( ":" | "AS" ) tipo ] , [ "=" expr ] ;
tipo        = [ "MAYBE" ] postfijo ;
postfijo    = primario , { "[" [ expr ] "]" } ;
primario    = ruta_tipo | "REF" tipo | "PTR" tipo | "(" tipo ")" ;
ruta_tipo   = ident , { "." ident } ;

type        = [ "EXPORT" ] "TYPE" ident , { campo } , "END" "TYPE" ;
campo       = [ "UNIQUE" ] ident , ( ":" | "AS" ) tipo ;

const_decl  = [ "EXPORT" ] "CONST" ident , ( "AS" | ":" ) tipo , "=" expr ;
```

## 3. Sentencias

```
sentencia   = [ ident "=" ] expr
            | ident , ( "+=" | "-=" | "*=" | "/=" ) , expr
            | "DIM" binding , { "," binding }
            | "CONST" binding , { "," binding }
            | "PRINT" [ item , { ( ";" | "," ) item } ]
            | "IF" expr "THEN" bloque [ "ELSEIF" expr "THEN" bloque ]* [ "ELSE" bloque ] "END" "IF"
            | "IF" expr "THEN" sentencia
            | "WHILE" expr , { sentencia } , [ "END" "WHILE" | "WEND" ]
            | "FOR" ident [ (":"|"AS") tipo ] "=" expr "TO" expr [ "STEP" expr ] , { sentencia } , [ "END" ] "NEXT" [ ident ]
            | "ARENA" [ ident ] , { sentencia } , "END" "ARENA"
            | "DEFER" sentencia
            | "MATCH" expr [ "AS" ident ] , { "CASE" patron [ "WHEN" expr ] "THEN" { sentencia } } [ "CASE" "ELSE" "THEN" { sentencia } ] "END" "MATCH"
            | "RETURN" [ expr ]
            | "BREAK" | "CONTINUE"
            | "EXIT" [ "(" expr ")" | "FUNCTION" | "FOR" | "WHILE" ]
            | "." ;
```

`=` es asignación en posición de sentencia y comparación en expresión;
el parser resuelve la ambigüedad parseando el lado izquierdo por encima del
nivel de comparación. `==` es comparación explícita y siempre válido.

`PRINT` imprime una línea por sentencia: un `;` final suprime el salto
(como QBasic). `,` emite un tabulador de 8 columnas.

## 4. Expresiones

Precedencia de menor a mayor:

| nivel | operadores |
|---|---|
| 1 | `OR` `XOR` |
| 2 | `AND` |
| 3 | `=` `==` `<>` `!=` `<` `<=` `>` `>=` |
| 4 | `+` `-` `++` `+%` `-%` `+|` `-|` |
| 5 | `*` `/` `MOD` `*%` `*|` |
| 6 | unario `-` `+` `NOT` |
| 7 | llamada `f(…)`, índice `a[…]`,método `a.b`, propagación `?`, `(…)` |

```
expr        = or_expr ;
or_expr     = and_expr , { ( "OR" | "XOR" ) and_expr } ;
and_expr    = cmp_expr , { "AND" cmp_expr } ;
cmp_expr    = add_expr , { cmp_op , add_expr } ;
add_expr    = mul_expr , { add_op , mul_expr } ;
mul_expr    = unary , { mul_op , unary } ;
unary       = ( "NOT" | "-" | "+" ) unary | postfijo ;
postfijo    = primario , { "(" args ")" | "[" expr [ ".." expr ] "]" | "?" | "." ident } ;
primario    = literal | ruta | "(" expr [ "," expr [ "," expr ] ] ")"
            | "REF" designador | "[" expr { "," expr } "]" ;
args        = [ ( ident ":" )? expr , { "," … } ] ;
```

No existen tuplas: `(a,b)` es `vec2`, `(a,b,c)` es `vec3`, `(a,b,c,d)` es `vec4`.
Los literales de vector llegan en M8; antes son error `E0402`.

## 5. Patrones (MATCH)

```
patron      = ( patron_rango | patron_primario ) , { "|" patron_rango } ;
patron_rango= patron_primario , [ ( ".." | "TO" ) patron_primario ] ;
patron_prim = literal | "TRUE" | "FALSE" | "_"
            | ruta , [ "(" [ patron , { "," patron } ] ")" ]
            | ident ;              (* SIEMPRE es un binding *)
```

Regla sin ambigüedad: un identificador desnudo en un patrón es siempre un
*binding*. Para comparar con una constante hay que cualificarla
(`CASE MiMod.MAX_N`, `CASE Color.Rojo`). Así un error tipográfico en un
nombre en mayúsculas nunca se convierte silenciosamente en un binding.

## 6. Tipos incorporados

`BOOL INT I64 FLOAT STRING DURATION VEC2 VEC3 VEC4` más `REF T`, `PTR T`,
`MAYBE T`, `ARRAY[T]` y los tipos declarados con `TYPE`.

### `ToString()`

`INT`, `I64`, `FLOAT`, `BOOL` y `DURATION` tienen un método: `n.ToString()`. Sin
él no hay forma de poner un número dentro de un texto, y sin eso no se puede
escribir casi nada.

```
PRINT "n = " ++ n.ToString()
```

El texto que devuelve vive en memoria propia y **no se libera**: Hixean no tiene
recolector y no va a fingir uno. Quien llame a `ToString` muchas veces en un bucle
debe encerrarlo en un `ARENA` (que se aligeran al salir del bloque).

### `ToInt()` y `ToFloat()`

El camino de vuelta: un `STRING` tiene `s.ToInt()` (devuelve `I64`) y
`s.ToFloat()` (devuelve `FLOAT`).

```
DIM puerto AS I64 = "8080".ToInt()
DIM precio AS FLOAT = "19.99".ToFloat()
```

Se escribe el parser a mano en el runtime, sin `strtol` ni `strtod`: el perfil
`freestanding` no tiene libc, y una función de la biblioteca estándar entrevería
en el binario su nombre y su versión. Además `strtol` se para en el primer carácter
raro y devuelve 0 sin decirlo, que es justo lo que no puede pasar al leer datos de
fuera.

`ToInt` devuelve **`I64` y no `INT`** a propósito: `INT` son 32 bits, y un entero
escrito en un fichero de configuración cabe en 64. Con `INT`,
`"9223372036854775807"` salía como `-1` sin decir nada. Pedir un `INT` a propósito
da `E0301` en vez de truncar en silencio.

El contrato es el de la división verificada: si el texto no es un número, el
programa **aborta con el código 70** y un mensaje que lo dice, en vez de devolver
0. Un 0 silencioso convierte un dato malo en un dato bueno. Se permite espacio al
principio y al final; cualquier otro carácter hace fallar la conversión.

Un entero que no cabe en 64 bits también aborta, y un exponente mayor que 400
también, porque un `FLOAT` no lo representa y dar 0 sería inventarse un dato.

Los ejemplos van en `tests/texto_a_numero.hxt`, y el abortar con 70 se comprueba
en `tests/run.sh` y no en el corpus: un programa que aborta a mitad no ejecuta lo
que viene después, así que su salida no probaría nada de lo que se pusiera a
continuación.

### Vectores

`VEC2`, `VEC3` y `VEC4` son `FLOAT` agrupados: un `VEC3` son tres `FLOAT` en la
pila, sin puntero ni indirección. Se escriben con un literal de componentes y se
imprimen como sus componentes.

```
DIM a AS VEC3 = (1.0, 2.0, 3.0)
DIM b AS VEC3 = (4.0, 5.0, 6.0)
DIM c AS VEC2 = (1.0, 2.0)
DIM v AS VEC4 = (1.0, 2.0, 3.0, 4.0)

PRINT a                 "(1, 2, 3)"
PRINT a.DOT(b)          32
PRINT a.CROSS(b)        "(-3, 6, -3)"
PRINT a.LEN()           "la longitud, no la longitud al cuadrado"
PRINT a.ADD(b)          "(5, 7, 9)"
PRINT a.SUB(b)
PRINT a.SCALE(2.0)
PRINT a.NORMALIZED()    "unitario: su LEN da 1"
PRINT a.x               "un componente"
PRINT a.zy              "un swizzle: (a.z, a.y)"
```

Cada verbo existe en dos formas, la de método y la de función: `a.DOT(b)` y
`DOT(a, b)` son lo mismo. Los componentes se nombran `x`, `y`, `z`, `w` (también
`r`, `g`, `b`, `a`), y un nombre de dos a cuatro letras es un *swizzle*: `a.zy`
es un `VEC2` con los componentes en el orden escrito.

Un `DIM v AS VEC3` sin valor inicial es el vector cero. Los vectores son
**valores**: `DIM copia AS VEC3 = a` copia, no apunta.

`MAT4` y `QUAT` existen en el runtime pero **no tienen nombre**: no hay manera de
construirlos (un literal de cuatro componentes es un `VEC4`, no una matriz), y dar
el nombre sin constructor sería una promesa que el lenguaje no puede cumplir.
Cuando haya constructor, entran en la lista de arriba.


### `MAYBE T`

`MAYBE T` es un valor de tipo `T` o nada. En C sale como un struct con una
bandera y el valor, y el tamaño lo pone el tipo interior.

```
FUNCTION perfil(usuario AS STRING) AS MAYBE STRING
  IF usuario == "ana" THEN RETURN "admin"
  RETURN NIL
END FUNCTION
```

- **Envolver es automático.** Un `T` donde se espera `MAYBE T` se convierte
  solo; lo contrario no: un `MAYBE T` no se desempaqueta por sorpresa, y
  detrás de un `MAYBE` siempre hay que decidir (`E0301`). Tampoco vale un
  `MAYBE U` donde se espera `MAYBE T`: hay que decidir cuál de los dos.
- **Tres métodos, ningún operador nuevo.** `m.IsNil` es un miembro sin
  paréntesis, `m.Or(x)` devuelve el valor o el reemplazo, y `m.Map(f)` aplica
  `f` sólo si hay valor y devuelve otro `MAYBE`. No hay `??`, no hay `IS`: el
  lenguaje ya resuelve métodos sobre valores y añadir un operador sería la
  excepción.
- **`NIL` sólo vale dentro de un `MAYBE`.** Como no tiene tipo propio, fuera de
  un `MAYBE` es un error (`E0211`), no un cero disfrazado.
- **`MATCH` sobre un `MAYBE`** usa `CASE NIL` para el hueco y cualquier binding
  para el valor:

```
MATCH perfil("ana")
  CASE NIL THEN PRINT "sin nombre"
  CASE nombre THEN PRINT "hola " ++ nombre
END MATCH
```

Un `MATCH` así necesita las dos ramas o da `E0405`, igual que un `Result` sin
`Ok` y `Err`.

Reglas aritméticas actuales:

| operador | ENTERO / I64 / DURATION | FLOAT |
|---|---|---|
| `+` `-` | **verificado**: trampa y salida 70 | normal |
| `+%` `-%` | wrapping | — |
| `+|` `-|` `*|` | saturante | `*|` normal |
| `*` `/` `MOD` | sin verificación | normal |

Un desbordamiento en `+`/`-` aborta con `hx: error: desbordamiento de Entero en +`.

### Bits

`AND` `OR` `XOR` leen de dos maneras y el tipo de los operandos dice cuál: con
`BOOL` son los operadores booleanos, y con `INT` o `I64` son la operación de
bits. Mezclar un `BOOL` con un entero es `E0307`, con la nota de que a un lado le
falta el otro: el lenguaje no convierte un `BOOL` a entero ni al revés por su
cuenta.

`~` es el complemento a bits y `NOT` la negación booleana. Se escriben igual a
propósito: `NOT` sobre un entero también es el complemento, y el verificador no
distingue uno de otro porque el símbolo es el mismo.

`<<` y `>>` mueven bits. Se pegan a `*` y `/` en precedencia, como en C, así que
`x AND 1 == 1` se lee como `x AND (1 == 1)` y hay que poner paréntesis.

El C no dice nada de mover 32 posiciones un `INT`, ni de mover a la izquierda un
número negativo: es indefinido. Aquí no lo es. Con cuenta constante lo dice el
verificador (`E0316`, "no cabe en un INT"), y con cuenta variable el runtime
comprueba y aborta con el mismo código 70 que el desbordamiento de la suma.

```
DIM mascara AS INT = 255
IF (p.bits AND 4) == 4 THEN p.ejecucion = TRUE
h = h XOR 255
h = ~h
PRINT h << vueltas
```


## 7. Tipos de línea

| sufijo | rol |
|---|---|
| `.hxe` | punto de entrada ejecutable |
| `.hxf` | biblioteca de funciones exportables |
| `.hxs` | módulo interno importable |
| `.hxt` | prueba (su salida esperada vive en `.hxt.out`) |
| `.hxk` | manifiesto de paquete (M9) |
| `.hxq` | consulta de paquetes (M12) |
| `.hxc` | interfaz + biblioteca publicada con `--emit-hxc` (M7) |

La tabla de arriba son los sufijos que existen. `.hxv` (build portable) y
`.hxa` (binario empaquetado) aparecen en ADR 0004 como objetivo, no como
realidad: `hxc build` deja un ELF/Mach-O/PE nativo y nada más.

## 8. Result, ? y MATCH (implementado)

```
Result<T,E>  =  tipo con dos parámetros: T (Ok) y E (Err)
Ok(x)        =  constructor de éxito
Err(e)       =  constructor de fallo
e?           =  propagación: evalúa, y si es Err devuelve el error
```

`?` es válido **sólo** como valor completo de una asignación, un `DIM`, un
`RETURN` o un elemento de `PRINT`; en cualquier otra posición es `E0403`.
El lowering es explícito, sin *unwinding*:

```c
hx_result hx_t0 = f(x);
if (hx_t0.tag) return hx_t0;      /* la función devuelve Result */
hx_str hx_v_r = hx_t0.s;          /* si no, hx_propagate_top + salida 70 */
```

En M4 `Result` tiene una representación única sin genéricos
(`{tag, i, f, s}`); la monomorfización `Result<T,E>` llega en M5.

```
MATCH expr [ AS nombre ]
  { "CASE" patron [ "WHEN" expr ] "THEN" { sentencia } }
  [ "CASE" "ELSE" "THEN" { sentencia } ]
  "END" "MATCH"
```

Un `MATCH` cuyo sujeto es un `Result` no necesita `CASE ELSE` si cubre `Ok`
y `Err`; en cualquier otro caso es obligatorio (`E0405`).

El guard de un `CASE` ve lo que el patrón liga: `CASE n WHEN n > 10 THEN` es
legítimo y `n` está disponible dentro del cuerpo. Como las ligaduras de un
`CASE` no sobreviven al siguiente, dos alternativas de un mismo patrón no pueden
ligar el mismo nombre: `CASE Ok(v) | Err(v)` da `E0217`, porque el cuerpo no
podría saber de cuál de las dos habla `v`. Un `CASE` por alternativa lo dice
mejor.

## 9. Módulos y unidades `.hxc`

```
modulo     = "MODULE" ident , { import | constante | tipo | "FUNCTION" funcion } ;
import     = "IMPORT" , ruta , [ "AS" ident ] ;
```

Cada módulo compila a su propia unidad de traducción (`build/gen/<modulo>.c`)
que incluye `_runtime.h`, las cabeceras de los módulos que importa y la suya.
> **El perfil `freestanding` es de Linux x86-64.** Emite su propio `_start` y sus
> syscalls con `asm` en línea, así que los programas que produce solo arrancan en
> Linux x86-64. El compilador en sí se compila en macOS y Windows, y allí el perfil
> por defecto pasa a ser `libc`. Poner `--freestanding` a mano sigue funcionando
> como generador de código, pero el binario que sale no se ejecuta en esos
> sistemas, y la puerta de 12 KiB solo se mide en Linux por eso.

El punto de entrada genera `_entry.c` y el perfil `freestanding` añade
`_rtmem.c` con `memcpy`/`memset`. Cada objeto se guarda en
`build/obj/<hash>.o`, donde el hash cubre la fuente C, la versión de `hxc`, el
perfil y las cabeceras de las que depende; por eso tocar un módulo recompila
una sola unidad.

Una biblioteca se publica como dos artefactos:

- `<modulo>.hxc`: interfaz versionada (número de formato y ABI) con el hash del
  fuente, la longitud y el hash del cuerpo, el nombre del módulo y los elementos
  exportados: tipos con sus campos, constantes con su valor y funciones con sus
  parámetros y su retorno.
- `lib<modulo>.a`: la implementación, con `_start`/`hx_main` renombrados a
  `__hxlib_*` para que el programa que la enlaza apporta su propio punto de
  entrada.

```
hxc build mate.hxs --emit-hxc unidades/     ' publica mate.hxc + libmate.a
hxc build usa.hxe --use-hxc unidades/       ' sin fuentes de mate en el arbol
```

Sólo se exporta lo marcado `EXPORT`: un tipo o una constante no exportados dan
`E0308` al usarse desde otro módulo. El formato tiene número de versión y se
lee con validación de cada desplazamiento y profundidad (`E0601`..`E0603`),
de modo que una unidad truncada, manipulada o de otra versión se rechaza en
lugar de fallar más tarde:

| código | significa |
|---|---|
| `E0601` | el archivo no empieza por la magia `HXCU` |
| `E0602` | truncado, hash del cuerpo incorrecto, nombres ausentes o tipo demasiado anidado |
| `E0603` | número de formato o ABI que esta versión no entiende |

## 10. Genéricos

```
funcion    = "FUNCTION" ident , [ "<" ident { "," ident } ">" ] , "(" parametros ")" ...
tipo       = ... | ident , [ "<" tipo { "," tipo } ">" ] , ...
```

Los parámetros de tipo de una `FUNCTION` o de un `TYPE` se declaran entre
corchetes angulares justo después del nombre. Los argumentos se deducen de los
argumentos de la llamada; si no se pueden deducir todos, `E0702`. Un tipo
parametrizado se unifica estructuralmente, de modo que `Caja<T>` contra
`Caja<Int>` liga `T` con `Int` y no con `Caja<Int>`.

Cada combinación de argumentos de tipo que aparece en el programa produce una
instancia: el cuerpo se comprueba y se emite una sola vez por combinación. Las
instancias se nombran `<modulo>__<funcion>__<arg1>_<arg2>`, así que
`main.Max_INT` y `main.Max_STRING` conviven en C.

| código | significa |
|---|---|
| `E0701` | número de parámetros de tipo incorrecto |
| `E0702` | no se pudo deducir un parámetro de tipo |
| `E0703` | el mismo parámetro se deduce con dos tipos distintos |
| `E0704` | la instancia falla al comprobarse (nota con el nombre de la instancia) |
| `E0705` | un `TYPE` genérico usado sin sus argumentos o con otros |

## 11. Traits

```
trait      = [ "EXPORT" ] "TRAIT" ident , { "METODO" ident "(" parametros ")" [ "AS" tipo ] } ,
              "END" "TRAIT" ;
impl       = "IMPLEMENTAR" ident "PARA" ident , { metodo } , "END" "IMPLEMENTAR" ;
metodo     = "METODO" ident "(" parametros ")" [ "AS" tipo ] , bloque , [ "END" "METODO" ] ;
```

`SELF` significa «el tipo que implementa el trait» y sólo tiene sentido dentro
de una implementación (`E0706`). El despacho es estático: `Compara.Mayor(a, b)`
mira el tipo de `a`, busca la implementación de ese tipo para ese trait y llama
directamente a esa función; si no la hay, `E0707`. Una implementación debe
incluir todos los métodos del trait (`E0707`), y una función genérica no puede
servir todavía como valor (`E0714`).

## 12. Iteradores perezosos

```
para_in    = "FOR" ident "IN" expresion , bloque , ( "END" ) "NEXT" [ ident ] ;
```

Un iterador es `ITER<T>`. El compilador reconoce los constructores `Rango` y
`RangoF` y los adaptadores `MAP`, `FILTER`, `TAKE` y `FIRST`, y los representa
con un protocolo de dos campos (`estado` y una función de paso). El estado de
cada adaptador se reserva en una arena que crea la sentencia `FOR`, de modo que
encadenar `Rango(1, 1000000).MAP(f).TAKE(3)` no reserva memoria proporcional a
la secuencia y consume sólo tres elementos.

| código | significa |
|---|---|
| `E0712` | `FOR ... IN` sobre algo que no es un iterador |
| `E0713` | adaptador aplicado a algo que no es `ITER<T>` |
| `E0717` | un constructor de iteradores (`Rango`) fuera del `FOR` |
| `E0714` | `MAP`/`FILTER` sin una función de primer orden |
| `E0715` | la función no tiene la firma que exige el adaptador |

## 13. ENUM

```
enum       = [ "EXPORT" ] "ENUM" ident , { ident } , "END" "ENUM" ;
```

Cada variante es una constante con nombre `ENUM_VARIANTE` que vale su posición.
El tipo se representa como `int32_t` en C, pero conserva su nombre: se puede
comparar, pasar a funciones, guardar en un `TYPE` y usar en un `MATCH`. No
admite aritmética (`E0308`).

| método | devuelve |
|---|---|
| `.ordinal` / `.ENUM_A_INT` | `INT` con el valor de la variante |
| `.Nombre` | `STRING` con el nombre de la variante |

Un `MATCH` sobre un `ENUM` es exhaustivo cuando aparecen todas las variantes,
igual que un `Result` con `Ok` y `Err`; si falta una, `E0405`.

## 14. Restricciones sobre genéricos

```
funcion    = ... , [ "DONDE" ident ":" ident { "," ident ":" ident } ] , ...
```

`DONDE T: Compara` exige que el tipo concreto que se ligue a `T` tenga una
implementación de ese `TRAIT`. La comprobación ocurre al instanciar: si falta,
`E0716` en el punto de llamada. Dentro del cuerpo, `Compara.Mayor(a, b)` se
resuelve por el tipo ya sustituido de `a`.

## 15. Funciones anónimas

```
lambda     = "FUNC" "(" parametros ")" [ "AS" tipo ] , { sentencia } ,
             ( "END" ) ( "FUNC" ) ;
```

Una `FUNC` se acepta donde el lenguaje espera una función de primer orden, es
decir en `MAP` y `FILTER`.

**Captura por valor.** Una `FUNC` puede leer lo que hay en el ámbito de quien la
creó, sin necesidad de pasarlo como parámetro:

```hixean
DIM suelo AS INT = 10
FOR z IN Rango(1, 4).Map(FUNC(n AS INT) AS INT
  RETURN n * suelo
END FUNC)
  PRINT z
NEXT
' 10, 20, 30
```

El compilador detecta qué nombres libres usa el cuerpo —los que resuelven en el
ámbito de otra `FUNCTION`, no los del módulo, que se ven sin cerrar nada— y les
da una estructura:

```c
struct hx_cap_hx_anon_ejemplo_0 { int64_t f0; };   /* suelo */
static int64_t hx_call_hx_anon_ejemplo_0(int64_t hx_v_n, struct hx_cap_hx_anon_ejemplo_0 *cap);
/* dentro:  hx_v_n * cap->f0  */
```

El cierre se construye en el punto donde aparece el `MAP`, copiando el valor que
tenía la variable en ese momento. Tres reglas:

- **Por valor, no por referencia.** Un bucle que crea la lambda puede seguir su
  camino sin que el valor cambie por debajo. Con `MAP` y `FILTER` esto todavía no
  se puede distinguir, porque el iterador se consume en la misma sentencia, pero
  la estructura guarda el valor y es lo correcto para cuando los cierres sean
  valores de primer orden.
- **Sin coste si no captura.** Una `FUNC` que no usa nada de fuera sigue siendo
  una función normal con su puntero, y `MAP` sigue usando el ayudante de siempre.
  Los ayudantes con cierre se generan solo para las combinaciones de tipos que
  los necesitan: en un lenguaje con una puerta de 12 KiB, pagar una llamada
  indirecta de más en todos los `MAP` para algo que no se usa no sería honesto.
- **Lo de dentro tapa lo de fuera.** Un `DIM` propio, un parámetro de `FOR` o un
  enlace de `MATCH` no se capturan aunque se llamen como una variable de fuera.

## 16. División verificada

`/` y `MOD` sobre enteros emiten una comprobación en ejecución: divisor cero
aborta con 70 (`division por cero` / `modulo por cero`) y `INT_MIN / -1`
aborta con desbordamiento. Si el divisor es una constante cero, el error es de
compilación (`E0305`). La división entre `FLOAT` conserva el resultado de IEEE.

## 17. Punteros

```
tipo       = ... | "PTR" [ ( ":" | "AS" ) ] tipo | "REF" [ ( ":" | "AS" ) ] tipo ;
unario     = "&" primario | "^" postfijo ;
postfijo   = ... | "^" | "." identificador "^" | ...
```

`&x` toma la dirección de una variable y `p^` lee o escribe a través del
puntero. `PTR T` es el tipo; `0` es el puntero nulo y se puede comparar con `==`
y `!=`. No hay conversiones implícitas ni aritmética de punteros.

| código | significa |
|---|---|
| `E0720` | `&` sobre algo que no es una variable con nombre |
| `E0721` | `&` sobre una variable de `ARENA`: quedaría colgando |
| `E0722` | `^` sobre algo que no es `PTR` |
| `E0723` | asignar a un miembro de un valor temporal (`f(a).v = 1`) |

## 18. Paquetes

```
manifiesto = "KIT" ident version ,
              { instruccion } , "END" "KIT" ;
instruccion = "ENTRY" ruta | "TARGET" version | "PROFILE" perfil
            | "DEP" ident [ ">=" version ] | "REQUIRE" ident
            | "FEATURE" ident | "PROVIDES" ident | "ASSET" ruta
            | "DEFINE" texto | "OPT" texto | "BENCH" texto | "EXPECT" texto
            | "LINK" texto | "BACKEND" texto ;
```

Un `DEP` se busca como `<ruta>/<nombre>/<nombre>.hxk` o `<ruta>/<nombre>.hxk` en
las rutas de `--path`; su `ENTRY` no se construye, pero sus `FEATURE` y
`PROVIDES` se heredan y su directorio entra en la búsqueda de módulos. Un
`REQUIRE` se satisface con una `FEATURE` propia o heredada.

| código | significa |
|---|---|
| `E0801` | el manifiesto no existe |
| `E0802` | falta `KIT` inicial o `END KIT` final |
| `E0803` | falta `ENTRY` |
| `E0804` | instrucción desconocida |
| `E0805` | cadena de dependencias más profunda de 16 |
| `E0806` | la versión encontrada no cumple la petición |
| `E0807` | el paquete no está en ninguna ruta |
| `E0808` | un `REQUIRE` sin `FEATURE` que lo satisfaga |

## 19. Subtipado estructural

Los registros tienen subtipado de anchura: un `TYPE` sirve donde se pide otro
con menos campos, siempre que los campos comúns tengan exactamente el mismo
tipo. Los campos son invariantes porque se puede escribir a través de `REF`:

```
DIM p AS Perro
DIM a AS Animal = p     ' legal: a Animal le faltan campos
DIM x AS Incompatible = p   ' E0301: 'nombre' es STRING en uno e INT en otro
```

Cuando el verificador acepta la conversión, la materializa al emitir como un
literal compuesto que copia los campos comunes, de modo que el C generado nunca
mezcla structs distintas. Los `ENUM` nunca participan: dos enums con las mismas
variantes siguen siendo tipos nominalmente distintos.

## 20. Capacidades

```
habilitar  = "ENABLE" ident { "," ident } ;
```

Una capacidad es algo que el programa no puede decidir por su cuenta: hablar con
el kernel, abrir un dispositivo, lanzar un kernel de cálculo. `ENABLE net` la
declara en el fuente; el manifiesto tiene que declararla también con
`CAPABILITY net`, y si no, `hxc build --kit` se niega a construir. Así una
revisión puede exigir que una capacidad esté autorizada sin leer el código.

Hay dos. `net` habla con el kernel por sockets. `time` da reloj, espera y azar, y es
la más pequeña de las dos: cinco funciones, que están en `src/emit.c` como un bloque
del runtime y en los dos perfiles.

```
ENABLE time

DIM t0 AS I64 = TIME_MS()
TIME_SLEEP(500)
PRINT "han pasado al menos 500 ms: " ++ ((TIME_MS() - t0) >= 500).ToString()
PRINT TIME_RANDOM(6)                 ' un dado: de 0 a 5
PRINT TIME_RANDOM_BETWEEN(10, 20)    ' de 10 a 19
```

| función | qué da |
|---|---|
| `TIME_MS()` | milisegundos de un reloj que no se sabe desde cuándo |
| `TIME_NS()` | lo mismo en nanosegundos |
| `TIME_SLEEP(ms)` | espera, como mucho; el kernel devuelve cuando puede |
| `TIME_RANDOM(max)` | un entero de `0` a `max - 1` |
| `TIME_RANDOM_BETWEEN(lo, hi)` | un entero de `lo` a `hi - 1` |

Tres decisiones que no son obvias:

**El origen del reloj no se dice y no se puede pedir.** Es monótono —no se mueve
cuando alguien cambia la hora del sistema, porque un reloj que se puede atrasar da
diferencias negativas y rompe cualquier medida— pero es desconocido, así que un
programa que lo imprima da un número distinto cada vez que se compila. Lo que sirve
son diferencias.

**Los rangos son medio abiertos**, como `Rango(0, n)`: `TIME_RANDOM(6)` sale de 0 a
5. Cerrarlos por arriba obligaría a decidir qué pasa cuando sale el máximo, y un
bucle que sortea hasta alcanzarlo se colgaría. Un rango invertido o vacío devuelve
el límite de abajo, que es lo único que se puede devolver sin inventarse un número.

**En Windows hay dos pérdidas.** Su reloj es `QueryPerformanceCounter` y su espera es
`Sleep` del CRT, que solo acepta milisegundos enteros: una espera de menos de uno se
redondea a cero. Y su azar viene de `BCryptGenRandom`, que se busca en tiempo de
ejecución para no hacer depender a todos los programas de `bcrypt.dll`; si esa DLL no
está —Windows anterior a 7— se cae a `rand()` del CRT, que es un LCG y **no sirve
para una clave**. Para algo que necesite criptografía de verdad, hay que portablearlo.

**El azar sale del kernel, no de un generador sembrado con la hora.** Un LCG con la
hora como semilla regala su clave: el estado inicial se prueba, y unas pocas semillas
bastan. La entropía se pide una vez, con `getrandom` (syscall 318) y cayendo a
`/dev/urandom` si el kernel no lo tiene; a partir de ahí el reparto es SplitMix64 con
descarte de resto, para que `TIME_RANDOM(6)` no dé el 0 más veces que el 5.

| código | significa |
|---|---|
| `E0902` | un argumento de la capacidad no es del tipo que pide |
| `E0903` | un valor de la capacidad está fuera de lo que significa: una espera negativa, un rango de azar vacío |
| `E0306` | número de argumentos incorrecto |

Un nombre no puede declararse dos veces en el mismo ámbito: `E0315` lo dice el
verificador, no el compilador de C. En un ámbito más hondo sí se puede
sombrear, como en cualquier lenguaje con alcance léxico.

`[]` **no comprueba el rango**: `a[9]` en un arreglo de 3 es una lectura fuera
de la memoria, y el C generado sale idéntico al que escribiría una persona. Para
el acceso que aborta con un diagnóstico hay métodos de `ARRAY[T]`:

| método | qué hace |
|---|---|
| `a.Len()` | el tamaño, que en uno fijo es una constante del tipo: no cuesta código |
| `a.At(i)` | el elemento en `i`, o salida con código 70 y el índice en pantalla |

```
DIM a AS INT[5]
a[4] = 7             ' escribir sin comprobar es con []
PRINT a.At(4)        ' leer sí se puede comprobar
PRINT a.At(9)        ' hx: indice fuera de rango: 9 no cabe en un arreglo de ese tamaño
```

En un arreglo dinámico, `Set(i, v)` es la escritura comprobada: mismo índice,
misma salida con el 70.

### `ARRAY[T]`: el arreglo que crece

`ARRAY[T]` (o `T[]`, que es lo mismo escrito de otra forma) es un arreglo sin
tamaño en el tipo: empieza vacío y crece con `Push`, que devuelve el largo
nuevo.

| método | en `ARRAY[T]` | en `T[n]` |
|---|---|---|
| `a.Len()` | el largo, leído del struct | constante del tipo, sin coste |
| `a.At(i)` | comprobado, como siempre | comprobado |
| `a.Set(i, v)` | escribe comprobando el índice | `E0306`: no tiene sentido, el tamaño está en el tipo |
| `a.Push(v)` | añade al final y devuelve el largo nuevo | `E0306`: no crece |
| `a.Map(f)` | devuelve `ITER[R]` | `E0306`: un tamaño fijo se puede recorrer con `At` |
| `a.Filter(f)` | devuelve `ITER[T]` | igual que `Map` |
| `a.Fold(inicial, f)` | reduce a un valor | igual que `Map` |

`Map` y `Filter` sobre un `ARRAY[T]` no son una forma distinta de las suyas sobre
un iterador: el `ARRAY` se convierte en iterador con `hx_iter_darr` y a partir de ahí
encadena igual que un `Rango`.

```
DIM a AS ARRAY[INT]
a.Push(1)
a.Push(2)
a.Push(3)

FOR x IN a.Map(FUNC(n AS INT) AS INT
  RETURN n * 2
END FUNC)
  PRINT x
NEXT                                       ' 2, 4, 6

FUNCTION Suma(acc AS INT, n AS INT) AS INT
  RETURN acc + n
END FUNCTION
PRINT a.Fold(0, Suma)                      ' 6
```

`Fold` es distinto de los otros dos: produce un **valor**, no una secuencia. La
función recibe el acumulador y el elemento, en ese orden, y `Fold` devuelve lo que
devuelva la última llamada. El acumulador puede ser de otro tipo que el elemento:
`Fold("", Concat)` sobre un `ARRAY[STRING]` va bien.

La función puede ser una lambda que capture, con la misma regla que en `Map` y
`Filter`, y el bloque se construye en el momento de la llamada:

```
DIM a AS ARRAY[INT]
a.Push(1)
a.Push(2)
DIM extra AS INT = 10
PRINT a.Fold(0, FUNC(acc AS INT, n AS INT) AS INT
  RETURN acc + n + extra
END FUNC)                              ' 23
```

Como el bloque se copia por valor y `Fold` se ejecuta entero en esa expresión, el
valor capturado es el que hay en el momento de la llamada, que es también el
momento en que se consume.

El `ARRAY` no es un `ITER` por sí solo: `FOR x IN a` no vale, porque no hay forma de
saber que se quiere empezar por el principio. Con `.Map` o `.Filter` encima sí.

No hay `Sort` ni `Revés`: ordenar exige comparar, y comparar dos `T` arbitrarios no
tiene sentido sin que el program'sabe comparar. Con `ARRAY[T]` de números se puede
con un `Fold` de intercambio —burbuja, inserción— mientras haya hueco, que es lo que
se puede hacer sin inventar una estructura de datos aparte.


```
DIM numeros AS ARRAY[INT]
PRINT numeros.Len()      ' 0
PRINT numeros.Push(10)   ' 1
PRINT numeros.Push(20)   ' 2
PRINT numeros.At(0)      ' 10
```

Un `ARRAY[T]` no lleva tamaño detrás: para uno fijo está `T[n]`. Un arreglo
dinámico tampoco es uno fijo, ni al revés (`ARRAY[3]` y `ARRAY[T]` en un mismo
sitio dan `E0301`).

Sirve de elementos de cualquier tipo (`ARRAY[Punto]`, `ARRAY[STRING]`) y vive
dentro de un registro igual que uno de tamaño fijo. Se pasa a una función como un
`ARRAY[T]` cualquiera: `FUNCTION total(a AS ARRAY[INT]) AS I64`.

**Crece por duplicación y sin `realloc`**: se reserva el bloque nuevo desde la
arena y se copia el contenido, de modo que el viejo se queda hasta que la arena
se libera. Es memoria de más a cambio de no meter `realloc` en el perfil
`freestanding`, que solo tiene `mmap`/`munmap` y `malloc`/`free`. Si un programa
empuja y pops en bucle dentro de una `ARENA`, esa memoria no se devuelve hasta
salir del bloque; con `ARENA` alrededor, ese es el sitio donde va un ciclo así.

`a[i]` sobre un arreglo dinámico lee y escribe **sin comprobar**, como en el
fijo: `a[0] = 1` es rápido y `a.At(0) = 1` no existe (la escritura comprobada es
`a.Set(0, 1)`).

Un rango en un índice (`a[1..3]`) todavía **no** está implementado: se acepta en
el parser, pero el emisor solo leería el primer elemento, así que da `E0210` en
lugar de fingir.

`UNIQUE` ya hace lo que dice: convierte un campo `REF` en el dueño del
préstamo (ver abajo). `E0212`, que decía «UNIQUE está reservado pero no
implementado», queda retirado en 0.2.0 según la política de ADR 0013; a partir de
ahora los diagnósticos de UNIQUE son `E0216` y `E0218`, y ambos llevan una nota que
menciona `E0212` para quien tenga una herramienta filtrando por ese código.

### `UNIQUE`: el campo REF es el dueño

`UNIQUE` va delante del nombre de un campo, y ese campo tiene que ser `REF`
(`E0216` si no). Asignarle una variable **mueve** el préstamo: el campo pasa a
ser el dueño y la variable deja de ser prestable.

```
TYPE Ranura
  UNIQUE slot AS REF Caja
  spare AS REF Caja
END TYPE

DIM primera AS Caja
primera.v = 1
DIM ranura AS Ranura
ranura.slot = primera          ' el campo toma el prestamo
PRINT ranura.slot^.v           ' 1: se lee a traves del campo
ranura.slot^.v = 42            ' y se escribe en la variable original
```

`^` funciona también sobre `REF`, no solo sobre `PTR`: los dos son punteros en
C, y sin `^` un campo `REF` no se podía ni leer.

Tres reglas, y las tres con código:

| código | cuándo |
|---|---|
| `E0216` | `UNIQUE` en un campo que no es `REF` |
| `E0218` | la variable ya está en otro campo `UNIQUE`, o se presta después de moverse, o el origen no es una variable con nombre |
| `E0408` | reasignar un `REF` que es una variable: el préstamo se hace al declararlo (`DIM d AS REF C = k`) |

Si el campo `UNIQUE` recibe otra variable, suelta la anterior y ésta vuelve a
ser prestable. Eso es lo que hace lineal a `UNIQUE`: cada préstamo vive en un sitio.

**Lo que no se comprueba**, y conviene saberlo: el análisis es local a la
función y ve los movimientos que ve. No sabe si dos campos de *registros
distintos* apuntan al mismo dato, ni si un `REF` que llegó como parámetro
apunta al mismo sitio que un campo `UNIQUE`, ni lo que pasa entre funciones.
Es la misma clase de límite que `a[i]` sin comprobar: se dice en voz alta en
vez de prometer un análisis de aliasing que no hay.

### Sobrecarga de operadores

`OPERATOR <signo>` declara cómo se comporta un `TYPE` con un operador del
lenguaje. Se admiten `+` `-` `*` `/` `MOD` `++` `==` `<>` `<` `<=` `>` `>=` y los
que llevan aritmética explícita: `+%` `-%` `+|` `-|` `*|`. Cualquier otro signo
da `E0213`.

`!=` es otra forma de `<>`: los dos se renombran al mismo `op_ne__<Tipo>`, así que
sobrecargar los dos es `E0213` con la explicación, no dos definiciones en el C.

```
TYPE Fraccion
  num AS INT
  den AS INT
END TYPE

FUNCTION OPERATOR + (a AS Fraccion, b AS Fraccion) AS Fraccion
  DIM r AS Fraccion
  r.num = a.num * b.den + b.num * a.den
  r.den = a.den * b.den
  RETURN r
END FUNCTION

DIM media AS Fraccion
media.num = 1
media.den = 2
DIM tercio AS Fraccion
tercio.num = 1
tercio.den = 3
DIM suma AS Fraccion = media + tercio   ' 5/6
```

Tres cosas que conviene saber:

- **El tipo del primer parámetro es el que manda.** `a * 6` con
  `OPERATOR * (a AS Fraccion, b AS INT)` funciona; dos sobrecargas del mismo
  signo conviven porque cada una se renombra a `op_add__Fraccion`.
- **`+` sobre `INT` no cambia.** La aritmética entera sigue verificada: la
  sobrecarga solo se busca cuando el operando izquierdo es un `TYPE`.
- **No hay resolución por tipos de retorno.** El primer parámetro decide; el
  segundo recibe lo que le llegue, con la conversión de siempre. Un parámetro
  que no sea de un `TYPE` declarado da `E0215`, y una sobrecarga con un número
  distinto de dos parámetros da `E0214`.

Funciones de `net` (perfil `freestanding`: syscalls directas, sin libc):

| llamada | hace |
|---|---|
| `NET_UDP()` / `NET_TCP()` | abre un socket y devuelve el descriptor |
| `NET_BIND(fd, puerto)` | enlaza a `127.0.0.1:puerto`, `0` si ok |
| `NET_LISTEN(fd, cola)` | pone a escuchar |
| `NET_CONNECT(fd, puerto)` | conecta a `127.0.0.1:puerto` |
| `NET_ACCEPT(fd)` | acepta una conexión y devuelve el nuevo descriptor |
| `NET_SEND(fd, puerto, ip, datos)` | envía; `ip` es un `I64` con los cuatro octetos |
| `NET_RECV(fd)` | espera un datagrama y devuelve su texto |
| `NET_RECV_DE(fd, REF puerto, REF ip)` | además devuelve quién envió el datagrama |
| `NET_CLOSE(fd)` | cierra el descriptor |

El texto recibido se copia en una arena, así que vive hasta que termina el
bloque. En plataformas que no son Linux x86_64, las llamadas devuelven `-1` en
lugar de romper el binario.

Los intrínsecos de vector exigen la anchura que el runtime implementa: `DOT`,
`CROSS`, `NORMALIZED` y `NORMALIZE` son de 3 componentes (`E0402` con un `vec2` o
un `vec4`), `LEN` sirve con cualquiera.

## 21. Consultas `.hxq`

Una consulta busca paquetes en las rutas por lo que ofrecen, sin compilar nada.
No es código Hixean: es un archivo aparte, como el manifiesto, para que un
gestor de dependencias pueda elegir sin ejecutar el compilador sobre el mundo.

```
consulta   = "QUERY" texto { predicado } , "END" "QUERY" ;
predicado  = "PROVIDES" ident
           | "FEATURE" ident
           | "CAPABILITY" ident
           | "DEP" ident [ op version ]
           | "VERSION" op version ;
op         = ">=" | "<=" | ">" | "<" | "=" ;
```

El texto que sigue a `QUERY` en la misma línea es una descripción libre: existe
para quien lea el archivo. Los predicados se combinan con Y, así que
`PROVIDES`+`VERSION` acota qué y `CAPABILITY`+`DEP` filtra quién depende de qué.

```
QUERY matematicas por encima de la base 0.2
  DEP base >= 0.2
  PROVIDES aritmetica
END QUERY
```

```
$ hxc query tests/queries/aritmetica.hxq --path tests/kits
aritmetica 1.0.0  tests/kits/aritmetica.hxk
```

La búsqueda recorre cada ruta de `--path` en orden alfabético y acepta tres
formas de manifiesto: `<ruta>/<nombre>.hxk`, `<ruta>/<nombre>/<nombre>.hxk` y
cualquier `<ruta>/*.hxk` suelto. La salida es `nombre version  ruta`, estable
entre máquinas, y `sin resultados` cuando nada encaja. Un manifiesto roto se
ignora en silencio: una consulta no es el sitio donde se oyen los errores de un
paquete ajeno.

| código | significa |
|---|---|
| `E0811` | el archivo de consulta no existe |
| `E0812` | falta `QUERY` inicial o `END QUERY` final |
| `E0813` | la consulta no pide nada |
| `E0814` | predicado desconocido, sin valor, o con una comparación que no admite |

## 22. Lo que este documento *no* cubre todavía

Las capacidades `audio` y `gpu`, que siguen sin existir: necesitan un dispositivo
o un compilador por objetivo, y no hay forma honesta de probarlos aquí.

Las palabras clave `COMPTIME`, `DYN`, `IMPL`, `PUBLIC`, `SHADER`, `VERTEX`,
`FRAGMENT`, `COMPUTE`, `INPUT`, `UNIFORM`, `OUTPUT`, `SHADOW`, `ASSERT`, `PURE` y
`ENTRY` tampoco están aquí, pero no por falta de soporte: nunca hicieron nada y se
quitaron en 0.2.0, así que ahora son identificadores corrientes.