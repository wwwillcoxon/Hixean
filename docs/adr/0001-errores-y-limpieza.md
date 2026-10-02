# ADR 0001 — Errores, limpieza y llamadas de cola

Estado: aceptada. Motivación: el principio «runtime núcleo ≈ 2 KB» y
«tail calls garantizados» son incompatibles con *unwinding*.

## Contexto

`?` debe propagar errores ejecutando los `DEFER` y liberando la `ARENA`
del scope por el que sale. Si eso se implementa con *unwinding* hace falta
una tabla de personnelidades (`__cxa_*`), +8–20 KB de runtime y una
dependencia de la biblioteca de C — las dos cosas prohibidas.

## Decisión

1. **Sin *unwinding*.** `?` es una comprobación + salto a un destino de error
   explícito. `panic` es `abort` (salida 70) y nunca despliega la pila.
2. **`DEFER` es epílogo generado**, no una pila en tiempo de ejecución.
3. **`ARENA` es epílogo con `free`.**
4. **TCO verificable**: una auto-llamada en posición de cola con argumentos
   posicionales se convierte en un bucle en HIR:

   ```
   FUNCTION f(n)
     IF n <= 0 THEN RETURN base
     RETURN f(n - 1)
   ```
   →
   ```c
   static int f(int n) { for (;;) { if (n <= 0) return base; n = n - 1; } }
   ```

   Es una garantía que el compilador posee, no una esperanza sobre `cc`.
   La recursión mutua en posición de cola **no** está garantizada: requiere
   `musttail`, que sólo existe en Clang; con GCC es mejor-esfuerzo.
   Los parámetros `REF` inhibits la transformación (comparten almacenamiento).

## Consecuencias

- El runtime no necesita tabla de personalidades ni soporte de excepciones.
- Los iteradores lazy pueden fundirse a un bucle sinالسحب de pila.
- Una recursión profunda mal escrita desborda la pila de forma normal: es
  un error del programador, no una trampa silenciosa.

## Alternativas descartadas

- *Unwinding* estilo C++/Rust: +8–20 KB, dependencia de libc, imposible con
  el backend C.
- `setjmp`/`longjmp`: no portable a MSVC ni a GPU, y `longjmp` no funde bucles.
