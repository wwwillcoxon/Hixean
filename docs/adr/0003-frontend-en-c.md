# ADR 0003 — El compilador se escribe en C11

Estado: aceptada. (Esta decisión contradice la recomendación inicial de Rust;
el registro del cambio es deliberado.)

## Contexto

El objetivo declarado incluye **bootstrapping** y el entorno sólo trae `cc`.

## Decisión

El front-end está en C11 sin dependencias externas: arena bump, tabla de
símbolos interns, diagnósticos, lexer, parser, verificador y emisor. Un único
binario `build/hxc` que compila con `cc -O2`.

## Motivos

1. **Bootstrapping**: escribir el compilador en el lenguaje que compila es el
   objetivo de la fase 5; hacerlo en C lo hace alcanzable y no河流域 agrega
   una condición extra.
2. **Cero dependencias**: `cc` es el único toolchain necesario. `rustup` no
   estaba instalado en el entorno y el objetivo del proyecto es no arrastrar
   toolchains.
3. **Mismo lenguaje que el backend**: el compilador sabe exactamente qué C
   produce, con qué layouts y qué ABI; no hay surprises de layout.
4. El coste — ergonomía y seguridad — se paga con disciplina explícita:
   toda la memoria del compilador sale de una arena, no hay estado global, y
   las interfaces entre fases son structs opacos.

## Consecuencias

- `COMPTIME` (que necesita reentrar al front-end durante la compilación) es una
  llamada a funciones, no una vuelta al lenguaje anfitrión.
- El formateador y el LSP se apoyan en el mismo AST desde el primer día
  (`hx-parse` produce el árbol; `pretty` lo recorre).
- No hay `Rc`/`String`: `HxStr` es `(ptr,len)` y las tablas son arenas.
