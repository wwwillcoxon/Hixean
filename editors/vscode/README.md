# Extensión de Hixean para VS Code

Resaltado, plantillas, compilación y **los diagnósticos reales de `hxc`** en el
panel de problemas. La extensión no reimplementa el lenguaje: lee
`hxc check --json` (ver ADR 0013) y lo convierte en `Diagnostic` de VS Code, de
modo que el mensaje, el código `E0301` y la posición son los del compilador, no
una aproximación del analizador de texto.

## Qué trae

| archivo | para qué |
|---|---|
| `package.json` | lenguajes (`.hxe` `.hxs` `.hxf` `.hxt` `.hxk` `.hxq`), gramáticas, plantillas, comandos y ajustes |
| `syntaxes/*.tmLanguage.json` | resaltado de Hixean, de manifiestos `.hxk` y de consultas `.hxq` |
| `snippets/hixean.json` | `fun`, `funx`, `fung`, `tipo`, `enum`, `trait`, `impl`, `res`, `arena`, `defer`, `match`, `forin`, `mod`, `prueba`, `enable` |
| `extension.js` | diagnósticos al guardar, comandos `hixean.build`, `hixean.test`, `hixean.checkActive` |
| `test/smoke.js` | prueba con un `vscode` falso: el panel de problemas se comprueba sin abrir VS Code |

## Ajustes

| ajuste | por defecto | para qué |
|---|---|---|
| `hixean.path` | `hxc` | dónde está el compilador, si no está en el `PATH` |
| `hixean.profile` | `freestanding` | perfil con el que se compila |
| `hixean.diagnostics` | `true` | comprobar al guardar |
| `hixean.checkDelay` | `400` | milisegundos de espera tras guardar |

## Probarlo sin publicarlo

```
cd editors/vscode
npm install -g @vscode/vsce
code --extensionDevelopmentPath="$PWD"
```

Hace falta `hxc` en el `PATH` (o `hixean.path` apuntando a `build/hxc`) para que
el panel de problemas funcione; sin él, el resaltado y las plantillas siguen
sirviendo.

## Publicarlo

El Marketplace pide una cuenta de Azure DevOps y un *personal access token* de
esa cuenta; sin eso no hay publicación automática. Con el token:

```
cd editors/vscode
npx @vscode/vsce publish --pat "$VSCE_PAT"
```

Mientras tanto, `npx @vscode/vsce package` produce un `.vsix` que se puede
instalar con **Extensions: Install from VSIX…**. El campo `publisher` de
`package.json` (`hixean`) tiene que existir en la organización de destino o
`vsce` lo rechaza.

## La prueba de humo

```
make && node editors/vscode/test/smoke.js
```

Escribe un `.hxe` con dos errores, llama a lo mismo que llama el editor al
guardar y comprueba que los diagnósticos que salen tienen la línea, la columna,
el código y la nota correctos. Se ejecuta también desde `make test`. En su
desarrollo ya encontró dos fallos reales: las funciones se exportaban con el
nombre equivocado (`activar` en vez de `activate`, que es lo que exige la API) y
las tareas se creaban sin `new`, que es una clase.

## Qué le falta

- **LSP de verdad.** Hoy los diagnósticos llegan al guardar, no mientras se
  escribe. El salto es un `hxc lsp --stdio`, y el formato JSON ya está para que
  ese servidor no tenga que inventar nada.
- **`hxc fmt`.** Un comando de formateo del compilador con la acción de
  "formatear al guardar" sería el resto de la mitad invisible de la experiencia.
- **Plantillas para `.hxk` y `.hxq`**: hoy solo hay snippets del lenguaje.