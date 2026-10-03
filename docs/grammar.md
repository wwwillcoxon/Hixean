# Gramática de Hixean (subconjunto implementado, v0.1-dev)

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
ruta        = ident , { "." , ident } ;      (* std.net *)
```

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

`BOOL INT I64 FLOAT STRING DURATION` más `REF T`, `PTR T`, `MAYBE T`,
`ARRAY[T]` y los tipos declarados con `TYPE`.

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
  detrás de un `MAYBE` siempre hay que decidir (`E0301`).
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

## 9. Módulos y unidades `.hxc`

```
modulo     = "MODULE" ident , { import | constante | tipo | "FUNCTION" funcion } ;
import     = "IMPORT" , ruta , [ "AS" ident ] ;
```

Cada módulo compila a su propia unidad de traducción (`build/gen/<modulo>.c`)
que incluye `_runtime.h`, las cabeceras de los módulos que importa y la suya.
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

Una `FUNC` no captura el entorno: el compilador la eleva a una función del
módulo con nombre generado y la referencia se toma como puntero a función. Se
acepta donde el lenguaje espera una función de primer orden, es decir en `MAP`
y `FILTER`.

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

| código | significa |
|---|---|
| `E0902` | un argumento de la capacidad no es `INT`, `I64` ni `STRING` |
| `E0306` | número de argumentos incorrecto |

Un nombre no puede declararse dos veces en el mismo ámbito: `E0315` lo dice el
verificador, no el compilador de C. En un ámbito más hondo sí se puede
sombrear, como en cualquier lenguaje con alcance léxico.

`[]` **no comprueba el rango**: `a[9]` en un arreglo de 3 es una lectura fuera
de la memoria, y el C generado sale idéntico al que escribiría una persona. Para
el acceso que aborta con un diagnóstico hay dos métodos de `ARRAY[T]`:

| método | qué hace |
|---|---|
| `a.Len()` | el tamaño, que es una constante del tipo: no cuesta código |
| `a.At(i)` | el elemento en `i`, o salida con código 70 y el índice en pantalla |

```
DIM a AS INT[5]
a.At(4) = 7          ' así se escribe: [] sigue sin comprobar
PRINT a.At(4)
PRINT a.At(9)        ' hx: indice fuera de rango: 9 no cabe en un arreglo de ese tamaño
```

Un rango en un índice (`a[1..3]`) todavía **no** está implementado: se acepta en
el parser, pero el emisor solo leería el primer elemento, así que da `E0210` en
lugar de fingir.

Dos palabras clave están reservadas y **no** hacen nada todavía, y en vez de
callar lo dicen: `NIL` (`E0211`) y `UNIQUE` en un campo (`E0212`).

### Sobrecarga de operadores

`OPERATOR <signo>` declara cómo se comporta un `TYPE` con un operador del
lenguaje. Se admiten `+` `-` `*` `/` `MOD` `++` `==` `<>` `<` `<=` `>` `>=`;
cualquier otro signo da `E0213`.

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

`COMPTIME` y las capacidades `audio` y `gpu`, que siguen sin existir: necesitan
un dispositivo o un compilador por objetivo, y no hay forma honesta de probarlos
aquí.