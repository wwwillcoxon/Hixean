// Extensión de Hixean para VS Code.
//
// No reimplementa nada del lenguaje: pide a hxc los diagnósticos en JSON
// (hxc check --json, ADR 0013) y los enseña en el panel de problemas. Si hxc
// no está en el PATH, la extensión deja de avisar pero el resaltado sigue
// funcionando, porque el resaltado no depende del compilador.

const vscode = require("vscode");
const { execFile } = require("child_process");

let timer = null;
const collector = vscode.languages.createDiagnosticCollection("hixean");

function config() {
  return vscode.workspace.getConfiguration("hixean");
}

function hxcPath() {
  return config().get("path", "hxc");
}

function diagnosticSeverity(s) {
  switch (s) {
    case "aviso":
      return vscode.DiagnosticSeverity.Warning;
    case "nota":
    case "ayuda":
      return vscode.DiagnosticSeverity.Information;
    default:
      return vscode.DiagnosticSeverity.Error;
  }
}

function runHxc(args, cwd) {
  return new Promise((resolve) => {
    execFile(
      hxcPath(),
      args,
      { cwd, maxBuffer: 8 * 1024 * 1024 },
      (err, stdout, stderr) => resolve({ err, stdout, stderr })
    );
  });
}

function uriToFs(uri) {
  return uri.fsPath;
}

function rangeOf(doc, d) {
  const line = Math.max(0, (d.line || 1) - 1);
  const col = Math.max(0, (d.col || 1) - 1);
  const endLine = Math.max(0, (d.endLine || d.line || 1) - 1);
  const endCol = Math.max((d.endCol || d.col || 1) - 1, 0);
  const start = new vscode.Position(line, col);
  const end = new vscode.Position(endLine, Math.max(endCol, col + 1));
  return new vscode.Range(start, end);
}

async function comprobar(uri) {
  const doc = vscode.workspace.textDocuments.find((d) => d.uri.toString() === uri.toString());
  if (!doc) return;
  const fsPath = uriToFs(uri);
  const cwd = vscode.workspace.getWorkspaceFolder(doc.uri)?.uri.fsPath;

  const { err, stdout, stderr } = await runHxc(["check", fsPath, "--json"], cwd);

  if (err && !stderr.trim()) {
    collector.set(uri, []);
    vscode.window.setStatusBarMessage("hixean: no se pudo ejecutar hxc", 4000);
    return;
  }

  let payload = null;
  const bruto = stderr.trim() || stdout.trim();
  const inicio = bruto.indexOf("{");
  if (inicio >= 0) {
    try {
      payload = JSON.parse(bruto.slice(inicio));
    } catch (e) {
      payload = null;
    }
  }
  if (!payload) {
    collector.set(uri, []);
    return;
  }

  const lista = (payload.diagnostics || []).map((d) => {
    const diag = new vscode.Diagnostic(
      rangeOf(doc, d),
      d.note ? `${d.message} — ${d.note}` : d.message,
      diagnosticSeverity(d.severity)
    );
    diag.source = `hxc ${d.code || ""}`.trim();
    diag.code = d.code || undefined;
    return diag;
  });
  collector.set(uri, lista);
}

function guardar() {
  clearTimeout(timer);
  if (!config().get("diagnostics", true)) return;
  const editor = vscode.window.activeTextEditor;
  if (!editor || !["hixean", "hxk", "hxq"].includes(editor.document.languageId)) return;
  timer = setTimeout(() => comprobar(editor.document.uri), config().get("checkDelay", 400));
}

function activate(context) {
  const disposable = vscode.workspace.onDidSaveTextDocument((doc) => {
    if (["hixean", "hxk", "hxq"].includes(doc.languageId)) {
      comprobar(doc.uri);
    }
  });
  context.subscriptions.push(disposable);

  context.subscriptions.push(
    vscode.commands.registerCommand("hixean.checkActive", async () => {
      const editor = vscode.window.activeTextEditor;
      if (!editor) return;
      await comprobar(editor.document.uri);
    })
  );

  context.subscriptions.push(
    vscode.commands.registerCommand("hixean.build", async () => {
      const editor = vscode.window.activeTextEditor;
      if (!editor) return;
      const carpeta = vscode.workspace.getWorkspaceFolder(editor.document.uri);
      const tarea = new vscode.Task(
        {
          type: "process",
          task: "hxc build",
          detail: editor.document.uri.fsPath,
        },
        vscode.TaskScope.Workspace,
        "Hixean",
        new vscode.ProcessExecution(
          hxcPath(),
          ["build", editor.document.uri.fsPath, "--profile", config().get("profile", "freestanding")],
          carpeta ? carpeta.uri.fsPath : undefined
        ),
        ["$hx"]
      );
      vscode.tasks.executeTask(tarea);
    })
  );

  context.subscriptions.push(
    vscode.commands.registerCommand("hixean.test", async () => {
      const carpeta = vscode.workspace.workspaceFolders?.[0];
      if (!carpeta) return;
      const tarea = new vscode.Task(
        { type: "process", task: "hxc test", detail: "corpus completo" },
        vscode.TaskScope.Workspace,
        "Hixean",
        new vscode.ProcessExecution(hxcPath(), ["test", "tests/*.hxt", "tests/*.hxe"], carpeta.uri.fsPath),
        ["$hx"]
      );
      vscode.tasks.executeTask(tarea);
    })
  );

  guardar();
}

function deactivate() {
  clearTimeout(timer);
  collector.dispose();
}

module.exports = { activate, deactivate };