# ADR 0014: distribuir el compilador y los paquetes

## Estado

aceptado (0.1.0)

## Contexto

Un lenguaje sin canal de distribución es un repositorio: se clona, se compila a
mano y se depende de que quien llega tenga un compilador de C11. Eso vale para
quien lee el README y nada más.

Hixean tiene dos cosas que distribuir, y son de naturaleza distinta:

1. **el compilador**, que es un programa que se instala y se actualiza;
2. **los paquetes**, que son código de otra gente y se *consultan*, no se
   instalan en el mismo sentido.

La segunda es la que hace que un lenguaje sea un lenguaje. Cargo, npm y apt no
son programas que descargan paquetes; son índices con nombres, versiones y
dependencias. Hixean ya tenía la forma de esa pregunta (`hxc query`, ADR 0012)
pero no tenía dónde mirar.

## Decisión

### El compilador: la release de GitHub es la fuente de verdad

Una etiqueta `v0.1.0` dispara un workflow que compila y **pasa la suite** en las
seis combinaciones de sistema y arquitectura, empaqueta cada una con
`make dist` y calcula `SHA256SUMS` sobre los artefactos que subió cada runner.
Los canales de arriba —instalador, Homebrew, winget— solo saben leer de ahí.

El checksum va en un archivo aparte, no en la firma de la release, porque
`install.sh` tiene que poder compararlo **antes** de extraer nada: si los bytes
no son los del archivo publicado, no se ejecuta, no se borra y no se avisa de
que ya había un hxc instalado. Verificado contra un servidor local: con el
tarball intacto instala, con un byte cambiado aborta.

Los `sha256` de la fórmula de Homebrew y del manifiesto de winget se dejan
**marcados** (`REEMPLAZAR_CON_EL_SHA256…`) a propósito. Copiarlos de la release
es trabajo de una persona; escribirlos de memoria es una forma de publicar mal
sin darse cuenta, y un checksum falso en un gestor de paquetes es peor que no
tener checksum.

### Los paquetes: el registro es un directorio

`hxc pack <kit.hxk> --out DIR` copia el manifiesto y **los módulos que ese
paquete importa** al layout `<DIR>/<nombre>/<nombre>.hxk` más sus fuentes. No
copia el directorio del manifiesto, que puede contener media región y otros
paquetes, y no inventa un formato nuevo: es exactamente el layout que ya
entendían `hx_kit_resolve` y `hx_query_visit`.

`hxc install <nombre> --registry DIR --into DIR` copia desde ahí y dice el
`--path` que hay que añadir. Por defecto `~/.hixean/registro` y
`~/.hixean/paquetes`.

Que un registro sea un directorio y no un servidor no es una limitación
provisional de escribirlo rápido: es lo que permite que el mismo registro sea

- un repositorio git clonado, con historial y auditoría;
- un directorio local, para probar y para proyectos cerrados;
- un servidor estático detrás de HTTPS, sin que cambie el compilador.

Un servidor de verdad (API, cuentas, tokens, semver completo) no está excluido:
encaja detrás de esta misma forma cuando haga falta. Lo que sí exige es una
decisión que no se puede deshacer fácil — quién publica, con qué credenciales,
cómo se retira un paquete malicioso— y hoy no hay nadie más que el autor.

## Alternativas

- **Instalar con `curl | sh` sin checksum.** Es lo que hace la mitad de internet
  y es la razón por la que el checksum va en un archivo aparte y se verifica
  antes de extraer.
- **Subir el binario a un gestor de paquetes (Homebrew y winget) sin releases.**
  Rompe en la primera versión que no compila para una plataforma, y ninguna de
  las dos herramientas permite volver atrás con una verificación de integridad.
- **Registro con servidor y base de datos.** Es el diseño correcto para un
  lenguaje con cien autores. Con uno, es infraestructura que hay que mantener y
  que nadie audita.
- **Un `.hxq` que consulte un índice remoto.** Necesita HTTPS dentro del
  compilador, es decir, TLS y una política de rotación de certificados en un
  programa que hoy habla con el kernel y nada más. El camino corto es clonar
  con git y dejar que git se encargue.
- **Permitir sobrescribir al publicar.** Publicar sobre un paquete existente es
  la forma más fácil de que un cambio local accidental se convierta en un
  release. `E0815` obliga a elegir una versión nueva.

## Consecuencias

- `make dist` lee la versión de `src/common.c`, así que el nombre del artefacto no
  puede desincronizarse de `hxc version`.
- El registro necesita una convención, no un protocolo: un `README.md` con la
  regla de "un directorio por paquete, el manifiesto con el nombre del paquete"
  y ya.
- Publicar es `hxc pack` y subir el directorio resultante al repositorio del
  registro. Instalar es clonar ese repositorio una vez y pasar `--path`.
- **No hay firma.** El checksum protege del transporte corrupto o manipulado en
  tránsito; no protege de que alguien con acceso al repositorio suba un paquete
  con código malicioso. Eso es una decisión de futuro, no un olvido.
- `E0815`–`E0819` pasan a ser parte de la API de diagnósticos (ADR 0013), con la
  misma lectura: un código significa siempre lo mismo.