# ADR 0004 — Aliasing: unicidad y procedencia, no *borrow checker*

Estado: aceptada. Implementada en M4, M5c y M18.
Revisado 2026-10-05: aquí decía «aceptada para M4 (no implementado aún)». Ya
está.

## Problema

`ARENA` + `REF` + `PTR` + sin GC + sin movimientos = C con esteroides. Si no
se dice nada, los dobles `free` y el *aliasing* mutable caen en el usuario, y
entonces la ventaja competitiva del lenguaje (mensajes de error claros) se
pierde justo en los bugs más caros.

## Decisión

1. Los valores son **inmutables por defecto**; `DIM x` produce una copia.
2. Para mutar hay que pasar por `REF` sobre un origen declarado `UNIQUE`, y el
   verificador comprueba **unicidad de procedencia**: como mucho un `REF` vivo
   por origen, y ninguna dos rutas mutables a la misma celda.
3. `PTR` es aritmética cruda y explícita; el diagnóstico la marca como tal.
4. En `ARENA`, los bindings nacen con procedencia de arena y no pueden
  devolverse, guardarse en un `TYPE` más longevo ni capturarse en una closure
   que escape del bloque. El análisis es un barrido lineal sobre el HIR (que ya
   tiene los puntos de terminación materializados), no un grafo de *__borrows__.

## Alternativas

- *Borrow checker* completo estilo Rust: contradice el principio 8 y multiplica
  el coste de los diagnósticos.
- C crudo: coherente pero pierde el producto.
