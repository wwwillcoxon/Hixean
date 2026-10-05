# ADR 0013: publicar Hixean como lenguaje

## Estado

aceptado (0.1.0)

Revisado 2026-10-05: `hxc fmt` sale de la lista de «lo invisible», porque no existe
y estaba escrito en pasado como una de las cosas que el proyecto ya tenía.

## Contexto

Hixean llevaba doce hitos como proyecto de una sola persona: funcional,
medido y documentado, pero con la forma de un Beware. Para quien llega por
primera vez, un repositorio sin licencia, sin changelog y con la versión
`0.1.0-dev` en el binario transmite tres cosas: que no se puede depender de
él, que nadie sabe qué cambió, y que la versión no significa nada.

Al mismo tiempo, el lenguaje ya tiene lo que hace falta para ser real: una
gramática completa, diagnósticos con códigos estables, un compilador que
compila su propia salida, un corpus con salidas esperadas, paquetes,
capacidades y una puerta de tamaño que nadie puede subir sin querer.

Lo que faltaba no era funcionalidad, sino las dos cosas que hacen que un
lenguaje sea un contrato: decir qué se promete y comprobar que se cumple.

## Decisión

**Una sola versión para el lenguaje y el compilador.** `hxc version` la
imprime y `hxc version --json` la expone para las herramientas.
Semántico: `0.1.0` hoy; dentro de `0.x`, un cambio incompatible (una
palabra clave nueva que rompa un programa, un perfil que cambie de
comportamiento, un formato que ya no se lea) sube a `0.2.0` y no a `1.0.0`.
`1.0.0` significa que el lenguaje se congela y solo se amplía.

**Los códigos de diagnóstico son parte de la API.** `E0305` significará
siempre lo mismo. Un código nuevo es un número nuevo; un código retirado
deja de emitirse tras dos versiones menores, con nota de diagnóstico
primero. Una herramienta puede filtrar por código sin leer el mensaje.

**El `.hxc` tiene formato y abi.** Ya lo comprueba (E0603): una unidad de una
versión distinta se rechaza con un mensaje que dice qué formato y qué abi
usa este `hxc`. Un paquete publicado lleva su `TARGET hixe >= 0.1`, que es
la puerta que el usuario ve antes de que nada compilado falle.

**Lo que no se promete se dice en el changelog.** `audio`, `gpu`, `.hxv`,
`.hxa` y el auto-hospedaje aparecen en "Notas" como lo que no existe. Un
manual que omite los límites se lee como un manual de un lenguaje que sí los
tiene.

### Lo invisible, que es lo que de verdad sostiene lo anterior

Un lenguaje se perdona un tutorial malo; no se perdona un compilador que
pierde datos. Estas cuatro cosas no se ven en una demo y son las que
convierten "proyecto" en "lenguaje":

1. **`-Werror` en el build.** Los avisos que quedan hoy (dos, en
   `common.h` y `main.c`) son la letra pequeña de esto: mientras el build
   los tolere, cada commit añade ruido y el ojo deja de verlos. Con
   `-Werror` no hay ruido.
2. **El corpus entero bajo `-fsanitize=address,undefined`.** El compilador
   manipula el AST del programa que está compilando; un error de memoria
   ahí no se manifiesta como un error de memoria, sino como un diagnóstico
   raro en el código de otra persona. Sanitizar el propio compilador es la
   única forma de que eso no llegue a producción.
3. **Fuzzing del front-end con entradas rotas.** El parser promete
   recuperación de errores (ADR 0002). Una promesa así solo se puede
   sostener con entradas que nadie eligió: truncar el corpus, voltear bytes,
   quitar comillas. El criterio no es "compila": es que nunca muere con una
   señal y siempre da un diagnóstico.
4. **`hxc check --json`.** El editor, el LSP y el CI necesitan los
   diagnósticos en una forma que una máquina pueda leer. Imprimirlos para
   humanos y ya está el trabajo: el formato es el contrato.

La quinta que aquí había, **`hxc fmt`**, sale de la lista y pasa a lo que
falta: el comando no existe. Estar en una lista de «estas cosas que
convierten proyecto en lenguaje» escrita en pasado es una promesa, y una
promesa que no está escrita en ninguna parte que la compruebe es la forma
más fácil de mentir sin querer. Formatear es la mitad de la experiencia de
un lenguaje en un editor y no depende de ningún protocolo ni servidor, así
que sigue siendo lo que más falta por el mismo precio: lo que falta es
escribirlo, y decidir cómo se decide el sangrado.

## Alternativas

- **Esperar a 1.0.0 para publicar.** 1.0 significa "congelado". Publicar
  ahora, con criterios explícitos sobre qué es estable, da más información
  que un 1.0 con doce hitos de alcance.
- **Versionar el lenguaje y el compilador por separado.** Sería más
  honesto en un proyecto con varios front-ends. Con uno solo obliga a
  mantener una tabla de correspondencias que nadie consulta.
- **Sólo con `-Werror` y sanitizers, sin fuzzing.** Los sanitizers
  encuentran errores de memoria en lo que se ejecuta; el fuzzing encuentra
  los errores de lógica del parser en lo que no se ejecuta. Son
  complements, no sustitutos.
- **Escribir el LSP antes que el JSON.** El LSP es la respuesta correcta a
  la pregunta "¿cómo se edita Hixean?", y es un protocolo con ciclo de
  vida, versiones y casos que esta herramienta no tiene todavía. Con
  `--json` se resuelve el 80% (diagnósticos en el panel) con el 20% del
  trabajo, y el LSP de verdad se puede escribir después sobre el mismo
  contrato.

## Consecuencias

- `LICENSE` (MIT), `CHANGELOG.md` y `docs/manual.html` entran en el
  repositorio; `hxc version` sale de la rama de desarrollo.
- La CI deja de ser "compila en tres sistemas" y pasa a compilar con
  `-Werror`, ejecutar el corpus bajo sanitizers y pasar el fuzzer. Los tres
  pueden fallar una CI que antes pasaba.
- `hxc check --json` es un formato que hay que mantener: entra en el mismo
  compromiso que los códigos de diagnóstico.
- El auto-hospedaje (compilar `hxc` con `hxc`) sigue siendo un objetivo
  posterior, y el changelog lo dice.
- Cualquiera puede publicar un paquete `.hxk` sin pedir permiso: la
  licencia es la puerta, no el registro.