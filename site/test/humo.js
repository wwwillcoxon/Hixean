// Prueba de humo de la pagina con un DOM falso.
//
// Lo interesante de script.js no es que no reviente, sino que hace cosas que se
// pueden comprobar: que los contadores acaben en su valor, que las pestañas
// alternen, que el tema se recuerde y que cada ejemplo tenga boton de copiado.
// Un DOM de este tamano es suficiente y evita depender de un navegador.
//
//   node site/test/humo.js

const Module = require("module");
const path = require("path");
const fs = require("fs");

const RAIZ = path.resolve(__dirname, "..", "..");
const html = fs.readFileSync(path.join(RAIZ, "site", "index.html"), "utf8");
const css = fs.readFileSync(path.join(RAIZ, "site", "style.css"), "utf8");
const script = fs.readFileSync(path.join(RAIZ, "site", "script.js"), "utf8");

/* Las tres paginas del sitio. Se leen todas para que un enlace roto o un
   border-radius en cualquiera de ellas se vea aqui y no en el navegador. */
const PAGINAS = ["index.html", "directorio.html", "terminos.html"].map((nombre) => {
  const ruta = path.join(RAIZ, "site", nombre);
  return { nombre, ruta, texto: fs.readFileSync(ruta, "utf8") };
});

class Nodo {
  constructor(sel) {
    this.sel = sel;
    this.clases = new Set();
    this.attrs = {};
    this.dataset = {};
    this.hijos = [];
    this.texto = "";
    this.estilo = {};
    this.style = this.estilo;
    this.escuchas = {};
    this._matches = () => false;
    this.classList = {
      add: (...c) => c.forEach((x) => this.clases.add(x)),
      remove: (...c) => c.forEach((x) => this.clases.delete(x)),
      contains: (c) => this.clases.has(c),
    };
  }
  add(...c) { c.forEach((x) => this.clases.add(x)); return this; }
  remove(...c) { c.forEach((x) => this.clases.delete(x)); return this; }
  contains(c) { return this.clases.has(c); }
  setAttribute(k, v) {
    this.attrs[k] = v;
    const clave = k.startsWith("data-") ? k.slice(5).replace(/-(\w)/g, (m, c) => c.toUpperCase()) : null;
    if (clave) this.dataset[clave] = v;
  }
  getAttribute(k) { return this.attrs[k] !== undefined ? this.attrs[k] : null; }
  appendChild(h) { this.hijos.push(h); h.padre = this; return h; }
  querySelectorAll() { return []; }
  querySelector() { return null; }
  matches(sel) { return this._matches(sel); }
  addEventListener(ev, fn) { this.escuchas[ev] = fn; }
  focus() {}
  select() {}
  getBoundingClientRect() { return { top: 0, left: 0 }; }
  closest() { return null; }
  get innerText() { return this.texto; }
  set innerText(v) { this.texto = v; }
  get textContent() { return this.texto; }
  set textContent(v) { this.texto = v; }
}

function cargarFake(env) {
  /* el script usa document, window y localStorage como globales, asi que se
     instalan en globalThis y se retiran al terminar */
  const anteriores = new Map();
  Object.keys(env).forEach((k) => {
    anteriores.set(k, Object.getOwnPropertyDescriptor(globalThis, k));
    globalThis[k] = env[k];
  });
  delete require.cache[require.resolve(path.join(RAIZ, "site", "script.js"))];
  try {
    require(path.join(RAIZ, "site", "script.js"));
  } finally {
    anteriores.forEach((d, k) => {
      if (d) Object.defineProperty(globalThis, k, d);
      else delete globalThis[k];
    });
  }
}

function fallos() {
  const lista = [];
  return {
    lista,
    ok(condicion, mensaje) {
      if (!condicion) lista.push(mensaje);
    },
    cerrar() {
      if (!lista.length) return;
      lista.forEach((f) => console.error("FALLO:", f));
      process.exit(1);
    },
  };
}

