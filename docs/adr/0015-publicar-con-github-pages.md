# ADR 0015: publicar el sitio con GitHub Pages

## Estado

aceptado (0.1.0)

## Contexto

El proyecto tiene tres documentos que se leen en un navegador: la página
pública (`site/`), el manual interactivo (`docs/manual.html`) y la gramática
(`docs/grammar.md`, que se lee mejor en crudo). Los tres están escritos con
rutas relativas y sin recursos remotos, así que ninguno necesita construirse
antes de servirse.

La pregunta es dónde servirlos. GitHub Pages es la opción obvia para un
proyecto que ya vive en GitHub, y tiene dos modalities que no son la misma
cosa:

- **despliegado por rama**: GitHub construye el directorio elegido de `main`;
- **despliegado por workflow**: el workflow compone un artefacto y lo publica.

La primera solo ofrece dos sitios: la raíz del repositorio o una carpeta
llamada `docs`. En la raíz no hay ningún `index.html` (el proyecto es un
repositorio de código, no un sitio), y en `docs/` está el manual, no la página.

## Decisión

**Workflow.** `site/` va a la raíz del artefacto y `docs/` al lado, de forma que

```
https://wwwillcoxon.github.io/Hixean/                    ->  la página
https://wwwillcoxon.github.io/Hixean/docs/manual.html    ->  el manual
```

resuelven sin configuración de subruta. El enlace `../docs/manual.html` de la
página apunta justo a la segunda URL, y funciona igual servido desde
`https://wwwillcoxon.github.io/Hixean/` que abierto con `file://` desde el
repositorio clonado.

**El workflow se dispara solo cuando cambian `site/` o `docs/`.** Un push al
front-end no tiene por qué redesplegar el sitio, y un sitio que se redespliega
siempre acaba pidiendo un PR para un cambio de README que no lo toca.

**`.nojekyll` en el artefacto.** Jekyll ignora ficheros y carpetas que empiezan
por `_`; sin el `.nojekyll`, cualquier archivo así desaparece en silencio. Hoy no
hay ninguno, y esa es exactamente la razón para ponerlo ahora: que no se
pierda nada el día que sí lo haya.

**Un `404.html` propio** que devuelve a la página en vez de la pantalla de error
de GitHub. Es el mismo esqueleto, sin JavaScript, en la línea de los otros
documentos.

**Sin CNAME todavía.** Un dominio propio se añade con un `CNAME` dentro del
artefacto y un `CNAME` en el DNS apuntando a `<usuario>.github.io`. Decidirlo
antes de tener contenido es cómo se acaba con un dominio que hay que devolver.

## Alternativas

- **`docs/` como raíz de Pages**, sin workflow.** Lo más simple de configurar,
  pero publica el manual como página principal y obliga a reorganizar el
  repositorio para que la página quede dentro de `docs/`.
- **Mover la página a la raíz del repositorio** (`index.html`, `style.css`,
  `script.js` arriba). Funciona con "deploy from a branch" sin más, y mezcla
  el sitio con el proyecto: un `style.css` en la raíz del repositorio de un
  compilador es una decisión que hay que explicar.
- **GitHub Pages para el manual y otra cosa para la página.** Dos dominios o
  dos repositorios para un proyecto de una persona.
- **Generar la web con Sphinx o MkDocs.** Son herramientas que hay que
  mantener, y para dos documentos que ya están escritos a mano y funcionan sin
  nada externo. El día que haya que generar referencias cruzadas o varios
  idiomas, la discusión se repite.

## Consecuencias

- El workflow `pages.yml` es el que decide qué se sirve; si se añade un
  documento nuevo que deba salir en la web, hay que copiarlo a `_site` en el
  paso de composición (hoy copia `manual.html` y `grammar.md`).
- La página y el manual comparten la política de "los ejemplos se ejecutan":
  `tools/verificar-ejemplos.py` los comprueba a los dos en cada `make test`.
- Publicar no depende de que el repositorio tenga una build: si el artefacto
  está mal, se ve en el pull request antes de llegar a `main`, porque el
  workflow corre en cada push a `main`.
- Si algún día hay un dominio propio, el cambio es un `CNAME` en el artefacto
  y una entrada en el DNS; nada del contenido cambia.
- El manual sigue funcionando como fichero local, que es lo que quiere quien
  lee el repositorio con `git clone` y sin red.