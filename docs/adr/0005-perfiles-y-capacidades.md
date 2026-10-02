# ADR 0005 — Un lenguaje, N perfiles

Estado: aceptada para M4/M7.

## Decisión

Una única gramática. Un **perfil** es un objeto de datos del compilador
(`allowed_types`, `available_decls`, `allowed_ops`, `needs_panics`) que el
verificador recibe como parámetro. `core`, `game`, `audio_rt` y `gpu` son
perfiles.

`audio_rt` y `gpu` prohíben reservas, `STRING` como retorno, `Result` en el
callback, `+`/`-` verificados (obligatorio `+%`), división por variable, y
cualquier llamada que no esté marcada `PURE`.

`SHADER` no es un noveno lenguaje: es un perfil (`PROFILE GPU`) del mismo AST.

## Por qué

La alternativa —tratar GPU/audio como un sublenguaje con su propia
gramática— duplica el compilador y hace que "un solo lenguaje" sea falso.
Con perfiles, la unificación es verificable: el mismo `FOR`, el mismo `IF`, los
mismos tipos vectoriales, y sólo cambia lo que el perfil permite.

## Auditoría

El principio 9 se comprueba, no se promete: `hx size --caps <binario>` imprime
la tabla de símbolos importados del ELF y falla si aparece algo fuera del
conjunto de capacidades solicitado.
