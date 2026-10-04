// Prueba de humo de la extensión sin VS Code.
//
// La extensión no se puede probar en un navegador ni sin el editor, pero su
// parte interesante -- leer hxc check --json y convertirlo en diagnósticos --
// sí se puede ejercitar con un `vscode` falso. Esto es lo que evita que un
// cambio en el formato JSON rompa el panel de problemas sin que nadie lo note.
//
//   node editors/vscode/test/smoke.js

const Module = require("module");
const path = require("path");
const assert = require("assert");
const fs = require("fs");

const RAIZ = path.resolve(__dirname, "..", "..", "..");
// En Windows el enlazador anade .exe, igual que en hxc size. El binario se
// busca por los dos nombres y el error dice los dos, para que no parezca que
// falta la compilacion cuando lo que falta es la extension.
const HXC = ["hxc", "hxc.exe"].map((n) => path.join(RAIZ, "build", n)).find((p) => fs.existsSync(p))
  || path.join(RAIZ, "build", "hxc");

const puestos = [];
const tareas = [];
let guardarCallback = null;
const comandos = {};

class Position {
  constructor(line, character) {
    this.line = line;
    this.character = character;
  }
}

const vscodeFalso = {
  Position,
  Range: class {
    constructor(start, end) {
      this.start = start;
      this.end = end;
    }
  },
  Diagnostic: class {
    constructor(range, message, severity) {
      this.range = range;
      this.message = message;
      this.severity = severity;
    }
  },
  DiagnosticSeverity: { Error: 0, Warning: 1, Information: 2, Hint: 3 },
  languages: {
    createDiagnosticCollection() {
      return {
        set(uri, lista) {
          puestos.push({ uri: uri.fsPath, lista });
        },
        dispose() {},
      };
    },
  },
  workspace: {
    getConfiguration() {
      return {
        get: (clave, por_defecto) =>
          clave === "path" ? path.join(RAIZ, "build", "hxc") : por_defecto,
      };
    },
    onDidSaveTextDocument(cb) {
      guardarCallback = cb;
      return { dispose() {} };
    },
    getWorkspaceFolder() {
      return null;
    },
    workspaceFolders: [],
    textDocuments: [],
  },
  window: {
    activeTextEditor: null,
    setStatusBarMessage() {},
    showErrorMessage() {},
  },
  commands: {
    registerCommand(nombre, cb) {
      comandos[nombre] = cb;
      return { dispose() {} };
    },
  },
  tasks: {
    executeTask(t) {
      tareas.push(t);
      return Promise.resolve();
    },
  },
  Task: class {
    constructor(definition, _scope, _source, execution) {
      this.definition = definition;
      this.execution = execution;
    }
  },
  TaskScope: { Workspace: 1 },
  ProcessExecution: class {
    constructor(cmd, args, cwd) {
      this.process = { cmd, args, cwd };
    }
  },
};

const original = Module._load;
Module._load = function (request, parent, isMain) {
  if (request === "vscode") return vscodeFalso;
  return original.apply(this, arguments);
};

const { activate } = require(path.join(RAIZ, "editors", "vscode", "extension.js"));

const fuente = "DIM x AS INT = \"hola\"\nDIM y AS INT = noexiste\n";
const rutaFalsa = path.join(RAIZ, "build", "fumo.hxe");
fs.mkdirSync(path.join(RAIZ, "build"), { recursive: true });
fs.writeFileSync(rutaFalsa, fuente);

const uri = { fsPath: rutaFalsa, toString: () => "file://" + rutaFalsa };
const documento = { uri, languageId: "hixean" };
vscodeFalso.workspace.textDocuments = [documento];
vscodeFalso.window.activeTextEditor = { document: documento };

const ctx = { subscriptions: [] };
activate(ctx);

assert.ok(guardarCallback, "la extensión debe suscribirse a onDidSaveTextDocument");
assert.ok(comandos["hixean.build"], "debe registrar el comando hixean.build");
assert.ok(comandos["hixean.test"], "debe registrar el comando hixean.test");

(async () => {
  assert.ok(fs.existsSync(HXC), `hxc tiene que estar compilado (make); no hay ni ${HXC} ni build/hxc.exe`);

  const p = new Promise((r) => setTimeout(r, 1200));
  guardarCallback(documento);
  await p;

  assert.ok(puestos.length > 0, "no se publicó ningún diagnóstico");
  const lista = puestos[puestos.length - 1].lista;
  assert.ok(lista.length >= 2, `esperaba 2 diagnósticos, hubo ${lista.length}`);

  const primero = lista[0];
  assert.match(primero.message, /se esperaba INT/);
  assert.strictEqual(primero.code, "E0301");
  assert.strictEqual(primero.range.start.line, 0, "la línea 1 del archivo es la 0 en VS Code");
  assert.strictEqual(primero.range.start.character, 15, "columna 16 del archivo → 15");
  assert.match(primero.source, /hxc E0301/);
  assert.ok(lista[1].message.includes("—"), "la nota del diagnóstico debe verse en el mensaje");

  comandos["hixean.build"]().then(() => {
    assert.ok(tareas.length > 0, "hixean.build debe lanzar una tarea");
    assert.ok(tareas[0].execution.process.args.includes("build"));
    console.log("ok     la extensión convierte hxc check --json en diagnósticos del panel");
  });
})().catch((e) => {
  console.error("FALLO:", e.message);
  process.exit(1);
});