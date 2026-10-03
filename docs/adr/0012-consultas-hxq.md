# ADR 0012: consultas `.hxq`

## Estado

aceptado (M12)

## Contexto

M9 introdujo los paquetes `.hxk`: un manifiesto dice qué ofrece un paquete, qué
necesita y con qué perfil se construye. Desde entonces hay dos formas de
responder "¿qué me sirve de lo que hay instalado?":

1. un gestor de dependencias externo que lee los manifiestos, o
2. el propio compilador.

La primera es la habitual (Cargo, npm, apt), pero obliga a duplicar el
formato: el gestor necesita su propio parser de `.hxk`, y ese parser queda
desincronizado del compilador en cuanto cambia una instrucción. La segunda es
más coherente, pero `hxc kit` resuelve un manifiesto concreto: sirve para
responder "¿este paquete se puede construir aquí?", no "¿qué paquetes de este
directorio me sirven?".

En la práctica faltaba la pregunta de descubrimiento: elegir entre muchos
paquetes sin compilar ninguno.

## Decisión

**Un `.hxq` es un archivo de consulta, no un programa Hixean.** Se parece a los
manifiestos (léxico de una palabra por instrucción, `QUERY ... END QUERY`) y
deliberadamente no es Hixean: es entrada de una herramienta, y hacerlo lenguaje
obligaría a arrastrar el verificador y el runtime a un programa que no genera
código.

**Los predicados son los del manifiesto.** `PROVIDES`, `FEATURE`, `CAPABILITY` y
`DEP` ya describían lo que un paquete da y lo que necesita; una consulta los
combina con Y y añade `VERSION` con comparación. No hay predicados nuevos: si
un `.hxk` no se puede describir con ellos, la información no está en el
manifiesto y no se inventa en la consulta.

**La consulta no compila nada y no informa de errores ajenos.** Un manifiesto
roto se salta en silencio; `hxc kit` es quien debediagnosticarlo. Así un
`hxc query` sobre un directorio con basura no falla por culpa de la basura, y
el código de salida depende sólo de si la consulta está bien escrita.

**La salida es texto estable.** `nombre version  ruta`, en orden alfabético por
ruta, porque el destino principal es alguien leyendo el terminal y un
script comparando con un fichero de referencia. Por eso el listado de
directorios se ordena: `readdir` y `FindFirstFileA` no prometen orden, y una
búsqueda de paquetes reproducible vale más que ahorrar unas comparaciones.

**El listado de directorios tiene dos implementaciones.** `dirent.h` en POSIX y
`FindFirstFileA` en Windows, con el mismo resultado. La alternativa —usar
`dirent.h` en todas partes— funciona con MinGW pero ata el compilador a un
toolchain concreto, y la matriz de CI ya incluye Windows.

## Alternativas

- **Extender `hxc kit` con flags (`--provides`, `--feature`).** Más rápido de
  escribir, pero la combinación de filtros crece con cada caso y acaba siendo
  un lenguaje de argumentos. El `.hxq` es un archivo: se versiona, se guarda en
  el repositorio y se lee sin ejecutar nada.
- **SQL sobre los manifiestos.** Potente y estándar, pero obliga a tener una base
  de datos (o un motor completo) para algo que son cuarenta líneas de lectura de
  texto en un directorio.
- **Hacer la consulta un programa Hixean (`FOR p IN kits(...)`).** Da tipado y
  control de flujo al precio de compilar para responder una pregunta que no
  genera código. Queda como posibilidad futura si las consultas necesitan lógica
  (elegir la versión más alta, priorizar por capacidad), no como forma inicial.

## Consecuencias

- `hxc query` es una subcomando más, con su propio parser (~90 líneas en
  `src/kit.c`) y cuatro códigos de diagnóstico nuevos (`E0811`–`E0814`).
- La suite cubre cuatro consultas con salida esperada (`tests/queries/*.hxq`) y
  cuatro consultas mal formadas que deben fallar.
- Las consultas no entienden de `DEFINE`, `OPT` ni assets: son sobre lo que un
  paquete *es*, no sobre cómo se compila. Si hace falta, se añade un predicado
  nuevo al manifiesto primero.
- `audio` y `gpu` quedan fuera de M12 a propósito: declarar una capacidad que
  no abre nada pondría `CAPABILITY` en el sitio de la promesa vacía, que es
  justo lo que el ADR 0011 intenta evitar.
