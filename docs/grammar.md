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

**No** hay comentarios entre comillas: `"…"` es siempre una cadena.

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
| 7 | llamada `f(…)`, índice `a[…]`,、方法 `a.b`, propagación `?`, `(…)` |

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
| `.hxk` | kit de proyecto (M6) |
| `.hxq` | paquete de capacidades (M7) |
| `.hxc` | AST tipado serializado (M4) |
| `.hxv` | build portable (M4) |
| `.hxa` | binario nativo final (M4) |

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

## 13. Lo que este documento *no* cubre todavía

`COMPTIME`, restricciones `DONDE T: Trait` sobre funciones genéricas, funciones
anónimas, `.hxk`/`.hxq`, capacidades (`std.net`, `audio`, `gpu`).