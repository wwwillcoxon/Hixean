# ADR 0002 — Qué significa «compilar en menos de 200 ms»

Estado: aceptada. Muros: esta decisión condiciona el diseño del emisor.

## Mediciones (GCC 13.3, x86_64, Ubuntu 24.04)

C generado con la forma que produce la monomorfización (N funciones vivas,
con llamadas cruzadas):

| linkage | N | `cc -O2 -S` |
|---|---|---|
| externo | 1000 | 2.70 s |
| externo | 4000 | 10.74 s |
| `static` | 2000 | 1.70 s |

`cc -fsyntax-only` sobre las mismas 28 004 líneas: **158 ms**. Es decir, el
*frontend* de cc ya consume el 75 % del presupuesto de 200 ms; el resto es IPA.

Y por función: ≈ 2.7 ms con linkage externo, ≈ 0.6 ms con `static`.

## Decisión

1. La puerta de 200 ms mide **front-end + emisión a C**, es decir
   `hx build --timing` hasta el archivo `.c` escrito. El enlace es una fase
   separada, incremental y cacheada.
2. **Toda función interna se emite `static`.** El linkage externo cuesta 3–4×
   en tiempo de `cc` y no aporta nada a un programa enlazado.
3. Una unidad de traducción por módulo, no por función.
4. La puerta end-to-end se publica aparte y no se promete: hoy, 10 012 líneas
   dan 50 ms de front-end y 866 ms de `cc -O2`.

## Consecuencias

- LaGate es alcanzable y medible en CI en cualquier máquina.
- El presupuesto de 200 ms es una restricción de **diseño de emisión**, no del
  analizador: obliga a `static` interno y a particionar por módulo.
- Cuando 866 ms deje de ser aceptable, el siguiente paso es un backend en
  proceso (Cranelift) como camino «rápido», con C como camino «release».
  El punto de inserción es el mismo: `hx_emit_unit`.