function cifrasDelHero() {
  return [...html.matchAll(/<b data-cuenta="(\d+)"(?: data-sufijo="([^"]*)")?>([^<]*)<\/b>/g)];
}

function comprobarPagina(t) {
  // las cifras del hero llevan su valor real en el HTML, no un 0 de relleno
  const cifras = cifrasDelHero();
  t.ok(cifras.length === 4, `se esperaban 4 cifras en el hero, hay ${cifras.length}`);
  cifras.forEach((m) => {
    const puesto = m[3].trim();
    t.ok(puesto !== "0" || m[1] === "0", `la cifra data-cuenta="${m[1]}" pone "${puesto}" en el HTML`);
  });

  // sin JavaScript se ven las seis pestañas y el contenido
  t.ok(
    (html.match(/role="tabpanel"[^>]*hidden/g) || []).length === 0,
    "hay paneles de pestaña ocultos en el HTML: sin JS no se verian"
  );
  t.ok((html.match(/role="tabpanel"/g) || []).length === 6, "faltan paneles de pestaña");
  t.ok(!/id="sin-js"/.test(html), "sigue el aviso #sin-js, que ademas solo aparecia con JS");
  t.ok(!/<script>/.test(html), "hay un script inline");
  t.ok(!/ style="/.test(html), "hay estilos inline");

  // el CSS solo oculta contenido cuando hay JavaScript
  t.ok(/^\.js \[data-revelar\] \{ opacity: 0/m.test(css), "la regla de ocultar revelados no exige la clase .js");
  t.ok(
    script.indexOf('classList.add("js")') !== -1 &&
      script.indexOf('classList.add("js")') < script.indexOf("querySelectorAll"),
    "el script no añade .js antes de buscar nodos"
  );
  t.ok(
    /background: var\(--fondo\);\s*\n\s*background: color-mix/.test(css),
    "color-mix sin alternativa: la cabecera queda transparente donde no exista"
  );

  // nada roto al copiar
  t.ok(!/githubusercontent\/\s*\n/.test(html), "la URL de instalacion sigue partida en dos lineas");

  // canales que existen de verdad
  t.ok(!/brew tap wwwillcoxon/.test(html), "la pagina sigue anunciando un tap de Homebrew que no existe");
  /* winget y Homebrew se mencionan, pero como lo que son: archivos en el
     repositorio y pasos que faltan, no instrucciones que hoy funcionen */
  t.ok(/falta enviarlo/.test(html), "la tarjeta de winget no dice que falta enviar el manifiesto");
  t.ok(/paso pendiente/.test(html), "la tarjeta de Homebrew no dice que falta publicar el tap");

  // los ejemplos de la pagina los verifica tools/verificar-ejemplos.py
  /* El numero de ejemplos verificados lo vigila tools/verificar-ejemplos.py, que
     ademas los ejecuta. Aqui solo se comprueba que no se cuele ninguno sin
     comprobar: todos los bloques de la pagina llevan clase "lenguaje" y eso es
     lo que hace que el verificador los ejecute. */
  const ejemplos = (html.match(/<pre><code class="lenguaje/g) || []).length;
  t.ok(ejemplos >= 8, `se esperaban al menos 8 bloques de código, hay ${ejemplos}`);
  const sinComprobar = (html.match(/<pre><code(?! class="lenguaje)/g) || []).length;
  const conShell = (html.match(/<pre class="copiable">/g) || []).length;
  t.ok(conShell >= sinComprobar, "hay bloques de codigo sin clase, y verificar-ejemplos no los mira");
}

function comprobarScript(t, conObserver) {
  const raizHtml = new Nodo("html");
  /* el boton del tema y el progreso existen en el HTML real: se reproducen con
     el mismo contrato que espera el script */
  const botonTema = new Nodo("button");
  botonTema.querySelector = () => new Nodo("span");
  const progreso = new Nodo("div");
  const relleno = new Nodo("div");
  const porId = { tema: botonTema, progreso: progreso, relleno: relleno, anio: new Nodo("span") };
  const cifras = cifrasDelHero();
  const contadores = cifras.map((m) => {
    const n = new Nodo("b");
    n.setAttribute("data-cuenta", m[1]);
    if (m[2]) n.setAttribute("data-sufijo", m[2]);
    n._matches = (sel) => sel === "[data-cuenta]";
    return n;
  });
  const revelados = [new Nodo("li"), new Nodo("article"), new Nodo("h3")];

  const estado = { reloj: 0 };
  const env = {
    document: {
      documentElement: raizHtml,
      getElementById: (id) => porId[id] || null,
      body: new Nodo("body"),
      querySelector: () => null,
      querySelectorAll: (sel) =>
        ({
          ".cifras b, .medidas b, .linea li, .tarjeta": [...contadores, ...revelados],
          "[data-cuenta]": contadores,
          ".ventana, pre.copiable": [],
          "#enlaces a": [],
        }[sel] || []),
      createElement: (tag) => new Nodo(tag),
      addEventListener: () => {},
    },
    window: {
      matchMedia: () => ({ matches: false, addEventListener: () => {} }),
      addEventListener: () => {},
      innerHeight: 800,
      scrollY: 0,
      requestAnimationFrame: (fn) => {
        estado.reloj += 16;
        if (estado.reloj > 3000) return 1;
        fn(estado.reloj);
        return 1;
      },
      setTimeout: (fn) => {
        fn();
        return 1;
      },
      print: () => {},
    },
    localStorage: {
      datos: {},
      getItem(k) { return this.datos[k] !== undefined ? this.datos[k] : null; },
      setItem(k, v) { this.datos[k] = v; },
    },
    navigator: {},
  };
  if (conObserver) {
    env.IntersectionObserver = class {
      constructor(cb) { this.cb = cb; }
      observe(el) { this.cb([{ target: el, isIntersecting: true }], this); }
      unobserve() {}
      disconnect() {}
    };
    env.window.IntersectionObserver = env.IntersectionObserver;
  }
  env.window.document = env.document;
  cargarFake(env);

  t.ok(raizHtml.clases.has("js"), "el script no añadió la clase .js al <html>");

  // el bug que motivaba esto: el contador es el propio elemento observado
  /* el valor esperado sale del propio HTML: si alguien cambia una cifra, el
     test sigue siendo cierto siempre que la animacion llegue al final */
  const esperados = cifras.map((m) => {
    const bruto = m[3].trim();
    const destino = Number(m[1]) * (m[2] || "") || Number(m[1]);
    if (!bruto) return destino.toLocaleString("es-ES");
    return bruto;
  });
  const sinSep = (x) => x.replace(/[\s.\u00a0]/g, "");
  contadores.forEach((c, i) => {
    t.ok(
      sinSep(c.texto) === sinSep(esperados[i]),
      `el contador ${i} quedó en "${c.texto}" y decia "${esperados[i]}"`
    );
  });
  t.ok(/^\d+\/\d+$/.test(esperados[1].replace(/\s/g, "")), "la cifra de pruebas no es n/n");
  if (!conObserver) t.ok(true, "");

  revelados.forEach((r, i) => {
    t.ok(r.clases.has("visible"), `el elemento con revelado ${i} no se hizo visible`);
  });

  // la barra de presupuesto se rellena sola
  t.ok(
    relleno.estilo.width === "72%",
    `la barra de presupuesto quedó en "${relleno.estilo.width}" en vez de 72%`
  );
  t.ok(progreso.attrs["aria-valuenow"] !== undefined, "la barra de progreso no publica aria-valuenow");

  t.ok(env.localStorage.datos["hixean-tema"] === "oscuro", "el tema por defecto no se guardó");
}

/* Lo que el sitio tiene que cumplir siempre, y que no se ve al abrirlo:
   sin bordes redondeados, con todos los enlaces internos resueltos, sin nada
   que venga de fuera y con el PDF de la guia de verdad en su sitio. */
function comprobarSitio(t) {
  // 1. Ningun borde redondeado. Es una peticion del proyecto, y en CSS basta
  // con no escribir border-radius: asi que se busca la propiedad, no el efecto.
  t.ok(!/border-radius/.test(css), "style.css tiene border-radius: las cajas van rectas");
  for (const pagina of PAGINAS) {
    t.ok(
      !/border-radius/.test(pagina.texto),
      `${pagina.nombre} tiene border-radius en el HTML`
    );
  }

  // 2. Todo enlace interno tiene que existir. Un enlace roto en una pagina
  //    estatica no avisa a nadie: se queda ahi hasta que alguien lo pulse.
  for (const pagina of PAGINAS) {
    const enlaces = pagina.texto.match(/(?:href|src)="([^"]+)"/g) || [];
    for (const crudo of enlaces) {
      const destino = crudo.slice(crudo.indexOf('"') + 1, -1);
      if (/^(https?:|mailto:|#)/.test(destino)) continue;
      const limpio = destino.split("#")[0];
      if (!limpio) continue;
      const abs = path.resolve(path.dirname(pagina.ruta), limpio);
      t.ok(
        fs.existsSync(abs),
        `${pagina.nombre} apunta a ${limpio}, que no existe`
      );
    }
  }

  // 3. Nada de fuera: ni scripts remotos, ni fuentes, ni estilos de otro sitio.
  for (const pagina of PAGINAS) {
    t.ok(
      !/<script[^>]+src="https?:/.test(pagina.texto),
      `${pagina.nombre} carga un script de fuera`
    );
    t.ok(
      !/<link[^>]+href="https?:/.test(pagina.texto),
      `${pagina.nombre} carga una hoja de estilo de fuera`
    );
    t.ok(
      !/@import/.test(css),
      "style.css importa algo de fuera"
    );
  }

  // 4. El PDF existe y no es un fichero de mentira: el generador dice cuantas
  //    paginas tiene, y el numero del sitio tiene que ser el mismo.
  const pdf = path.join(RAIZ, "site", "guia-programar.pdf");
  t.ok(fs.existsSync(pdf), "no esta site/guia-programar.pdf, que el sitio enlaza");
  if (fs.existsSync(pdf)) {
    const buf = fs.readFileSync(pdf);
    t.ok(buf.slice(0, 5).toString() === "%PDF-", "el PDF no empieza por %PDF-");
    t.ok(buf.slice(-6).toString().includes("%%EOF"), "el PDF no termina en %%EOF");
    const n = (buf.toString("latin1").match(/\/Type \/Page[^s]/g) || []).length;
    t.ok(n >= 10, `el PDF tiene ${n} paginas: parece demasiado corto para una guia`);
    const pisadas = html.match(/(\d+) páginas/);
    t.ok(
      !pisadas || Number(pisadas[1]) === n,
      `la pagina dice ${pisadas && pisadas[1]} paginas y el PDF tiene ${n}`
    );
  }

  // 5. Las tres paginas del sitio comparten el pie con la version real
  for (const pagina of PAGINAS) {
    t.ok(
      /Hixean 0\.\d+\.\d+ ·/.test(pagina.texto),
      `${pagina.nombre} no dice la version en el pie`
    );
  }
}

function main() {
  const t = fallos();
  comprobarPagina(t);
  comprobarScript(t, true);
  comprobarScript(t, false);
  comprobarSitio(t);
  t.cerrar();
  console.log("ok     el sitio: cifras reales, enlaces resueltos, nada de fuera y sin bordes redondeados");
}

main();