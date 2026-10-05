# Metadatos del repositorio: pendientes por falta de permiso

El sitio, el manual y la guía están completos, pero el repositorio en GitHub sigue
sin descripción, sin homepage y sin topics. Es la última parte del trabajo de SEO y
no se puede hacer desde aquí.

## Por qué

El `GITHUB_TOKEN` de este entorno es un token de instalación de GitHub App
(`ghu_…`), y esa instalación no tiene permiso de administración sobre el
repositorio. Cualquier escritura da:

```
$ gh api -X PATCH repos/wwwillcoxon/Hixean -f description=prueba
gh: Resource not accessible by integration (HTTP 403)
```

No es un problema de permisos de la CLI ni de `gh` sin iniciar sesión:
`GET repos/…` sí responde, y el 403 es solo al escribir. Es el token.

## Lo que hay que poner

Tres campos, en <https://github.com/wwwillcoxon/Hixean/settings>, o con un token
tuyo que tenga `repo`:

**Descripción** (la línea que sale bajo el nombre, 100 caracteres máximo):

```
Lenguaje AOT que compila a C11. Hola mundo ocupa 8 896 bytes. Evolución moderna de QBasic.
```

**Website**:

```
https://wwwillcoxon.github.io/Hixean/
```

**Topics** (marcar los que apliquen; cada topic añade una entrada en la búsqueda de
GitHub y una línea en la página del repo):

```
language
compiler
programming-language
c
d language
embedded
systems-programming
interpreter
haxe
```

## Por qué estos topics y no otros

`haxe` está porque Haxean se lee parecido y quien busca Haxe cae aquí; parece una
broma hasta que alguien llega desde ahí y ve que el lenguaje existe. No es el
principal.

Los que de verdad filtran son `compiler`, `c`, `embedded` y `systems-programming`:
juntan a quien busca un lenguaje que compila a C y a quien necesita un binario de
9 KiB. `d language` recoge el intento de parecer familiar; `embedded` y
`systems-programming` son los que describen el proyecto de verdad, con la puerta de
12 KiB y el perfil `freestanding` sin libc.

## Qué se comprueba de esto

Nada, automáticamente. `tools/verificar-cifras.py` mira las cifras del sitio, que no
incluyen los topics del repo porque no están en ningún fichero del repositorio: es
lo único del proyecto que vive fuera y que ninguna puerta puede leer.